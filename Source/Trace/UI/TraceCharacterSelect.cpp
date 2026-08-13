// Trace — the character select screen. See TraceCharacterSelect.h.

#include "UI/TraceCharacterSelect.h"

#include "CanvasItem.h"                 // FCanvasTextItem — the only way to typeset with a real face
#include "Engine/Canvas.h"              // UCanvas::Canvas (the FCanvas) for the DPI scale
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
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
#include "UI/Text/TraceCanvasText.h"       // spec v22 §A1 — the cards type from the glyph atlas
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"   // the artist's sprites, colours and 9-slice numbers

#if !UE_BUILD_SHIPPING
int32 GTraceCharacterSelectDebugPick = 0;
int32 GTraceCharacterSelectClickTest = 0;

// NAMED, not anonymous: UBT compiles this module as a unity/jumbo build, so two files that each
// define something at the top of an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
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
	enum class ESprite : uint8
	{
		PlateDefault = 0,
		PlateHover,
		PlateDisabled,
		ValueBox,
		Cursor,
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
		case ESprite::Cursor:        return TraceMenuArtStyle::Cursor;
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
			TEXT("[CharSelect] Art: %d/%d of the artist's sprites resolved (button plate x3, value chip, "
			     "cursor). Anything missing falls back to a drawn rectangle."), Resolved, SpriteCount);
	}

	/** One stretched tile. Used for the cursor, which has no slicing and must keep its aspect. */
	void DrawSprite(AHUD* HUD, UTexture2D* Texture, float X, float Y, float W, float H, const FLinearColor& Tint)
	{
		if (HUD == nullptr || Texture == nullptr || W <= 0.f || H <= 0.f)
		{
			return;
		}
		HUD->DrawTexture(Texture, X, Y, W, H, 0.f, 0.f, 1.f, 1.f, Tint);
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

	float Width(AHUD* HUD, const FString& Text, UFont* Font, float Scale, float Tracking)
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
		float X, float Y, UFont* Font, float Scale, float Tracking)
	{
		if (HUD == nullptr || Text.IsEmpty())
		{
			return;
		}

		if (Font == nullptr && TraceText::IsAtlasActive())
		{
			TraceText::FStyle Style(FMath::Max(1.f, Scale), Color);
			Style.Tracking = Tracking;
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
	 * How many lines @p Text would wrap to at @p Scale in @p MaxWidth. Same greedy walk as DrawWrapped,
	 * so the count and the draw cannot disagree.
	 */
	int32 WrapLines(AHUD* HUD, const FString& Text, float MaxWidth, UFont* Font, float Scale)
	{
		TArray<FString> Words;
		Text.ParseIntoArray(Words, TEXT(" "), /*InCullEmpty=*/true);

		int32 Lines = 0;
		FString Line;

		for (const FString& Word : Words)
		{
			const FString Candidate = Line.IsEmpty() ? Word : (Line + TEXT(" ") + Word);
			if (!Line.IsEmpty() && Width(HUD, Candidate, Font, Scale, 0.f) > MaxWidth)
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

		// BOTS ARE CONSULTED NOW. Spec v14 §3 made them permanently characterless and this test used to
		// skip them for that reason; spec v15 §2 reverses it, and a bot team-mate holding MACE greys
		// MACE out exactly as a human team-mate would. Leaving the skip in would have been a card that
		// looked free, was sent, and came back refused — the one outcome this local belief exists to
		// avoid.
		//
		// WHEN THIS ACTUALLY SHOWS SOMETHING. Under §2's ordering the bots on your team hold nothing
		// while your screen is open, so on a fresh match every card is free. The case it is for is the
		// player who joins a match ALREADY IN PROGRESS: the bots filled long ago, and their picks are
		// the only reason a card would be grey.
		//
		// Enemies are still skipped: mirroring the pick is legal (§3), so an enemy holding a character
		// blocks nothing.
		if (Candidate->Team != LocalState->Team)
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
//   4. No identity beyond a 3 px stripe Accent-coloured name, a monogram watermark, a keycap and the
//                                       artist's plate — per tile, in the character's own colour.
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

	// One line, once per process. A screenshot cannot tell "the redesign is not compiled in" from "it
	// is compiled in and computed these numbers", and this screen has already cost one round trip to
	// exactly that ambiguity. Now it also says which typeface actually reached the pixels, because the
	// menu face failing to resolve is invisible in a still and changes every measurement here.
	if (!bLoggedLayoutOnce)
	{
		bLoggedLayoutOnce = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect] Layout v20: view %.0fx%.0f scale %.3f | %d tiles as %d row(s) x %d col(s) | "
			     "tile %.0fx%.0f at y=%.0f | detail panel %.0fx%.0f at y=%.0f | face=%s"),
			ViewW, ViewH, S, TraceCharacterRoster::Count, TraceSelectGrid::Rows, TraceSelectGrid::Columns,
			TileW, TileH, GridY, InnerW, TraceSelectLayout::DetailH * S, TraceSelectLayout::DetailTop * S,
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

		// The key that picks it, as a key, and in reading order: the word PRESS, then the cap. §6.6.
		{
			const FString Glyph = TraceCharacterSelectFile::KeyGlyphForIndex(Highlighted);
			if (!Glyph.IsEmpty())
			{
				const float CapH = 44.f * S;
				const float WordW = TraceCharacterSelectType::Width(HUD, TEXT("PRESS"), nullptr,
					TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);

				TraceCharacterSelectType::Draw(HUD, TEXT("PRESS"), TraceSelectStyle::InkDim,
					IdentityX,
					IdentityY + (CapH - TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeLabel * S)) * 0.5f,
					nullptr, TraceSelectLayout::SizeLabel * S, TraceSelectLayout::TrackLabel * S);

				TraceCharacterSelectFile::DrawChip(HUD, Glyph, IdentityX + WordW + (12.f * S), IdentityY,
					CapH, CapH, Entry.Accent, TraceSelectStyle::Ink, 22.f * S, 0.f);

				IdentityY += CapH + (18.f * S);
			}
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
			const float NameW = TraceCharacterSelectType::Width(HUD, Entry.Name, nullptr, NameSize, NameTrack);
			if (NameW > IdentityW && NameW > 1.f)
			{
				NameSize = FMath::Max(NameSize * (IdentityW / NameW),
					TraceSelectLayout::SizeLabel * S * 1.2f);
			}

			TraceCharacterSelectType::Draw(HUD, Entry.Name,
				bTaken ? TraceSelectStyle::Dimmed(Entry.Accent, 0.45f) : Entry.Accent,
				IdentityX, IdentityY, nullptr, NameSize, NameTrack);
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
					TraceCharacterSelectType::WrapLines(HUD, MovementText, ColumnW, nullptr, TrySize) * TryLineH <= PlainRoom &&
					TraceCharacterSelectType::WrapLines(HUD, PassiveText, ColumnW, nullptr, TrySize) * TryLineH <= PlainRoom &&
					TraceCharacterSelectType::WrapLines(HUD, ActivatedText, ColumnW, nullptr, TrySize) * TryLineH <= ActivatedRoom;

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
			DrawWrapped(HUD, MovementText, BodyColor, ColumnsX, Y, ColumnW, nullptr, BodySize, BodyGap);
		}

		{
			const float X = ColumnsX + ColumnW + ColumnGap;
			float Y = DrawHeading(TEXT("PASSIVE"), TraceSelectStyle::Cyan, X, ColumnsY);
			DrawWrapped(HUD, PassiveText, BodyColor, X, Y, ColumnW, nullptr, BodySize, BodyGap);
		}

		// ---- ACTIVATED — spec v20 §6.5 -----------------------------------------------------------
		//
		// The measured defect, verbatim: "ACTIVATED [E] - RIPPLE - 20s CD runs key, name and cooldown
		// together with hyphens and no visual separation". They are three different KINDS of fact — a
		// control, a name and a duration — so they get three different objects: a keycap, a line of
		// display type in the character's colour, and a chip.
		{
			const float X = ColumnsX + (ColumnW + ColumnGap) * 2.f;
			float Y = DrawHeading(TEXT("ACTIVATED"), Entry.Accent, X, ColumnsY);

			const float RowH = 30.f * S;
			const float KeyW = TraceCharacterSelectFile::DrawChip(HUD, TEXT("E"), X, Y, RowH, RowH,
				TraceSelectStyle::Dimmed(Entry.Accent, BodyDim), TraceSelectStyle::Ink,
				TraceSelectLayout::SizeChip * S, 0.f);

			const FString Cooldown = FString::Printf(TEXT("%ds"), FMath::RoundToInt(Entry.ActivatedCooldown));
			const float CooldownW = TraceCharacterSelectType::Width(HUD, Cooldown, nullptr, TraceSelectLayout::SizeChip * S, 0.f)
				+ RowH * 0.90f;

			TraceCharacterSelectFile::DrawChip(HUD, Cooldown, X + ColumnW - CooldownW, Y, RowH, CooldownW,
				TraceSelectStyle::Cyan, TraceSelectStyle::InkSoft, TraceSelectLayout::SizeChip * S, 0.f);

			const float NameH = TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeLead * S);
			TraceCharacterSelectType::Draw(HUD, Entry.ActivatedName,
				TraceSelectStyle::Dimmed(Entry.Accent, BodyDim),
				X + KeyW + (12.f * S), Y + (RowH - NameH) * 0.5f, nullptr,
				TraceSelectLayout::SizeLead * S, TraceSelectLayout::TrackName * S);

			Y += RowH + (14.f * S);
			DrawWrapped(HUD, ActivatedText, BodyColor, X, Y, ColumnW, nullptr, BodySize, BodyGap);
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
 * own accent, the activated ability's name and cooldown, and a monogram watermark. No paragraph, no
 * variable-length anything. That is what makes a fixed-height box honest rather than "roughly half
 * empty" (Rocco) and "nearly overflowing" (Mortimer), which is the §6.1 complaint verbatim.
 *
 * §6.4 asked for identity "beyond a 3 px colour stripe". There are no portraits in this project and
 * inventing one is not a layout pass, so the identity is built out of what does exist: the accent as a
 * large field rather than a sliver, the name set at 25 px in that accent, the artist's plate carrying
 * the state (default / hover / disabled — the sheet's own three), and the initial as a big low-alpha
 * watermark so the tiles differ in SHAPE as well as in hue at a glance.
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

	const float Pad = 18.f * S;

	// THE TILE IS TWO BLOCKS PINNED TO OPPOSITE EDGES, not a stack that runs down from the top: the
	// name group hangs off the top and the ability group off the BOTTOM. That is what stops the v1
	// version of this redesign reintroducing §6.1's own complaint at a smaller scale — a top-down stack
	// left about fifty pixels of nothing under every tile, which is dead space in exactly the way the
	// old fixed-height cards were. The gap between the two blocks is the monogram's, on purpose.
	const float CapH = 30.f * S;
	const float LeadH = TraceCharacterSelectType::LineHeight(HUD, nullptr, TraceSelectLayout::SizeLead * S);
	const float MicroSize = TraceSelectLayout::SizeLabel * 0.86f * S;
	const float MicroH = TraceCharacterSelectType::LineHeight(HUD, nullptr, MicroSize);

	const float BottomY = Y + H - Pad - LeadH;              // the ability name's line box
	const float MicroY = BottomY - MicroH - (5.f * S);      // its ACTIVATED label

	// ---- The monogram watermark ------------------------------------------------------------------
	//
	// Drawn FIRST so nothing has to fight it, and at an alpha low enough that it is texture rather than
	// text. It is the cheapest per-character silhouette available without art: R and S and M and X do
	// not look alike even out of focus, which is the property §6.4 is actually asking for. Sized and
	// placed to sit in the gap BETWEEN the two blocks rather than behind either of them.
	{
		const FString Monogram = FString(Entry.Name).Left(1);
		const float MonoSize = 88.f * S;
		const float MonoW = TraceCharacterSelectType::Width(HUD, Monogram, nullptr, MonoSize, 0.f);
		const float MonoH = TraceCharacterSelectType::LineHeight(HUD, nullptr, MonoSize);

		TraceCharacterSelectType::Draw(HUD, Monogram,
			TraceSelectStyle::WithAlpha(Entry.Accent, bTaken ? 0.06f : 0.13f),
			X + W - Pad - MonoW, MicroY - MonoH + (MonoH * 0.22f), nullptr, MonoSize, 0.f);
	}

	// ---- Keycap + name ---------------------------------------------------------------------------
	float PenY = Y + Pad;
	{
		const FString Glyph = TraceCharacterSelectFile::KeyGlyphForIndex(CardIndex);

		float CapW = 0.f;
		if (!Glyph.IsEmpty())
		{
			CapW = TraceCharacterSelectFile::DrawChip(HUD, Glyph, X + Pad, PenY, CapH, CapH,
				TraceSelectStyle::Dimmed(Entry.Accent, Dim),
				TraceSelectStyle::WithAlpha(TraceSelectStyle::Ink, Dim),
				TraceSelectLayout::SizeChip * S, 0.f);
			CapW += 12.f * S;
		}

		// THE NAME IS FITTED, NOT ASSUMED. SLIMEBALL and MORTIMER are half again as wide as ROCCO, and
		// at the 250-pixel tile a 4:3 viewport produces they ran past the tile's own padding in the
		// first capture of this redesign. Shrinking the two longest names by a few points is invisible;
		// a name touching the edge of its card is the "sloppy" this pass exists to remove. Floored at
		// 72% so it degrades into smaller type rather than into unreadable type.
		float NameSize = TraceSelectLayout::SizeName * S;
		float NameTrack = TraceSelectLayout::TrackName * S;
		{
			const float Room = (W - Pad * 2.f) - CapW;
			const float Natural = TraceCharacterSelectType::Width(HUD, Entry.Name, nullptr, NameSize, NameTrack);
			if (Natural > Room && Natural > 1.f)
			{
				const float Fit = FMath::Max(0.72f, Room / Natural);
				NameSize *= Fit;
				NameTrack *= Fit;
			}
		}

		const float NameH = TraceCharacterSelectType::LineHeight(HUD, nullptr, NameSize);
		TraceCharacterSelectType::Draw(HUD, Entry.Name,
			bTaken ? TraceSelectStyle::InkDim : Entry.Accent,
			X + Pad + CapW, PenY + (CapH - NameH) * 0.5f, nullptr, NameSize, NameTrack);

		PenY += CapH + (10.f * S);
	}

	// The accent rule. This is the "3 px colour stripe" from §6.4 promoted into the layout: full tile
	// width, under the name, in the character's colour.
	HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Dimmed(Entry.Accent, Dim), bTaken ? 0.35f : 0.75f),
		X + Pad, PenY, W - Pad * 2.f, FMath::Max(1.f, 2.f * S));

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
			X + Pad, StripY, W - Pad * 2.f, StripH);
		HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Danger, 0.85f),
			X + Pad, StripY, 3.f * S, StripH);

		const float LineH = TraceCharacterSelectType::LineHeight(HUD, nullptr, StripTextSize);
		TraceCharacterSelectType::Draw(HUD, TakenLine, TraceSelectStyle::Danger,
			X + Pad + (10.f * S), StripY + (StripH - LineH) * 0.5f, nullptr, StripTextSize, 1.2f * S);
	}
	else
	{
		TraceCharacterSelectType::Draw(HUD, TEXT("ACTIVATED"), TraceSelectStyle::InkDim,
			X + Pad, MicroY, nullptr, MicroSize, TraceSelectLayout::TrackLabel * S);

		const FString Cooldown = FString::Printf(TEXT("%ds"), FMath::RoundToInt(Entry.ActivatedCooldown));
		const float CooldownH = 24.f * S;
		const float CooldownW = TraceCharacterSelectFile::ChipWidth(HUD, Cooldown, CooldownH, 0.f,
			TraceSelectLayout::SizeLabel * S, 0.f);

		TraceCharacterSelectFile::DrawChip(HUD, Cooldown, X + W - Pad - CooldownW,
			BottomY + (LeadH - CooldownH) * 0.5f, CooldownH, CooldownW,
			TraceSelectStyle::Cyan, TraceSelectStyle::InkSoft, TraceSelectLayout::SizeLabel * S, 0.f);

		// Fitted for the same reason the name is: SLIMEWALL beside a cooldown chip is the widest
		// combination in the roster.
		float LeadSize = TraceSelectLayout::SizeLead * S;
		float LeadTrack = TraceSelectLayout::TrackName * S;
		{
			const float Room = (W - Pad * 2.f) - CooldownW - (12.f * S);
			const float Natural = TraceCharacterSelectType::Width(HUD, Entry.ActivatedName, nullptr, LeadSize, LeadTrack);
			if (Natural > Room && Natural > 1.f)
			{
				const float Fit = FMath::Max(0.72f, Room / Natural);
				LeadSize *= Fit;
				LeadTrack *= Fit;
			}
		}

		TraceCharacterSelectType::Draw(HUD, Entry.ActivatedName, TraceSelectStyle::Ink,
			X + Pad, BottomY + (LeadH - TraceCharacterSelectType::LineHeight(HUD, nullptr, LeadSize)) * 0.5f,
			nullptr, LeadSize, LeadTrack);
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
	// Greedy word wrap: measure a candidate line and back off one word. Exact rather than approximate
	// for the engine's bitmap fonts (no shaping, no kerning), and near enough for a proportional face —
	// there is no script here where adding a word makes a line narrower.
	//
	// See the parameter-convention block above TraceCharacterSelectType: Font == nullptr means the menu
	// typeface and Scale is a POINT SIZE.
	TArray<FString> Words;
	Text.ParseIntoArray(Words, TEXT(" "), /*InCullEmpty=*/true);

	const float LineH = TraceCharacterSelectType::LineHeight(HUD, Font, Scale);
	float CursorY = Y;
	FString Line;

	for (const FString& Word : Words)
	{
		const FString Candidate = Line.IsEmpty() ? Word : (Line + TEXT(" ") + Word);
		if (!Line.IsEmpty() && TraceCharacterSelectType::Width(HUD, Candidate, Font, Scale, 0.f) > MaxWidth)
		{
			TraceCharacterSelectType::Draw(HUD, Line, Color, X, CursorY, Font, Scale, 0.f);
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
		TraceCharacterSelectType::Draw(HUD, Line, Color, X, CursorY, Font, Scale, 0.f);
		CursorY += LineH + LineGap;
	}

	return CursorY;
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

	// SPEC v20 §6.7 and §0.8 — the artist's cursor, which until now existed only on the UMG title
	// screen. It is TIP-ANCHORED, not centre-anchored: PollInput hit-tests at CursorPos, and a
	// centre-anchored arrow would draw its point about eleven pixels away from the pixel that is
	// actually being clicked. The tip sits at (12, 6) in the sprite's own 64 x 87.
	if (UTexture2D* Arrow = TraceCharacterSelectArt::Sprite(TraceCharacterSelectArt::ESprite::Cursor))
	{
		const float ArrowH = 30.f * UIScale;
		const float ArrowW = ArrowH * (64.f / 87.f);

		TraceCharacterSelectArt::DrawSprite(HUD, Arrow,
			CursorPos.X - ArrowW * (12.f / 64.f), CursorPos.Y - ArrowH * (6.f / 87.f),
			ArrowW, ArrowH, FLinearColor::White);
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
