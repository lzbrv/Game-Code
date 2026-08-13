// Trace — settings overlay implementation. See TraceOptionsMenu.h.

#include "UI/TraceOptionsMenu.h"

#include "Camera/CameraComponent.h"
#include "CanvasTypes.h"                 // FCanvas - the foreground surface, see FTraceOverSlateCanvas
#include "Containers/Ticker.h"          // FTSTicker - defer the viewport resize out of DrawHUD
#include "DynamicRHI.h"                  // RHIGetGPUFrameCycles
#include "Engine/Canvas.h"               // UCanvas::Canvas, the pointer this file swaps
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"   // GEngine->GameViewport->Viewport->GetDebugCanvas()
#include "Engine/Texture2D.h"            // the artist's sprites - see the art block below
#include "TextureResource.h"             // FTextureResource::TextureRHI - see IsDrawable
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"         // Trace.UI.ModalOverSlate - spec v23 §A2's red arm
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"            // -TraceMenuActivate, the paused-world capture hook
#include "Misc/Parse.h"
#include "Scalability.h"
#include "Settings/TraceGameUserSettings.h"
#include "Trace.h"                       // LogTraceGame
#include "UI/TraceMatchOptions.h"        // TraceCharacters - the spec v14 §3 toggle's storage
#include "UI/Text/TraceCanvasText.h" // spec v22 §A1 - this page types in the artist's face
#include "UnrealClient.h"                // FViewport::GetDebugCanvas - the foreground surface

// =================================================================================================
// WHERE THE VIDEO SETTINGS ACTUALLY LIVE — AND WHY NONE OF THEM LIVE HERE
//
// Every row on the VIDEO page is stored, validated, applied and persisted by
// UTraceGameUserSettings (Settings/TraceGameUserSettings.h). This file holds NO video state at all:
// no cached resolution list, no preset ladder, no frame-cap table, no field-of-view value. Each row
// is read live from that class on the frame it is drawn and written straight back on the frame it
// is changed.
//
// That is a deliberate line, and it is drawn where it is because both halves have been written by
// somebody who could plausibly have written the other. Two copies of "what resolutions exist" or
// "which levels count as Epic" would agree on the day they were written and diverge on the first
// day either was edited, and the symptom of that divergence is a menu row that quietly controls
// nothing — a failure this project has already been bitten by and now warns about in its own build
// notes. So: that class decides what a setting MEANS. This one decides what it LOOKS like, where it
// sits in the list, and what happens when a key is held down on it.
//
// The one thing this file argues about is ORDER, and that argument is spec v11 §0: the frame is
// GPU-bound per pixel, so RESOLUTION SCALE and AUTO-DETECT come first, above the window mode and
// above all nine quality groups. See RebuildRows.
// =================================================================================================

// =================================================================================================
// Palette
//
// The same two hues as the title screen — a cyan that carries the interface and an amber that only
// ever means "danger, or something is waiting on you" — over near-black. Restated here rather than
// shared with TraceMenuHUD.cpp's TraceMenuStyle because this overlay also draws over the MATCH,
// where the title screen's palette namespace is not in scope, and a header shared between them
// would exist purely to hold six colours.
// =================================================================================================

// =================================================================================================
// SPEC v22 §A1 — THIS PAGE IS ONE TYPEFACE, AND IT IS THE ARTIST'S
// =================================================================================================
//
// The settings page had exactly the defect the spec calls the headline, one screen further in than
// anybody looked. Its CONTROLS header drew the artist's baked KEYBIND and KEY word sprites, and
// every other string on the page — SETTINGS, DISPLAY, MOUSE, MOVE FORWARD, the key names, the
// numbers — came out of AHUD::DrawText in the engine's stand-in font, in the same column, four
// pixels apart. Photographed at 1920x1080 in v22integ_03_settings_open.png before this change.
//
// Now every one of them goes through UI/Text: TraceCanvasText blits one atlas quad per glyph, and
// the word sprites are retired the same way the title row's PLAY and SETTINGS were.
//
// ---- THE ONE THING THAT IS NOT OBVIOUS: THE UNITS ------------------------------------------------
//
// This page's whole layout is expressed as (UFont*, Scale) pairs, where Scale is a MULTIPLIER on a
// bitmap font's natural size — `FontMedium, 1.5f * UIScale`. TraceText wants a point size in
// pixels. Converting with a constant would have been a guess, so SizeFor() measures instead: it
// asks the engine what line height that font at that scale actually produces, and returns the point
// size whose line height matches. TraceText::LineHeight is linear in size, so that inverse is exact.
//
// The consequence is the one that matters for a page this dense: every row keeps the height it had.
// MeasureHeight() below returns the same number it returned before this change, so the vertical
// rhythm of nineteen rows, their plates and their hit rects are all untouched. Only the letterforms
// and the WIDTHS move — and the widths are measured, not assumed, by the same call that draws.
//
// The fallback needs no branch here. With no atlas, TraceCanvasText types Lato at the same size and
// TraceText measures Lato, so this page degrades to "the wrong face, laid out correctly".
// =================================================================================================

// =================================================================================================
// SPEC v23 §A2 — THE FOREGROUND CANVAS
//
// The argument for all of this is in the header, above FTraceOverSlateCanvas. What is worth having
// next to the code is the list of things that were CHECKED rather than assumed, because every one of
// them would have shown up as a subtly wrong screen rather than as a failure:
//
//  * COORDINATE SPACE. `FSceneViewport::Draw` builds the scene canvas with
//    `ShouldDPIScaleSceneCanvas() ? GetDPIScale() : 1` and `FDebugCanvasDrawer::InitDebugCanvas`
//    builds the foreground one with `GetDPIScale()`. `UGameViewportClient` does not override that
//    predicate, so both carry the same base transform and a pixel is a pixel on either. The panel
//    therefore lands on the same rectangle it landed on before, which is exactly what the §A2
//    re-measurement checks.
//
//  * THE TWO TRANSFORMS THE HUD PATH PUSHES AND THIS ONE DOES NOT. `UGameViewportClient::Draw`
//    pushes `FTranslationMatrix(CanvasOrigin)` on the scene canvas and calls
//    `UCanvas::ApplySafeZoneTransform()`. CanvasOrigin is `View->UnscaledViewRect.Min`, which is
//    (0,0) for a single full-screen player — this game never splits the screen on a menu — and the
//    safe-zone call returns immediately when the padding is zero, which it is on desktop. Both are
//    therefore identity here. A split-screen or console port would have to revisit this.
//
//  * FLUSHING. The foreground canvas is created with `SetAllowedModes(Allow_DeleteOnRender)`, i.e.
//    without Allow_Flush. `FCanvas::Flush_GameThread` returns early rather than asserting when the
//    mode is absent, and nothing on the drawing path below calls it: every draw here batches, the
//    same way it batches on the scene canvas.
//
//  * WHAT ELSE SHARES THE SURFACE. `DrawStatsHUD` and `UConsole::PostRender_Console` draw into it
//    AFTER the HUD, so `stat fps` and the console still land on top of the modal rather than under
//    it. That ordering is the engine's and it is the one we want.
// =================================================================================================

namespace TraceOptionsMenuOverSlate
{
	/**
	 * `Trace.UI.ModalOverSlate 0|1` — the red arm for spec v23 §A2.
	 *
	 * 1 (default): the settings overlay and the JOIN prompt draw on the foreground canvas, so the
	 * UMG title screen (or the in-match UMG HUD) stays up behind them and the player never sees the
	 * renderer change.
	 *
	 * 0: the pre-v23 behaviour — the modal draws on the scene canvas, and because that is composited
	 * UNDER Slate, ATraceMenuHUD has to collapse its widget and fall back to the old Canvas title
	 * screen for as long as the modal is up. That is the defect, reproducible from the shipping
	 * binary, which is the only way a screenshot of the fix means anything.
	 */
	static int32 GOverSlate = 1;
	static FAutoConsoleVariableRef CVarModalOverSlate(
		TEXT("Trace.UI.ModalOverSlate"),
		GOverSlate,
		TEXT("1 = DEFAULT. Canvas modals (SETTINGS, the pause menu, the JOIN prompt) draw on the\n")
		TEXT("    engine's foreground canvas, which Slate paints ON TOP of the UMG screens. The title\n")
		TEXT("    screen stays up behind them.\n")
		TEXT("0 = pre-v23: modals draw under Slate, so the UMG title screen stands down and the old\n")
		TEXT("    Canvas one draws instead for those frames. This is the red arm - it reproduces the\n")
		TEXT("    58.8%-of-the-screen renderer swap that spec v23 A2 was raised for.\n")
		TEXT("-TraceModalUnderSlate on the command line is the same 0, available before the first frame."),
		ECVF_Default);

	/**
	 * `-TraceModalUnderSlate` — the same red arm, on the command line, and it has to exist.
	 *
	 * A cvar set with -ExecCmds arrives during engine init, which is after the first title screen has
	 * already decided which renderer to build; the same reasoning is written out above
	 * TraceMenuHUDFile::WantsUMG, which reads its switches off FCommandLine for exactly that reason.
	 * It also keeps the harness free of quoted arguments with spaces in them, which do not survive
	 * argv -> FCommandLine reconstruction reliably.
	 *
	 * Parsed once: this is asked twice a frame, and a linear scan of the command line in front of a
	 * paused match would be its own small crime.
	 */
	static bool WantsOverSlate()
	{
		static const bool bForcedUnder = FParse::Param(FCommandLine::Get(), TEXT("TraceModalUnderSlate"));
		return !bForcedUnder && GOverSlate != 0;
	}

	/** One line naming the surface and the reason. Read by the log and by the menu's verifier. */
	static FString GStatus = TEXT("FOREGROUND - not yet decided this session.");

	/**
	 * The literal GStatus was last built from, or null when it was formatted.
	 *
	 * Resolve() runs up to twice a frame — once for the host's decision, once for the modal's own
	 * scope — and an unconditional FString assignment there is an allocation and an 80-character copy
	 * 120 times a second for a string nobody reads unless something changed. The literals have stable
	 * addresses, so a pointer compare answers "same reason as last time?" exactly.
	 */
	static const TCHAR* GStatusLiteral = nullptr;

	static void SetStatus(const TCHAR* Text)
	{
		if (GStatusLiteral != Text)
		{
			GStatusLiteral = Text;
			GStatus = Text;
		}
	}

	/**
	 * The engine's own canvases, by the names `UGameViewportClient::Draw` gives them.
	 *
	 * FindObject and not a member, for the same reason TraceCanvasText::GameCanvas does it: this
	 * class is not a UObject and must not hold a UObject reference across a frame, and the HUD's own
	 * `Canvas` / `DebugCanvas` members are PROTECTED, so a plain C++ class holding an AHUD* cannot
	 * read them. The names are stable engine constants ("CanvasObject", GameViewportClient.cpp:1529;
	 * "DebugCanvasObject", :1528) and the whole Sofachrome Canvas path already depends on the first
	 * of them.
	 */
	static UCanvas* FindGameCanvas()
	{
		return FindObject<UCanvas>(GetTransientPackage(), TEXT("CanvasObject"));
	}

	/** The FCanvas Slate paints in front of the UI, or null when there is no viewport to ask. */
	static FCanvas* FindForegroundCanvas()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr)
		{
			return nullptr;
		}
		FViewport* Viewport = GEngine->GameViewport->Viewport;
		return (Viewport != nullptr) ? Viewport->GetDebugCanvas() : nullptr;
	}

	/**
	 * Resolves both surfaces and says whether the swap is on.
	 *
	 * @param OutGameCanvas   the UCanvas whose FCanvas would be swapped, when the answer is true.
	 * @param OutForeground   the FCanvas it would be swapped to.
	 */
	static bool Resolve(const AHUD* InHUD, float InViewW, float InViewH,
		UCanvas*& OutGameCanvas, FCanvas*& OutForeground)
	{
		OutGameCanvas = nullptr;
		OutForeground = nullptr;

		if (InHUD == nullptr || !WantsOverSlate())
		{
			SetStatus((InHUD == nullptr)
				? TEXT("SCENE - no HUD to draw through.")
				: TEXT("SCENE - Trace.UI.ModalOverSlate 0 / -TraceModalUnderSlate (the v23 A2 red arm)."));
			return false;
		}

		UCanvas* GameCanvas = FindGameCanvas();
		if (GameCanvas == nullptr || GameCanvas->Canvas == nullptr)
		{
			// Between frames the UCanvas still exists with no FCanvas behind it. Not an error.
			SetStatus(TEXT("SCENE - the game canvas is not mid-draw."));
			return false;
		}

		// IDENTITY, not availability. A canvas of a different size is not the canvas this frame is
		// being drawn through — a stale one from a viewport that has since been resized, say — and
		// swapping it would send AHUD::DrawRect to one surface and TraceCanvasText to another. The
		// tolerance is a pixel because both numbers come from the same int.
		if (FMath::Abs(static_cast<float>(GameCanvas->SizeX) - InViewW) > 1.f
			|| FMath::Abs(static_cast<float>(GameCanvas->SizeY) - InViewH) > 1.f)
		{
			GStatusLiteral = nullptr;
			GStatus = FString::Printf(
				TEXT("SCENE - the game canvas is %dx%d but this frame is %.0fx%.0f."),
				GameCanvas->SizeX, GameCanvas->SizeY, InViewW, InViewH);
			return false;
		}

		FCanvas* Foreground = FindForegroundCanvas();
		if (Foreground == nullptr || Foreground == GameCanvas->Canvas)
		{
			SetStatus(TEXT("SCENE - this viewport has no foreground canvas."));
			return false;
		}

		SetStatus(TEXT("FOREGROUND - the modal draws in front of Slate; UMG screens stay up behind it."));
		OutGameCanvas = GameCanvas;
		OutForeground = Foreground;
		return true;
	}
}

FTraceOverSlateCanvas::FTraceOverSlateCanvas(AHUD* InHUD, float InViewW, float InViewH)
{
	UCanvas* GameCanvas = nullptr;
	FCanvas* Foreground = nullptr;
	if (!TraceOptionsMenuOverSlate::Resolve(InHUD, InViewW, InViewH, GameCanvas, Foreground))
	{
		return;
	}

	Surface = GameCanvas;
	SavedCanvas = GameCanvas->Canvas;
	GameCanvas->Canvas = Foreground;
	bElevated = true;
}

FTraceOverSlateCanvas::~FTraceOverSlateCanvas()
{
	// Unconditional, and it must stay that way: the scene canvas is what every OTHER drawer on this
	// frame — the in-match HUD, the character select, the engine's own subtitle pass — expects to
	// find behind that pointer. Leaving the foreground one in place would move the whole game's
	// drawing in front of Slate the moment somebody opened SETTINGS.
	if (bElevated && Surface != nullptr)
	{
		Surface->Canvas = SavedCanvas;
	}
}

bool FTraceOverSlateCanvas::IsAvailable(const AHUD* InHUD, float InViewW, float InViewH)
{
	UCanvas* GameCanvas = nullptr;
	FCanvas* Foreground = nullptr;
	return TraceOptionsMenuOverSlate::Resolve(InHUD, InViewW, InViewH, GameCanvas, Foreground);
}

const FString& FTraceOverSlateCanvas::LastStatus()
{
	return TraceOptionsMenuOverSlate::GStatus;
}

namespace TraceOptionsMenuType
{
	/** The point size whose line height equals what @p Font at @p Scale draws. See the block above. */
	static float SizeFor(AHUD* HUD, UFont* Font, float Scale)
	{
		float MeasuredW = 0.f;
		float MeasuredH = 0.f;
		if (HUD != nullptr)
		{
			HUD->GetTextSize(TEXT("Ag"), MeasuredW, MeasuredH, Font, Scale);
		}

		const float UnitLine = TraceText::LineHeight(1.f);
		if (MeasuredH > 1.f && UnitLine > KINDA_SMALL_NUMBER)
		{
			return MeasuredH / UnitLine;
		}

		// Only reachable with no HUD or a font the engine could not measure. 16 px is the engine's own
		// medium font line box, so the page comes out readable rather than microscopic.
		return FMath::Max(1.f, 16.f * Scale);
	}

	static float Width(AHUD* HUD, const FString& Text, UFont* Font, float Scale)
	{
		return TraceText::MeasureWidth(Text, SizeFor(HUD, Font, Scale));
	}

	/** The LINE BOX, which is what every caller on this page uses it for. */
	static float Height(AHUD* HUD, UFont* Font, float Scale)
	{
		return TraceText::LineHeight(SizeFor(HUD, Font, Scale));
	}

	static void Draw(AHUD* HUD, const FString& Text, const FLinearColor& Color,
		float X, float Y, UFont* Font, float Scale, TraceText::EHAlign HAlign = TraceText::EHAlign::Left)
	{
		TraceText::FStyle Style(SizeFor(HUD, Font, Scale), Color);
		Style.HAlign = HAlign;
		TraceCanvasText::Draw(HUD, Text, X, Y, Style);
	}
}

namespace TraceOptionsStyle
{
	static const FLinearColor Cyan     (0.16f, 0.88f, 1.00f, 1.00f);
	static const FLinearColor Amber    (1.00f, 0.46f, 0.08f, 1.00f);
	static const FLinearColor Ink      (0.90f, 0.97f, 1.00f, 1.00f);
	static const FLinearColor InkDim   (0.42f, 0.58f, 0.66f, 1.00f);
	static const FLinearColor Panel    (0.004f, 0.014f, 0.026f, 0.96f);
	static const FLinearColor Trough   (0.03f, 0.06f, 0.08f, 0.90f);

	static FLinearColor WithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, A);
	}
}

// =================================================================================================
// THE ARTIST'S ART, ON THIS SCREEN — spec v20 §0.6
//
// The artist's menu sheet was sliced into /Game/Trace/UI/Art and hung on a UMG title screen. This
// overlay could not reach any of it: it is a plain C++ class that paints through AHUD::DrawRect /
// DrawText / DrawLine from inside DrawHUD, and there is no Slate here to hang a Box brush on. So
// the same textures are drawn through AHUD::DrawTexture, which is the Canvas equivalent — same
// assets, same package, no new plumbing, and identical on both hosts.
//
// AND IT HAS TO BE BOTH HOSTS. This class draws the title screen's SETTINGS page and the in-match
// pause menu (Escape during a match) from one Draw(); the user asked for the art in-game, not only
// on the way in. Everything below is therefore in the shared path, and the pause root — RESUME /
// SETTINGS / VIDEO / RETURN TO TITLE / QUIT — is five Action rows that pick up the artist's button
// plates without a single line of host-specific code.
//
// THREE THINGS MAKE THIS SAFE TO PUT IN FRONT OF A PAUSED MATCH:
//
//  1. EVERY CALL SITE HAS A FALLBACK. Sprite() returns null if a texture is missing, if the package
//     failed to cook, or if the layer is switched off, and every caller then draws the exact
//     rectangle it drew before this change. A missing asset cannot produce a white box or an empty
//     row; it produces last week's screen. Grep this file for DrawTexture: not one of them stands
//     without an else.
//
//  2. NO GEOMETRY MOVES. Every sprite is fitted to a rectangle that was already being computed —
//     the row rect, the slider track, the key chip. FRow::Rect and FRow::Track are written from the
//     same expressions as before, so hit testing, slider dragging, Trace.Menu.Nudge, DebugGetRowRect
//     and the -TraceMenuClickTest harness all measure exactly what they measured before. Art shrinks
//     to fit a row; a row never grows to fit art.
//
//  3. THE TEXTURES ARE ROOTED ON FIRST USE. This class is deliberately not a UObject and holds no
//     UObject reference that outlives a frame (see the header). In a match nothing else in the world
//     references these textures, so a cached raw pointer would be collected out from under the pause
//     menu and a bare weak pointer would re-stream the art off disk mid-match. AddToRoot costs about
//     a megabyte for ten small UI textures and makes both failures impossible — and it is done here,
//     at file scope, rather than in a member, so the header's invariant still holds.
//
// CANVAS HAS NO 9-SLICE. FCanvasTileItem stretches the whole bitmap, so the 160x91 chip drawn 120x17
// would come out with its corner squashed 7:1 — which reads as sloppy art rather than as a missing
// engine feature. Everything with a corner is therefore drawn as a THREE-slice in X: left cap,
// stretched middle, right cap, with the cap's on-screen width derived from the sprite's HEIGHT
// scale, which is what keeps the corner circular at every row width. Three and not nine because the
// vertical scale is the reference — nothing here is ever stretched past its natural aspect in Y.
// =================================================================================================

namespace TraceOptionsMenuArt
{
	/**
	 * Off switch for the whole layer, so ONE binary produces both arms of a before/after.
	 *
	 * Spec v20 §4: a harness that cannot fail is not evidence. `Trace.Menu.Art 0` puts every fallback
	 * path on screen in the same session, which is both the red arm for these captures and the
	 * fastest way to confirm the fallbacks are still there.
	 */
	static int32 GEnabled = 1;

#if !UE_BUILD_SHIPPING
	// A CVar, not a console command: this file already fatals at module load if the two share a name
	// (see the Trace.Menu.Video block), and Trace.Menu.Art collides with neither command next door.
	static FAutoConsoleVariableRef CVarMenuArt(
		TEXT("Trace.Menu.Art"),
		GEnabled,
		TEXT("1 (default): the settings / pause overlay draws the artist's sprites. 0: the plain ")
		TEXT("rectangles it drew before spec v20. Both arms come out of one build, which is what makes ")
		TEXT("a before/after capture evidence rather than decoration."),
		ECVF_Default);
#endif

	enum class ESprite : uint8
	{
		PlateDefault = 0,
		PlateHover,
		PlateDisabled,
		SliderTrack,
		SliderHandle,
		ValueBox,
		WordKeybind,
		WordKey,
		Cursor,
		Chevron,
		Count
	};

	static const TCHAR* const SpritePaths[int32(ESprite::Count)] =
	{
		TEXT("/Game/Trace/UI/Art/T_MenuBtn_Default.T_MenuBtn_Default"),
		TEXT("/Game/Trace/UI/Art/T_MenuBtn_Hover.T_MenuBtn_Hover"),
		TEXT("/Game/Trace/UI/Art/T_MenuBtn_Disabled.T_MenuBtn_Disabled"),
		TEXT("/Game/Trace/UI/Art/T_MenuSliderTrack.T_MenuSliderTrack"),
		TEXT("/Game/Trace/UI/Art/T_MenuSliderHandle.T_MenuSliderHandle"),
		TEXT("/Game/Trace/UI/Art/T_MenuValueBox.T_MenuValueBox"),
		TEXT("/Game/Trace/UI/Art/T_MenuWord_Keybind.T_MenuWord_Keybind"),
		TEXT("/Game/Trace/UI/Art/T_MenuWord_Key.T_MenuWord_Key"),
		TEXT("/Game/Trace/UI/Art/T_MenuCursor.T_MenuCursor"),
		TEXT("/Game/Trace/UI/Art/T_MenuBack.T_MenuBack"),
	};

	static TWeakObjectPtr<UTexture2D> GCache[int32(ESprite::Count)];

	/** Set only on a genuine load failure, so a collected texture is re-fetched but a missing one is not re-hunted every frame. */
	static bool GFailed[int32(ESprite::Count)] = {};

	/**
	 * *** A LOADED TEXTURE IS NOT A DRAWABLE ONE, AND DRAWING ONE ANYWAY IS A CRASH. ***
	 *
	 * `AHUD::DrawTexture` passes `Texture->GetResource()` STRAIGHT into an FCanvasTileItem and checks
	 * only the UTexture (Engine HUD.cpp:986). A texture that is LOADED but whose render resource has
	 * no RHI texture yet therefore becomes a batched element the render thread cannot draw, and it
	 * dies on it: SIGSEGV in FBatchedElements::Draw at address 0x30, on the render thread, ~130 ms
	 * after this page first drew.
	 *
	 * MEASURED, in this order, because the first two answers were wrong:
	 *   - `Trace.Menu.Art 0` (no textures at all) ran clean, and so did the JOIN prompt, which draws
	 *     rects and atlas text and no textures. So it was the sprites, not the surface.
	 *   - guarding on `GetResource() != nullptr` did NOT fix it: the resource object exists straight
	 *     away. It is `FTextureResource::TextureRHI` that arrives later, from the render thread.
	 *   - guarding on that fixed it. Same binary, same arm, six captures, no crash.
	 *
	 * THIS IS A LATENT BUG SPEC v23 §A2 EXPOSED, not one it introduced. LoadObject returns the object
	 * as soon as the package is in memory and the RHI texture lands a frame or two later. Until v23
	 * this overlay could not be on screen without the CANVAS title screen being on screen underneath
	 * it — that is the whole defect §A2 fixed — and that screen draws TraceMenuCanvasArt out of the
	 * same /Game/Trace/UI/Art package, earlier in the same DrawHUD, which happened to warm these ten
	 * textures before this page ever asked for one. Take the Canvas title screen away and this page
	 * is the first thing in the process to touch that package.
	 *
	 * The guard costs one pointer compare per sprite per frame and fails the way every other sprite
	 * failure on this page fails: Sprite() returns null, the caller draws the plain rectangle it drew
	 * before spec v20, for the one or two frames before the RHI texture exists.
	 */
	static bool IsDrawable(const UTexture2D* Tex)
	{
		if (Tex == nullptr)
		{
			return false;
		}
		const FTextureResource* Resource = Tex->GetResource();
		if (Resource == nullptr || !Resource->TextureRHI.IsValid())
		{
			// Loud once per sprite: this is the state that used to crash, so a build that starts
			// hitting it a lot is a build whose art is arriving later than this page draws.
			UE_LOG(LogTraceGame, Verbose, TEXT("[Options] '%s' is loaded but has no RHI texture yet; ")
				TEXT("drawing the plain rectangle this frame."), *Tex->GetName());
			return false;
		}
		return true;
	}

	/** The texture, or null — which every caller treats as "draw the rectangle you drew before". */
	static UTexture2D* Sprite(ESprite Which)
	{
		if (GEnabled == 0)
		{
			return nullptr;
		}

		const int32 Index = int32(Which);
		if (UTexture2D* Cached = GCache[Index].Get())
		{
			return IsDrawable(Cached) ? Cached : nullptr;
		}
		if (GFailed[Index])
		{
			return nullptr;
		}

		UTexture2D* Loaded = LoadObject<UTexture2D>(nullptr, SpritePaths[Index]);
		if (Loaded == nullptr)
		{
			// Once. A warning per frame per sprite in front of a paused match is its own defect.
			GFailed[Index] = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Options] Menu art '%s' did not load; that control keeps its plain rectangle."),
				SpritePaths[Index]);
			return nullptr;
		}

		// See point 3 in the block above: nothing else in a match holds these.
		Loaded->AddToRoot();
		GCache[Index] = Loaded;
		return IsDrawable(Loaded) ? Loaded : nullptr;
	}

	/** Plain stretch. For alpha masks and for anything whose corners are not being distorted. */
	static void Draw(AHUD* HUD, UTexture2D* Tex, float X, float Y, float W, float H, const FLinearColor& Tint)
	{
		HUD->DrawTexture(Tex, X, Y, W, H, 0.f, 0.f, 1.f, 1.f, Tint, BLEND_Translucent);
	}

	/**
	 * Three-slice in X: the two caps keep their shape, only the middle stretches.
	 *
	 * @param CapU   the cap as a fraction of the sprite's WIDTH (a texture coordinate)
	 * @param CapPx  the cap's width on screen, derived from the sprite's height scale by the caller
	 */
	static void Draw3H(AHUD* HUD, UTexture2D* Tex, float X, float Y, float W, float H,
		float CapU, float CapPx, const FLinearColor& Tint)
	{
		// A row narrower than two caps is not a layout this screen produces, but clamping is one line
		// and the alternative is the two caps drawing over each other back to front.
		const float Cap = FMath::Min(CapPx, W * 0.5f);
		const float MidW = W - Cap * 2.f;

		HUD->DrawTexture(Tex, X, Y, Cap, H, 0.f, 0.f, CapU, 1.f, Tint, BLEND_Translucent);
		if (MidW > 0.f)
		{
			HUD->DrawTexture(Tex, X + Cap, Y, MidW, H, CapU, 0.f, 1.f - CapU * 2.f, 1.f, Tint, BLEND_Translucent);
		}
		HUD->DrawTexture(Tex, X + W - Cap, Y, Cap, H, 1.f - CapU, 0.f, CapU, 1.f, Tint, BLEND_Translucent);
	}

	/**
	 * A sprite that was cut as a PLATE plus a margin of glow, in the sheet's own pixels.
	 *
	 * Mirrored from TraceMenuArtStyle::FSpriteFrame rather than included: that header describes Slate
	 * Box brushes, which do not exist on this side of the fence, and the numbers below are the sheet's
	 * and not the engine's. If the slicer's crop boxes change, both copies change.
	 */
	struct FPlateFrame
	{
		float PlateW;
		float PlateH;
		/** Sheet pixels of glow kept OUTSIDE the plate on every side. */
		float Glow;
		/** Sheet pixels from the sprite's edge to where the corner curve is fully open. */
		float Cap;

		float SpriteW() const { return PlateW + Glow * 2.f; }
	};

	/** The wide button: plate 4723x1230 inside a 4979x1486 crop, corner open by 428. */
	static const FPlateFrame ButtonFrame = { 4723.f, 1230.f, 128.f, 428.f };

	/** The chip beside a slider: ring 1034x538 inside a 1154x656 crop, corner open by 150. */
	static const FPlateFrame ValueFrame  = { 1034.f,  538.f,  60.f, 150.f };

	/**
	 * Draws @p Tex so that its PLATE lands exactly on (X, Y, W, H), with the glow overhanging outside.
	 *
	 * Forget the overhang and the plate comes out a fifth small inside its own row — which is the
	 * mistake the UMG row widget documents having made once already.
	 */
	static void DrawPlate(AHUD* HUD, UTexture2D* Tex, const FPlateFrame& Frame,
		float X, float Y, float W, float H, const FLinearColor& Tint)
	{
		// Height is the reference scale: the glow and the corner are square in the sheet, so scaling
		// both by H/PlateH is what keeps the corner circular however wide the row is.
		const float Scale = H / Frame.PlateH;
		const float GlowPx = Frame.Glow * Scale;
		const float CapPx = Frame.Cap * Scale;
		const float CapU = Frame.Cap / Frame.SpriteW();

		Draw3H(HUD, Tex, X - GlowPx, Y - GlowPx, W + GlowPx * 2.f, H + GlowPx * 2.f, CapU, CapPx, Tint);
	}

	// Sprite aspects, measured off the PNGs rather than guessed, so nothing here is stretched.
	static constexpr float CursorAspect  = 64.f / 87.f;
	static constexpr float HandleAspect  = 64.f / 87.f;
	static constexpr float ChevronAspect = 96.f / 125.f;
	static constexpr float KeybindAspect = 256.f / 42.f;
	static constexpr float KeyAspect     = 128.f / 47.f;

	/**
	 * The tip of T_MenuCursor, as a fraction of the sprite, measured from its alpha: the arrow's point
	 * is at about (11.5, 6.5) of 64x87.
	 *
	 * It matters because PollMouse hit-tests at CursorPos. A centre-anchored arrow would draw its
	 * point about eleven pixels away from the pixel it is about to click, and every click in every
	 * screenshot would look like it landed on the wrong row.
	 */
	static constexpr float CursorTipU = 0.180f;
	static constexpr float CursorTipV = 0.075f;

	/** The slider sprite is a trough: its solid rail occupies rows 6..17 of its 23. */
	static constexpr float TrackRailTopV = 6.f / 23.f;
	static constexpr float TrackRailV    = 11.f / 23.f;

	/**
	 * Draws T_MenuSliderTrack as a trough, AVOIDING THE HANDLE THAT IS BAKED INTO IT.
	 *
	 * MEASURED, because it cost a capture to find: the slicer's crop kept the artist's own handle
	 * blade inside the track sprite. Columns 37..78 of its 512 — 7.2% to 15.2% along — are a bright
	 * white diagonal, and a plain stretch therefore paints a SECOND, immovable handle at a fixed
	 * tenth of every slider, next to the real one. The first capture of this work had two blades on
	 * every row and it read as a rendering bug.
	 *
	 * So the middle is sampled from a clean band in the sprite's uniform centre rather than from the
	 * span between the caps. The caps themselves (16 px at each end, the artist's soft lip) are clear
	 * of the blade and are drawn as they were cut.
	 *
	 * The real fix is a re-cut in Scripts/slice-ui-assets.py, which is not this agent's file — this
	 * is a faithful presentation of the sprite as shipped, not a workaround hiding a bad asset.
	 */
	static void DrawTrough(AHUD* HUD, UTexture2D* Tex, float X, float Y, float W, float H, const FLinearColor& Tint)
	{
		constexpr float CapU = 16.f / 512.f;   // the lip, and it is clean
		constexpr float MidU = 0.30f;          // a band of the uniform centre, well past the blade
		constexpr float MidUW = 0.40f;

		const float Cap = FMath::Min(H * (16.f / 23.f), W * 0.5f);
		const float MidW = W - Cap * 2.f;

		HUD->DrawTexture(Tex, X, Y, Cap, H, 0.f, 0.f, CapU, 1.f, Tint, BLEND_Translucent);
		if (MidW > 0.f)
		{
			HUD->DrawTexture(Tex, X + Cap, Y, MidW, H, MidU, 0.f, MidUW, 1.f, Tint, BLEND_Translucent);
		}
		HUD->DrawTexture(Tex, X + W - Cap, Y, Cap, H, 1.f - CapU, 0.f, CapU, 1.f, Tint, BLEND_Translucent);
	}

	/**
	 * Resolves every sprite once and says out loud what landed.
	 *
	 * `Trace.UI.VerifyMenuArt` only ever asked the UMG title screen's brushes whether they had a
	 * texture; it has no opinion about this screen at all, and its own message still says these
	 * sprites "belong to the settings screen, which is still Canvas". So this is the only thing that
	 * can tell a reader of a log whether the pause menu in front of them is wearing the art or its
	 * fallbacks — and it prints the count both ways round, which is what makes a red arm readable.
	 */
	static void LogOnce()
	{
		static bool bLogged = false;
		if (bLogged)
		{
			return;
		}
		bLogged = true;

		int32 Resolved = 0;
		for (int32 Index = 0; Index < int32(ESprite::Count); ++Index)
		{
			if (Sprite(ESprite(Index)) != nullptr)
			{
				++Resolved;
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Options] Menu art: %d of %d sprites resolved (Trace.Menu.Art = %d). %s"),
			Resolved, int32(ESprite::Count), GEnabled,
			(GEnabled == 0)
				? TEXT("Art is OFF: every control is drawing the plain rectangle it drew before spec v20.")
				: ((Resolved == int32(ESprite::Count))
					? TEXT("Plates, slider, chips, KEYBIND/KEY and the cursor are the artist's, on both hosts.")
					: TEXT("Some controls are drawing their fallback rectangles; see the warnings above.")));
	}
}

namespace
{
	/**
	 * Every key a player is allowed to bind, built once.
	 *
	 * EKeys::GetAllKeys() is a few hundred entries including every gamepad axis and every gesture,
	 * and this list is walked once per frame during a rebind capture. Filtering it up front keeps
	 * that walk to the ~150 real buttons and, more importantly, means an axis can never be captured
	 * as a binding — Dash on "MouseX" would fire every time the player looked around.
	 */
	const TArray<FKey>& BindableKeys()
	{
		static TArray<FKey> Keys;
		if (Keys.Num() == 0)
		{
			TArray<FKey> All;
			EKeys::GetAllKeys(All);
			Keys.Reserve(All.Num());
			for (const FKey& Key : All)
			{
				if (UTraceUserSettings::IsBindableKey(Key))
				{
					Keys.Add(Key);
				}
			}
		}
		return Keys;
	}

	/**
	 * True if either key is being held OR was pressed this frame.
	 *
	 * The held test alone is what the repeat logic wants, but it MISSES a press and release that both
	 * land inside one frame — measured during the scripted settings drive, where a synthetic tap that
	 * straddled a stalled frame was silently dropped. A human hand holds a key for ~80ms, so this is
	 * rare in practice, but at a low frame rate a quick tap on an arrow key would do nothing at all,
	 * and "the menu ignored me" is exactly the impression an options screen must never give.
	 *
	 * Adding the edge cannot double-apply: the caller acts once when the direction BECOMES non-zero,
	 * and a press-and-released key reports non-zero for one frame and zero the next.
	 */
	bool AnyDown(const APlayerController* PC, const FKey& A, const FKey& B)
	{
		if (PC == nullptr)
		{
			return false;
		}
		return PC->IsInputKeyDown(A) || PC->IsInputKeyDown(B)
			|| PC->WasInputKeyJustPressed(A) || PC->WasInputKeyJustPressed(B);
	}

}

// =================================================================================================
// Dev access — opening a page without a keyboard
//
// A headless run has no way to press anything, so there is no way to CAPTURE the video page, and a
// menu page that cannot be captured is a menu page nobody can be shown to have checked. This is the
// same class of hole -TraceAutoPause was added to fill for the pause root.
//
// A raw pointer to the last overlay that ticked. Both hosts call Tick() every frame whether the
// overlay is open or not, so this is always the one the player is looking at; on a travel the two
// HUDs overlap for a frame and last-writer-wins, which for a dev command is the right answer anyway.
// Cleared in the destructor so a HUD destroyed by a travel cannot leave this dangling.
//
// NOT a CVar, and NOT in the Trace.Video.* namespace. This project fatals at module load if a CVar
// and a console command share a name, and Trace.Video.* already contains a CVar next door
// (Trace.Video.FOVAutoApply). Trace.Menu.* is unambiguously this file's.
// =================================================================================================

#if !UE_BUILD_SHIPPING
namespace
{
	FTraceOptionsMenu* GActiveOptionsMenu = nullptr;

	FAutoConsoleCommand CmdMenuVideo(
		TEXT("Trace.Menu.Video"),
		TEXT("Opens the VIDEO settings page on whichever HUD is up. Works on the title screen and in ")
		TEXT("a match. Exists so a headless run can screenshot the page: -TraceExec=\"Trace.Menu.Video\"."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->OpenVideo();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Video: no HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuSettings(
		TEXT("Trace.Menu.Settings"),
		TEXT("Opens the SETTINGS page on whichever HUD is up. Works on the title screen and in a match. ")
		TEXT("Twin of Trace.Menu.Video, and it exists for the same reason: until spec v20 there was no ")
		TEXT("headless way to photograph the page that has the most art on it, so nobody could show a ")
		TEXT("before and an after of it. -TraceExec=\"Trace.Menu.Settings\"."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->OpenSettings();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Settings: no HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuNudge(
		TEXT("Trace.Menu.Nudge"),
		TEXT("Trace.Menu.Nudge <rows-from-top> <delta>. Moves the selection and adjusts it, exactly as ")
		TEXT("the arrow keys would, then logs the row and its new value.\n")
		TEXT("This is the ONLY headless way to exercise the menu's own write path: every capture-only ")
		TEXT("test drives the settings through the Trace.Video.* commands instead, which proves the ")
		TEXT("settings class persists and proves nothing whatever about the rows on this page."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (GActiveOptionsMenu == nullptr || Args.Num() < 2)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Nudge <rows-from-top> <delta>"));
				return;
			}
			GActiveOptionsMenu->DebugNudge(FCString::Atoi(*Args[0]), FCString::Atoi(*Args[1]));
		}));
}
#endif

FTraceOptionsMenu::~FTraceOptionsMenu()
{
#if !UE_BUILD_SHIPPING
	if (GActiveOptionsMenu == this)
	{
		GActiveOptionsMenu = nullptr;
	}
#endif
}

#if !UE_BUILD_SHIPPING
void FTraceOptionsMenu::DebugNudge(int32 RowsFromTop, int32 Delta)
{
	if (!Rows.IsValidIndex(RowsFromTop))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Options] Nudge: row %d is out of range (%d rows)."),
			RowsFromTop, Rows.Num());
		return;
	}
	if (!Rows[RowsFromTop].IsSelectable())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Options] Nudge: row %d ('%s') is not selectable."),
			RowsFromTop, *Rows[RowsFromTop].Label);
		return;
	}

	Selected = RowsFromTop;

	const int32 Steps = FMath::Abs(Delta);
	const int32 Dir = (Delta >= 0) ? 1 : -1;
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		AdjustSelected(Dir);
	}

	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float StepSize = 1.f;
	GetSettingValue(Rows[Selected].Setting, Value, Min, Max, StepSize);

	UE_LOG(LogTraceGame, Display, TEXT("[Options] Nudge: '%s' is now %s."),
		*Rows[Selected].Label, *FormatSettingValue(Rows[Selected].Setting, Value));
}

bool FTraceOptionsMenu::DebugGetRowRect(const TCHAR* Label, FBox2D& OutRect) const
{
	for (const FRow& Row : Rows)
	{
		// bIsValid, not just a label match: FRow::Rect is only written by DrawRow, so a page that has
		// been rebuilt but not yet drawn would otherwise hand back a zero rect and the harness would
		// click at (0,0) and report a failure that is its own.
		if (Row.Rect.bIsValid && Row.Label.Equals(Label, ESearchCase::IgnoreCase))
		{
			OutRect = Row.Rect;
			return true;
		}
	}
	return false;
}
#endif

// =================================================================================================
// Lifecycle
// =================================================================================================

void FTraceOptionsMenu::OpenRoot()
{
	Page = EPage::Root;
	bSettingsIsRootPage = false;
	bCapturingKey = false;

	// The key press that opened us is still "just pressed" for the rest of this frame. See
	// IgnoreInputBeforeFrame in the header.
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Pause menu opened."));
}

void FTraceOptionsMenu::OpenSettings()
{
	Page = EPage::Settings;
	bSettingsIsRootPage = true;
	bCapturingKey = false;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Settings opened."));
}

void FTraceOptionsMenu::OpenVideo()
{
	Page = EPage::Video;
	bCapturingKey = false;

	// Closed, not Settings: this entry point IS the top of the stack, so BACK has to close rather
	// than drop the player onto a settings page they never asked for.
	VideoReturnPage = EPage::Closed;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Video settings opened."));
}

void FTraceOptionsMenu::Close()
{
	if (Page == EPage::Closed)
	{
		return;
	}

	// Before the page goes away: a resolution or window-mode change the player made and then escaped
	// out of is still a change they made. Dropping it would mean the value they can see in the .ini
	// next launch is not the one the window is running at.
	if (bResolutionApplyPending)
	{
		ApplyVideo(/*bResolutionAffecting=*/true, /*bPersist=*/true);
	}

	Page = EPage::Closed;
	bCapturingKey = false;
	CapturingAction = ETraceInputAction::Count;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bAutoDetectPending = false;
	LastAdjustDir = 0;
	LastNavDir = 0;

	UE_LOG(LogTraceGame, Display, TEXT("[Options] Closed."));

	if (OnClosed)
	{
		OnClosed();
	}
}

// =================================================================================================
// Rows
// =================================================================================================

void FTraceOptionsMenu::RebuildRows()
{
	Rows.Reset();

	auto AddHeader = [this](const TCHAR* Label)
	{
		FRow Row;
		Row.Kind = ERowKind::Header;
		Row.Label = Label;
		Rows.Add(MoveTemp(Row));
	};

	auto AddAction = [this](const TCHAR* Label, EAction Action)
	{
		FRow Row;
		Row.Kind = ERowKind::Action;
		Row.Label = Label;
		Row.Action = Action;
		Rows.Add(MoveTemp(Row));
	};

	auto AddNote = [this](const TCHAR* Label)
	{
		FRow Row;
		Row.Kind = ERowKind::Note;
		Row.Label = Label;
		Rows.Add(MoveTemp(Row));
	};

	auto AddValue = [this](ERowKind Kind, const TCHAR* Label, ESetting Setting)
	{
		FRow Row;
		Row.Kind = Kind;
		Row.Label = Label;
		Row.Setting = Setting;
		Rows.Add(MoveTemp(Row));
	};

	if (Page == EPage::Root)
	{
		// Only offered when the host supplied somewhere to go. The title screen has no RESUME.
		if (OnResume)         { AddAction(TEXT("RESUME"), EAction::Resume); }
		AddAction(TEXT("SETTINGS"), EAction::OpenSettings);

		// Its own row on the pause root rather than only inside SETTINGS. Spec v11 §0: the player
		// this feature exists for is one whose frame rate has collapsed, and making them walk past
		// mouse sensitivity and eleven key bindings to reach the resolution scale is exactly the kind
		// of burial that left the collaborator with no way to improve anything.
		AddAction(TEXT("VIDEO"), EAction::OpenVideo);

		if (OnReturnToTitle)  { AddAction(TEXT("RETURN TO TITLE"), EAction::ReturnToTitle); }
		if (OnQuit)           { AddAction(TEXT("QUIT"), EAction::Quit); }
	}
	else if (Page == EPage::Video)
	{
		// ---- Performance first ------------------------------------------------------------------
		//
		// The order on this page is an argument, not a taxonomy. Spec v11 §0 measured the frame as
		// GPU-bound PER PIXEL — instancing the arena removed 893 draw calls and bought 1.4% — so the
		// number of pixels is the dominant term and the two controls that change it come first,
		// above the mode, the resolution and all nine quality groups.
		AddHeader(TEXT("PERFORMANCE"));
		AddValue(ERowKind::Slider, TEXT("RESOLUTION SCALE"), ESetting::ResolutionScale);
		AddNote(TEXT("THE BIGGEST WIN IF THE GAME RUNS SLOW. THIS FRAME IS LIMITED BY PIXELS."));
		AddAction(TEXT("AUTO-DETECT QUALITY"), EAction::AutoDetectQuality);
		AddValue(ERowKind::Choice, TEXT("OVERALL QUALITY"), ESetting::OverallQuality);

		// ---- Display ----------------------------------------------------------------------------
		AddHeader(TEXT("DISPLAY"));
		AddValue(ERowKind::Choice, TEXT("WINDOW MODE"), ESetting::WindowMode);
		AddValue(ERowKind::Choice, TEXT("RESOLUTION"), ESetting::Resolution);
		AddValue(ERowKind::Toggle, TEXT("VSYNC"), ESetting::VSync);
		AddValue(ERowKind::Choice, TEXT("FRAME RATE LIMIT"), ESetting::FrameRateLimit);
		AddValue(ERowKind::Slider, TEXT("FIELD OF VIEW"), ESetting::FieldOfView);

		// ---- The nine groups --------------------------------------------------------------------
		AddHeader(TEXT("QUALITY"));
		AddValue(ERowKind::Choice, TEXT("VIEW DISTANCE"), ESetting::QualityViewDistance);
		AddValue(ERowKind::Choice, TEXT("ANTI-ALIASING"), ESetting::QualityAntiAliasing);
		AddValue(ERowKind::Choice, TEXT("POST PROCESSING"), ESetting::QualityPostProcess);
		AddValue(ERowKind::Choice, TEXT("SHADOWS"), ESetting::QualityShadows);
		AddValue(ERowKind::Choice, TEXT("GLOBAL ILLUMINATION"), ESetting::QualityGlobalIllumination);
		AddValue(ERowKind::Choice, TEXT("REFLECTIONS"), ESetting::QualityReflections);
		AddValue(ERowKind::Choice, TEXT("TEXTURES"), ESetting::QualityTextures);
		AddValue(ERowKind::Choice, TEXT("EFFECTS"), ESetting::QualityEffects);
		AddValue(ERowKind::Choice, TEXT("SHADING"), ESetting::QualityShading);

		AddHeader(TEXT(""));
		AddAction(TEXT("RESET TO DEFAULTS"), EAction::ResetVideoDefaults);
		AddAction(TEXT("BACK"), EAction::Back);
	}
	else if (Page == EPage::Settings)
	{
		// First row on the page, above the mouse. Same reasoning as the pause root's VIDEO entry —
		// and this is the ONLY route to the video page from the title screen, where there is no
		// pause root at all, so it cannot be buried at the bottom next to RESET.
		AddHeader(TEXT("DISPLAY"));
		AddAction(TEXT("VIDEO SETTINGS"), EAction::OpenVideo);

		// ---- Match rules (spec v14 §3) ----------------------------------------------------------
		//
		// Above MOUSE and above CONTROLS, because it is the only row on this page that changes what
		// the GAME is rather than how it is driven, and because the one thing a player is looking for
		// when they come here about characters is the switch that turns them off.
		//
		// The note is not optional. This row cannot retro-apply to a match already being served by
		// somebody else's machine, and a toggle that appears to do nothing is worse than no toggle —
		// the player needs to be told it is a HOST setting and that it lands on the next match.
		AddHeader(TEXT("MATCH"));
		AddValue(ERowKind::Toggle, TEXT("CHARACTERS"), ESetting::CharactersEnabled);
		AddNote(TEXT("OFF: EVERYONE PLAYS THE DEFAULT MANNEQUIN, NO ABILITIES, NO SELECT SCREEN."));
		AddNote(TEXT("APPLIES TO MATCHES YOU HOST, FROM THE NEXT MATCH. GOALS MODE ONLY."));

		AddHeader(TEXT("MOUSE"));

		{
			FRow Row;
			Row.Kind = ERowKind::Slider;
			Row.Label = TEXT("SENSITIVITY");
			Row.Setting = ESetting::Sensitivity;
			Rows.Add(MoveTemp(Row));
		}
		{
			FRow Row;
			Row.Kind = ERowKind::Slider;
			Row.Label = TEXT("VERTICAL SENSITIVITY");
			Row.Setting = ESetting::SensitivityY;
			Rows.Add(MoveTemp(Row));
		}
		{
			FRow Row;
			Row.Kind = ERowKind::Toggle;
			Row.Label = TEXT("INVERT MOUSE Y");
			Row.Setting = ESetting::InvertY;
			Rows.Add(MoveTemp(Row));
		}

		AddHeader(TEXT("CONTROLS"));

		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			FRow Row;
			Row.Kind = ERowKind::Binding;
			Row.Label = Info.DisplayName;
			Row.Binding = Info.Action;
			Rows.Add(MoveTemp(Row));
		}

		AddHeader(TEXT(""));
		AddAction(TEXT("RESET TO DEFAULTS"), EAction::ResetDefaults);
		AddAction(TEXT("BACK"), EAction::Back);
	}

	// Before picking a selection, not after: a row that is greyed out right now is not somewhere the
	// highlight may land, and RESOLUTION is the first selectable row on the video page's DISPLAY
	// block whenever the window mode is not windowed fullscreen.
	RefreshRowStates();

	// Land on the first thing that can actually be selected, so a page never opens with the
	// highlight sitting on a caption.
	Selected = 0;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].IsSelectable())
		{
			Selected = Index;
			break;
		}
	}
}

void FTraceOptionsMenu::RefreshRowStates()
{
	// Only one rule so far, and it is worth stating rather than generalising: in windowed fullscreen
	// the window always takes the desktop's size, so a stored resolution is accepted, saved, and then
	// ignored by the platform. A row that takes input and changes nothing is the worst kind of
	// control, so it is greyed and its value reads DESKTOP. IsResolutionSelectable is the settings
	// class's own answer to that question, so the two files cannot disagree about it.
	const bool bResolutionMeaningful = (Video() == nullptr) || Video()->IsResolutionSelectable();

	for (FRow& Row : Rows)
	{
		if (Row.Setting == ESetting::Resolution)
		{
			Row.bEnabled = bResolutionMeaningful;
		}
	}

	// The selection may have been sitting on a row that just went grey — switching to windowed
	// fullscreen while RESOLUTION is highlighted does exactly that. Walk down, then up.
	if (Rows.IsValidIndex(Selected) && !Rows[Selected].IsSelectable())
	{
		for (int32 Index = Selected + 1; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].IsSelectable()) { Selected = Index; return; }
		}
		for (int32 Index = Selected - 1; Index >= 0; --Index)
		{
			if (Rows[Index].IsSelectable()) { Selected = Index; return; }
		}
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void FTraceOptionsMenu::Tick(AHUD* HUD, APlayerController* PC, float InViewW, float InViewH, float InUIScale, float InNow)
{
#if !UE_BUILD_SHIPPING
	// Claimed every frame, open or not, so Trace.Menu.Video always reaches the overlay the player is
	// actually looking at. See GActiveOptionsMenu.
	GActiveOptionsMenu = this;
#endif

	// ---- Runs even while the overlay is CLOSED ---------------------------------------------------
	//
	// Both hosts call Tick unconditionally every frame and rely on it being a no-op while closed, so
	// this is the one hook the video settings have into an ordinary gameplay frame — and field of
	// view needs exactly that. ATraceCharacter::ATraceCharacter sets the camera's FOV once, in the
	// constructor, which means a fresh launch and every single respawn both come back at 95 degrees
	// no matter what the player saved. Reasserting it here costs one weak-pointer compare and one
	// float compare per frame, and it is the difference between the row persisting and the row
	// appearing to persist until the player next dies.
	MaintainFieldOfView(PC);

	if (Page == EPage::Closed || HUD == nullptr || InViewW <= 0.f || InViewH <= 0.f)
	{
		return;
	}

	// Sampled every frame the page is up, before anything else can spend time. See UpdatePerfReadout
	// for why this uses real time rather than the InNow the host passes in.
	UpdatePerfReadout();

	// Deferred from the click one frame ago so that "MEASURING…" was actually on screen for the
	// second the benchmark spends blocking the game thread. See bAutoDetectPending.
	if (bAutoDetectPending)
	{
		bAutoDetectPending = false;
		RunAutoDetect();
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

	// Before input, so a click cannot land on a row that stopped being meaningful last frame.
	RefreshRowStates();

	// Input first, then draw, so a value changed this frame is the value the player sees this frame.
	// The row rects the mouse tests against are from the PREVIOUS draw, which is correct: they are
	// where the player was actually looking when they clicked.
	PollInput(PC);

	// PollInput can close us (Escape, RESUME, QUIT). Drawing a closed overlay would leave a frame of
	// dimmed screen over a game that has already resumed.
	if (Page == EPage::Closed)
	{
		return;
	}

	// The coalesced window resize, once the player has stopped moving through the list.
	if (bResolutionApplyPending && Now >= ResolutionApplyAtTime)
	{
		ApplyVideo(/*bResolutionAffecting=*/true, /*bPersist=*/true);
		RefreshRowStates();
	}

#if !UE_BUILD_SHIPPING
	// After input and before the draw, so the page a capture photographs is the page this frame
	// actually drew. Counted in DRAWN FRAMES - see the header for why seconds cannot work here.
	++DrawsSinceOpen;
	TickAutoActivate();

	// It presses a real row, and a real row can be BACK or QUIT. Same guard, same reason, as the one
	// after PollInput above: drawing a closed overlay leaves a frame of dimmed screen over a game
	// that has already resumed.
	if (Page == EPage::Closed)
	{
		return;
	}
#endif

	// ---- SPEC v23 §A2 — IN FRONT OF SLATE, ON BOTH HOSTS -----------------------------------------
	//
	// Held for the draw only, never across the input poll above: PollInput can close the page, and a
	// scope that outlived it would leave the game's canvas pointer swapped on a frame that drew
	// nothing.
	//
	// This one line is what reaches the IN-MATCH pause menu as well, without a line changing in
	// TraceHUD.cpp. Both hosts call this Tick, so both of them get a panel that draws over their UMG
	// screens instead of under them — the title screen's over WBP_TitleMenu, the pause menu's over
	// the in-match corner widgets. The owner asked for the art to reach the in-game menus; this is
	// the same requirement one layer down, and it would have been wrong to fix it only on the way in.
	//
	// Nothing below branches on the answer. When it is false the panel draws precisely where it drew
	// before, and it is the HOST that adapts (see ATraceMenuHUD::DrawHUD).
	const FTraceOverSlateCanvas OverSlate(HUD, ViewW, ViewH);
	LogSurfaceOnce(OverSlate.IsElevated());

	Draw(HUD);
}

void FTraceOptionsMenu::LogSurfaceOnce(bool bElevated) const
{
	// Once per process per answer. This is the line that says whether the fix is live in a session
	// somebody else is looking at, and it has to be greppable out of a log without a debugger — a
	// modal that quietly fell back to the scene canvas looks like a modal that works, right up until
	// somebody notices the title screen behind it changed design.
	static int32 LastLogged = -1;
	const int32 Answer = bElevated ? 1 : 0;
	if (LastLogged != Answer)
	{
		LastLogged = Answer;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Modal surface: %s"),
			*FTraceOverSlateCanvas::LastStatus());
	}
}

#if !UE_BUILD_SHIPPING
void FTraceOptionsMenu::TickAutoActivate()
{
	// Parsed once per process. FParse over the whole command line every frame in front of a paused
	// match would be its own small crime.
	static bool bParsed = false;
	static FString WantedRow;
	if (!bParsed)
	{
		bParsed = true;
		FParse::Value(FCommandLine::Get(), TEXT("TraceMenuActivate="), WantedRow);
	}

	// Twelve drawn frames: long enough that the opening frame's IgnoreInputBeforeFrame guard and the
	// first layout have both passed, and comfortably inside ATraceHUD's own auto-pause capture, which
	// fires twenty draws after the menu appears. Order matters - the page has to change BEFORE the
	// screenshot, or the capture is of the root again.
	if (WantedRow.IsEmpty() || bAutoActivateDone || DrawsSinceOpen < 12)
	{
		return;
	}
	bAutoActivateDone = true;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].IsSelectable() && Rows[Index].Label.Equals(WantedRow, ESearchCase::IgnoreCase))
		{
			Selected = Index;
			UE_LOG(LogTraceGame, Display,
				TEXT("[Options] -TraceMenuActivate: pressing row %d ('%s')."), Index, *Rows[Index].Label);

			// Through the real activation, not around it. A harness that set Page directly would
			// photograph a page no key press can reach.
			ActivateSelected();
			return;
		}
	}

	UE_LOG(LogTraceGame, Warning,
		TEXT("[Options] -TraceMenuActivate=%s: no selectable row on this page carries that label."),
		*WantedRow);
}
#endif

// =================================================================================================
// Input
// =================================================================================================

void FTraceOptionsMenu::PollInput(APlayerController* PC)
{
	if (PC == nullptr || GFrameCounter < IgnoreInputBeforeFrame)
	{
		return;
	}

	if (bCapturingKey)
	{
		// *** SPEC v10 §8 — WHY MOUSE BUTTONS "COULD NOT BE BOUND". ***
		//
		// IsBindableKey never rejected them; it could not, the shipped defaults ARE LMB and RMB. The
		// defect was a STALE MOUSE EDGE manufactured on the way out of this capture.
		//
		// A capture is entered on a mouse RELEASE, so bMouseWasDown is false at that moment. PollMouse
		// then does not run for the whole capture — the branch below returns before it. The player
		// presses LMB to bind it, PollKeyCapture calls SetKey and closes the capture, and the player
		// is STILL PHYSICALLY HOLDING THE BUTTON. The first frame PollMouse runs again it compares
		// bDown=true against a bMouseWasDown that has been false since before the capture opened,
		// invents a press edge that never happened, arms PressedRow on the row under the cursor —
		// which is the binding row the player just used — and activates it on the real release. The
		// capture re-opens. Every subsequent click does it again, which reads exactly as "this row
		// refuses to take a mouse button".
		//
		// IgnoreInputBeforeFrame = GFrameCounter + 1 was the old defence and it is not enough by an
		// order of magnitude: it buys ONE frame, and a human holds a mouse button ~80 ms, about five.
		//
		// THE FIX IS AN INVARIANT, NOT A PATCH AT THE EXIT SITES. bMouseWasDown means "the button
		// state last frame", so it must be maintained on EVERY frame this function runs, including
		// the frames a capture is swallowing input. Then no edge can be synthesised across the
		// capture at all — not on the SetKey path, not on the Escape-cancel path, and not on any
		// third exit somebody adds later. Read it before PollKeyCapture, so the frame that closes the
		// capture is recorded with the button still down.
		bMouseWasDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);

		// Nothing may stay armed across a capture. Even with the edge fixed, a PressedRow armed by
		// the click that OPENED the capture would fire its activation on the next release.
		PressedRow = INDEX_NONE;
		bDraggingSlider = false;

		// A capture swallows everything. Navigating away mid-rebind would leave the player unsure
		// which action their next key press was about to land on.
		PollKeyCapture(PC);
		return;
	}

	PollNavigation(PC);
	PollMouse(PC);
}

void FTraceOptionsMenu::PollKeyCapture(APlayerController* PC)
{
	// Escape is filtered out of BindableKeys precisely so it can mean "cancel" here and nothing else.
	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		bCapturingKey = false;
		CapturingAction = ETraceInputAction::Count;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Rebind cancelled."));
		return;
	}

	for (const FKey& Key : BindableKeys())
	{
		if (!PC->WasInputKeyJustPressed(Key))
		{
			continue;
		}

		UTraceUserSettings::Get().SetKey(CapturingAction, Key);
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Bound %s to '%s'."),
			TraceInputActions::Info(CapturingAction).DisplayName, *UTraceUserSettings::DescribeKey(Key));

		bCapturingKey = false;
		CapturingAction = ETraceInputAction::Count;

		// One more frame of quiet: the key that was just bound is still down, and if it happens to be
		// Enter or a mouse button the very next poll would read it as "activate this row again".
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		return;
	}
}

void FTraceOptionsMenu::PollNavigation(APlayerController* PC)
{
	// ---- Vertical: move the selection -----------------------------------------------------------
	int32 NavDir = 0;
	if (AnyDown(PC, EKeys::Down, EKeys::S)) { NavDir += 1; }
	if (AnyDown(PC, EKeys::Up,   EKeys::W)) { NavDir -= 1; }

	if (NavDir != 0)
	{
		if (NavDir != LastNavDir)
		{
			// Direction just became held: act immediately, then wait out the repeat delay.
			MoveSelection(NavDir);
			NextNavTime = Now + RepeatDelay;
		}
		else if (Now >= NextNavTime)
		{
			MoveSelection(NavDir);
			NextNavTime = Now + NavRepeatInterval;
		}
	}
	LastNavDir = NavDir;

	// ---- Horizontal: adjust the selected row ----------------------------------------------------
	int32 AdjustDir = 0;
	if (AnyDown(PC, EKeys::Right, EKeys::D)) { AdjustDir += 1; }
	if (AnyDown(PC, EKeys::Left,  EKeys::A)) { AdjustDir -= 1; }

	if (AdjustDir != 0)
	{
		if (AdjustDir != LastAdjustDir)
		{
			AdjustSelected(AdjustDir);
			NextAdjustTime = Now + RepeatDelay;
		}
		else if (Now >= NextAdjustTime)
		{
			AdjustSelected(AdjustDir);
			NextAdjustTime = Now + RepeatInterval;
		}
	}
	LastAdjustDir = AdjustDir;

	// ---- Buttons --------------------------------------------------------------------------------
	if (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::SpaceBar))
	{
		ActivateSelected();
		return;
	}

	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		GoBack();
		return;
	}

	// Explicit unbind. Every options screen that lets you bind should let you UNbind, and without it
	// there is no way to express "I do not want a parry key" short of hiding it under some other one.
	if (PC->WasInputKeyJustPressed(EKeys::BackSpace) || PC->WasInputKeyJustPressed(EKeys::Delete))
	{
		if (Rows.IsValidIndex(Selected) && Rows[Selected].Kind == ERowKind::Binding)
		{
			// ClearKey, not SetKey: SetKey refuses an invalid key on purpose, because "invalid" is
			// what an unparseable .ini entry looks like and it must never be able to wipe a binding.
			// Unbinding is a separate, explicit intent.
			UTraceUserSettings::Get().ClearKey(Rows[Selected].Binding);
		}
	}
}

void FTraceOptionsMenu::PollMouse(APlayerController* PC)
{
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (PC->GetMousePosition(MouseX, MouseY))
	{
		CursorPos = FVector2D(MouseX, MouseY);
		bHasCursor = true;
	}

	const bool bDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);
	const bool bJustPressed = bDown && !bMouseWasDown;
	const bool bJustReleased = !bDown && bMouseWasDown;
	bMouseWasDown = bDown;

	if (!bHasCursor)
	{
		return;
	}

	// Hover follows the pointer, but ONLY when the pointer actually moved.
	//
	// The guard is not about window-activation clicks (an earlier comment here claimed that and used
	// it to justify having no guard). It is about the keyboard: this runs AFTER PollNavigation, so
	// without it an arrow key moved Selected and the very same frame a STATIONARY cursor resting
	// over a row dragged it straight back. The measured symptom was that the arrow keys did nothing
	// whatsoever whenever the pointer happened to be over the list — and it is also why the
	// -TraceAutoSettings script landed on different rows depending on viewport size.
	const float CursorMoveThresholdSq = 4.f;   // 2 px; below that it is jitter, not intent
	const bool bCursorMoved = !bHasHoverCursorPos
		|| FVector2D::DistSquared(CursorPos, LastHoverCursorPos) > CursorMoveThresholdSq;
	LastHoverCursorPos = CursorPos;
	bHasHoverCursorPos = true;

	int32 HoverRow = INDEX_NONE;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].IsSelectable() && Rows[Index].Rect.bIsValid && Rows[Index].Rect.IsInside(CursorPos))
		{
			HoverRow = Index;
			break;
		}
	}

	// bCursorMoved, or the keyboard cannot win an argument with a resting pointer. A click still
	// selects regardless — that path reads HoverRow directly below.
	if (HoverRow != INDEX_NONE && !bDraggingSlider && bCursorMoved)
	{
		Selected = HoverRow;
	}

	if (bJustPressed)
	{
		PressedRow = HoverRow;
		bDraggingSlider = false;

		// Grabbing the track is a drag, not a click: the value follows the pointer from this frame on
		// and no activation happens on release. This is the single most useful interaction on the
		// whole screen, because "too sensitive" is found by sweeping, not by stepping.
		if (HoverRow != INDEX_NONE && Rows[HoverRow].Kind == ERowKind::Slider && Rows[HoverRow].Track.bIsValid)
		{
			const FBox2D& Track = Rows[HoverRow].Track;
			// Generous vertical tolerance: the track is a few pixels tall and the row is not.
			if (CursorPos.X >= Track.Min.X - 4.f && CursorPos.X <= Track.Max.X + 4.f)
			{
				bDraggingSlider = true;
			}
		}
	}

	if (bDown && bDraggingSlider && Rows.IsValidIndex(Selected) && Rows[Selected].Track.bIsValid)
	{
		const FBox2D& Track = Rows[Selected].Track;
		const float Width = FMath::Max(1.f, Track.Max.X - Track.Min.X);
		SetSettingNormalised(Rows[Selected].Setting, (CursorPos.X - Track.Min.X) / Width);
	}

	if (bJustReleased)
	{
		const int32 Armed = PressedRow;
		PressedRow = INDEX_NONE;

		if (bDraggingSlider)
		{
			// The drag already wrote every intermediate value; the release only ends it — and writes
			// the one .ini flush the whole gesture is allowed.
			bDraggingSlider = false;

			const ESetting Dragged = Rows.IsValidIndex(Selected) ? Rows[Selected].Setting : ESetting::None;
			if (IsVideoSetting(Dragged))
			{
				ApplyVideo(/*bResolutionAffecting=*/false, /*bPersist=*/true);
			}
			else
			{
				UTraceUserSettings::Get().Save();
			}
			return;
		}

		// Press and release must land on the same row — ordinary button behaviour, and the same rule
		// the title screen uses.
		if (Armed != INDEX_NONE && Armed == HoverRow)
		{
			Selected = Armed;
			ActivateSelected();
		}
	}
}

void FTraceOptionsMenu::MoveSelection(int32 Delta)
{
	if (Rows.Num() == 0 || Delta == 0)
	{
		return;
	}

	int32 Index = Selected;
	for (int32 Guard = 0; Guard < Rows.Num(); ++Guard)
	{
		Index += Delta;
		if (!Rows.IsValidIndex(Index))
		{
			// Clamp rather than wrap. On a fifteen-row list, wrapping from BACK to SENSITIVITY reads
			// as the menu having jumped somewhere on its own.
			return;
		}
		if (Rows[Index].IsSelectable())
		{
			Selected = Index;
			return;
		}
	}
}

void FTraceOptionsMenu::GetSettingValue(ESetting Setting, float& OutValue, float& OutMin, float& OutMax, float& OutStep) const
{
	// Set before the switch, not in a default case, so that every early return below — and any case
	// somebody adds later that forgets one of the four — still hands back a coherent 0..1 range
	// instead of whatever the caller happened to have on its stack.
	OutValue = 0.f;
	OutMin = 0.f;
	OutMax = 1.f;
	OutStep = 1.f;

	const UTraceUserSettings& Settings = UTraceUserSettings::Get();

	switch (Setting)
	{
	case ESetting::Sensitivity:
		OutValue = Settings.MouseSensitivity;
		OutMin = UTraceUserSettings::MinSensitivity;
		OutMax = UTraceUserSettings::MaxSensitivity;
		OutStep = 0.05f;
		break;

	case ESetting::SensitivityY:
		OutValue = Settings.MouseSensitivityYScale;
		OutMin = UTraceUserSettings::MinSensitivityYScale;
		OutMax = UTraceUserSettings::MaxSensitivityYScale;
		OutStep = 0.05f;
		break;

	case ESetting::InvertY:
		OutValue = Settings.bInvertMouseY ? 1.f : 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;

	// Spec v14 §3. Read live from its own storage — see TraceCharacters in UI/TraceMatchOptions.h.
	// It is neither a UTraceUserSettings value nor a video value, which is why it is answered here
	// and then falls through the IsVideoSetting() early-out below untouched.
	case ESetting::CharactersEnabled:
		OutValue = TraceCharacters::GetEnabledSetting() ? 1.f : 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;

	default:
		break;
	}

	if (!IsVideoSetting(Setting))
	{
		return;
	}

	// ---- Video ----------------------------------------------------------------------------------
	//
	// Ranges come from UTraceGameUserSettings' own constants and option arrays, never from a number
	// written down again here. A menu that clamps a slider to a range the settings class does not
	// share is a menu that will one day refuse to reach a value the settings class allows.
	const UTraceGameUserSettings* GUS = Video();
	if (GUS == nullptr)
	{
		return;
	}

	static_assert(
		int32(ESetting::QualityShading) - int32(ESetting::QualityViewDistance) + 1 == int32(ETraceQualityGroup::Count),
		"ESetting's quality rows and ETraceQualityGroup have diverged. See QualityGroupIndex.");

	if (IsQualityGroup(Setting))
	{
		OutMin = float(UTraceGameUserSettings::MinQualityLevel);
		OutMax = float(UTraceGameUserSettings::MaxQualityLevel);
		OutStep = 1.f;
		OutValue = float(GUS->GetGroupQuality(ETraceQualityGroup(QualityGroupIndex(Setting))));
		return;
	}

	switch (Setting)
	{
	case ESetting::ResolutionScale:
		OutMin = float(UTraceGameUserSettings::MinResolutionScalePercent);
		OutMax = float(UTraceGameUserSettings::MaxResolutionScalePercent);
		// 5% steps: eleven stops across the range, every one of them a round number a player can
		// report back ("I'm at 70") and a second player can reproduce exactly.
		OutStep = 5.f;
		OutValue = float(GUS->GetResolutionScalePercent());
		break;

	case ESetting::OverallQuality:
	{
		const ETraceVideoQuality Quality = GUS->GetOverallQuality();
		const bool bCustom = (Quality == ETraceVideoQuality::Custom);

		OutMin = float(UTraceGameUserSettings::MinQualityLevel);
		OutStep = 1.f;
		// CUSTOM sits one past EPIC and is reachable only by BEING there — it is what the nine rows
		// below report when they disagree, not something a player can ask for. So the range stops at
		// EPIC unless we are already in it, and right-arrow on EPIC clamps rather than stepping into
		// a state that would mean nothing if it were set.
		OutMax = float(UTraceGameUserSettings::MaxQualityLevel + (bCustom ? 1 : 0));
		OutValue = float(int32(Quality));
		break;
	}

	case ESetting::WindowMode:
	{
		const TArray<EWindowMode::Type>& Modes = UTraceGameUserSettings::GetWindowModeOptions();
		OutMin = 0.f;
		OutMax = float(FMath::Max(0, Modes.Num() - 1));
		OutStep = 1.f;
		OutValue = float(FMath::Max(0, Modes.IndexOfByKey(GUS->GetWindowMode())));
		break;
	}

	case ESetting::Resolution:
	{
		const int32 Index = GUS->GetResolutionOptionIndex();
		OutMin = 0.f;
		OutMax = float(FMath::Max(0, GUS->GetResolutionOptions().Num() - 1));
		OutStep = 1.f;
		// INDEX_NONE means the current mode is not in the list — a hand-edited ini, or a monitor
		// change since it was written. Showing entry 0 would be a lie; FormatSettingValue prints the
		// real size instead, and the first arrow press moves to a mode that IS in the list.
		OutValue = float(Index == INDEX_NONE ? 0 : Index);
		break;
	}

	case ESetting::VSync:
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		OutValue = GUS->IsVSyncEnabled() ? 1.f : 0.f;
		break;

	case ESetting::FrameRateLimit:
		OutMin = 0.f;
		OutMax = float(FMath::Max(0, UTraceGameUserSettings::GetFrameRateLimitOptions().Num() - 1));
		OutStep = 1.f;
		OutValue = float(GUS->GetFrameRateLimitIndex());
		break;

	case ESetting::FieldOfView:
		OutMin = UTraceGameUserSettings::MinFieldOfView;
		OutMax = UTraceGameUserSettings::MaxFieldOfView;
		OutStep = 1.f;
		OutValue = GUS->GetFieldOfView();
		break;

	default:
		break;
	}
}

FString FTraceOptionsMenu::FormatSettingValue(ESetting Setting, float Value) const
{
	// Every label a video row prints comes from UTraceGameUserSettings' own Describe* helpers. This
	// file does not get to decide what "EPIC" or "UNLIMITED" is called — the settings class prints
	// the same strings into its log and its Trace.Video.Status output, and a menu that spelled them
	// differently would make a bug report and the log it came with impossible to line up.
	if (IsQualityGroup(Setting))
	{
		return UTraceGameUserSettings::DescribeQualityLevel(FMath::RoundToInt(Value));
	}

	switch (Setting)
	{
	case ESetting::ResolutionScale:
		return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Value));

	case ESetting::FieldOfView:
		// No degree sign: AHUD::DrawText goes through the engine's bitmap fonts, whose glyph pages
		// are ASCII, and a missing glyph draws as a blank box that reads as a rendering fault.
		return FString::Printf(TEXT("%d DEG"), FMath::RoundToInt(Value));

	case ESetting::OverallQuality:
		return UTraceGameUserSettings::DescribeOverallQuality(ETraceVideoQuality(FMath::RoundToInt(Value)));

	case ESetting::WindowMode:
	{
		const TArray<EWindowMode::Type>& Modes = UTraceGameUserSettings::GetWindowModeOptions();
		const int32 Index = FMath::RoundToInt(Value);
		return Modes.IsValidIndex(Index)
			? UTraceGameUserSettings::DescribeWindowMode(Modes[Index])
			: FString(TEXT("N/A"));
	}

	case ESetting::Resolution:
	{
		const UTraceGameUserSettings* GUS = Video();
		if (GUS == nullptr)
		{
			return TEXT("N/A");
		}

		if (!GUS->IsResolutionSelectable())
		{
			// Not the stored size greyed out — that would still be a claim, and a false one. In
			// windowed fullscreen the window takes the desktop's size whatever this is set to, so
			// the row says what is actually true.
			return TEXT("DESKTOP");
		}

		const TArray<FTraceResolutionOption>& Options = GUS->GetResolutionOptions();
		const int32 Index = FMath::RoundToInt(Value);

		// GetResolutionOptionIndex returned INDEX_NONE — the running mode is not one of the offered
		// ones. Print the truth about the window rather than the label of a row we are not on.
		if (GUS->GetResolutionOptionIndex() == INDEX_NONE)
		{
			const FIntPoint Current = GUS->GetScreenResolution();
			return FString::Printf(TEXT("%d x %d  (CUSTOM)"), Current.X, Current.Y);
		}

		return Options.IsValidIndex(Index) ? Options[Index].Label : FString(TEXT("N/A"));
	}

	case ESetting::VSync:
	case ESetting::InvertY:
	case ESetting::CharactersEnabled:
		return (Value >= 0.5f) ? TEXT("ON") : TEXT("OFF");

	case ESetting::FrameRateLimit:
	{
		const TArray<float>& Limits = UTraceGameUserSettings::GetFrameRateLimitOptions();
		const int32 Index = FMath::RoundToInt(Value);
		return Limits.IsValidIndex(Index)
			? UTraceGameUserSettings::DescribeFrameRateLimit(Limits[Index])
			: FString(TEXT("N/A"));
	}

	default:
		return FString::Printf(TEXT("%.2f"), Value);
	}
}

void FTraceOptionsMenu::SetSettingNormalised(ESetting Setting, float Alpha)
{
	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float Step = 1.f;
	GetSettingValue(Setting, Value, Min, Max, Step);

	const float Raw = Min + FMath::Clamp(Alpha, 0.f, 1.f) * (Max - Min);

	// Snapped to the step so a drag produces the same set of values the arrow keys do — otherwise the
	// printed number never lands on a round figure and two players who both "set it to 1.0" have
	// different sensitivities.
	const float Snapped = FMath::Clamp(FMath::RoundToFloat(Raw / Step) * Step, Min, Max);

	if (IsVideoSetting(Setting))
	{
		UTraceGameUserSettings* GUS = Video();
		if (GUS == nullptr)
		{
			return;
		}

		if (IsQualityGroup(Setting))
		{
			GUS->SetGroupQuality(ETraceQualityGroup(QualityGroupIndex(Setting)), FMath::RoundToInt(Snapped));
		}
		else
		{
			switch (Setting)
			{
			case ESetting::ResolutionScale:
				GUS->SetResolutionScalePercent(FMath::RoundToInt(Snapped));
				break;

			case ESetting::OverallQuality:
				// Custom is silently ignored by SetOverallQuality, which is why the range in
				// GetSettingValue only reaches it when we are already there.
				GUS->SetOverallQuality(ETraceVideoQuality(FMath::RoundToInt(Snapped)));
				break;

			case ESetting::WindowMode:
			{
				const TArray<EWindowMode::Type>& Modes = UTraceGameUserSettings::GetWindowModeOptions();
				const int32 Index = FMath::RoundToInt(Snapped);
				if (Modes.IsValidIndex(Index))
				{
					GUS->SetWindowMode(Modes[Index]);
				}
				break;
			}

			case ESetting::Resolution:
				GUS->SetResolutionByOptionIndex(FMath::RoundToInt(Snapped));
				break;

			case ESetting::VSync:
				GUS->SetVSyncEnabled(Snapped >= 0.5f);
				break;

			case ESetting::FrameRateLimit:
				GUS->SetFrameRateLimitByIndex(FMath::RoundToInt(Snapped));
				// Cheap, single-cvar, and immediate — the settings class exposes this precisely so a
				// menu row does not have to drag the whole ApplyNonResolutionSettings machine behind
				// it just to change t.MaxFPS.
				GUS->ApplyFrameRateLimitNow();
				break;

			case ESetting::FieldOfView:
				// Applies to the live camera inside the setter. Nothing further to push.
				GUS->SetFieldOfView(Snapped);
				break;

			default:
				break;
			}
		}

		// RESOLUTION and WINDOW MODE do not apply here at all: they are queued, because applying
		// them re-creates the swap chain. See bResolutionApplyPending.
		if (Setting == ESetting::Resolution || Setting == ESetting::WindowMode)
		{
			bResolutionApplyPending = true;
			ResolutionApplyAtTime = Now + ResolutionApplyDelay;
			return;
		}

		// Everything else previews live, every step, and is NOT persisted here — same contract as
		// the mouse sliders below. The commit paths (AdjustSelected, and the mouse release in
		// PollMouse) go through ApplyVideoSettings, which writes the .ini once.
		ApplyVideo(/*bResolutionAffecting=*/false, /*bPersist=*/false);
		return;
	}

	// Spec v14 §3, and it is handled BEFORE the UTraceUserSettings block below because it does not
	// live there: it persists into GameUserSettings.ini through TraceCharacters, and falling into the
	// switch below would hit `default: return` and silently do nothing — which is precisely the
	// failure mode a settings row must never have.
	if (Setting == ESetting::CharactersEnabled)
	{
		TraceCharacters::SetEnabledSetting(Snapped >= 0.5f);
		return;
	}

	UTraceUserSettings& Settings = UTraceUserSettings::Get();
	switch (Setting)
	{
	case ESetting::Sensitivity:  Settings.MouseSensitivity = Snapped; break;
	case ESetting::SensitivityY: Settings.MouseSensitivityYScale = Snapped; break;
	case ESetting::InvertY:      Settings.bInvertMouseY = (Snapped >= 0.5f); break;
	default: return;
	}

	// Deliberately no Save() here: a drag would otherwise write and flush the .ini every frame. The
	// mouse-up path saves once, and the keyboard path in AdjustSelected saves per press.
	UTraceUserSettings::OnChanged().Broadcast();
}

void FTraceOptionsMenu::AdjustSelected(int32 Delta)
{
	if (!Rows.IsValidIndex(Selected) || Delta == 0)
	{
		return;
	}

	FRow& Row = Rows[Selected];
	if (Row.Kind != ERowKind::Slider && Row.Kind != ERowKind::Toggle && Row.Kind != ERowKind::Choice)
	{
		return;
	}

	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float Step = 1.f;
	GetSettingValue(Row.Setting, Value, Min, Max, Step);

	const float Range = FMath::Max(UE_KINDA_SMALL_NUMBER, Max - Min);
	SetSettingNormalised(Row.Setting, ((Value + Delta * Step) - Min) / Range);

	if (IsVideoSetting(Row.Setting))
	{
		// The keyboard path commits: one press, one write. RESOLUTION and WINDOW MODE are excluded —
		// SetSettingNormalised queued those, and persisting here would apply the resize this press
		// was specifically trying not to trigger. Their commit happens when the coalesce expires, or
		// when the page is left.
		if (Row.Setting != ESetting::Resolution && Row.Setting != ESetting::WindowMode)
		{
			ApplyVideo(/*bResolutionAffecting=*/false, /*bPersist=*/true);
		}

		// The window mode decides whether RESOLUTION is a live row, and OVERALL QUALITY moves nine
		// other rows. Both are visible on screen right now, so both have to be re-evaluated now.
		RefreshRowStates();
		return;
	}

	UTraceUserSettings::Get().Save();
}

void FTraceOptionsMenu::ActivateSelected()
{
	if (!Rows.IsValidIndex(Selected))
	{
		return;
	}

	FRow& Row = Rows[Selected];

	switch (Row.Kind)
	{
	case ERowKind::Toggle:
	{
		// FLIP, do not increment. This used to be AdjustSelected(1), and AdjustSelected clamps at
		// Max — so pressing Enter or clicking a toggle that was already ON did nothing at all, and
		// the row could never be turned back off by the control a player actually reaches for. Only
		// the LEFT arrow could undo it.
		//
		// This shipped, and it was reported from the other side: "the mouse was inverted and the
		// button to uninvert it didn't work." INVERT MOUSE Y, VSYNC and CHARACTERS were all one-way.
		// The behaviour was even known — the -TraceAutoSettings script documents it and steers around
		// it with LEFT/RIGHT instead of pressing Enter, which is how it stayed invisible in testing.
		float Value = 0.f;
		float Min = 0.f;
		float Max = 1.f;
		float Step = 1.f;
		GetSettingValue(Row.Setting, Value, Min, Max, Step);

		AdjustSelected((Value >= 0.5f) ? -1 : 1);
		return;
	}

	case ERowKind::Choice:
	{
		// Enter WRAPS where the arrows clamp. Clamping is right for arrows — holding right must
		// arrive somewhere and stay there — but a click on a row whose value is already at the top
		// has to do something, or the row reads as broken. Wrapping is the only answer that does not
		// need a second control.
		float Value = 0.f;
		float Min = 0.f;
		float Max = 1.f;
		float Step = 1.f;
		GetSettingValue(Row.Setting, Value, Min, Max, Step);

		const int32 StepsAcross = FMath::RoundToInt((Max - Min) / FMath::Max(Step, UE_KINDA_SMALL_NUMBER));
		const bool bAtTop = (Value >= Max - UE_KINDA_SMALL_NUMBER);
		AdjustSelected(bAtTop ? -StepsAcross : 1);
		return;
	}

	case ERowKind::Slider:
		// Nothing sensible for Enter to do to a continuous value. Left/right and the mouse own it.
		return;

	case ERowKind::Binding:
		bCapturingKey = true;
		CapturingAction = Row.Binding;
		// The Enter (or click) that started the capture is still live this frame; without this it
		// would immediately become the new binding.
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Waiting for a key to bind to %s."),
			TraceInputActions::Info(Row.Binding).DisplayName);
		return;

	default:
		break;
	}

	switch (Row.Action)
	{
	case EAction::Resume:
		Close();
		if (OnResume) { OnResume(); }
		break;

	case EAction::OpenSettings:
		Page = EPage::Settings;
		bSettingsIsRootPage = false;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::OpenVideo:
		// Remember where we came from — the pause root and the settings page both reach this row,
		// and BACK has to undo the step the player actually took.
		VideoReturnPage = Page;
		Page = EPage::Video;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::AutoDetectQuality:
		// Deferred one frame so the "MEASURING…" state is drawn before the benchmark blocks. Cleared
		// and run at the top of the next Tick.
		bAutoDetectPending = true;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Hardware benchmark requested."));
		break;

	case EAction::ResetVideoDefaults:
		ResetVideoToDefaults();
		break;

	case EAction::ReturnToTitle:
		Close();
		if (OnReturnToTitle) { OnReturnToTitle(); }
		break;

	case EAction::Quit:
		Close();
		if (OnQuit) { OnQuit(); }
		break;

	case EAction::ResetDefaults:
		UTraceUserSettings::Get().ResetToDefaults();
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Reset to defaults."));
		break;

	case EAction::Back:
		GoBack();
		break;

	default:
		break;
	}
}

void FTraceOptionsMenu::GoBack()
{
	if (Page == EPage::Video)
	{
		// A queued resize must not be able to outlive the page that queued it. Leaving the page is
		// as much a commit as pressing enter is, and a mode change that lands two frames after the
		// menu shut would look like the game deciding on its own to resize.
		if (bResolutionApplyPending)
		{
			ApplyVideo(/*bResolutionAffecting=*/true, /*bPersist=*/true);
		}

		if (VideoReturnPage == EPage::Root || VideoReturnPage == EPage::Settings)
		{
			Page = VideoReturnPage;
			IgnoreInputBeforeFrame = GFrameCounter + 1;
			RebuildRows();
			return;
		}

		Close();
		return;
	}

	if (Page == EPage::Settings && !bSettingsIsRootPage)
	{
		// Came in through the pause menu: step back to it rather than dropping the player straight
		// into a firefight they did not ask to return to.
		Page = EPage::Root;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		return;
	}

	const bool bWasRoot = (Page == EPage::Root);
	Close();

	// Escape out of the pause root means "resume", which is what every game does.
	if (bWasRoot && OnResume)
	{
		OnResume();
	}
}

// =================================================================================================
// Video
// =================================================================================================

UTraceGameUserSettings* FTraceOptionsMenu::Video()
{
	return UTraceGameUserSettings::Get();
}

void FTraceOptionsMenu::ApplyVideo(bool bResolutionAffecting, bool bPersist)
{
	UTraceGameUserSettings* GUS = Video();
	if (GUS == nullptr)
	{
		return;
	}

	if (bPersist)
	{
		// ---- The resolution-affecting apply MUST NOT run inside a Canvas draw --------------------
		//
		// This whole menu is ticked from inside AHUD::DrawHUD (see ATraceMenuHUD::DrawHUD ->
		// OptionsMenu.Tick, and the same shape on ATraceHUD). ApplyVideoSettings(true) re-requests
		// the window, which tears down and rebuilds the viewport — and doing that with a live
		// FCanvas on the stack frees the memory the canvas is still drawing into. Measured: the
		// engine's own guard fires ~100 times ("Canvas Draw functions may only be called during the
		// handling of the DrawHUD event") and the next FCanvas::PushAbsoluteTransform walks a freed
		// allocation. Both WINDOW MODE and RESOLUTION crashed the process this way.
		//
		// The proof it is the call site and not the engine call: the identical
		// ApplyVideoSettings(true) driven from a console command — same function, same arguments,
		// but executed from a ticker instead of from DrawHUD — applies cleanly and the process
		// survives. So the fix is scheduling, not the call.
		//
		// Deferring to the next core tick puts it after the draw has finished. The weak pointer is
		// to the SETTINGS object, not to this menu: the menu is a plain struct owned by the HUD and
		// can be destroyed between scheduling and firing (closing the menu is one of the paths that
		// commits a pending resize), whereas the settings object is GC-tracked and outlives it.
		if (bResolutionAffecting)
		{
			bResolutionApplyPending = false;

			TWeakObjectPtr<UTraceGameUserSettings> WeakSettings(GUS);
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakSettings](float /*Delta*/) -> bool
				{
					if (UTraceGameUserSettings* Settings = WeakSettings.Get())
					{
						Settings->ApplyVideoSettings(/*bResolutionAffecting=*/true);
					}
					return false;   // fire exactly once
				}),
				0.f);
			return;
		}

		// The non-resolution half is safe inline: it writes GameUserSettings.ini, re-pushes the FOV
		// onto live cameras and broadcasts, but never touches the viewport.
		GUS->ApplyVideoSettings(/*bResolutionAffecting=*/false);
		bResolutionApplyPending = false;
		return;
	}

	// ---- Preview only ---------------------------------------------------------------------------
	//
	// Every frame of a drag comes through here, and ApplyVideoSettings(false) would be wrong for it
	// twice over: it writes and flushes the .ini (one disk write per frame), and it goes through
	// ApplyNonResolutionSettings, which re-validates every setting, walks the audio device and calls
	// every console-variable sink on the way past. None of that belongs on a slider.
	//
	// Scalability::SetQualityLevels is the narrow path: it writes the group CVars and
	// r.ScreenPercentage and skips whatever has not actually moved. It is what makes RESOLUTION
	// SCALE respond under the cursor, which is the entire reason that row is at the top of the page.
	// The release then calls this again with bPersist and the value is written exactly once.
	Scalability::SetQualityLevels(GUS->ScalabilityQuality);
}

void FTraceOptionsMenu::RunAutoDetect()
{
	UTraceGameUserSettings* GUS = Video();
	if (GUS == nullptr)
	{
		return;
	}

	// Benchmarks, applies, persists and logs — all of it next door. This row is a button, not an
	// implementation. Note that auto-detect is the one path allowed to move RESOLUTION SCALE, which
	// is correct: the player asked the machine to decide, and on a weak GPU the render scale is the
	// biggest single part of that answer.
	GUS->RunAutoDetect();

	// Nine group rows, the overall row and the scale row have all potentially moved. They are read
	// live so they redraw correctly on their own; what has to be re-derived is which rows are still
	// selectable.
	RefreshRowStates();
}

void FTraceOptionsMenu::ResetVideoToDefaults()
{
	UTraceGameUserSettings* GUS = Video();
	if (GUS == nullptr)
	{
		return;
	}

	// VIDEO only. The controls page has its own RESET, and a player who pressed this one asked about
	// their display — silently clearing their key bindings from here would be the single most
	// destructive thing this menu could do.
	GUS->ResetVideoToDefaults();

	// Any queued resize is about a mode that no longer exists in the settings; the reset has already
	// applied the one that does.
	bResolutionApplyPending = false;
	RefreshRowStates();
}

// =================================================================================================
// Field of view
//
// The VALUE and its persistence belong to UTraceGameUserSettings. What is here is only the reassert.
// =================================================================================================

void FTraceOptionsMenu::MaintainFieldOfView(APlayerController* PC)
{
	const UTraceGameUserSettings* GUS = Video();
	if (PC == nullptr || GUS == nullptr)
	{
		return;
	}

	APawn* ControlledPawn = PC->GetPawn();
	if (ControlledPawn == nullptr)
	{
		// Between death and respawn, or on the title screen, where there is no pawn at all. Drop the
		// cache so the next pawn is looked up fresh rather than inheriting a stale camera.
		FovPawn = nullptr;
		FovCamera = nullptr;
		return;
	}

	if (ControlledPawn != FovPawn.Get())
	{
		FovPawn = ControlledPawn;
		FovCamera = ControlledPawn->FindComponentByClass<UCameraComponent>();
	}

	UCameraComponent* Cam = FovCamera.Get();
	if (Cam == nullptr)
	{
		return;
	}

	// The same write UTraceGameUserSettings::ApplyFieldOfViewToWorlds does, at frame rate instead of
	// at 1 Hz. That ticker is what makes the setting survive a respawn at all, but it can be up to a
	// second late, and a second of the wrong field of view immediately after respawning is a second
	// of a shooter feeling wrong at exactly the moment the player is trying to re-orient.
	//
	// Idempotent and self-limiting: one float compare when nothing has changed, one SetFieldOfView on
	// the single frame after a respawn when it has. The two writers cannot fight — they write the
	// same value from the same source.
	//
	// NOT APlayerCameraManager::SetFOV, which was the obvious answer and is a dead end: in UE 5.8
	// LockedFOV is read by GetFOVAngle() and by nothing else in the view pipeline, so it reports a
	// number without changing a single pixel. The camera component is where the projection actually
	// comes from.
	const float DesiredFOV = GUS->GetFieldOfView();
	if (!FMath::IsNearlyEqual(Cam->FieldOfView, DesiredFOV, 0.01f))
	{
		Cam->SetFieldOfView(DesiredFOV);
	}
}

// =================================================================================================
// Live performance readout
// =================================================================================================

void FTraceOptionsMenu::UpdatePerfReadout()
{
	const double RealNow = FPlatformTime::Seconds();

	if (PerfLastRealTime <= 0.0)
	{
		// First frame on the page. There is no interval yet, and inventing one from a zero would
		// print an infinite frame rate for a quarter of a second.
		PerfLastRealTime = RealNow;
		return;
	}

	const float Delta = float(RealNow - PerfLastRealTime);
	PerfLastRealTime = RealNow;

	// A hitch of half a second — a resolution change, or the benchmark — is not the frame rate and
	// must not be averaged into it, or the readout spends the next window claiming 2 fps.
	if (Delta <= 0.f || Delta > 0.5f)
	{
		return;
	}

	PerfWindowSeconds += Delta;
	++PerfWindowFrames;

	// A quarter of a second: long enough that the number stops flickering, short enough that letting
	// go of the resolution-scale slider shows a new number before the player has moved their hand.
	if (PerfWindowSeconds >= 0.25f && PerfWindowFrames > 0)
	{
		PerfFrameMs = (PerfWindowSeconds / float(PerfWindowFrames)) * 1000.f;
		PerfFps = float(PerfWindowFrames) / PerfWindowSeconds;
		PerfWindowSeconds = 0.f;
		PerfWindowFrames = 0;

		// The RHI's own timer. Reported directly rather than through FStatUnitData, which is only
		// filled while the `stat unit` overlay is enabled and would otherwise need this page to
		// switch on an engine overlay it then has to draw around. Zero on an RHI that does not
		// implement it, and the readout simply omits the column in that case rather than printing a
		// confident 0.00.
		PerfGpuMs = float(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));
	}
}

void FTraceOptionsMenu::DrawPerfReadout(AHUD* HUD, float RightX, float Y)
{
	if (PerfFps <= 0.f)
	{
		return;
	}

	// Amber below 45 fps, cyan above. The threshold is a judgement, not a measurement: this is a
	// shooter, and the collaborator's report was about a build that was unplayable rather than one
	// that was merely not smooth. It exists so the player can tell at a glance whether a change they
	// just made moved them across the line, which is the entire point of putting this here.
	const FLinearColor Color = (PerfFps < 45.f) ? TraceOptionsStyle::Amber : TraceOptionsStyle::Cyan;

	const FString Line = (PerfGpuMs > 0.01f)
		? FString::Printf(TEXT("%.0f FPS    %.2f MS    GPU %.2f MS"), PerfFps, PerfFrameMs, PerfGpuMs)
		: FString::Printf(TEXT("%.0f FPS    %.2f MS"), PerfFps, PerfFrameMs);

	DrawTextRight(HUD, Line, Color, RightX, Y, FontSmall, 1.1f * UIScale);
}

// =================================================================================================
// Draw
// =================================================================================================

void FTraceOptionsMenu::Draw(AHUD* HUD)
{
	// Once per process, and on the first frame the panel is up rather than at module load: it forces
	// the ten UI textures resident before the player can move a selection, and it puts a line in the
	// log that says whether this screen is wearing the art or its fallbacks.
	TraceOptionsMenuArt::LogOnce();

	// Dim whatever is behind us. In a match that is the arena; on the title screen it is the grid.
	// Either way the panel has to be the only thing the eye can land on, and the arena in particular
	// is a field of bright emissive strips that a translucent panel loses to.
	HUD->DrawRect(FLinearColor(0.f, 0.008f, 0.018f, 0.88f), 0.f, 0.f, ViewW, ViewH);

	// ---- Panel geometry -------------------------------------------------------------------------
	//
	// The panel is sized to its CONTENT, not to the screen. Both pages share this class, and they are
	// wildly different shapes: the settings page has sixteen rows, the pause root has four. A fixed
	// 88%-of-height panel fitted the settings page and left the pause menu as four buttons stranded
	// at the top of an enormous empty box — which is exactly what the first capture showed.
	//
	// So: ask for a comfortable row pitch, add it up, and only then clamp to the screen. The clamp
	// bites on the settings page at 720p and nowhere else, and when it bites the pitch shrinks to fit
	// rather than the last rows falling off the bottom.
	const float TitleBlockH = 82.f * UIScale;      // title + rule + the gap under it
	const float FooterH     = 44.f * UIScale;      // the key-hint line
	const float PadY        = 14.f * UIScale;

	const int32 RowCount = FMath::Max(1, Rows.Num());
	const float PreferredPitch = 44.f * UIScale;

	const float MaxPanelH = ViewH * 0.90f;
	const float PanelH = FMath::Min(MaxPanelH, TitleBlockH + RowCount * PreferredPitch + FooterH + PadY);

	// Whatever height survived the clamp is what the rows get to share.
	const float RowsRegion = FMath::Max(1.f, PanelH - TitleBlockH - FooterH - PadY);
	const float Pitch = FMath::Clamp(RowsRegion / RowCount, 18.f * UIScale, PreferredPitch);
	const float RowH = Pitch * 0.86f;

	// A page of four buttons does not want to be as wide as a page of sixteen labelled settings — and
	// the video page is wider still, because its values are words rather than numbers: "WINDOWED
	// FULLSCREEN" and "GLOBAL ILLUMINATION" have to fit on one line at 720p without the label and
	// the value colliding in the middle.
	float PanelW = FMath::Min(ViewW * 0.74f, 880.f * UIScale);
	if (Page == EPage::Root)
	{
		PanelW = FMath::Min(ViewW * 0.46f, 520.f * UIScale);
	}
	else if (Page == EPage::Video)
	{
		PanelW = FMath::Min(ViewW * 0.86f, 1020.f * UIScale);
	}

	const float PanelX = (ViewW - PanelW) * 0.5f;
	const float PanelY = (ViewH - PanelH) * 0.5f;
	const float CX = ViewW * 0.5f;

	HUD->DrawRect(TraceOptionsStyle::Panel, PanelX, PanelY, PanelW, PanelH);
	DrawFrame(HUD, PanelX, PanelY, PanelW, PanelH);

	// ---- Title ---------------------------------------------------------------------------------
	FString Title = TEXT("SETTINGS");
	if (Page == EPage::Root)       { Title = TEXT("PAUSED"); }
	else if (Page == EPage::Video) { Title = TEXT("VIDEO"); }

	const float TitleY = PanelY + (22.f * UIScale);
	DrawTextCentered(HUD, Title, TraceOptionsStyle::Cyan, CX, TitleY, FontLarge, 1.9f * UIScale);

	// The live readout, on the title line and only on the video page. Spec v11 §2: the collaborator
	// could tell the build was slow and had no way to measure it, so every row on this page is a
	// control whose effect is visible in the number sitting three inches above it. It is drawn last
	// in reading order but first in importance, which is why it shares the title's line rather than
	// being another row at the bottom of twenty-two.
	if (Page == EPage::Video)
	{
		DrawPerfReadout(HUD, PanelX + PanelW - (28.f * UIScale), TitleY + (14.f * UIScale));
	}

	const float RuleY = TitleY + (46.f * UIScale);
	HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.45f),
		PanelX + (28.f * UIScale), RuleY, PanelW - (56.f * UIScale), FMath::Max(1.f, 1.5f * UIScale));

	// ---- Rows ----------------------------------------------------------------------------------
	const float RowsTop = RuleY + (14.f * UIScale);
	const float RowX = PanelX + (28.f * UIScale);
	const float RowW = PanelW - (56.f * UIScale);

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		DrawRow(HUD, Rows[Index], RowX, RowsTop + Index * Pitch, RowW, RowH, Index == Selected);
	}

	// ---- Footer --------------------------------------------------------------------------------
	FString Hint;
	if (bCapturingKey)
	{
		Hint = TEXT("PRESS ANY KEY TO BIND          ESC   CANCEL");
	}
	else if (Page == EPage::Root)
	{
		Hint = TEXT("W / S  OR  ARROWS   MOVE          ENTER   SELECT          ESC   RESUME");
	}
	else if (Page == EPage::Video)
	{
		// No BKSP/UNBIND on this page — there is nothing to unbind — and the hint says so rather
		// than offering a key that does nothing.
		Hint = TEXT("ARROWS  MOVE / ADJUST          ENTER  SELECT          ESC  BACK");
	}
	else
	{
		Hint = TEXT("ARROWS  MOVE / ADJUST      ENTER  SELECT      BKSP  UNBIND      ESC  BACK");
	}

	DrawTextCentered(HUD, Hint, TraceOptionsStyle::InkDim, CX,
		PanelY + PanelH - (30.f * UIScale), FontSmall, 1.0f * UIScale);

	DrawCursor(HUD);
}

void FTraceOptionsMenu::DrawRow(AHUD* HUD, FRow& Row, float X, float Y, float W, float H, bool bSelected)
{
	Row.Rect = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));
	Row.Track = FBox2D(ForceInit);

	const float PadX = 16.f * UIScale;
	const float LabelScale = FMath::Min(1.15f, H / (26.f * UIScale)) * UIScale;
	const float TextY = Y + (H - MeasureHeight(HUD, TEXT("X"), FontMedium, LabelScale)) * 0.5f;

	// ---- Header --------------------------------------------------------------------------------
	if (Row.Kind == ERowKind::Header)
	{
		if (Row.Label.IsEmpty())
		{
			return;
		}

		const float Gap = 10.f * UIScale;
		float RuleLeft = 0.f;
		float RuleRight = X + W;
		bool bWordsDrawn = false;

		// THE CONTROLS HEADER IS WHERE THE ARTIST'S LETTERING BELONGS. KEYBIND and KEY were cut off
		// the sheet as a left/right pair on one row — a wide label plate beside a narrow key chip —
		// which is exactly the two columns every Binding row underneath this one is laid out in. They
		// are drawn INSTEAD of the word "CONTROLS", not beside it: at a 24px row there is no room for
		// three labels, and "KEYBIND ......... KEY" is what those columns actually are.
		//
		// FRow::Label is untouched. DebugGetRowRect and the click harness match on it, and nothing
		// about this is allowed to be a contract change.
		// SPEC v22 §A1 RETIRED THE SPRITES HERE TOO. This branch used to blit the artist's baked
		// T_MenuWord_Keybind and T_MenuWord_Key, which is why the shipped settings page had TWO faces
		// on it: those two words in the artist's squared lettering and the nineteen rows under them in
		// the engine's stand-in font, four pixels apart. The page can type in the artist's face now,
		// so the header is typed like everything else and the two-column KEYBIND / KEY layout — which
		// is the part that was actually worth keeping — survives unchanged.
		//
		// The sprites stay in the repo and stay loadable, exactly as A1 asks. Nothing draws them.
		if (Row.Label.Equals(TEXT("CONTROLS"), ESearchCase::IgnoreCase))
		{
			const FLinearColor WordTint = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.80f);
			const float ValueRightHdr = X + W - PadX;

			TraceOptionsMenuType::Draw(HUD, TEXT("KEYBIND"), WordTint, X, TextY, FontSmall, 1.0f * UIScale);
			TraceOptionsMenuType::Draw(HUD, TEXT("KEY"), WordTint, ValueRightHdr, TextY, FontSmall,
				1.0f * UIScale, TraceText::EHAlign::Right);

			// The rule has to stop short at BOTH ends, or it strikes straight through KEY.
			RuleLeft = X + MeasureWidth(HUD, TEXT("KEYBIND"), FontSmall, 1.0f * UIScale) + Gap;
			RuleRight = ValueRightHdr - MeasureWidth(HUD, TEXT("KEY"), FontSmall, 1.0f * UIScale) - Gap;
			bWordsDrawn = true;
		}

		if (!bWordsDrawn)
		{
			TraceOptionsMenuType::Draw(HUD, Row.Label,
				TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.75f),
				X, TextY, FontSmall, 1.0f * UIScale);

			RuleLeft = X + MeasureWidth(HUD, Row.Label, FontSmall, 1.0f * UIScale) + Gap;
		}

		if (RuleRight > RuleLeft)
		{
			HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.20f),
				RuleLeft, Y + H * 0.5f, RuleRight - RuleLeft, FMath::Max(1.f, 1.f * UIScale));
		}
		return;
	}

	// ---- Note ----------------------------------------------------------------------------------
	//
	// Amber, indented under the row it belongs to, with no plate. Amber is the palette's "this is
	// the one you want" colour and it is spent here deliberately: on a page of nineteen controls
	// the eye needs to be told which one is worth more than the other eighteen.
	if (Row.Kind == ERowKind::Note)
	{
		TraceOptionsMenuType::Draw(HUD, Row.Label,
			TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, 0.78f),
			X + PadX, TextY, FontSmall, 1.0f * UIScale);
		return;
	}

	// ---- Plate ---------------------------------------------------------------------------------
	//
	// The artist's own button plate, one sprite per state, landed exactly on the row rect the mouse
	// already hit-tests. This is what makes a row here the same object as a row on the title screen —
	// and because the pause root is nothing but Action rows, it is also what puts the art in front of
	// a player who pressed Escape mid-match.
	//
	// The three states are the sheet's three states and they differ in the PLATE, not in the word:
	// default is a bare navy plate, hover adds the amber ring, disabled is near-black with a grey one.
	// So the selected row is marked by its ring and by a BRIGHTER label, never by a dimmer one — spec
	// v20 §0.5 is what happens when a decorative hover tint gets promoted to a selection indicator.
	bool bPlateDrawn = false;
	{
		const TraceOptionsMenuArt::ESprite Which = !Row.bEnabled
			? TraceOptionsMenuArt::ESprite::PlateDisabled
			: (bSelected ? TraceOptionsMenuArt::ESprite::PlateHover : TraceOptionsMenuArt::ESprite::PlateDefault);

		if (UTexture2D* Plate = TraceOptionsMenuArt::Sprite(Which))
		{
			// Unselected rows are knocked back rather than the selected row being knocked forward: a
			// page of thirty plates all at full strength is a wall, and the eye needs the selected one
			// to be the brightest thing in the list.
			const FLinearColor Tint = !Row.bEnabled
				? FLinearColor(0.80f, 0.80f, 0.80f, 0.75f)
				: (bSelected ? FLinearColor::White : FLinearColor(0.78f, 0.78f, 0.78f, 0.90f));

			TraceOptionsMenuArt::DrawPlate(HUD, Plate, TraceOptionsMenuArt::ButtonFrame, X, Y, W, H, Tint);
			bPlateDrawn = true;
		}
	}
	if (!bPlateDrawn)
	{
		HUD->DrawRect(FLinearColor(0.f, 0.02f, 0.04f, bSelected ? 0.85f : 0.45f), X, Y, W, H);
	}

	if (bSelected)
	{
		const float Pulse = 0.72f + 0.28f * FMath::Sin(Now * 4.5f);

		// A wash, never a dim: this only ever ADDS light to the selected plate. The pulse rides on the
		// wash and on the marker, not on the plate itself, so a before/after capture of the selected
		// row cannot be moved by more than a few percent by the phase it was caught at.
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.10f * Pulse), X, Y, W, H);

		// A SOLID RAIL ON THE LEADING EDGE — the same mark, in the same place, as the UMG title row
		// (spec v22 §A4, UI/Widgets/Menu/TraceMenuRowWidget.cpp), and for the same reason.
		//
		// This slot drew the artist's T_MenuBack crescent until this pass. A4 replaced it on the title
		// screen because a tapered stroke at ~20 px stops reading as a pointer and starts reading as a
		// stray ")" — and then left this page still drawing it, so pressing SETTINGS swapped the mark
		// for the thing A4 had just removed. Photographed at 1920x1080 beside VIDEO SETTINGS in
		// v22integ_03_settings_open.png before this change.
		//
		// A filled bar has no small-size failure mode; this is a rectangle rather than the title row's
		// rounded capsule only because there is no Slate brush on a Canvas and a 9-px-wide rounded end
		// is under one pixel of difference at this size. Height is tied to the row, not to a constant,
		// so it tracks the 18px row this page clamps to at 720p as well as the full-height one.
		const float MarkW = FMath::Max(3.f, 5.f * UIScale);
		const float MarkH = H * 0.66f;
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, Pulse),
			X - MarkW - (6.f * UIScale), Y + (H - MarkH) * 0.5f, MarkW, MarkH);
	}

	// A greyed row is not a selected row and cannot be, so this collapses to two states, not four.
	const FLinearColor LabelColor = !Row.bEnabled
		? TraceOptionsStyle::WithAlpha(TraceOptionsStyle::InkDim, 0.45f)
		: (bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim);

	// Action rows are buttons; centring their label is what makes them read as one.
	if (Row.Kind == ERowKind::Action)
	{
		// AUTO-DETECT is the row a confused player on a weak machine should press, so it is the only
		// button on the page drawn in amber with a plate behind it — everything else here is a list.
		if (Row.Action == EAction::AutoDetectQuality)
		{
			const bool bMeasuring = bAutoDetectPending;
			HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, bSelected ? 0.30f : 0.16f), X, Y, W, H);

			const FString Text = bMeasuring ? TEXT("MEASURING THIS MACHINE...") : Row.Label;
			DrawTextCentered(HUD, Text, bMeasuring ? FLinearColor::White : TraceOptionsStyle::Amber,
				X + W * 0.5f, TextY, FontMedium, LabelScale);
			return;
		}

		DrawTextCentered(HUD, Row.Label, LabelColor, X + W * 0.5f, TextY, FontMedium, LabelScale);
		return;
	}

	TraceOptionsMenuType::Draw(HUD, Row.Label, LabelColor, X + PadX, TextY, FontMedium, LabelScale);

	// ---- Value ---------------------------------------------------------------------------------
	const float ValueRight = X + W - PadX;
	const float ValueColW = 96.f * UIScale;

	if (Row.Kind == ERowKind::Binding)
	{
		const bool bWaiting = bCapturingKey && CapturingAction == Row.Binding;
		const FKey Key = UTraceUserSettings::Get().GetKey(Row.Binding);

		FString ValueText;
		FLinearColor ValueColor;
		if (bWaiting)
		{
			ValueText = TEXT("PRESS A KEY");
			ValueColor = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, 0.6f + 0.4f * FMath::Sin(Now * 9.f));
		}
		else
		{
			ValueText = UTraceUserSettings::DescribeKey(Key);
			// Amber for UNBOUND: it is not an error, but the player should not be able to miss it.
			ValueColor = Key.IsValid() ? TraceOptionsStyle::Ink : TraceOptionsStyle::Amber;
		}

		// UNCHANGED ARITHMETIC. The chip rect this screen has always drawn is exactly the shape of
		// T_MenuValueBox, so the sprite is a one-for-one swap onto a rectangle that already existed.
		const float PlateW = FMath::Max(MeasureWidth(HUD, ValueText, FontMedium, LabelScale) + (16.f * UIScale), 120.f * UIScale);
		const float ChipX = ValueRight - PlateW;
		const float ChipY = Y + H * 0.14f;
		const float ChipH = H * 0.72f;

		const bool bChipDrawn = DrawValueChip(HUD, ChipX, ChipY, PlateW, ChipH);

		// The cyan wash the chip used to BE, kept on top of the sprite at a whisper. The artist's chip
		// is the same navy as the plate it sits on — its amber ring is what separates them, and a
		// little light inside it is what stops the key name reading as a hole in the row.
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan,
			bWaiting ? 0.22f : (bChipDrawn ? 0.05f : 0.10f)), ChipX, ChipY, PlateW, ChipH);

		DrawTextCentered(HUD, ValueText, ValueColor, ValueRight - PlateW * 0.5f, TextY, FontMedium, LabelScale);
		return;
	}

	// Every remaining kind reads its value the same way, which is what lets one Toggle path serve
	// INVERT MOUSE Y and VSYNC and one Choice path serve all thirteen enumerated video rows.
	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float Step = 1.f;
	GetSettingValue(Row.Setting, Value, Min, Max, Step);

	if (Row.Kind == ERowKind::Toggle)
	{
		const bool bOn = (Value >= 0.5f);
		DrawTextRight(HUD, FormatSettingValue(Row.Setting, Value),
			bOn ? TraceOptionsStyle::Amber : TraceOptionsStyle::InkDim,
			ValueRight, TextY, FontMedium, LabelScale);
		return;
	}

	if (Row.Kind == ERowKind::Choice)
	{
		const FString ValueText = FormatSettingValue(Row.Setting, Value);

		// Arrow glyphs, drawn only on the selected row and only on the side there is somewhere to go.
		// This is the one affordance that tells a player a row is a LIST rather than a label, and
		// without it the quality groups look like readouts.
		const FLinearColor ValueColor = !Row.bEnabled
			? TraceOptionsStyle::WithAlpha(TraceOptionsStyle::InkDim, 0.45f)
			: (bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim);

		// The arrow columns are reserved WHETHER OR NOT an arrow is drawn in them. If the value moved
		// right every time it hit the end of its range, every list on the page would twitch sideways
		// as the player walked it, which reads as a layout bug rather than as an end stop.
		const float ArrowColW = 20.f * UIScale;
		const float ValueW = MeasureWidth(HUD, ValueText, FontMedium, LabelScale);
		const float ValueTextRight = ValueRight - ArrowColW;

		DrawTextRight(HUD, ValueText, ValueColor, ValueTextRight, TextY, FontMedium, LabelScale);

		if (bSelected && Row.bEnabled)
		{
			const FLinearColor ArrowColor = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.9f);
			if (Value > Min + UE_KINDA_SMALL_NUMBER)
			{
				DrawTextRight(HUD, TEXT("<"), ArrowColor, ValueTextRight - ValueW - (6.f * UIScale),
					TextY, FontMedium, LabelScale);
			}
			if (Value < Max - UE_KINDA_SMALL_NUMBER)
			{
				DrawTextRight(HUD, TEXT(">"), ArrowColor, ValueRight, TextY, FontMedium, LabelScale);
			}
		}
		return;
	}

	// ---- Slider --------------------------------------------------------------------------------
	const float Alpha = FMath::Clamp((Value - Min) / FMath::Max(UE_KINDA_SMALL_NUMBER, Max - Min), 0.f, 1.f);

	const FString ValueText = FormatSettingValue(Row.Setting, Value);
	const FLinearColor SliderValueColor = bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim;

	// The sheet drew this control as a slider WITH a numbered chip beside it — the slicer erased the
	// "13" that was in it and kept the chip. So the chip goes back where the artist put it, behind the
	// value, in the column the track already stops short of (TrackRight below is its left edge).
	if (DrawValueChip(HUD, ValueRight - ValueColW, Y + H * 0.14f, ValueColW, H * 0.72f))
	{
		// Centred in the chip rather than right-aligned to the panel, because it now sits inside a
		// box and a number pinned to one wall of its box looks like a mistake.
		DrawTextCentered(HUD, ValueText, SliderValueColor, ValueRight - ValueColW * 0.5f, TextY, FontMedium, LabelScale);
	}
	else
	{
		DrawTextRight(HUD, ValueText, SliderValueColor, ValueRight, TextY, FontMedium, LabelScale);
	}

	const float TrackRight = ValueRight - ValueColW;
	const float TrackLeft = X + W * 0.48f;
	const float TrackW = FMath::Max(20.f * UIScale, TrackRight - TrackLeft);
	const float TrackH = FMath::Max(3.f, 6.f * UIScale);
	const float TrackY = Y + (H - TrackH) * 0.5f;

	bool bTrackDrawn = false;
	if (UTexture2D* TrackTex = TraceOptionsMenuArt::Sprite(TraceOptionsMenuArt::ESprite::SliderTrack))
	{
		// The sprite is a TROUGH — 23 rows with the solid rail occupying its middle eleven and a halo
		// above and below — so drawing it at the 6px the plain bar used would throw the shape away. It
		// takes the height the row can spare instead. TrackLeft and TrackW are untouched, which is the
		// only thing that matters: Row.Track is still computed from them at the bottom of this
		// function, and that rect is what a drag maps the pointer across.
		const float SpriteH = FMath::Clamp(H * 0.60f, TrackH, 21.f * UIScale);
		const float SpriteY = Y + (H - SpriteH) * 0.5f;
		TraceOptionsMenuArt::DrawTrough(HUD, TrackTex, TrackLeft, SpriteY, TrackW, SpriteH, FLinearColor::White);

		// THE FILL IS STILL A PLAIN BAR, deliberately. The sheet has no filled-track sprite (the
		// slicer's own note says the artist drew an empty trough), and tinting this one cannot make a
		// fill: a Canvas tint MULTIPLIES, so a navy trough times cyan is a darker navy trough. So the
		// rail is drawn INSIDE the artist's trough, inset to the sprite's own solid band, and the
		// artist's lip and halo frame it.
		const float RailTop = SpriteY + SpriteH * TraceOptionsMenuArt::TrackRailTopV;
		const float RailH = FMath::Max(2.f, SpriteH * TraceOptionsMenuArt::TrackRailV);
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, bSelected ? 0.95f : 0.55f),
			TrackLeft, RailTop, TrackW * Alpha, RailH);

		bTrackDrawn = true;
	}
	if (!bTrackDrawn)
	{
		HUD->DrawRect(TraceOptionsStyle::Trough, TrackLeft, TrackY, TrackW, TrackH);
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, bSelected ? 0.95f : 0.55f),
			TrackLeft, TrackY, TrackW * Alpha, TrackH);
	}

	// Handle, so the value has a thing to grab as well as a bar to read.
	const float HandleW = FMath::Max(4.f, 6.f * UIScale);
	const float HandleH = H * 0.56f;
	bool bHandleDrawn = false;
	if (UTexture2D* HandleTex = TraceOptionsMenuArt::Sprite(TraceOptionsMenuArt::ESprite::SliderHandle))
	{
		// The artist subtracted the rail from underneath the blade, so this composites over the trough
		// with no stub showing through. It is a white alpha mask, so the tint is the whole state.
		const float SpriteHandleH = FMath::Min(H * 0.92f, 26.f * UIScale);
		const float SpriteHandleW = SpriteHandleH * TraceOptionsMenuArt::HandleAspect;
		TraceOptionsMenuArt::Draw(HUD, HandleTex,
			TrackLeft + TrackW * Alpha - SpriteHandleW * 0.5f, Y + (H - SpriteHandleH) * 0.5f,
			SpriteHandleW, SpriteHandleH,
			bSelected ? FLinearColor::White : TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.85f));
		bHandleDrawn = true;
	}
	if (!bHandleDrawn)
	{
		HUD->DrawRect(bSelected ? FLinearColor::White : TraceOptionsStyle::Cyan,
			TrackLeft + TrackW * Alpha - HandleW * 0.5f, Y + (H - HandleH) * 0.5f, HandleW, HandleH);
	}

	// Stored AFTER drawing so the poll on the next frame drags against exactly what was on screen.
	// UNCHANGED BY SPEC v20: the sprite above is fitted to TrackLeft/TrackW, never the other way
	// round, so this is the same rectangle it has always been and a drag lands where it always did.
	Row.Track = FBox2D(FVector2D(TrackLeft, TrackY - H * 0.4f), FVector2D(TrackLeft + TrackW, TrackY + H * 0.4f));
}

bool FTraceOptionsMenu::DrawValueChip(AHUD* HUD, float X, float Y, float W, float H) const
{
	UTexture2D* Chip = TraceOptionsMenuArt::Sprite(TraceOptionsMenuArt::ESprite::ValueBox);
	if (Chip == nullptr)
	{
		return false;
	}

	TraceOptionsMenuArt::DrawPlate(HUD, Chip, TraceOptionsMenuArt::ValueFrame, X, Y, W, H, FLinearColor::White);
	return true;
}

void FTraceOptionsMenu::DrawFrame(AHUD* HUD, float X, float Y, float W, float H)
{
	// Same instrument-panel frame as the title screen's bezel, at panel scale, so the overlay reads
	// as part of the same machine rather than as a dialog box dropped on top of it.
	const float Thin = FMath::Max(1.f, 1.f * UIScale);
	const FLinearColor Frame = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.30f);

	HUD->DrawRect(Frame, X, Y, W, Thin);
	HUD->DrawRect(Frame, X, Y + H - Thin, W, Thin);
	HUD->DrawRect(Frame, X, Y, Thin, H);
	HUD->DrawRect(Frame, X + W - Thin, Y, Thin, H);

	const float Tick = 24.f * UIScale;
	const float TickT = FMath::Max(1.f, 2.f * UIScale);
	const FLinearColor Bright = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.9f);

	HUD->DrawRect(Bright, X, Y, Tick, TickT);
	HUD->DrawRect(Bright, X, Y, TickT, Tick);
	HUD->DrawRect(Bright, X + W - Tick, Y, Tick, TickT);
	HUD->DrawRect(Bright, X + W - TickT, Y, TickT, Tick);
	HUD->DrawRect(Bright, X, Y + H - TickT, Tick, TickT);
	HUD->DrawRect(Bright, X, Y + H - Tick, TickT, Tick);
	HUD->DrawRect(Bright, X + W - Tick, Y + H - TickT, Tick, TickT);
	HUD->DrawRect(Bright, X + W - TickT, Y + H - Tick, TickT, Tick);
}

void FTraceOptionsMenu::DrawCursor(AHUD* HUD)
{
	// The OS cursor does not appear in captured frames, and in the match it is hidden outright until
	// this overlay releases it — so the overlay draws its own. Same shape as the title screen's.
	if (!bHasCursor)
	{
		return;
	}

	// The artist's pointer, everywhere this overlay is — which is the title screen's SETTINGS page AND
	// the in-match pause menu. Spec v20 §0.8: until now it existed only on the UMG title screen.
	//
	// TIP-ANCHORED, NOT CENTRED. PollMouse hit-tests at CursorPos, so an arrow whose middle sat on the
	// hit point would draw its point about eleven pixels away from the pixel it is about to click, and
	// every click in every screenshot would look like it landed on the wrong row.
	if (UTexture2D* Arrow = TraceOptionsMenuArt::Sprite(TraceOptionsMenuArt::ESprite::Cursor))
	{
		const float CursorH = 30.f * UIScale;
		const float CursorW = CursorH * TraceOptionsMenuArt::CursorAspect;
		TraceOptionsMenuArt::Draw(HUD, Arrow,
			CursorPos.X - CursorW * TraceOptionsMenuArt::CursorTipU,
			CursorPos.Y - CursorH * TraceOptionsMenuArt::CursorTipV,
			CursorW, CursorH, FLinearColor::White);
		return;
	}

	const float S = 9.f * UIScale;
	const float T = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.95f);

	HUD->DrawLine(CursorPos.X - S, CursorPos.Y, CursorPos.X - S * 0.35f, CursorPos.Y, Color, T);
	HUD->DrawLine(CursorPos.X + S * 0.35f, CursorPos.Y, CursorPos.X + S, CursorPos.Y, Color, T);
	HUD->DrawLine(CursorPos.X, CursorPos.Y - S, CursorPos.X, CursorPos.Y - S * 0.35f, Color, T);
	HUD->DrawLine(CursorPos.X, CursorPos.Y + S * 0.35f, CursorPos.X, CursorPos.Y + S, Color, T);
}

// =================================================================================================
// Text helpers
// =================================================================================================

// ALL FOUR go through UI/Text now — spec v22 §A1. The (UFont*, Scale) signatures stay because they
// are this page's layout vocabulary and forty call sites speak it; TraceOptionsMenuType::SizeFor
// translates. See the block at the top of this file for why the translation is a measurement.

float FTraceOptionsMenu::MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale)
{
	return TraceOptionsMenuType::Width(HUD, Text, Font, Scale);
}

float FTraceOptionsMenu::MeasureHeight(AHUD* HUD, const FString& Text, UFont* Font, float Scale)
{
	// The LINE BOX, deliberately independent of @p Text — every caller on this page uses it to sit a
	// row's baseline, and a row whose height depended on whether its label happened to contain a
	// descender would jitter as the value changed. Measured to equal what the old bitmap path
	// returned, which is what keeps nineteen rows in exactly the places they were.
	(void)Text;
	return TraceOptionsMenuType::Height(HUD, Font, Scale);
}

void FTraceOptionsMenu::DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale)
{
	TraceOptionsMenuType::Draw(HUD, Text, Color, CenterX, Y, Font, Scale, TraceText::EHAlign::Center);
}

void FTraceOptionsMenu::DrawTextRight(AHUD* HUD, const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale)
{
	TraceOptionsMenuType::Draw(HUD, Text, Color, RightX, Y, Font, Scale, TraceText::EHAlign::Right);
}
