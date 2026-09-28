// Trace — see TraceLoadingScreen.h. THE STUDIO CARD AND THE TRAVEL CARD (P12).
//
// =================================================================================================
// HOW THE ENGINE RUNS A LOADING SCREEN, AND WHERE THIS FILE HOOKS IN (UE 5.8, measured in source)
// =================================================================================================
//
//   BOOT      The movie player opens the game window in PreInit and calls PlayMovie BEFORE any game
//             module is loaded (LaunchEngineLoop.cpp, "PlayFirstPreLoadScreen etc"), with nothing to
//             play, so for seconds the window stayed empty black. Startup() below runs in this
//             module's StartupModule — the first moment Trace code exists — and calls PlayMovie
//             ITSELF with the studio card. From there the Slate loading thread paints the card
//             through GEngine->Init and the first LoadMap(MainMenu), until FEngineLoop::Init's
//             WaitForMovieToFinish. The engine's own PreLoadMap PlayMovie returns false while ours is
//             playing, so it binds nothing and our attributes are never overwritten.
//
//   DISMISSAL The studio card waits for a MANUAL stop (bWaitForManualStop). WaitForMovieToFinish
//             then spins on the game thread, ticking Slate each iteration; TickStudioDismissal is
//             bound to Slate's pre-tick for exactly that stretch and calls StopMovie once the hold
//             is over AND the fade-out has run. A minimum display time alone could not do this:
//             the engine hides "loading finished" from the widget while it enforces one, and the exit
//             is a hard cut, so the fade would either run before the title was ready or not at all.
//             A watchdog stops it anyway StudioWatchdogSeconds after loading finished.
//
//   TRAVEL    PreLoadMapWithContext is broadcast immediately BEFORE PreLoadMap, which is where the
//             movie player calls PlayMovie with whatever it was last given — so OnPreLoadMap below
//             hands it the travel card first, independent of delegate order. Auto-complete, no
//             minimum: WaitForMovieToFinish (bound by the engine to PostLoadMap) returns at once, so a
//             listen host with guests connected is not held up by a single frame. Seamless travel never
//             broadcasts PreLoadMap and so never shows (or waits on) a card.
//
// THREADS. The widget paints on the SlateLoadingThread, then on the game thread inside
// WaitForMovieToFinish. So it paints from state that is fixed before it goes up: strings copied at
// construction, textures loaded and rooted by the game thread (the glyph atlas by TraceText's
// FAtlasRef, the kit sprites by TraceMenuKit::Sprite's root, the Lato fallback by TraceMenuArtStyle's
// FFontRef) and three atomics. It loads nothing, logs nothing, and calls no engine function. Every
// LoadObject in this file runs on the game thread.

#include "UI/TraceLoadingScreen.h"

#include <atomic>

#include "Brushes/SlateColorBrush.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "MoviePlayer.h"
#include "Rendering/DrawElements.h"
#include "RenderingThread.h"          // FlushRenderingCommands
#include "Styling/SlateBrush.h"
#include "UObject/GCObject.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SLeafWidget.h"

#if !UE_BUILD_SHIPPING
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"              // TActorIterator — the verify harness finds the title HUD
#include "ImageUtils.h"               // -TraceCardShot's PNG
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#endif

#include "Trace.h"                                  // LogTraceGame
#include "Audio/TraceSoundBank.h"                   // UTraceAudioSettings — the sound bank's path
#include "Core/TracePreload.h"
#include "Gameplay/TraceKnifeView.h"                // UTraceKnifeViewSubsystem::AppendWarmAssetPaths
#include "Settings/TraceGamepadInput.h"             // UTraceGamepadInputSubsystem::AppendWarmAssetPaths
#include "UI/Text/TraceAtlasTextWidget.h"           // STraceAtlasText::PaintString — the one Slate blitter
#include "UI/Text/TraceGameText.h"
#include "UI/Text/TraceText.h"
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"
#include "UI/Widgets/Menu/TraceMenuKit.h"
#include "UI/Widgets/Menu/TraceTitleMenuWidget.h"   // TraceTitleLayout — the lockup, the caption, the crescent

#if !UE_BUILD_SHIPPING
#include "UI/TraceMenuHUD.h"
#include "UI/Widgets/Menu/TraceMenuPalette.h"       // TraceMenuStyle::ActivationGraceSeconds
#endif

// Named after the file for the unity build (Scripts/check-jumbo-build-collisions.py); never anonymous.
namespace TraceLoadingScreenFile
{
	using TraceLoadingScreen::ECard;

	/** The studio name's cap height, 1080-reference px. The travel caption's is 22; the name leads it. */
	static constexpr float StudioCap = 26.f;

	/** The studio name may use at most this fraction of the view's width (it shrinks to fit). */
	static constexpr float StudioMaxWidthFraction = 0.80f;

	/** The crescent's own fade-in when it appears on the studio card (loading outlasted the hold). */
	static constexpr double SpinnerFadeSeconds = 0.30;

	static const TCHAR* CardName(ECard Kind)
	{
		switch (Kind)
		{
		case ECard::Studio: return TEXT("studio");
		case ECard::Travel: return TEXT("travel");
		default:            return TEXT("none");
		}
	}

	/** Smoothstep on 0..1 — the kit's fade curve (FTraceKitFade::Alpha). */
	static float Ease(double Linear)
	{
		const float P = FMath::Clamp(static_cast<float>(Linear), 0.f, 1.f);
		return P * P * (3.f - 2.f * P);
	}

	// ---------------------------------------------------------------------------------------------
	// WHAT A CARD SHOWS — fixed on the game thread before the card goes up, read-only afterwards
	// ---------------------------------------------------------------------------------------------

	/** One sprite, with the aspect read on the game thread (UTexture2D sizes are not a paint-thread question). */
	struct FCardSprite
	{
		UTexture2D* Texture = nullptr;
		/** Height / width. */
		float Aspect = 0.f;

		bool IsSet() const { return Texture != nullptr && Aspect > 0.f; }
	};

	struct FCardContent
	{
		ECard Kind = ECard::None;
		/** The studio's name, or the travel caption (may be empty: no caption line). */
		FString Line;
		FCardSprite Mark;
		FCardSprite Swoosh;
		FCardSprite Crescent;
	};

	/**
	 * The card's clock, shared between the painting thread (reads) and the game thread (writes).
	 * FPlatformTime::Seconds() throughout — FApp's clock does not advance while the engine is not ticking.
	 */
	struct FCardClock
	{
		std::atomic<double> Up{ 0.0 };
		/** When the studio name began fading out; < 0 until then. */
		std::atomic<double> FadeOutStart{ -1.0 };
		/** When the game thread first saw loading finished; 0 until then. */
		std::atomic<double> LoadingDone{ 0.0 };
		/** Dev mount only: >= 0 freezes the card at this many seconds after Up. Set before it is shown. */
		double FrozenAt = -1.0;

		/**
		 * How many frames of this card were painted by the movie player's own thread, and how many by the
		 * game thread (inside WaitForMovieToFinish, or the dev mount). The first is the proof the card was
		 * really on screen while the game thread was busy — which is the whole point of it.
		 */
		std::atomic<int32> LoadingThreadPaints{ 0 };
		std::atomic<int32> GameThreadPaints{ 0 };

		double NowSeconds() const
		{
			return (FrozenAt >= 0.0) ? Up.load() + FrozenAt : FPlatformTime::Seconds();
		}
	};

	/** A kit sprite for a card: loaded and rooted by the kit, drawable, with its aspect. Game thread. */
	static FCardSprite SpriteFor(ETraceKitSprite Which)
	{
		FCardSprite Out;
		// Sprite() loads and ROOTS it, and returns null until its render resource exists — so a card
		// only ever holds a texture the paint thread can hand to Slate.
		UTexture2D* Texture = TraceMenuKit::Sprite(Which);
		if (Texture != nullptr && Texture->GetSizeX() > 0 && Texture->GetSizeY() > 0)
		{
			Out.Texture = Texture;
			Out.Aspect = static_cast<float>(Texture->GetSizeY()) / static_cast<float>(Texture->GetSizeX());
		}
		return Out;
	}

	// ---------------------------------------------------------------------------------------------
	// THE CARD — one Slate leaf, both kinds
	// ---------------------------------------------------------------------------------------------

	class STraceLoadingCard : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(STraceLoadingCard) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const FCardContent& InContent, const TSharedRef<FCardClock>& InClock)
		{
			CardContent = InContent;
			CardClock = InClock;
			SetCanTick(false);
			// Repainted every frame whatever the window's invalidation mode: it animates, and a cached
			// paint would be a frozen card — the very thing it is here to replace.
			ForceVolatile(true);

			// Brushes are built HERE, on the game thread, and only read while painting.
			const auto MakeSpriteBrush = [](FSlateBrush& OutBrush, const FCardSprite& Sprite)
			{
				if (Sprite.IsSet())
				{
					OutBrush.SetResourceObject(Sprite.Texture);
					OutBrush.DrawAs = ESlateBrushDrawType::Image;
					OutBrush.Tiling = ESlateBrushTileType::NoTile;
					OutBrush.Margin = FMargin(0.f);
					OutBrush.ImageSize = FVector2f(static_cast<float>(Sprite.Texture->GetSizeX()),
						static_cast<float>(Sprite.Texture->GetSizeY()));
				}
			};
			MakeSpriteBrush(MarkBrush, CardContent.Mark);
			MakeSpriteBrush(SwooshBrush, CardContent.Swoosh);
			MakeSpriteBrush(CrescentBrush, CardContent.Crescent);
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(1920.0, 1080.0);
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
			const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
			const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	private:
		FCardContent CardContent;
		TSharedPtr<FCardClock> CardClock;
		FSlateColorBrush FillBrush{ FLinearColor::White };
		FSlateBrush MarkBrush;
		FSlateBrush SwooshBrush;
		FSlateBrush CrescentBrush;

		/** A sprite, axis-aligned, at a rect in local units. */
		void PaintSprite(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo, const FSlateBrush& Brush,
			const FVector2f& Pos, const FVector2f& Size, float Alpha) const
		{
			FSlateDrawElement::MakeBox(Out, Layer, Geo.ToPaintGeometry(Size, FSlateLayoutTransform(Pos)), &Brush,
				ESlateDrawEffect::None, FLinearColor(1.f, 1.f, 1.f, Alpha));
		}

		/** One line of white Sofachrome Light, its CAPS centred on (@p CentreX, @p CapCentreY) — TraceMenuKit::DrawCapText's rule. */
		int32 PaintCapLine(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo, const FString& Text,
			float CentreX, float CapCentreY, float CapH, float MaxW, float Alpha) const
		{
			if (Text.IsEmpty() || CapH <= 0.f || Alpha <= 0.f)
			{
				return Layer;
			}
			TraceText::FStyle Style(TraceText::SizeForCapHeight(CapH, ETraceTextWeight::Light),
				TraceMenuArtStyle::WordDefault, ETraceTextWeight::Light);
			Style.HAlign = TraceText::EHAlign::Center;
			Style.VAlign = TraceText::EVAlign::CapTop;
			if (MaxW > 0.f)
			{
				const float Natural = TraceText::MeasureWidth(Text, Style);
				if (Natural > MaxW && Natural > 0.f)
				{
					Style.Size *= MaxW / Natural;
				}
			}
			const float Caps = TraceText::CapHeight(Style.Size, Style.Weight);
			FLinearColor Ink = TraceMenuArtStyle::WordDefault;
			Ink.A *= Alpha;
			return STraceAtlasText::PaintString(Text, Style, FVector2f(CentreX, CapCentreY - Caps * 0.5f), Geo, Out, Layer, Ink);
		}
	};

	int32 STraceLoadingCard::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
		const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
	{
		const FVector2f Local = FVector2f(AllottedGeometry.GetLocalSize());
		if (Local.X <= 1.f || Local.Y <= 1.f || !CardClock.IsValid())
		{
			return LayerId;
		}
		(IsInGameThread() ? CardClock->GameThreadPaints : CardClock->LoadingThreadPaints).fetch_add(1, std::memory_order_relaxed);

		// THE KIT'S BLACK, opaque, whatever the card is doing (stylespec §1). The movie player's own
		// border is black too; this does not rely on it.
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), &FillBrush,
			ESlateDrawEffect::None, FLinearColor::Black);
		int32 Layer = LayerId + 1;

		// ONE REFERENCE PIXEL IN LOCAL UNITS. The Canvas screens use UIScale = clamp(ViewH / 1080, 0.5, 2)
		// on PIXELS; Slate's local units are pixels divided by the geometry's scale (the window's DPI
		// factor, which the loading thread applies and the game thread's window also applies). Sizing
		// from pixels and converting back keeps the card identical to the Canvas card it replaces, and
		// identical between the two threads, at any DPI.
		const float PixelsPerUnit = FMath::Max(AllottedGeometry.GetAccumulatedLayoutTransform().GetScale(), KINDA_SMALL_NUMBER);
		const float UIScalePx = FMath::Clamp(Local.Y * PixelsPerUnit / 1080.f, 0.5f, 2.f);
		const float RefPx = UIScalePx / PixelsPerUnit;

		const double Now = CardClock->NowSeconds();
		const double Up = CardClock->Up.load();
		const double CardSeconds = FMath::Max(0.0, Now - Up);

		// The crescent, shared by both kinds: bottom-right, turning on the platform clock — the same
		// box, pivot and clock as TraceMenuKit::DrawTravelSpinner on the Canvas card.
		const auto PaintCrescent = [&](float CrescentAlpha)
		{
			if (!CardContent.Crescent.IsSet() || CrescentAlpha <= 0.f)
			{
				return;
			}
			const float SpinH = TraceTitleLayout::SpinnerHeight * RefPx;
			const float SpinW = SpinH / CardContent.Crescent.Aspect;
			const float Inset = TraceTitleLayout::SpinnerInset * RefPx;
			const FVector2f Pos(Local.X - Inset - SpinW, Local.Y - Inset - SpinH);
			FSlateDrawElement::MakeRotatedBox(OutDrawElements, Layer,
				AllottedGeometry.ToPaintGeometry(FVector2f(SpinW, SpinH), FSlateLayoutTransform(Pos)),
				&CrescentBrush, ESlateDrawEffect::None,
				FMath::DegreesToRadians(TraceTitleLayout::SpinnerAngleDegrees(Now)),
				TOptional<FVector2f>(), FSlateDrawElement::RelativeToElement,
				FLinearColor(1.f, 1.f, 1.f, CrescentAlpha));
		};

		if (CardContent.Kind == ECard::Studio)
		{
			// In over StudioFadeInSeconds; out over StudioFadeOutSeconds from whenever the game thread
			// started the fade (TickStudioDismissal: hold over AND loading finished).
			const float In = Ease(CardSeconds / TraceLoadingScreen::StudioFadeInSeconds);
			const double FadeStart = CardClock->FadeOutStart.load();
			const float Out = (FadeStart >= 0.0)
				? 1.f - Ease((Now - FadeStart) / TraceLoadingScreen::StudioFadeOutSeconds)
				: 1.f;
			const float Alpha = In * Out;

			Layer = PaintCapLine(OutDrawElements, Layer, AllottedGeometry, CardContent.Line, Local.X * 0.5f, Local.Y * 0.5f,
				StudioCap * RefPx, Local.X * StudioMaxWidthFraction, Alpha);

			// Only when the load outlasts the hold: a card still up after its name has been read should
			// say it is working. It never appears when loading finished inside the hold (a packaged build,
			// usually), and it leaves with the name.
			const double LoadingDone = CardClock->LoadingDone.load();
			const double HoldEnd = Up + TraceLoadingScreen::StudioHoldSeconds;
			const bool bOutlasted = CardSeconds > TraceLoadingScreen::StudioHoldSeconds
				&& (LoadingDone <= 0.0 || LoadingDone > HoldEnd + 0.05);
			if (bOutlasted)
			{
				PaintCrescent(Ease((Now - HoldEnd) / SpinnerFadeSeconds) * Out);
			}
			return Layer + 1;
		}

		// TRAVEL — the title's travel overlay, exactly (ATraceMenuHUD::DrawTravelOverlay): the lockup at
		// the title's own place, the caption, the crescent. No fade: it replaces a card that looks the same.
		if (CardContent.Mark.IsSet())
		{
			const TraceTitleLayout::FTitleBlockRects Block = TraceTitleLayout::ComputeTitleBlock(Local.X, RefPx,
				CardContent.Mark.Aspect, CardContent.Swoosh.IsSet() ? CardContent.Swoosh.Aspect : 0.f);
			PaintSprite(OutDrawElements, Layer, AllottedGeometry, MarkBrush, Block.MarkPos, Block.MarkSize, 1.f);
			if (Block.bSwoosh)
			{
				PaintSprite(OutDrawElements, Layer, AllottedGeometry, SwooshBrush, Block.SwooshPos, Block.SwooshSize,
					TraceTitleLayout::SwooshOpacity);
			}
			++Layer;
		}

		Layer = PaintCapLine(OutDrawElements, Layer, AllottedGeometry, CardContent.Line, Local.X * 0.5f,
			Local.Y * TraceTitleLayout::TravelCaptionY, TraceTitleLayout::TravelCaptionCap * RefPx,
			Local.X - 2.f * TraceTitleLayout::TravelCaptionSideClear * RefPx, 1.f);
		PaintCrescent(1.f);
		return Layer + 1;
	}

	/**
	 * A card for the movie player: hit-testable only so it can say "no pointer" — the OS arrow otherwise
	 * sits on the black card while the game thread waits. A click still reaches the movie player's own
	 * border (it bubbles), which is what lets a click skip the studio card once loading has finished.
	 */
	static TSharedRef<SWidget> MakeScreenCard(const FCardContent& InContent, const TSharedRef<FCardClock>& InClock)
	{
		const TSharedRef<STraceLoadingCard> Card = SNew(STraceLoadingCard, InContent, InClock);
		Card->SetCursor(TOptional<EMouseCursor::Type>(EMouseCursor::None));
		return Card;
	}

	// ---------------------------------------------------------------------------------------------
	// PROCESS STATE — game thread
	// ---------------------------------------------------------------------------------------------

	static bool GStarted = false;
	static FDelegateHandle GPreLoadHandle;
	static FDelegateHandle GPostEngineInitHandle;
	static FDelegateHandle GStartedHandle;
	static FDelegateHandle GFinishedHandle;
	static FDelegateHandle GPreTickHandle;

	/** The card handed to the movie player and not yet reported started. */
	static ECard GPendingCard = ECard::None;
	static FString GPendingMap;
	static FString GPendingLine;

	/** The card on screen now. */
	static ECard GCardUp = ECard::None;
	static TSharedPtr<FCardClock> GClockUp;
	static TSharedPtr<SWidget> GWidgetUp;
	static bool GStoppedByUs = false;
	static bool GWatchdogFired = false;

	static double GLastEndSeconds = 0.0;
	static ECard GLastEndKind = ECard::None;

	/** The line the next travel card shows (SetNextTravelCaption). */
	static FString GNextCaption;

#if !UE_BUILD_SHIPPING
	static TArray<TraceLoadingScreen::FCardRecord> GCards;
	static TArray<TraceLoadingScreen::FLoadRecord> GLoads;
	static FString GStudioSkipReason;

	static TraceLoadingScreen::FCardRecord* OpenCard()
	{
		return (GCardUp != ECard::None && GCards.Num() > 0) ? &GCards.Last() : nullptr;
	}
#endif

	/**
	 * Holds what the warm-up loaded, for the life of the process — GC would otherwise drop it at the
	 * first LoadMap and the next world would load it again, which is the per-travel cost it removes.
	 */
	class FWarmRefs : public FGCObject
	{
	public:
		TArray<TObjectPtr<UObject>> Held;

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			Collector.AddReferencedObjects(Held);
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("TraceLoadingScreen::Warm");
		}
	};

	static FWarmRefs& Warm()
	{
		static FWarmRefs Instance;
		return Instance;
	}

	/** "MainMenu" for "/Game/Maps/MainMenu", "MainMenu?listen", "/Game/Maps/MainMenu.MainMenu" or "MainMenu". */
	static FString MapShortName(const FString& InMap)
	{
		FString Out = InMap;
		int32 Cut = INDEX_NONE;
		if (Out.FindChar(TEXT('?'), Cut))
		{
			Out.LeftInline(Cut);
		}
		if (Out.FindLastChar(TEXT('/'), Cut))
		{
			Out.RightChopInline(Cut + 1);
		}
		if (Out.FindChar(TEXT('.'), Cut))
		{
			Out.LeftInline(Cut);
		}
		return Out.TrimStartAndEnd();
	}

	/** GameDefaultMap — the title. Read from config rather than UGameMapsSettings (no EngineSettings dependency). */
	static FString DefaultMapShortName()
	{
		FString DefaultMap;
		if (GConfig != nullptr)
		{
			GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameDefaultMap"), DefaultMap, GEngineIni);
		}
		return MapShortName(DefaultMap);
	}

	static bool IsTitleMap(const FString& InMap)
	{
		const FString Default = DefaultMapShortName();
		return !Default.IsEmpty() && MapShortName(InMap).Equals(Default, ESearchCase::IgnoreCase);
	}

	/** The caption rule: the pending line, except on a load of the title (a failed JOIN going home). */
	static FString CaptionFor(const FString& InMap, const FString& InPending)
	{
		return IsTitleMap(InMap) ? FString() : InPending;
	}

	/**
	 * Whether this boot shows the studio card: only when the game starts on its default map (the
	 * title). A harness that launches straight into Arena_Baked skips it; so would a player's shortcut
	 * with a map on it, except that SHIPPING ignores a map on the command line altogether
	 * (UGameInstance::StartGameInstance), so a player always boots the title and always sees the card.
	 */
	static bool ShouldShowStudioCard(FString& OutWhyNot)
	{
#if UE_BUILD_SHIPPING
		return true;
#else
		// The first bare token (or -map=), exactly the parse UGameInstance::GetMapOverrideName does for
		// the boot's Browse; that function is protected, so this is its six lines, not a call.
		FString Override;
		const TCHAR* Scan = FCommandLine::Get();
		while (Scan != nullptr && *Scan != TEXT('\0'))
		{
			const FString Token = FParse::Token(Scan, false);
			if (Token.IsEmpty())
			{
				continue;
			}
			if (Token[0] != TEXT('-'))
			{
				Override = Token;
				break;
			}
			if (FParse::Value(*Token, TEXT("-map="), Override))
			{
				break;
			}
		}
		if (Override.IsEmpty() || IsTitleMap(Override))
		{
			return true;
		}
		OutWhyNot = FString::Printf(TEXT("launched straight into %s, not the title"), *MapShortName(Override));
		return false;
#endif
	}

	// ---------------------------------------------------------------------------------------------
	// THE STUDIO CARD'S DISMISSAL — game thread, inside WaitForMovieToFinish
	// ---------------------------------------------------------------------------------------------

#if !UE_BUILD_SHIPPING
	/**
	 * -TraceCardShot: one capture of the REAL WINDOW while the studio card owns it, read back from the
	 * Slate renderer (FSlateApplication::TakeScreenshot) — what the window shows, through the same
	 * renderer, not a re-creation of it. Taken on the game thread inside WaitForMovieToFinish, on the
	 * tick loading is first seen finished (the name fully up). Needs no screen-recording permission and
	 * no unlocked screen. Writes Saved/Screenshots/TraceLoadingCard_studio_<time>.png.
	 *
	 * -RenderOffScreen ONLY. MEASURED: on a real window on macOS the back buffer is the Metal drawable,
	 * which cannot be read back — the renderer's readback of it is a SIGSEGV on the render thread
	 * (AGX getBytes). Off screen the window renders into an ordinary texture and the readback is fine.
	 */
	static FString CaptureCardWindow()
	{
		static const bool bWanted = FParse::Param(FCommandLine::Get(), TEXT("TraceCardShot"));
		if (!bWanted || !GWidgetUp.IsValid() || !FSlateApplication::IsInitialized())
		{
			return FString();
		}
		if (!FSlateApplication::Get().IsRenderingOffScreen())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[LoadingCard] -TraceCardShot is ignored in an on-screen window (its drawable cannot be read "
				     "back); add -RenderOffScreen, or photograph the window with screencapture."));
			return FString();
		}

		TArray<FColor> Pixels;
		FIntVector Size(0, 0, 0);
		if (!FSlateApplication::Get().TakeScreenshot(GWidgetUp.ToSharedRef(), Pixels, Size) || Size.X <= 0 || Size.Y <= 0)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[LoadingCard] -TraceCardShot: the window could not be read back."));
			return FString();
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}

		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		const FString ShotPath = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir() / FString::Printf(
			TEXT("TraceLoadingCard_%s_%s.png"), CardName(GCardUp), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
		if (!FFileHelper::SaveArrayToFile(Png, *ShotPath))
		{
			return FString();
		}
		UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] Window capture of the %s card (%dx%d): %s"),
			CardName(GCardUp), Size.X, Size.Y, *ShotPath);
		return ShotPath;
	}
#endif

	static void TickStudioDismissal(float /*DeltaTime*/)
	{
		IGameMoviePlayer* Player = GetMoviePlayer();
		if (GCardUp != ECard::Studio || !GClockUp.IsValid() || Player == nullptr)
		{
			return;
		}

		// True only once WaitForMovieToFinish has begun: no minimum display time is set, so the engine
		// marks loading done the moment it starts waiting. Before that the game thread is still loading
		// and this tick does not even run.
		if (!Player->IsLoadingFinished())
		{
			return;
		}

		FCardClock& Clock = *GClockUp;
		const double Now = FPlatformTime::Seconds();
		if (Clock.LoadingDone.load() <= 0.0)
		{
			Clock.LoadingDone.store(Now);
#if !UE_BUILD_SHIPPING
			if (TraceLoadingScreen::FCardRecord* Record = OpenCard())
			{
				Record->LoadingDone = Now;
				Record->ShotPath = CaptureCardWindow();
			}
#endif
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadingCard] Loading finished under the studio card at +%.2fs; it fades out at +%.2fs."),
				Now - Clock.Up.load(),
				FMath::Max(Now, Clock.Up.load() + TraceLoadingScreen::StudioHoldSeconds) - Clock.Up.load());
		}

		double FadeStart = Clock.FadeOutStart.load();
		if (FadeStart < 0.0 && Now - Clock.Up.load() >= TraceLoadingScreen::StudioHoldSeconds)
		{
			FadeStart = Now;
			Clock.FadeOutStart.store(Now);
#if !UE_BUILD_SHIPPING
			if (TraceLoadingScreen::FCardRecord* Record = OpenCard())
			{
				Record->FadeOutStart = Now;
			}
#endif
		}

		if (FadeStart >= 0.0 && Now - FadeStart >= TraceLoadingScreen::StudioFadeOutSeconds)
		{
			GStoppedByUs = true;
			Player->StopMovie();
		}
		else if (Now - Clock.LoadingDone.load() > TraceLoadingScreen::StudioWatchdogSeconds)
		{
			GStoppedByUs = true;
			GWatchdogFired = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[LoadingCard] The studio card was still up %.1fs after loading finished; stopping it. "
				     "Its fade never completed — the card's clock and this tick disagree."),
				TraceLoadingScreen::StudioWatchdogSeconds);
			Player->StopMovie();
		}
	}

	static void BindDismissal()
	{
		if (!GPreTickHandle.IsValid() && FSlateApplication::IsInitialized())
		{
			GPreTickHandle = FSlateApplication::Get().OnPreTick().AddStatic(&TickStudioDismissal);
		}
	}

	static void UnbindDismissal()
	{
		if (GPreTickHandle.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnPreTick().Remove(GPreTickHandle);
		}
		GPreTickHandle.Reset();
	}

	// ---------------------------------------------------------------------------------------------
	// THE MOVIE PLAYER'S EVENTS — game thread
	// ---------------------------------------------------------------------------------------------

	static void OnPlaybackStarted()
	{
		// Broadcast inside PlayMovie, after the loading thread has started painting. Anything we did not
		// hand the movie player ourselves (a startup movie, say) is not ours to describe.
		if (GPendingCard == ECard::None)
		{
			return;
		}

		GCardUp = GPendingCard;
		GPendingCard = ECard::None;
		GStoppedByUs = false;
		GWatchdogFired = false;

		const double Now = FPlatformTime::Seconds();
#if !UE_BUILD_SHIPPING
		TraceLoadingScreen::FCardRecord Record;
		Record.Kind = GCardUp;
		Record.Map = GPendingMap;
		Record.Caption = GPendingLine;
		Record.Up = GClockUp.IsValid() ? GClockUp->Up.load() : Now;
		GCards.Add(Record);
#endif

		if (GCardUp == ECard::Studio)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadingCard] Card up: studio ('%s'). Holds %.2fs, then fades out once the engine has loaded."),
				*GPendingLine, TraceLoadingScreen::StudioHoldSeconds);
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] Card up: travel -> %s%s%s"),
				*MapShortName(GPendingMap), GPendingLine.IsEmpty() ? TEXT("") : TEXT(" — "), *GPendingLine);
		}
	}

	static void OnPlaybackFinished()
	{
		// End of WaitForMovieToFinish: the window is the game's again.
		UnbindDismissal();
		if (GCardUp == ECard::None)
		{
			return;
		}

		const double Now = FPlatformTime::Seconds();
		const double Up = GClockUp.IsValid() ? GClockUp->Up.load() : Now;

		if (GCardUp == ECard::Studio)
		{
			const double LoadingDone = GClockUp.IsValid() ? GClockUp->LoadingDone.load() : 0.0;
			const double FadeStart = GClockUp.IsValid() ? GClockUp->FadeOutStart.load() : -1.0;
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadingCard] Card down: studio after %.2fs (loading finished at +%.2fs, fade-out from +%.2fs, %s)."),
				Now - Up,
				LoadingDone > 0.0 ? LoadingDone - Up : -1.0,
				FadeStart >= 0.0 ? FadeStart - Up : -1.0,
				GWatchdogFired ? TEXT("ended by the watchdog")
					: (GStoppedByUs ? TEXT("ended by its fade") : TEXT("skipped by a key or click, or the window closed")));
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] Card down: travel after %.2fs."), Now - Up);
		}

		const int32 LoadingPaints = GClockUp.IsValid() ? GClockUp->LoadingThreadPaints.load() : 0;
		const int32 GamePaints = GClockUp.IsValid() ? GClockUp->GameThreadPaints.load() : 0;
		UE_LOG(LogTraceGame, Display,
			TEXT("[LoadingCard]   painted %d frame(s) on the movie player's thread and %d on the game thread."),
			LoadingPaints, GamePaints);

#if !UE_BUILD_SHIPPING
		if (TraceLoadingScreen::FCardRecord* Record = OpenCard())
		{
			Record->Down = Now;
			Record->bStoppedByUs = GStoppedByUs;
			Record->bWatchdog = GWatchdogFired;
			Record->LoadingThreadPaints = LoadingPaints;
			Record->GameThreadPaints = GamePaints;
		}
#endif

		GLastEndSeconds = Now;
		GLastEndKind = GCardUp;
		GCardUp = ECard::None;
		GClockUp.Reset();
		GWidgetUp.Reset();
	}

	// ---------------------------------------------------------------------------------------------
	// TRAVEL — every LoadMap after boot
	// ---------------------------------------------------------------------------------------------

	static void NoteLoad(const FString& InMap, bool bCard, const TCHAR* WhyNot)
	{
#if !UE_BUILD_SHIPPING
		TraceLoadingScreen::FLoadRecord Record;
		Record.Map = InMap;
		Record.At = FPlatformTime::Seconds();
		Record.bCard = bCard;
		Record.WhyNot = WhyNot;
		GLoads.Add(Record);
#endif
	}

	static void OnPreLoadMap(const FWorldContext& InContext, const FString& InMapName)
	{
		if (InContext.WorldType != EWorldType::Game)
		{
			NoteLoad(InMapName, false, TEXT("not a game world"));
			return;
		}

		IGameMoviePlayer* Player = GetMoviePlayer();
		if (Player == nullptr || !Player->IsInitialized())
		{
			NoteLoad(InMapName, false, TEXT("no movie player"));
			return;
		}

		// The boot LoadMap arrives while the studio card is up. That card covers it; its attributes
		// (manual stop) must not be replaced by a travel card's.
		if (Player->IsMovieCurrentlyPlaying())
		{
#if !UE_BUILD_SHIPPING
			if (GCardUp == ECard::Studio)
			{
				if (TraceLoadingScreen::FCardRecord* Record = OpenCard())
				{
					Record->Map = InMapName;
				}
			}
#endif
			NoteLoad(InMapName, GCardUp == ECard::Studio,
				GCardUp == ECard::Studio ? TEXT("") : TEXT("another loading screen is already up"));
			return;
		}

		FCardContent Content;
		Content.Kind = ECard::Travel;
		Content.Line = CaptionFor(InMapName, GNextCaption);
		GNextCaption.Reset();
		Content.Mark = SpriteFor(ETraceKitSprite::Wordmark);
		Content.Swoosh = SpriteFor(ETraceKitSprite::Swoosh);
		Content.Crescent = SpriteFor(ETraceKitSprite::Chevron);

		const TSharedRef<FCardClock> Clock = MakeShared<FCardClock>();
		Clock->Up.store(FPlatformTime::Seconds());

		FLoadingScreenAttributes Attributes;
		Attributes.WidgetLoadingScreen = MakeScreenCard(Content, Clock);
		GWidgetUp = Attributes.WidgetLoadingScreen;
		Attributes.MinimumLoadingScreenDisplayTime = -1.f;      // no minimum: it adds no wait to a load
		Attributes.bAutoCompleteWhenLoadingCompletes = true;
		Attributes.bWaitForManualStop = false;
		Attributes.bMoviesAreSkippable = true;
		Attributes.bAllowEngineTick = false;                    // MoviePlayer.h: "potentially unsafe"
		Player->SetupLoadingScreen(Attributes);

		GPendingCard = ECard::Travel;
		GPendingMap = InMapName;
		GPendingLine = Content.Line;
		GClockUp = Clock;
		NoteLoad(InMapName, true, TEXT(""));
	}

	// ---------------------------------------------------------------------------------------------
	// THE WARM-UP BEHIND THE STUDIO CARD (P11's first-use hitch list, section A) — game thread
	// ---------------------------------------------------------------------------------------------

	/**
	 * At FCoreDelegates::GetOnPostEngineInit: the engine and the game instance exist, the boot LoadMap
	 * has not happened yet, and the studio card is on screen. Everything here is a synchronous load on
	 * the game thread, which is exactly what a loading screen is for; everything loaded is HELD (FWarmRefs)
	 * so no later world pays for it again.
	 *
	 *   match tables       TracePreload::WarmMatchTables — the roster and the ability table, which latch.
	 *   sound bank         DA_TraceSoundBank and its waves: TryLoaded per WORLD and dropped at every travel.
	 *   knife art          the pack blade's mesh and clips: loaded on the first tick of EVERY world, the title's too.
	 *   pad actions        the IA_ assets the pad context resolves by name: a FlushAsyncLoading on the
	 *                      title's frame 0.
	 *
	 * The glyph atlas, the Lato fallback and the kit sprites are warmed earlier still, in Startup(),
	 * because the card itself draws with them. NOT here, on purpose: the arena's own content (it loads
	 * inside LoadMap, under the travel card, since P11) and first-draw PSO compiles, which need real
	 * draws of the real materials and cannot happen behind a card that draws none.
	 */
	static void WarmUnderStudioCard()
	{
		const double Start = FPlatformTime::Seconds();

		TracePreload::WarmMatchTables();

		TArray<FSoftObjectPath> Paths;
		const FSoftObjectPath& BankPath = UTraceAudioSettings::Get().SoundBankPath;
		if (BankPath.IsValid())
		{
			Paths.Add(BankPath);
		}
		UTraceKnifeViewSubsystem::AppendWarmAssetPaths(Paths);
		UTraceGamepadInputSubsystem::AppendWarmAssetPaths(Paths);

		int32 Held = 0;
		for (const FSoftObjectPath& Path : Paths)
		{
			if (UObject* Loaded = Path.TryLoad())
			{
				Warm().Held.AddUnique(Loaded);
				++Held;
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Preload] Boot warm-up behind the studio card: match tables, then %d of %d assets held for "
			     "the process (sound bank, knife, pad actions) in %.1f ms."),
			Held, Paths.Num(), (FPlatformTime::Seconds() - Start) * 1000.0);
	}
}

// =================================================================================================
// Lifetime
// =================================================================================================

void TraceLoadingScreen::Startup()
{
	namespace F = TraceLoadingScreenFile;

	// The engine's own rule decides: false in the editor and PIE, commandlets, a dedicated server,
	// -nullrhi and -NoLoadingScreen. None of them gets a card, a delegate or a load from this file.
	if (!IsMoviePlayerEnabled() || GetMoviePlayer() == nullptr || !GetMoviePlayer()->IsInitialized())
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("[LoadingCard] No loading cards: the movie player is off here (editor, PIE, commandlet, "
			     "dedicated server, -nullrhi or -NoLoadingScreen)."));
		return;
	}
	F::GStarted = true;

	IGameMoviePlayer* Player = GetMoviePlayer();
	F::GStartedHandle = Player->OnMoviePlaybackStarted().AddStatic(&F::OnPlaybackStarted);
	F::GFinishedHandle = Player->OnMoviePlaybackFinished().AddStatic(&F::OnPlaybackFinished);
	F::GPreLoadHandle = FCoreUObjectDelegates::PreLoadMapWithContext.AddStatic(&F::OnPreLoadMap);
	F::GPostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&F::WarmUnderStudioCard);

	// ---- What every card draws with, loaded NOW, here, on the game thread ------------------------
	//
	// The card paints on the Slate loading thread, where nothing may be loaded; so the glyph sheets,
	// the Lato fallback and the kit sprites are resolved and kept alive before it can paint, and the
	// render thread is flushed so their RHI textures exist before its first frame. This is also P11's
	// hitch list items A1, A2 and A4 moved from the title's first frames to behind the card.
	const double WarmStart = FPlatformTime::Seconds();
	TraceText::AtlasTexture(ETraceTextWeight::Light);   // resolves all four sheets (TraceText's FAtlasRef holds them)
	TraceMenuArtStyle::MenuFont(24.f);                    // the Lato fallback (TraceMenuArtStyle's FFontRef holds it)
	const int32 Sprites = TraceMenuKit::Prime();         // the kit sprites and the pointer, rooted
	FlushRenderingCommands();

	UE_LOG(LogTraceGame, Display,
		TEXT("[LoadingCard] Card art ready in %.1f ms: type %s, %d kit sprites."),
		(FPlatformTime::Seconds() - WarmStart) * 1000.0, *TraceText::FaceName(), Sprites);

	// ---- The studio card ------------------------------------------------------------------------
	FString WhyNot;
	bool bStudio = false;
	if (Player->IsMovieCurrentlyPlaying())
	{
		WhyNot = TEXT("a startup movie is already playing");
	}
	else
	{
		bStudio = F::ShouldShowStudioCard(WhyNot);
	}
	if (!bStudio)
	{
#if !UE_BUILD_SHIPPING
		F::GStudioSkipReason = WhyNot;
#endif
		UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] No studio card: %s."), *WhyNot);
		return;
	}

	F::FCardContent Content;
	Content.Kind = ECard::Studio;
	Content.Line = TRACE_TEXT("BOOT.STUDIO", "Oyster With a Gun Studios, Inc");
	Content.Crescent = F::SpriteFor(ETraceKitSprite::Chevron);

	const TSharedRef<F::FCardClock> Clock = MakeShared<F::FCardClock>();
	Clock->Up.store(FPlatformTime::Seconds());

	FLoadingScreenAttributes Attributes;
	Attributes.WidgetLoadingScreen = F::MakeScreenCard(Content, Clock);
	F::GWidgetUp = Attributes.WidgetLoadingScreen;
	Attributes.MinimumLoadingScreenDisplayTime = -1.f;   // the hold is TickStudioDismissal's, so the fade can follow loading
	Attributes.bAutoCompleteWhenLoadingCompletes = false;
	Attributes.bWaitForManualStop = true;
	Attributes.bMoviesAreSkippable = true;               // any key or click, once loading is done, cuts to the title
	Attributes.bAllowEngineTick = false;
	Player->SetupLoadingScreen(Attributes);

	F::GPendingCard = ECard::Studio;
	F::GPendingMap.Reset();
	F::GPendingLine = Content.Line;
	F::GClockUp = Clock;

	if (!Player->PlayMovie())
	{
		// Never leave a manual-stop card armed that nothing is driving: the boot LoadMap would start it
		// and FEngineLoop::Init would wait on it forever.
		Player->SetupLoadingScreen(FLoadingScreenAttributes());
		F::GPendingCard = ECard::None;
		F::GClockUp.Reset();
		F::GWidgetUp.Reset();
#if !UE_BUILD_SHIPPING
		F::GStudioSkipReason = TEXT("the movie player refused to play it");
#endif
		UE_LOG(LogTraceGame, Warning, TEXT("[LoadingCard] No studio card: the movie player refused to play it."));
		return;
	}

	F::BindDismissal();
}

void TraceLoadingScreen::Shutdown()
{
	namespace F = TraceLoadingScreenFile;
	if (!F::GStarted)
	{
		return;
	}
	F::GStarted = false;

	F::UnbindDismissal();
	FCoreUObjectDelegates::PreLoadMapWithContext.Remove(F::GPreLoadHandle);
	FCoreDelegates::GetOnPostEngineInit().Remove(F::GPostEngineInitHandle);

	// The movie player may already be gone at module shutdown; GetMoviePlayer() is null then.
	if (IGameMoviePlayer* Player = GetMoviePlayer())
	{
		Player->OnMoviePlaybackStarted().Remove(F::GStartedHandle);
		Player->OnMoviePlaybackFinished().Remove(F::GFinishedHandle);
	}
	F::GClockUp.Reset();
	F::GWidgetUp.Reset();
}

// =================================================================================================
// For the rest of the game
// =================================================================================================

void TraceLoadingScreen::SetNextTravelCaption(const FString& Caption)
{
	TraceLoadingScreenFile::GNextCaption = Caption;
}

bool TraceLoadingScreen::IsCardUp()
{
	return TraceLoadingScreenFile::GCardUp != ECard::None;
}

TraceLoadingScreen::ECard TraceLoadingScreen::CurrentCardKind()
{
	return TraceLoadingScreenFile::GCardUp;
}

double TraceLoadingScreen::LastCardEndSeconds()
{
	return TraceLoadingScreenFile::GLastEndSeconds;
}

TraceLoadingScreen::ECard TraceLoadingScreen::LastCardKind()
{
	return TraceLoadingScreenFile::GLastEndKind;
}

bool TraceLoadingScreen::IsWithinCardGrace(double GraceSeconds)
{
	if (IsCardUp())
	{
		return true;
	}
	const double LastEnd = TraceLoadingScreenFile::GLastEndSeconds;
	return LastEnd > 0.0 && (FPlatformTime::Seconds() - LastEnd) < GraceSeconds;
}

#if !UE_BUILD_SHIPPING

const TArray<TraceLoadingScreen::FCardRecord>& TraceLoadingScreen::DebugCards()
{
	return TraceLoadingScreenFile::GCards;
}

const TArray<TraceLoadingScreen::FLoadRecord>& TraceLoadingScreen::DebugLoads()
{
	return TraceLoadingScreenFile::GLoads;
}

const FString& TraceLoadingScreen::DebugStudioSkipReason()
{
	return TraceLoadingScreenFile::GStudioSkipReason;
}

double TraceLoadingScreen::DebugSetLastCardEnd(double PlatformSeconds, ECard Kind)
{
	const double Old = TraceLoadingScreenFile::GLastEndSeconds;
	TraceLoadingScreenFile::GLastEndSeconds = PlatformSeconds;
	TraceLoadingScreenFile::GLastEndKind = Kind;
	return Old;
}

FString TraceLoadingScreen::DebugCaptionFor(const FString& MapName, const FString& Pending)
{
	return TraceLoadingScreenFile::CaptionFor(MapName, Pending);
}

// =================================================================================================
// DEV TOOLS — a card in the viewport for a screenshot, and the verdict on this process's cards
// =================================================================================================

namespace TraceLoadingScreenFile
{
	static TSharedPtr<SWidget> GMounted;

	static void Unmount()
	{
		if (GMounted.IsValid() && GEngine != nullptr && GEngine->GameViewport != nullptr)
		{
			GEngine->GameViewport->RemoveViewportWidgetContent(GMounted.ToSharedRef());
		}
		GMounted.Reset();
	}

	/**
	 * Trace.UI.LoadingCard studio|travel|off [seconds] [caption...]
	 *
	 * Mounts THE SAME widget the movie player shows over the game viewport, with its clock frozen at
	 * [seconds] after the card came up, so each state photographs the same way every time — the real
	 * card paints before any HUD exists and AutoShot cannot reach it. Studio: 0.2 is mid fade-in, 1.0
	 * fully up, 2.2 mid fade-out, 2.6 past the hold with loading still running (the crescent). Travel:
	 * the seconds only turn the crescent; the rest of the line is the caption.
	 */
	static void RunMountCommand(const TArray<FString>& Args)
	{
		Unmount();
		const FString Mode = Args.Num() > 0 ? Args[0].ToLower() : FString(TEXT("studio"));
		if (Mode == TEXT("off"))
		{
			UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] Dev card removed."));
			return;
		}
		if (GEngine == nullptr || GEngine->GameViewport == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[LoadingCard] Trace.UI.LoadingCard needs a game viewport."));
			return;
		}

		const double At = Args.Num() > 1 ? FCString::Atod(*Args[1]) : 1.0;
		FString Caption;
		for (int32 Index = 2; Index < Args.Num(); ++Index)
		{
			Caption += (Caption.IsEmpty() ? TEXT("") : TEXT(" ")) + Args[Index];
		}

		FCardContent Content;
		const TSharedRef<FCardClock> Clock = MakeShared<FCardClock>();
		Clock->Up.store(FPlatformTime::Seconds());
		Clock->FrozenAt = FMath::Max(0.0, At);
		Content.Crescent = SpriteFor(ETraceKitSprite::Chevron);
		if (Mode == TEXT("travel"))
		{
			Content.Kind = ECard::Travel;
			Content.Line = Caption;
			Content.Mark = SpriteFor(ETraceKitSprite::Wordmark);
			Content.Swoosh = SpriteFor(ETraceKitSprite::Swoosh);
		}
		else
		{
			Content.Kind = ECard::Studio;
			Content.Line = TRACE_TEXT("BOOT.STUDIO", "Oyster With a Gun Studios, Inc");
			// Past the hold the dev card behaves as if loading had finished AT the hold (so it shows the
			// fade-out) unless the time is past hold + fade, where it shows the crescent instead: the
			// "loading outlasted the hold" state.
			const double HoldEnd = TraceLoadingScreen::StudioHoldSeconds;
			if (At > HoldEnd && At < HoldEnd + TraceLoadingScreen::StudioFadeOutSeconds)
			{
				Clock->FadeOutStart.store(Clock->Up.load() + HoldEnd);
				Clock->LoadingDone.store(Clock->Up.load() + HoldEnd * 0.5);
			}
		}

		GMounted = SNew(STraceLoadingCard, Content, Clock).Visibility(EVisibility::HitTestInvisible);
		GEngine->GameViewport->AddViewportWidgetContent(GMounted.ToSharedRef(), /*ZOrder=*/10000);
		UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] Dev card up: %s at %.2fs%s%s."),
			CardName(Content.Kind), At, Content.Line.IsEmpty() ? TEXT("") : TEXT(", '"),
			Content.Line.IsEmpty() ? TEXT("") : *(Content.Line + TEXT("'")));
	}

	static FAutoConsoleCommand CmdLoadingCard(
		TEXT("Trace.UI.LoadingCard"),
		TEXT("studio|travel|off [seconds] [caption...] — mounts the loading card the movie player shows, over ")
		TEXT("the game viewport, frozen at [seconds] after it came up, for a screenshot. See UI/TraceLoadingScreen.cpp."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunMountCommand));

	// ---------------------------------------------------------------------------------------------
	// Trace.UI.LoadingCard.Verify
	// ---------------------------------------------------------------------------------------------

	static void RunVerify(const TArray<FString>& /*Args*/, UWorld* World)
	{
		int32 Failures = 0;
		int32 Passes = 0;
		int32 Inconclusive = 0;
		const auto Check = [&Failures, &Passes](const TCHAR* What, bool bOk, const FString& Detail)
		{
			if (bOk)
			{
				++Passes;
				UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] PASS  %s — %s"), What, *Detail);
			}
			else
			{
				++Failures;
				UE_LOG(LogTraceGame, Error, TEXT("[LoadingCard] FAIL  %s — %s"), What, *Detail);
			}
		};
		const auto Skip = [&Inconclusive](const TCHAR* What, const FString& Why)
		{
			++Inconclusive;
			UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] INCONCLUSIVE  %s — %s"), What, *Why);
		};

		const bool bPlayerOn = IsMoviePlayerEnabled() && GetMoviePlayer() != nullptr && GetMoviePlayer()->IsInitialized();
		UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] ===== Trace.UI.LoadingCard.Verify: movie player %s, %d card(s), %d load(s) this process ====="),
			bPlayerOn ? TEXT("ON") : TEXT("OFF"), GCards.Num(), GLoads.Num());
		for (const TraceLoadingScreen::FLoadRecord& Load : GLoads)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard]   load %s: %s%s"), *MapShortName(Load.Map),
				Load.bCard ? TEXT("card") : TEXT("NO CARD"), Load.WhyNot.IsEmpty() ? TEXT("") : *(TEXT(" (") + Load.WhyNot + TEXT(")")));
		}
		for (const TraceLoadingScreen::FCardRecord& Card : GCards)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadingCard]   card %s over %s: up %.2fs%s%s"), CardName(Card.Kind),
				Card.Map.IsEmpty() ? TEXT("(boot)") : *MapShortName(Card.Map),
				Card.Down > 0.0 ? Card.Down - Card.Up : -1.0,
				Card.Kind == ECard::Studio && Card.LoadingDone > 0.0 ? *FString::Printf(TEXT(", loading done +%.2fs"), Card.LoadingDone - Card.Up) : TEXT(""),
				Card.Kind == ECard::Studio && Card.FadeOutStart > 0.0 ? *FString::Printf(TEXT(", fade +%.2fs"), Card.FadeOutStart - Card.Up) : TEXT(""));
		}

		// ---- 1. THE STUDIO CARD --------------------------------------------------------------------
		const TraceLoadingScreen::FCardRecord* Studio = GCards.FindByPredicate(
			[](const TraceLoadingScreen::FCardRecord& Card) { return Card.Kind == ECard::Studio; });
		if (!bPlayerOn)
		{
			Skip(TEXT("studio card"), TEXT("the movie player is off in this process (-NoLoadingScreen, -nullrhi, editor)"));
		}
		else if (!GStudioSkipReason.IsEmpty())
		{
			Skip(TEXT("studio card"), FString::Printf(TEXT("not shown this boot: %s"), *GStudioSkipReason));
		}
		else if (Studio == nullptr || Studio->Down <= 0.0)
		{
			Check(TEXT("studio card came up and went down"), false, TEXT("no complete studio card record"));
		}
		else
		{
			const double Life = Studio->Down - Studio->Up;
			Check(TEXT("the studio card covered the boot LoadMap of the title"),
				IsTitleMap(Studio->Map), FString::Printf(TEXT("map '%s'"), *Studio->Map));
			if (!Studio->bStoppedByUs)
			{
				Skip(TEXT("studio card timing"), TEXT("it was skipped by a key or click (the engine's skip), so its hold was cut short on purpose"));
			}
			else
			{
				Check(TEXT("the studio card held at least hold + fade-out"),
					Life >= TraceLoadingScreen::StudioHoldSeconds + TraceLoadingScreen::StudioFadeOutSeconds - 0.05,
					FString::Printf(TEXT("%.2fs, minimum %.2fs"), Life,
						TraceLoadingScreen::StudioHoldSeconds + TraceLoadingScreen::StudioFadeOutSeconds));
				Check(TEXT("its fade-out began only after loading had finished and the hold was over"),
					Studio->LoadingDone > 0.0 && Studio->FadeOutStart >= Studio->LoadingDone - 0.001
						&& Studio->FadeOutStart - Studio->Up >= TraceLoadingScreen::StudioHoldSeconds - 0.001,
					FString::Printf(TEXT("loading done +%.2fs, fade +%.2fs"), Studio->LoadingDone - Studio->Up, Studio->FadeOutStart - Studio->Up));
				Check(TEXT("the movie player's own thread painted the studio card (it was really on screen)"),
					Studio->LoadingThreadPaints > 0,
					FString::Printf(TEXT("%d frame(s) on the loading thread, %d on the game thread"),
						Studio->LoadingThreadPaints, Studio->GameThreadPaints));
				Check(TEXT("it came down when its fade had run, not by the watchdog"),
					!Studio->bWatchdog && Studio->Down - Studio->FadeOutStart >= TraceLoadingScreen::StudioFadeOutSeconds - 0.02
						&& Studio->Down - Studio->FadeOutStart < TraceLoadingScreen::StudioFadeOutSeconds + 0.5,
					FString::Printf(TEXT("down %.2fs after the fade began"), Studio->Down - Studio->FadeOutStart));
			}
		}

		// ---- 2. TRAVEL CARDS -----------------------------------------------------------------------
		// Every LoadMap after the first (the first is the boot's, covered by the studio card or, on a
		// harness launch into a match, by its own travel card).
		if (!bPlayerOn)
		{
			Skip(TEXT("travel cards"), TEXT("the movie player is off in this process"));
		}
		else if (GLoads.Num() < 2)
		{
			Skip(TEXT("travel cards"), TEXT("no travel yet this process — travel once (PLAY, RETURN TO TITLE, `open`) and run this again"));
		}
		else
		{
			int32 Carded = 0;
			FString Missing;
			for (int32 Index = 1; Index < GLoads.Num(); ++Index)
			{
				if (GLoads[Index].bCard)
				{
					++Carded;
				}
				else
				{
					Missing += FString::Printf(TEXT(" %s(%s)"), *MapShortName(GLoads[Index].Map), *GLoads[Index].WhyNot);
				}
			}
			const int32 Travels = GLoads.Num() - 1;
			Check(TEXT("every travel's LoadMap showed a card instead of a frozen frame"), Carded == Travels,
				FString::Printf(TEXT("%d of %d%s"), Carded, Travels, Missing.IsEmpty() ? TEXT("") : *(TEXT(", none for:") + Missing)));

			const TraceLoadingScreen::FCardRecord* LastTravel = nullptr;
			for (const TraceLoadingScreen::FCardRecord& Card : GCards)
			{
				if (Card.Kind == ECard::Travel)
				{
					LastTravel = &Card;
				}
			}
			Check(TEXT("the last travel card came down on its own, as soon as the map had loaded"),
				LastTravel != nullptr && LastTravel->Down >= LastTravel->Up,
				LastTravel != nullptr ? FString::Printf(TEXT("%s, up %.2fs"), *MapShortName(LastTravel->Map), LastTravel->Down - LastTravel->Up)
					: FString(TEXT("no travel card record")));
			if (LastTravel != nullptr && LastTravel->Down - LastTravel->Up > 0.25)
			{
				Check(TEXT("the movie player's own thread painted that travel card while the map loaded"),
					LastTravel->LoadingThreadPaints > 0,
					FString::Printf(TEXT("%d frame(s) in %.2fs"), LastTravel->LoadingThreadPaints, LastTravel->Down - LastTravel->Up));
			}
		}

		// ---- 3. THE CAPTION RULE (pure) ---------------------------------------------------------
		Check(TEXT("a travel card shows the caption it was handed"),
			CaptionFor(TEXT("/Game/Maps/Arena_Baked"), TEXT("HOSTING ON 1.2.3.4:7777")) == TEXT("HOSTING ON 1.2.3.4:7777"),
			TEXT("Arena_Baked"));
		Check(TEXT("...but never on a load of the title (a failed JOIN going home)"),
			CaptionFor(TEXT("/Game/Maps/MainMenu"), TEXT("CONNECTING TO 1.2.3.4")).IsEmpty()
				&& CaptionFor(TEXT("MainMenu?closed"), TEXT("X")).IsEmpty(),
			FString::Printf(TEXT("title = '%s'"), *DefaultMapShortName()));

		// ---- 4. THE MENU FONT OUTLIVES A GARBAGE COLLECTION -------------------------------------
		//
		// The bug this proves fixed (TraceMenuArtStyle.cpp, FFontRef): the resolved UFont lived in a
		// static the collector could not see. On a map without the title's widgets nothing else holds
		// F_TraceMenu, so a full purge freed it and the next fallback draw read freed memory. Asked
		// through a WEAK pointer taken when the font was resolved, so the red arm reports the freed font
		// instead of reading it.
		{
			// A process that has not resolved the font yet (the loading screen resolves it at boot, so only
			// a -NoLoadingScreen run gets here) resolves it NOW, on a map where nothing else holds it. That
			// run is the discriminating one: with the loading screen on, the font was loaded during engine
			// init, and the engine roots everything loaded before it closes its disregard-for-GC window
			// (FUObjectArray::CloseDisregardForGC, straight after LoadStartupModules) — so it survives
			// either way there, and this check can only report that.
			bool bResolvedNow = false;
			if (TraceMenuArtStyle::DebugResolvedFont().IsExplicitlyNull())
			{
				TraceMenuArtStyle::MenuFont(24.f);
				bResolvedNow = true;
			}
			const TWeakObjectPtr<const UObject> Weak = TraceMenuArtStyle::DebugResolvedFont();
			const bool bRootedAtBoot = Weak.IsValid() && Weak->IsRooted();
			bool bTitleWidgetAlive = false;
			for (TObjectIterator<UTraceTitleMenuWidget> It; It; ++It)
			{
				if (!It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
				{
					bTitleWidgetAlive = true;
					break;
				}
			}
			if (Weak.IsExplicitlyNull())
			{
				Skip(TEXT("menu font survives GC"), TEXT("no font was resolved from a /Game asset in this process"));
			}
			else if (bTitleWidgetAlive)
			{
				Skip(TEXT("menu font survives GC"), TEXT("the title's widget is alive and holds the font too — run this on the arena"));
			}
			else
			{
				const bool bBefore = Weak.IsValid();
				if (bBefore)
				{
					CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPurgeObjectsOnFullPurge=*/true);
				}
				const bool bAfter = Weak.IsValid();
				Check(TEXT("the menu font survives a full garbage collection with no title on screen"), bBefore && bAfter,
					bBefore && bAfter ? FString::Printf(TEXT("%s still loaded after a full purge (%s)"), *Weak->GetPathName(),
							bRootedAtBoot ? TEXT("rooted since engine init by the boot warm-up — run with -NoLoadingScreen for the discriminating case")
								: (bResolvedNow ? TEXT("resolved just now, held only by TraceMenuArtStyle's FFontRef") : TEXT("held by TraceMenuArtStyle's FFontRef")))
						: FString::Printf(TEXT("the resolved font %s — every later fallback draw reads a freed UFont"),
							bBefore ? TEXT("was collected by this purge") : TEXT("had ALREADY been collected (by the travel's GC)")));
			}
		}

		// ---- 5. THE CARD'S ART IS READY BEFORE IT PAINTS ----------------------------------------
		if (bPlayerOn)
		{
			Check(TEXT("the glyph atlas is live (the cards set type from it)"), TraceText::IsAtlasActive(), TraceText::FaceName());
			const FCardSprite Mark = SpriteFor(ETraceKitSprite::Wordmark);
			const FCardSprite Swoosh = SpriteFor(ETraceKitSprite::Swoosh);
			const FCardSprite Crescent = SpriteFor(ETraceKitSprite::Chevron);
			Check(TEXT("the wordmark, swoosh and crescent are loaded, rooted and drawable"),
				Mark.IsSet() && Swoosh.IsSet() && Crescent.IsSet() && Mark.Texture->IsRooted() && Crescent.Texture->IsRooted(),
				FString::Printf(TEXT("mark %d, swoosh %d, crescent %d"), Mark.IsSet() ? 1 : 0, Swoosh.IsSet() ? 1 : 0, Crescent.IsSet() ? 1 : 0));
			Check(TEXT("the boot warm-up holds its assets for the process"), Warm().Held.Num() > 0,
				FString::Printf(TEXT("%d held"), Warm().Held.Num()));
		}

		// ---- 6. THE TITLE'S ACTIVATION GRACE COUNTS FROM THE CARD LIFTING ----------------------------
		ATraceMenuHUD* MenuHUD = nullptr;
		if (World != nullptr)
		{
			TActorIterator<ATraceMenuHUD> It(World);
			MenuHUD = It ? *It : nullptr;
		}
		if (MenuHUD == nullptr)
		{
			Skip(TEXT("activation grace"), TEXT("no title screen in this world — run this on the title"));
		}
		else
		{
			const ECard SavedKind = GLastEndKind;
			const double Saved = TraceLoadingScreen::DebugSetLastCardEnd(FPlatformTime::Seconds(), ECard::Studio);
			const bool bRightAfter = MenuHUD->DebugAcceptsActivation();
			TraceLoadingScreen::DebugSetLastCardEnd(FPlatformTime::Seconds() - 10.0, ECard::Studio);
			const bool bLongAfter = MenuHUD->DebugAcceptsActivation();
			TraceLoadingScreen::DebugSetLastCardEnd(Saved, SavedKind);
			Check(TEXT("a press in the first moments after a card lifts is not a menu activation"), !bRightAfter,
				FString::Printf(TEXT("accepted %d straight after the card, %d ten seconds later (grace %.2fs of real time)"),
					bRightAfter ? 1 : 0, bLongAfter ? 1 : 0, TraceMenuStyle::ActivationGraceSeconds));
			Check(TEXT("...and the title takes presses again once that grace is over"), bLongAfter, TEXT(""));

			// The title rises out of black only after the STUDIO card (which ends on black); after a travel
			// card (which already showed the wordmark where the title draws it) it comes straight in.
			if (GLastEndKind == ECard::Studio)
			{
				Check(TEXT("the title faded in from black after the studio card"),
					MenuHUD->DebugIntroFadeArmed() && MenuHUD->DebugIntroAlpha() >= 0.999f,
					FString::Printf(TEXT("armed %d, alpha now %.2f"), MenuHUD->DebugIntroFadeArmed() ? 1 : 0, MenuHUD->DebugIntroAlpha()));
			}
			else if (GLastEndKind == ECard::Travel)
			{
				Check(TEXT("the title came straight in after a travel card (no fade from black)"),
					!MenuHUD->DebugIntroFadeArmed() && MenuHUD->DebugIntroAlpha() >= 0.999f,
					FString::Printf(TEXT("armed %d, alpha now %.2f"), MenuHUD->DebugIntroFadeArmed() ? 1 : 0, MenuHUD->DebugIntroAlpha()));
			}
		}

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadingCard] ===== PASS — %d check(s), %d inconclusive: the studio card holds and fades, every travel is carded, the font survives GC ====="),
				Passes, Inconclusive);
			UE_LOG(LogTraceGame, Display, TEXT("[LoadingCard] VERDICT: PASS (%d pass, %d inconclusive)"), Passes, Inconclusive);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[LoadingCard] ===== *** FAIL *** %d check(s) — see above ====="), Failures);
			UE_LOG(LogTraceGame, Error, TEXT("[LoadingCard] VERDICT: FAIL (%d fail, %d pass, %d inconclusive)"), Failures, Passes, Inconclusive);
		}
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdLoadingCardVerify(
		TEXT("Trace.UI.LoadingCard.Verify"),
		TEXT("Checks this process's loading cards: the studio card held and faded after loading, every travel's ")
		TEXT("LoadMap got a card, the caption rule, the menu font surviving a full GC (run on the arena), and the ")
		TEXT("title's activation grace after a card (run on the title). PASS/FAIL VERDICT line."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunVerify));
}

#endif // !UE_BUILD_SHIPPING
