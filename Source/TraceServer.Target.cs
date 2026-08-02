// Copyright Trace. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

/// <summary>
/// Dedicated server target (Trace).
///
/// Build with, e.g.:
///   RunUAT BuildCookRun -project=Trace.uproject -server -serverconfig=Development -noclient
/// or directly:
///   Build.sh TraceServer Mac Development -project=/abs/path/Trace.uproject
///
/// The dedicated server cooks without renderer data, so no gameplay code may *require*
/// a material/mesh to resolve. Runtime code guards visual setup with
/// GetNetMode() != NM_DedicatedServer (see the build contract, section 2).
/// </summary>
public class TraceServerTarget : TargetRules
{
	public TraceServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;

		// Tracks the engine deliberately - see Trace.Target.cs. Pinning to an older
		// BuildSettingsVersion breaks the shared build environment of an installed engine.
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		// A headless server with no log is undebuggable in the field; the cost is negligible.
		bUseLoggingInShipping = true;

		ExtraModuleNames.Add("Trace");
	}
}
