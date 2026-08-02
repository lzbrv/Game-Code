// Copyright Trace. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

/// <summary>
/// Editor target (Trace). Used by the editor and by PIE / "Play As Listen Server".
/// </summary>
public class TraceEditorTarget : TargetRules
{
	public TraceEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;

		// Tracks the engine deliberately - see Trace.Target.cs. Pinning to an older
		// BuildSettingsVersion breaks the shared build environment of an installed engine.
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("Trace");
	}
}
