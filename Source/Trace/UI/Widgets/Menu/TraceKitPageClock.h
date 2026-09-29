// Trace — what the kit's page clock drew (TraceMenuKit::DrawPageClock), for a harness to compare.
//
// Its own tiny header, as TraceKitMotion.h is, so a page (FTraceTeamSelect, FTraceLoadoutSelect,
// FTraceOptionsMenu, ATraceHUD's draw record) can HOLD one as a member without its header pulling in
// the whole kit renderer.

#pragma once

#include "CoreMinimal.h"

/**
 * ONE PAGE CLOCK AS THE KIT DREW IT: TIME and the whole seconds left in a value box. Team select and
 * the loadout page draw one in their headers, and the pause menu draws one over either page.
 *
 * Only DrawPageClock writes it (through its OutDrawn argument), and a page resets its copy before
 * drawing. So a page whose countdown took any other path has none: team select's used to be a line
 * of its own ("KEEPING YOUR TEAM IN n"), which the pause menu and the loadout page then swapped for
 * this box. Trace.Menu.Verify and Trace.UI.Fade.Verify hold two of these against each other.
 *
 * Positions are in 1080p px (screen px / UIScale), from the left and from the top of the view.
 */
struct FTraceKitPageClockDraw
{
	/** The whole seconds in the box, or -1 when no clock was drawn. */
	int32 ShownSeconds = -1;

	/** The left edge of the clock (TIME's, or the box's when TIME is blanked). */
	float LeftPx = 0.f;

	/** The line TIME and the box are centred on. The title's line, unless a page moved it. */
	float CapMidPx = 0.f;

	bool IsDrawn() const { return ShownSeconds >= 0; }
};
