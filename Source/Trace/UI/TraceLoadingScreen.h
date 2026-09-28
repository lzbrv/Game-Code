// =================================================================================================
// Trace — TraceLoadingScreen.h   (P12)
//
// THE STUDIO CARD AND THE TRAVEL CARD — what the window shows while the game thread cannot draw.
//
// Before this there was nothing. From the moment the movie player opened the game window at boot to
// the first title frame the window was empty black for seconds, and every travel (PLAY, PRACTICE,
// JOIN once connected, RETURN TO TITLE, the end of a match) froze on its last frame for the whole of
// UEngine::LoadMap — up to 2.3 s measured, long enough on macOS for the beachball.
//
// Both cards are Slate widgets handed to the engine's movie player (FLoadingScreenAttributes), which
// paints them on its own thread (the "SlateLoadingThread") while the game thread is busy, and then
// on the game thread inside WaitForMovieToFinish. They are built from the handmade kit:
//
//   STUDIO   at boot, only when the game starts on its default map (the title): black, the studio's
//            name in white Sofachrome, faded in, held at least StudioHoldSeconds from when it came up,
//            faded out only once the engine has finished loading. Then the title fades in from black.
//   TRAVEL   on every LoadMap after that: black, the title's TRACE wordmark and swoosh exactly where
//            the title draws them, the travel's caption where the title's travel overlay draws it,
//            and the kit's crescent turning in the corner. It auto-completes: it adds no wait to a
//            load, so a listen host's clients see no extra stall.
//
// The safe warm-up the P11 hitch list asked for runs behind the studio card, on the game thread
// (see WarmUnderStudioCard in the .cpp for exactly what and why each item is there).
//
// NOT IN THE EDITOR, PIE, COMMANDLETS, A DEDICATED SERVER, -nullrhi OR -NoLoadingScreen: the engine's
// IsMoviePlayerEnabled() is false for all of them and Startup() binds nothing. Under -RenderOffScreen
// the movie player does run (it paints an off-screen window), so headless harness runs go through the
// same code the player sees. `Trace.UI.LoadingCard` mounts a card in the viewport for a screenshot;
// `Trace.UI.LoadingCard.Verify` checks what happened this process.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

namespace TraceLoadingScreen
{
	enum class ECard : uint8
	{
		None,
		Studio,
		Travel
	};

	// ---- Timing (seconds, platform clock) -------------------------------------------------------------

	/** The studio name's fade-in, from the moment the card is up. */
	static constexpr double StudioFadeInSeconds = 0.45;

	/**
	 * The earliest the name may START fading out, measured from the card coming up. With the fade that
	 * makes the card's minimum life StudioHoldSeconds + StudioFadeOutSeconds = 2.4 s. It fades out
	 * later than this whenever the engine is still loading.
	 */
	static constexpr double StudioHoldSeconds = 2.0;

	/** The name's fade-out, once the hold is over AND the engine has finished loading. */
	static constexpr double StudioFadeOutSeconds = 0.40;

	/** After loading has finished, the card comes down within this long whatever else happens. */
	static constexpr double StudioWatchdogSeconds = 10.0;

	/** The title's fade-in from black after the studio card lifts (ATraceMenuHUD). */
	static constexpr float TitleFadeInSeconds = 0.35f;

	// ---- Lifetime (FTraceGameModule) ----------------------------------------------------------------

	/** StartupModule: if the movie player runs, warm the card's art, put the studio card up, bind travel. */
	void Startup();

	/** ShutdownModule: unbinds. Safe if Startup bound nothing. */
	void Shutdown();

	// ---- For the rest of the game (game thread) -------------------------------------------------------

	/**
	 * The caption the NEXT travel card shows, e.g. "HOSTING ON 100.64.0.2:7777". Call it just before
	 * OpenLevel / ClientTravel with the same line the title's travel overlay shows, so the card that
	 * replaces the overlay says the same thing. Consumed by that card. Never shown on a load of the
	 * title map (a failed JOIN returning home must not say "CONNECTING TO"). Empty clears it.
	 */
	TRACE_API void SetNextTravelCaption(const FString& Caption);

	/** True while a card (either kind) owns the window. */
	TRACE_API bool IsCardUp();

	/** Which card owns the window right now (None when none does). */
	TRACE_API ECard CurrentCardKind();

	/** FPlatformTime::Seconds() when the last card came down, or 0 when none has. */
	TRACE_API double LastCardEndSeconds();

	/** Which kind of card came down last (None before the first). */
	TRACE_API ECard LastCardKind();

	/**
	 * True while a card is up, or within @p GraceSeconds (platform clock) of one lifting. The title's
	 * activation grace asks this: its own grace runs on WORLD time, which is frozen under a card and
	 * jumps 0.4 s on the first frame after it, so on its own it expired before the window had focus —
	 * and before the key or click that skipped the card had been released.
	 */
	TRACE_API bool IsWithinCardGrace(double GraceSeconds);

#if !UE_BUILD_SHIPPING
	/** One card's life, for Trace.UI.LoadingCard.Verify and the log. All times FPlatformTime::Seconds(). */
	struct FCardRecord
	{
		ECard Kind = ECard::None;
		/** The map the card covered ("" for the studio card until its LoadMap is seen). */
		FString Map;
		FString Caption;
		double Up = 0.0;
		/** When the movie player first reported loading finished (studio card only), else 0. */
		double LoadingDone = 0.0;
		double FadeOutStart = 0.0;
		double Down = 0.0;
		/** We ended it (the studio card's fade finished) rather than a key, a click or auto-complete. */
		bool bStoppedByUs = false;
		bool bWatchdog = false;
		/** Frames painted by the movie player's thread / by the game thread while it was up. */
		int32 LoadingThreadPaints = 0;
		int32 GameThreadPaints = 0;
		/** -TraceCardShot: where the studio card's window capture was written ("" when none). */
		FString ShotPath;
	};

	/** Every LoadMap this process saw, and whether it got a card, in order. */
	struct FLoadRecord
	{
		FString Map;
		double At = 0.0;
		bool bCard = false;
		FString WhyNot;
	};

	TRACE_API const TArray<FCardRecord>& DebugCards();
	TRACE_API const TArray<FLoadRecord>& DebugLoads();

	/** Why no studio card was shown at boot ("" when one was). */
	TRACE_API const FString& DebugStudioSkipReason();

	/** Harness seam: pretend a card of @p Kind came down at @p PlatformSeconds. Returns the old value. */
	TRACE_API double DebugSetLastCardEnd(double PlatformSeconds, ECard Kind);

	/** The caption rule, as a pure function: what a card for @p MapName would show given @p Pending. */
	TRACE_API FString DebugCaptionFor(const FString& MapName, const FString& Pending);
#endif
}
