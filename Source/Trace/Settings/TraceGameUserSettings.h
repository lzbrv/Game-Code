// Trace — the player's VIDEO settings: window, resolution, render scale, scalability, vsync, cap.
//
// WHY THIS CLASS EXISTS AT ALL (spec v11 §1)
// UTraceUserSettings (next door) derives from UObject. That was fine for mouse feel and keybinds,
// but it is exactly why this project shipped with NO resolution, NO fullscreen, NO vsync, NO frame
// cap and NO scalability: Unreal puts every one of those on UGameUserSettings, and nothing in this
// project was one. This class IS a UGameUserSettings, registered through
//     [/Script/Engine.Engine] GameUserSettingsClassName=/Script/Trace.TraceGameUserSettings
// in Config/DefaultEngine.ini, so GEngine->GetGameUserSettings() returns it and every engine path
// that already knows how to persist, validate and apply video settings works on it unmodified.
//
// TWO CLASSES, TWO JOBS, NO MERGING.
//   UTraceUserSettings      -> controls: sensitivity, invert, keybinds. Saved/Config/<P>/TraceUserSettings.ini
//   UTraceGameUserSettings  -> video.                                    Saved/Config/<P>/GameUserSettings.ini
// They share nothing and neither may grow into the other.
//
// WHAT THIS CLASS DELIBERATELY DOES NOT REIMPLEMENT
// Resolution changes, fullscreen transitions, the scalability CVar tables, the hardware benchmark
// and the ini round-trip are all engine machinery and all of it is called, not copied. What is
// added here is only the part the engine has no opinion about:
//   * an option LIST the Canvas menu can page through (the engine gives you a raw mode dump),
//   * an overall Low/Medium/High/Epic/Custom that IGNORES resolution scale (see GetOverallQuality),
//   * first-run auto-detect that actually fires by itself,
//   * a field of view, which is a video setting to a player and a camera property to the engine.
//
// THE ONE ENGINE-CONFIG TRAP THAT HAD TO BE FIXED FOR ANY OF THIS TO WORK
// Scalability pushes the render scale onto r.ScreenPercentage at ECVF_SetByScalability (0x02...).
// Config/DefaultEngine.ini's [/Script/Engine.RendererSettings] block is applied at
// ECVF_SetByProjectSetting (0x04...), which is STRONGER. So while that file said
// "r.ScreenPercentage=100", every write from this class was silently discarded and the single most
// effective performance dial in a per-pixel-bound frame did nothing at all. DefaultEngine.ini now
// carries only r.ScreenPercentage.Default=100 plus r.ScreenPercentage.Default.Desktop.Mode=0
// (Manual), which produces the same 100% baseline through
// FStaticResolutionFractionHeuristic::FUserSettings::PullRunTimeRenderingSettings while leaving
// r.ScreenPercentage itself at 0 and therefore writable by scalability. Do not put
// r.ScreenPercentage back in that section.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/DelegateCombinations.h"
#include "GameFramework/GameUserSettings.h"
#include "GenericPlatform/GenericWindow.h"   // EWindowMode::Type

#include "TraceGameUserSettings.generated.h"

/**
 * The overall preset row.
 *
 * Custom is a real, reachable state and not an error: touching any single group puts the player
 * here, and the menu should print it rather than lie about which preset is active. Cinematic
 * (scalability level 4) is deliberately NOT offered — it exists in the engine tables but it is a
 * cutscene-capture tier, not a play tier, and this is a 5v5 shooter.
 */
UENUM()
enum class ETraceVideoQuality : uint8
{
	Low = 0,
	Medium = 1,
	High = 2,
	Epic = 3,
	/** Not a level. Returned when the groups disagree; never accepted by SetOverallQuality. */
	Custom = 4
};

/**
 * The nine individual scalability groups the VIDEO page exposes, in the order it should list them.
 *
 * The engine has twelve. Foliage and Landscape are omitted because this arena has neither a single
 * foliage instance nor a landscape actor, so a row for each would be two knobs that provably do
 * nothing — the exact failure mode this project has been bitten by before. They are still kept
 * numerically in step by SetOverallQuality so a future map does not inherit a stale value.
 * ResolutionQuality is also absent on purpose: it is its own row (GetResolutionScalePercent) and it
 * must not be dragged around by the preset. See GetOverallQuality for why that matters.
 */
UENUM()
enum class ETraceQualityGroup : uint8
{
	ViewDistance = 0,
	AntiAliasing,
	PostProcess,
	Shadows,
	GlobalIllumination,
	Reflections,
	Textures,
	Effects,
	Shading,

	Count UMETA(Hidden)
};

/**
 * One selectable entry in the resolution row.
 *
 * A plain struct, not a USTRUCT: nothing reflects it, nothing replicates it, and the menu that
 * consumes it is C++ Canvas code. Keeping it out of UHT means this header stays cheap to include.
 */
struct FTraceResolutionOption
{
	FIntPoint Resolution = FIntPoint(0, 0);

	/** Highest refresh rate the mode was reported at. Informational; the label does not print it. */
	int32 RefreshRate = 0;

	/** Ready to draw, upper case, aspect annotated: "1920 x 1080  (16:9)". */
	FString Label;

	/** True for the entry that matches the desktop resolution of the monitor the game is on. */
	bool bIsNative = false;
};

/**
 * Fires after any video setting has been changed AND applied through this class.
 *
 * The menu redraws from live getters every frame, so it does not strictly need this; the HUD and
 * anything caching a derived value do. File scope rather than inside the UCLASS for the same reason
 * as FTraceUserSettingsChanged: UHT has no business parsing a delegate macro.
 */
DECLARE_MULTICAST_DELEGATE(FTraceVideoSettingsChanged);

/**
 * Trace's video settings.
 *
 * PERSISTENCE. Everything here lands in Saved/Config/<Platform>/GameUserSettings.ini:
 * the inherited properties under [/Script/Engine.GameUserSettings], the ones added here under
 * [/Script/Trace.TraceGameUserSettings], and the scalability levels under [ScalabilityGroups]
 * (written by Scalability::SaveState, which UGameUserSettings::SaveSettings calls for us).
 * ApplyVideoSettings() saves and flushes on every change rather than on close, because this build
 * is normally terminated with a kill and an unflushed setting is a setting the player has to redo.
 *
 * THREADING. Game thread only. Every setter ends up in an IConsoleVariable::Set.
 */
UCLASS(config = GameUserSettings, configdonotcheckdefaults)
class TRACE_API UTraceGameUserSettings : public UGameUserSettings
{
	GENERATED_BODY()

public:
	UTraceGameUserSettings();

	// =============================================================================================
	// Access
	// =============================================================================================

	/**
	 * The one accessor. Never null in a normal client; null only if the engine is gone (shutdown)
	 * or GameUserSettingsClassName was not pointed at this class, which the log will have said.
	 */
	static UTraceGameUserSettings* Get();

	/** Change broadcast. See FTraceVideoSettingsChanged. */
	static FTraceVideoSettingsChanged& OnVideoChanged();

	/**
	 * Apply everything, persist it, and broadcast.
	 *
	 * @param bIncludingResolution  false skips the window/resolution half, which is what a slider
	 *                              being dragged wants: changing render scale must not tear the
	 *                              window down and rebuild it sixty times a second.
	 *
	 * Always passes bCheckForCommandLineOverrides=false to the engine. -ResX/-ResY on the command
	 * line already win at startup through UGameUserSettings::PreloadResolutionSettings; letting
	 * them win again HERE would mean a player who changes resolution in the menu watches it snap
	 * back, with no visible cause, on every machine launched from a script.
	 */
	void ApplyVideoSettings(bool bIncludingResolution = true);

	// =============================================================================================
	// Window mode
	// =============================================================================================

	EWindowMode::Type GetWindowMode() const;

	/** Stores the mode. Call ApplyVideoSettings(true) to make the window actually change. */
	void SetWindowMode(EWindowMode::Type NewMode);

	/** Fullscreen, WindowedFullscreen, Windowed — in the order the menu should cycle them. */
	static const TArray<EWindowMode::Type>& GetWindowModeOptions();

	/** "FULLSCREEN" / "BORDERLESS" / "WINDOWED". */
	static FString DescribeWindowMode(EWindowMode::Type Mode);

	// =============================================================================================
	// Resolution
	// =============================================================================================

	/**
	 * Every resolution the current monitor can present, ascending by pixel count.
	 *
	 * Built from RHIGetAvailableResolutions and then filtered to the monitor the game window is on
	 * (anything larger than that monitor's desktop rect is dropped — the RHI happily reports modes
	 * from other displays and from the panel's full native list). Built once, lazily, and cached;
	 * call RefreshResolutionOptions after a monitor change.
	 *
	 * Never empty: if the RHI declines to enumerate (it is unimplemented on some platforms, and a
	 * -nullrhi run has no RHI at all) this falls back to a fixed 16:9 ladder clamped to the desktop
	 * size, so the menu always has something to page through.
	 */
	const TArray<FTraceResolutionOption>& GetResolutionOptions() const;

	/** Rebuilds the cache above. */
	void RefreshResolutionOptions();

	/** Index into GetResolutionOptions of the current resolution, or INDEX_NONE if it is not in the list. */
	int32 GetResolutionOptionIndex() const;

	/** Stores the resolution at @p OptionIndex. Call ApplyVideoSettings(true) to apply it. */
	void SetResolutionByOptionIndex(int32 OptionIndex);

	/**
	 * False in WindowedFullscreen, where the resolution is the desktop's and the row is meaningless.
	 * The menu should grey the row out rather than hide it, so the layout does not jump.
	 */
	bool IsResolutionSelectable() const;

	// =============================================================================================
	// Resolution scale  —  spec v11 §2.3, the most valuable dial on the page
	// =============================================================================================
	//
	// The frame is GPU-bound PER PIXEL, so this is the one row that scales the whole cost curve
	// rather than trimming a feature off it. It belongs high in the list and it deserves the
	// annotation the spec asks for.

	static constexpr int32 MinResolutionScalePercent = 50;
	static constexpr int32 MaxResolutionScalePercent = 100;

	/** 50..100. Rounded from the engine's float; the menu never has to see a 66.662. */
	int32 GetResolutionScalePercent() const;

	/** Clamped to [MinResolutionScalePercent, MaxResolutionScalePercent]. Cheap: no window rebuild. */
	void SetResolutionScalePercent(int32 NewPercent);

	// =============================================================================================
	// Quality — overall preset and the nine groups
	// =============================================================================================

	static constexpr int32 MinQualityLevel = 0;   // Low
	static constexpr int32 MaxQualityLevel = 3;   // Epic

	/**
	 * The preset the nine exposed groups currently agree on, or Custom.
	 *
	 * NOT UGameUserSettings::GetOverallScalabilityLevel(). That one also requires
	 * ScalabilityQuality.ResolutionQuality to equal the render scale its ladder associates with the
	 * level (50 / 71 / 87 / 100), so a player on High who drags render scale to 75% is reported as
	 * "Custom" by the engine even though every quality group is still exactly High. On this page
	 * render scale is its own row; conflating the two would make the two rows fight in a way the
	 * player cannot see the cause of.
	 */
	ETraceVideoQuality GetOverallQuality() const;

	/**
	 * Sets all nine groups (plus foliage and landscape, kept in step but unexposed) to one level.
	 *
	 * PRESERVES the current resolution scale, for the reason above. Scalability's own
	 * SetFromSingleQualityLevel would have overwritten it with the ladder value. Auto-detect is the
	 * one path that IS allowed to move render scale, because there the benchmark is what chose it.
	 *
	 * ETraceVideoQuality::Custom is not a level and is ignored.
	 */
	void SetOverallQuality(ETraceVideoQuality NewQuality);

	/** 0..3, Low..Epic. */
	int32 GetGroupQuality(ETraceQualityGroup Group) const;

	/** Clamped to 0..3. Flips the overall row to Custom the moment it disagrees with its neighbours. */
	void SetGroupQuality(ETraceQualityGroup Group, int32 NewLevel);

	/** "VIEW DISTANCE", "ANTI-ALIASING", ... — the menu's row labels, upper case, never localised. */
	static const TCHAR* GetGroupLabel(ETraceQualityGroup Group);

	/** "LOW" / "MEDIUM" / "HIGH" / "EPIC". */
	static FString DescribeQualityLevel(int32 Level);

	/** As above, plus "CUSTOM". */
	static FString DescribeOverallQuality(ETraceVideoQuality Quality);

	// =============================================================================================
	// VSync and frame cap
	// =============================================================================================
	//
	// VSync itself is inherited: IsVSyncEnabled() / SetVSyncEnabled(bool).
	//
	// A WARNING FOR WHOEVER MEASURES WITH THESE. On macOS Metal, present pacing survives both
	// SetFrameRateLimit(0) and SetVSyncEnabled(false) — a table where every arm reads ~8.34 ms /
	// 119.9 fps is the platform cap, not a result. That has shipped a wrong headline in this project
	// once already. Measure GPU-bound at r.ScreenPercentage 300.

	/** { 0, 30, 60, 120, 144, 240 }. 0 is Unlimited and is index 0 on purpose. */
	static const TArray<float>& GetFrameRateLimitOptions();

	/** Index into the array above; falls back to the nearest entry for a hand-edited ini value. */
	int32 GetFrameRateLimitIndex() const;

	void SetFrameRateLimitByIndex(int32 OptionIndex);

	/** "UNLIMITED" / "60 FPS". */
	static FString DescribeFrameRateLimit(float Limit);

	/**
	 * Pushes the stored cap onto t.MaxFPS on its own, without the rest of an apply.
	 *
	 * This exists because UGameUserSettings::SetFrameRateLimitCVar and GetEffectiveFrameRateLimit
	 * are both PROTECTED, so the menu — which is not a UGameUserSettings — cannot reach either, and
	 * the only alternative is ApplyNonResolutionSettings(), which also re-validates settings,
	 * re-applies every scalability group and calls every console variable sink. That is far too much
	 * machinery to hang off one row of a menu. A subclass can reach the protected pair; the menu
	 * calls this.
	 */
	void ApplyFrameRateLimitNow();

	// =============================================================================================
	// Field of view
	// =============================================================================================
	//
	// A video setting to the player; a UCameraComponent property to the engine. DefaultFieldOfView
	// is the 95 that ATraceCharacter's constructor hard-codes today, so shipping this changes
	// nothing until the player moves it.
	//
	// HOW IT REACHES THE CAMERA. ApplyFieldOfViewToWorlds() walks the local player controllers'
	// pawns and sets UCameraComponent::FieldOfView. It runs on apply and again from a 1 Hz ticker,
	// which is what makes it survive a respawn without this class having to reach into
	// ATraceCharacter (which it does not own). The ticker only writes when the value actually
	// differs, and can be turned off with Trace.Video.FOVAutoApply 0.
	//
	// It deliberately does NOT touch UCameraComponent::FirstPersonFieldOfView. That is the view
	// model's own projection and ATraceCharacter documents the relationship it has with the scene
	// FOV; widening the world should not stretch the gun.

	static constexpr float MinFieldOfView = 80.f;
	static constexpr float MaxFieldOfView = 120.f;
	static constexpr float DefaultFieldOfView = 95.f;

	float GetFieldOfView() const;

	/** Clamped. Applies to any live camera immediately; persisted by the next ApplyVideoSettings. */
	void SetFieldOfView(float NewFOV);

	/** Pushes the stored FOV onto every local player's camera component. Safe to call at any time. */
	void ApplyFieldOfViewToWorlds();

	// =============================================================================================
	// Auto-detect  —  spec v11 §1, "the single most valuable behaviour in this whole pass"
	// =============================================================================================

	/**
	 * Runs the engine's synthetic CPU/GPU benchmark and applies what it recommends.
	 *
	 * This is both the AUTO-DETECT QUALITY row and the thing that fires by itself on a machine's
	 * very first launch. A player on a weak GPU gets a preset without ever finding this menu, which
	 * is the entire point: the collaborator who reported the build "unplayable at 1/6 the fps" had
	 * no dial to reach for and no number to read.
	 *
	 * Unlike SetOverallQuality this DOES move the render scale, because here the benchmark chose it.
	 * Costs a visible hitch (tens of ms of synthetic work); never call it per frame.
	 *
	 * If the benchmark comes back non-positive — no RHI, a null adapter, a virtualised GPU — this
	 * falls back to Medium at 100% scale rather than trusting a zero, and says so in the log.
	 */
	void RunAutoDetect();

	/** True once auto-detect has ever completed on this machine. Persisted. */
	bool HasAutoDetected() const { return bHasAutoDetected; }

	/** The benchmark's GPU perf index, or -1 if it has never run. Worth printing in the menu. */
	float GetGPUBenchmarkIndex() const { return GetLastGPUBenchmarkResult(); }

	// =============================================================================================
	// Reset
	// =============================================================================================

	/**
	 * Back to shipped defaults for VIDEO only: desktop resolution, borderless, High, 100% scale,
	 * vsync off, no cap, FOV 95. Does not touch UTraceUserSettings, and does not re-run the
	 * benchmark — a player who resets has said "give me the shipped state", not "guess again".
	 */
	void ResetVideoToDefaults();

	/** Multi-line dump for the log and the Trace.Video.Status console command. */
	FString DescribeCurrentSettings() const;

	// =============================================================================================
	// UGameUserSettings
	// =============================================================================================

	/** Also arms first-run auto-detect. See ScheduleFirstRunAutoDetect. */
	virtual void LoadSettings(bool bForceReload = false) override;

	/** Also resets the video-only additions this class declares. */
	virtual void SetToDefaults() override;

private:
	/**
	 * Set once auto-detect has run, so it never runs twice on the same machine.
	 *
	 * A config property and not a marker file: it rides in the same ini as everything else it
	 * guards, so deleting GameUserSettings.ini to "start clean" genuinely starts clean, including
	 * the benchmark. If the engine ever wipes the ini for a version bump, the benchmark re-runs,
	 * which is the correct answer to "these settings are from a different build".
	 */
	UPROPERTY(config)
	bool bHasAutoDetected = false;

	/** Degrees, clamped to [MinFieldOfView, MaxFieldOfView]. See the field-of-view block above. */
	UPROPERTY(config)
	float FieldOfView = DefaultFieldOfView;

	/**
	 * Arms the one-shot first-run benchmark, or declines and says why.
	 *
	 * Declines on a dedicated server, in the editor (PIE shares this object and would benchmark the
	 * editor's own load), when the process cannot render, under -benchmark (the engine's fixed-step
	 * mode, where a synthetic GPU load would corrupt the very numbers being collected), and under
	 * -notracevideoautodetect. Declining does NOT set bHasAutoDetected: the next real client launch
	 * on this machine still gets its detection.
	 *
	 * Deferred onto a ticker rather than run inline. LoadSettings is called from UGameEngine::Init,
	 * before the game viewport exists and before GEngine->IsInitialized(); benchmarking there would
	 * be measuring a half-built engine.
	 */
	void ScheduleFirstRunAutoDetect();

	/** Rebuilds ResolutionOptions. Const because GetResolutionOptions fills the cache on demand. */
	void BuildResolutionOptions() const;

	mutable TArray<FTraceResolutionOption> ResolutionOptions;
	mutable bool bResolutionOptionsBuilt = false;

	/** Guards ScheduleFirstRunAutoDetect against LoadSettings being called more than once (it is). */
	bool bAutoDetectScheduled = false;
};
