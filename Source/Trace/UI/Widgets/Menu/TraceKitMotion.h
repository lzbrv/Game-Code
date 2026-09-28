// Trace — the handmade kit's motion: ONE open/close fade type, on real time (P10).
//
// Its own tiny header so a screen can HOLD a fade as a member (FTraceTeamSelect, FTraceLoadoutSelect,
// FTraceOptionsMenu, ATraceHUD) without its header pulling in the whole kit renderer. The timings and
// everything that draws at a fade's alpha (FScopedOpacity, Faded) are in TraceMenuKit.h; the
// implementation is in TraceMenuKit.cpp.

#pragma once

#include "CoreMinimal.h"

/**
 * ONE OPEN/CLOSE FADE, ON REAL TIME (P10). Feed it whether the thing should be on screen, every frame;
 * read Alpha() and draw at it. A kit screen owns one per overlay.
 *
 *     const float A = Fade.Update(bOpen);              // real clock: TraceMenuKit::RealSeconds()
 *     if (A > 0.f) { TraceMenuKit::FScopedOpacity Fading(A); Draw(...); }
 *
 * THE CLOCK IS THE APPLICATION'S, NEVER THE WORLD'S. UWorld::GetTimeSeconds stops dead while the world
 * is paused (the pause menu pauses a standalone match), so a fade timed on it would never finish
 * opening the very menu that paused it. It is RATE-based on elapsed real seconds rather than stepped
 * per frame, so it takes the same time at 30 fps and at 240, and a reversal mid-fade (open, close
 * again 60 ms later) turns round from where it is instead of jumping. Calling Update twice in one
 * frame is harmless: the second call sees no time pass.
 *
 * Alpha() is eased (smoothstep); Linear() is the raw progress, for a harness.
 */
struct TRACE_API FTraceKitFade
{
	/** Real-time update: the target for this frame; returns Alpha(). */
	float Update(bool bShown, float InSeconds = -1.f, float OutSeconds = -1.f);

	/** The same, at an explicit time in seconds — the harness drives a fade frame by frame with this. */
	float UpdateAt(bool bShown, double NowSeconds, float InSeconds = -1.f, float OutSeconds = -1.f);

	/** Jumps straight to fully shown or fully hidden (no animation), e.g. for a screen that must not fade. */
	void Snap(bool bShown);

	/** Eased opacity, 0..1. */
	float Alpha() const;

	/** Raw progress, 0..1, linear in real time. */
	float Linear() const { return Progress; }

	/** Anything to draw at all (fading in, fully in, or fading out). */
	bool IsVisible() const { return Progress > 0.f; }

	/** Fully in: nothing behind it needs to be drawn any more if it is opaque. */
	bool IsFullyShown() const { return Progress >= 1.f; }

	/** The last target it was given. */
	bool IsTargetShown() const { return bTarget; }

private:
	float Progress = 0.f;
	double LastTime = -1.0;
	bool bTarget = false;
};
