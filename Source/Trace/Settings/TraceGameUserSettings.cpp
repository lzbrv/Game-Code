// Trace — video settings implementation. See TraceGameUserSettings.h for the design notes.

#include "Settings/TraceGameUserSettings.h"

#include "Camera/CameraComponent.h"
#include "Containers/Ticker.h"
#include "DynamicRHI.h"                      // RHIGetAvailableResolutions / FScreenResolutionArray
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericApplication.h"   // FDisplayMetrics / FMonitorInfo
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Scalability.h"
#include "Trace.h"                           // LogTraceGame

// =================================================================================================
// File-scope state
//
// Deliberately not members. The change delegate has to outlive any single settings object (the
// engine can recreate it), and the two ticker handles are process-lifetime registrations that a
// UPROPERTY-bearing class has no business serialising.
// =================================================================================================

namespace
{
	FTraceVideoSettingsChanged GVideoChanged;

	/** 1 Hz re-application of the FOV, so a respawned pawn does not silently revert to 95. */
	FTSTicker::FDelegateHandle GFieldOfViewTickerHandle;

	/**
	 * Escape hatch for the FOV ticker.
	 *
	 * It exists because the ticker writes into a component ATraceCharacter owns. Nothing there
	 * animates FOV today (the constructor sets it once), but the day something does — an ADS zoom,
	 * a dash punch — that code and this ticker would fight at 1 Hz, and the symptom would be a
	 * once-a-second flick that is very hard to attribute. Turning this off is then the one-line
	 * diagnosis, and the permanent fix is the cross-file change described in the report: have
	 * ATraceCharacter read UTraceGameUserSettings::Get()->GetFieldOfView() when it builds its camera.
	 *
	 * NOTE the name. It is a CVar, so no console command may ever share it — that combination is
	 * fatal at module load in this engine version, and every command below is under
	 * "Trace.Video.<Verb>" precisely so the two namespaces cannot collide.
	 */
	int32 GFOVAutoApply = 1;
	FAutoConsoleVariableRef CVarTraceVideoFOVAutoApply(
		TEXT("Trace.Video.FOVAutoApply"),
		GFOVAutoApply,
		TEXT("1 (default): re-push the saved field of view onto the local camera once a second, so ")
		TEXT("it survives respawns. 0: apply only when the setting is changed."),
		ECVF_Default);

	/** Aspect-ratio annotation for a resolution label: "16:9", "16:10", "21:9", or a reduced W:H. */
	FString DescribeAspect(int32 InWidth, int32 InHeight)
	{
		if (InWidth <= 0 || InHeight <= 0)
		{
			return FString();
		}

		// Match against the ratios players recognise before falling back to arithmetic. 3440x1440 is
		// 43:18 exactly, which is true and useless; "21:9" is what the panel is sold as.
		struct FNamedAspect { float Ratio; const TCHAR* Label; };
		static const FNamedAspect Named[] =
		{
			{ 16.f / 9.f,   TEXT("16:9")  },
			{ 16.f / 10.f,  TEXT("16:10") },
			{ 4.f / 3.f,    TEXT("4:3")   },
			{ 21.f / 9.f,   TEXT("21:9")  },
			{ 32.f / 9.f,   TEXT("32:9")  },
			{ 3.f / 2.f,    TEXT("3:2")   },
			{ 5.f / 4.f,    TEXT("5:4")   },
		};

		const float Ratio = static_cast<float>(InWidth) / static_cast<float>(InHeight);
		for (const FNamedAspect& Candidate : Named)
		{
			if (FMath::Abs(Ratio - Candidate.Ratio) < 0.02f)
			{
				return Candidate.Label;
			}
		}

		// Reduced form only when it is short enough to be read as a ratio. MEASURED: this Mac's
		// desktop is 1728x1117 (the notch steals 32 points off 1728x1149), whose reduced ratio is
		// 1728:1117 — arithmetically correct and complete noise on a menu row. Better to print no
		// annotation at all than one the player has to parse.
		const int32 Divisor = FMath::Max(1, FMath::GreatestCommonDivisor(InWidth, InHeight));
		const int32 ReducedW = InWidth / Divisor;
		const int32 ReducedH = InHeight / Divisor;
		if (ReducedW > 32 || ReducedH > 32)
		{
			return FString();
		}
		return FString::Printf(TEXT("%d:%d"), ReducedW, ReducedH);
	}
}

// =================================================================================================
// Construction and access
// =================================================================================================

UTraceGameUserSettings::UTraceGameUserSettings()
{
	// Nothing but the declared defaults. The engine calls SetToDefaults() and then LoadSettings()
	// on the real instance immediately after construction (UEngine::CreateGameUserSettings), so any
	// work done here would be thrown away one line later.
}

UTraceGameUserSettings* UTraceGameUserSettings::Get()
{
	if (GEngine == nullptr)
	{
		return nullptr;
	}

	UGameUserSettings* const Base = GEngine->GetGameUserSettings();

	// A Cast rather than a CastChecked. If DefaultEngine.ini's GameUserSettingsClassName is missing
	// or misspelled the engine silently falls back to plain UGameUserSettings, and the whole video
	// page would then be one null dereference away from taking the process down. Fail loud, once.
	UTraceGameUserSettings* const Typed = Cast<UTraceGameUserSettings>(Base);
	if (Typed == nullptr)
	{
		static bool bComplained = false;
		if (!bComplained)
		{
			bComplained = true;
			UE_LOG(LogTraceGame, Error,
				TEXT("[Video] GEngine->GetGameUserSettings() is a %s, not a UTraceGameUserSettings. ")
				TEXT("Config/DefaultEngine.ini must contain, under [/Script/Engine.Engine]: ")
				TEXT("GameUserSettingsClassName=/Script/Trace.TraceGameUserSettings"),
				Base ? *Base->GetClass()->GetName() : TEXT("null"));
		}
	}

	return Typed;
}

FTraceVideoSettingsChanged& UTraceGameUserSettings::OnVideoChanged()
{
	return GVideoChanged;
}

// =================================================================================================
// Apply
// =================================================================================================

void UTraceGameUserSettings::ApplyVideoSettings(bool bIncludingResolution)
{
	if (bIncludingResolution)
	{
		// ApplySettings does resolution + everything else + SaveSettings.
		ApplySettings(/*bCheckForCommandLineOverrides=*/false);
	}
	else
	{
		ApplyNonResolutionSettings();
		SaveSettings();
	}

	ApplyFieldOfViewToWorlds();
	GVideoChanged.Broadcast();
}

// =================================================================================================
// Window mode
// =================================================================================================

EWindowMode::Type UTraceGameUserSettings::GetWindowMode() const
{
	return GetFullscreenMode();
}

void UTraceGameUserSettings::SetWindowMode(EWindowMode::Type NewMode)
{
	SetFullscreenMode(NewMode);

	// The resolution list is filtered against the monitor's desktop rect, and in WindowedFullscreen
	// the effective resolution IS that rect, so the list the menu is showing may no longer be the
	// right one. Cheap to drop; rebuilt on the next read.
	bResolutionOptionsBuilt = false;
}

const TArray<EWindowMode::Type>& UTraceGameUserSettings::GetWindowModeOptions()
{
	static const TArray<EWindowMode::Type> Options =
	{
		EWindowMode::Fullscreen,
		EWindowMode::WindowedFullscreen,
		EWindowMode::Windowed
	};
	return Options;
}

FString UTraceGameUserSettings::DescribeWindowMode(EWindowMode::Type Mode)
{
	switch (Mode)
	{
	case EWindowMode::Fullscreen:         return TEXT("FULLSCREEN");
	// "BORDERLESS", not "WINDOWED FULLSCREEN": it is what every other shooter calls it, and it does
	// not read as a near-duplicate of the row above it in a list the player is scanning quickly.
	case EWindowMode::WindowedFullscreen: return TEXT("BORDERLESS");
	case EWindowMode::Windowed:           return TEXT("WINDOWED");
	default:                              return TEXT("UNKNOWN");
	}
}

// =================================================================================================
// Resolution
// =================================================================================================

void UTraceGameUserSettings::BuildResolutionOptions() const
{
	ResolutionOptions.Reset();
	bResolutionOptionsBuilt = true;

	// ---- What counts as "the current monitor" ---------------------------------------------------
	//
	// GetDesktopResolution() is the primary display. When the game window has been moved to a second
	// monitor, GetClosestMonitorIndex() plus the display metrics give the right one, so try that
	// first and keep the primary as the fallback.
	FIntPoint MonitorSize = GetDesktopResolution();
	if (FApp::CanEverRender())
	{
		FDisplayMetrics Metrics;
		FDisplayMetrics::RebuildDisplayMetrics(Metrics);

		const int32 MonitorIndex = GetClosestMonitorIndex();
		if (Metrics.MonitorInfo.IsValidIndex(MonitorIndex))
		{
			const FMonitorInfo& Info = Metrics.MonitorInfo[MonitorIndex];
			const int32 RectW = Info.DisplayRect.Right - Info.DisplayRect.Left;
			const int32 RectH = Info.DisplayRect.Bottom - Info.DisplayRect.Top;
			if (RectW > 0 && RectH > 0)
			{
				MonitorSize = FIntPoint(RectW, RectH);
			}
		}
	}

	if (MonitorSize.X <= 0 || MonitorSize.Y <= 0)
	{
		MonitorSize = FIntPoint(1920, 1080);
	}

	// ---- Ask the RHI ----------------------------------------------------------------------------
	//
	// bIgnoreRefreshRate=true collapses a panel's 60/120/144 Hz variants of the same size into one
	// entry, which is what a resolution row wants. This is a list of MODES, not of refresh rates.
	TMap<FIntPoint, int32> BestRefreshBySize;
	if (GDynamicRHI != nullptr)
	{
		FScreenResolutionArray RHIModes;
		if (RHIGetAvailableResolutions(RHIModes, /*bIgnoreRefreshRate=*/true))
		{
			for (const FScreenResolutionRHI& Mode : RHIModes)
			{
				const FIntPoint Size(static_cast<int32>(Mode.Width), static_cast<int32>(Mode.Height));
				if (Size.X < 640 || Size.Y < 480)
				{
					continue;   // Below this nothing in the Canvas HUD is legible.
				}
				if (Size.X > MonitorSize.X || Size.Y > MonitorSize.Y)
				{
					continue;   // Not presentable on the monitor the game is actually on.
				}

				int32& BestRefresh = BestRefreshBySize.FindOrAdd(Size, 0);
				BestRefresh = FMath::Max(BestRefresh, static_cast<int32>(Mode.RefreshRate));
			}
		}
	}

	// ---- Fallback ladder ------------------------------------------------------------------------
	//
	// FMetalDynamicRHI implements enumeration on Mac and Windows' RHIs do too, but the interface is
	// allowed to return false, a -nullrhi run has no GDynamicRHI at all, and an empty resolution row
	// is worse than an approximate one. Everything here is clamped to the monitor, so the ladder can
	// never offer a mode the display cannot show.
	if (BestRefreshBySize.Num() == 0)
	{
		static const FIntPoint Ladder[] =
		{
			FIntPoint(1280, 720),  FIntPoint(1600, 900),  FIntPoint(1920, 1080),
			FIntPoint(2048, 1152), FIntPoint(2560, 1440), FIntPoint(3200, 1800),
			FIntPoint(3840, 2160)
		};
		for (const FIntPoint& Size : Ladder)
		{
			if (Size.X <= MonitorSize.X && Size.Y <= MonitorSize.Y)
			{
				BestRefreshBySize.FindOrAdd(Size, 0);
			}
		}
	}

	// The monitor's own resolution is always offered, whatever the enumeration said.
	BestRefreshBySize.FindOrAdd(MonitorSize, 0);

	// The resolution currently in force is always offered too, even if it is an odd size a script
	// passed with -ResX/-ResY. Otherwise GetResolutionOptionIndex returns INDEX_NONE and the menu
	// has nothing to highlight.
	const FIntPoint CurrentSize = GetScreenResolution();
	if (CurrentSize.X > 0 && CurrentSize.Y > 0)
	{
		BestRefreshBySize.FindOrAdd(CurrentSize, 0);
	}

	ResolutionOptions.Reserve(BestRefreshBySize.Num());
	for (const TPair<FIntPoint, int32>& Entry : BestRefreshBySize)
	{
		FTraceResolutionOption Option;
		Option.Resolution = Entry.Key;
		Option.RefreshRate = Entry.Value;
		Option.bIsNative = (Entry.Key == MonitorSize);

		const FString Aspect = DescribeAspect(Entry.Key.X, Entry.Key.Y);
		Option.Label = Aspect.IsEmpty()
			? FString::Printf(TEXT("%d x %d"), Entry.Key.X, Entry.Key.Y)
			: FString::Printf(TEXT("%d x %d  (%s)"), Entry.Key.X, Entry.Key.Y, *Aspect);

		ResolutionOptions.Add(MoveTemp(Option));
	}

	// Ascending by pixel count: left on the row is always cheaper, right is always sharper, which is
	// the only ordering a performance-minded player can navigate without reading every entry.
	ResolutionOptions.Sort([](const FTraceResolutionOption& A, const FTraceResolutionOption& B)
	{
		const int64 PixelsA = static_cast<int64>(A.Resolution.X) * static_cast<int64>(A.Resolution.Y);
		const int64 PixelsB = static_cast<int64>(B.Resolution.X) * static_cast<int64>(B.Resolution.Y);
		return (PixelsA != PixelsB) ? (PixelsA < PixelsB) : (A.Resolution.X < B.Resolution.X);
	});
}

const TArray<FTraceResolutionOption>& UTraceGameUserSettings::GetResolutionOptions() const
{
	if (!bResolutionOptionsBuilt)
	{
		BuildResolutionOptions();
	}
	return ResolutionOptions;
}

void UTraceGameUserSettings::RefreshResolutionOptions()
{
	BuildResolutionOptions();
}

int32 UTraceGameUserSettings::GetResolutionOptionIndex() const
{
	const FIntPoint CurrentSize = GetScreenResolution();
	const TArray<FTraceResolutionOption>& Options = GetResolutionOptions();
	for (int32 Index = 0; Index < Options.Num(); ++Index)
	{
		if (Options[Index].Resolution == CurrentSize)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void UTraceGameUserSettings::SetResolutionByOptionIndex(int32 OptionIndex)
{
	const TArray<FTraceResolutionOption>& Options = GetResolutionOptions();
	if (!Options.IsValidIndex(OptionIndex))
	{
		return;
	}
	SetScreenResolution(Options[OptionIndex].Resolution);
}

bool UTraceGameUserSettings::IsResolutionSelectable() const
{
	return GetFullscreenMode() != EWindowMode::WindowedFullscreen;
}

// =================================================================================================
// Resolution scale
// =================================================================================================

int32 UTraceGameUserSettings::GetResolutionScalePercent() const
{
	float Normalized = 0.f;
	float CurrentValue = 0.f;
	float MinValue = 0.f;
	float MaxValue = 0.f;
	GetResolutionScaleInformationEx(Normalized, CurrentValue, MinValue, MaxValue);

	// 0 is the engine's "defer to the project's default screen percentage" sentinel, not 0%. The
	// project's default is 100 (r.ScreenPercentage.Default), so that is what the row should read.
	if (CurrentValue <= 0.f)
	{
		return MaxResolutionScalePercent;
	}

	return FMath::Clamp(FMath::RoundToInt(CurrentValue),
		MinResolutionScalePercent, MaxResolutionScalePercent);
}

void UTraceGameUserSettings::SetResolutionScalePercent(int32 NewPercent)
{
	const int32 Clamped = FMath::Clamp(NewPercent, MinResolutionScalePercent, MaxResolutionScalePercent);
	SetResolutionScaleValueEx(static_cast<float>(Clamped));
}

// =================================================================================================
// Quality
// =================================================================================================

namespace
{
	/** The nine exposed groups, read out of a quality-levels struct. */
	int32 ReadGroup(const Scalability::FQualityLevels& Levels, ETraceQualityGroup Group)
	{
		switch (Group)
		{
		case ETraceQualityGroup::ViewDistance:       return Levels.ViewDistanceQuality;
		case ETraceQualityGroup::AntiAliasing:       return Levels.AntiAliasingQuality;
		case ETraceQualityGroup::PostProcess:        return Levels.PostProcessQuality;
		case ETraceQualityGroup::Shadows:            return Levels.ShadowQuality;
		case ETraceQualityGroup::GlobalIllumination: return Levels.GlobalIlluminationQuality;
		case ETraceQualityGroup::Reflections:        return Levels.ReflectionQuality;
		case ETraceQualityGroup::Textures:           return Levels.TextureQuality;
		case ETraceQualityGroup::Effects:            return Levels.EffectsQuality;
		case ETraceQualityGroup::Shading:            return Levels.ShadingQuality;
		default:                                     return 0;
		}
	}

	/**
	 * Writes one group through the struct's own clamping setters.
	 *
	 * SetShadowQuality and friends clamp against sg.<Group>.NumLevels, which a device profile is
	 * allowed to lower. Assigning the members raw would let a value past that clamp and produce a
	 * scalability lookup with no matching ini section — which silently applies nothing.
	 */
	void WriteGroup(Scalability::FQualityLevels& Levels, ETraceQualityGroup Group, int32 Level)
	{
		switch (Group)
		{
		case ETraceQualityGroup::ViewDistance:       Levels.SetViewDistanceQuality(Level); break;
		case ETraceQualityGroup::AntiAliasing:       Levels.SetAntiAliasingQuality(Level); break;
		case ETraceQualityGroup::PostProcess:        Levels.SetPostProcessQuality(Level); break;
		case ETraceQualityGroup::Shadows:            Levels.SetShadowQuality(Level); break;
		case ETraceQualityGroup::GlobalIllumination: Levels.SetGlobalIlluminationQuality(Level); break;
		case ETraceQualityGroup::Reflections:        Levels.SetReflectionQuality(Level); break;
		case ETraceQualityGroup::Textures:           Levels.SetTextureQuality(Level); break;
		case ETraceQualityGroup::Effects:            Levels.SetEffectsQuality(Level); break;
		case ETraceQualityGroup::Shading:            Levels.SetShadingQuality(Level); break;
		default: break;
		}
	}
}

ETraceVideoQuality UTraceGameUserSettings::GetOverallQuality() const
{
	const int32 First = ReadGroup(ScalabilityQuality, ETraceQualityGroup::ViewDistance);
	if (First < MinQualityLevel || First > MaxQualityLevel)
	{
		return ETraceVideoQuality::Custom;
	}

	for (int32 GroupIndex = 1; GroupIndex < static_cast<int32>(ETraceQualityGroup::Count); ++GroupIndex)
	{
		if (ReadGroup(ScalabilityQuality, static_cast<ETraceQualityGroup>(GroupIndex)) != First)
		{
			return ETraceVideoQuality::Custom;
		}
	}

	return static_cast<ETraceVideoQuality>(First);
}

void UTraceGameUserSettings::SetOverallQuality(ETraceVideoQuality NewQuality)
{
	if (NewQuality == ETraceVideoQuality::Custom)
	{
		return;
	}

	const int32 Level = FMath::Clamp(static_cast<int32>(NewQuality), MinQualityLevel, MaxQualityLevel);

	// SetFromSingleQualityLevel also stamps ResolutionQuality with the ladder value for that level
	// (50 / 71 / 87 / 100 from BaseScalability.ini). Render scale is its own row on this page, so
	// put the player's value back. See the header for why.
	const float PreservedResolutionQuality = ScalabilityQuality.ResolutionQuality;
	ScalabilityQuality.SetFromSingleQualityLevel(Level);
	ScalabilityQuality.ResolutionQuality = PreservedResolutionQuality;
}

int32 UTraceGameUserSettings::GetGroupQuality(ETraceQualityGroup Group) const
{
	return FMath::Clamp(ReadGroup(ScalabilityQuality, Group), MinQualityLevel, MaxQualityLevel);
}

void UTraceGameUserSettings::SetGroupQuality(ETraceQualityGroup Group, int32 NewLevel)
{
	WriteGroup(ScalabilityQuality, Group, FMath::Clamp(NewLevel, MinQualityLevel, MaxQualityLevel));
}

const TCHAR* UTraceGameUserSettings::GetGroupLabel(ETraceQualityGroup Group)
{
	switch (Group)
	{
	case ETraceQualityGroup::ViewDistance:       return TEXT("VIEW DISTANCE");
	case ETraceQualityGroup::AntiAliasing:       return TEXT("ANTI-ALIASING");
	case ETraceQualityGroup::PostProcess:        return TEXT("POST PROCESSING");
	case ETraceQualityGroup::Shadows:            return TEXT("SHADOWS");
	case ETraceQualityGroup::GlobalIllumination: return TEXT("GLOBAL ILLUMINATION");
	case ETraceQualityGroup::Reflections:        return TEXT("REFLECTIONS");
	case ETraceQualityGroup::Textures:           return TEXT("TEXTURES");
	case ETraceQualityGroup::Effects:            return TEXT("EFFECTS");
	case ETraceQualityGroup::Shading:            return TEXT("SHADING");
	default:                                     return TEXT("");
	}
}

FString UTraceGameUserSettings::DescribeQualityLevel(int32 Level)
{
	switch (FMath::Clamp(Level, MinQualityLevel, MaxQualityLevel))
	{
	case 0:  return TEXT("LOW");
	case 1:  return TEXT("MEDIUM");
	case 2:  return TEXT("HIGH");
	default: return TEXT("EPIC");
	}
}

FString UTraceGameUserSettings::DescribeOverallQuality(ETraceVideoQuality Quality)
{
	return (Quality == ETraceVideoQuality::Custom)
		? FString(TEXT("CUSTOM"))
		: DescribeQualityLevel(static_cast<int32>(Quality));
}

// =================================================================================================
// Frame rate limit
// =================================================================================================

const TArray<float>& UTraceGameUserSettings::GetFrameRateLimitOptions()
{
	static const TArray<float> Options = { 0.f, 30.f, 60.f, 120.f, 144.f, 240.f };
	return Options;
}

int32 UTraceGameUserSettings::GetFrameRateLimitIndex() const
{
	const TArray<float>& Options = GetFrameRateLimitOptions();
	const float Current = GetFrameRateLimit();

	int32 BestIndex = 0;
	float BestDistance = TNumericLimits<float>::Max();
	for (int32 Index = 0; Index < Options.Num(); ++Index)
	{
		const float Distance = FMath::Abs(Options[Index] - Current);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			BestIndex = Index;
		}
	}
	return BestIndex;
}

void UTraceGameUserSettings::SetFrameRateLimitByIndex(int32 OptionIndex)
{
	const TArray<float>& Options = GetFrameRateLimitOptions();
	if (Options.IsValidIndex(OptionIndex))
	{
		SetFrameRateLimit(Options[OptionIndex]);
	}
}

FString UTraceGameUserSettings::DescribeFrameRateLimit(float Limit)
{
	return (Limit <= 0.f)
		? FString(TEXT("UNLIMITED"))
		: FString::Printf(TEXT("%d FPS"), FMath::RoundToInt(Limit));
}

void UTraceGameUserSettings::ApplyFrameRateLimitNow()
{
	// Both halves are protected on UGameUserSettings and reachable only from a derived class, and
	// GetEffectiveFrameRateLimit is virtual for the benefit of platforms that clamp it. Going
	// through it rather than through FrameRateLimit directly keeps that override honoured.
	if (!IsRunningDedicatedServer())
	{
		SetFrameRateLimitCVar(GetEffectiveFrameRateLimit());
	}
}

// =================================================================================================
// Field of view
// =================================================================================================

float UTraceGameUserSettings::GetFieldOfView() const
{
	return FMath::Clamp(FieldOfView, MinFieldOfView, MaxFieldOfView);
}

void UTraceGameUserSettings::SetFieldOfView(float NewFOV)
{
	FieldOfView = FMath::Clamp(NewFOV, MinFieldOfView, MaxFieldOfView);
	ApplyFieldOfViewToWorlds();
}

void UTraceGameUserSettings::ApplyFieldOfViewToWorlds()
{
	if (GEngine == nullptr)
	{
		return;
	}

	const float Desired = GetFieldOfView();

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		// Game and PIE worlds only. An inactive or editor world has no local player to look through.
		if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
		{
			continue;
		}

		UWorld* const World = Context.World();
		if (World == nullptr)
		{
			continue;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* const PC = It->Get();

			// IsLocalController, because on a listen server this iterator also walks the remote
			// players' controllers and their pawns' cameras are not rendering anything here.
			if (PC == nullptr || !PC->IsLocalController())
			{
				continue;
			}

			APawn* const ControlledPawn = PC->GetPawn();
			if (ControlledPawn == nullptr)
			{
				continue;
			}

			UCameraComponent* const Camera = ControlledPawn->FindComponentByClass<UCameraComponent>();
			if (Camera != nullptr && !FMath::IsNearlyEqual(Camera->FieldOfView, Desired, 0.01f))
			{
				Camera->SetFieldOfView(Desired);
			}
		}
	}
}

// =================================================================================================
// Auto-detect
// =================================================================================================

void UTraceGameUserSettings::RunAutoDetect()
{
	// WorkScale 10 is the engine's own default and the value the editor's "auto" button uses. It is
	// a few tens of milliseconds of synthetic work; anything smaller starts returning noise on a
	// fast machine, which is the direction that would misclassify a good GPU as weak.
	RunHardwareBenchmark(/*WorkScale=*/10, /*CPUMultiplier=*/1.0f, /*GPUMultiplier=*/1.0f);

	const float GPUIndex = GetLastGPUBenchmarkResult();
	const float CPUIndex = GetLastCPUBenchmarkResult();

	// WHAT COUNTS AS A USABLE READING, AND WHY THERE IS AN UPPER BOUND.
	//
	// Engine/Config/BaseScalability.ini calibrates every group against thresholds of 18 / 42 / 115,
	// so a real GPU lands somewhere around 10-300. MEASURED ON THIS MACHINE (Apple M3 Max, Metal,
	// -RenderOffScreen), FSynthBenchmark reports "GPU final test: 0.00s" for every sub-test and
	// derives GPUIndex = 346319.6 — three thousand times the top of the calibrated range. The GPU
	// timer, not the GPU, is what got measured. A number like that is not "very fast", it is "no
	// reading", and it must not be allowed to silently select the most expensive preset.
	//
	// 10000 is deliberately two orders of magnitude above anything a real card can score, so this
	// cannot misfire on genuinely fast hardware — it only catches a broken clock.
	constexpr float ImplausibleGPUBenchmarkIndex = 10000.f;
	const bool bUsableReading = (GPUIndex > 0.f) && (GPUIndex < ImplausibleGPUBenchmarkIndex);

	if (!bUsableReading)
	{
		// Spec v11 §0: nothing that costs frames may become the default unless AUTO-DETECT CHOSE IT.
		// A benchmark that could not measure has chosen nothing, so fall back to the shipped default
		// (High at native scale, one step below what this build has effectively been running) rather
		// than to whatever the garbage index happened to point at. High and not Low/Medium because a
		// visible downgrade on hardware that never asked for one is its own bug report; the VIDEO
		// page and its live fps readout are how a player on a struggling machine goes lower.
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Video] Hardware benchmark produced no usable GPU reading (gpu=%.2f cpu=%.2f; the ")
			TEXT("calibrated range in BaseScalability.ini tops out at 115). Falling back to the ")
			TEXT("shipped default: HIGH at 100%% render scale. Use the VIDEO page to go lower."),
			GPUIndex, CPUIndex);

		ScalabilityQuality.SetFromSingleQualityLevel(static_cast<int32>(ETraceVideoQuality::High));
		ScalabilityQuality.ResolutionQuality = 100.f;
	}

	// Unlike SetOverallQuality this keeps whatever render scale the benchmark chose: on a weak GPU
	// that drop is the single largest win available, and here it was measured rather than guessed.
	ApplyHardwareBenchmarkResults();

	bHasAutoDetected = true;

	UE_LOG(LogTraceGame, Display,
		TEXT("[Video] Auto-detect complete. gpuIndex=%.2f cpuIndex=%.2f -> overall=%s renderScale=%d%%. %s"),
		GPUIndex, CPUIndex,
		*DescribeOverallQuality(GetOverallQuality()),
		GetResolutionScalePercent(),
		*DescribeCurrentSettings());

	// ApplyHardwareBenchmarkResults already saved the scalability state and the ini; this picks up
	// bHasAutoDetected, which it wrote before we set it.
	SaveSettings();

	GVideoChanged.Broadcast();
}

void UTraceGameUserSettings::ScheduleFirstRunAutoDetect()
{
	if (bAutoDetectScheduled || bHasAutoDetected)
	{
		return;
	}

	const TCHAR* DeclineReason = nullptr;
	if (IsRunningDedicatedServer())
	{
		DeclineReason = TEXT("dedicated server");
	}
	else if (!FApp::CanEverRender())
	{
		DeclineReason = TEXT("process cannot render");
	}
	else if (GIsEditor)
	{
		// PIE shares this settings object with the editor, and benchmarking would measure an editor
		// under load rather than the game. The player's own first launch is the run that matters.
		DeclineReason = TEXT("editor");
	}
	else if (FParse::Param(FCommandLine::Get(), TEXT("benchmark")))
	{
		DeclineReason = TEXT("-benchmark (fixed timestep capture)");
	}
	else if (FParse::Param(FCommandLine::Get(), TEXT("notracevideoautodetect")))
	{
		DeclineReason = TEXT("-notracevideoautodetect");
	}

	if (DeclineReason != nullptr)
	{
		// Deliberately does NOT set bHasAutoDetected. The next real client launch still detects.
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Video] First-run auto-detect skipped: %s."), DeclineReason);
		return;
	}

	bAutoDetectScheduled = true;

	UE_LOG(LogTraceGame, Display,
		TEXT("[Video] No previous auto-detect on this machine — hardware benchmark will run shortly."));

	// 0.75 s rather than inline. LoadSettings runs from UGameEngine::Init, where GEngine is not yet
	// "initialized", the game viewport does not exist and Scalability::SetQualityLevels is explicitly
	// skipped by ApplyNonResolutionSettings — a benchmark there would measure a half-built engine and
	// then apply into a world that is about to overwrite it.
	TWeakObjectPtr<UTraceGameUserSettings> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakThis](float /*DeltaTime*/) -> bool
		{
			if (UTraceGameUserSettings* const Settings = WeakThis.Get())
			{
				if (!Settings->bHasAutoDetected)
				{
					Settings->RunAutoDetect();
				}
			}
			return false;   // one shot
		}), 0.75f);
}

// =================================================================================================
// Reset and reporting
// =================================================================================================

void UTraceGameUserSettings::ResetVideoToDefaults()
{
	SetScreenResolution(GetDesktopResolution());
	SetFullscreenMode(EWindowMode::WindowedFullscreen);
	SetVSyncEnabled(false);
	SetFrameRateLimit(0.f);
	FieldOfView = DefaultFieldOfView;

	// HIGH, not EPIC. Spec v11 §0: nothing that costs frames may become the default unless
	// auto-detect chose it, and the engine's own default is Epic-minus-nothing. High is one step
	// down from what this build has been shipping, so a reset can only ever get cheaper.
	SetOverallQuality(ETraceVideoQuality::High);
	SetResolutionScalePercent(MaxResolutionScalePercent);

	bResolutionOptionsBuilt = false;
}

FString UTraceGameUserSettings::DescribeCurrentSettings() const
{
	const FIntPoint CurrentSize = GetScreenResolution();

	FString Report = FString::Printf(
		TEXT("mode=%s res=%dx%d scale=%d%% overall=%s vsync=%d cap=%s fov=%.0f autodetected=%d"),
		*DescribeWindowMode(GetFullscreenMode()),
		CurrentSize.X, CurrentSize.Y,
		GetResolutionScalePercent(),
		*DescribeOverallQuality(GetOverallQuality()),
		IsVSyncEnabled() ? 1 : 0,
		*DescribeFrameRateLimit(GetFrameRateLimit()),
		GetFieldOfView(),
		bHasAutoDetected ? 1 : 0);

	for (int32 GroupIndex = 0; GroupIndex < static_cast<int32>(ETraceQualityGroup::Count); ++GroupIndex)
	{
		const ETraceQualityGroup Group = static_cast<ETraceQualityGroup>(GroupIndex);
		Report += FString::Printf(TEXT(" | %s=%s"),
			GetGroupLabel(Group), *DescribeQualityLevel(GetGroupQuality(Group)));
	}

	return Report;
}

// =================================================================================================
// UGameUserSettings overrides
// =================================================================================================

void UTraceGameUserSettings::LoadSettings(bool bForceReload)
{
	Super::LoadSettings(bForceReload);

	// The resolution list depends on the loaded resolution and window mode.
	bResolutionOptionsBuilt = false;

	ScheduleFirstRunAutoDetect();

	// Register the FOV re-application exactly once per process. See GFOVAutoApply for why this is a
	// ticker and not a hook inside ATraceCharacter (which this class does not own).
	if (!GFieldOfViewTickerHandle.IsValid() && !IsRunningDedicatedServer() && FApp::CanEverRender())
	{
		TWeakObjectPtr<UTraceGameUserSettings> WeakThis(this);
		GFieldOfViewTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakThis](float /*DeltaTime*/) -> bool
			{
				if (GFOVAutoApply != 0)
				{
					if (UTraceGameUserSettings* const Settings = WeakThis.Get())
					{
						Settings->ApplyFieldOfViewToWorlds();
					}
				}
				return true;   // keep ticking
			}), 1.0f);
	}
}

void UTraceGameUserSettings::SetToDefaults()
{
	Super::SetToDefaults();

	// Super resets everything it declares. These are ours, and SetToDefaults runs BEFORE
	// LoadSettings on a fresh object, so forgetting them here would mean a stale FOV surviving a
	// version wipe. bHasAutoDetected is deliberately included: a wiped ini means a machine we know
	// nothing about again.
	FieldOfView = DefaultFieldOfView;
	bHasAutoDetected = false;
	bResolutionOptionsBuilt = false;
}

#if !UE_BUILD_SHIPPING

// =================================================================================================
// Console commands
//
// These exist because the VIDEO menu is being built in parallel and this backend had to be provable
// on its own — every one of them is a thin wrapper over the public API above, so a green run here
// is a green run for the menu. They are also the only way to reproduce the persistence check in the
// report without a human clicking through a UI.
//
// NAMING. Every command is "Trace.Video.<Verb>" and the one CVar in this file is
// "Trace.Video.FOVAutoApply". A console command and a CVar sharing a name is fatal at module load
// in this engine, so the two families must never converge on a spelling.
// =================================================================================================

namespace
{
	UTraceGameUserSettings* CommandTarget()
	{
		UTraceGameUserSettings* const Settings = UTraceGameUserSettings::Get();
		if (Settings == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[Video] No UTraceGameUserSettings available."));
		}
		return Settings;
	}

	void CmdStatusImpl()
	{
		if (UTraceGameUserSettings* const Settings = CommandTarget())
		{
			UE_LOG(LogTraceGame, Display, TEXT("[Video] %s"), *Settings->DescribeCurrentSettings());

			// The applied CVars, not the stored intent. If these ever disagree with the line above,
			// something outranked scalability — which is exactly the r.ScreenPercentage trap this
			// class was written around, so it is worth printing every time.
			const IConsoleVariable* const CVarScreenPercentage =
				IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
			const IConsoleVariable* const CVarShadowQuality =
				IConsoleManager::Get().FindConsoleVariable(TEXT("sg.ShadowQuality"));
			const IConsoleVariable* const CVarMaxFPS =
				IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
			const IConsoleVariable* const CVarVSync =
				IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));

			UE_LOG(LogTraceGame, Display,
				TEXT("[Video] live cvars: r.ScreenPercentage=%.1f sg.ShadowQuality=%d t.MaxFPS=%.1f r.VSync=%d"),
				CVarScreenPercentage ? CVarScreenPercentage->GetFloat() : -1.f,
				CVarShadowQuality ? CVarShadowQuality->GetInt() : -1,
				CVarMaxFPS ? CVarMaxFPS->GetFloat() : -1.f,
				CVarVSync ? CVarVSync->GetInt() : -1);
		}
	}

	void CmdListResolutionsImpl()
	{
		if (UTraceGameUserSettings* const Settings = CommandTarget())
		{
			const TArray<FTraceResolutionOption>& Options = Settings->GetResolutionOptions();
			const int32 CurrentIndex = Settings->GetResolutionOptionIndex();
			UE_LOG(LogTraceGame, Display, TEXT("[Video] %d resolution(s), current index %d, selectable=%d:"),
				Options.Num(), CurrentIndex, Settings->IsResolutionSelectable() ? 1 : 0);
			for (int32 Index = 0; Index < Options.Num(); ++Index)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[Video]   [%2d]%s %s%s"),
					Index,
					(Index == CurrentIndex) ? TEXT(" *") : TEXT("  "),
					*Options[Index].Label,
					Options[Index].bIsNative ? TEXT("  NATIVE") : TEXT(""));
			}
		}
	}

	void CmdSetQualityImpl(const TArray<FString>& Args)
	{
		UTraceGameUserSettings* const Settings = CommandTarget();
		if (Settings == nullptr || Args.Num() < 1)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Video] usage: Trace.Video.SetQuality <0=low 1=med 2=high 3=epic>"));
			return;
		}
		const int32 Level = FMath::Clamp(FCString::Atoi(*Args[0]),
			UTraceGameUserSettings::MinQualityLevel, UTraceGameUserSettings::MaxQualityLevel);
		Settings->SetOverallQuality(static_cast<ETraceVideoQuality>(Level));
		Settings->ApplyVideoSettings(/*bIncludingResolution=*/false);
		CmdStatusImpl();
	}

	void CmdSetGroupImpl(const TArray<FString>& Args)
	{
		UTraceGameUserSettings* const Settings = CommandTarget();
		if (Settings == nullptr || Args.Num() < 2)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Video] usage: Trace.Video.SetGroup <0..8 | ViewDistance|AntiAliasing|PostProcess|")
				TEXT("Shadows|GlobalIllumination|Reflections|Textures|Effects|Shading> <0..3>"));
			return;
		}

		int32 GroupIndex = INDEX_NONE;
		if (Args[0].IsNumeric())
		{
			GroupIndex = FCString::Atoi(*Args[0]);
		}
		else
		{
			static const TCHAR* Names[] =
			{
				TEXT("ViewDistance"), TEXT("AntiAliasing"), TEXT("PostProcess"), TEXT("Shadows"),
				TEXT("GlobalIllumination"), TEXT("Reflections"), TEXT("Textures"), TEXT("Effects"),
				TEXT("Shading")
			};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
			{
				if (Args[0].Equals(Names[Index], ESearchCase::IgnoreCase))
				{
					GroupIndex = Index;
					break;
				}
			}
		}

		if (GroupIndex < 0 || GroupIndex >= static_cast<int32>(ETraceQualityGroup::Count))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Video] unknown group '%s'"), *Args[0]);
			return;
		}

		Settings->SetGroupQuality(static_cast<ETraceQualityGroup>(GroupIndex), FCString::Atoi(*Args[1]));
		Settings->ApplyVideoSettings(/*bIncludingResolution=*/false);
		CmdStatusImpl();
	}

	void CmdSetScaleImpl(const TArray<FString>& Args)
	{
		UTraceGameUserSettings* const Settings = CommandTarget();
		if (Settings == nullptr || Args.Num() < 1)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Video] usage: Trace.Video.SetScale <50..100>"));
			return;
		}
		Settings->SetResolutionScalePercent(FCString::Atoi(*Args[0]));
		Settings->ApplyVideoSettings(/*bIncludingResolution=*/false);
		CmdStatusImpl();
	}

	void CmdSetResolutionImpl(const TArray<FString>& Args)
	{
		UTraceGameUserSettings* const Settings = CommandTarget();
		if (Settings == nullptr || Args.Num() < 2)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Video] usage: Trace.Video.SetResolution <width> <height> [0=fullscreen 1=borderless 2=windowed]"));
			return;
		}

		const int32 NewWidth = FCString::Atoi(*Args[0]);
		const int32 NewHeight = FCString::Atoi(*Args[1]);
		if (NewWidth < 320 || NewHeight < 240)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Video] refusing %dx%d"), NewWidth, NewHeight);
			return;
		}

		if (Args.Num() >= 3)
		{
			const int32 ModeIndex = FMath::Clamp(FCString::Atoi(*Args[2]), 0, 2);
			Settings->SetWindowMode(static_cast<EWindowMode::Type>(ModeIndex));
		}

		Settings->SetScreenResolution(FIntPoint(NewWidth, NewHeight));
		Settings->ApplyVideoSettings(/*bIncludingResolution=*/true);
		CmdStatusImpl();
	}

	void CmdSetFrameLimitImpl(const TArray<FString>& Args)
	{
		UTraceGameUserSettings* const Settings = CommandTarget();
		if (Settings == nullptr || Args.Num() < 1)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Video] usage: Trace.Video.SetFrameLimit <fps, 0 = unlimited>"));
			return;
		}
		Settings->SetFrameRateLimit(FCString::Atof(*Args[0]));
		Settings->ApplyVideoSettings(/*bIncludingResolution=*/false);
		CmdStatusImpl();
	}

	void CmdSetVSyncImpl(const TArray<FString>& Args)
	{
		UTraceGameUserSettings* const Settings = CommandTarget();
		if (Settings == nullptr || Args.Num() < 1)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Video] usage: Trace.Video.SetVSync <0|1>"));
			return;
		}
		Settings->SetVSyncEnabled(FCString::Atoi(*Args[0]) != 0);
		Settings->ApplyVideoSettings(/*bIncludingResolution=*/false);
		CmdStatusImpl();
	}

	void CmdSetFOVImpl(const TArray<FString>& Args)
	{
		UTraceGameUserSettings* const Settings = CommandTarget();
		if (Settings == nullptr || Args.Num() < 1)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Video] usage: Trace.Video.SetFOV <80..120>"));
			return;
		}
		Settings->SetFieldOfView(FCString::Atof(*Args[0]));
		Settings->ApplyVideoSettings(/*bIncludingResolution=*/false);
		CmdStatusImpl();
	}

	void CmdAutoDetectImpl()
	{
		if (UTraceGameUserSettings* const Settings = CommandTarget())
		{
			Settings->RunAutoDetect();
			CmdStatusImpl();
		}
	}

	void CmdResetImpl()
	{
		if (UTraceGameUserSettings* const Settings = CommandTarget())
		{
			Settings->ResetVideoToDefaults();
			Settings->ApplyVideoSettings(/*bIncludingResolution=*/true);
			CmdStatusImpl();
		}
	}

	FAutoConsoleCommand CmdVideoStatus(
		TEXT("Trace.Video.Status"),
		TEXT("Prints every video setting, plus the live r.ScreenPercentage / sg.ShadowQuality / ")
		TEXT("t.MaxFPS / r.VSync so stored intent can be compared against what the renderer got."),
		FConsoleCommandDelegate::CreateStatic(&CmdStatusImpl));

	FAutoConsoleCommand CmdVideoListResolutions(
		TEXT("Trace.Video.ListResolutions"),
		TEXT("Lists the resolutions the VIDEO page will offer, filtered to the current monitor."),
		FConsoleCommandDelegate::CreateStatic(&CmdListResolutionsImpl));

	FAutoConsoleCommand CmdVideoSetQuality(
		TEXT("Trace.Video.SetQuality"),
		TEXT("Trace.Video.SetQuality <0=low 1=medium 2=high 3=epic>. Sets every quality group and ")
		TEXT("leaves the render scale alone."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdSetQualityImpl));

	FAutoConsoleCommand CmdVideoSetGroup(
		TEXT("Trace.Video.SetGroup"),
		TEXT("Trace.Video.SetGroup <index|name> <0..3>. Sets one scalability group; the overall row ")
		TEXT("becomes CUSTOM."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdSetGroupImpl));

	FAutoConsoleCommand CmdVideoSetScale(
		TEXT("Trace.Video.SetScale"),
		TEXT("Trace.Video.SetScale <50..100>. Render (screen) percentage - the strongest performance ")
		TEXT("dial on a per-pixel-bound frame."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdSetScaleImpl));

	FAutoConsoleCommand CmdVideoSetResolution(
		TEXT("Trace.Video.SetResolution"),
		TEXT("Trace.Video.SetResolution <w> <h> [0=fullscreen 1=borderless 2=windowed]."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdSetResolutionImpl));

	FAutoConsoleCommand CmdVideoSetFrameLimit(
		TEXT("Trace.Video.SetFrameLimit"),
		TEXT("Trace.Video.SetFrameLimit <fps, 0 = unlimited>. NOTE: on macOS Metal, present pacing ")
		TEXT("survives 0 - a flat ~119.9 fps across every arm is the platform cap, not a result."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdSetFrameLimitImpl));

	FAutoConsoleCommand CmdVideoSetVSync(
		TEXT("Trace.Video.SetVSync"),
		TEXT("Trace.Video.SetVSync <0|1>."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdSetVSyncImpl));

	FAutoConsoleCommand CmdVideoSetFOV(
		TEXT("Trace.Video.SetFOV"),
		TEXT("Trace.Video.SetFOV <80..120>. Scene field of view; the view model's own FOV is left alone."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdSetFOVImpl));

	FAutoConsoleCommand CmdVideoAutoDetect(
		TEXT("Trace.Video.AutoDetect"),
		TEXT("Runs the hardware benchmark and applies its recommendation, render scale included. ")
		TEXT("This is what fires by itself on a machine's first launch."),
		FConsoleCommandDelegate::CreateStatic(&CmdAutoDetectImpl));

	FAutoConsoleCommand CmdVideoReset(
		TEXT("Trace.Video.Reset"),
		TEXT("Restores the shipped video defaults. Does not touch controls and does not re-benchmark."),
		FConsoleCommandDelegate::CreateStatic(&CmdResetImpl));
}

#endif // !UE_BUILD_SHIPPING
