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
			"AIModule"            // AAIController base for ATraceBotController
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
