// Copyright Trace. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

/// <summary>
/// Standalone game client/listen-server target (Trace).
/// Deliberately does not set SupportedPlatforms: the project must build for both
/// Mac (Apple Silicon) and Windows, and restricting the list here would break that.
/// </summary>
public class TraceTarget : TargetRules
{
	public TraceTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;

		// Must track the engine, NOT be pinned to an older version. With an installed
		// (launcher) engine every target here shares UnrealEditor's build environment, and
		// UBT hard-errors if a target changes any setting that environment already fixed:
		//     "TraceEditor modifies the values of properties: [ UndefinedIdentifierWarningLevel,
		//      UnreachableCodeWarningLevel, ReturnTypeWarningLevel, DanglingWarningLevel ] ...
		//      This is not allowed, as TraceEditor has build products in common with UnrealEditor."
		// V5 turns those four warning levels off; 5.8's defaults make them errors. Latest is
		// also what keeps this project buildable on 5.4 through 5.8 from one checkout.
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("Trace");
	}
}
