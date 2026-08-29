// Trace — who owns the pointer on screen (spec v24 §2).
//
// =================================================================================================
// THE DEFECT
// =================================================================================================
//     "The new cursor ui is working, but my cursor shows on top of it"
//
// Every menu surface in this project draws the artist's arrow itself — T_MenuCursor, tip-anchored on
// the exact pixel the hit test uses (UI/Widgets/Menu/TraceTitleMenuWidget.h's MenuCursor,
// FTraceOptionsMenu::DrawCursor, FTraceCharacterSelect::DrawCursor). Every one of those surfaces
// also runs with APlayerController::bShowMouseCursor set to TRUE, because that is what keeps
// GetMousePosition answering and the GameAndUI input mode from capturing the mouse. So the operating
// system paints its own arrow on the same pixels, on top of ours, and the player sees two pointers.
//
// =================================================================================================
// WHY bShowMouseCursor IS NOT THE LEVER
// =================================================================================================
// The obvious fix — clear bShowMouseCursor while a menu is up — changes far more than the picture.
// APlayerController::ShouldShowMouseCursor() feeds the input mode and the viewport's capture and
// lock behaviour, and ATraceMenuPlayerController::BeginPlay has a long, hard-won comment about
// exactly that machinery being the source of a phantom PLAY activation. Touching it to hide a
// picture would put that back at risk.
//
// The lever that is ONLY the picture is APlayerController::CurrentMouseCursor. The engine reads it
// like this (Engine/Private/PlayerController.cpp):
//
//     EMouseCursor::Type APlayerController::GetMouseCursor() const
//     {
//         if (ShouldShowMouseCursor()) { return CurrentMouseCursor; }
//         return EMouseCursor::None;
//     }
//
// so setting CurrentMouseCursor to EMouseCursor::None hides the hardware arrow and changes nothing
// else: the mouse still moves, still reports its position, still clicks, and the input mode is
// untouched.
//
// =================================================================================================
// THE ALT-TAB RULE, AND WHY IT IS THE ENGINE'S GUARANTEE RATHER THAN THIS FILE'S PROMISE
// =================================================================================================
// Spec §2: "Do not leave the player with no cursor at all if they alt-tab or the game loses focus."
// That is not a case this file has to remember to handle, because the query path already refuses to
// honour a game-side cursor choice when the game is not the active window
// (Engine/Private/GameViewportClient.cpp):
//
//     EMouseCursor::Type UGameViewportClient::GetCursor(FViewport* InViewport, int32 X, int32 Y)
//     {
//         if (!FSlateApplication::Get().IsActive())                                 return EMouseCursor::Default;
//         else if (!InViewport->HasMouseCapture() && !InViewport->HasFocus())       return EMouseCursor::Default;
//         else if (ViewportConsole && ViewportConsole->ConsoleActive())             return EMouseCursor::Default;
//         else if (GameInstance && GameInstance->GetFirstLocalPlayerController())
//             return GameInstance->GetFirstLocalPlayerController()->GetMouseCursor();
//         ...
//
// Three separate guards, each of which hands the player an ordinary arrow back before this file's
// value is ever consulted. Suppression is therefore *inert* while the window is in the background
// and comes back the frame focus returns, with no state to get stuck in. Trace.UI.CursorReport
// prints all three inputs and the value the platform is actually being handed, so this is a
// measurement rather than a claim.
//
// =================================================================================================
// A LEASE, NOT A MODE
// =================================================================================================
// The second failure this has to be immune to is the one the spec calls worse than the bug: a
// surface that hides the pointer and then stops drawing its own. A travel starts, a modal with no
// pointer of its own opens (the JOIN prompt is exactly that — it is keyboard-only and draws no
// arrow), the map changes, the HUD is destroyed. Any of those, with a set-once flag, strands the
// player with nothing to point with.
//
// So suppression is a LEASE THAT EXPIRES. Whoever is drawing Trace's own pointer renews it every
// frame; two frames without a renewal and the hardware arrow comes back on its own. Nothing has to
// remember to release it, which means no exit path can forget to.
//
// =================================================================================================
// WHO RENEWS IT
// =================================================================================================
//   * UTraceTitleMenuWidget::ApplyView          — the title screen, renewed on the frames its own
//                                                 MenuCursor image is visible. That is a fact about
//                                                 the view it was handed, so the widget stays the
//                                                 pure view its header says it is.
//   * FTraceCharacterSelect::Tick               — the in-match character select, and (via the
//                                                 bInputAllowed flag its caller already passes) the
//                                                 pause / settings overlay in front of it.
//   * PollMenuSurfaces(), below                 — the title screen's SETTINGS modal, which lives in
//                                                 UI/TraceOptionsMenu.cpp. That file belongs to
//                                                 another slice this pass, so the poll asks
//                                                 ATraceMenuHUD::IsOptionsOpen() — already public —
//                                                 instead of editing it. See the note there.

// =================================================================================================
// AND SINCE THE UI QA PASS: WHAT THE POINTER LOOKS LIKE, ONCE
// =================================================================================================
// This file already decided WHO owns the pointer. It now also decides WHAT IT IS, because the QA
// pass found the answer had forked:
//
//     "the hardware cursor is unthemed AND the two title renderers now draw DIFFERENT pointers
//      (white blade vs cyan cross)"
//
// The UMG title screen drew the artist's blade (T_MenuCursor). The CANVAS title screen — the same
// screen, one CVar away — drew a nine-pixel cyan gap-cross left over from before the sprite existed.
// The settings overlay and character select drew the blade again, each from its own copy of the
// sprite's proportions, one of which disagreed with the other in the fourth decimal place. Four
// surfaces, three pictures, one pointer.
//
// So the pointer is now ONE function (DrawPointer) and ONE colour (PointerTint), and every Canvas
// surface calls them. The UMG title screen cannot call an AHUD draw, so it takes the tint and the
// geometry constants and applies them to its own UImage — which is the same three numbers reaching
// the same sprite by the only route a widget has.
//
// WHY THE POINTER IS TINTED AT ALL, since the sprite was perfectly visible white. The title palette
// states the rule this was breaking (UI/Widgets/Menu/TraceMenuPalette.h): "the whole screen is two
// hues — a cyan that carries the interface and an amber that only ever marks danger or the Hard
// setting — over near-black ... the moment a third colour appears, the grid stops reading as a grid
// and starts reading as noise." A pure-white pointer WAS that third colour, on every screen, at all
// times. Cyan is not an invention here either: it is the colour the Canvas title screen was already
// drawing its pointer in, so unifying on it keeps one of the two arms exactly as it shipped.
//
// The sprite survives the tint intact because of what is actually in it: 835 of its 1,382 opaque
// pixels are pure white and the rim is sRGB(71,54,4), a near-black olive. A Canvas tint multiplies,
// so the body becomes the tint and the rim stays a dark keyline — which is the thing that keeps the
// pointer readable over the arena's white walkways on the select screen.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/ICursor.h"

class AHUD;
class APlayerController;

namespace TraceHardwareCursor
{
	/**
	 * Start the expiry ticker if it is not already running. Idempotent and free after the first call.
	 *
	 * Call it unconditionally from any surface that might need the arbiter, INCLUDING on the frames
	 * that surface is not renewing anything. That is not a formality: the ticker is what releases an
	 * expired lease and what covers the one surface that cannot renew its own (see PollMenuSurfaces
	 * in the .cpp), so it has to be running on a screen whose own pointer never appears.
	 */
	TRACE_API void EnsureRunning();

	/**
	 * Renew the lease for this frame: "Trace is drawing its own pointer right now, hide the OS one."
	 *
	 * Idempotent, cheap, and safe to call every frame from more than one surface.
	 *
	 * @param InPC     the local controller whose cursor is being suppressed. Null falls back to the
	 *                 engine's own first local controller rather than skipping the renewal — a caller
	 *                 that has momentarily lost its controller pointer has not stopped drawing.
	 * @param InOwner  a short static string naming the caller, for Trace.UI.CursorReport. Not copied
	 *                 per frame — pass a literal.
	 */
	TRACE_API void RenewSuppression(APlayerController* InPC, const TCHAR* InOwner);

	/**
	 * Hand the pointer straight back, without waiting for the lease to run out.
	 *
	 * The lease expiring already covers every exit path, so this is only for the one case where two
	 * frames of latency would themselves be the bug: a surface that KNOWS it has just stopped drawing
	 * a pointer while the player is still on a mouse-driven screen.
	 */
	TRACE_API void ReleaseNow();

	/**
	 * One line of truth for the log and the console command: who holds the lease, what this file set,
	 * and — the part that actually decides what the player sees — what the engine is handing the
	 * platform for the viewport under the pointer.
	 */
	TRACE_API FString Describe();

	/**
	 * THE RED ARM. Non-zero restores the shipped-today behaviour: the lease is never taken and the OS
	 * arrow is drawn over the artist's, so one binary produces both arms of the fix.
	 *
	 * Console: `Trace.UI.HardwareCursorRedArm 1`.
	 */
	TRACE_API bool IsRedArmed();

	/**
	 * THE ONE COLOUR TRACE'S POINTER IS DRAWN IN, on every surface and both renderers.
	 *
	 * A function rather than a constant so the UMG title screen and the three Canvas surfaces cannot
	 * end up holding two copies of it — which is exactly how they ended up holding two pictures.
	 */
	TRACE_API FLinearColor PointerTint();

	/**
	 * The pointer's height in SCREEN pixels for a surface drawing at @p InUIScale.
	 *
	 * 30 reference pixels, which is what the settings overlay and character select already used; the
	 * UMG title screen sizes its own image from this so the two renderers agree by construction
	 * rather than by two people having typed the same number.
	 */
	TRACE_API float PointerHeight(float InUIScale);

	/**
	 * Draw Trace's pointer through @p HUD with its POINT on @p TipPos.
	 *
	 * @return false when the sprite is not available (a build with no menu art, or the frame or two
	 *         before its RHI texture lands). The caller then draws its own fallback — every one of
	 *         them keeps the vector cross it drew before the sprite existed.
	 *
	 * Tip-anchored, not centre-anchored. Every surface hit-tests at the pointer position, so an arrow
	 * anchored anywhere but its point draws about eleven pixels from the pixel it is about to click
	 * and every click in every screenshot looks like it landed on the wrong row.
	 */
	TRACE_API bool DrawPointer(AHUD* HUD, const FVector2D& TipPos, float InUIScale);
}
