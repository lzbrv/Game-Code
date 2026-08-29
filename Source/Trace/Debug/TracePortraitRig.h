// =================================================================================================
// Trace — TracePortraitRig.h
//
// The character-portrait capture rig: ONE lighting/camera set-up, dressed with each of the ten
// generated bodies in turn, photographed through the game's own renderer.
//
//   Trace.Portrait.Rig <CharacterId 1-10>   build/dress the rig for one character and look at it
//   Trace.Portrait.CaptureAll [OutDir]      sequence ids 1..10, one PNG each, then DONE n/10
//
// *** WHY THE GAME AND NOT THE EDITOR. *** Editor-python SceneCapture was MEASURED dead for this
// job (PIPELINE_DESIGN.md §0, runs 2 and 3: a commandlet's capture renders nothing and both export
// APIs are inert). The game launched `-game -RenderOffScreen -nosplash` demonstrably writes real
// frames headlessly — it produced every frame of the release visual audit through TraceAutoShot —
// so the portraits ride that proven path instead of a second, unproven one.
//
// *** ONE RIG, ONE RUN, TEN FRAMES. *** ART_BIBLE §7.5 requires the ten portraits to be provably
// ONE set: same camera, same lights, same backdrop, same margins, differing only in the subject and
// in the accent the rim light carries. A rig that were rebuilt per character could drift; this one
// is built once, dressed ten times, and torn down at the end.
//
// Dev-only; the whole file compiles out of Shipping.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

class UWorld;

#if !UE_BUILD_SHIPPING

namespace TracePortraitRig
{
	/**
	 * Ensures the rig exists in @p World and dresses it for @p CharacterId (a roster id, 1..10):
	 * loads that character's body onto the subject actor, tints the rim light with that character's
	 * accent, re-reads the framing CVars, and makes the rig camera the local view target.
	 *
	 * @return false when there is no world, no local controller, or the character's body asset is
	 *         not on this machine — each of which is logged with its own reason. A false is not a
	 *         crash and not a half-dressed rig: the subject is left hidden.
	 */
	bool Dress(UWorld* World, uint8 CharacterId);

	/**
	 * Sequences ids 1..10 through Dress(), one every 1.6 s, requesting a screenshot 1.2 s into each
	 * (which is the settle time TSR and streaming want before the shutter opens).
	 *
	 * Writes `<OutDir>/raw_<Name>.png` at the viewport's own resolution — launch the run at the
	 * supersampled portrait size (`-ResX=1024 -ResY=1024`) and the composite step downsamples.
	 * Prints one `[Portrait] wrote raw_<Name>.png` per file that reached disk and one
	 * `[Portrait] DONE <n>/10` at the end, where n counts FILES THAT EXIST rather than requests
	 * made — the distinction TraceAutoShot's own "requested" vs "written" pair exists to keep, and
	 * the reason a wrapper may grep `DONE 10/10` and believe it.
	 *
	 * @param OutDir Absolute or project-relative; empty means `<Project>/Saved/Portraits`.
	 */
	void CaptureAll(UWorld* World, const FString& OutDir);
}

#endif // !UE_BUILD_SHIPPING
