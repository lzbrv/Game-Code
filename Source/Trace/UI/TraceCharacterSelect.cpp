// Trace — the character select screen. See TraceCharacterSelect.h.

#include "UI/TraceCharacterSelect.h"

#include "CanvasItem.h"                 // FCanvasTextItem — the only way to typeset with a real face
#include "Engine/Canvas.h"              // UCanvas::Canvas (the FCanvas) for the DPI scale
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"            // FTextureResource::TextureRHI — see the portrait loader
#include "Engine/World.h"
#include "EngineFontServices.h"         // FEngineFontServices — measuring a Slate face from Engine
#include "Fonts/FontMeasure.h"          // FSlateFontMeasure
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"           // FPlatformTime::Cycles64 — the click harness's injector
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"          // FInputKeyEventArgs — same injector
#include "Misc/CoreMiscDefines.h"       // FInputDeviceId

#include "Core/TracePlayerState.h"
#include "Trace.h"                      // LogTraceGame
#include "TraceTypes.h"                 // TraceTeamColor / TraceTeamName
#include "UI/TraceHardwareCursor.h"     // spec v24 §2 — one pointer on screen, not two
#include "UI/Text/TraceCanvasText.h"       // spec v22 §A1 — the cards type from the glyph atlas
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"   // the artist's sprites, colours and 9-slice numbers

#if !UE_BUILD_SHIPPING
int32 GTraceCharacterSelectDebugPick = 0;
int32 GTraceCharacterSelectClickTest = 0;

// NAMED, not anonymous: UBT compiles this module as a unity/jumbo build, so two files that each
// define something at the top of an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
namespace TraceCharSelectHighlightLatch
{
	/**
	 * Trace.Characters.Highlight's latch — 1..N, or 0 for "the player is driving". File-scope rather
	 * than a member for the same reason GTraceCharacterSelectDebugPick is, and file-scope rather than
	 * exported because both ends of it — the console command and the Tick that reads it — are in this
	 * translation unit and nothing outside has any business setting it.
	 *
	 * *** WHY A LATCH AND NOT A ONE-SHOT LIKE Trace.Characters.Select. ***
	 * PollInput's mouse branch writes `Highlighted = Index` on EVERY tick the cursor rests inside a
	 * card rectangle, and an unattended -game run has a cursor sitting wherever the platform left it.
	 * A one-shot write would therefore survive exactly zero frames on any run whose cursor happens to
	 * land on the grid — which is most of them; W8-VISUAL's own 16:10 select frames show the pointer
	 * parked on card 2 and CHUT highlighted for the whole run. Held, the override is re-applied after
	 * PollInput every tick and the panel stays on the character that was asked for.
	 *
	 * *** WHY THIS EXISTS AT ALL. *** W5-UIQA §7.2 wrote down that the detail panel's crop is a fixed
	 * rectangle while the busts differ, and closed with "I could only exercise one character … that is
	 * the check to run". Nobody ran it for four days, and the reason is that there was no way to: the
	 * only two ways into the detail panel are hovering a card, which an unattended run cannot aim, and
	 * Trace.Characters.Select, which CONFIRMS and closes the screen on the same tick it highlights. So
	 * the check that was left open was not left open out of negligence — the instrument was missing.
	 * This is the instrument. Ten frames, one per character, at
	 *
	 *     -TraceExec=Trace.Characters.Highlight 9
	 *
	 * is now a thing a script can do, which is what makes "all ten crops are clean" a measurement
	 * rather than a claim about one of them.
	 */
	int32 Held = 0;
}

namespace TraceCharSelectClickTest
{
	/** Frames between stages. Enough that a Tick, and therefore a Draw and a PollInput, lands between. */
	constexpr uint64 FramesPerStage = 6;

	/** Complete down/up pairs a card is given before the test is called a failure. */
	constexpr int32 MaxPairs = 3;

	/** One key edge through the same entry point a physical mouse reaches. */
	void InjectKey(APlayerController* PC, const FKey& Key, bool bPressed)
	{
		if (PC == nullptr)
		{
			return;
		}

		// Internal id 0 rather than IPlatformInputDeviceMapper::GetDefaultInputDevice(): that lives in
		// the ApplicationCore module, and desktop maps keyboard and mouse to id 0. Same call and same
		// reasoning as ATraceMenuHUD's injector.
		const FInputKeyEventArgs Args(
			/*Viewport*/ nullptr,
			FInputDeviceId::CreateFromInternalId(0),
			Key,
			bPressed ? IE_Pressed : IE_Released,
			/*AmountDepressed*/ bPressed ? 1.f : 0.f,
			/*bIsTouchEvent*/ false,
			FPlatformTime::Cycles64());

		PC->InputKey(Args);
	}
}
#endif

namespace TraceSelectStyle
{
	// The same neon instrument-panel palette the title screen and the options overlay use, so this
	// screen reads as another page of one machine rather than as a dialog dropped on top of it.
	static const FLinearColor Cyan   (0.16f, 0.88f, 1.00f, 1.00f);
	static const FLinearColor Ink    (0.94f, 0.97f, 1.00f, 1.00f);
	static const FLinearColor Danger (0.95f, 0.28f, 0.22f, 1.00f);
	static const FLinearColor Good   (0.24f, 0.90f, 0.42f, 1.00f);

	// ---------------------------------------------------------------------------------------------
	// SPEC v20 §6.2 — THE TYPE RAMP HAS THREE STEPS OF COLOUR, NOT ONE.
	//
	// The measured complaint was "a wall of tiny all-caps body text with almost no hierarchy". Size
	// alone does not fix that: MOVEMENT / PASSIVE / ACTIVATED were already smaller than the names and
	// still read as one grey mass. So the ramp below is used strictly — Ink for what you read, InkSoft
	// for supporting prose, InkDim for micro-labels and hints — and InkDim was LIFTED from
	// (0.42,0.58,0.66) because at the size it is used it was the hardest thing on the screen to read.
	// ---------------------------------------------------------------------------------------------
	static const FLinearColor InkSoft(0.76f, 0.84f, 0.90f, 1.00f);
	static const FLinearColor InkDim (0.56f, 0.66f, 0.75f, 1.00f);

	/** The artist's plate navy, RGB(29,41,81), taken from the sheet rather than invented. */
	static const FLinearColor Plate = TraceMenuArtStyle::PlateFill;

	/** The artist's amber, RGB(116,58,0). The hover plate carries it; this is for rules and ticks. */
	static const FLinearColor Amber = TraceMenuArtStyle::Amber;

	/**
	 * THE BACKDROP, AND IT IS OPAQUE — SPEC v20 §6.3.
	 *
	 * *** THIS IS A DELIBERATE REVERSAL OF WHAT THIS FILE USED TO ARGUE. *** The old comment here read
	 * "not opaque ... the arena stays faintly visible behind the cards, which is the difference between
	 * 'the match is loading' and 'the match is running and waiting for you'", and it shipped at alpha
	 * 0.94. The measured capture (TraceAutoShot_Match_20260812_235325_01.png) shows what 6% of
	 * transparency actually buys: the ammo counter, the health bar, the dash and weapon meters, the
	 * scoreboard, the "HOSTING" panel and the crosshair are ALL legible through it and all compete with
	 * the cards. The arena itself is barely there. The 6% was paying for the HUD, not for the arena.
	 *
	 * ATraceHUD::DrawHUD draws every gameplay pass BEFORE it ticks this screen (TraceHUD.cpp, the
	 * CharacterSelect.Tick call is the second-to-last thing in the function) and hides the UMG corner
	 * outright while this screen is open, so ONE opaque rectangle here removes all of it. That is the
	 * whole fix for §6.3 and it is entirely inside this file.
	 *
	 * WHAT IT COSTS, STATED PLAINLY: the arena is no longer visible behind the pick screen. The
	 * cheaper-looking alternative — keep the scrim translucent and skip the gameplay passes while
	 * CharacterSelect.IsOpen() — lives in ATraceHUD::DrawHUD, which this pass does not own. It is in
	 * the report.
	 */
	static const FLinearColor Backdrop(0.0055f, 0.0090f, 0.0190f, 1.00f);

	/** Large surfaces (the detail panel) are the artist's navy, dropped so type sits comfortably on it. */
	static const FLinearColor PanelFill(Plate.R * 0.62f, Plate.G * 0.62f, Plate.B * 0.62f, 0.94f);

	static FLinearColor WithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, A);
	}

	/**
	 * Scales RGB and leaves alpha alone.
	 *
	 * House rule, and it has cost this project time before: AHUD::DrawLine discards alpha entirely
	 * (FCanvasItem defaults to Opaque), so anything that has to be dimmed and might end up on a line
	 * has to be dimmed in RGB. Used for every "taken" card on this screen.
	 */
	static FLinearColor Dimmed(const FLinearColor& C, float Mul)
	{
		return FLinearColor(C.R * Mul, C.G * Mul, C.B * Mul, C.A);
	}

	/** Everything below is authored against a 1080p-tall viewport, exactly like the HUD. */
	static constexpr float ReferenceHeight = 1080.f;

	/** How long a server verdict stays on screen. Long enough to read at a glance, short enough to go. */
	static constexpr float MessageDuration = 3.5f;
}

/**
 * THE LAYOUT, IN ONE PLACE — every number is a 1080p design pixel and is multiplied by UIScale.
 *
 * Spec v20 §6.1's complaint was "huge, uneven dead space: every card is a fixed-height box, but
 * content length varies enormously". The fix is structural rather than arithmetic: the cards no longer
 * carry the prose at all. Ten IDENTITY TILES carry name, key, accent and ability name — facts that are
 * the same size for every character — and ONE detail panel underneath carries the full movement /
 * passive / activated text for whichever tile is highlighted. A tile can then be a fixed box honestly,
 * because what is in it really is a fixed amount, and Rocco's card cannot be half empty while
 * Mortimer's overflows.
 *
 * The vertical budget adds up to 1080:
 *   34 top margin | 34..104 title, team chip and countdown | 106..143 rule, countdown bar, the screen's
 *   rule line | 156..566 the grid (two rows of 196) | 592..928 the detail panel | 958..1020 the footer
 *   controls and the server's verdict | the rest is bottom margin.
 */
namespace TraceSelectLayout
{
	constexpr float Margin      = 54.f;

	constexpr float HeaderTop   = 34.f;
	constexpr float TitleSize   = 42.f;
	constexpr float TitleTrack  = 7.0f;
	constexpr float RuleY       = 106.f;
	constexpr float BarY        = 112.f;
	constexpr float BarH        = 5.f;
	constexpr float SubY        = 128.f;

	constexpr float GridTop     = 156.f;
	constexpr float TileH       = 196.f;
	constexpr float TileGapX    = 20.f;
	constexpr float TileGapY    = 18.f;

	// DetailH IS MEASURED AGAINST THE LONGEST CHARACTER, not picked to look balanced. At the narrowest
	// viewport this ships on (4:3 at 1080, where the columns are only ~284 design px wide) Lily's ZIP
	// paragraph — the longest string in the roster — wraps to six lines, which with the ability row and
	// the heading needs 288. 336 clears it with room, and anything longer than Lily is caught by the
	// fit pass in the panel rather than running off the bottom. The 404 this was first built at simply
	// reserved space nothing could ever use, which is §6.1's own complaint at panel scale.
	constexpr float DetailTop   = 592.f;
	constexpr float DetailH     = 336.f;
	constexpr float DetailPad   = 30.f;
	constexpr float IdentityW   = 340.f;
	constexpr float ColumnGap   = 26.f;

	constexpr float FooterY     = 958.f;

	/** Type sizes. One ramp, used everywhere; nothing on this screen picks a size off the cuff. */
	constexpr float SizeDisplay = 46.f;   // the highlighted character's name in the detail panel
	constexpr float SizeName    = 25.f;   // a tile's character name
	constexpr float SizeLead    = 20.f;   // the activated ability's name
	constexpr float SizeBody    = 17.f;   // every paragraph
	constexpr float SizeLabel   = 13.f;   // MOVEMENT / PASSIVE / ACTIVATED and other micro-labels
	constexpr float SizeChip    = 16.f;   // the text inside a key chip

	/** Tracking (extra letter spacing) for the all-caps display and label sizes only. */
	constexpr float TrackLabel  = 2.6f;
	constexpr float TrackName   = 2.2f;
}

/**
 * THE CARD GRID — spec v18 §2, because eight cards do not fit the row that five did.
 *
 * Everything here is compile-time, derived from TraceCharacterRoster::Count and nothing else, which
 * is what lets PollInput (which runs BEFORE the first Draw of a frame) and Draw agree about where the
 * rows are without one of them caching the other's arithmetic.
 *
 * WHY IT WRAPS RATHER THAN JUST GETTING NARROWER. It used to be about wrapped prose: five cards
 * across left 219 px of text width and eight across would have left 127. Spec v20 §6 moved the prose
 * off the cards entirely, so the argument is now about IDENTITY — a tile has to be wide enough to set
 * a name like SLIMEBALL at 25 px beside a key chip, which is ~250 px at the narrowest viewport this
 * ships on (4:3 at 1080). Five across clears that; eight would not.
 *
 * *** THESE TWO NUMBERS ARE ALSO AN INPUT CONTRACT, WHICH IS WHY THE v20 REDESIGN DID NOT TOUCH THEM.
 * PollInput steps the highlight by +/-Columns for up and down (see the NavDir block), so Columns is
 * not a drawing detail — it is what makes "the card below this one" mean the card that is visually
 * below it. Changing the grid shape without changing that arithmetic silently breaks the arrow keys,
 * and spec v20 §6 puts the input model out of scope.
 *
 * A ROSTER OF FIVE OR FEWER STILL DRAWS AS ONE ROW: Rows collapses to 1 and every number below
 * reduces to the old expression.
 */
namespace TraceSelectGrid
{
	/** Cards per row before the screen wraps. Five is what the pre-v18 layout was authored against. */
	constexpr int32 MaxPerRow = 5;

	/** 1 up to five characters, 2 beyond. A third row would need the header to shrink; say so then. */
	constexpr int32 Rows = (TraceCharacterRoster::Count <= MaxPerRow) ? 1 : 2;

	/** Rounded UP, so with 9 characters the last row is the short one and gets centred. */
	constexpr int32 Columns = (TraceCharacterRoster::Count + Rows - 1) / Rows;

	static_assert(Rows * Columns >= TraceCharacterRoster::Count,
		"The card grid must have room for every character, or the last ones would never be drawn - "
		"and an undrawn card cannot be clicked, because the hit test reads the rects the draw left.");
}

// =============================================================================================
// THE ARTIST'S SPRITES ON CANVAS — spec v20 §6.7 ("nothing uses the artist's art")
// =============================================================================================
//
// NAMED after the file, like every other namespace here, for the unity-build reason at the top.
//
// TWO THINGS MAKE THIS SAFE TO PUT IN A SCREEN THAT OPENS INSIDE A LIVE MATCH:
//
// 1. EVERY CALL SITE FALLS BACK. Sprite() returns null when a texture is missing, and every caller
//    below is written as "if the sprite is there use it, otherwise draw the rectangle this screen
//    drew before". A build where the art fails to cook loses the artist's plates and keeps a working,
//    readable pick screen — it can never lose the element altogether, which is what a Slate brush
//    would do (it would draw a white box instead).
//
// 2. THE CACHE IS WEAK. FTraceCharacterSelect is deliberately not a UObject and its header states as
//    an invariant that it "holds no UObject references that outlive a frame". Nothing else on the
//    Arena map references these menu textures, so a raw UTexture2D* here would be collected out from
//    under a long match and then drawn through. A TWeakObjectPtr that re-resolves when it goes stale
//    costs a FindObject on an already-loaded package, which is nothing.
//
// CANVAS HAS NO 9-SLICE. AHUD::DrawTexture builds one FCanvasTileItem and stretches the whole bitmap,
// so the artist's corner radius would come out oval on anything that is not the sheet's aspect ratio.
// DrawPlate below hand-rolls the 9-slice out of nine DrawTexture calls with UV sub-rects, using the
// SAME measurements the UMG title screen uses (TraceMenuArtStyle::FSpriteFrame) rather than new ones.
namespace TraceCharacterSelectArt
{
	// The POINTER is not in this list any more. It used to be, and this file drew it itself; since the
	// UI QA pass every surface draws it through TraceHardwareCursor::DrawPointer instead, which owns
	// the one sprite, the one size and the one tint. See FTraceCharacterSelect::DrawCursor.
	enum class ESprite : uint8
	{
		PlateDefault = 0,
		PlateHover,
		PlateDisabled,
		ValueBox,
		Count
	};

	static const TCHAR* PathFor(ESprite Which)
	{
		switch (Which)
		{
		case ESprite::PlateDefault:  return TraceMenuArtStyle::BtnDefault;
		case ESprite::PlateHover:    return TraceMenuArtStyle::BtnHover;
		case ESprite::PlateDisabled: return TraceMenuArtStyle::BtnDisabled;
		case ESprite::ValueBox:      return TraceMenuArtStyle::ValueBox;
		default:                     return nullptr;
		}
	}

	static constexpr int32 SpriteCount = static_cast<int32>(ESprite::Count);

	static TWeakObjectPtr<UTexture2D> Cache[SpriteCount];

	/** Set once when a path fails, so a broken install does not attempt a package load every frame. */
	static bool bFailed[SpriteCount] = {};

	static bool bLoggedInventory = false;

	UTexture2D* Sprite(ESprite Which)
	{
		const int32 Index = static_cast<int32>(Which);
		if (Index < 0 || Index >= SpriteCount || bFailed[Index])
		{
			return nullptr;
		}

		if (UTexture2D* Live = Cache[Index].Get())
		{
			return Live;
		}

		UTexture2D* Loaded = LoadObject<UTexture2D>(nullptr, PathFor(Which));
		if (Loaded == nullptr)
		{
			bFailed[Index] = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CharSelect] %s did not load. That element falls back to the plain rectangle it "
				     "used to be; the screen is still readable."), PathFor(Which));
			return nullptr;
		}

		Cache[Index] = Loaded;
		return Loaded;
	}

	/** One line, once per process, so a capture can be told apart from a build with no art in it. */
	void LogInventoryOnce()
	{
		if (bLoggedInventory)
		{
			return;
		}
		bLoggedInventory = true;

		int32 Resolved = 0;
		for (int32 Index = 0; Index < SpriteCount; ++Index)
		{
			Resolved += (Sprite(static_cast<ESprite>(Index)) != nullptr) ? 1 : 0;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect] Art: %d/%d of the artist's sprites resolved (button plate x3, value chip). "
			     "Anything missing falls back to a drawn rectangle. The pointer is not counted here - "
			     "TraceHardwareCursor owns it and logs its own line."), Resolved, SpriteCount);
	}

	/**
	 * The artist's plate, 9-sliced onto the rect (@p X, @p Y, @p W, @p H).
	 *
	 * @param CornerHeight  the PLATE HEIGHT the corner should be sized as if it were, in screen px.
	 *
	 * That last parameter is the whole reason this is not three lines. The sheet's button is 4723 x
	 * 1230 with a 428-pixel corner, i.e. a corner 35% of the plate's height — which reads as a modest
	 * rounding on a very wide, short menu row and as a lozenge on anything squarer. A card tile here is
	 * 346 x 196, so taking the corner from the tile's own height would round it by 68 px and the
	 * artist's shape language would come out as a pill. A 9-slice explicitly lets the author choose the
	 * corner size independently of the stretched middle (Slate does the same thing through a Box
	 * brush's ImageSize), so callers pass the height of a MENU ROW and the corner comes out the size
	 * the artist drew it, whatever shape the thing being framed is.
	 *
	 * The glow margin is scaled by the same corner scale and drawn OUTSIDE the rect: the hover plate's
	 * amber ring lives in it, and forgetting it is what makes a 9-sliced plate come out too small.
	 */
	void DrawPlate(AHUD* HUD, UTexture2D* Texture, const TraceMenuArtStyle::FSpriteFrame& Frame,
		float X, float Y, float W, float H, float CornerHeight, const FLinearColor& Tint)
	{
		if (HUD == nullptr || Texture == nullptr || W <= 1.f || H <= 1.f || Frame.PlateH <= 0.f)
		{
			return;
		}

		const float CornerScale = FMath::Max(CornerHeight, 1.f) / Frame.PlateH;
		const float Inset = Frame.Glow * CornerScale;

		const float SX = X - Inset;
		const float SY = Y - Inset;
		const float SW = W + Inset * 2.f;
		const float SH = H + Inset * 2.f;

		// Half the sprite, minus a pixel, is the hard ceiling: two corners that met in the middle would
		// draw the flat centre at a negative width and flip the quad.
		const float Cap = FMath::Clamp(Frame.Cap * CornerScale, 1.f, FMath::Min(SW, SH) * 0.5f - 1.f);

		const float UCap = Frame.Cap / Frame.SpriteW();
		const float VCap = Frame.Cap / Frame.SpriteH();

		const float Xs[3] = { SX, SX + Cap, SX + SW - Cap };
		const float Ws[3] = { Cap, SW - Cap * 2.f, Cap };
		const float Us[3] = { 0.f, UCap, 1.f - UCap };
		const float UWs[3] = { UCap, 1.f - UCap * 2.f, UCap };

		const float Ys[3] = { SY, SY + Cap, SY + SH - Cap };
		const float Hs[3] = { Cap, SH - Cap * 2.f, Cap };
		const float Vs[3] = { 0.f, VCap, 1.f - VCap };
		const float VHs[3] = { VCap, 1.f - VCap * 2.f, VCap };

		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Column = 0; Column < 3; ++Column)
			{
				if (Ws[Column] <= 0.f || Hs[Row] <= 0.f)
				{
					continue;
				}
				HUD->DrawTexture(Texture, Xs[Column], Ys[Row], Ws[Column], Hs[Row],
					Us[Column], Vs[Row], UWs[Column], VHs[Row], Tint);
			}
		}
	}
}

// =============================================================================================
// THE TEN PORTRAITS — UI plan WP10
// =============================================================================================
//
// W4-PORTRAITS rendered, composed and imported ten 512-square busts and nobody consumed them: this
// screen was the only place they were ever meant to appear, and until this pass `T_Portrait` did not
// occur anywhere in Source/. That is what made character select the emptiest screen in the build —
// ten cards, each about seventy per cent bare navy behind one faded watermark letter.
//
// ---------------------------------------------------------------------------------------------
// THE PATH IS DERIVED FROM THE ROSTER, NOT TABULATED
// ---------------------------------------------------------------------------------------------
// One line per character in a table here would be an eleventh place a new character has to be
// registered, and TraceCharacterRoster.h's whole argument is that there is exactly one. The shoot
// names its files after the roster name (`compose_portraits.py` reads the same table), so the path
// is that name title-cased: ROCCO -> T_Portrait_Rocco, X -> T_Portrait_X, SLIMEBALL ->
// T_Portrait_Slimeball. An eleventh character that has been through the portrait stage is picked up
// with no edit here; one that has not falls back to the monogram, which is the point of the fallback.
//
// ---------------------------------------------------------------------------------------------
// A LOADED TEXTURE IS NOT A DRAWABLE ONE — AND THIS SCREEN IS THE WORST CASE FOR THAT
// ---------------------------------------------------------------------------------------------
// UI/TraceOptionsMenu.cpp documents the crash in full: AHUD::DrawTexture hands
// `Texture->GetResource()` straight to an FCanvasTileItem and checks only the UTexture, so a texture
// that is loaded but whose FTextureResource::TextureRHI has not arrived yet becomes a batched
// element the render thread dies on (SIGSEGV in FBatchedElements::Draw). That page found it because
// it was the first thing in the process to touch /Game/Trace/UI/Art.
//
// This screen is a stronger version of the same case. It opens during warm-up INSIDE A LIVE MATCH,
// and /Game/Trace/UI/Art/Portraits is a package nothing else in a match references at all — no
// cook-time reference from the map, no menu having warmed it, because the menu map was unloaded two
// travels ago. So the portraits are guaranteed to be a cold synchronous load whose RHI textures land
// one or two frames after LoadObject returns. Every draw below therefore goes through Drawable(),
// and the frames before the resource exists draw the monogram — which is the same fallback a missing
// file takes, so there is one path to test rather than two.
//
// ---------------------------------------------------------------------------------------------
// AT MOST TWO NEW LOADS PER FRAME
// ---------------------------------------------------------------------------------------------
// Ten synchronous package loads on one frame is ~2.3 MB of BC7 read on the game thread, on the frame
// the select screen opens — which is already the frame that opens the screen. Spreading them over
// five frames costs 80 ms of monogram on a screen that is up for thirty seconds and nobody will see
// it; a hitch on the frame a modal appears is the thing players do notice.
namespace TraceCharacterSelectPortraits
{
	/** `/Game/Trace/UI/Art/Portraits/T_Portrait_<Name>` — W4-PORTRAITS' shipped location. */
	static const TCHAR* const PathPrefix = TEXT("/Game/Trace/UI/Art/Portraits/T_Portrait_");

	/**
	 * THE DETAIL-PANEL CROP, CORRECTED AGAINST THE SHIPPED FRAMING.
	 *
	 * UI plan WP10 §3 asked for a 2x crop at UV (0.25, 0.05)-(0.75, 0.55). Those numbers were written
	 * before anyone had seen a portrait. W4-PORTRAITS §4.1 measured the shipped set and the plan's
	 * window CUTS THE CHIN OFF every bust — the chin sits at v 0.614 and the clavicle at v 0.715, so
	 * a window ending at 0.55 stops above the mouth:
	 *
	 *     Lily fin tips  v 0.012      Rocco crest  v 0.232      chin      v 0.614
	 *     X top bead     v 0.030      head top     v 0.354      clavicle  v 0.715
	 *
	 * (0.25, 0.20)-(0.75, 0.70) was the same width one row lower. *** IT DECAPITATED MORTIMER, AND
	 * THE TOP OF THE WINDOW IS NOW 0.16 BECAUSE OF HIM. *** W5-UIQA §7.2 wrote down that the crop is
	 * a fixed rectangle while the busts differ, and said the check to run was "all ten, not one";
	 * W8-VISUAL ran it four days later and found exactly one failure, still shipping.
	 *
	 * THE NUMBER IS DERIVED, NOT NUDGED, AND THE DERIVATION IS THE POINT. Both halves are exact:
	 *
	 *   * the rig shot all ten through ONE frozen camera and printed it next to every capture —
	 *     `[Portrait] proj <Name>: ... frameZ=[116.23,208.77] frameH=92.54uu`
	 *     (Saved/Logs/release/W4-PORTRAITS-final.log), so v = (208.77 - Z) / 92.54 for every bust;
	 *   * `compose_portraits.head_extent_uu()` reports, per character, the top of everything rigidly
	 *     bound to the `head` bone — the definition W4-PORTRAITS' own framing gate is measured
	 *     against, and the right one here too: geometry bound to the head bone moves with the head
	 *     and reads as head in a silhouette, so a window that cuts it has cut the head.
	 *
	 * Run over the ten, that says every crown that MUST be inside the window is at v >= 0.181:
	 *
	 *     Mortimer  sig_lintel   Z 192.0  ->  v 0.181     <-- the binding one, and the only failure
	 *     Rocco     sig_crest    Z 187.3  ->  v 0.232
	 *     the other eight        Z 176.0  ->  v 0.354
	 *
	 * 0.16 clears the binding case by 0.021 (10.8 px on the 512 source). The window keeps its 0.50
	 * span and its u pair, so this is the SAME 2x square one row higher, not a different crop — and
	 * the bottom edge at 0.66 still clears every head core's floor, the tightest being Oyster's
	 * helmet at Z 150 -> v 0.635, by 0.025 (12.8 px).
	 *
	 * The two tallest CROWNS still run off the top, deliberately: X's masts (Z 206 -> v 0.030) and
	 * Lily's fins (Z 207.7 -> v 0.012) are not bound to the head bone, a window that held them would
	 * have to be 0.99 tall, and a detail crop that shows the whole figure is not a detail crop.
	 *
	 * Re-measure rather than trusting the table above:
	 *     python3 -c "import sys;sys.path.insert(0,'Scripts');import compose_portraits as c;\
	 *                 print({n:(208.77-c.head_extent_uu(n)['full'][1])/92.54 for n in \
	 *                 ['Rocco','Chut','Mace','Oyster','X','Roxie','Elle','Slimeball','Mortimer','Lily']})"
	 *
	 * Square, so the panel can size it from whichever of its two dimensions is the binding one.
	 */
	static constexpr float DetailMinU = 0.25f;
	static constexpr float DetailMaxU = 0.75f;

	/** The shipped top edge. See the derivation above; 0.20 is the value that cut Mortimer. */
	static constexpr float DetailTopV = 0.16f;

	/** Height AND width of the window. Square: the panel draws it into a square and never stretches. */
	static constexpr float DetailSpanV = DetailMaxU - DetailMinU;

#if !UE_BUILD_SHIPPING
	/**
	 * THE RED ARM FOR THE CROP, because a fix nobody can make fail is not evidence.
	 *
	 * `Trace.Characters.DetailCropTop 0.20` restores the window that shipped through W8 and puts the
	 * underside of Mortimer's lintel back on the panel, on the same binary, in the same run:
	 *
	 *   -TraceExec=Trace.Characters.Highlight 9 -TraceExec2=Trace.Characters.DetailCropTop 0.20
	 *
	 * Cheat-only and compiled out of shipping, so no ini and no player can move the shipped framing.
	 */
	static TAutoConsoleVariable<float> CVarDetailCropTop(
		TEXT("Trace.Characters.DetailCropTop"),
		DetailTopV,
		TEXT("Dev only. Top edge (v) of the detail panel's square portrait crop. Shipped value 0.16, "
		     "derived from Mortimer's head-bone crown at v 0.181 — see TraceCharacterSelect.cpp. Set "
		     "0.20 to restore the pre-W9 window and reproduce the decapitation."),
		ECVF_Cheat);
#endif

	/** The window's top edge, this frame. One place, so the log line and the draw cannot disagree. */
	static float DetailMinV()
	{
#if !UE_BUILD_SHIPPING
		return FMath::Clamp(CVarDetailCropTop.GetValueOnGameThread(), 0.f, 1.f - DetailSpanV);
#else
		return DetailTopV;
#endif
	}

	/** The bottom edge. Derived from the top so the window can never stop being square. */
	static float DetailMaxV()
	{
		return DetailMinV() + DetailSpanV;
	}

	/**
	 * THE CARD DRAWS THE WHOLE SQUARE, UNCROPPED, AND THAT IS A DECISION WITH TWO REASONS.
	 *
	 * 1. The composite has a 2 px accent frame and corner brackets BAKED INTO ITS EDGES
	 *    (`compose_portraits.py`). Any crop throws them away; drawing the full square keeps the
	 *    artist's frame and means this file must NOT draw a hairline of its own around a card
	 *    portrait — that would be a second frame on top of the one already there. The detail crop
	 *    below is the opposite case and does draw one, because the crop cuts the baked frame off.
	 * 2. A card exists to make a SILHOUETTE legible. W4-PORTRAITS' framing gate deliberately spent
	 *    headroom to keep Lily's fin tips (v 0.012) and X's top bead (v 0.030) inside the frame, and
	 *    those are two of the four strongest silhouettes in the roster. Cropping two per cent off the
	 *    top of the card to tidy the framing would spend the thing the gate bought.
	 */
	static constexpr float CardMinU = 0.f;
	static constexpr float CardMaxU = 1.f;
	static constexpr float CardMinV = 0.f;
	static constexpr float CardMaxV = 1.f;

	static TWeakObjectPtr<UTexture2D> Cache[TraceCharacterRoster::Count];

	/** Set once when a path fails, so a build with no portrait stage does not hunt for one every frame. */
	static bool bFailed[TraceCharacterRoster::Count] = {};

	/** LoadObject calls left this frame. Reset by BeginFrame(); see the header note above. */
	static int32 LoadBudget = 0;

	static bool bLoggedInventory = false;

	/** "ROCCO" -> "Rocco", "X" -> "X", "SLIMEBALL" -> "Slimeball". The shoot's own naming. */
	static FString AssetNameFor(const TCHAR* RosterName)
	{
		FString Out = FString(RosterName).ToLower();
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	/** See the header note: loaded is not drawable, and drawing a not-yet-drawable texture is a crash. */
	static bool Drawable(const UTexture2D* Texture)
	{
		if (Texture == nullptr)
		{
			return false;
		}
		const FTextureResource* Resource = Texture->GetResource();
		return Resource != nullptr && Resource->TextureRHI.IsValid();
	}

	/** Call once at the top of a Draw. */
	void BeginFrame()
	{
		LoadBudget = 2;
	}

	/**
	 * The bust for roster index @p Index, or null — which every caller treats as "draw the monogram".
	 *
	 * Null covers three different states on purpose and the caller does not need to tell them apart:
	 * the file is absent (a clone that has not run `import-characters.sh --stage portraits`), the file
	 * is present but its RHI texture has not landed yet, and this frame's load budget is spent.
	 */
	UTexture2D* For(int32 Index)
	{
		if (Index < 0 || Index >= TraceCharacterRoster::Count || bFailed[Index])
		{
			return nullptr;
		}

		if (UTexture2D* Live = Cache[Index].Get())
		{
			return Drawable(Live) ? Live : nullptr;
		}

		if (LoadBudget <= 0)
		{
			return nullptr;
		}
		--LoadBudget;

		const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Roster = TraceCharacterRoster::All();
		if (!Roster.IsValidIndex(Index))
		{
			bFailed[Index] = true;
			return nullptr;
		}

		const FString AssetName = AssetNameFor(Roster[Index].Name);
		const FString Path = FString::Printf(TEXT("%s%s.T_Portrait_%s"), PathPrefix, *AssetName, *AssetName);

		UTexture2D* Loaded = LoadObject<UTexture2D>(nullptr, *Path);
		if (Loaded == nullptr)
		{
			// ONCE PER CHARACTER, EVER — not once per frame. bFailed is what makes that true, and it
			// is the reason this is a Warning rather than a Verbose: a build with no portraits says so
			// ten times at the top of the log and then never again.
			bFailed[Index] = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CharSelect] No portrait at %s. %s's card and detail panel fall back to the "
				     "monogram watermark; nothing else changes. Run "
				     "Scripts/import-characters.sh --stage portraits to produce it."),
				*Path, Roster[Index].Name);
			return nullptr;
		}

		Cache[Index] = Loaded;
		return Drawable(Loaded) ? Loaded : nullptr;
	}

	/** One line, once per process, so a capture can be told apart from a build with no portraits in it. */
	void LogInventoryOnce()
	{
		if (bLoggedInventory)
		{
			return;
		}

		// Only once every portrait has been ASKED for at least once, which the load budget makes take
		// several frames. Logging on frame one would report "2/10 resolved" and mean nothing.
		int32 Resolved = 0;
		for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
		{
			if (Cache[Index].IsValid())
			{
				++Resolved;
			}
			else if (!bFailed[Index])
			{
				return;   // not asked yet — say nothing until the whole set has been tried
			}
		}

		bLoggedInventory = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect] Portraits: %d/%d resolved from %s*. Card draws the full 512 square "
			     "(the accent frame is baked into it); the detail panel draws UV (%.2f,%.2f)-(%.2f,%.2f). "
			     "Anything missing falls back to the monogram watermark."),
			Resolved, TraceCharacterRoster::Count, PathPrefix,
			DetailMinU, DetailMinV(), DetailMaxU, DetailMaxV());
	}

	/** One portrait, or a sub-rect of one. Aspect is the caller's problem; nothing here stretches. */
	void Draw(AHUD* HUD, UTexture2D* Texture, float X, float Y, float W, float H,
		float MinU, float MinV, float MaxU, float MaxV, const FLinearColor& Tint)
	{
		if (HUD == nullptr || Texture == nullptr || W <= 0.f || H <= 0.f)
		{
			return;
		}
		HUD->DrawTexture(Texture, X, Y, W, H, MinU, MinV, MaxU - MinU, MaxV - MinV, Tint);
	}
}

// =============================================================================================
// TYPE — spec v20 §6.2 and §6.7 ("engine font throughout")
// =============================================================================================
//
// *** WHY THIS EXISTS AT ALL, RATHER THAN JUST PASSING A BIGGER Scale TO AHUD::DrawText. ***
//
// GEngine->GetSmallFont() and friends are OFFLINE-cached bitmap fonts: one baked glyph page at one
// size. Scaling one up does not typeset it larger, it magnifies a bitmap, and that is exactly what the
// reference capture shows — every string on the screen at the same tiny size because the moment you
// ask for a bigger one it goes soft. A type hierarchy is not reachable from there.
//
// The menu typeface is a RUNTIME-cached face, so Slate rasterises it at whatever point size it is
// asked for. AHUD::DrawText cannot ask for one (it takes a UFont and a scale factor), but the item it
// builds underneath can: FCanvasTextItem has a constructor that takes an FSlateFontInfo, and
// FEngineFontServices measures the same face without needing FSlateApplication. So this screen
// typesets through those two directly.
//
// *** THE FACE COMES FROM TraceMenuArtStyle::MenuFont AND NOWHERE ELSE. *** Spec v20 §1 requires the
// substitution to be one constant; this file names no font asset, no typeface and no path. Whatever
// that one place resolves to is what this screen draws in.
//
// *** THE PARAMETER CONVENTION, BECAUSE THE HEADER'S SIGNATURES PREDATE IT. ***
// MeasureWidth / MeasureHeight / DrawTextCentered / DrawWrapped are declared in the header with a
// (UFont* Font, float Scale) pair from the Canvas era, and the header is not this pass's to edit. So
// the rule in this file is:
//     Font == nullptr  ->  the menu typeface, and Scale IS THE POINT SIZE in screen pixels.
//     Font != nullptr  ->  the old bitmap path, and Scale is the old scale factor.
// The second form is not vestigial: it is the live fallback taken whenever the menu face fails to
// resolve, and it is why FontSmall / FontMedium / FontLarge are still cached in Tick.
namespace TraceCharacterSelectType
{
	/** The measure service, or null when Slate's font services are not up (never true during DrawHUD). */
	static TSharedPtr<FSlateFontMeasure> Measurer()
	{
		if (!FEngineFontServices::IsInitialized())
		{
			return nullptr;
		}
		return FEngineFontServices::Get().GetFontMeasure();
	}

	/**
	 * THE UCanvas THE ENGINE IS CURRENTLY DRAWING THE HUD INTO.
	 *
	 * *** WHY THIS IS A LOOKUP AND NOT `HUD->Canvas`. *** AHUD::Canvas is PROTECTED, and this class is
	 * not an AHUD — it is handed one. AHUD exposes DrawText / DrawRect / DrawTexture and no way at all
	 * to reach the canvas object underneath them, and FCanvasTextItem (the only thing that can typeset
	 * with a real face) has to be submitted through it.
	 *
	 * So it is fetched from where the engine put it. UGameViewportClient::Draw creates exactly one
	 * UCanvas named "CanvasObject" in the transient package, Inits it to the view rect, and passes that
	 * same object to AHUD::SetCanvas — see GameViewportClient.cpp, GetCanvasByName("CanvasObject") then
	 * `PlayerController->MyHUD->SetCanvas(CanvasObject, DebugCanvasObject)`. Finding it by name gets
	 * the identical object AHUD::DrawText is using two lines away, not a second canvas.
	 *
	 * GUARDED, because a name lookup deserves it: only accepted while it holds a live FCanvas and a
	 * non-zero size, which is only true inside a draw pass. Null makes HaveFace() false, and every
	 * measurement and every draw in this file then takes the engine-bitmap fallback together — the one
	 * thing that must not happen is measuring with one and drawing with the other.
	 */
	static UCanvas* GameCanvas()
	{
		static TWeakObjectPtr<UCanvas> Cached;

		UCanvas* Found = Cached.Get();
		if (Found == nullptr)
		{
			Found = FindObject<UCanvas>(GetTransientPackage(), TEXT("CanvasObject"));
			if (Found != nullptr)
			{
				Cached = Found;
			}
		}

		if (Found != nullptr && Found->Canvas != nullptr && Found->SizeX > 0 && Found->SizeY > 0)
		{
			return Found;
		}
		return nullptr;
	}

	/**
	 * FCanvasTextItem works in DPI-SCALED pixels internally and multiplies the position it is given by
	 * the canvas DPI scale, so a size measured at FontScale=1 would disagree with what is drawn on any
	 * display where that is not 1. Every measurement below asks at the canvas's own scale and divides
	 * back, which is precisely what FCanvasSimpleTextItem::GetTextSizeInternal does.
	 */
	static float DpiScale(AHUD* HUD)
	{
		(void)HUD;
		if (const UCanvas* Surface = GameCanvas())
		{
			return FMath::Max(0.01f, Surface->Canvas->GetDPIScale());
		}
		return 1.f;
	}

	/** The menu face at @p Size points, or an invalid FSlateFontInfo when it did not resolve. */
	static FSlateFontInfo Face(float Size)
	{
		return TraceMenuArtStyle::MenuFont(FMath::Max(1.f, Size));
	}

	static bool HaveFace()
	{
		return Face(16.f).HasValidFont() && Measurer().IsValid() && GameCanvas() != nullptr;
	}

	/**
	 * THE FALLBACK, AND IT HAS TO CONVERT UNITS OR IT DRAWS THE SCREEN FORTY TIMES TOO BIG.
	 *
	 * Every caller in this file passes a POINT SIZE where the old code passed a SCALE FACTOR — 42 means
	 * "42 pixels tall", not "42 times bigger". If the menu face fails to resolve and that number went
	 * straight to AHUD::DrawText it would magnify the engine's bitmap font 42x. So the bitmap path
	 * measures its own line box once and derives the multiplier that lands on the size that was asked
	 * for. The result is the wrong TYPEFACE at the right SIZE, which is the failure this is meant to be.
	 */
	static void ResolveFallback(AHUD* HUD, float Size, UFont*& OutFont, float& OutScale)
	{
		OutFont = (GEngine != nullptr) ? GEngine->GetMediumFont() : nullptr;
		OutScale = 1.f;

		float MeasuredW = 0.f;
		float MeasuredH = 0.f;
		HUD->GetTextSize(TEXT("Ag"), MeasuredW, MeasuredH, OutFont, 1.f);
		if (MeasuredH > 1.f)
		{
			OutScale = FMath::Clamp(Size / MeasuredH, 0.35f, 6.f);
		}
	}

	/**
	 * @param Weight  MUST match the weight the string will be DRAWN in. It defaults to Light like
	 *                every other entry point here, so the ~30 call sites that draw light need no
	 *                edit; the two that draw the character NAMES bold pass it explicitly.
	 */
	float Width(AHUD* HUD, const FString& Text, UFont* Font, float Scale, float Tracking,
		ETraceTextWeight Weight = ETraceTextWeight::Light)
	{
		if (HUD == nullptr || Text.IsEmpty())
		{
			return 0.f;
		}

		// SPEC v22 §A1 — the artist's face first, and it needs no unit conversion here: this file
		// already made Scale mean A POINT SIZE IN PIXELS (see the convention block above), which is
		// exactly what TraceText::FStyle::Size means. Font != nullptr is still the old bitmap path and
		// is left alone; nothing in this file passes one.
		if (Font == nullptr && TraceText::IsAtlasActive())
		{
			TraceText::FStyle Style(FMath::Max(1.f, Scale));
			Style.Tracking = Tracking;
			// THE WEIGHT HAS TO REACH THE MEASUREMENT, and this is the correction the v23 integration
			// pass had to make. §A3 landed on the assumption that "both weights share every advance",
			// which was true while the light cut was Regular ERODED — same font, same metrics. It
			// stopped being true the moment the real Sofachrome W05 ExtraLight arrived: 94 of the 95
			// cells now differ, and Bold is roughly half again as wide (compare '!' at 19 px light
			// against 40 px bold in TraceFontAtlasMetrics.h). Measuring a bold name with light
			// advances under-reports it by a third, so the shrink-to-fit clamps below let it through
			// and "MORTIMER" printed through the MOVEMENT paragraph — photographed at 1920x1080.
			Style.Weight = Weight;
			return TraceText::MeasureWidth(Text, Style);
		}

		if (Font == nullptr && !HaveFace())
		{
			UFont* Fallback = nullptr;
			float FallbackScale = 1.f;
			ResolveFallback(HUD, Scale, Fallback, FallbackScale);

			float OutWidth = 0.f;
			float OutHeight = 0.f;
			HUD->GetTextSize(Text, OutWidth, OutHeight, Fallback, FallbackScale);
			return OutWidth;
		}

		if (Font == nullptr)
		{
			const float Dpi = DpiScale(HUD);
			const FVector2D Measured = FVector2D(Measurer()->Measure(Text, Face(Scale), Dpi));

			// HorizSpacingAdjust is applied per character by the canvas and the measure service knows
			// nothing about it, so tracking is added here. Len-1 rather than Len: the trailing gap after
			// the last glyph is not ink and must not shift a centred string by half of itself.
			return (Measured.X / Dpi) + Tracking * static_cast<float>(FMath::Max(0, Text.Len() - 1));
		}

		float OutWidth = 0.f;
		float OutHeight = 0.f;
		HUD->GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
		return OutWidth;
	}

	/** The LINE BOX height for the size, not the ink height of this particular string. */
	float LineHeight(AHUD* HUD, UFont* Font, float Scale)
	{
		if (HUD == nullptr)
		{
			return 0.f;
		}

		if (Font == nullptr && TraceText::IsAtlasActive())
		{
			return TraceText::LineHeight(FMath::Max(1.f, Scale));
		}

		if (Font == nullptr && HaveFace())
		{
			const float Dpi = DpiScale(HUD);
			return FVector2D(Measurer()->Measure(TEXT("Ag"), Face(Scale), Dpi)).Y / Dpi;
		}

		UFont* Chosen = Font;
		float ChosenScale = Scale;
		if (Font == nullptr)
		{
			ResolveFallback(HUD, Scale, Chosen, ChosenScale);
		}

		float OutWidth = 0.f;
		float OutHeight = 0.f;
		HUD->GetTextSize(TEXT("Ag"), OutWidth, OutHeight, Chosen, ChosenScale);
		return OutHeight;
	}

	void Draw(AHUD* HUD, const FString& Text, const FLinearColor& Color,
		float X, float Y, UFont* Font, float Scale, float Tracking,
		ETraceTextWeight Weight = ETraceTextWeight::Light)
	{
		if (HUD == nullptr || Text.IsEmpty())
		{
			return;
		}

		if (Font == nullptr && TraceText::IsAtlasActive())
		{
			TraceText::FStyle Style(FMath::Max(1.f, Scale), Color);
			Style.Tracking = Tracking;
			// spec v23 A3 — the character NAMES are the one thing on this screen the owner wants
			// left bold; every other string keeps the light default, which is why this argument
			// defaults rather than being passed at each of the call sites. The two weights do NOT
			// share advances (they are two different font files), so every fit-to-tile measurement
			// of a name passes this same weight to Width().
			Style.Weight = Weight;
			TraceCanvasText::Draw(HUD, Text, X, Y, Style);
			return;
		}

		if (Font == nullptr && HaveFace())
		{
			UCanvas* Surface = GameCanvas();
			if (Surface == nullptr)
			{
				return;   // unreachable while HaveFace() is true; kept so the deref is unconditional
			}

			FCanvasTextItem Item(FVector2D(X, Y), FText::FromString(Text), Face(Scale), Color);

			// In DPI-scaled pixels, like everything else inside the item.
			Item.HorizSpacingAdjust = Tracking * DpiScale(HUD);
			Surface->DrawItem(Item);
			return;
		}

		UFont* Chosen = Font;
		float ChosenScale = Scale;
		if (Font == nullptr)
		{
			ResolveFallback(HUD, Scale, Chosen, ChosenScale);
		}

		HUD->DrawText(Text, Color, X, Y, Chosen, ChosenScale);
	}

	void DrawCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color,
		float CenterX, float Y, UFont* Font, float Scale, float Tracking)
	{
		Draw(HUD, Text, Color, CenterX - Width(HUD, Text, Font, Scale, Tracking) * 0.5f, Y, Font, Scale, Tracking);
	}

	void DrawRight(AHUD* HUD, const FString& Text, const FLinearColor& Color,
		float RightX, float Y, UFont* Font, float Scale, float Tracking)
	{
		Draw(HUD, Text, Color, RightX - Width(HUD, Text, Font, Scale, Tracking), Y, Font, Scale, Tracking);
	}

	/**
	 * How many lines @p Text would wrap to at @p Scale in @p MaxWidth. Same greedy walk as WrapDraw,
	 * so the count and the draw cannot disagree.
	 *
	 * @param Weight  MUST be the weight the paragraph will be DRAWN in. The faces do not share
	 *                advances, so a count taken in one face is a count of a different paragraph:
	 *                Erbaum is about 30% narrower than the light cut, and a fit-to-column search
	 *                that measured light would step the body size down further than it needs to.
	 */
	int32 WrapLines(AHUD* HUD, const FString& Text, float MaxWidth, UFont* Font, float Scale,
		ETraceTextWeight Weight = ETraceTextWeight::Light)
	{
		TArray<FString> Words;
		Text.ParseIntoArray(Words, TEXT(" "), /*InCullEmpty=*/true);

		int32 Lines = 0;
		FString Line;

		for (const FString& Word : Words)
		{
			const FString Candidate = Line.IsEmpty() ? Word : (Line + TEXT(" ") + Word);
			if (!Line.IsEmpty() && Width(HUD, Candidate, Font, Scale, 0.f, Weight) > MaxWidth)
			{
				++Lines;
				Line = Word;
			}
			else
			{
				Line = Candidate;
			}
		}

		return Lines + (Line.IsEmpty() ? 0 : 1);
	}

	/**
	 * THE ONE WORD-WRAP ON THIS SCREEN. FTraceCharacterSelect::DrawWrapped is a thin call into it.
	 *
	 * It moved here from that member function for one reason: spec v25 §4 puts the ability
	 * DESCRIPTIONS in a different face from everything around them, and the member's signature is
	 * declared in TraceCharacterSelect.h, which this pass does not own. Rather than grow a second
	 * greedy walk beside the first — two wrap algorithms that would drift the first time either was
	 * touched — the algorithm lives here with a weight on the end and both callers reach it.
	 *
	 * @param Weight  the face to measure AND draw in. Both, from the same argument, because a wrap
	 *                that measured one face and drew another would break its lines in the wrong
	 *                places and overflow the column — silently, since a wrapped paragraph has no
	 *                obvious right edge to overrun.
	 */
	float WrapDraw(AHUD* HUD, const FString& Text, const FLinearColor& Color,
		float X, float Y, float MaxWidth, UFont* Font, float Scale, float LineGap,
		ETraceTextWeight Weight = ETraceTextWeight::Light)
	{
		// Greedy word wrap: measure a candidate line and back off one word. Exact rather than
		// approximate for the engine's bitmap fonts (no shaping, no kerning), and near enough for a
		// proportional face — there is no script here where adding a word makes a line narrower.
		//
		// See the parameter-convention block above: Font == nullptr means the menu typeface and Scale
		// is a POINT SIZE.
		TArray<FString> Words;
		Text.ParseIntoArray(Words, TEXT(" "), /*InCullEmpty=*/true);

		const float LineH = LineHeight(HUD, Font, Scale);
		float CursorY = Y;
		FString Line;

		for (const FString& Word : Words)
		{
			const FString Candidate = Line.IsEmpty() ? Word : (Line + TEXT(" ") + Word);
			if (!Line.IsEmpty() && Width(HUD, Candidate, Font, Scale, 0.f, Weight) > MaxWidth)
			{
				Draw(HUD, Line, Color, X, CursorY, Font, Scale, 0.f, Weight);
				CursorY += LineH + LineGap;
				Line = Word;
			}
			else
			{
				Line = Candidate;
			}
		}

		if (!Line.IsEmpty())
		{
			Draw(HUD, Line, Color, X, CursorY, Font, Scale, 0.f, Weight);
			CursorY += LineH + LineGap;
		}

		return CursorY;
	}

	/**
	 * THE FACE THE ABILITY DESCRIPTIONS ARE SET IN (spec v25 §4).
	 *
	 *     "Use Erbaum Bold for in game hud and character ability descriptions"
	 *
	 * The three prose paragraphs — MOVEMENT, PASSIVE, ACTIVATED — and nothing else on this screen.
	 * The character NAME stays Sofachrome Regular (v23 §A3, restated by v25 §4), the headings, the
	 * keycaps, the cooldown chip, the status line and the footer stay in the light default. That is
	 * the owner's instruction read literally: the DESCRIPTION text changes, the screen does not.
	 *
	 * Shares Trace.HUD.Text.Erbaum with the in-match HUD deliberately — one switch puts both halves
	 * of the §4 change back to the pre-v25 face for a same-binary before/after, and two switches
	 * would let a capture show one half changed and read as the whole thing.
	 */
	static ETraceTextWeight DescriptionWeight()
	{
		// Looked up rather than cached in a static. The arm is declared in UI/TraceHUD.cpp and this is
		// a different translation unit, so a cached pointer would bake in whatever the answer was on
		// the first frame this screen ever drew — including "not registered yet" — and the red arm
		// would then be dead for the rest of the process. Called once per card per frame; a console
		// lookup is a hash probe.
		const IConsoleVariable* Arm =
			IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.HUD.Text.Erbaum"));
		const bool bErbaum = (Arm == nullptr) || (Arm->GetInt() != 0);
		return bErbaum ? ETraceTextWeight::Hud : ETraceTextWeight::Light;
	}
}

// NAMED after the file rather than anonymous. UBT compiles this module as a unity/jumbo build, so two
// files that each open `namespace { }` become ONE namespace holding both sets of definitions, and
// "NumberKeyForIndex" is exactly the kind of name a second UI file would also want.
// Scripts/check-jumbo-build-collisions.py gates the build on it; this used to be anonymous.
namespace TraceCharacterSelectFile
{
	/**
	 * The number keys, in roster order, so "press 3 for Mace" is literally true.
	 *
	 * ONE KEY PER CARD ONLY WORKS WHILE THE ROSTER FITS THE NUMBER ROW, AND SPEC v19 §3 IS EXACTLY
	 * WHERE IT RUNS OUT. This used to stop at 9 and hand the tenth card nothing but the arrows and
	 * the mouse; the tenth character now exists (Lily), so the tenth card takes ZERO — the last key
	 * on the number row, and the one every player already reads as "ten".
	 *
	 * IT STOPS THERE FOR REAL. An ELEVENTH character has no key left and gets EKeys::Invalid, which
	 * is deliberate rather than an omission: WasInputKeyJustPressed on Invalid is simply always
	 * false, so a card is never picked by a key that is really some other card's. The next roster
	 * addition needs a different scheme (modifiers, or a second page), not another entry here.
	 */
	const FKey& NumberKeyForIndex(int32 Index)
	{
		switch (Index)
		{
		case 0:  return EKeys::One;
		case 1:  return EKeys::Two;
		case 2:  return EKeys::Three;
		case 3:  return EKeys::Four;
		case 4:  return EKeys::Five;
		case 5:  return EKeys::Six;
		case 6:  return EKeys::Seven;
		case 7:  return EKeys::Eight;
		case 8:  return EKeys::Nine;
		case 9:  return EKeys::Zero;      // spec v19 §3 — Lily. "0" is the tenth key, not a tenth name.
		default: return EKeys::Invalid;
		}
	}

	/** "TAKEN BY BOB" needs the holder's name; APlayerState::GetPlayerName is not const-safe to call blind. */
	FString SafePlayerName(const ATracePlayerState* State)
	{
		if (State == nullptr)
		{
			return FString(TEXT("A TEAM-MATE"));
		}

		// The BOT suffix is not decoration. Since spec v15 §2 a card can be greyed out by a computer
		// team-mate, and "TAKEN BY BOT BLUE 3" is the difference between a player understanding why
		// and a player thinking the screen is broken. The bot names this project generates already
		// begin with "BOT ", so the suffix is only added when they do not — a server may rename them.
		const FString Name = State->GetPlayerName();
		if (State->IsABot() && !Name.StartsWith(TEXT("BOT"), ESearchCase::IgnoreCase))
		{
			return Name + TEXT(" (BOT)");
		}

		return Name;
	}

	/**
	 * THE KEY THAT PICKS A CARD, AS A KEY — spec v20 §6.6.
	 *
	 * The tenth card's key is ZERO, and the old screen printed a bare "0" in the corner where every
	 * other card printed its ordinal. Measured complaint, verbatim: "the tenth character is labelled 0
	 * and reads as an ordinal, not a key". The number is right and the PRESENTATION was wrong — so the
	 * fix is not to renumber anything (that would make the screen lie about its own controls) but to
	 * draw all ten inside a keycap. This returns the glyph; DrawKeyCap puts the cap around it.
	 *
	 * Empty past the tenth card, exactly like NumberKeyForIndex returns EKeys::Invalid there: a card
	 * with no key must show no key rather than a wrong one.
	 */
	FString KeyGlyphForIndex(int32 Index)
	{
		if (Index < 0 || Index > 9)
		{
			return FString();
		}
		return (Index == 9) ? FString(TEXT("0")) : FString::Printf(TEXT("%d"), Index + 1);
	}

	// -----------------------------------------------------------------------------------------
	// SENTENCE CASE — spec v20 §6.2
	// -----------------------------------------------------------------------------------------
	//
	// Verbatim from the defect list: "A wall of tiny all-caps body text ... All-caps at that size is
	// the single biggest legibility cost on the screen."
	//
	// The roster's prose is stored upper case and STAYS stored upper case — TraceCharacterRoster is
	// shared with the game mode, the ability framework and the generated data assets, and the header
	// there gives a real reason for the ASCII-only, upper-case rule (the engine's BITMAP fonts have no
	// glyph for an em dash or a curly quote). This is a PRESENTATION change in this one file: nothing
	// upstream is touched and nothing downstream can notice.
	//
	// The naive version of this — lowercase everything, capitalise after a full stop — is wrong here
	// and the roster shows exactly why: "HOLD V IN THE AIR" would become "hold v in the air", and V is
	// a KEY. So three classes of token are left alone:
	//
	//   * anything containing a digit           1.25S, +30%, 1.65X, 60, 550   (units and numbers)
	//   * any single letter that is not "A"     V, E, X                        (keys, and X's name)
	//   * proper nouns                          Rocco, Ripple, Core            (title-cased, not shouted)
	//
	// and the proper-noun list is BUILT FROM THE ROSTER rather than written out, so a new character or
	// a renamed ability cannot fall out of it.
	//
	// Worst case if a rule mis-fires: one word reads lower case that a copy editor would have
	// capitalised. There is no failure mode that loses text, changes meaning, or touches data.

	static bool TokenHasDigit(const FString& Token)
	{
		for (int32 Index = 0; Index < Token.Len(); ++Index)
		{
			if (FChar::IsDigit(Token[Index]))
			{
				return true;
			}
		}
		return false;
	}

	/** The letters of @p Token with the leading and trailing punctuation stripped off. */
	static FString TokenCore(const FString& Token)
	{
		int32 Start = 0;
		int32 End = Token.Len();
		while (Start < End && !FChar::IsAlnum(Token[Start]))
		{
			++Start;
		}
		while (End > Start && !FChar::IsAlnum(Token[End - 1]))
		{
			--End;
		}
		return (End > Start) ? Token.Mid(Start, End - Start) : FString();
	}

	/** Character names and ability names, upper case, resolved once from whichever roster is serving. */
	static const TSet<FString>& ProperNouns()
	{
		static TSet<FString> Nouns;
		if (Nouns.Num() == 0)
		{
			for (const TraceCharacterRoster::FTraceCharacterEntry& Entry : TraceCharacterRoster::All())
			{
				Nouns.Add(FString(Entry.Name).ToUpper());
				Nouns.Add(FString(Entry.ActivatedName).ToUpper());
			}

			// The one proper noun that is not a character or an ability. It is the object the whole game
			// is about and it appears in half the cards; lower-casing it would read as a typo.
			Nouns.Add(TEXT("CORE"));
		}
		return Nouns;
	}

	/** Upper-cases the first letter of @p Token and lower-cases the rest. "CORE." -> "Core." */
	static FString TitleCased(const FString& Token)
	{
		FString Out = Token.ToLower();
		for (int32 Index = 0; Index < Out.Len(); ++Index)
		{
			if (FChar::IsAlpha(Out[Index]))
			{
				Out[Index] = FChar::ToUpper(Out[Index]);
				break;
			}
		}
		return Out;
	}

	FString SentenceCase(const FString& Upper)
	{
		TArray<FString> Tokens;
		Upper.ParseIntoArray(Tokens, TEXT(" "), /*InCullEmpty=*/true);

		FString Flowed;
		Flowed.Reserve(Upper.Len());

		for (int32 Index = 0; Index < Tokens.Num(); ++Index)
		{
			if (Index > 0)
			{
				Flowed += TEXT(" ");
			}

			const FString& Token = Tokens[Index];
			const FString Core = TokenCore(Token);

			if (Core.IsEmpty() || TokenHasDigit(Token))
			{
				// A token carrying a digit is a measurement, and its letters are a UNIT: "1.25S" is
				// one and a quarter seconds, not an initialism. Lower-casing just the letters gives
				// "1.25s" / "4s" / "1.65x" and leaves "+30%", "550" and "60" untouched.
				Flowed += Token.ToLower();
			}
			else if (Core.Len() == 1 && Core != TEXT("A"))
			{
				Flowed += Token;                       // "V" / "E" / "X"
			}
			else if (ProperNouns().Contains(Core))
			{
				Flowed += TitleCased(Token);           // "Core," / "Ripple" / "Slimeball"
			}
			else
			{
				Flowed += Token.ToLower();
			}
		}

		// Sentence starts. A full stop only ends a sentence when a space (or the end of the string)
		// follows it, which is what keeps "1.25S" from capitalising its own fraction.
		FString Cased;
		Cased.Reserve(Flowed.Len());

		bool bAtSentenceStart = true;
		for (int32 Index = 0; Index < Flowed.Len(); ++Index)
		{
			TCHAR Ch = Flowed[Index];

			if (bAtSentenceStart && FChar::IsAlpha(Ch))
			{
				Ch = FChar::ToUpper(Ch);
				bAtSentenceStart = false;
			}
			// Deliberately NOT a colon. "ALWAYS: TWO NORMALLY" is one sentence with a list after it, and
			// treating the colon as a full stop produced "always: Two normally" in the first capture.
			else if (Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?'))
			{
				bAtSentenceStart = (Index + 1 >= Flowed.Len()) || (Flowed[Index + 1] == TEXT(' '));
			}

			Cased.AppendChar(Ch);
		}

		return Cased;
	}

	/**
	 * How long this opening of the screen was given, latched when it opened.
	 *
	 * The countdown BAR needs a denominator and the player state only replicates a deadline, so the
	 * span is caught the first time it is seen. A file-scope float rather than a member for the reason
	 * the header gives about GTraceCharacterSelectDebugPick — this class is a plain member of an AHUD
	 * that a travel destroys — and there is exactly one select screen open on a client at a time.
	 * FMath::Max rather than a straight assignment because the deadline can replicate a frame or two
	 * after the open flag does, and the first read would otherwise latch a zero span forever.
	 */
	static float SelectSpanSeconds = 0.f;

	// =============================================================================================
	// THE TILE'S TYPE AND PICTURE SIZES — SOLVED ONCE FOR THE WHOLE GRID (UI plan WP10)
	// =============================================================================================
	//
	// *** WHY THIS IS NOT PER-CARD ARITHMETIC, WHICH IS WHERE THE FIRST ATTEMPT WENT WRONG. ***
	//
	// The obvious way to fit a picture beside a name is to give each card whatever is left after its
	// OWN name. That was written, built and photographed, and the frame settles the argument: X's
	// portrait came out 180 px square and SLIMEBALL's 78, because their names are 10 px and 233 px
	// wide. Ten cards in one grid with ten different picture sizes does not read as a roster of ten
	// characters; it reads as a rendering fault. Two of them also overflowed, because a name allowed
	// to stop shrinking at its 72 % floor can still be wider than the room a long name left.
	//
	// So every size below is solved ONCE, against the WIDEST string in the roster, and every card
	// draws at the same numbers. A grid is a grid.
	//
	// *** THE KEYCAP MOVED OFF THE NAME'S LINE, AND THAT IS WHAT MAKES THE PICTURE POSSIBLE. ***
	// Measured off the shipped frame at 1080p, in the menu face at the authored sizes:
	//
	//     widest tile name     SLIMEBALL   233 px of ink at 25 px
	//     widest ability name  SLIMEWALL   200 px of ink at 20 px
	//     tile                             346 px wide
	//
	// A keycap and its gap are 42 of those 346. With the cap on the name's line the arithmetic has no
	// solution worth having: leaving SLIMEBALL a legible 18 px caps the picture at 86 px square, and
	// a 128 px picture would have forced every name in the roster down to 16 px. Stacking the cap
	// above the name — where it also reads more honestly, as the key that picks this card rather than
	// as a bullet beside its name — hands the name the full text column and buys 42 px for both.
	//
	// The cooldown chip moved for the same reason: it now shares the ACTIVATED label's line instead
	// of the ability name's, so SLIMEWALL also gets the whole column.
	//
	// =============================================================================================
	// *** AND THE MICRO ROW IS SOLVED HERE TOO NOW, BECAUSE IT WAS THE ONE THING ON THIS TILE THAT
	// *** WAS NOT — WHICH IS WHY "ACTIVATED" PRINTED THROUGH THE COOLDOWN CHIP ON EVERY 16:10
	// *** DISPLAY, INCLUDING THIS PROJECT OWNER'S LAPTOP, AND NEVER ON A 16:9 TEST MONITOR.
	// =============================================================================================
	//
	// W8-VISUAL §4 measured the mechanism rather than reading it, and the measurement is the reason
	// this note is long: the bug is a CLASS, not a label.
	//
	//     UIScale = Clamp(ViewH / ReferenceHeight, 0.6, 2.0)          — HEIGHT, TraceHUD.cpp
	//     the text column TextW follows the tile, which follows       — WIDTH
	//
	// So the size of the type was derived from one axis of the viewport while the box it had to fit
	// inside was derived from the other. Nothing compared them, and nothing elided. Narrow the display
	// and the type grows while its column shrinks, until the right-aligned chip lands on the word.
	// The governing number is UIScale / ViewW, and normalised to 1920x1080 it reads:
	//
	//     1920 x 1080  16:9    UIScale 1.000   ratio 1.00x  ->  "ACTIVATED"   (the developer's monitor)
	//     1920 x 1200  16:10   UIScale 1.111   ratio 1.11x  ->  "ACTIVATE"    (every laptop)
	//     3456 x 2234  1.547   UIScale 2.000   ratio 1.11x  ->  "ACTIVATE"    (this machine, native)
	//     1728 x 1117  1.547   UIScale 1.034   ratio 1.15x  ->  "ACTIVAT"     (this machine, default)
	//
	// The two arms at 1.11x clip IDENTICALLY across a 3.3x difference in pixel count, which is what
	// proves the governing variable is the ratio and not the resolution.
	//
	// THE FIX IS NOT A BIGGER MARGIN OR A SHORTER WORD. It is to solve the row against the dimension
	// that bounds it, exactly as NameSize and LeadSize below are already solved against TextW — the
	// label, its tracking, the chip's type and the chip's height all step down together until the
	// whole row fits the column, and they step down UNIFORMLY so a chip never stops matching the type
	// beside it. Fitted once for the grid against the WIDEST cooldown string in the roster, for the
	// same reason everything else here is: ten cards with ten row scales photograph as a fault.
	//
	// The fit is ITERATED, not divided. A first-order scale (Need/Have) is only exact if advance width
	// were perfectly linear in point size, and it is not — the face is rasterised and hinted per size,
	// and ChipWidth adds H * 0.90 of plate. Two refinement passes cost two measurements of one short
	// string per frame and remove the guesswork; the loop then verifies and stops.
	struct FTileMetrics
	{
		float PortraitSide = 0.f;   // square, uniform, right-aligned inside the plate
		float TextW = 0.f;          // the text column; NOTHING is measured against the tile's width
		float NameSize = 0.f;
		float NameTrack = 0.f;
		float LeadSize = 0.f;
		float LeadTrack = 0.f;
		float CapH = 0.f;
		float PortraitInset = 0.f;
		float PortraitGap = 0.f;
		float Pad = 0.f;

		// ---- the micro row: "ACTIVATED" at the left of the text column, the cooldown chip at its
		// ---- right, on one line. All four fall together; see the note above.
		float MicroSize = 0.f;      // the ACTIVATED label's type size
		float MicroTrack = 0.f;     // its tracking
		float ChipTextSize = 0.f;   // the cooldown chip's type size
		float ChipH = 0.f;          // the cooldown chip's height (and, via ChipWidth, its padding)
		float MicroScale = 1.f;     // what the row had to give up to fit; 1.0 = nothing. Logged.
		float MicroRowW = 0.f;      // what the fitted row actually measures. Must be <= TextW.
	};

	/** Set by FTraceCharacterSelect::Draw before the tiles, read by DrawCard. One grid, one answer. */
	static FTileMetrics TileMetrics;

	/**
	 * Hash of the numbers on the [CharSelect] Layout line that a console command can move. See the
	 * note at the log site: the line is once-per-process, and again when the answer changes, so an
	 * A/B run inside one process is legible in the log and not only in the frames.
	 */
	static uint32 LastLoggedLayoutSignature = 0;

	/**
	 * Declared early so the micro-row fit below can ASK the chip how wide it would be rather than
	 * re-deriving its padding. The definition is a few dozen lines down, next to DrawChip, because the
	 * two must never disagree about what a chip costs; a copied `+ H * 0.90f` here is exactly the
	 * shape this project's DEMO 21 rule forbids, and it is what a fit computed against the wrong
	 * padding would silently become.
	 */
	float ChipWidth(AHUD* HUD, const FString& Text, float H, float MinW, float TextSize, float Tracking);

#if !UE_BUILD_SHIPPING
	/**
	 * THE RED ARM FOR THE MICRO ROW, because a layout fix that cannot be made to fail again is not
	 * evidence that it was ever broken.
	 *
	 * `Trace.Characters.FitMicroRow 0` puts the row back on UIScale alone — the height-derived sizing
	 * that shipped through W8 — without touching anything else, so the overprint comes back on the
	 * same binary in the same run and the A/B is two frames rather than two builds:
	 *
	 *   -TraceExec=Trace.Characters.FitMicroRow 0
	 *
	 * It is also the regression test: any future change that reintroduces a height-sized element in a
	 * width-bounded column will look, at 1920x1200, exactly like arm 0 of this pair.
	 *
	 * Cheat-only and compiled out of shipping, so no ini and no player can turn the fit off.
	 */
	static TAutoConsoleVariable<int32> CVarFitMicroRow(
		TEXT("Trace.Characters.FitMicroRow"),
		1,
		TEXT("Dev only. 1 (default): BOTH ability rows — the select card's ACTIVATED / cooldown line "
		     "and the detail panel's keycap / ability name / cooldown line — are fitted to the WIDTH of "
		     "the column that bounds them. 0: the pre-W9 behaviour, sized from UIScale (view HEIGHT) "
		     "alone, which overprints the card's label on every display narrower than 16:9 and the "
		     "panel's ability name on anything narrower than about 1.45:1."),
		ECVF_Cheat);
#endif

	void SolveTileMetrics(AHUD* HUD, float TileW, float TileH, float S)
	{
		FTileMetrics M;
		M.Pad = 18.f * S;
		M.PortraitInset = 8.f * S;
		M.PortraitGap = 12.f * S;
		M.CapH = 26.f * S;

		// 37 % of the tile, capped by the height the plate can spare. At the shipped 346 x 196 tile
		// that is a 128 px square with 34 px of plate above and below it — a framed picture inside the
		// card rather than a texture bled to its edges, which is what the composite is drawn as (it
		// carries its own 2 px accent frame, corner brackets and accent underline; see
		// TraceCharacterSelectPortraits::CardMinU).
		M.PortraitSide = FMath::Clamp(TileW * 0.37f, 0.f, TileH - M.PortraitInset * 2.f);

		M.TextW = TileW - M.PortraitInset - M.PortraitSide - M.PortraitGap - M.Pad;

		// Below 56 design px the bust is a smudge; the card gives the picture up entirely rather than
		// print a bad one, and the text column takes the whole tile back. Same floor governs the
		// monogram fallback, so a narrow viewport does not get a tiny letter either.
		if (M.PortraitSide < 56.f * S || M.TextW < 60.f * S)
		{
			M.PortraitSide = 0.f;
			M.TextW = TileW - M.Pad * 2.f;
		}

		// ---- The two uniform type scales ---------------------------------------------------------
		const float BaseName = TraceSelectLayout::SizeName * S;
		const float BaseLead = TraceSelectLayout::SizeLead * S;
		const float BaseNameTrack = TraceSelectLayout::TrackName * S;

		float WidestName = 1.f;
		float WidestLead = 1.f;
		for (const TraceCharacterRoster::FTraceCharacterEntry& Entry : TraceCharacterRoster::All())
		{
			// Measured in the WEIGHT EACH IS DRAWN IN. Bold is about 1.5x the light advance, and
			// measuring the wrong one is exactly how MORTIMER once printed through the panel beside it.
			WidestName = FMath::Max(WidestName,
				TraceCharacterSelectType::Width(HUD, Entry.Name, nullptr, BaseName, BaseNameTrack,
					ETraceTextWeight::Bold));
			WidestLead = FMath::Max(WidestLead,
				TraceCharacterSelectType::Width(HUD, Entry.ActivatedName, nullptr, BaseLead, BaseNameTrack));
		}

		const float NameScale = FMath::Min(1.f, M.TextW / WidestName);
		M.NameSize = BaseName * NameScale;
		M.NameTrack = BaseNameTrack * NameScale;

		// The ability name gets the same column, and is then held to 88 % of the character's name so
		// the hierarchy is guaranteed by construction rather than by two numbers happening to differ.
		// It is the light weight in Ink against a bold accent, so 88 % is plenty of separation.
		const float LeadScale = FMath::Min(1.f, M.TextW / WidestLead);
		M.LeadSize = FMath::Min(BaseLead * LeadScale, M.NameSize * 0.88f);

		// Tracking follows the size that was actually CHOSEN, not the size the column fit produced —
		// the 88 % clamp above can be the binding one, and tracking scaled off the other number is how
		// a line ends up spaced for a size it is not set at.
		M.LeadTrack = BaseNameTrack * (M.LeadSize / FMath::Max(BaseLead, 1.f));

		// ---- THE MICRO ROW, FITTED TO THE COLUMN THAT BOUNDS IT ----------------------------------
		//
		// See the long note on FTileMetrics. Everything here was previously computed in DrawCard from
		// UIScale alone — the one thing on the tile that was sized by view HEIGHT and bounded by view
		// WIDTH — and that is the whole of the 16:10 overprint.
		const float BaseMicro      = TraceSelectLayout::SizeLabel * 0.86f * S;
		const float BaseMicroTrack = TraceSelectLayout::TrackLabel * S;
		const float BaseChipText   = TraceSelectLayout::SizeLabel * S;
		const float BaseChipH      = 22.f * S;

		// Air between the last letter of ACTIVATED and the chip's left edge. Without it, "fits" means
		// "touches", which photographs as a collision even when it is not one.
		const float BaseMicroGap = 10.f * S;

		// The WIDEST cooldown in the roster, not this card's — same reason as the two scales above.
		// Built with the identical Printf DrawCard uses, so the string measured is the string drawn.
		FString WidestCooldown = TEXT("00s");
		{
			float WidestCooldownW = 0.f;
			for (const TraceCharacterRoster::FTraceCharacterEntry& Entry : TraceCharacterRoster::All())
			{
				const FString Text = FString::Printf(TEXT("%ds"), FMath::RoundToInt(Entry.ActivatedCooldown));
				const float W = TraceCharacterSelectType::Width(HUD, Text, nullptr, BaseChipText, 0.f);
				if (W > WidestCooldownW)
				{
					WidestCooldownW = W;
					WidestCooldown = Text;
				}
			}
		}

		auto RowWidthAt = [&](float Scale) -> float
		{
			return TraceCharacterSelectType::Width(HUD, TEXT("ACTIVATED"), nullptr,
					   BaseMicro * Scale, BaseMicroTrack * Scale)
				+ (BaseMicroGap * Scale)
				+ ChipWidth(HUD, WidestCooldown, BaseChipH * Scale, 0.f, BaseChipText * Scale, 0.f);
		};

		// Iterated rather than divided once: glyph advance is not perfectly linear in point size (the
		// face is rasterised and hinted per size) and ChipWidth adds plate padding on top, so a single
		// first-order step can land a hair over. Three passes is two corrections and a verification;
		// each pass is two measurements of two short strings, once per frame for the whole grid.
		M.MicroScale = 1.f;

#if !UE_BUILD_SHIPPING
		const bool bFitMicroRow = (CVarFitMicroRow.GetValueOnGameThread() != 0);
#else
		constexpr bool bFitMicroRow = true;
#endif

		for (int32 Pass = 0; bFitMicroRow && Pass < 3; ++Pass)
		{
			const float Need = RowWidthAt(M.MicroScale);
			if (Need <= M.TextW)
			{
				break;
			}
			// Floored well below anything a display can ask for, and NOT floored at a "readable" size:
			// a small row is a legibility cost the player can still read past, an overprinted one is a
			// word with a box on it. Nothing is dropped and nothing is elided at any width.
			M.MicroScale = FMath::Max(0.05f, M.MicroScale * (M.TextW / FMath::Max(Need, 1.f)));
		}

		M.MicroSize    = BaseMicro * M.MicroScale;
		M.MicroTrack   = BaseMicroTrack * M.MicroScale;
		M.ChipTextSize = BaseChipText * M.MicroScale;
		M.ChipH        = BaseChipH * M.MicroScale;

		// Measured at the size finally chosen, not predicted from it, so the layout log reports what
		// the row IS rather than what the fit hoped for. This is the number a capture is checked
		// against: MicroRowW <= TextW is the whole of "ACTIVATED cannot be overprinted".
		M.MicroRowW = RowWidthAt(M.MicroScale);

		TileMetrics = M;
	}

	/** A hairline rectangle outline. The free-function twin of FTraceCharacterSelect::DrawFrame. */
	void StrokeRect(AHUD* HUD, float X, float Y, float W, float H, float Thick, const FLinearColor& Color)
	{
		if (HUD == nullptr || W <= 0.f || H <= 0.f)
		{
			return;
		}
		const float T = FMath::Max(1.f, Thick);
		HUD->DrawRect(Color, X, Y, W, T);
		HUD->DrawRect(Color, X, Y + H - T, W, T);
		HUD->DrawRect(Color, X, Y, T, H);
		HUD->DrawRect(Color, X + W - T, Y, T, H);
	}

	/**
	 * The artist's value chip with @p Text centred in it. Returns the width it took.
	 *
	 * This is the screen's ONE small-container primitive, and everything that is a discrete fact goes
	 * in one: the key that picks a card, the cooldown, the team, the countdown, every footer control.
	 * Spec v20 §6.5 and §6.6 are both really the same complaint — facts run together into a sentence
	 * ("ACTIVATED [E] - RIPPLE - 20s CD", "0") when they should be separate objects on the screen.
	 *
	 * The chip is drawn at its NATURAL corner scale (CornerHeight == H): at chip sizes the sheet's own
	 * corner radius is already right, and only the big surfaces need the override DrawPlate takes.
	 */
	/** What DrawChip would take, without drawing. The header has to lay chips out before the title. */
	float ChipWidth(AHUD* HUD, const FString& Text, float H, float MinW, float TextSize, float Tracking)
	{
		if (HUD == nullptr || H <= 0.f)
		{
			return 0.f;
		}
		return FMath::Max(MinW,
			TraceCharacterSelectType::Width(HUD, Text, nullptr, TextSize, Tracking) + H * 0.90f);
	}

	float DrawChip(AHUD* HUD, const FString& Text, float X, float Y, float H, float MinW,
		const FLinearColor& PlateTint, const FLinearColor& TextColor, float TextSize, float Tracking)
	{
		if (HUD == nullptr || H <= 0.f)
		{
			return 0.f;
		}

		const float TextW = TraceCharacterSelectType::Width(HUD, Text, nullptr, TextSize, Tracking);
		const float W = FMath::Max(MinW, TextW + H * 0.90f);

		if (UTexture2D* Box = TraceCharacterSelectArt::Sprite(TraceCharacterSelectArt::ESprite::ValueBox))
		{
			TraceCharacterSelectArt::DrawPlate(HUD, Box, TraceMenuArtStyle::ValueFrame, X, Y, W, H, H, PlateTint);
		}
		else
		{
			// The fallback is the rectangle this screen drew before there was any art, so a missing
			// texture costs the artist's corner and loses nothing a player needs.
			HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Dimmed(PlateTint, 0.35f), 0.92f), X, Y, W, H);
			StrokeRect(HUD, X, Y, W, H, 1.f, TraceSelectStyle::WithAlpha(PlateTint, 0.55f));
		}

		const float TextH = TraceCharacterSelectType::LineHeight(HUD, nullptr, TextSize);
		TraceCharacterSelectType::Draw(HUD, Text, TextColor,
			X + (W - TextW) * 0.5f, Y + (H - TextH) * 0.5f, nullptr, TextSize, Tracking);

		return W;
	}
}

// =============================================================================================
// Lifecycle + input
// =============================================================================================

void FTraceCharacterSelect::Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
	float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed)
{
	if (HUD == nullptr || InViewW <= 0.f || InViewH <= 0.f)
	{
		return;
	}

	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;
	Now = InNow;

	if (GEngine != nullptr)
	{
		FontSmall  = GEngine->GetSmallFont();
		FontMedium = GEngine->GetMediumFont();
		FontLarge  = GEngine->GetLargeFont();
	}

	// THE ONLY CONDITION. Replicated from the server, so mode A, the settings toggle, bots and
	// "already picked" are all answered upstream and none of them are re-derived here. See the
	// header for why that matters.
	const bool bShouldBeOpen = (LocalState != nullptr) && LocalState->IsCharacterSelectOpen();

	if (bShouldBeOpen != bOpen)
	{
		bOpen = bShouldBeOpen;

		if (bOpen)
		{
			HoveredCard = INDEX_NONE;
			PendingRequest = TraceCharacterRoster::NoneId;

			// FBox2D's default constructor leaves bIsValid UNINITIALISED, and PollInput runs before
			// Draw on this very frame — so without this the first frame's hit test would read garbage
			// and could report the pointer as being inside a card that has never been drawn. Cleared
			// here rather than in a member initialiser so the array's size stays a single constant.
			for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
			{
				CardRects[Index] = FBox2D(ForceInit);
			}

			// Swallow the remainder of this frame's input. Without it, the key that was being held
			// when the screen appeared — most often a movement key during warm-up — lands on a card.
			IgnoreInputBeforeFrame = GFrameCounter + 1;

			// The countdown bar's denominator, latched per opening. See SelectSpanSeconds.
			TraceCharacterSelectFile::SelectSpanSeconds = 0.f;

			// Resolve the artist's sprites now rather than on the first Draw, so the one-line inventory
			// is in the log next to the "[CharSelect] Screen opened" line that says what it is for.
			TraceCharacterSelectArt::LogInventoryOnce();

			// Start the highlight on the first character no team-mate is believed to hold, so the
			// default action is a legal one rather than one that will be refused.
			Highlighted = 0;
			bool bFoundFree = false;

			// The believed roster, printed once per opening.
			//
			// It is here because spec v15 §2 made this screen's belief able to be wrong in a NEW way:
			// a BOT team-mate can now hold a card, and until §2 this function skipped bots entirely.
			// A greyed card is otherwise invisible to a headless run and indistinguishable in a
			// screenshot from a card that failed to draw, so the screen says out loud what it thinks
			// is taken and by whom — which is also the first thing to read when a player reports
			// "it would not let me pick".
			FString RosterBelief;
			for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
			{
				const uint8 CandidateId = static_cast<uint8>(TraceCharacterRoster::FirstId + Index);
				const ATracePlayerState* const Holder = FindTeammateHolding(LocalState, CandidateId);

				if (Holder == nullptr && !bFoundFree)
				{
					Highlighted = Index;
					bFoundFree = true;
				}

				RosterBelief += FString::Printf(TEXT("%s%s=%s"),
					(Index > 0) ? TEXT(" ") : TEXT(""),
					*TraceCharacterRoster::NameFor(CandidateId),
					(Holder != nullptr) ? *TraceCharacterSelectFile::SafePlayerName(Holder).ToUpper() : TEXT("free"));
			}

			UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Screen opened (team %s, %.0fs to pick). Believes: %s"),
				*TraceTeamName(LocalState->Team).ToString(), LocalState->GetCharacterSelectTimeRemaining(),
				*RosterBelief);

			if (OnOpened)
			{
				OnOpened();
			}
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Screen closed."));
#if !UE_BUILD_SHIPPING
			// A click that WORKS closes the screen, and Tick returns below without ever reaching the
			// judging stage. Reporting here is what makes the passing run the one that prints a
			// verdict, instead of the one that goes quiet.
			ReportClickTest(LocalState);
#endif
			if (OnClosed)
			{
				OnClosed();
			}
		}
	}

	// ---- SPEC v24 §2 — THE OS POINTER STOPS BEING DRAWN OVER OURS -------------------------------
	//
	//     "The new cursor ui is working, but my cursor shows on top of it"
	//
	// ABOVE the `if (!bOpen) return` on purpose, because this call is the one place in a match that
	// runs every single frame whether or not this screen is up (ATraceHUD::DrawHUD calls it outside
	// every gate — see the comment at that call site), and the pause / settings overlay in front of
	// it draws the artist's arrow too.
	//
	// TWO SURFACES, ONE ANSWER:
	//   * this screen, while it is open and has a pointer — DrawCursor() below draws T_MenuCursor at
	//     CursorPos, and bHasCursor is exactly "PollInput has had a mouse position", i.e. the same
	//     condition DrawCursor itself returns on;
	//   * whatever is in FRONT of it, which is what bInputAllowed already means. Its own header
	//     defines the parameter as "false while something in front of this owns the keyboard (the
	//     pause menu)", and ATraceHUD passes literally `!PauseMenu.IsOpen()`. That overlay is an
	//     FTraceOptionsMenu and FTraceOptionsMenu::DrawCursor draws the same artist's arrow — so the
	//     flag this screen is already given answers the question for the screen on top of it, with no
	//     reach into a class this slice does not own.
	//
	// Renewed, never latched: the lease expires two frames after this stops being called, so a match
	// that ends, a HUD that is destroyed or a screen that closes hands the hardware pointer straight
	// back. See UI/TraceHardwareCursor.h.
	{
		TraceHardwareCursor::EnsureRunning();

		const bool bOwnPointerDrawn = bOpen && bHasCursor;
		const bool bOverlayInFront = !bInputAllowed;
		if (bOwnPointerDrawn || bOverlayInFront)
		{
			TraceHardwareCursor::RenewSuppression(PC,
				bOverlayInFront ? TEXT("pause / settings overlay") : TEXT("character select"));
		}
	}

	if (!bOpen)
	{
		return;
	}

	// A request that never came back must not lock the screen. Reliable RPCs do not get dropped, but
	// a server that travelled mid-request, or a listen server that lost its game mode, would leave
	// this pending forever — and an unresponsive select screen with a running auto-pick timer is the
	// worst combination this feature can produce.
	if (PendingRequest != TraceCharacterRoster::NoneId && (Now - PendingRequestTime) > PendingRequestTimeout)
	{
		PendingRequest = TraceCharacterRoster::NoneId;
	}

	if (bInputAllowed && PC != nullptr && GFrameCounter >= IgnoreInputBeforeFrame)
	{
		PollInput(PC, LocalState);
	}

#if !UE_BUILD_SHIPPING
	// Consumed here rather than acted on inside the console command, so the scripted pick lands on a
	// frame where the screen is genuinely up and the rects are genuinely drawn.
	if (GTraceCharacterSelectDebugPick != 0)
	{
		const int32 RequestedId = GTraceCharacterSelectDebugPick;
		GTraceCharacterSelectDebugPick = 0;
		DebugPick(RequestedId);
		ConfirmHighlighted(LocalState);
	}

	// Trace.Characters.Highlight's latch, applied AFTER PollInput and therefore after the mouse branch
	// that would otherwise overwrite it every tick (see the note on TraceCharSelectHighlightLatch).
	// It highlights and does not confirm, which is the whole difference from Trace.Characters.Select:
	// the screen stays up and the detail panel below the grid can be photographed.
	if (TraceCharSelectHighlightLatch::Held != 0)
	{
		const int32 Index = TraceCharSelectHighlightLatch::Held - 1;
		if (TraceCharacterRoster::All().IsValidIndex(Index))
		{
			Highlighted = Index;
		}
	}

	// Spec v15 §4. Armed the same way, and for the same reason: the cards have to have been DRAWN
	// before a cursor can be parked on one.
	if (GTraceCharacterSelectClickTest != 0 && ClickTestCard == INDEX_NONE)
	{
		ClickTestCard = FMath::Clamp(GTraceCharacterSelectClickTest, 1, TraceCharacterRoster::Count) - 1;
		GTraceCharacterSelectClickTest = 0;
		ClickTestStage = 0;
		ClickTestClicks = 0;
		ClickTestNextFrame = GFrameCounter;
	}
	if (ClickTestCard != INDEX_NONE)
	{
		ClickTestStep(PC, LocalState);
	}
#endif

	Draw(HUD, LocalState);
}

void FTraceCharacterSelect::PollInput(APlayerController* PC, ATracePlayerState* LocalState)
{
	// ---- Direct number keys. One key per card — no walking required. -----------------------------
	for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
	{
		if (PC->WasInputKeyJustPressed(TraceCharacterSelectFile::NumberKeyForIndex(Index)))
		{
			Highlighted = Index;
			ConfirmHighlighted(LocalState);
			return;
		}
	}

	// ---- Left / right / up / down, with repeat ---------------------------------------------------
	//
	// UP AND DOWN ARE NEW IN SPEC v18 §2 and they exist because the cards do. With one row they moved
	// nothing and were not offered; with two, a player who wants the card directly below theirs would
	// otherwise have to walk the whole rest of the top row to reach it. Left/right still walk the
	// roster in reading order and still step from the end of one row to the start of the next, which
	// is what makes "press right four times from ROCCO" land where the numbers say it should.
	const bool bLeft  = PC->IsInputKeyDown(EKeys::Left)  || PC->IsInputKeyDown(EKeys::A);
	const bool bRight = PC->IsInputKeyDown(EKeys::Right) || PC->IsInputKeyDown(EKeys::D);
	// bNavUp / bNavDown rather than bUp / bDown: `bDown` is already the LEFT MOUSE BUTTON further down
	// this same function, and clang caught the collision as a redefinition. Worth the ugly prefix —
	// had the mouse's line come FIRST this would have been a shadow in a nested scope instead, which
	// MSVC reports as C4458/C4459 and this platform structurally cannot see.
	const bool bNavUp   = PC->IsInputKeyDown(EKeys::Up)   || PC->IsInputKeyDown(EKeys::W);
	const bool bNavDown = PC->IsInputKeyDown(EKeys::Down) || PC->IsInputKeyDown(EKeys::S);

	// One repeat clock for both axes, and horizontal wins a diagonal. Two independent clocks would
	// let a player holding right-and-down travel twice as fast as one holding either.
	int32 NavDir = (bRight ? 1 : 0) - (bLeft ? 1 : 0);
	if (NavDir == 0)
	{
		NavDir = ((bNavDown ? 1 : 0) - (bNavUp ? 1 : 0)) * TraceSelectGrid::Columns;
	}

	if (NavDir != 0)
	{
		if (NavDir != LastNavDir)
		{
			LastNavDir = NavDir;
			NextNavTime = Now + NavRepeatDelay;
			MoveHighlight(NavDir);
		}
		else if (Now >= NextNavTime)
		{
			NextNavTime = Now + NavRepeatInterval;
			MoveHighlight(NavDir);
		}
	}
	else
	{
		LastNavDir = 0;
	}

	// ---- Commit ---------------------------------------------------------------------------------
	if (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::SpaceBar))
	{
		ConfirmHighlighted(LocalState);
		return;
	}

	// ---- Mouse ----------------------------------------------------------------------------------
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (PC->GetMousePosition(MouseX, MouseY))
	{
		CursorPos = FVector2D(MouseX, MouseY);
		bHasCursor = true;
	}

	const bool bDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);
	const bool bJustReleased = !bDown && bMouseWasDown;
	bMouseWasDown = bDown;

	if (!bHasCursor)
	{
		return;
	}

	HoveredCard = INDEX_NONE;
	for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
	{
		if (CardRects[Index].bIsValid && CardRects[Index].IsInside(CursorPos))
		{
			HoveredCard = Index;
			Highlighted = Index;
			break;
		}
	}

	// ACTIVATION ON RELEASE, matching the options overlay: a press that started on one card and
	// finished on another is a slip, not a choice, and this is a decision the player lives with for
	// the whole match.
	if (bJustReleased && HoveredCard != INDEX_NONE)
	{
		ConfirmHighlighted(LocalState);
	}
}

void FTraceCharacterSelect::MoveHighlight(int32 Delta)
{
	// CLAMPED, NOT WRAPPED. Wrapping from the last card back to the first makes a held key cycle
	// forever and makes the highlight's position uninformative about where the ends are. Same call
	// the options overlay makes for the same reason.
	//
	// One clamp covers both axes because Delta is already an index step: +/-1 walks the roster and
	// +/-Columns is a row. Down from the bottom row lands on the last card rather than doing nothing,
	// which is the behaviour a player reads as "that is the end" instead of "that key is broken".
	Highlighted = FMath::Clamp(Highlighted + Delta, 0, TraceCharacterRoster::Count - 1);
}

void FTraceCharacterSelect::ConfirmHighlighted(ATracePlayerState* LocalState)
{
	if (LocalState == nullptr || !TraceCharacterRoster::All().IsValidIndex(Highlighted))
	{
		return;
	}

	const uint8 RequestedId = TraceCharacterRoster::All()[Highlighted].Id;

	// One request in flight at a time. Without this a held Enter sends one per frame, and each one is
	// a reliable RPC the server must process — the second onwards would all come back AlreadyLocked
	// and the screen would end on a refusal message for a pick that actually succeeded.
	if (PendingRequest != TraceCharacterRoster::NoneId)
	{
		return;
	}

	// A card we believe a team-mate holds is not sent. This is the ONLY place local belief is allowed
	// to stop anything, and it is a courtesy rather than a rule: it saves a round trip in the common
	// case. If the belief is stale the server refuses, which is the path that actually enforces §3.
	if (const ATracePlayerState* Holder = FindTeammateHolding(LocalState, RequestedId))
	{
		LocalState->LastPickResult = ETraceCharacterPickResult::TakenByTeammate;
		LocalState->LastPickResultCharacter = RequestedId;
		LocalState->LastPickResultLocalTime = Now;

		UE_LOG(LogTraceGame, Verbose, TEXT("[CharSelect] %s is held by team-mate '%s'; not sending."),
			*TraceCharacterRoster::NameFor(RequestedId), *TraceCharacterSelectFile::SafePlayerName(Holder));
		return;
	}

	PendingRequest = RequestedId;
	PendingRequestTime = Now;

	UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Requesting %s."), *TraceCharacterRoster::NameFor(RequestedId));

	// THE ONE CALL OFF THIS SCREEN. Everything about whether it succeeds happens on the server.
	LocalState->ServerRequestCharacter(RequestedId);
}

const ATracePlayerState* FTraceCharacterSelect::FindTeammateHolding(const ATracePlayerState* LocalState, uint8 CharacterId) const
{
	if (LocalState == nullptr || LocalState->Team == ETraceTeam::None || !TraceCharacterRoster::IsValidId(CharacterId))
	{
		return nullptr;
	}

	const UWorld* const ThisWorld = LocalState->GetWorld();
	const AGameStateBase* const BaseGameState = (ThisWorld != nullptr) ? ThisWorld->GetGameState() : nullptr;
	if (BaseGameState == nullptr)
	{
		return nullptr;
	}

	for (APlayerState* const EachState : BaseGameState->PlayerArray)
	{
		const ATracePlayerState* Candidate = Cast<ATracePlayerState>(EachState);
		if (Candidate == nullptr || Candidate == LocalState)
		{
			continue;
		}

		// Enemies are skipped: mirroring the pick is legal (§3), so an enemy holding a character
		// blocks nothing.
		if (Candidate->Team != LocalState->Team)
		{
			continue;
		}

		// *** BOTS DO NOT BLOCK A PERSON ANY MORE — SPEC v28 §9b(b). ***
		//
		// The history in three lines, because this test has now been all three ways round. Spec v14
		// §3 made bots permanently characterless, so it skipped them. Spec v15 §2 gave bots
		// characters, so it stopped skipping them and a bot team-mate holding MACE greyed MACE out
		// exactly as a human team-mate would. Spec v28 §9b makes a bot's hold YIELD to a human, so it
		// skips them again — and this time the reason is not that the bot holds nothing, it is that
		// what the bot holds is available to you.
		//
		// THE CASE THIS DECIDES IS THE OWNER'S. Verbatim: "it should be all the players first before
		// the bots". A player whose client finished loading after the whistle finds the bots already
		// filled; without this, every character a computer team-mate took would draw greyed with
		// "TAKEN BY BOT BLUE 3" and this courtesy check would refuse to even send the request. The
		// server would happily have preempted the bot — ATraceGameMode::RequestCharacter and
		// UTraceAbilityComponent::ServerSetCharacter both do — so the rule would have been correct,
		// enforced, and completely unreachable from the only screen that can ask for it.
		//
		// STILL A COURTESY, NOT A RULE. Everything this function is allowed to do is save a round
		// trip; the server is what enforces §3's uniqueness and §9b's yield, and it refuses (or
		// preempts) whatever local belief happens to be stale. Being wrong in THIS direction costs a
		// refused RPC. Being wrong in the other direction cost the player their hero.
		//
		// Human team-mates are untouched: they hold what they locked in, and §3's "first request
		// wins, the loser is told and re-picks" is exactly as it was.
		//
		// ASKED, NOT ASSUMED — ATracePlayerState::DoBotsYieldToHumans is true in every shipped build
		// and false only under §9b's red arm, so a run launched with -TraceNoBotPreempt draws this
		// screen exactly as the pre-v28 build drew it. That is what makes the red capture from a real
		// second client a comparison rather than an assertion.
		if (Candidate->IsABot() && ATracePlayerState::DoBotsYieldToHumans())
		{
			continue;
		}

		if (Candidate->GetSelectedCharacter() == CharacterId)
		{
			return Candidate;
		}
	}

	return nullptr;
}

#if !UE_BUILD_SHIPPING
void FTraceCharacterSelect::ReportClickTest(ATracePlayerState* LocalState)
{
	if (ClickTestCard == INDEX_NONE)
	{
		return;
	}

	const uint8 Picked = (LocalState != nullptr) ? LocalState->GetSelectedCharacter() : TraceCharacterRoster::NoneId;
	const bool bPicked = TraceCharacterRoster::IsValidId(Picked);

	if (bPicked && ClickTestClicks == 1)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect.ClickTest] VERDICT: ONE PRESS = ONE ACTION. Card %d locked in %s on click %d."),
			ClickTestCard + 1, *TraceCharacterRoster::NameFor(Picked), ClickTestClicks);
	}
	else if (bPicked)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[CharSelect.ClickTest] VERDICT: A CARD NEEDS MORE THAN ONE CLICK. Card %d locked in %s only on click %d."),
			ClickTestCard + 1, *TraceCharacterRoster::NameFor(Picked), ClickTestClicks);
	}
	else
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[CharSelect.ClickTest] VERDICT: card %d picked NOTHING after %d complete click(s)."),
			ClickTestCard + 1, ClickTestClicks);
	}

	ClickTestCard = INDEX_NONE;
}

void FTraceCharacterSelect::ClickTestStep(APlayerController* PC, ATracePlayerState* LocalState)
{
	if (PC == nullptr || LocalState == nullptr || !CardRects[ClickTestCard].bIsValid)
	{
		return;
	}
	if (GFrameCounter < ClickTestNextFrame)
	{
		return;
	}
	ClickTestNextFrame = GFrameCounter + TraceCharSelectClickTest::FramesPerStage;

	switch (ClickTestStage)
	{
	case 0:
	{
		// SetMouseLocation writes the viewport's cached cursor position, which is the exact value
		// PollInput reads back out of GetMousePosition above.
		const FVector2D Point = CardRects[ClickTestCard].GetCenter();
		PC->SetMouseLocation(FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y));

		float ReadX = 0.f;
		float ReadY = 0.f;
		const bool bReadBack = PC->GetMousePosition(ReadX, ReadY);
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect.ClickTest] card %d: cursor -> (%.0f, %.0f); the screen reads it back as (%.0f, %.0f) valid=%d."),
			ClickTestCard + 1, Point.X, Point.Y, ReadX, ReadY, bReadBack ? 1 : 0);

		if (!bReadBack)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CharSelect.ClickTest] No cursor position in this run, so no click can be aimed. "
				     "That is a harness failure, not a screen failure."));
			ClickTestCard = INDEX_NONE;
			return;
		}
		ClickTestStage = 1;
		return;
	}

	case 1:
		UE_LOG(LogTraceGame, Display, TEXT("[CharSelect.ClickTest] card %d, click %d: pressing."),
			ClickTestCard + 1, ClickTestClicks + 1);
		TraceCharSelectClickTest::InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/true);
		ClickTestStage = 2;
		return;

	case 2:
		TraceCharSelectClickTest::InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/false);
		// Counted HERE, on the release, not at the judging stage below. A successful click closes the
		// screen, which stops Tick calling this function at all — so if the count only moved at
		// judging time the run that PASSES would be the one that never printed a verdict. Measured
		// exactly that way once: "click 1: pressing" then "Requesting OYSTER" and then silence.
		++ClickTestClicks;
		ClickTestStage = 3;
		return;

	default:
		break;
	}

	// JUDGE, a stage after the release. APlayerController::InputKey queues the event for the next
	// input pass and this screen then polls IsInputKeyDown from Tick, so the release needs a pass and
	// a Tick before it can have had any effect. Judging on the release call itself reports every
	// surface as needing one extra click — which is the reported bug, manufactured by the harness.
	//
	// The server's answer, not our own request: PendingRequest is cleared as soon as the reply lands,
	// so a test that watched it would race. A granted pick is the only thing a player would call
	// "the click worked".
	const bool bPicked = TraceCharacterRoster::IsValidId(LocalState->GetSelectedCharacter());

	if (bPicked)
	{
		ReportClickTest(LocalState);
		return;
	}

	if (ClickTestClicks >= TraceCharSelectClickTest::MaxPairs)
	{
		ReportClickTest(LocalState);
		return;
	}

	// Same card, same cursor, another complete click. Back to the press rather than the cursor move:
	// re-parking every attempt would hide a bug that only bites the FIRST click.
	ClickTestStage = 1;
}

void FTraceCharacterSelect::DebugPick(int32 CharacterId)
{
	if (!TraceCharacterRoster::IsValidId(static_cast<uint8>(CharacterId)))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[CharSelect] Trace.Characters.Select: %d is not %d..%d."),
			CharacterId, static_cast<int32>(TraceCharacterRoster::FirstId),
			static_cast<int32>(TraceCharacterRoster::LastId));
		return;
	}

	Highlighted = CharacterId - TraceCharacterRoster::FirstId;
}
#endif

// =============================================================================================
// Draw — SPEC v20 §6: a LAYOUT AND LEGIBILITY REDESIGN, not a reskin
// =============================================================================================
//
// The user's words were "generally clean up the character selection, right now it is sloppy", and §6
// turns that into eight measured defects. Where each one is answered:
//
//   1. Huge, uneven dead space          The prose moved OFF the cards into one detail panel. A tile
//                                       now holds name + key + accent + ability name, which is the
//                                       same amount of content for every character, so a fixed box is
//                                       an honest box. Rocco's card cannot be half empty while
//                                       Mortimer's overflows, because neither card holds a paragraph.
//   2. A wall of tiny all-caps body     A four-step type ramp (display / name / body / micro-label)
//                                       set in the menu typeface, and every paragraph sentence-cased.
//   3. The gameplay HUD shows through   The backdrop is OPAQUE. See TraceSelectStyle::Backdrop.
//   4. No identity beyond a 3 px stripe Accent-coloured name, a keycap, the artist's plate and — since
//                                       UI plan WP10 — THE CHARACTER'S OWN PORTRAIT, per tile and again
//                                       as a 2x crop in the detail panel. The monogram watermark that
//                                       used to be the whole answer here is now the fallback for a
//                                       build whose portraits are missing.
//   5. Cramped ability lines            ACTIVATED is a labelled section: the key in a cap, the name at
//                                       lead size, the cooldown in its own chip. Not one hyphenated run.
//   6. "0" reads as an ordinal          Every key is drawn inside a KEYCAP, so 0 reads as a key.
//   7. None of the artist's art         Button plates, the value chip and the cursor, 9-sliced.
//   8. Undersized header, weak timer    A 42 px tracked title, and the countdown is a chip plus a
//                                       depleting bar that turns red and pulses under five seconds.
//
// *** NOTHING BELOW TOUCHES BEHAVIOUR, WHICH §6 PUTS OUT OF SCOPE. *** The rects left in CardRects are
// still exactly the hit rects; the grid is still TraceSelectGrid's Rows x Columns, which is what
// PollInput steps the highlight by; and the picking rules, the bot fill, the human wait, the auto-pick
// duration and the 1-9/0 + arrows + Enter + click model are all upstream of this function.

void FTraceCharacterSelect::Draw(AHUD* HUD, ATracePlayerState* LocalState)
{
	if (LocalState == nullptr)
	{
		return;
	}

	const float S = UIScale;
	const FLinearColor TeamTint = TraceTeamColor(LocalState->Team);
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Roster = TraceCharacterRoster::All();

	// WP10: hand this frame its portrait load budget before anything asks for one. See the note above
	// namespace TraceCharacterSelectPortraits for why a budget exists at all.
	TraceCharacterSelectPortraits::BeginFrame();

	// ---- The backdrop ----------------------------------------------------------------------------
	//
	// Opaque, and that is spec v20 §6.3 in one line: ATraceHUD::DrawHUD has already drawn the ammo, the
	// health bar, the dash and weapon meters, the scoreboard, the HOSTING panel and the crosshair by the
	// time it ticks this screen, so one opaque rectangle is what makes a modal modal.
	HUD->DrawRect(TraceSelectStyle::Backdrop, 0.f, 0.f, ViewW, ViewH);

	// A team-coloured wash under the header and a matching one on the floor. Ten bands is what a
	// gradient is on Canvas; it costs twenty rectangles and stops the screen reading as a void.
	{
		constexpr int32 Bands = 10;
		const float WashH = 320.f * S;
		for (int32 Band = 0; Band < Bands; ++Band)
		{
			const float T = static_cast<float>(Band) / static_cast<float>(Bands);
			const float BandY = WashH * T;
			const float BandH = (WashH / Bands) + 1.f;

			HUD->DrawRect(TraceSelectStyle::WithAlpha(TeamTint, 0.055f * (1.f - T)), 0.f, BandY, ViewW, BandH);
			HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Plate, 0.32f * (1.f - T)),
				0.f, ViewH - BandY - BandH, ViewW, BandH);
		}
	}

	const float Margin = TraceSelectLayout::Margin * S;
	const float InnerW = ViewW - (2.f * Margin);
	const float CenterX = ViewW * 0.5f;

	// ---- Header ----------------------------------------------------------------------------------
	//
	// §6.8: "the header is undersized for a full-screen modal". 42 design pixels with 7 of tracking, so
	// it carries the top of the screen the way the title menu's wordmark carries its own.
	//
	// THE FLANKING CHIPS ARE MEASURED BEFORE THE TITLE IS DRAWN, and that ordering is the whole reason
	// the header survives a narrow viewport. The title is centred and the chips are pinned to the two
	// margins, so at 4:3 the title's right edge and "AUTO-PICK IN" end up about ten pixels apart — and
	// at anything narrower they would simply overlap, which is precisely the class of defect §6 exists
	// to remove. Measuring first lets the title stand down instead.
	const float SelectRemaining = LocalState->GetCharacterSelectTimeRemaining();
	const bool bHasDeadline = (LocalState->CharacterSelectDeadlineServerTime > 0.f);
	const bool bUrgent = bHasDeadline && (SelectRemaining <= 5.f);

	const float ChipH = 40.f * S;
	const FString TeamLine = FString::Printf(TEXT("%s TEAM"),
		*TraceTeamName(LocalState->Team).ToString().ToUpper());
	const FString CountText = FString::Printf(TEXT("%d"), FMath::Max(0, FMath::CeilToInt(SelectRemaining)));
	const FString CountLabel(TEXT("AUTO-PICK IN"));

	const float TeamChipW = TraceCharacterSelectFile::ChipWidth(HUD, TeamLine, ChipH, 0.f,
		TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);

	const float CountChipW = bHasDeadline
		? TraceCharacterSelectFile::ChipWidth(HUD, CountText, ChipH, 64.f * S, 26.f * S, 0.f)
		: 0.f;
	const float CountBlockW = bHasDeadline
		? CountChipW + (14.f * S) + TraceCharacterSelectType::Width(HUD, CountLabel, nullptr,
			TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S)
		: 0.f;

	float TitleSize = TraceSelectLayout::TitleSize * S;
	float TitleTrack = TraceSelectLayout::TitleTrack * S;
	{
		const FString Title(TEXT("SELECT YOUR CHARACTER"));

		// The title is centred, so the space it may occupy is symmetrical about the centre and bounded
		// by whichever flank is wider.
		const float Flank = FMath::Max(TeamChipW, CountBlockW);
		const float Allowed = FMath::Max(80.f * S, InnerW - (Flank + 26.f * S) * 2.f);

		const float Natural = TraceCharacterSelectType::Width(HUD, Title, nullptr, TitleSize, TitleTrack);
		if (Natural > Allowed)
		{
			// Floored at 62%: below that the header stops being a header, and at that point the honest
			// answer is that the viewport is too narrow for this composition rather than that the type
			// should keep shrinking.
			const float Fit = FMath::Max(0.62f, Allowed / Natural);
			TitleSize *= Fit;
			TitleTrack *= Fit;
		}

		TraceCharacterSelectType::DrawCentered(HUD, Title, TraceSelectStyle::Ink, CenterX,
			TraceSelectLayout::HeaderTop * S, nullptr, TitleSize, TitleTrack);
	}

	const float TitleH = TraceCharacterSelectType::LineHeight(HUD, nullptr, TitleSize);
	const float ChipY = TraceSelectLayout::HeaderTop * S + (TitleH - ChipH) * 0.5f;
	const float LabelH = TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeLabel * S);

	// The team, in the team's colour, in a chip on the left. It is the most load-bearing fact up here:
	// per-team uniqueness means the greyed cards only make sense once you know which side you are on.
	TraceCharacterSelectFile::DrawChip(HUD, TeamLine, Margin, ChipY, ChipH, TeamChipW,
		TeamTint, TraceSelectStyle::Ink,
		TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);

	// ---- The auto-pick countdown -----------------------------------------------------------------
	//
	// §6.8: "the timer (AUTO-PICK IN 25) is a plain line rather than a clear countdown". It is now a
	// number in a chip, right-aligned beside the title, plus a bar across the whole header that empties.
	// A timeout the player cannot see is indistinguishable from the game choosing at random for them.
	if (bHasDeadline)
	{
		TraceCharacterSelectFile::SelectSpanSeconds =
			FMath::Max(TraceCharacterSelectFile::SelectSpanSeconds, SelectRemaining);

		const float Pulse = 0.72f + 0.28f * FMath::Sin(Now * 9.f);
		const FLinearColor CountColor = bUrgent
			? TraceSelectStyle::WithAlpha(TraceSelectStyle::Danger, Pulse)
			: TraceSelectStyle::Cyan;

		TraceCharacterSelectFile::DrawChip(HUD, CountText, ViewW - Margin - CountChipW, ChipY, ChipH,
			CountChipW, bUrgent ? TraceSelectStyle::Danger : TraceSelectStyle::Cyan, CountColor,
			26.f * S, 0.f);

		TraceCharacterSelectType::DrawRight(HUD, CountLabel, TraceSelectStyle::InkDim,
			ViewW - Margin - CountChipW - (14.f * S), ChipY + (ChipH - LabelH) * 0.5f,
			nullptr, TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);
	}

	// The rule, then the bar that empties along it.
	HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::InkDim, 0.30f),
		Margin, TraceSelectLayout::RuleY * S, InnerW, FMath::Max(1.f, 1.f * S));

	if (bHasDeadline && TraceCharacterSelectFile::SelectSpanSeconds > 0.5f)
	{
		const float BarY = TraceSelectLayout::BarY * S;
		const float BarH = FMath::Max(2.f, TraceSelectLayout::BarH * S);
		const float Fraction = FMath::Clamp(SelectRemaining / TraceCharacterSelectFile::SelectSpanSeconds, 0.f, 1.f);

		HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Plate, 0.85f), Margin, BarY, InnerW, BarH);
		HUD->DrawRect(
			bUrgent ? TraceSelectStyle::WithAlpha(TraceSelectStyle::Danger, 0.70f + 0.30f * FMath::Sin(Now * 9.f))
			        : TraceSelectStyle::WithAlpha(TraceSelectStyle::Cyan, 0.85f),
			Margin, BarY, InnerW * Fraction, BarH);
	}

	// The rule of the screen, in sentence case and at micro-copy size. It is guidance, not a heading,
	// and setting it as one was part of what made the old screen read as an undifferentiated wall.
	TraceCharacterSelectType::DrawCentered(HUD,
		TEXT("Nobody on your team may take the same character. The enemy may mirror your pick."),
		TraceSelectStyle::InkDim, CenterX, TraceSelectLayout::SubY * S, nullptr,
		TraceSelectLayout::SizeBody * 0.88f * S, 0.f);

	// ---- The ten identity tiles ------------------------------------------------------------------
	const float TileGapX = TraceSelectLayout::TileGapX * S;
	const float TileGapY = TraceSelectLayout::TileGapY * S;
	const float TileH = TraceSelectLayout::TileH * S;
	const float GridY = TraceSelectLayout::GridTop * S;
	const float TileW = (InnerW - (TileGapX * (TraceSelectGrid::Columns - 1)))
		/ static_cast<float>(TraceSelectGrid::Columns);

	// The grid's uniform picture and type sizes, solved once for all ten cards before any of them is
	// drawn. See TraceCharacterSelectFile::SolveTileMetrics.
	TraceCharacterSelectFile::SolveTileMetrics(HUD, TileW, TileH, S);

	// One line, once per process. A screenshot cannot tell "the redesign is not compiled in" from "it
	// is compiled in and computed these numbers", and this screen has already cost one round trip to
	// exactly that ambiguity. Now it also says which typeface actually reached the pixels, because the
	// menu face failing to resolve is invisible in a still and changes every measurement here.
	//
	// *** ONCE PER PROCESS, AND AGAIN WHENEVER THE ANSWER ACTUALLY CHANGES. *** "Once" was the right
	// rule when nothing could change the answer mid-run. The two red arms this file now carries
	// (Trace.Characters.FitMicroRow, Trace.Characters.DetailCropTop) exist precisely to change it
	// inside one process, so a log that only ever described the first arm would make the second arm —
	// the one that is supposed to be the FIX — unprovable from anything but a picture. The signature
	// covers exactly the numbers on the line that a command can move, so a steady run still logs once.
	const uint32 LayoutSignature = GetTypeHash(FString::Printf(TEXT("%.0f|%.0f|%.4f|%.4f|%.4f"),
		ViewW, ViewH, S,
		TraceCharacterSelectFile::TileMetrics.MicroScale,
		TraceCharacterSelectPortraits::DetailMinV()));

	if (!bLoggedLayoutOnce || LayoutSignature != TraceCharacterSelectFile::LastLoggedLayoutSignature)
	{
		bLoggedLayoutOnce = true;
		TraceCharacterSelectFile::LastLoggedLayoutSignature = LayoutSignature;
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect] Layout v20+WP10: view %.0fx%.0f scale %.3f (scale/width %.3e — the number "
			     "that decides the micro row; 5.21e-4 is 1920x1080) | %d tiles as %d row(s) x %d col(s) | "
			     "tile %.0fx%.0f at y=%.0f | card portrait %.0f square, text column %.0f, name %.1fpx, "
			     "ability %.1fpx (uniform across the grid) | micro row fitted x%.3f -> label %.1fpx, "
			     "chip %.1fpx tall, row %.0fpx of %.0fpx column | detail crop v %.3f..%.3f | "
			     "detail panel %.0fx%.0f at y=%.0f | face=%s"),
			ViewW, ViewH, S, S / FMath::Max(ViewW, 1.f),
			TraceCharacterRoster::Count, TraceSelectGrid::Rows, TraceSelectGrid::Columns,
			TileW, TileH, GridY,
			TraceCharacterSelectFile::TileMetrics.PortraitSide, TraceCharacterSelectFile::TileMetrics.TextW,
			TraceCharacterSelectFile::TileMetrics.NameSize, TraceCharacterSelectFile::TileMetrics.LeadSize,
			TraceCharacterSelectFile::TileMetrics.MicroScale, TraceCharacterSelectFile::TileMetrics.MicroSize,
			TraceCharacterSelectFile::TileMetrics.ChipH, TraceCharacterSelectFile::TileMetrics.MicroRowW,
			TraceCharacterSelectFile::TileMetrics.TextW,
			TraceCharacterSelectPortraits::DetailMinV(), TraceCharacterSelectPortraits::DetailMaxV(),
			InnerW, TraceSelectLayout::DetailH * S, TraceSelectLayout::DetailTop * S,
			TraceCharacterSelectType::HaveFace() ? TEXT("menu typeface") : TEXT("ENGINE BITMAP FALLBACK"));
	}

	// TWO PASSES, AND THE SELECTED TILE IS DRAWN LAST. The artist's hover plate carries an amber ring
	// that lives OUTSIDE the plate rectangle, so drawing the tiles in index order would let tile N+1
	// paint over the right-hand side of a selected tile N's ring. Nothing about the rects changes.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
		{
			const bool bSelected = (Index == Highlighted);
			if ((Pass == 0) == bSelected)
			{
				continue;
			}

			const int32 Row = Index / TraceSelectGrid::Columns;
			const int32 Column = Index % TraceSelectGrid::Columns;

			// A SHORT LAST ROW IS CENTRED, not left-aligned. With ten characters both rows are full and
			// this is a no-op; with nine, hanging four cards off the left edge under five would read as a
			// card having failed to draw rather than as the roster being odd.
			const int32 TilesInRow = FMath::Min(TraceSelectGrid::Columns,
				TraceCharacterRoster::Count - Row * TraceSelectGrid::Columns);
			const float RowW = TilesInRow * TileW + (TilesInRow - 1) * TileGapX;
			const float RowX = Margin + FMath::Max(0.f, (InnerW - RowW) * 0.5f);

			DrawCard(HUD, LocalState, Index,
				RowX + Column * (TileW + TileGapX), GridY + Row * (TileH + TileGapY), TileW, TileH);
		}
	}

	// After the tiles, because it will not print until every card has asked for its portrait at least
	// once — which the two-per-frame load budget makes take five frames.
	TraceCharacterSelectPortraits::LogInventoryOnce();

	// ---- The detail panel ------------------------------------------------------------------------
	//
	// THIS IS WHERE §6.1 IS ACTUALLY FIXED. The old screen printed all three ability paragraphs on all
	// ten cards at once — roughly 1,900 words of upper-case text on one screen — and then had to give
	// every card the height of the longest one. Showing ONE character's prose at a time costs nothing a
	// player can use (they can only read one at a time anyway), buys back two thirds of the screen, and
	// lets the body size go from ~9 px to 17 design px with a real line gap.
	if (Roster.IsValidIndex(Highlighted))
	{
		const TraceCharacterRoster::FTraceCharacterEntry& Entry = Roster[Highlighted];
		const ATracePlayerState* const Holder = FindTeammateHolding(LocalState, Entry.Id);
		const bool bTaken = (Holder != nullptr);

		const float PanelX = Margin;
		const float PanelY = TraceSelectLayout::DetailTop * S;
		const float PanelH = TraceSelectLayout::DetailH * S;
		const float Pad = TraceSelectLayout::DetailPad * S;

		HUD->DrawRect(TraceSelectStyle::PanelFill, PanelX, PanelY, InnerW, PanelH);
		TraceCharacterSelectFile::StrokeRect(HUD, PanelX, PanelY, InnerW, PanelH, 1.f * S,
			TraceSelectStyle::WithAlpha(Entry.Accent, 0.34f));

		// The character's colour as a full-height bar down the leading edge — the same signal the tile
		// gives, at the scale of the panel, so "this text belongs to that tile" needs no thinking about.
		HUD->DrawRect(TraceSelectStyle::WithAlpha(Entry.Accent, bTaken ? 0.35f : 0.95f),
			PanelX, PanelY, 5.f * S, PanelH);

		const float IdentityW = FMath::Min(TraceSelectLayout::IdentityW * S, InnerW * 0.27f);
		const float IdentityX = PanelX + Pad;
		float IdentityY = PanelY + Pad;

		// ---- THE 2x DETAIL CROP, AND THE KEY BESIDE IT — UI plan WP10 §3 -------------------------
		//
		// W5-UIQA photographed what used to be here: "the detail panel's left third is a blank column
		// under the name". The picture goes at the TOP of the column, where a portrait belongs, and
		// the PRESS row moves alongside it instead of above it — which is what makes room for a
		// picture worth looking at rather than for whatever was left at the bottom.
		//
		// THE SIZE IS SOLVED, NOT CHOSEN. It is the largest square that clears BOTH neighbours:
		//   * the PRESS label and its keycap to its right, inside the identity column's width;
		//   * the name, its rule and the status line underneath, inside the panel's inner height.
		// At the shipped 1812 x 336 panel that is about 160 px, against the ~112 a bottom-anchored
		// picture could have had. Square because the crop is square, and a portrait is the one thing
		// on this screen that must never be stretched: the busts were shot through one frozen camera
		// precisely so the set reads as one set.
		const float DetailCapH = 44.f * S;
		{
			const FString Glyph = TraceCharacterSelectFile::KeyGlyphForIndex(Highlighted);
			const float WordW = Glyph.IsEmpty() ? 0.f
				: TraceCharacterSelectType::Width(HUD, TEXT("PRESS"), nullptr,
					TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);
			const float KeyBlockW = Glyph.IsEmpty() ? 0.f : (WordW + (12.f * S) + DetailCapH);

			// What the column still owes below the picture: the display name's line box, its rule and
			// one status line, with their gaps. Measured rather than reserved as a constant, because
			// the name's size is itself fitted a few lines below.
			const float BelowH = TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeDisplay * S)
				+ (10.f * S) + (3.f * S) + (16.f * S)
				+ TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeLabel * S)
				+ (18.f * S);

			const float ByWidth = IdentityW - KeyBlockW - (16.f * S);
			const float ByHeight = (PanelY + PanelH - Pad) - IdentityY - BelowH;
			const float Side = FMath::Min(ByWidth, ByHeight);

			// Below about sixty pixels the crop stops being a face and starts being a smear; at that
			// point the honest answer is the empty column this replaced, not a bad picture. The PRESS
			// row then keeps the whole width it always had.
			if (Side >= 60.f * S)
			{
				if (UTexture2D* Portrait = TraceCharacterSelectPortraits::For(Highlighted))
				{
					TraceCharacterSelectPortraits::Draw(HUD, Portrait, IdentityX, IdentityY, Side, Side,
						TraceCharacterSelectPortraits::DetailMinU, TraceCharacterSelectPortraits::DetailMinV(),
						TraceCharacterSelectPortraits::DetailMaxU, TraceCharacterSelectPortraits::DetailMaxV(),
						bTaken ? TraceSelectStyle::Dimmed(FLinearColor::White, 0.45f) : FLinearColor::White);

					// A HAIRLINE HERE AND NOT ON THE CARD, and the asymmetry is deliberate rather than
					// an oversight. `compose_portraits.py` bakes a 2 px accent frame, corner brackets
					// and an accent underline into the edges of the 512 square; the card draws that
					// square whole and inherits all three, so a stroke there would be a second frame
					// on the artist's. This crop is the middle half of the image, so none of it is in
					// frame and the picture would otherwise bleed into the panel with no edge at all.
					TraceCharacterSelectFile::StrokeRect(HUD, IdentityX, IdentityY, Side, Side, 1.f * S,
						TraceSelectStyle::WithAlpha(Entry.Accent, bTaken ? 0.30f : 0.70f));
				}
				else
				{
					// The monogram, at panel scale. Same fallback the card takes and for the same three
					// reasons (no file / RHI not up yet / load budget spent), so there is one path to
					// test rather than two.
					const FString Monogram = FString(Entry.Name).Left(1);
					const float MonoSize = Side * 0.78f;
					const float MonoW = TraceCharacterSelectType::Width(HUD, Monogram, nullptr, MonoSize, 0.f);
					const float MonoH = TraceCharacterSelectType::LineHeight(HUD, nullptr, MonoSize);

					TraceCharacterSelectType::Draw(HUD, Monogram,
						TraceSelectStyle::WithAlpha(Entry.Accent, bTaken ? 0.10f : 0.22f),
						IdentityX + (Side - MonoW) * 0.5f, IdentityY + (Side - MonoH) * 0.5f,
						nullptr, MonoSize, 0.f);
				}
			}

			// The key that picks it, as a key, and in reading order: the word PRESS, then the cap. §6.6.
			// Beside the picture now, top-aligned with it.
			if (!Glyph.IsEmpty())
			{
				const float KeyX = (Side >= 60.f * S) ? (IdentityX + Side + (16.f * S)) : IdentityX;

				TraceCharacterSelectType::Draw(HUD, TEXT("PRESS"), TraceSelectStyle::InkDim,
					KeyX,
					IdentityY + (DetailCapH - TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeLabel * S)) * 0.5f,
					nullptr, TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);

				TraceCharacterSelectFile::DrawChip(HUD, Glyph, KeyX + WordW + (12.f * S), IdentityY,
					DetailCapH, DetailCapH, Entry.Accent, TraceSelectStyle::Ink, 22.f * S, 0.f);
			}

			IdentityY += FMath::Max(Side >= 60.f * S ? Side : 0.f, Glyph.IsEmpty() ? 0.f : DetailCapH) + (18.f * S);
		}

		// The name, at display size, in the character's own colour — SHRUNK TO FIT ITS COLUMN.
		//
		// The clamp is spec v22 §A1's doing and it is not optional. This block is the identity column
		// and the three prose columns start at IdentityX + IdentityW; the name used to be typeset in a
		// humanist face that happened to fit inside that at every roster name. Sofachrome is about a
		// third wider at the same size, and "MORTIMER" at the authored display size ran 190 px past
		// the rule and printed straight through the MOVEMENT paragraph — photographed at 1920x1080
		// before this line existed.
		//
		// Measured rather than a smaller constant: the roster is data, the next character added could
		// be "SLIMEBALL" again or something longer, and a size hand-tuned to today's longest name is a
		// bug waiting for the eleventh character. The floor stops a pathological name from shrinking
		// the display line into the body copy's size, at which point it stops being a display line.
		{
			const float NameTrack = TraceSelectLayout::TrackName * S;
			float NameSize = TraceSelectLayout::SizeDisplay * S;
			// Measured in the weight it is DRAWN in — see the note in Width(). Bold is ~1.5x the
			// light advance, so measuring light here let MORTIMER overrun the identity column.
			const float NameW = TraceCharacterSelectType::Width(HUD, Entry.Name, nullptr, NameSize, NameTrack,
				ETraceTextWeight::Bold);
			if (NameW > IdentityW && NameW > 1.f)
			{
				NameSize = FMath::Max(NameSize * (IdentityW / NameW),
					TraceSelectLayout::SizeLabel * S * 1.2f);
			}

			TraceCharacterSelectType::Draw(HUD, Entry.Name,
				bTaken ? TraceSelectStyle::Dimmed(Entry.Accent, 0.45f) : Entry.Accent,
				IdentityX, IdentityY, nullptr, NameSize, NameTrack, ETraceTextWeight::Bold);
			IdentityY += TraceCharacterSelectType::LineHeight(HUD, nullptr, NameSize) + (10.f * S);
		}

		HUD->DrawRect(TraceSelectStyle::WithAlpha(Entry.Accent, bTaken ? 0.30f : 0.80f),
			IdentityX, IdentityY, IdentityW * 0.55f, 3.f * S);
		IdentityY += (3.f * S) + (16.f * S);

		// One status line, and it is the same fact the tile shows, spelled out.
		{
			FString Status;
			FLinearColor StatusColor = TraceSelectStyle::InkDim;

			if (bTaken)
			{
				Status = FString::Printf(TEXT("TAKEN BY %s"),
					*TraceCharacterSelectFile::SafePlayerName(Holder).ToUpper());
				StatusColor = TraceSelectStyle::Danger;
			}
			else if (LocalState->GetSelectedCharacter() == Entry.Id)
			{
				Status = TEXT("LOCKED IN");
				StatusColor = TraceSelectStyle::Good;
			}
			else
			{
				Status = TEXT("PRESS ENTER TO LOCK IN");
			}

			IdentityY = DrawWrapped(HUD, Status, StatusColor, IdentityX, IdentityY, IdentityW,
				nullptr, TraceSelectLayout::SizeLabel * S, 4.f * S);
		}

		// A hairline between the identity block and the prose, so the three columns read as one group.
		HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::InkDim, 0.18f),
			IdentityX + IdentityW + (TraceSelectLayout::ColumnGap * 0.5f * S), PanelY + Pad,
			FMath::Max(1.f, 1.f * S), PanelH - Pad * 2.f);

		// ---- MOVEMENT / PASSIVE / ACTIVATED, as three columns with real headings ------------------
		const float ColumnGap = TraceSelectLayout::ColumnGap * S;
		const float ColumnsX = IdentityX + IdentityW + ColumnGap;
		const float ColumnsW = (PanelX + InnerW - Pad) - ColumnsX;
		const float ColumnW = (ColumnsW - ColumnGap * 2.f) / 3.f;
		const float ColumnsY = PanelY + Pad;
		const float BodyDim = bTaken ? 0.55f : 1.f;

		// The section heading: a micro-label in the accent, a short rule under it, then the paragraph.
		// The rule is what actually separates a heading from its body — §6.2's complaint was that
		// "MOVEMENT / PASSIVE / ACTIVATED headers barely separate from the paragraphs under them", and
		// no amount of size difference fixes that on its own when everything is the same colour.
		auto DrawHeading = [&](const TCHAR* Label, const FLinearColor& Tint, float X, float Y) -> float
		{
			TraceCharacterSelectType::Draw(HUD, Label, TraceSelectStyle::Dimmed(Tint, BodyDim), X, Y,
				nullptr, TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);

			float Below = Y + TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeLabel * S) + (7.f * S);
			HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Dimmed(Tint, BodyDim), 0.9f),
				X, Below, 30.f * S, 2.f * S);
			return Below + (2.f * S) + (13.f * S);
		};

		const FLinearColor BodyColor = TraceSelectStyle::Dimmed(TraceSelectStyle::InkSoft, BodyDim);

		// ---- ONE BODY SIZE, FITTED TO THE LONGEST OF THE THREE COLUMNS ---------------------------
		//
		// MEASURED, not guessed, and the first capture of this redesign is why: Lily's ZIP paragraph is
		// the longest string in the roster and at a flat 17 px it wrapped to seven lines and ran out of
		// the bottom of the panel and into the footer — the exact overflow §6 calls sloppy, reproduced
		// by a redesign meant to remove it. So the size steps down until the tallest of the three blocks
		// fits the room it has.
		//
		// ONE size for all three columns rather than three: type that changed size between adjacent
		// columns of the same panel would read as a rendering fault, and the whole point of §6.2 is a
		// ramp the eye can trust. The cost is that Lily's short MOVEMENT column is set at the size her
		// long ACTIVATED column needs, which is invisible.
		const FString MovementText = TraceCharacterSelectFile::SentenceCase(Entry.Movement);
		const FString PassiveText = TraceCharacterSelectFile::SentenceCase(Entry.Passive);
		const FString ActivatedText = TraceCharacterSelectFile::SentenceCase(Entry.Activated);

		const float HeadingH = TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeLabel * S)
			+ (7.f * S) + (2.f * S) + (13.f * S);
		const float AbilityRowH = (30.f * S) + (14.f * S);
		const float PanelBottom = PanelY + PanelH - Pad;

		// SPEC v25 §4 — the ability DESCRIPTIONS, and only they, are set in Erbaum Bold. Resolved ONCE
		// for the card so the fit-to-column search below and the three draws underneath it cannot
		// disagree about the face: they do not share advances, and a search that measured one face
		// while the draws used another would pick a size for a paragraph nobody is going to see.
		const ETraceTextWeight BodyWeight = TraceCharacterSelectType::DescriptionWeight();

		float BodySize = TraceSelectLayout::SizeBody * S;
		float BodyGap = 7.f * S;
		{
			static const float Steps[] = { 1.00f, 0.94f, 0.88f, 0.82f, 0.76f, 0.70f };
			for (int32 StepIndex = 0; StepIndex < UE_ARRAY_COUNT(Steps); ++StepIndex)
			{
				const float TrySize = TraceSelectLayout::SizeBody * S * Steps[StepIndex];
				const float TryGap = 7.f * S * Steps[StepIndex];
				const float TryLineH = TraceCharacterSelectType::LineHeight(HUD, nullptr, TrySize) + TryGap;

				const float PlainRoom = PanelBottom - (ColumnsY + HeadingH);
				const float ActivatedRoom = PlainRoom - AbilityRowH;

				const bool bFits =
					TraceCharacterSelectType::WrapLines(HUD, MovementText, ColumnW, nullptr, TrySize, BodyWeight) * TryLineH <= PlainRoom &&
					TraceCharacterSelectType::WrapLines(HUD, PassiveText, ColumnW, nullptr, TrySize, BodyWeight) * TryLineH <= PlainRoom &&
					TraceCharacterSelectType::WrapLines(HUD, ActivatedText, ColumnW, nullptr, TrySize, BodyWeight) * TryLineH <= ActivatedRoom;

				BodySize = TrySize;
				BodyGap = TryGap;
				if (bFits)
				{
					break;
				}
			}
		}

		{
			float Y = DrawHeading(TEXT("MOVEMENT"), TraceSelectStyle::Cyan, ColumnsX, ColumnsY);
			TraceCharacterSelectType::WrapDraw(HUD, MovementText, BodyColor, ColumnsX, Y, ColumnW,
				nullptr, BodySize, BodyGap, BodyWeight);
		}

		{
			const float X = ColumnsX + ColumnW + ColumnGap;
			float Y = DrawHeading(TEXT("PASSIVE"), TraceSelectStyle::Cyan, X, ColumnsY);
			TraceCharacterSelectType::WrapDraw(HUD, PassiveText, BodyColor, X, Y, ColumnW,
				nullptr, BodySize, BodyGap, BodyWeight);
		}

		// ---- ACTIVATED — spec v20 §6.5 -----------------------------------------------------------
		//
		// The measured defect, verbatim: "ACTIVATED [E] - RIPPLE - 20s CD runs key, name and cooldown
		// together with hyphens and no visual separation". They are three different KINDS of fact — a
		// control, a name and a duration — so they get three different objects: a keycap, a line of
		// display type in the character's colour, and a chip.
		{
			// One switch for both ability rows — the card's and this one — because they are one defect
			// wearing two hats, and an A/B that only put half of it back would prove half of the fix.
#if !UE_BUILD_SHIPPING
			const bool bFitAbilityRow = (TraceCharacterSelectFile::CVarFitMicroRow.GetValueOnGameThread() != 0);
#else
			constexpr bool bFitAbilityRow = true;
#endif

			const float X = ColumnsX + (ColumnW + ColumnGap) * 2.f;
			float Y = DrawHeading(TEXT("ACTIVATED"), Entry.Accent, X, ColumnsY);

			// ---- THE ROW IS FITTED TO ITS COLUMN, FOR THE SAME REASON THE CARD'S MICRO ROW IS ------
			//
			// Keycap, ability name and cooldown chip are all sized from UIScale — view HEIGHT — and
			// laid out inside ColumnW, which is a third of the panel and therefore view WIDTH. That is
			// the identical shape as the "ACTIVATED" overprint W8-VISUAL §4 measured on the cards, one
			// panel down, and it fails the same way: at 1440x1080 (4:3, scale/width 6.94e-4) the 25s
			// chip printed over the last two letters of SLIMEWALL. Captured before this block existed:
			// scratchpad/w9uifix/ab_4x3.png, bottom strip.
			//
			// It survived W8 because it needs BOTH a narrow display AND the roster's longest ability
			// name highlighted, and until Trace.Characters.Highlight there was no way to hold a chosen
			// character on this panel at all. It is comfortable at every shape wider than about 1.45:1
			// — measured 8.7% of the column still clear at 1728x1117 — so nothing that looks right
			// today changes: the solve returns 1.0 wherever there was room.
			//
			// Fitted against the WIDEST ability name in the roster and not this card's, so walking the
			// grid does not resize the row under the player's eyes.
			const float BaseRowH = 30.f * S;
			const float BaseChipText = TraceSelectLayout::SizeChip * S;
			const float BaseLeadSize = TraceSelectLayout::SizeLead * S;
			const float BaseLeadTrack = TraceSelectLayout::TrackName * S;
			const float BaseRowGap = 12.f * S;

			const FString Cooldown = FString::Printf(TEXT("%ds"), FMath::RoundToInt(Entry.ActivatedCooldown));

			FString WidestName = Entry.ActivatedName;
			FString WidestCd = Cooldown;
			{
				float BestName = 0.f;
				float BestCd = 0.f;
				for (const TraceCharacterRoster::FTraceCharacterEntry& Row : Roster)
				{
					const float NameW = TraceCharacterSelectType::Width(HUD, Row.ActivatedName, nullptr,
						BaseLeadSize, BaseLeadTrack);
					if (NameW > BestName) { BestName = NameW; WidestName = Row.ActivatedName; }

					const FString Cd = FString::Printf(TEXT("%ds"), FMath::RoundToInt(Row.ActivatedCooldown));
					const float CdW = TraceCharacterSelectType::Width(HUD, Cd, nullptr, BaseChipText, 0.f);
					if (CdW > BestCd) { BestCd = CdW; WidestCd = Cd; }
				}
			}

			auto AbilityRowWidthAt = [&](float Scale) -> float
			{
				const float H = BaseRowH * Scale;
				return TraceCharacterSelectFile::ChipWidth(HUD, TEXT("E"), H, H, BaseChipText * Scale, 0.f)
					+ (BaseRowGap * Scale)
					+ TraceCharacterSelectType::Width(HUD, WidestName, nullptr, BaseLeadSize * Scale, BaseLeadTrack * Scale)
					+ (BaseRowGap * Scale)
					+ TraceCharacterSelectFile::ChipWidth(HUD, WidestCd, H, 0.f, BaseChipText * Scale, 0.f);
			};

			// Iterated for the same reason SolveTileMetrics iterates: advance is not exactly linear in
			// point size and the chips add plate padding, so one first-order step can land a hair over.
			float RowScale = 1.f;
			for (int32 Pass = 0; bFitAbilityRow && Pass < 3; ++Pass)
			{
				const float Need = AbilityRowWidthAt(RowScale);
				if (Need <= ColumnW)
				{
					break;
				}
				RowScale = FMath::Max(0.05f, RowScale * (ColumnW / FMath::Max(Need, 1.f)));
			}

			const float RowH = BaseRowH * RowScale;
			const float ChipText = BaseChipText * RowScale;
			const float LeadSize = BaseLeadSize * RowScale;
			const float LeadTrack = BaseLeadTrack * RowScale;

			const float KeyW = TraceCharacterSelectFile::DrawChip(HUD, TEXT("E"), X, Y, RowH, RowH,
				TraceSelectStyle::Dimmed(Entry.Accent, BodyDim), TraceSelectStyle::Ink,
				ChipText, 0.f);

			const float CooldownW = TraceCharacterSelectFile::ChipWidth(HUD, Cooldown, RowH, 0.f, ChipText, 0.f);

			TraceCharacterSelectFile::DrawChip(HUD, Cooldown, X + ColumnW - CooldownW, Y, RowH, CooldownW,
				TraceSelectStyle::Cyan, TraceSelectStyle::InkSoft, ChipText, 0.f);

			const float NameH = TraceCharacterSelectType::LineHeight(HUD, nullptr, LeadSize);
			TraceCharacterSelectType::Draw(HUD, Entry.ActivatedName,
				TraceSelectStyle::Dimmed(Entry.Accent, BodyDim),
				X + KeyW + (BaseRowGap * RowScale), Y + (RowH - NameH) * 0.5f, nullptr,
				LeadSize, LeadTrack);

			// The paragraph below keeps the row's ORIGINAL height in its budget (AbilityRowH above is
			// 30 * S + 14 * S), so a shrunk row gives the prose a little more air rather than moving it
			// up. Deliberate: the fit-to-column search that chose BodySize ran against AbilityRowH, and
			// advancing by the shrunk height here would hand the paragraph room the search never
			// offered it — which is how a "fix" to one row silently re-opens the overflow in another.
			Y += BaseRowH + (14.f * S);
			TraceCharacterSelectType::WrapDraw(HUD, ActivatedText, BodyColor, X, Y, ColumnW,
				nullptr, BodySize, BodyGap, BodyWeight);
		}
	}

	// ---- Footer: the controls, then the last server verdict ---------------------------------------
	float FooterY = TraceSelectLayout::FooterY * S;

	{
		// Built from the roster rather than written out, so adding a character cannot leave the screen
		// telling the player about one fewer key than it accepts. The upper bound is the number ROW,
		// which holds TEN keys and not nine — 1..9 then 0 — so at the v19 §3 roster of ten this reads
		// "1-9" and "0" as two separate caps, which is also §6.6's point: 0 is a KEY, not a tenth name.
		struct FControl
		{
			FString Cap;
			FString Label;
		};

		TArray<FControl> Controls;
		if (TraceCharacterRoster::Count >= 10)
		{
			Controls.Add({ TEXT("1-9"), FString() });
			Controls.Add({ TEXT("0"), TEXT("CHOOSE") });
		}
		else
		{
			Controls.Add({ FString::Printf(TEXT("1-%d"), TraceCharacterRoster::Count), TEXT("CHOOSE") });
		}
		Controls.Add({ TEXT("ARROWS"), TEXT("MOVE") });
		Controls.Add({ TEXT("ENTER"), TEXT("LOCK IN") });
		Controls.Add({ TEXT("CLICK"), TEXT("A CARD") });

		const float CapH = 30.f * S;
		const float CapGap = 9.f * S;
		const float GroupGap = 30.f * S;

		// Measured first, then drawn, because a centred row of variable-width chips cannot be laid out
		// left to right without knowing the total.
		float TotalWidth = 0.f;
		for (int32 Index = 0; Index < Controls.Num(); ++Index)
		{
			const float CapW = FMath::Max(CapH,
				TraceCharacterSelectType::Width(HUD, Controls[Index].Cap, nullptr, TraceSelectLayout::SizeLabel * S, 1.4f * S)
				+ CapH * 0.90f);
			TotalWidth += CapW;

			if (!Controls[Index].Label.IsEmpty())
			{
				TotalWidth += CapGap + TraceCharacterSelectType::Width(HUD, Controls[Index].Label,
					nullptr, TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);
				if (Index + 1 < Controls.Num())
				{
					TotalWidth += GroupGap;
				}
			}
			else
			{
				TotalWidth += CapGap;
			}
		}

		// LabelH is the header's, and it is deliberately reused rather than re-declared: the footer's
		// labels are the same size, and shadowing it is a -Wshadow error on this toolchain and a
		// C4459 on MSVC. See the note in Trace.Build.cs about which of the two actually catches it.
		float PenX = CenterX - TotalWidth * 0.5f;

		for (int32 Index = 0; Index < Controls.Num(); ++Index)
		{
			const float CapW = TraceCharacterSelectFile::DrawChip(HUD, Controls[Index].Cap, PenX, FooterY, CapH,
				CapH, TraceSelectStyle::Cyan, TraceSelectStyle::Ink, TraceSelectLayout::SizeLabel * S, 1.4f * S);
			PenX += CapW + CapGap;

			if (!Controls[Index].Label.IsEmpty())
			{
				TraceCharacterSelectType::Draw(HUD, Controls[Index].Label, TraceSelectStyle::InkDim,
					PenX, FooterY + (CapH - LabelH) * 0.5f, nullptr,
					TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);

				PenX += TraceCharacterSelectType::Width(HUD, Controls[Index].Label, nullptr,
					TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S) + GroupGap;
			}
		}

		FooterY += CapH + (14.f * S);
	}

	// THE VERDICT LINE. Spec v14 §3's "the loser is TOLD and re-picks" is this. It reads the answer the
	// server sent back to ClientCharacterPickResult and prints it in plain words — a refusal that only
	// appeared in a log is a refusal the player never received.
	if ((Now - LocalState->LastPickResultLocalTime) < TraceSelectStyle::MessageDuration)
	{
		FString Message;
		FLinearColor MessageColor = TraceSelectStyle::Danger;

		const FString PickName = TraceCharacterRoster::NameFor(LocalState->LastPickResultCharacter);

		switch (LocalState->LastPickResult)
		{
		case ETraceCharacterPickResult::Granted:
			Message = FString::Printf(TEXT("%s LOCKED IN"), *PickName);
			MessageColor = TraceSelectStyle::Good;
			break;
		case ETraceCharacterPickResult::TakenByTeammate:
			Message = FString::Printf(TEXT("%s WAS TAKEN BY A TEAM-MATE FIRST - PICK ANOTHER"), *PickName);
			break;
		case ETraceCharacterPickResult::AlreadyLocked:
			Message = TEXT("YOU HAVE ALREADY LOCKED IN");
			break;
		case ETraceCharacterPickResult::Disabled:
			Message = TEXT("CHARACTERS ARE OFF IN THIS MATCH");
			break;
		case ETraceCharacterPickResult::NotSelecting:
			Message = TEXT("NOT PICKING RIGHT NOW");
			break;
		default:
			Message = TEXT("THAT IS NOT ONE OF THE CHARACTERS");
			break;
		}

		// Pulsed, because it may replace a message that was already there — a static line that merely
		// changed its words would be missed by a player who is looking at the cards.
		const FLinearColor Pulsed = TraceSelectStyle::WithAlpha(MessageColor, 0.7f + 0.3f * FMath::Sin(Now * 10.f));
		DrawTextCentered(HUD, Message, Pulsed, CenterX, FooterY, nullptr, TraceSelectLayout::SizeBody * S);
	}
	else if (PendingRequest != TraceCharacterRoster::NoneId)
	{
		DrawTextCentered(HUD, FString::Printf(TEXT("ASKING THE SERVER FOR %s..."),
			*TraceCharacterRoster::NameFor(PendingRequest)),
			TraceSelectStyle::InkDim, CenterX, FooterY, nullptr, TraceSelectLayout::SizeBody * S);
	}

	DrawCursor(HUD);
}

/**
 * ONE IDENTITY TILE — spec v20 §6.1 and §6.4.
 *
 * What it holds is deliberately the same for every character: a keycap, the name in the character's
 * own accent, the activated ability's name and cooldown, and the character's PORTRAIT. No paragraph,
 * no variable-length anything. That is what makes a fixed-height box honest rather than "roughly half
 * empty" (Rocco) and "nearly overflowing" (Mortimer), which is the §6.1 complaint verbatim.
 *
 * *** THE PARAGRAPH THAT USED TO BE HERE IS FALSIFIED AND IS REWRITTEN BELOW (UI plan WP10). ***
 * It read: "§6.4 asked for identity beyond a 3 px colour stripe. There are no portraits in this
 * project and inventing one is not a layout pass, so the identity is built out of what does exist
 * ... the initial as a big low-alpha watermark". That was true when it was written and stopped being
 * true when W4-PORTRAITS shot ten busts. There ARE portraits now, at
 * /Game/Trace/UI/Art/Portraits/T_Portrait_<Name>, and this tile draws one.
 *
 * WHAT THE PICTURE DID TO THE LAYOUT, because it is not simply "a texture in the empty half":
 *
 *   * The tile is now TWO COLUMNS — a text column on the left and a square portrait pinned to the
 *     right edge — where before it was two blocks pinned to the top and bottom edges with a watermark
 *     in the gap. Every width in the text half is measured against the text column, not against the
 *     tile, so nothing can run underneath the picture.
 *   * THE NAME NEVER YIELDS TO THE PORTRAIT. The portrait's width is what is left after the keycap
 *     and the smallest size the name is allowed to shrink to; on a viewport narrow enough it gives up
 *     its own width, and below 56 design px it gives up altogether and the monogram comes back. A
 *     card whose picture had pushed SLIMEBALL off its own tile would be a worse card than one with no
 *     picture, and that is the sort of thing that only shows up on somebody else's monitor.
 *   * The square is drawn WHOLE, never cropped and never stretched — see
 *     TraceCharacterSelectPortraits::CardMinU for the two reasons (the accent frame is baked into the
 *     512's edges, and the framing gate spent real headroom keeping Lily's fins and X's beads inside
 *     the frame).
 *
 * The monogram is still here. It is the fallback now rather than the answer: a clone that has not run
 * the portrait stage, a frame or two before the RHI texture lands, or a load budget already spent all
 * draw the same watermark this tile used to draw, in the square the portrait would have filled.
 */
void FTraceCharacterSelect::DrawCard(AHUD* HUD, ATracePlayerState* LocalState, int32 CardIndex, float X, float Y, float W, float H)
{
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Roster = TraceCharacterRoster::All();
	if (!Roster.IsValidIndex(CardIndex))
	{
		return;
	}

	const TraceCharacterRoster::FTraceCharacterEntry& Entry = Roster[CardIndex];

	// *** THE HIT RECT. Unchanged in meaning: PollInput hover, the click path and
	// *** Trace.Characters.ClickTest all read this and nothing else. ***
	CardRects[CardIndex] = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));

	const ATracePlayerState* Holder = FindTeammateHolding(LocalState, Entry.Id);
	const bool bTaken = (Holder != nullptr);
	const bool bSelected = (CardIndex == Highlighted);
	const float S = UIScale;

	// Dead cards are drawn, never removed. A card that vanished would move its neighbours under the
	// player's pointer and would hide the ONE thing they need to understand — that a team-mate has it.
	const float Dim = bTaken ? 0.42f : 1.f;

	// ---- The artist's plate, in the sheet's own three states -------------------------------------
	//
	// The corner is sized as if the tile were a 78-pixel menu row rather than a 196-pixel tile: see
	// DrawPlate's CornerHeight argument for why taking it from the tile's own height turns the artist's
	// rounding into a lozenge.
	{
		const TraceCharacterSelectArt::ESprite Which = bTaken
			? TraceCharacterSelectArt::ESprite::PlateDisabled
			: (bSelected ? TraceCharacterSelectArt::ESprite::PlateHover
			             : TraceCharacterSelectArt::ESprite::PlateDefault);

		if (UTexture2D* Plate = TraceCharacterSelectArt::Sprite(Which))
		{
			TraceCharacterSelectArt::DrawPlate(HUD, Plate, TraceMenuArtStyle::ButtonFrame,
				X, Y, W, H, 78.f * S, FLinearColor::White);
		}
		else
		{
			// Exactly the rectangle this screen drew before there was any art.
			HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Plate, bTaken ? 0.55f : 0.92f), X, Y, W, H);
			DrawFrame(HUD, X, Y, W, H,
				bSelected ? TraceSelectStyle::WithAlpha(Entry.Accent, 0.95f)
				          : TraceSelectStyle::WithAlpha(TraceSelectStyle::Cyan, bTaken ? 0.12f : 0.30f));
		}
	}

	// Every number below comes from the grid-wide solve in SolveTileMetrics, never from this card's
	// own name. See the long note there for why: per-card arithmetic produced ten different picture
	// sizes in one grid, which photographs as a rendering fault.
	const TraceCharacterSelectFile::FTileMetrics& M = TraceCharacterSelectFile::TileMetrics;
	const float Pad = M.Pad;
	const float TextW = M.TextW;

	// THE TILE IS TWO COLUMNS, and inside the left one two blocks pinned to opposite edges: the key
	// and name hang off the top and the ability group off the BOTTOM. That is what stops the v1
	// version of this redesign reintroducing §6.1's own complaint at a smaller scale — a top-down
	// stack left about fifty pixels of nothing under every tile, which is dead space in exactly the
	// way the old fixed-height cards were. The gap between the two blocks used to be the monogram's;
	// since WP10 the picture is a column of its own beside them and the gap is breathing room.
	const float CapH = M.CapH;
	const float LeadH = TraceCharacterSelectType::LineHeight(HUD, nullptr, M.LeadSize);
	// The micro row's four numbers come from the grid-wide solve, NOT from UIScale. That is the fix
	// for the 16:10 overprint: they are fitted to the text column's width in SolveTileMetrics, which
	// is the dimension that bounds them. Deriving them here from S again — as this line used to —
	// is precisely how a height-sized label ends up under a width-placed chip. See FTileMetrics.
	const float MicroSize = M.MicroSize;
	const float MicroH = TraceCharacterSelectType::LineHeight(HUD, nullptr, MicroSize);
	const float CooldownH = M.ChipH;

	const float BottomY = Y + H - Pad - LeadH;                                   // the ability name's line box
	const float MicroRowH = FMath::Max(MicroH, CooldownH);
	const float MicroY = BottomY - MicroRowH - (5.f * S);                        // ACTIVATED + the cooldown chip

	// ---- The portrait column — UI plan WP10 ------------------------------------------------------
	if (M.PortraitSide > 0.f)
	{
		const float PortraitSide = M.PortraitSide;
		const float PortraitX = X + W - M.PortraitInset - PortraitSide;
		const float PortraitY = Y + (H - PortraitSide) * 0.5f;

		if (UTexture2D* Portrait = TraceCharacterSelectPortraits::For(CardIndex))
		{
			// NO STROKE AROUND IT. `compose_portraits.py` bakes a 2 px accent frame, corner brackets
			// and an accent underline into the square, and this draws the square whole — a hairline
			// here would be a second frame on the artist's. (The detail panel's 2x crop cuts all three
			// off and does draw one; see the note there.)
			//
			// A taken card's portrait is dimmed by the same factor its type is. Greying the face is
			// the point: the card has to say "somebody has this" louder than "here is a character".
			TraceCharacterSelectPortraits::Draw(HUD, Portrait, PortraitX, PortraitY, PortraitSide, PortraitSide,
				TraceCharacterSelectPortraits::CardMinU, TraceCharacterSelectPortraits::CardMinV,
				TraceCharacterSelectPortraits::CardMaxU, TraceCharacterSelectPortraits::CardMaxV,
				bTaken ? TraceSelectStyle::Dimmed(FLinearColor::White, Dim) : FLinearColor::White);
		}
		else
		{
			// ---- The monogram watermark, now the FALLBACK ------------------------------------------
			//
			// Unchanged in what it is and why it works — an alpha low enough that it is texture rather
			// than text, and the cheapest per-character silhouette available without art: R and S and M
			// and X do not look alike even out of focus. What changed is where it sits. It used to hang
			// off the tile's right padding in the gap between the two text blocks; it now fills the
			// square the picture would have occupied, so a build with no portraits keeps the same
			// composition as one with them rather than falling back to a different, emptier layout.
			const FString Monogram = FString(Entry.Name).Left(1);
			const float MonoSize = PortraitSide * 0.80f;
			const float MonoW = TraceCharacterSelectType::Width(HUD, Monogram, nullptr, MonoSize, 0.f);
			const float MonoH = TraceCharacterSelectType::LineHeight(HUD, nullptr, MonoSize);

			TraceCharacterSelectType::Draw(HUD, Monogram,
				TraceSelectStyle::WithAlpha(Entry.Accent, bTaken ? 0.08f : 0.16f),
				PortraitX + (PortraitSide - MonoW) * 0.5f, PortraitY + (PortraitSide - MonoH) * 0.5f,
				nullptr, MonoSize, 0.f);
		}
	}

	// ---- Keycap, then the name under it ----------------------------------------------------------
	//
	// STACKED, not side by side, and it is the picture that pays for it — see SolveTileMetrics. It
	// also reads better this way: a number sitting on the name's line looks like a bullet, and a
	// keycap above it looks like what it is, the key that picks this card.
	float PenY = Y + Pad;
	{
		const FString Glyph = TraceCharacterSelectFile::KeyGlyphForIndex(CardIndex);
		if (!Glyph.IsEmpty())
		{
			TraceCharacterSelectFile::DrawChip(HUD, Glyph, X + Pad, PenY, CapH, CapH,
				TraceSelectStyle::Dimmed(Entry.Accent, Dim),
				TraceSelectStyle::WithAlpha(TraceSelectStyle::Ink, Dim),
				TraceSelectLayout::SizeChip * 0.92f * S, 0.f);
			PenY += CapH + (6.f * S);
		}

		const float NameH = TraceCharacterSelectType::LineHeight(HUD, nullptr, M.NameSize);
		TraceCharacterSelectType::Draw(HUD, Entry.Name,
			bTaken ? TraceSelectStyle::InkDim : Entry.Accent,
			X + Pad, PenY, nullptr, M.NameSize, M.NameTrack, ETraceTextWeight::Bold);

		PenY += NameH + (8.f * S);
	}

	// The accent rule. This is the "3 px colour stripe" from §6.4 promoted into the layout: under the
	// name, in the character's colour, and the full width of the TEXT COLUMN — it stops where the
	// picture starts rather than running underneath it, which is what makes the two columns read as
	// two columns instead of as a rule with a texture pasted over its right end.
	HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Dimmed(Entry.Accent, Dim), bTaken ? 0.35f : 0.75f),
		X + Pad, PenY, TextW, FMath::Max(1.f, 2.f * S));

	// ---- Bottom block: either the activated ability, or who has taken this card -------------------
	//
	// One or the other, in the same place, because they answer the same question — "what does this card
	// offer me" and "nothing, somebody has it". A taken card showing both would be advertising an
	// ability the player cannot have.
	if (bTaken)
	{
		const FString TakenLine = FString::Printf(TEXT("TAKEN BY %s"),
			*TraceCharacterSelectFile::SafePlayerName(Holder).ToUpper());

		const float StripH = 28.f * S;
		const float StripY = Y + H - Pad - StripH;
		const float StripTextSize = TraceSelectLayout::SizeLabel * 0.9f * S;

		HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Danger, 0.20f),
			X + Pad, StripY, TextW, StripH);
		HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Danger, 0.85f),
			X + Pad, StripY, 3.f * S, StripH);

		const float LineH = TraceCharacterSelectType::LineHeight(HUD, nullptr, StripTextSize);
		TraceCharacterSelectType::Draw(HUD, TakenLine, TraceSelectStyle::Danger,
			X + Pad + (10.f * S), StripY + (StripH - LineH) * 0.5f, nullptr, StripTextSize, 1.2f * S);
	}
	else
	{
		// ACTIVATED and the cooldown share one line, at opposite ends of the text column. The chip used
		// to sit on the ability NAME's line and against the tile's right padding — which since WP10 is
		// inside the picture, and which cost SLIMEWALL a third of its column. Both facts are still one
		// glance apart; they are simply a row higher.
		TraceCharacterSelectType::Draw(HUD, TEXT("ACTIVATED"), TraceSelectStyle::InkDim,
			X + Pad, MicroY + (MicroRowH - MicroH) * 0.5f, nullptr, MicroSize,
			M.MicroTrack);

		const FString Cooldown = FString::Printf(TEXT("%ds"), FMath::RoundToInt(Entry.ActivatedCooldown));
		const float CooldownW = TraceCharacterSelectFile::ChipWidth(HUD, Cooldown, CooldownH, 0.f,
			M.ChipTextSize, 0.f);

		TraceCharacterSelectFile::DrawChip(HUD, Cooldown, X + Pad + TextW - CooldownW,
			MicroY + (MicroRowH - CooldownH) * 0.5f, CooldownH, CooldownW,
			TraceSelectStyle::Cyan, TraceSelectStyle::InkSoft, M.ChipTextSize, 0.f);

		// Size and tracking are the grid's, not this card's: SLIMEWALL and ZIP are set at the same
		// size on their own cards, which is the whole point of the uniform solve.
		TraceCharacterSelectType::Draw(HUD, Entry.ActivatedName, TraceSelectStyle::Ink,
			X + Pad, BottomY, nullptr, M.LeadSize, M.LeadTrack);
	}

	// ---- Selection --------------------------------------------------------------------------------
	//
	// The artist's hover plate already rings the tile in amber. This adds the character's own colour
	// just OUTSIDE the tile, breathing — drawn outside so the hit rectangle is untouched, and in the
	// accent so the selected tile and the detail panel below it are visibly the same object.
	if (bSelected)
	{
		const float Out = 4.f * S;
		DrawFrame(HUD, X - Out, Y - Out, W + Out * 2.f, H + Out * 2.f,
			TraceSelectStyle::WithAlpha(Entry.Accent, 0.45f + 0.35f * FMath::Sin(Now * 6.f)));
	}
}

float FTraceCharacterSelect::DrawWrapped(AHUD* HUD, const FString& Text, const FLinearColor& Color,
	float X, float Y, float MaxWidth, UFont* Font, float Scale, float LineGap)
{
	// The algorithm lives in TraceCharacterSelectType::WrapDraw so that the ability descriptions can
	// run the SAME greedy walk in a different face — see the note there. This overload keeps meaning
	// exactly what it meant: the light default, for the status line and anything else the header's
	// signature reaches.
	return TraceCharacterSelectType::WrapDraw(HUD, Text, Color, X, Y, MaxWidth, Font, Scale, LineGap);
}

void FTraceCharacterSelect::DrawFrame(AHUD* HUD, float X, float Y, float W, float H, const FLinearColor& Color)
{
	TraceCharacterSelectFile::StrokeRect(HUD, X, Y, W, H, 1.f * UIScale, Color);
}

void FTraceCharacterSelect::DrawCursor(AHUD* HUD)
{
	// The OS cursor does not appear in captured frames and is hidden outright during a match, so the
	// overlay draws its own.
	if (!bHasCursor)
	{
		return;
	}

	// ONE POINTER, DRAWN IN ONE PLACE — the UI QA pass's finding 6.
	//
	// Spec v20 §6.7 put the artist's arrow on this screen; spec v24 §0 moved its aspect and tip into
	// TraceMenuArtStyle so this file stopped carrying its own copy. What was still not shared was the
	// DRAW: this function, FTraceOptionsMenu::DrawCursor and ATraceMenuHUD::DrawCursor each had their
	// own, and the third one drew a completely different picture (a cyan cross). All three now call
	// the same function, which is also the one that decides the tint — see
	// UI/TraceHardwareCursor.h's second header block for what "themed" means and why it is cyan.
	//
	// Still TIP-ANCHORED, and that is DrawPointer's contract rather than this caller's arithmetic:
	// PollInput hit-tests at CursorPos, and a centre-anchored arrow would draw its point about eleven
	// pixels away from the pixel that is actually being clicked.
	if (TraceHardwareCursor::DrawPointer(HUD, CursorPos, UIScale))
	{
		return;
	}

	// The vector cross this screen used to draw, kept as the fallback.
	const float Size = 9.f * UIScale;
	const float Thick = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceSelectStyle::WithAlpha(TraceSelectStyle::Cyan, 0.95f);

	HUD->DrawLine(CursorPos.X - Size, CursorPos.Y, CursorPos.X - Size * 0.35f, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X + Size * 0.35f, CursorPos.Y, CursorPos.X + Size, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y - Size, CursorPos.X, CursorPos.Y - Size * 0.35f, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y + Size * 0.35f, CursorPos.X, CursorPos.Y + Size, Color, Thick);
}

// The four helpers below are the header's, and they are this file's ONE text API. See the parameter
// convention spelled out above namespace TraceCharacterSelectType: Font == nullptr selects the menu
// typeface and Scale is a POINT SIZE; a non-null Font is the engine bitmap path with Scale as a
// multiplier, which is what the debug and fallback callers still want.

float FTraceCharacterSelect::MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale) const
{
	return TraceCharacterSelectType::Width(HUD, Text, Font, Scale, 0.f);
}

float FTraceCharacterSelect::MeasureHeight(AHUD* HUD, const FString& Text, UFont* Font, float Scale) const
{
	// The LINE BOX, not this string's ink. Every caller here is stacking lines, and a height that
	// changed depending on whether the string happened to contain a descender is exactly how the old
	// screen ended up with rows that did not line up between one card and the next.
	(void)Text;
	return TraceCharacterSelectType::LineHeight(HUD, Font, Scale);
}

void FTraceCharacterSelect::DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color,
	float CenterX, float Y, UFont* Font, float Scale)
{
	TraceCharacterSelectType::DrawCentered(HUD, Text, Color, CenterX, Y, Font, Scale, 0.f);
}

#if !UE_BUILD_SHIPPING
// Named after the file, like the block at the top of it and for the same jumbo-build reason —
// "TraceCharacterSelectCommand" is a name a second console-command file could plausibly reuse, and
// under the unity build both would land in the same anonymous namespace and fail to link on MSVC.
namespace TraceCharacterSelectCommands
{
	/**
	 * Trace.Characters.Select <1..N>   (N = TraceCharacterRoster::LastId, 8 since spec v18 §2)
	 *
	 * Drives a pick from the console so a headless run can prove the SCREEN's request path, not just
	 * the game mode's. It sets a plain int that the open screen consumes on its next Tick; see the
	 * note on GTraceCharacterSelectDebugPick in the header for why it is not a pointer.
	 */
	void TraceCharacterSelectCommand(const TArray<FString>& Args)
	{
		const int32 Requested = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 1;
		GTraceCharacterSelectDebugPick = Requested;

		UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Console pick queued: %s."),
			*TraceCharacterRoster::NameFor(static_cast<uint8>(Requested)));
	}

	/**
	 * Trace.Characters.ClickTest <1..N>   (N = TraceCharacterRoster::Count, 8 since spec v18 §2)
	 *
	 * Spec v15 §4 for this screen. Parks a real cursor on the card and clicks it through the real
	 * input pipeline, then reports how many complete clicks it took. One is the requirement.
	 */
	void TraceCharacterSelectClickTestCommand(const TArray<FString>& Args)
	{
		const int32 Requested = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 1;
		GTraceCharacterSelectClickTest = FMath::Clamp(Requested, 1, TraceCharacterRoster::Count);

		UE_LOG(LogTraceGame, Display, TEXT("[CharSelect.ClickTest] Queued a click on card %d."),
			GTraceCharacterSelectClickTest);
	}

	/**
	 * Trace.Characters.Highlight <1..N | 0>
	 *
	 * HIGHLIGHTS WITHOUT CONFIRMING, and holds it. Trace.Characters.Select cannot be used to look at
	 * the detail panel because it confirms on the same tick, and hovering cannot be aimed from a
	 * headless run — so before this there was no way to photograph nine of the ten detail panels. See
	 * TraceCharSelectHighlightLatch for the four-day defect that fact hid.
	 *
	 * 0 hands the screen back to the player, so a run can hold a card, shoot it, and let go.
	 */
	void TraceCharacterHighlightCommand(const TArray<FString>& Args)
	{
		const int32 Requested = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 0;

		if (Requested < 0 || Requested > TraceCharacterRoster::Count)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CharSelect] Trace.Characters.Highlight: %d is not 0..%d."),
				Requested, TraceCharacterRoster::Count);
			return;
		}

		TraceCharSelectHighlightLatch::Held = Requested;

		if (Requested == 0)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Highlight released; the player drives it again."));
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[CharSelect] Highlight HELD on card %d (%s). The screen stays open — this does not confirm."),
				Requested, TraceCharacterRoster::All()[Requested - 1].Name);
		}
	}

	FAutoConsoleCommand CmdTraceCharacterHighlight(
		TEXT("Trace.Characters.Highlight"),
		TEXT("Dev only. Hold the select screen's highlight on card 1..N (0 releases it) WITHOUT "
		     "confirming, so the detail panel for any character can be photographed by an unattended "
		     "run. Trace.Characters.Select confirms and closes the screen; this does not. No effect "
		     "unless the select screen is open on this machine."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceCharacterHighlightCommand));

	FAutoConsoleCommand CmdTraceCharacterSelectClickTest(
		TEXT("Trace.Characters.ClickTest"),
		TEXT("Dev only. Spec v15 s4. Parks the cursor on card 1..N (N = the roster size, 10 since spec "
		     "v19 s3 added Mortimer and Lily) and clicks it through the real input pipeline, then "
		     "reports how many complete clicks the card needed. Card 10 is the one the 0 key selects. "
		     "No effect unless the select screen is open on this machine."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceCharacterSelectClickTestCommand));

	FAutoConsoleCommand CmdTraceCharacterSelect(
		TEXT("Trace.Characters.Select"),
		TEXT("Dev only. Spec v14 3. Picks character 1..N (N = the roster size, 8 since spec v18 s2) "
		     "through the select screen's own request path (highlight, then confirm), so a headless run "
		     "exercises the screen rather than bypassing it. No effect unless the select screen is open "
		     "on this machine."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceCharacterSelectCommand));
}
#endif
