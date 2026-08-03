// Copyright Trace. All Rights Reserved.

using UnrealBuildTool;

public class Trace : ModuleRules
{
	public Trace(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

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
