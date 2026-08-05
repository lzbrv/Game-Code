// Copyright Trace. All Rights Reserved.

using UnrealBuildTool;

public class Trace : ModuleRules
{
	public Trace(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Catch variable shadowing on macOS/clang too.
		//
		// MSVC enables C4458 (local hides class member) and C4459 (local hides global) as part of
		// Unreal's warnings-as-errors set; clang does not by default. That asymmetry meant a clean
		// Mac build shipped code that failed outright on Windows - locals named Character, Mesh,
		// bHidden, Bounds and LogInput shadowing ACharacter::Mesh, AActor::bHidden,
		// UPrimitiveComponent::Bounds and the engine's LogInput category.
		//
		// Module-scoped deliberately: setting this on the Target would modify the shared build
		// environment, which an installed (launcher) engine refuses with "modifies the values of
		// properties ... not allowed, as TraceEditor has build products in common with
		// UnrealEditor".
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Error;

		// This module uses a flat layout (no Public/Private split), so make the module root an
		// explicit public include path. That guarantees both forms resolve from anywhere in the
		// module, regardless of UBT's legacy-include-path defaults:
		//     #include "TraceTypes.h"
		//     #include "Core/TraceGameMode.h"
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",          // FKey / EKeys used when building the mapping context in C++
			"EnhancedInput",      // UInputAction, UInputMappingContext, UEnhancedInputComponent
			"DeveloperSettings",  // UDeveloperSettings base for UTraceSettings
			"NetCore",            // FFastArraySerializer (Net/Serialization/FastArraySerializer.h)
			"AIModule",           // AAIController base for ATraceBotController
			// Both of the following are LINK-time requirements of UI/TraceNetworking.cpp. Their
			// headers resolve transitively through Engine, so omitting them still COMPILES cleanly
			// and then fails at the very end with "Undefined symbols for architecture arm64" - do
			// not remove either because the includes appear to be satisfied.
			"Sockets",            // ISocketSubsystem::Get/CreateUniqueSocket - adapter enumeration
			                      // and the UDP 7777 port probe behind TraceNet::.
			"ApplicationCore",    // FPlatformApplicationMisc::ClipboardPaste - paste into the JOIN
			                      // address field. Optional: set TRACE_HAS_CLIPBOARD 0 at the top of
			                      // TraceNetworking.cpp and this entry can go, at the cost of paste.
			// The SAME TRAP AS THE TWO ABOVE, and it caught this project once already: RenderCore and
			// RHI headers resolve transitively through Engine, so UTraceTrailComponent's perf probe
			// compiled and then failed at link with "Undefined symbols" for GGameThreadTime /
			// GRenderThreadTime / RHIGetGPUFrameCycles. The probe currently routes around it via
			// FStatUnitData from the viewport client, which works but reads the engine's smoothed
			// numbers rather than the raw counters. These are here so Trace.Trail.PerfAB - the A/B
			// that produced this pass's before/after frame times, and the only way to reproduce
			// them - can read the counters directly instead.
			"RenderCore",         // GGameThreadTime, GRenderThreadTime
			"RHI"                 // RHIGetGPUFrameCycles
			// AIModule is an engine RUNTIME module, not a plugin, so this adds no plugin dependency
			// and no asset dependency. Nothing here uses BehaviorTree, Blackboard or the navigation
			// system: those would need .uassets (contract 2) or a runtime-built navmesh. The bots
			// steer directly with AddMovementInput against ATraceArenaBuilder::GetFieldBounds().
			// Deliberately NOT listed: Slate / SlateCore / PhysicsCore. Nothing in this module
			// includes a header from any of them - the HUD is pure AHUD::DrawText/DrawRect, UFont
			// comes from Engine/Font.h and the collision channels from Engine/EngineTypes.h, all of
			// which are Engine. UMG is absent for the same reason (contract 7: Canvas only).
		});

		// Nothing private yet. UMG is intentionally absent: the HUD is pure Canvas (contract 7).
		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
