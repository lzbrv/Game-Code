// Copyright Trace. All Rights Reserved.

using UnrealBuildTool;

public class Trace : ModuleRules
{
	public Trace(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Shadowing: an ERROR on both machines - but the two compilers do NOT catch the same cases,
		// and the gap between them is where every Windows-only break in this project has come from.
		//
		// ON WINDOWS this setting makes MSVC's C4456 (local hides local), C4458 (local or parameter
		// hides a class member, INCLUDING one inherited from a base class) and C4459 (local hides a
		// global or namespace-scope name) errors. Past breaks: locals named Owner, Role, Player, Mesh,
		// bHidden, Bounds and Slot hiding members INHERITED from AActor, APlayerController,
		// ACharacter, USceneComponent and UTraceCharacterAbilitySet, and a local LogInput hiding the
		// engine's log category.
		//
		// ON THIS MAC (checked 2026-09-28: UE 5.8, Xcode 26.1.1) this setting adds -Wshadow, and the
		// build's -Werror makes that an error too. It is NOT a no-op. An earlier version of this
		// comment said it was, because UBT switches shadow warnings off for clang 17..18.1.3
		// (ApplyWarningsAttribute.cs) and `clang --version` prints "Apple clang 17.0.0". But UBT does
		// not compare that number: it maps the XCODE version to an LLVM version through
		// Engine/Config/Apple/Apple_SDK.json ("26.0.0-19.1.5"), so this toolchain counts as LLVM
		// 19.1.5, past the exemption - UBT's own log prints "Using Clang compiler 19.1.5" right
		// above "Apple clang version 17.0.0". See -Wshadow in any
		// Intermediate/Build/Mac/arm64/*/*/Trace/Module.Trace.*.cpp.o.rsp.
		//
		// So a Mac build DOES stop on: a local hiding another local; a local or method parameter
		// hiding a field declared in the SAME class; a local hiding a namespace-scope or global
		// variable that is visible in the same translation unit (an anonymous-namespace one, one a
		// `using namespace` exposed, or an engine global such as LogInput).
		//
		// IT DOES NOT STOP ON THESE, AND WINDOWS DOES - this is where the risk is:
		//   1. A member INHERITED from a base class (AActor::Owner, AActor::bHidden, ACharacter::Mesh,
		//      AHUD::Canvas). clang's -Wshadow never looks at base classes. Every C4458 break this
		//      project has had was this case. (-Wshadow-field catches a PARAMETER hiding an
		//      inherited member; no clang flag catches a LOCAL doing it.)
		//      Scripts/check-engine-member-shadowing.py (run by Scripts/build.sh) gates the AActor
		//      names; anything else is down to review.
		//   2. A CONSTRUCTOR parameter hiding a field. clang files that under
		//      -Wshadow-field-in-constructor, which -Wshadow does not include.
		//   3. A name from ANOTHER .cpp in the same unity blob. Blobs are grouped differently on each
		//      machine - adaptive unity compiles the files git reports as modified on their own - so a
		//      C4459 against a neighbouring file's namespace-scope name can exist on Windows only.
		//      Scripts/unity-hygiene.py (pre-commit) stops the `using namespace` form. Nothing
		//      gates a local named after another file's GLOBAL or ANONYMOUS-namespace variable,
		//      which is why file-local helpers go in NAMED namespaces: those names are not visible
		//      to the next file in the blob, so there is nothing for its locals to hide.
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
			"RHI",                // RHIGetGPUFrameCycles

			// ---- Added by the v17 migration, and they RETIRE build contract 7 ------------------
			//
			// "Contract 7: Canvas only" and "contract 2: this project cannot author .uassets" were
			// both true when written and are both now false. The arena bake proved the second wrong
			// by producing 572 placed actors and 66 material assets from a headless commandlet, and
			// this migration retires the first: menus and the HUD move to real UMG widget assets.
			//
			// The architecture is deliberately NOT "logic in Blueprint". Logic stays in C++
			// UUserWidget subclasses with BindWidget properties; the .uasset carries only the widget
			// TREE and its styling, which is the half a designer actually needs to edit. That keeps
			// every behaviour under the same review and the same harnesses it has today.
			"UMG",                // UUserWidget, UWidgetTree, the BindWidget contract
			"Slate",              // FSlateBrush / FSlateFontInfo used by the widget classes
			"SlateCore"
			// AIModule is an engine RUNTIME module, not a plugin, so this adds no plugin dependency
			// and no asset dependency. Nothing here uses BehaviorTree, Blackboard or the navigation
			// system: those would need .uassets (contract 2) or a runtime-built navmesh. The bots
			// steer directly with AddMovementInput against ATraceArenaBuilder::GetFieldBounds().
			// Deliberately NOT listed: Slate / SlateCore / PhysicsCore. Nothing in this module
			// includes a header from any of them - the HUD is pure AHUD::DrawText/DrawRect, UFont
			// comes from Engine/Font.h and the collision channels from Engine/EngineTypes.h, all of
			// which are Engine. UMG is absent for the same reason (contract 7: Canvas only).
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// P12 — the studio card and the travel card (UI/TraceLoadingScreen.cpp) go through the
			// engine's loading-screen player: GetMoviePlayer, IsMoviePlayerEnabled and
			// FLoadingScreenAttributes are MOVIEPLAYER_API, so without this the module COMPILES (the
			// header resolves) and then fails to LINK. An engine Runtime module that the Launch module
			// already links into every game on Mac and Win64 — it adds no plugin, no binary and no
			// .uproject entry. NOT "PreLoadScreen": that route needs a second module loaded before the
			// window exists, and the studio card gets nearly the same coverage from StartupModule.
			"MoviePlayer"
		});
	}
}
