// Trace — see TraceMenuKit.h. The one Canvas renderer for the artist's kit.

#include "UI/Widgets/Menu/TraceMenuKit.h"

#include "CanvasItem.h"                 // FCanvasTriangleItem — DrawStroke's alpha survives
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"                   // FApp::GetCurrentTime — the UI's real clock (P10)
#include "TextureResource.h"            // FTextureResource::TextureRHI — the guard
#include "UObject/Package.h"            // GetTransientPackage — Trace.UI.Kit.Verify's unready texture

#include "Trace.h"                      // LogTraceGame
#include "UI/TraceHardwareCursor.h"
#include "UI/Text/TraceCanvasText.h"
#include "UI/Text/TraceGameText.h"      // TRACE_TEXT — the pages' TIME label (DrawPageClock)
#include "UI/Text/TraceText.h"
#include "UI/Widgets/Menu/TraceTitleMenuWidget.h"   // TraceTitleLayout — the travel card's spinner

// Named after the file, not anonymous: UBT builds this module as a unity build, and two anonymous
// namespaces merged into one translation unit is MSVC C2084 on Windows only.
namespace TraceMenuKitFile
{
	static constexpr int32 SpriteCount = static_cast<int32>(ETraceKitSprite::Count);

	/** Rooted on load, so the weak pointer is only ever empty before the first load. */
	static TWeakObjectPtr<UTexture2D> GCache[SpriteCount];

	/** Set once a path genuinely fails to load, so a broken install is not re-hunted every frame. */
	static bool GFailed[SpriteCount] = {};

#if !UE_BUILD_SHIPPING
	/** Every DrawTexture this file has handed a HUD. Trace.UI.Kit.Verify reads it around a call. */
	static int64 GTexturedQuadsIssued = 0;

	/** The tint of the last quad handed to a HUD, so the verify can see what a panel ASKED for. */
	static FLinearColor GLastQuadTint = FLinearColor::Transparent;

	/** Every DrawStroke that reached a canvas, and the colour (fade included) it went out at. */
	static int64 GStrokesIssued = 0;
	static FLinearColor GLastStrokeColor = FLinearColor::Transparent;
#endif

	/**
	 * DrawStroke's triangles. One item for the process, emptied between strokes but keeping its
	 * capacity, so a frame of strokes allocates nothing; the canvas copies the triangles while the item
	 * draws, so it is free again as soon as DrawItem returns. Game thread only, like every kit draw.
	 */
	static FCanvasTriangleItem& StrokeItem()
	{
		static FCanvasTriangleItem Shared(FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, nullptr);
		return Shared;
	}

	// ---- P10: the opacity every kit draw is multiplied by (TraceMenuKit::FScopedOpacity) ----------

	/** Opacity() — 1 outside any scope. Game thread only, like every draw that reads it. */
	static float GOpacity = 1.f;

	/**
	 * `Trace.UI.Motion 0` puts back the pre-P10 behaviour for a side-by-side: every FTraceKitFade snaps
	 * to its target and every plate swaps its hover sprite in one frame. Not a player setting.
	 */
	static int32 GMotion = 1;
	static FAutoConsoleVariableRef CVarMotion(
		TEXT("Trace.UI.Motion"),
		GMotion,
		TEXT("1 (default): kit overlays fade open/closed on real time and plates ease their hover ring. ")
		TEXT("0: every fade snaps and every hover swaps in one frame (the behaviour before P10), for a ")
		TEXT("side-by-side comparison."),
		ECVF_Default);

#if !UE_BUILD_SHIPPING
	/**
	 * `Trace.UI.FadeScale 10`: every kit fade and hover ease takes ten times as long — a slow-motion
	 * switch for photographing a fade half-way (a 150 ms fade is two or three frames of a headless
	 * capture). Dev builds only; Trace.UI.Kit.Verify pins it to 1 while it measures.
	 */
	static float GFadeScale = 1.f;
	static FAutoConsoleVariableRef CVarFadeScale(
		TEXT("Trace.UI.FadeScale"),
		GFadeScale,
		TEXT("Dev only. Multiplies every kit fade and hover ease duration (1 = shipped timing). For ")
		TEXT("capturing a fade mid-way; never leave it on."),
		ECVF_Default);
	static float FadeScale() { return FMath::Max(0.01f, GFadeScale); }
#else
	static constexpr float FadeScale() { return 1.f; }
#endif

	static void IssueTexturedQuad(AHUD* HUD, UTexture2D* Texture, const FTraceKitQuad& Quad,
		const FLinearColor& Tint)
	{
		// The screen's fade, applied to every sprite quad in the kit at the one place they are issued.
		FLinearColor Shown = Tint;
		Shown.A *= GOpacity;
		if (Shown.A <= 0.f)
		{
			return;
		}
		HUD->DrawTexture(Texture, Quad.X, Quad.Y, Quad.W, Quad.H, Quad.U, Quad.V, Quad.UW, Quad.VH,
			Shown, BLEND_Translucent);
#if !UE_BUILD_SHIPPING
		++GTexturedQuadsIssued;
		GLastQuadTint = Shown;
#endif
	}

	/** Every flat rect the kit draws, through the screen's fade. Nothing at alpha 0. */
	static void FadedRect(AHUD* HUD, const FLinearColor& Color, float X, float Y, float W, float H)
	{
		const FLinearColor Shown = TraceMenuKit::Faded(Color);
		if (Shown.A > 0.f && W > 0.f && H > 0.f)
		{
			HUD->DrawRect(Shown, X, Y, W, H);
		}
	}

	/** A rect outline, @p Thick px, inside (X, Y, W, H). */
	static void StrokeRect(AHUD* HUD, float X, float Y, float W, float H, float Thick, const FLinearColor& Color)
	{
		const float T = FMath::Min(Thick, FMath::Min(W, H) * 0.5f);
		FadedRect(HUD, Color, X, Y, W, T);
		FadedRect(HUD, Color, X, Y + H - T, W, T);
		FadedRect(HUD, Color, X, Y + T, T, H - T * 2.f);
		FadedRect(HUD, Color, X + W - T, Y + T, T, H - T * 2.f);
	}

	// ---- P10: hover transitions, one blend per plate rect ---------------------------------------

	struct FHoverEntry
	{
		float Blend = 0.f;
		double LastTime = 0.0;
		uint64 LastFrame = 0;
	};

	static TMap<uint64, FHoverEntry> GHover;
	static uint64 GHoverSweptAt = 0;

	/** The current TraceMenuKit::FScopedHoverSalt's salt; 0 outside any scope. Game thread only. */
	static uint32 GHoverSalt = 0;

	/**
	 * A plate's identity for the blend: its rect, rounded to whole pixels, 16 bits a side — and, inside
	 * an FScopedHoverSalt, the salt mixed over it, so two surfaces on one rect are two plates. Salt 0
	 * leaves the key exactly as it was, so a screen that never asks for a salt is unchanged.
	 */
	static uint64 HoverKey(float X, float Y, float W, float H)
	{
		const uint64 KX = static_cast<uint64>(static_cast<uint16>(FMath::RoundToInt(X)));
		const uint64 KY = static_cast<uint64>(static_cast<uint16>(FMath::RoundToInt(Y)));
		const uint64 KW = static_cast<uint64>(static_cast<uint16>(FMath::RoundToInt(W)));
		const uint64 KH = static_cast<uint64>(static_cast<uint16>(FMath::RoundToInt(H)));
		const uint64 RectKey = KX | (KY << 16) | (KW << 32) | (KH << 48);
		return (GHoverSalt == 0) ? RectKey
			: (RectKey ^ (static_cast<uint64>(GHoverSalt) * 0x9E3779B97F4A7C15ull));
	}

	/** One rate-limited step toward the target: InSeconds from 0 to 1, OutSeconds back. Real seconds. */
	static float StepToward(float Value, bool bTarget, double Elapsed, float InSeconds, float OutSeconds)
	{
		if (Elapsed <= 0.0)
		{
			return Value;
		}
		return bTarget
			? FMath::Min(1.f, Value + static_cast<float>(Elapsed / FMath::Max(1e-3f, InSeconds)))
			: FMath::Max(0.f, Value - static_cast<float>(Elapsed / FMath::Max(1e-3f, OutSeconds)));
	}

	/**
	 * THE blend, at an explicit frame and time (the harness drives it directly). A plate that was not
	 * drawn on the previous frame starts AT its target: a page that has just opened must not animate a
	 * highlight in under its own fade, and a plate whose rect moved is a new plate.
	 */
	static float HoverBlendAt(uint64 Key, bool bTarget, uint64 Frame, double NowSeconds)
	{
		if (GMotion == 0)
		{
			return bTarget ? 1.f : 0.f;
		}

		// Forget plates nobody has drawn for a while, so the table stays the size of one screen.
		if (GHover.Num() > 256 || Frame > GHoverSweptAt + 600)
		{
			GHoverSweptAt = Frame;
			for (auto It = GHover.CreateIterator(); It; ++It)
			{
				if (It.Value().LastFrame + 2 < Frame)
				{
					It.RemoveCurrent();
				}
			}
		}

		FHoverEntry* Entry = GHover.Find(Key);
		if (Entry == nullptr || Entry->LastFrame + 1 < Frame)
		{
			FHoverEntry& Fresh = GHover.FindOrAdd(Key);
			Fresh.Blend = bTarget ? 1.f : 0.f;
			Fresh.LastTime = NowSeconds;
			Fresh.LastFrame = Frame;
			return Fresh.Blend;
		}

		// Same frame, same time: no time passes, so a second ask this frame reads the same value.
		Entry->Blend = StepToward(Entry->Blend, bTarget, NowSeconds - Entry->LastTime,
			TraceMenuKit::HoverInSeconds * FadeScale(), TraceMenuKit::HoverOutSeconds * FadeScale());
		Entry->LastTime = NowSeconds;
		Entry->LastFrame = Frame;
		return Entry->Blend;
	}

	static FLinearColor Scaled(const FLinearColor& InColor, float InTint)
	{
		return FLinearColor(InColor.R * InTint, InColor.G * InTint, InColor.B * InTint, InColor.A);
	}
}

// =================================================================================================
// REAL TIME, OPACITY AND THE FADE (P10)
// =================================================================================================

double TraceMenuKit::RealSeconds()
{
	return FApp::GetCurrentTime();
}

float TraceMenuKit::Opacity()
{
	return TraceMenuKitFile::GOpacity;
}

TraceMenuKit::FScopedOpacity::FScopedOpacity(float Alpha)
	: Saved(TraceMenuKitFile::GOpacity)
{
	TraceMenuKitFile::GOpacity = Saved * FMath::Clamp(Alpha, 0.f, 1.f);
}

TraceMenuKit::FScopedOpacity::~FScopedOpacity()
{
	TraceMenuKitFile::GOpacity = Saved;
}

FLinearColor TraceMenuKit::Faded(const FLinearColor& Color)
{
	return FLinearColor(Color.R, Color.G, Color.B, Color.A * TraceMenuKitFile::GOpacity);
}

float FTraceKitFade::Update(bool bShown, float InSeconds, float OutSeconds)
{
	return UpdateAt(bShown, TraceMenuKit::RealSeconds(), InSeconds, OutSeconds);
}

float FTraceKitFade::UpdateAt(bool bShown, double NowSeconds, float InSeconds, float OutSeconds)
{
	const float In = ((InSeconds > 0.f) ? InSeconds : TraceMenuKit::FadeInSeconds) * TraceMenuKitFile::FadeScale();
	const float Out = ((OutSeconds > 0.f) ? OutSeconds : TraceMenuKit::FadeOutSeconds) * TraceMenuKitFile::FadeScale();

	if (TraceMenuKitFile::GMotion == 0)
	{
		Progress = bShown ? 1.f : 0.f;
	}
	else if (LastTime >= 0.0)
	{
		// ELAPSED REAL SECONDS, not a per-frame step: the same 150 ms at 30 fps and at 240, and a long
		// hitch simply finishes the fade. A clock that went backwards (a new world, a test rewinding)
		// counts as no time.
		Progress = TraceMenuKitFile::StepToward(Progress, bShown, FMath::Max(0.0, NowSeconds - LastTime), In, Out);
	}

	LastTime = NowSeconds;
	bTarget = bShown;
	return Alpha();
}

void FTraceKitFade::Snap(bool bShown)
{
	Progress = bShown ? 1.f : 0.f;
	bTarget = bShown;
}

float FTraceKitFade::Alpha() const
{
	// Smoothstep: eases both ends, and because it is a function of Progress a reversal mid-fade is
	// continuous in alpha as well as in progress.
	const float P = FMath::Clamp(Progress, 0.f, 1.f);
	return P * P * (3.f - 2.f * P);
}

// =================================================================================================
// THE GUARD AND THE SPRITES
// =================================================================================================

bool TraceMenuKit::IsDrawable(const UTexture* Texture)
{
	if (Texture == nullptr)
	{
		return false;
	}

	// GetResource() alone is NOT enough: the resource object exists straight after the load, and it
	// is FTextureResource::TextureRHI that arrives a frame or two later from the render thread. That
	// was measured the hard way — the non-null check did not stop the crash, this one did.
	const FTextureResource* Resource = Texture->GetResource();
	return Resource != nullptr && Resource->TextureRHI.IsValid();
}

const TCHAR* TraceMenuKit::SpritePath(ETraceKitSprite Which)
{
	switch (Which)
	{
	case ETraceKitSprite::BtnDefault:   return TraceMenuArtStyle::BtnDefault;
	case ETraceKitSprite::BtnHover:     return TraceMenuArtStyle::BtnHover;
	case ETraceKitSprite::BtnDisabled:  return TraceMenuArtStyle::BtnDisabled;
	case ETraceKitSprite::ValueBox:     return TraceMenuArtStyle::ValueBox;
	case ETraceKitSprite::SliderTrack:  return TraceMenuArtStyle::SliderTrack;
	case ETraceKitSprite::SliderHandle: return TraceMenuArtStyle::SliderHandle;
	case ETraceKitSprite::Chevron:      return TraceMenuArtStyle::Chevron;
	case ETraceKitSprite::Wordmark:     return TraceMenuArtStyle::Wordmark;
	case ETraceKitSprite::Swoosh:       return TraceMenuArtStyle::Swoosh;
	default:                            return nullptr;
	}
}

UTexture2D* TraceMenuKit::PeekSprite(ETraceKitSprite Which)
{
	const int32 Slot = static_cast<int32>(Which);
	if (Slot < 0 || Slot >= TraceMenuKitFile::SpriteCount)
	{
		return nullptr;
	}
	return TraceMenuKitFile::GCache[Slot].Get();
}

UTexture2D* TraceMenuKit::Sprite(ETraceKitSprite Which)
{
	const int32 Slot = static_cast<int32>(Which);
	if (Slot < 0 || Slot >= TraceMenuKitFile::SpriteCount)
	{
		return nullptr;
	}

	UTexture2D* Loaded = TraceMenuKitFile::GCache[Slot].Get();
	if (Loaded == nullptr)
	{
		if (TraceMenuKitFile::GFailed[Slot])
		{
			return nullptr;
		}

		Loaded = LoadObject<UTexture2D>(nullptr, SpritePath(Which));
		if (Loaded == nullptr)
		{
			// Once. A warning per frame per sprite is its own defect.
			TraceMenuKitFile::GFailed[Slot] = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[MenuKit] '%s' did not load. Every screen drawing it keeps its fallback; the "
				     "screen stays readable."), SpritePath(Which));
			return nullptr;
		}

		// Rooted: on the arena map nothing else references the menu art, and a GC mid-match would
		// blank a screen (or, before the weak cache, draw through a freed texture).
		Loaded->AddToRoot();
		TraceMenuKitFile::GCache[Slot] = Loaded;
	}

	if (!IsDrawable(Loaded))
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[MenuKit] '%s' is loaded but has no RHI texture yet; its fallback draws this frame."),
			*Loaded->GetName());
		return nullptr;
	}
	return Loaded;
}

int32 TraceMenuKit::Prime()
{
	int32 Loaded = 0;
	for (int32 Slot = 0; Slot < TraceMenuKitFile::SpriteCount; ++Slot)
	{
		const ETraceKitSprite Which = static_cast<ETraceKitSprite>(Slot);
		// Sprite() loads and roots on first use; whether it is DRAWABLE yet does not matter here —
		// the point is that the render thread gets its frames to create the resource before a screen
		// needs it.
		Sprite(Which);
		Loaded += (PeekSprite(Which) != nullptr) ? 1 : 0;
	}

	// P11: the pointer is kit art too, drawn by every overlay (DrawCursor / ShowCursor), and it was the
	// one piece still loaded by its first draw — on the frame team select opens.
	TraceHardwareCursor::Prime();
	return Loaded;
}

// =================================================================================================
// STATES
// =================================================================================================

ETraceKitState TraceMenuKit::StateFor(bool bEnabled, bool bSelected, bool bPressed)
{
	if (!bEnabled) { return ETraceKitState::Disabled; }
	if (bPressed)  { return ETraceKitState::Pressed; }
	if (bSelected) { return ETraceKitState::Hover; }
	return ETraceKitState::Default;
}

FTraceKitVisuals TraceMenuKit::VisualsFor(ETraceKitState State)
{
	FTraceKitVisuals Out;
	switch (State)
	{
	case ETraceKitState::Disabled:
		Out.Plate = ETraceKitSprite::BtnDisabled;
		Out.Label = TraceMenuArtStyle::WordDisabled;
		Out.Furniture = TraceMenuArtStyle::WordDisabled;
		break;

	case ETraceKitState::Pressed:
		// The sheet has three plates and a press is not one of them, so pressed is the HOVER plate —
		// ring and all — knocked down. A fourth sprite would replace this case and nothing else.
		Out.Plate = ETraceKitSprite::BtnHover;
		Out.PlateTint = TraceMenuArtStyle::PressedTint;
		Out.Label = TraceMenuArtStyle::WordHoverLifted();
		Out.Furniture = FurnitureSelected;
		break;

	case ETraceKitState::Hover:
		// The ring comes from the plate, the green from the palette, on adjacent lines: neither can
		// be on screen without the other (spec v24 §3).
		Out.Plate = ETraceKitSprite::BtnHover;
		Out.bPulses = true;
		Out.Label = TraceMenuArtStyle::WordHoverLifted();
		Out.Furniture = FurnitureSelected;
		break;

	case ETraceKitState::Default:
	default:
		Out.Plate = ETraceKitSprite::BtnDefault;
		Out.Label = TraceMenuArtStyle::WordDefault;
		Out.Furniture = FurnitureUnselected;
		break;
	}
	return Out;
}

float TraceMenuKit::HoverPulse(float NowSeconds, float Speed)
{
	return HoverPulseBase + HoverPulseSwing * FMath::Sin(NowSeconds * Speed);
}

float TraceMenuKit::PlateTintAt(const FTraceKitVisuals& Visuals, float NowSeconds)
{
	return Visuals.bPulses ? HoverPulse(NowSeconds) : Visuals.PlateTint;
}

float TraceMenuKit::HoverBlend(float X, float Y, float W, float H, bool bHovered)
{
	return TraceMenuKitFile::HoverBlendAt(TraceMenuKitFile::HoverKey(X, Y, W, H), bHovered, GFrameCounter,
		RealSeconds());
}

TraceMenuKit::FScopedHoverSalt::FScopedHoverSalt(uint32 Salt)
	: Saved(TraceMenuKitFile::GHoverSalt)
{
	TraceMenuKitFile::GHoverSalt = Salt;
}

TraceMenuKit::FScopedHoverSalt::~FScopedHoverSalt()
{
	TraceMenuKitFile::GHoverSalt = Saved;
}

FTraceKitVisuals TraceMenuKit::VisualsForBlend(ETraceKitState State, float Blend)
{
	if (State == ETraceKitState::Disabled || State == ETraceKitState::Pressed)
	{
		return VisualsFor(State);
	}

	const float B = FMath::Clamp(Blend, 0.f, 1.f);
	const FTraceKitVisuals Off = VisualsFor(ETraceKitState::Default);
	const FTraceKitVisuals On = VisualsFor(ETraceKitState::Hover);
	FTraceKitVisuals Out = (B >= 0.5f) ? On : Off;
	Out.Label = FMath::Lerp(Off.Label, On.Label, B);
	Out.Furniture = FMath::Lerp(Off.Furniture, On.Furniture, B);
	return Out;
}

FTraceKitVisuals TraceMenuKit::VisualsAt(ETraceKitState State, float X, float Y, float W, float H)
{
	const bool bLit = (State == ETraceKitState::Hover || State == ETraceKitState::Pressed);
	return VisualsForBlend(State, HoverBlend(X, Y, W, H, bLit));
}

// =================================================================================================
// PLATES
// =================================================================================================

int32 TraceMenuKit::PlateQuads(const TraceMenuArtStyle::FSpriteFrame& Frame,
	float X, float Y, float W, float H, float CornerHeight, FTraceKitQuad (&Out)[9])
{
	// The same arithmetic character select shipped (it was the one full 9-slice of the three), so its
	// cards and chips do not move by a pixel. With CornerHeight == H it is also exactly the options
	// page's and the Canvas title's 3-slice: the vertical slices then map at the sprite's own scale,
	// which is what a single stretched column did. Trace.UI.Kit.Verify checks both claims.
	if (W <= 1.f || H <= 1.f || Frame.PlateH <= 0.f || Frame.SpriteW() <= 0.f || Frame.SpriteH() <= 0.f)
	{
		return 0;
	}

	const float CornerScale = FMath::Max(CornerHeight, 1.f) / Frame.PlateH;
	const float Inset = Frame.Glow * CornerScale;

	const float SX = X - Inset;
	const float SY = Y - Inset;
	const float SW = W + Inset * 2.f;
	const float SH = H + Inset * 2.f;

	// Half the sprite, minus a pixel, is the hard ceiling: two corners that met in the middle would
	// draw the flat centre at a negative width and flip the quad.
	const float CapPx = FMath::Clamp(Frame.Cap * CornerScale, 1.f, FMath::Min(SW, SH) * 0.5f - 1.f);

	const float UCap = Frame.Cap / Frame.SpriteW();
	const float VCap = Frame.Cap / Frame.SpriteH();

	const float Xs[3] = { SX, SX + CapPx, SX + SW - CapPx };
	const float Ws[3] = { CapPx, SW - CapPx * 2.f, CapPx };
	const float Us[3] = { 0.f, UCap, 1.f - UCap };
	const float UWs[3] = { UCap, 1.f - UCap * 2.f, UCap };

	const float Ys[3] = { SY, SY + CapPx, SY + SH - CapPx };
	const float Hs[3] = { CapPx, SH - CapPx * 2.f, CapPx };
	const float Vs[3] = { 0.f, VCap, 1.f - VCap };
	const float VHs[3] = { VCap, 1.f - VCap * 2.f, VCap };

	int32 Filled = 0;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Column = 0; Column < 3; ++Column)
		{
			if (Ws[Column] <= 0.f || Hs[Row] <= 0.f)
			{
				continue;
			}
			FTraceKitQuad& Quad = Out[Filled++];
			Quad.X = Xs[Column];
			Quad.Y = Ys[Row];
			Quad.W = Ws[Column];
			Quad.H = Hs[Row];
			Quad.U = Us[Column];
			Quad.V = Vs[Row];
			Quad.UW = UWs[Column];
			Quad.VH = VHs[Row];
		}
	}
	return Filled;
}

bool TraceMenuKit::DrawPlate(AHUD* HUD, UTexture2D* Texture, const TraceMenuArtStyle::FSpriteFrame& Frame,
	float X, float Y, float W, float H, float CornerHeight, const FLinearColor& Tint)
{
	// THE GUARD, FIRST AND UNCONDITIONALLY — before the HUD check, so no caller can get a texture past
	// it by any route. Character select's own copy of this function had no guard at all; a cold load
	// of the art inside a match was one frame away from the render-thread crash.
	if (!IsDrawable(Texture))
	{
		return false;
	}
	if (HUD == nullptr)
	{
		return false;
	}

	FTraceKitQuad Quads[9];
	const int32 NumQuads = PlateQuads(Frame, X, Y, W, H, CornerHeight, Quads);
	if (NumQuads <= 0)
	{
		return false;
	}

	for (int32 Each = 0; Each < NumQuads; ++Each)
	{
		TraceMenuKitFile::IssueTexturedQuad(HUD, Texture, Quads[Each], Tint);
	}
	return true;
}

void TraceMenuKit::DrawFallbackPlate(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H,
	float Tint)
{
	if (HUD == nullptr || W <= 0.f || H <= 0.f)
	{
		return;
	}

	const bool bDisabled = (State == ETraceKitState::Disabled);
	const float Thick = FMath::Max(1.f, FMath::RoundToFloat(H * 0.03f));

	TraceMenuKitFile::FadedRect(HUD,
		TraceMenuKitFile::Scaled(bDisabled ? TraceMenuArtStyle::DisabledFill : TraceMenuArtStyle::PlateFill, Tint),
		X, Y, W, H);

	if (bDisabled)
	{
		TraceMenuKitFile::StrokeRect(HUD, X, Y, W, H, Thick, TraceMenuArtStyle::DisabledRing);
	}
	else if (State == ETraceKitState::Hover || State == ETraceKitState::Pressed)
	{
		TraceMenuKitFile::StrokeRect(HUD, X, Y, W, H, Thick, TraceMenuKitFile::Scaled(TraceMenuArtStyle::AmberLifted(), Tint));
	}
}

void TraceMenuKit::DrawStatePlate(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H,
	float NowSeconds, float CornerHeight)
{
	if (HUD == nullptr)
	{
		return;
	}

	const float Corner = (CornerHeight > 0.f) ? CornerHeight : H;

	// P10 — THE HOVER RING EASES ON AND OFF instead of swapping in one frame. Disabled and Pressed stay
	// immediate (a press must feel instant), but still move the blend, so a plate released from a
	// press or re-enabled continues from where it is rather than jumping.
	const bool bLit = (State == ETraceKitState::Hover || State == ETraceKitState::Pressed);
	const float Blend = HoverBlend(X, Y, W, H, bLit);
	ETraceKitState Drawn = State;
	if (State == ETraceKitState::Default || State == ETraceKitState::Hover)
	{
		Drawn = (Blend >= 1.f) ? ETraceKitState::Hover : ETraceKitState::Default;

		if (Blend > 0.f && Blend < 1.f)
		{
			// Mid-transition: the default plate, and the hover plate (ring, glow and breath) laid over it
			// at the blend. Both sprites share the navy body, so the body stays solid and only the ring
			// comes and goes.
			UTexture2D* Base = Sprite(ETraceKitSprite::BtnDefault);
			UTexture2D* Lit = Sprite(ETraceKitSprite::BtnHover);
			if (Base != nullptr && Lit != nullptr)
			{
				const float Breath = HoverPulse(NowSeconds);
				DrawPlate(HUD, Base, TraceMenuArtStyle::ButtonFrame, X, Y, W, H, Corner, FLinearColor::White);
				DrawPlate(HUD, Lit, TraceMenuArtStyle::ButtonFrame, X, Y, W, H, Corner,
					FLinearColor(Breath, Breath, Breath, Blend));
				return;
			}
			Drawn = (Blend >= 0.5f) ? ETraceKitState::Hover : ETraceKitState::Default;
		}
	}

	const FTraceKitVisuals Visuals = VisualsFor(Drawn);
	const float PlateTint = PlateTintAt(Visuals, NowSeconds);

	if (!DrawPlate(HUD, Sprite(Visuals.Plate), TraceMenuArtStyle::ButtonFrame, X, Y, W, H, Corner,
		FLinearColor(PlateTint, PlateTint, PlateTint, 1.f)))
	{
		DrawFallbackPlate(HUD, Drawn, X, Y, W, H, PlateTint);
	}
}

bool TraceMenuKit::DrawPanelPlate(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H,
	float CornerHeight, float Alpha)
{
	const float A = FMath::Clamp(Alpha, 0.f, 1.f);
	if (HUD == nullptr || W <= 0.f || H <= 0.f || A <= 0.f)
	{
		return false;
	}

	// The state's plate at its FLAT tint: Pressed keeps its knock-down, Hover does not breathe.
	const FTraceKitVisuals Visuals = VisualsFor(State);
	const float PlateTint = Visuals.PlateTint;
	const float Corner = (CornerHeight > 0.f) ? CornerHeight : H;

	if (DrawPlate(HUD, Sprite(Visuals.Plate), TraceMenuArtStyle::ButtonFrame, X, Y, W, H, Corner,
		FLinearColor(PlateTint, PlateTint, PlateTint, A)))
	{
		return true;
	}

	// DrawFallbackPlate's rect, with the alpha carried through (that function scales RGB only).
	const bool bDisabled = (State == ETraceKitState::Disabled);
	const float Thick = FMath::Max(1.f, FMath::RoundToFloat(FMath::Min(H, Corner) * 0.03f));
	FLinearColor Fill = TraceMenuKitFile::Scaled(bDisabled ? TraceMenuArtStyle::DisabledFill : TraceMenuArtStyle::PlateFill,
		PlateTint);
	Fill.A = A;
	TraceMenuKitFile::FadedRect(HUD, Fill, X, Y, W, H);

	if (bDisabled || State == ETraceKitState::Hover || State == ETraceKitState::Pressed)
	{
		FLinearColor Edge = bDisabled ? TraceMenuArtStyle::DisabledRing
			: TraceMenuKitFile::Scaled(TraceMenuArtStyle::AmberLifted(), PlateTint);
		Edge.A = A;
		TraceMenuKitFile::StrokeRect(HUD, X, Y, W, H, Thick, Edge);
	}
	return false;
}

// =================================================================================================
// TEXT
// =================================================================================================

float TraceMenuKit::LabelSize(float PlateH, ETraceTextWeight Weight)
{
	return TraceText::SizeForCapHeight(FMath::Max(0.f, PlateH) * LabelCapFraction, Weight);
}

float TraceMenuKit::DrawLabel(AHUD* HUD, const FString& Text, float CenterX, float CenterY, float PlateH,
	const FLinearColor& Color, float MaxWidth, ETraceTextWeight Weight, bool bTabularDigits)
{
	if (HUD == nullptr || Text.IsEmpty() || PlateH <= 0.f)
	{
		return 0.f;
	}

	TraceText::FStyle Style(LabelSize(PlateH, Weight), Color, Weight);
	Style.bTabularDigits = bTabularDigits;
	if (MaxWidth > 0.f)
	{
		const float Natural = TraceText::MeasureWidth(Text, Style);
		if (Natural > MaxWidth && Natural > 0.f)
		{
			Style.Size *= MaxWidth / Natural;
		}
	}

	// Centred by the CAPS, not by the line box: the kit's words are capitals and the line box carries
	// a descender's worth of air under them, which would sit every label visibly high.
	Style.HAlign = TraceText::EHAlign::Center;
	Style.VAlign = TraceText::EVAlign::CapTop;
	const float Caps = TraceText::CapHeight(Style.Size, Style.Weight);
	return TraceCanvasText::Draw(HUD, Text, CenterX, CenterY - Caps * 0.5f, Style);
}

// =================================================================================================
// CONTROLS
// =================================================================================================

bool TraceMenuKit::DrawButton(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H,
	const FString& Label, float NowSeconds, float CornerHeight)
{
	if (HUD == nullptr || Label.IsEmpty() || W <= 0.f || H <= 0.f)
	{
		return false;
	}

	DrawStatePlate(HUD, State, X, Y, W, H, NowSeconds, CornerHeight);

	// A tall card keeps a button-sized corner, and a button-sized word with it.
	const float WordPlateH = (CornerHeight > 0.f) ? FMath::Min(CornerHeight, H) : H;
	// The word follows the plate's blend (VisualsAt reads the value DrawStatePlate just advanced).
	DrawLabel(HUD, Label, X + W * 0.5f, Y + H * 0.5f, WordPlateH, VisualsAt(State, X, Y, W, H).Label,
		W - WordPlateH * LabelPadFraction * 2.f);
	return true;
}

void TraceMenuKit::DrawValueBoxPlate(AHUD* HUD, float X, float Y, float W, float H, const FLinearColor& Tint)
{
	if (HUD == nullptr || W <= 0.f || H <= 0.f)
	{
		return;
	}

	if (!DrawPlate(HUD, Sprite(ETraceKitSprite::ValueBox), TraceMenuArtStyle::ValueFrame, X, Y, W, H, H, Tint))
	{
		TraceMenuKitFile::FadedRect(HUD, TraceMenuArtStyle::PlateFill, X, Y, W, H);
		TraceMenuKitFile::StrokeRect(HUD, X, Y, W, H, FMath::Max(1.f, FMath::RoundToFloat(H * 0.04f)),
			TraceMenuArtStyle::ValueGlowLifted());
	}
}

bool TraceMenuKit::DrawValueBox(AHUD* HUD, float X, float Y, float W, float H, const FString& Text,
	const FLinearColor& TextColor)
{
	if (HUD == nullptr || Text.IsEmpty() || W <= 0.f || H <= 0.f)
	{
		return false;
	}

	DrawValueBoxPlate(HUD, X, Y, W, H);
	DrawLabel(HUD, Text, X + W * 0.5f, Y + H * 0.5f, H, TextColor, W - H * LabelPadFraction * 2.f,
		ETraceTextWeight::Light, /*bTabularDigits=*/true);
	return true;
}

void TraceMenuKit::DrawStroke(AHUD* HUD, float X0, float Y0, float X1, float Y1, const FLinearColor& InColor,
	float Thickness)
{
	// AHUD::Canvas is protected: the canvas the engine is drawing the HUD into (null outside a draw).
	UCanvas* const TargetCanvas = (HUD != nullptr) ? TraceCanvasText::GameCanvas() : nullptr;
	const FLinearColor StrokeColor = Faded(InColor);
	const FVector2D From(X0, Y0);
	const FVector2D To(X1, Y1);
	FVector2D Along = To - From;
	const double StrokeLength = Along.Size();
	if (TargetCanvas == nullptr || GWhiteTexture == nullptr || !IsInGameThread() || StrokeLength < 1.e-3
		|| StrokeColor.A <= 0.f)
	{
		return;
	}
	Along /= StrokeLength;

	// A quad of two translucent triangles on the white texture: its vertex colour keeps its alpha all
	// the way to the blend, which an FCanvasLineItem's does not (the engine sets A to 1 in AddLine).
	const FVector2D Side = FVector2D(-Along.Y, Along.X) * (0.5 * FMath::Max(Thickness, 1.f));
	const FVector2D Corners[4] = { From + Side, To + Side, To - Side, From - Side };
	const int32 Order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };

	FCanvasTriangleItem& Item = TraceMenuKitFile::StrokeItem();
	Item.TriangleList.Reset();
	for (const int32 (&Tri)[3] : Order)
	{
		FCanvasUVTri& Out = Item.TriangleList.AddDefaulted_GetRef();
		Out.V0_Pos = Corners[Tri[0]];
		Out.V1_Pos = Corners[Tri[1]];
		Out.V2_Pos = Corners[Tri[2]];
		Out.V0_UV = Out.V1_UV = Out.V2_UV = FVector2D::ZeroVector;
		Out.V0_Color = Out.V1_Color = Out.V2_Color = StrokeColor;
	}
	Item.Texture = GWhiteTexture;
	Item.BlendMode = SE_BLEND_Translucent;
	TargetCanvas->DrawItem(Item);
	Item.TriangleList.Reset();
#if !UE_BUILD_SHIPPING
	++TraceMenuKitFile::GStrokesIssued;
	TraceMenuKitFile::GLastStrokeColor = StrokeColor;
#endif
}

#if !UE_BUILD_SHIPPING
int64 TraceMenuKit::DebugStrokesIssued()
{
	return TraceMenuKitFile::GStrokesIssued;
}

FLinearColor TraceMenuKit::DebugLastStrokeColor()
{
	return TraceMenuKitFile::GLastStrokeColor;
}
#endif

float TraceMenuKit::DrawPageClock(AHUD* HUD, float ViewW, float UIScale, float SecondsLeft, float NowSeconds)
{
	const float ClockRight = ViewW - PageMarginPx * UIScale;
	if (HUD == nullptr || SecondsLeft < 0.f)
	{
		return ClockRight;
	}

	const float BoxH = PageClockBoxPx * UIScale;
	const float BoxW = BoxH * (TraceMenuArtStyle::ValueFrame.PlateW / TraceMenuArtStyle::ValueFrame.PlateH);
	const float BoxX = ClockRight - BoxW;
	const float ClockCapMid = PageTitleCapMidPx * UIScale;

	// The last five seconds pulse in the kit's amber, as the team screen's countdown does.
	FLinearColor Ink = TraceMenuArtStyle::WordDefault;
	if (SecondsLeft <= 5.f)
	{
		const FLinearColor Amber = TraceMenuArtStyle::AmberLifted();
		Ink = FLinearColor(Amber.R, Amber.G, Amber.B, 0.72f + 0.28f * FMath::Sin(NowSeconds * 9.f));
	}
	DrawValueBox(HUD, BoxX, ClockCapMid - BoxH * 0.5f, BoxW, BoxH,
		FString::FromInt(FMath::Max(0, FMath::CeilToInt(SecondsLeft))), Ink);

	const FString& ClockLabel = TRACE_TEXT("LOADOUT.TIMER_LABEL", "TIME");
	if (ClockLabel.IsEmpty())
	{
		return BoxX;
	}
	TraceText::FStyle LabelStyle(PageClockLabelPx * UIScale, FurnitureUnselected, ETraceTextWeight::Light);
	LabelStyle.HAlign = TraceText::EHAlign::Right;
	const float LabelRight = BoxX - 14.f * UIScale;
	return LabelRight - DrawTextCapCentered(HUD, ClockLabel, LabelRight, ClockCapMid, LabelStyle);
}

void TraceMenuKit::DrawSliderTrack(AHUD* HUD, float X, float Y, float W, float H, const FLinearColor& Tint)
{
	if (HUD == nullptr || W <= 0.f || H <= 0.f)
	{
		return;
	}

	UTexture2D* Track = Sprite(ETraceKitSprite::SliderTrack);
	if (Track == nullptr)
	{
		TraceMenuKitFile::FadedRect(HUD, TraceMenuArtStyle::PlateFill, X, Y + H * TrackRailTopV, W, FMath::Max(2.f, H * TrackRailV));
		return;
	}

	// The middle is sampled from a band of the sprite's uniform centre rather than from the span
	// between the caps: an earlier cut of this sprite kept the artist's handle blade at columns
	// 37..78 of 512, and a plain stretch painted it as a second, immovable handle on every slider.
	// The caps (16 px each end, the artist's soft lip) are clean and drawn as cut. Lifted verbatim
	// from TraceOptionsMenuArt::DrawTrough, so the options sliders do not move.
	constexpr float LipU = 16.f / 512.f;
	constexpr float MidU = 0.30f;
	constexpr float MidUW = 0.40f;

	const float Lip = FMath::Min(H * (16.f / 23.f), W * 0.5f);
	const float MidW = W - Lip * 2.f;

	FTraceKitQuad Quad;
	Quad.Y = Y;
	Quad.H = H;
	Quad.V = 0.f;
	Quad.VH = 1.f;

	Quad.X = X;  Quad.W = Lip;  Quad.U = 0.f;  Quad.UW = LipU;
	TraceMenuKitFile::IssueTexturedQuad(HUD, Track, Quad, Tint);

	if (MidW > 0.f)
	{
		Quad.X = X + Lip;  Quad.W = MidW;  Quad.U = MidU;  Quad.UW = MidUW;
		TraceMenuKitFile::IssueTexturedQuad(HUD, Track, Quad, Tint);
	}

	Quad.X = X + W - Lip;  Quad.W = Lip;  Quad.U = 1.f - LipU;  Quad.UW = LipU;
	TraceMenuKitFile::IssueTexturedQuad(HUD, Track, Quad, Tint);
}

// Named, not anonymous: the unity build (Scripts/check-jumbo-build-collisions.py).
namespace TraceMenuKitRail
{
	// T_MenuSliderTrack in its own pixels (512 x 23). The rail — the navy and the gold lip either side of
	// it — is rows 6..16 and columns 5..506; everything outside that is the halo. The ends are cut 11
	// columns deep, the depth of the artist's soft end, and the stretched middle samples the same clean
	// band DrawSliderTrack does.
	static constexpr float SheetW = 512.f;
	static constexpr float SheetH = 23.f;
	static constexpr float RailRowTop = 6.f;
	static constexpr float RailRowBottom = 17.f;
	static constexpr float RailColLeft = 5.f;
	static constexpr float RailColRight = 507.f;
	static constexpr float EndCols = 11.f;
	static constexpr float MiddleU = 0.30f;
	static constexpr float MiddleUW = 0.40f;
}

float TraceMenuKit::RailLipInset(float H)
{
	namespace KR = TraceMenuKitRail;
	return FMath::Max(1.f, FMath::RoundToFloat(FMath::Max(0.f, H) / (KR::RailRowBottom - KR::RailRowTop)));
}

void TraceMenuKit::DrawRail(AHUD* HUD, float X, float Y, float W, float H, float MaxHalo, const FLinearColor& Tint)
{
	namespace KR = TraceMenuKitRail;
	if (HUD == nullptr || W <= 0.f || H <= 0.f)
	{
		return;
	}

	UTexture2D* const RailSprite = Sprite(ETraceKitSprite::SliderTrack);
	if (RailSprite == nullptr)
	{
		const float LipPx = RailLipInset(H);
		TraceMenuKitFile::FadedRect(HUD, TraceMenuArtStyle::PlateFill, X, Y, W, H);
		TraceMenuKitFile::FadedRect(HUD, TraceMenuArtStyle::ValueGlowLifted(), X, Y, W, LipPx);
		TraceMenuKitFile::FadedRect(HUD, TraceMenuArtStyle::ValueGlowLifted(), X, Y + H - LipPx, W, LipPx);
		return;
	}

	// The sprite's scale is set by the RAIL's height; the halo keeps that scale until it would be taller
	// than MaxHalo, and is squashed (both ways alike) from there.
	const float PxPerRow = H / (KR::RailRowBottom - KR::RailRowTop);
	const float NaturalHalo = KR::RailRowTop * PxPerRow;
	const float HaloScale = (NaturalHalo > 0.f) ? FMath::Clamp(FMath::Max(0.f, MaxHalo) / NaturalHalo, 0.f, 1.f) : 0.f;
	const float HaloUp = NaturalHalo * HaloScale;
	const float HaloDown = (KR::SheetH - KR::RailRowBottom) * PxPerRow * HaloScale;
	const float HaloLeft = KR::RailColLeft * PxPerRow * HaloScale;
	const float HaloRight = (KR::SheetW - KR::RailColRight) * PxPerRow * HaloScale;
	const float EndW = FMath::Min(KR::EndCols * PxPerRow, W * 0.5f);

	// Five columns (halo, end, stretched middle, end, halo) by three rows (halo, rail, halo): every cell
	// keeps its own sampled band, so the lips never stretch and the halo is never cut.
	struct FRailSpan
	{
		float From;
		float Size;
		float Start;
		float Span;
	};
	const FRailSpan Columns[5] = {
		{ X - HaloLeft, HaloLeft, 0.f, KR::RailColLeft / KR::SheetW },
		{ X, EndW, KR::RailColLeft / KR::SheetW, KR::EndCols / KR::SheetW },
		{ X + EndW, W - EndW * 2.f, KR::MiddleU, KR::MiddleUW },
		{ X + W - EndW, EndW, (KR::RailColRight - KR::EndCols) / KR::SheetW, KR::EndCols / KR::SheetW },
		{ X + W, HaloRight, KR::RailColRight / KR::SheetW, (KR::SheetW - KR::RailColRight) / KR::SheetW },
	};
	const FRailSpan Bands[3] = {
		{ Y - HaloUp, HaloUp, 0.f, KR::RailRowTop / KR::SheetH },
		{ Y, H, KR::RailRowTop / KR::SheetH, (KR::RailRowBottom - KR::RailRowTop) / KR::SheetH },
		{ Y + H, HaloDown, KR::RailRowBottom / KR::SheetH, (KR::SheetH - KR::RailRowBottom) / KR::SheetH },
	};

	for (const FRailSpan& Band : Bands)
	{
		for (const FRailSpan& Column : Columns)
		{
			if (Band.Size <= 0.f || Column.Size <= 0.f)
			{
				continue;
			}
			FTraceKitQuad Quad;
			Quad.X = Column.From;
			Quad.W = Column.Size;
			Quad.U = Column.Start;
			Quad.UW = Column.Span;
			Quad.Y = Band.From;
			Quad.H = Band.Size;
			Quad.V = Band.Start;
			Quad.VH = Band.Span;
			TraceMenuKitFile::IssueTexturedQuad(HUD, RailSprite, Quad, Tint);
		}
	}
}

FBox2D TraceMenuKit::SliderHandleRect(float CenterX, float CenterY, float TrackH)
{
	const float HandleH = FMath::Max(0.f, TrackH) * SliderHandleToTrack;
	const float HandleW = HandleH * SliderHandleAspect;
	return FBox2D(FVector2D(CenterX - HandleW * 0.5f, CenterY - HandleH * 0.5f),
		FVector2D(CenterX + HandleW * 0.5f, CenterY + HandleH * 0.5f));
}

void TraceMenuKit::DrawSliderHandle(AHUD* HUD, float CenterX, float CenterY, float TrackH, const FLinearColor& Tint)
{
	if (HUD == nullptr || TrackH <= 0.f)
	{
		return;
	}

	const FBox2D Rect = SliderHandleRect(CenterX, CenterY, TrackH);
	const FVector2D Size = Rect.GetSize();

	if (UTexture2D* Blade = Sprite(ETraceKitSprite::SliderHandle))
	{
		FTraceKitQuad Quad;
		Quad.X = static_cast<float>(Rect.Min.X);
		Quad.Y = static_cast<float>(Rect.Min.Y);
		Quad.W = static_cast<float>(Size.X);
		Quad.H = static_cast<float>(Size.Y);
		Quad.UW = 1.f;
		Quad.VH = 1.f;
		TraceMenuKitFile::IssueTexturedQuad(HUD, Blade, Quad, Tint);
		return;
	}

	const float BarW = FMath::Max(2.f, static_cast<float>(Size.X) * 0.25f);
	TraceMenuKitFile::FadedRect(HUD, Tint, CenterX - BarW * 0.5f, static_cast<float>(Rect.Min.Y), BarW, static_cast<float>(Size.Y));
}

float TraceMenuKit::KeyChipWidth(const FString& Key, float H)
{
	if (Key.IsEmpty() || H <= 0.f)
	{
		return 0.f;
	}
	const float Word = TraceText::MeasureWidth(Key, TraceText::FStyle(LabelSize(H), FLinearColor::White,
		ETraceTextWeight::Light));
	return FMath::Max(H * KeyChipAspect, Word + H * LabelPadFraction * 2.f);
}

float TraceMenuKit::DrawKeyChip(AHUD* HUD, ETraceKitState State, float X, float Y, float H,
	const FString& Key, float NowSeconds)
{
	const float W = KeyChipWidth(Key, H);
	if (HUD == nullptr || W <= 0.f)
	{
		return 0.f;
	}

	DrawStatePlate(HUD, State, X, Y, W, H, NowSeconds, H);
	DrawLabel(HUD, Key, X + W * 0.5f, Y + H * 0.5f, H, VisualsAt(State, X, Y, W, H).Label,
		W - H * LabelPadFraction * 2.f);
	return W;
}

float TraceMenuKit::DrawTextCapCentered(AHUD* HUD, const FString& Text, float X, float CapCenterY,
	const TraceText::FStyle& Style)
{
	if (HUD == nullptr || Text.IsEmpty())
	{
		return 0.f;
	}
	TraceText::FStyle CapTopStyle = Style;
	CapTopStyle.VAlign = TraceText::EVAlign::CapTop;
	const float Caps = TraceText::CapHeight(CapTopStyle.Size, CapTopStyle.Weight);
	return TraceCanvasText::Draw(HUD, Text, X, CapCenterY - Caps * 0.5f, CapTopStyle);
}

TraceText::FStyle TraceMenuKit::CapStyle(float CapH, const FLinearColor& Color, ETraceTextWeight Weight)
{
	return TraceText::FStyle(TraceText::SizeForCapHeight(FMath::Max(1.f, CapH), Weight), Color, Weight);
}

float TraceMenuKit::CapTextWidth(const FString& Text, float CapH, ETraceTextWeight Weight)
{
	return Text.IsEmpty() ? 0.f : TraceText::MeasureWidth(Text, CapStyle(CapH, FLinearColor::White, Weight));
}

float TraceMenuKit::DrawCapText(AHUD* HUD, const FString& Text, float X, float CapCenterY, float CapH,
	const FLinearColor& Color, ETraceTextWeight Weight, TraceText::EHAlign HAlign, float MaxW)
{
	if (HUD == nullptr || Text.IsEmpty() || CapH <= 0.f)
	{
		return 0.f;
	}
	TraceText::FStyle Drawn = CapStyle(CapH, Color, Weight);
	Drawn.HAlign = HAlign;
	if (MaxW > 0.f)
	{
		const float Natural = TraceText::MeasureWidth(Text, Drawn);
		if (Natural > MaxW && Natural > 0.f)
		{
			Drawn.Size *= MaxW / Natural;
		}
	}
	return DrawTextCapCentered(HUD, Text, X, CapCenterY, Drawn);
}

namespace TraceMenuKitFile
{
	/** A legend's two gaps, as fractions of the chip height: chip to its verb, and pair to pair. */
	static constexpr float LegendVerbGap = 0.31f;
	static constexpr float LegendPairGap = 1.06f;

	static TraceText::FStyle LegendVerbStyle(float ChipH)
	{
		return TraceText::FStyle(TraceMenuKit::LabelSize(ChipH), TraceMenuKit::FurnitureUnselected,
			ETraceTextWeight::Light);
	}
}

float TraceMenuKit::KeyLegendWidth(const TArray<FTraceKitLegendItem>& Items, float ChipH)
{
	if (ChipH <= 0.f)
	{
		return 0.f;
	}
	const TraceText::FStyle VerbStyle = TraceMenuKitFile::LegendVerbStyle(ChipH);
	float Total = 0.f;
	int32 Pairs = 0;
	for (const FTraceKitLegendItem& Item : Items)
	{
		if (Item.Key.IsEmpty() || Item.Label.IsEmpty())
		{
			continue;
		}
		Total += KeyChipWidth(Item.Key, ChipH) + ChipH * TraceMenuKitFile::LegendVerbGap
			+ TraceText::MeasureWidth(Item.Label, VerbStyle);
		++Pairs;
	}
	return Total + ChipH * TraceMenuKitFile::LegendPairGap * FMath::Max(0, Pairs - 1);
}

float TraceMenuKit::DrawKeyLegend(AHUD* HUD, const TArray<FTraceKitLegendItem>& Items, float CenterX, float Y,
	float ChipH, float NowSeconds)
{
	const float Total = KeyLegendWidth(Items, ChipH);
	if (HUD == nullptr || Total <= 0.f)
	{
		return 0.f;
	}

	const TraceText::FStyle VerbStyle = TraceMenuKitFile::LegendVerbStyle(ChipH);
	float PenX = CenterX - Total * 0.5f;
	bool bFirst = true;
	for (const FTraceKitLegendItem& Item : Items)
	{
		if (Item.Key.IsEmpty() || Item.Label.IsEmpty())
		{
			continue;
		}
		if (!bFirst)
		{
			PenX += ChipH * TraceMenuKitFile::LegendPairGap;
		}
		bFirst = false;
		PenX += DrawKeyChip(HUD, ETraceKitState::Default, PenX, Y, ChipH, Item.Key, NowSeconds);
		PenX += ChipH * TraceMenuKitFile::LegendVerbGap;
		PenX += DrawTextCapCentered(HUD, Item.Label, PenX, Y + ChipH * 0.5f, VerbStyle);
	}
	return Total;
}

float TraceMenuKit::KeyLegendFit(float ChipH, float MaxW, std::initializer_list<float> Widths)
{
	float Widest = 0.f;
	for (const float Each : Widths)
	{
		Widest = FMath::Max(Widest, Each);
	}
	if (MaxW <= 0.f || Widest <= MaxW)
	{
		return ChipH;
	}
	return ChipH * FMath::Max(0.7f, MaxW / Widest);
}

void TraceMenuKit::DrawBackground(AHUD* HUD, float ViewW, float ViewH)
{
	if (HUD != nullptr)
	{
		TraceMenuKitFile::FadedRect(HUD, Background, 0.f, 0.f, ViewW, ViewH);
	}
}

void TraceMenuKit::DrawScrim(AHUD* HUD, float ViewW, float ViewH, float Alpha)
{
	if (HUD != nullptr)
	{
		TraceMenuKitFile::FadedRect(HUD, FLinearColor(0.f, 0.f, 0.f, FMath::Clamp(Alpha, 0.f, 1.f)), 0.f, 0.f, ViewW, ViewH);
	}
}

bool TraceMenuKit::DrawTravelSpinner(AHUD* HUD, float ViewW, float ViewH, float UIScale, double PlatformSeconds)
{
	UTexture2D* Crescent = Sprite(ETraceKitSprite::Chevron);
	if (HUD == nullptr || Crescent == nullptr || Crescent->GetSizeX() <= 0 || Crescent->GetSizeY() <= 0)
	{
		return false;
	}

	// The same box, the same pivot and the same clock as the Slate card (UI/TraceLoadingScreen.cpp):
	// the sprite's own rect, centred at the pivot, rotated about its middle.
	const float SpinH = TraceTitleLayout::SpinnerHeight * UIScale;
	const float SpinW = SpinH * (static_cast<float>(Crescent->GetSizeX()) / static_cast<float>(Crescent->GetSizeY()));
	const float Inset = TraceTitleLayout::SpinnerInset * UIScale;
	HUD->DrawTexture(Crescent, ViewW - Inset - SpinW, ViewH - Inset - SpinH, SpinW, SpinH,
		0.f, 0.f, 1.f, 1.f, Faded(FLinearColor::White), BLEND_Translucent, 1.f, false,
		TraceTitleLayout::SpinnerAngleDegrees(PlatformSeconds), FVector2D(0.5f, 0.5f));
	return true;
}

// =================================================================================================
// THE POINTER
// =================================================================================================

bool TraceMenuKit::DrawCursor(AHUD* HUD, const FVector2D& Tip, float UIScale)
{
	return TraceHardwareCursor::DrawPointer(HUD, Tip, UIScale);
}

bool TraceMenuKit::ShowCursor(AHUD* HUD, APlayerController* PC, const TCHAR* Owner, const FVector2D& Tip,
	float UIScale)
{
	TraceHardwareCursor::EnsureRunning();
	TraceHardwareCursor::RenewSuppression(PC, Owner);
	return TraceHardwareCursor::DrawPointer(HUD, Tip, UIScale);
}

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.UI.Kit.Specimen — every kit control on one black page, for a screenshot
// =================================================================================================
//
// The kit has functions no screen calls yet (the screens are converted by later packages). A
// function nobody has looked at is a guess, so this draws each of them once, in every state, at the
// sheet's own proportions, over everything else on the Canvas title. Run it on the Canvas path:
//
//     MainMenu -TraceNoMenuUMG -TraceExecOn=Menu -TraceExec="Trace.UI.Kit.Specimen 1" -TraceAutoShot=10
//
// Dev-only page, so its words are plain literals rather than TRACE_TEXT: no player ever sees it.

namespace TraceMenuKitFile
{
	static int32 GSpecimen = 0;

	static FAutoConsoleVariableRef CVarKitSpecimen(
		TEXT("Trace.UI.Kit.Specimen"),
		GSpecimen,
		TEXT("Dev only. 1 draws every handmade-kit control (UI/Widgets/Menu/TraceMenuKit.h) on a black ")
		TEXT("page over the Canvas title screen: buttons in all four states, value box, slider track and ")
		TEXT("blade handle, keybind plate, key chips and the pointer. Use with -TraceNoMenuUMG."),
		ECVF_Default);
}

void TraceMenuKit::DrawSpecimenIfRequested(AHUD* HUD, float ViewW, float ViewH, float UIScale, float NowSeconds)
{
	if (TraceMenuKitFile::GSpecimen == 0 || HUD == nullptr || ViewW <= 0.f || ViewH <= 0.f)
	{
		return;
	}

	const float S = FMath::Max(0.25f, UIScale);
	DrawBackground(HUD, ViewW, ViewH);

	// Column 1: the four states on the sheet's own button, 283 x 74.
	const float BtnW = 283.f * S;
	const float BtnH = 74.f * S;
	const float Pitch = 100.f * S;
	const float Col1 = 140.f * S;
	float Top = 120.f * S;

	struct FSpecimenRow { ETraceKitState State; const TCHAR* Word; };
	const FSpecimenRow States[] =
	{
		{ ETraceKitState::Hover,    TEXT("HOVER") },
		{ ETraceKitState::Default,  TEXT("DEFAULT") },
		{ ETraceKitState::Pressed,  TEXT("PRESSED") },
		{ ETraceKitState::Disabled, TEXT("DISABLED") },
	};
	for (const FSpecimenRow& Each : States)
	{
		DrawButton(HUD, Each.State, Col1, Top, BtnW, BtnH, Each.Word, NowSeconds);
		Top += Pitch;
	}

	// A title-size row (720 x 60) in the two states the title uses.
	Top += 20.f * S;
	DrawButton(HUD, ETraceKitState::Hover, Col1, Top, 720.f * S, 60.f * S, TEXT("PLAY"), NowSeconds);
	DrawButton(HUD, ETraceKitState::Default, Col1, Top + 71.f * S, 720.f * S, 60.f * S, TEXT("SETTINGS"), NowSeconds);

	// Column 2: the slider — track, blade at 30 %, the value box on the same centre line.
	const float Col2 = 620.f * S;
	const float TrackH = 21.f * S;
	const float TrackW = 420.f * S;
	const float SliderMid = 170.f * S;
	DrawSliderTrack(HUD, Col2, SliderMid - TrackH * 0.5f, TrackW, TrackH);
	DrawSliderHandle(HUD, Col2 + TrackW * 0.30f, SliderMid, TrackH);

	const float BoxH = TrackH * ValuePlateToTrack;
	const float BoxW = BoxH * (TraceMenuArtStyle::ValueFrame.PlateW / TraceMenuArtStyle::ValueFrame.PlateH);
	DrawValueBox(HUD, Col2 + TrackW + 24.f * S, SliderMid - BoxH * 0.5f, BoxW, BoxH, TEXT("13"),
		TraceMenuArtStyle::WordHoverLifted());

	// Column 2, lower: KEYBIND on the wide plate, KEY chips beside it, hover over default like the sheet.
	const float KeyTop = 300.f * S;
	const float KeyH = 60.f * S;
	const float KeybindW = KeyH * ButtonAspect;
	DrawButton(HUD, ETraceKitState::Hover, Col2, KeyTop, KeybindW, KeyH, TEXT("KEYBIND"), NowSeconds);
	DrawButton(HUD, ETraceKitState::Default, Col2, KeyTop + 80.f * S, KeybindW, KeyH, TEXT("KEYBIND"), NowSeconds);

	const float ChipX = Col2 + KeybindW + 24.f * S;
	DrawKeyChip(HUD, ETraceKitState::Hover, ChipX, KeyTop, KeyH, TEXT("KEY"), NowSeconds);
	DrawKeyChip(HUD, ETraceKitState::Default, ChipX, KeyTop + 80.f * S, KeyH, TEXT("KEY"), NowSeconds);

	// A long key name widens its chip; a disabled chip is the dark plate.
	const float LongW = DrawKeyChip(HUD, ETraceKitState::Default, Col2, KeyTop + 160.f * S, KeyH,
		TEXT("LEFT SHIFT"), NowSeconds);
	DrawKeyChip(HUD, ETraceKitState::Disabled, Col2 + LongW + 24.f * S, KeyTop + 160.f * S, KeyH,
		TEXT("UNBOUND"), NowSeconds);

	// Tall cards with a button-sized corner, as character select draws its tiles.
	DrawStatePlate(HUD, ETraceKitState::Default, Col1, 740.f * S, 346.f * S, 196.f * S, NowSeconds, 60.f * S);
	DrawStatePlate(HUD, ETraceKitState::Hover, Col1 + 370.f * S, 740.f * S, 346.f * S, 196.f * S, NowSeconds, 60.f * S);
	DrawStatePlate(HUD, ETraceKitState::Disabled, Col1 + 740.f * S, 740.f * S, 346.f * S, 196.f * S, NowSeconds, 60.f * S);

	// The pointer, parked beside the hover button.
	DrawCursor(HUD, FVector2D(Col1 + BtnW * 0.8f, 120.f * S + BtnH * 0.7f), S);
}

// =================================================================================================
// Trace.UI.Kit.Verify — the kit is ONE renderer, and it is the one the three screens used to be
// =================================================================================================

namespace TraceMenuKitFile
{
	/** The options page's 3-slice, restated exactly as TraceOptionsMenuArt::DrawPlate shipped it. */
	static int32 LegacyOptions3Slice(const TraceMenuArtStyle::FSpriteFrame& Frame,
		float X, float Y, float W, float H, FTraceKitQuad (&Out)[3], bool& bOutClamped)
	{
		const float Scale = H / Frame.PlateH;
		const float GlowPx = Frame.Glow * Scale;
		const float CapWant = Frame.Cap * Scale;
		const float CapU = Frame.Cap / Frame.SpriteW();

		const float SX = X - GlowPx;
		const float SY = Y - GlowPx;
		const float SW = W + GlowPx * 2.f;
		const float SH = H + GlowPx * 2.f;

		const float CapPx = FMath::Min(CapWant, SW * 0.5f);
		bOutClamped = CapPx < CapWant;
		const float MidW = SW - CapPx * 2.f;

		int32 Filled = 0;
		Out[Filled++] = { SX, SY, CapPx, SH, 0.f, 0.f, CapU, 1.f };
		if (MidW > 0.f)
		{
			Out[Filled++] = { SX + CapPx, SY, MidW, SH, CapU, 0.f, 1.f - CapU * 2.f, 1.f };
		}
		Out[Filled++] = { SX + SW - CapPx, SY, CapPx, SH, 1.f - CapU, 0.f, CapU, 1.f };
		return Filled;
	}

	/** The Canvas title's 3-slice, restated exactly as TraceMenuHUDSprites::DrawPlate shipped it. */
	static int32 LegacyTitle3Slice(const TraceMenuArtStyle::FSpriteFrame& Frame,
		float X, float Y, float W, float H, FTraceKitQuad (&Out)[3], bool& bOutClamped)
	{
		const float Grow = H * (Frame.Glow / Frame.PlateH);
		const float DrawnH = H + Grow * 2.f;
		const float CapU = Frame.Cap / Frame.SpriteW();
		const float CapWant = DrawnH * (Frame.Cap / Frame.SpriteH());

		const float SX = X - Grow;
		const float SY = Y - Grow;
		const float SW = W + Grow * 2.f;

		const float CapPx = FMath::Min(CapWant, SW * 0.5f);
		bOutClamped = CapPx < CapWant;
		const float MidW = SW - CapPx * 2.f;

		int32 Filled = 0;
		Out[Filled++] = { SX, SY, CapPx, DrawnH, 0.f, 0.f, CapU, 1.f };
		if (MidW > 0.f)
		{
			Out[Filled++] = { SX + CapPx, SY, MidW, DrawnH, CapU, 0.f, 1.f - CapU * 2.f, 1.f };
		}
		Out[Filled++] = { SX + SW - CapPx, SY, CapPx, DrawnH, 1.f - CapU, 0.f, CapU, 1.f };
		return Filled;
	}

	/** The texture coordinate a quad set puts at screen point (PX, PY), or false if none covers it. */
	static bool UVAt(const FTraceKitQuad* Quads, int32 NumQuads, float PX, float PY, FVector2D& OutUV)
	{
		for (int32 Each = 0; Each < NumQuads; ++Each)
		{
			const FTraceKitQuad& Quad = Quads[Each];
			if (PX >= Quad.X && PX < Quad.X + Quad.W && PY >= Quad.Y && PY < Quad.Y + Quad.H)
			{
				OutUV.X = Quad.U + (PX - Quad.X) / Quad.W * Quad.UW;
				OutUV.Y = Quad.V + (PY - Quad.Y) / Quad.H * Quad.VH;
				return true;
			}
		}
		return false;
	}

	static FBox2D BoundsOf(const FTraceKitQuad* Quads, int32 NumQuads)
	{
		FBox2D Box(ForceInit);
		for (int32 Each = 0; Each < NumQuads; ++Each)
		{
			Box += FVector2D(Quads[Each].X, Quads[Each].Y);
			Box += FVector2D(Quads[Each].X + Quads[Each].W, Quads[Each].Y + Quads[Each].H);
		}
		return Box;
	}

	/**
	 * Worst texture-coordinate disagreement between two quad sets over a grid of points spanning
	 * both, in UV units, and the worst disagreement of their outer bounds, in pixels.
	 */
	static void Compare(const FTraceKitQuad* A, int32 NumA, const FTraceKitQuad* B, int32 NumB,
		double& OutWorstUV, double& OutWorstEdge, int32& OutUncovered)
	{
		const FBox2D BoxA = BoundsOf(A, NumA);
		const FBox2D BoxB = BoundsOf(B, NumB);
		OutWorstEdge = FMath::Max(
			FMath::Max(FMath::Abs(BoxA.Min.X - BoxB.Min.X), FMath::Abs(BoxA.Min.Y - BoxB.Min.Y)),
			FMath::Max(FMath::Abs(BoxA.Max.X - BoxB.Max.X), FMath::Abs(BoxA.Max.Y - BoxB.Max.Y)));

		OutWorstUV = 0.0;
		OutUncovered = 0;
		constexpr int32 Columns = 97;
		constexpr int32 Rows = 31;
		for (int32 Row = 0; Row < Rows; ++Row)
		{
			for (int32 Column = 0; Column < Columns; ++Column)
			{
				const float PX = static_cast<float>(BoxA.Min.X + (BoxA.Max.X - BoxA.Min.X) * (Column + 0.5) / Columns);
				const float PY = static_cast<float>(BoxA.Min.Y + (BoxA.Max.Y - BoxA.Min.Y) * (Row + 0.5) / Rows);
				FVector2D UVA;
				FVector2D UVB;
				const bool bA = UVAt(A, NumA, PX, PY, UVA);
				const bool bB = UVAt(B, NumB, PX, PY, UVB);
				if (bA != bB)
				{
					++OutUncovered;
					continue;
				}
				if (bA)
				{
					OutWorstUV = FMath::Max(OutWorstUV,
						FMath::Max(FMath::Abs(UVA.X - UVB.X), FMath::Abs(UVA.Y - UVB.Y)));
				}
			}
		}
	}

	static AHUD* FindLocalHUD()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
			{
				continue;
			}
			if (UWorld* World = Context.World())
			{
				if (APlayerController* PC = World->GetFirstPlayerController())
				{
					if (AHUD* Found = PC->GetHUD())
					{
						return Found;
					}
				}
			}
		}
		return nullptr;
	}

	static void RunVerify()
	{
		int32 Failures = 0;
		auto Check = [&Failures](const TCHAR* Label, bool bPass, const FString& Detail)
		{
			Failures += bPass ? 0 : 1;
			UE_LOG(LogTraceGame, Display, TEXT("[MenuKit]   %-4s %-60s %s"),
				bPass ? TEXT("ok") : TEXT("FAIL"), Label, *Detail);
		};

		UE_LOG(LogTraceGame, Display, TEXT("[MenuKit] ===== one kit renderer: states, plates, guard ====="));

		// ---- 1. THE STATE TABLE (stylespec §5) ---------------------------------------------------
		{
			const FTraceKitVisuals D = TraceMenuKit::VisualsFor(ETraceKitState::Default);
			const FTraceKitVisuals Hv = TraceMenuKit::VisualsFor(ETraceKitState::Hover);
			const FTraceKitVisuals P = TraceMenuKit::VisualsFor(ETraceKitState::Pressed);
			const FTraceKitVisuals Dis = TraceMenuKit::VisualsFor(ETraceKitState::Disabled);
			const FLinearColor Green = TraceMenuArtStyle::WordHoverLifted();
			const float QuarterTurn = static_cast<float>(UE_HALF_PI);

			Check(TEXT("DEFAULT: default plate, white word, still"),
				D.Plate == ETraceKitSprite::BtnDefault && D.Label == TraceMenuArtStyle::WordDefault
					&& !D.bPulses && D.PlateTint == 1.f && D.Furniture == TraceMenuKit::FurnitureUnselected,
				TEXT("BtnDefault / WordDefault / 1.0 / furniture 0.85"));
			Check(TEXT("HOVER: hover plate, lifted green word, breathes"),
				Hv.Plate == ETraceKitSprite::BtnHover && Hv.Label == Green && Hv.bPulses
					&& Hv.Furniture == TraceMenuKit::FurnitureSelected,
				TEXT("BtnHover / WordHoverLifted / pulse / furniture white"));
			Check(TEXT("PRESSED: hover plate knocked down, green word, held"),
				P.Plate == ETraceKitSprite::BtnHover && P.Label == Green && !P.bPulses
					&& P.PlateTint == TraceMenuArtStyle::PressedTint,
				FString::Printf(TEXT("BtnHover x %.2f"), TraceMenuArtStyle::PressedTint));
			Check(TEXT("DISABLED: disabled plate, grey word and furniture"),
				Dis.Plate == ETraceKitSprite::BtnDisabled && Dis.Label == TraceMenuArtStyle::WordDisabled
					&& Dis.Furniture == TraceMenuArtStyle::WordDisabled && !Dis.bPulses,
				TEXT("BtnDisabled / WordDisabled"));
			Check(TEXT("rank: disabled > pressed > hover > default"),
				TraceMenuKit::StateFor(false, true, true) == ETraceKitState::Disabled
					&& TraceMenuKit::StateFor(true, true, true) == ETraceKitState::Pressed
					&& TraceMenuKit::StateFor(true, true, false) == ETraceKitState::Hover
					&& TraceMenuKit::StateFor(true, false, false) == ETraceKitState::Default,
				TEXT("StateFor(enabled, selected, pressed)"));
			Check(TEXT("the hover breath is 1.0 +/- 0.10; only HOVER breathes"),
				FMath::IsNearlyEqual(TraceMenuKit::HoverPulse(-QuarterTurn / TraceMenuKit::HoverPulseSpeed), 0.9f, 1e-4f)
					&& FMath::IsNearlyEqual(TraceMenuKit::HoverPulse(QuarterTurn / TraceMenuKit::HoverPulseSpeed), 1.1f, 1e-4f)
					&& TraceMenuKit::PlateTintAt(D, 1.234f) == 1.f
					&& TraceMenuKit::PlateTintAt(P, 1.234f) == TraceMenuArtStyle::PressedTint,
				FString::Printf(TEXT("%.2f +/- %.2f"), TraceMenuKit::HoverPulseBase, TraceMenuKit::HoverPulseSwing));

			// THE KIT'S COLOURS (stylespec §2). The hover word is a MODERATE lift of the sheet's olive,
			// #88AB4B: dimmer than the white default word, as on the sheet. It shipped as #CBFF70, the
			// loudest text on the screen; the luminance comparison is what would catch that coming back.
			const FColor GreenBytes = Green.ToFColor(/*bSRGB=*/true);
			const float GreenLum = 0.2126f * Green.R + 0.7152f * Green.G + 0.0722f * Green.B;
			Check(TEXT("the hover word is the sheet's olive lifted to #88AB4B"),
				GreenBytes.R == 136 && GreenBytes.G == 171 && GreenBytes.B == 75,
				FString::Printf(TEXT("sRGB(%d,%d,%d)"), GreenBytes.R, GreenBytes.G, GreenBytes.B));
			Check(TEXT("...and it is DIMMER than the default white word"),
				GreenLum < 0.5f,
				FString::Printf(TEXT("relative luminance %.3f (white is 1.0)"), GreenLum));

			// The pointer is the artist's WHITE blade, not the pre-kit interface cyan (stylespec §9).
			const FLinearColor PointerInk = TraceHardwareCursor::PointerTint();
			Check(TEXT("the pointer is white"),
				PointerInk.Equals(FLinearColor::White, 1e-3f),
				FString::Printf(TEXT("tint (%.2f, %.2f, %.2f)"), PointerInk.R, PointerInk.G, PointerInk.B));
		}

		// ---- 2. THE PLATE IS THE ONE THE THREE SCREENS DREW ---------------------------------------
		//
		// The kit's 9-slice with CornerHeight == H against the options page's and the Canvas title's
		// 3-slices, restated above exactly as they shipped. Same covered rect, same texture
		// coordinate at every sampled point = the same pixels, so re-pointing those two screens is not
		// a visual change. (Character select's 9-slice IS this function, moved.)
		{
			const float Widths[] = { 48.f, 96.f, 283.f, 707.f, 1414.f };
			const float Heights[] = { 14.f, 18.f, 30.f, 60.f, 120.f };
			const TraceMenuArtStyle::FSpriteFrame* Frames[] = { &TraceMenuArtStyle::ButtonFrame, &TraceMenuArtStyle::ValueFrame };

			double WorstUV = 0.0;
			double WorstEdge = 0.0;
			int32 Uncovered = 0;
			int32 Compared = 0;
			int32 Skipped = 0;
			FString WorstCase;

			for (const TraceMenuArtStyle::FSpriteFrame* Frame : Frames)
			{
				for (const float PlateW : Widths)
				{
					for (const float PlateH : Heights)
					{
						FTraceKitQuad Kit[9];
						const int32 NumKit = TraceMenuKit::PlateQuads(*Frame, 100.25f, 50.5f, PlateW, PlateH, PlateH, Kit);

						for (int32 Legacy = 0; Legacy < 2; ++Legacy)
						{
							FTraceKitQuad Old[3];
							bool bClamped = false;
							const int32 NumOld = (Legacy == 0)
								? LegacyOptions3Slice(*Frame, 100.25f, 50.5f, PlateW, PlateH, Old, bClamped)
								: LegacyTitle3Slice(*Frame, 100.25f, 50.5f, PlateW, PlateH, Old, bClamped);

							// A plate narrower than its own two corners is clamped differently by
							// every copy, and no screen draws one. Counted, not compared.
							if (bClamped || NumKit == 0)
							{
								++Skipped;
								continue;
							}

							double CaseUV = 0.0;
							double CaseEdge = 0.0;
							int32 CaseUncovered = 0;
							Compare(Kit, NumKit, Old, NumOld, CaseUV, CaseEdge, CaseUncovered);
							++Compared;
							Uncovered += CaseUncovered;
							if (CaseUV > WorstUV || CaseEdge > WorstEdge)
							{
								WorstCase = FString::Printf(TEXT("%s %s %.0fx%.0f"),
									(Frame == &TraceMenuArtStyle::ButtonFrame) ? TEXT("button") : TEXT("value"),
									(Legacy == 0) ? TEXT("vs options") : TEXT("vs title"), PlateW, PlateH);
							}
							WorstUV = FMath::Max(WorstUV, CaseUV);
							WorstEdge = FMath::Max(WorstEdge, CaseEdge);
						}
					}
				}
			}

			Check(TEXT("9-slice == the options/title 3-slice, every sampled UV"),
				Compared > 0 && WorstUV < 1e-4 && Uncovered == 0,
				FString::Printf(TEXT("%d cases, worst %.2e UV (%s), %d uncovered samples, %d narrow cases skipped"),
					Compared, WorstUV, *WorstCase, Uncovered, Skipped));
			Check(TEXT("...and the same covered rect"),
				WorstEdge < 1e-3,
				FString::Printf(TEXT("worst edge %.2e px"), WorstEdge));
		}

		// ---- 3. THE PLATE LANDS ON THE RECT, THE CORNER STAYS ROUND ------------------------------
		{
			const TraceMenuArtStyle::FSpriteFrame& Frame = TraceMenuArtStyle::ButtonFrame;
			FTraceKitQuad Row[9];
			const int32 NumRow = TraceMenuKit::PlateQuads(Frame, 600.f, 400.f, 720.f, 60.f, 60.f, Row);
			const FBox2D Box = BoundsOf(Row, NumRow);
			const double Overhang = 60.0 * Frame.Glow / Frame.PlateH;
			Check(TEXT("a 720x60 row: sprite = rect grown by the glow on every side"),
				NumRow == 9
					&& FMath::IsNearlyEqual(Box.Min.X, 600.0 - Overhang, 1e-3)
					&& FMath::IsNearlyEqual(Box.Max.Y, 460.0 + Overhang, 1e-3),
				FString::Printf(TEXT("overhang %.3f px (0.1041 x H)"), Overhang));
			Check(TEXT("...and its corner slice is square (a circular corner)"),
				NumRow == 9 && FMath::IsNearlyEqual(Row[0].W, Row[0].H, 1e-4f)
					&& FMath::IsNearlyEqual(Row[0].W, 60.f * Frame.Cap / Frame.PlateH, 1e-3f),
				FString::Printf(TEXT("%.3f x %.3f px"), NumRow > 0 ? Row[0].W : 0.f, NumRow > 0 ? Row[0].H : 0.f));

			FTraceKitQuad Card[9];
			const int32 NumCard = TraceMenuKit::PlateQuads(Frame, 100.f, 100.f, 346.f, 196.f, 60.f, Card);
			Check(TEXT("a 346x196 card with CornerHeight 60 keeps a button's corner"),
				NumCard == 9 && FMath::IsNearlyEqual(Card[0].W, 60.f * Frame.Cap / Frame.PlateH, 1e-3f)
					&& FMath::IsNearlyEqual(Card[0].H, Card[0].W, 1e-4f),
				FString::Printf(TEXT("corner %.2f px, not %.2f"), NumCard > 0 ? Card[0].W : 0.f,
					196.f * Frame.Cap / Frame.PlateH));
		}

		// ---- 4. THE GUARD ----------------------------------------------------------------------
		//
		// A texture with no render resource — exactly what a cold load looks like for its first frame
		// or two, and what character select's own DrawPlate used to hand straight to the canvas. The
		// kit must refuse it and issue NOTHING. Drawn through the real HUD (outside its draw pass, so a
		// regression here costs a log warning, not the render thread).
		{
			UTexture2D* Unready = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
			AHUD* LiveHUD = FindLocalHUD();

			Check(TEXT("IsDrawable: null and resource-less textures are not drawable"),
				!TraceMenuKit::IsDrawable(nullptr) && Unready != nullptr && !TraceMenuKit::IsDrawable(Unready),
				FString::Printf(TEXT("resource %s"), (Unready != nullptr && Unready->GetResource() != nullptr)
					? TEXT("present") : TEXT("absent")));

			if (LiveHUD == nullptr)
			{
				Check(TEXT("DrawPlate refuses an undrawable texture"), false,
					TEXT("INCONCLUSIVE: no local HUD to draw through - run this in a game world"));
			}
			else
			{
				const int64 Before = GTexturedQuadsIssued;
				const bool bDrew = TraceMenuKit::DrawPlate(LiveHUD, Unready, TraceMenuArtStyle::ButtonFrame,
					10.f, 10.f, 200.f, 60.f, 60.f, FLinearColor::White);
				const int64 Issued = GTexturedQuadsIssued - Before;
				Check(TEXT("DrawPlate refuses an undrawable texture and issues no quad"),
					!bDrew && Issued == 0,
					FString::Printf(TEXT("returned %s, %lld quad(s) issued through %s"),
						bDrew ? TEXT("true") : TEXT("false"), Issued, *LiveHUD->GetClass()->GetName()));
			}

			if (Unready != nullptr)
			{
				Unready->MarkAsGarbage();
			}
		}

		// ---- 5. THE SPRITES --------------------------------------------------------------------
		{
			int32 Loaded = 0;
			int32 DrawableNow = 0;
			FString Missing;
			for (int32 Slot = 0; Slot < SpriteCount; ++Slot)
			{
				const ETraceKitSprite Which = static_cast<ETraceKitSprite>(Slot);
				DrawableNow += (TraceMenuKit::Sprite(Which) != nullptr) ? 1 : 0;
				if (TraceMenuKit::PeekSprite(Which) != nullptr)
				{
					++Loaded;
				}
				else
				{
					Missing += FString::Printf(TEXT("%s "), TraceMenuKit::SpritePath(Which));
				}
			}
			Check(TEXT("every kit sprite loads"), Loaded == SpriteCount,
				FString::Printf(TEXT("%d/%d loaded, %d drawable this frame %s"), Loaded, SpriteCount,
					DrawableNow, *Missing));
		}

		// ---- 6. THE KEY LEGEND (added for the pre-match screens) ---------------------------------
		{
			const TArray<FTraceKitLegendItem> Legend = { { TEXT("ENTER"), TEXT("EQUIP") }, { TEXT("Q / E"), TEXT("TAB") } };
			const TArray<FTraceKitLegendItem> Removed = { { TEXT("ENTER"), TEXT("") }, { TEXT(""), TEXT("TAB") } };
			const float At32 = TraceMenuKit::KeyLegendWidth(Legend, 32.f);
			const float At64 = TraceMenuKit::KeyLegendWidth(Legend, 64.f);
			Check(TEXT("a legend's width scales with its chip (one fit serves two rows)"),
				At32 > 0.f && FMath::IsNearlyEqual(At64, At32 * 2.f, At32 * 0.02f),
				FString::Printf(TEXT("%.1f at 32, %.1f at 64"), At32, At64));
			Check(TEXT("a pair with an emptied key or verb draws nothing at all"),
				TraceMenuKit::KeyLegendWidth(Removed, 32.f) == 0.f, TEXT("the text contract: no chip round a removed word"));
			Check(TEXT("KeyLegendFit shrinks to fit, never below 70 %"),
				FMath::IsNearlyEqual(TraceMenuKit::KeyLegendFit(32.f, At32 * 0.5f, { At32 }), 32.f * 0.7f)
					&& FMath::IsNearlyEqual(TraceMenuKit::KeyLegendFit(32.f, At32 * 0.9f, { At32, At32 * 0.5f }), 32.f * 0.9f, 1e-3f)
					&& TraceMenuKit::KeyLegendFit(32.f, At32 * 2.f, { At32 }) == 32.f,
				TEXT(""));
		}

		// ---- 7. A LABEL IN ANOTHER FACE (added for the settings submenus, Erbaum Bold body) ----------
		{
			const float LightCaps = TraceText::CapHeight(TraceMenuKit::LabelSize(60.f), ETraceTextWeight::Light);
			const float HudCaps = TraceText::CapHeight(TraceMenuKit::LabelSize(60.f, ETraceTextWeight::Hud),
				ETraceTextWeight::Hud);
			Check(TEXT("a label's caps are 0.37 of its plate in either face"),
				FMath::IsNearlyEqual(LightCaps, 60.f * TraceMenuKit::LabelCapFraction, 0.05f)
					&& FMath::IsNearlyEqual(HudCaps, 60.f * TraceMenuKit::LabelCapFraction, 0.05f),
				FString::Printf(TEXT("light %.2f px, hud %.2f px on a 60 px plate"), LightCaps, HudCaps));
		}

		// ---- 8. A PANEL (added for the match HUD) --------------------------------------------------
		//
		// The HUD's plates are panels, not buttons: they must carry an alpha (a kill-feed row fades out
		// with its names) and must not breathe (the "about you" hover plate is up for seconds). Drawn
		// through the real HUD outside its draw pass, like section 4: the canvas refuses the quads with
		// a log line, and what is checked is what the kit ASKED for.
		{
			AHUD* LiveHUD = FindLocalHUD();
			const int64 BeforeZero = GTexturedQuadsIssued;
			const bool bZeroDrew = TraceMenuKit::DrawPanelPlate(LiveHUD, ETraceKitState::Default,
				10.f, 10.f, 200.f, 40.f, 0.f, 0.f);
			Check(TEXT("a panel at alpha 0 draws nothing"),
				!bZeroDrew && GTexturedQuadsIssued == BeforeZero, TEXT(""));

			if (LiveHUD == nullptr || TraceMenuKit::Sprite(ETraceKitSprite::BtnHover) == nullptr)
			{
				Check(TEXT("a panel carries its alpha and does not breathe"), false,
					TEXT("INCONCLUSIVE: needs a local HUD and a drawable hover plate - run in a game world"));
			}
			else
			{
				const int64 Before = GTexturedQuadsIssued;
				const bool bDrew = TraceMenuKit::DrawPanelPlate(LiveHUD, ETraceKitState::Hover,
					10.f, 10.f, 200.f, 40.f, 0.f, 0.35f);
				const FLinearColor Asked = GLastQuadTint;
				Check(TEXT("a panel carries its alpha and does not breathe"),
					bDrew && GTexturedQuadsIssued > Before && FMath::IsNearlyEqual(Asked.A, 0.35f)
						&& Asked.R == 1.f && Asked.G == 1.f && Asked.B == 1.f,
					FString::Printf(TEXT("%lld quad(s), tint (%.2f, %.2f, %.2f, %.2f)"),
						GTexturedQuadsIssued - Before, Asked.R, Asked.G, Asked.B, Asked.A));
			}
		}

		// ---- 9. PRIME LOADS EVERY SPRITE ---------------------------------------------------------
		Check(TEXT("Prime() loads every kit sprite"), TraceMenuKit::Prime() == SpriteCount,
			FString::Printf(TEXT("%d/%d"), TraceMenuKit::Prime(), SpriteCount));

		// ---- 10. P10: A FADE TAKES THE SAME REAL TIME AT ANY FRAME RATE -----------------------------
		//
		// Driven with explicit timestamps, frame by frame, at four frame rates. A per-frame step (the
		// failure this guards against) would open in 9 frames at every rate: 38 ms at 240 fps, 300 ms at
		// 30 fps. The fade must instead be at the same place at the same TIME.
		{
			// GMotion and the fade scale are console knobs; the check measures the shipped behaviour.
			const int32 SavedMotion = GMotion;
			const float SavedScale = GFadeScale;
			GMotion = 1;
			GFadeScale = 1.f;

			const float Rates[] = { 30.f, 60.f, 144.f, 240.f };
			const double Probe = TraceMenuKit::FadeInSeconds * 0.5;
			float AtProbe[4] = {};
			float Worst = 0.f;
			bool bAllOpenOnTime = true;
			FString Detail;
			for (int32 Each = 0; Each < 4; ++Each)
			{
				FTraceKitFade Fade;
				const double Step = 1.0 / Rates[Each];
				double T = 100.0;
				Fade.UpdateAt(false, T);
				const double OpenedAt = T;
				bool bProbed = false;
				while (T - OpenedAt < TraceMenuKit::FadeInSeconds + 0.1)
				{
					T += Step;
					Fade.UpdateAt(true, T);
					if (!bProbed && (T - OpenedAt) >= Probe)
					{
						// Interpolated back to the probe instant so a frame boundary is not a difference.
						AtProbe[Each] = Fade.Linear() - static_cast<float>(((T - OpenedAt) - Probe) / TraceMenuKit::FadeInSeconds);
						bProbed = true;
					}
					if ((T - OpenedAt) >= TraceMenuKit::FadeInSeconds + 1e-6 && !Fade.IsFullyShown())
					{
						bAllOpenOnTime = false;
					}
					if ((T - OpenedAt) < TraceMenuKit::FadeInSeconds - Step && Fade.IsFullyShown())
					{
						bAllOpenOnTime = false;   // finished early: a per-frame step at a high rate
					}
				}
				Worst = FMath::Max(Worst, FMath::Abs(AtProbe[Each] - 0.5f));
				Detail += FString::Printf(TEXT("%.0ffps %.3f  "), Rates[Each], AtProbe[Each]);
			}
			Check(TEXT("P10: a fade is half open at half its time, at 30/60/144/240 fps"),
				Worst < 0.01f, Detail);
			Check(TEXT("P10: ...and fully open at FadeInSeconds, not a frame count"),
				bAllOpenOnTime, FString::Printf(TEXT("%.0f ms"), TraceMenuKit::FadeInSeconds * 1000.f));

			// A close that interrupts an open turns round from where it is: no jump in alpha.
			FTraceKitFade Turn;
			double T = 50.0;
			Turn.UpdateAt(false, T);
			float Previous = 0.f;
			float WorstJump = 0.f;
			for (int32 Frame = 0; Frame < 30; ++Frame)
			{
				T += 1.0 / 60.0;
				const float A = Turn.UpdateAt(Frame < 4, T);
				WorstJump = FMath::Max(WorstJump, FMath::Abs(A - Previous));
				Previous = A;
			}
			Check(TEXT("P10: an open reversed mid-fade closes from where it was"),
				WorstJump < 0.25f && Previous == 0.f,
				FString::Printf(TEXT("largest per-frame change %.3f, ends at %.2f"), WorstJump, Previous));

			// Two updates in one frame (the HUD advances the pause menu's fade, then the menu's Tick does).
			FTraceKitFade Twice;
			Twice.UpdateAt(false, 10.0);
			const float First = Twice.UpdateAt(true, 10.05);
			const float Second = Twice.UpdateAt(true, 10.05);
			Check(TEXT("P10: a second update in the same frame changes nothing"), First == Second && First > 0.f,
				FString::Printf(TEXT("%.3f then %.3f"), First, Second));

			GMotion = SavedMotion;
			GFadeScale = SavedScale;
		}

		// ---- 11. P10: THE SCREEN'S OPACITY SCOPE ---------------------------------------------------
		{
			const float Before = TraceMenuKit::Opacity();
			float Inner = 0.f;
			float Outer = 0.f;
			FLinearColor Shown = FLinearColor::White;
			{
				TraceMenuKit::FScopedOpacity Half(0.5f);
				Outer = TraceMenuKit::Opacity();
				{
					TraceMenuKit::FScopedOpacity Again(0.5f);
					Inner = TraceMenuKit::Opacity();
					Shown = TraceMenuKit::Faded(FLinearColor(1.f, 1.f, 1.f, 0.8f));
				}
			}
			Check(TEXT("P10: opacity scopes multiply, and restore on exit"),
				Before == 1.f && FMath::IsNearlyEqual(Outer, 0.5f) && FMath::IsNearlyEqual(Inner, 0.25f)
					&& FMath::IsNearlyEqual(Shown.A, 0.2f) && TraceMenuKit::Opacity() == 1.f,
				FString::Printf(TEXT("%.2f -> %.2f -> %.2f; 0.8 alpha drawn at %.2f"), Before, Outer, Inner, Shown.A));

			AHUD* LiveHUD = FindLocalHUD();
			if (LiveHUD != nullptr && TraceMenuKit::Sprite(ETraceKitSprite::BtnDefault) != nullptr)
			{
				const int64 BeforeQuads = GTexturedQuadsIssued;
				{
					TraceMenuKit::FScopedOpacity Faint(0.4f);
					TraceMenuKit::DrawPanelPlate(LiveHUD, ETraceKitState::Default, 10.f, 10.f, 200.f, 40.f, 0.f, 0.5f);
				}
				Check(TEXT("P10: a kit plate drawn inside a scope carries the scope's opacity"),
					GTexturedQuadsIssued > BeforeQuads && FMath::IsNearlyEqual(GLastQuadTint.A, 0.2f, 1e-4f),
					FString::Printf(TEXT("panel alpha 0.5 in a 0.4 scope drew at %.3f"), GLastQuadTint.A));
			}
			else
			{
				Check(TEXT("P10: a kit plate drawn inside a scope carries the scope's opacity"), false,
					TEXT("INCONCLUSIVE: needs a local HUD and a drawable plate - run in a game world"));
			}
		}

		// ---- 12. P10: THE HOVER RING EASES, ON REAL TIME ------------------------------------------
		{
			const int32 SavedMotion = GMotion;
			const float SavedScale = GFadeScale;
			const uint64 SavedSweep = GHoverSweptAt;
			GMotion = 1;
			GFadeScale = 1.f;
			const uint64 Key = HoverKey(-3000.f, -3000.f, 97.f, 31.f);   // a rect no screen draws
			GHover.Remove(Key);

			// A plate seen for the first time starts AT its target (a page that just opened does not
			// animate its highlight in under its own fade).
			const float Fresh = HoverBlendAt(Key, true, 1000, 5.0);

			// Then off, at 60 fps: it must take HoverOutSeconds, whatever the frame rate.
			float At60 = 1.f;
			uint64 Frame = 1000;
			double T = 5.0;
			const double Half = TraceMenuKit::HoverOutSeconds * 0.5;
			while (T - 5.0 < Half - 1e-9)
			{
				T += 1.0 / 60.0;
				At60 = HoverBlendAt(Key, false, ++Frame, T);
			}
			GHover.Remove(Key);
			HoverBlendAt(Key, true, 5000, 20.0);
			float At144 = 1.f;
			Frame = 5000;
			T = 20.0;
			while (T - 20.0 < Half - 1e-9)
			{
				T += 1.0 / 144.0;
				At144 = HoverBlendAt(Key, false, ++Frame, T);
			}
			// A gap of a frame (the plate was not drawn) and it starts over at its target.
			const float AfterGap = HoverBlendAt(Key, true, Frame + 5, T + 0.2);
			GHover.Remove(Key);
			GMotion = SavedMotion;
			GFadeScale = SavedScale;
			GHoverSweptAt = SavedSweep;

			Check(TEXT("P10: a plate seen for the first time starts at its target"), Fresh == 1.f,
				FString::Printf(TEXT("%.2f"), Fresh));
			Check(TEXT("P10: the hover ring is half off at half HoverOutSeconds, 60 or 144 fps"),
				FMath::Abs(At60 - 0.5f) < 0.07f && FMath::Abs(At144 - 0.5f) < 0.04f,
				FString::Printf(TEXT("60fps %.3f, 144fps %.3f (%.0f ms)"), At60, At144,
					TraceMenuKit::HoverOutSeconds * 1000.f));
			Check(TEXT("P10: a plate that was not drawn last frame starts over at its target"), AfterGap == 1.f,
				FString::Printf(TEXT("%.2f"), AfterGap));
			const FTraceKitVisuals Mid = TraceMenuKit::VisualsForBlend(ETraceKitState::Hover, 0.5f);
			const FLinearColor Expect = FMath::Lerp(TraceMenuArtStyle::WordDefault, TraceMenuArtStyle::WordHoverLifted(), 0.5f);
			Check(TEXT("P10: the word eases with the ring (half-way colour at half blend)"),
				Mid.Label.Equals(Expect, 1e-4f),
				FString::Printf(TEXT("(%.3f, %.3f, %.3f)"), Mid.Label.R, Mid.Label.G, Mid.Label.B));
		}

		// ---- 12b. TWO SURFACES ON ONE RECT, IN ONE FRAME, KEEP THEIR OWN HOVER ----------------------
		//
		// The pause menu's LOADOUTS editor is drawn over the match loadout page in the same frame, with
		// the same layout, so every card of one sits on a card of the other. Keyed by rect alone they
		// were ONE blend: the page asked first and lit its card, the editor's ask in the same frame
		// read what the page left, and the editor's ring stayed on the page's card. Each surface now
		// draws inside its own FScopedHoverSalt. Driven frame by frame as the two draw: both unlit,
		// then 50 ms on the first surface lights the card and the second leaves it dark.
		{
			const int32 SavedMotion = GMotion;
			const float SavedScale = GFadeScale;
			const uint64 SavedSweep = GHoverSweptAt;
			GMotion = 1;
			GFadeScale = 1.f;

			uint64 PageKey = 0;
			uint64 EditorKey = 0;
			{
				TraceMenuKit::FScopedHoverSalt PageSalt(0x51A7u);
				PageKey = HoverKey(-3000.f, -2800.f, 346.f, 326.f);   // a card-sized rect no screen draws
			}
			{
				TraceMenuKit::FScopedHoverSalt EditorSalt(0xED17u);
				EditorKey = HoverKey(-3000.f, -2800.f, 346.f, 326.f);
			}
			const uint32 SaltAfter = GHoverSalt;
			GHover.Remove(PageKey);
			GHover.Remove(EditorKey);

			HoverBlendAt(PageKey, false, 9000, 40.0);
			HoverBlendAt(EditorKey, false, 9000, 40.0);
			const float PageLit = HoverBlendAt(PageKey, true, 9001, 40.05);
			const float EditorDark = HoverBlendAt(EditorKey, false, 9001, 40.05);

			GHover.Remove(PageKey);
			GHover.Remove(EditorKey);
			GMotion = SavedMotion;
			GFadeScale = SavedScale;
			GHoverSweptAt = SavedSweep;

			Check(TEXT("P10: two surfaces on one rect in one frame keep their own hover"),
				PageKey != EditorKey && PageLit >= 0.5f && EditorDark == 0.f && SaltAfter == 0,
				FString::Printf(TEXT("first surface lit %.2f, second (not lit) %.2f, keys %s, salt after scopes %u"),
					PageLit, EditorDark, (PageKey != EditorKey) ? TEXT("differ") : TEXT("SHARED"), SaltAfter));
		}

		// ---- 13. P10: TABULAR FIGURES -----------------------------------------------------------------
		//
		// The clock, the countdowns, the slider values and the FPS readout ask for them; every digit
		// must then take the same advance in every face, so "11:11" and "00:00" are the same width.
		{
			FString Detail;
			bool bSteady = true;
			bool bProportionalDiffers = false;
			for (int32 Face = 0; Face < static_cast<int32>(ETraceTextWeight::Count); ++Face)
			{
				TraceText::FStyle Tab(40.f, FLinearColor::White, static_cast<ETraceTextWeight>(Face));
				Tab.bTabularDigits = true;
				const float W11 = TraceText::MeasureWidth(TEXT("11:11"), Tab);
				const float W00 = TraceText::MeasureWidth(TEXT("00:00"), Tab);
				const float W47 = TraceText::MeasureWidth(TEXT("47:29"), Tab);
				bSteady &= FMath::IsNearlyEqual(W11, W00, 0.01f) && FMath::IsNearlyEqual(W47, W00, 0.01f);

				TraceText::FStyle Prop = Tab;
				Prop.bTabularDigits = false;
				const float P11 = TraceText::MeasureWidth(TEXT("11:11"), Prop);
				const float P00 = TraceText::MeasureWidth(TEXT("00:00"), Prop);
				bProportionalDiffers |= !FMath::IsNearlyEqual(P11, P00, 0.5f);

				Detail += FString::Printf(TEXT("%s: %.1f/%.1f/%.1f (prop %.1f/%.1f)  "),
					TraceText::WeightName(static_cast<ETraceTextWeight>(Face)), W11, W00, W47, P11, P00);
			}
			Check(TEXT("P10: tabular digits keep a ticking number's width, every face"),
				bSteady && bProportionalDiffers && TraceText::IsAtlasActive(), Detail);

			// ...and a '1' sits CENTRED in its wider cell: its quad is shifted, not left-aligned.
			TraceText::FStyle One(40.f, FLinearColor::White, ETraceTextWeight::Light);
			One.bTabularDigits = true;
			TArray<TraceText::FGlyphQuad> Quads;
			const bool bLaid = TraceText::LayoutString(TEXT("1"), One, Quads);
			const float Cell = TraceText::MeasureWidth(TEXT("1"), One);
			const float Centre = (bLaid && Quads.Num() == 1) ? (Quads[0].Pos.X + Quads[0].Size.X * 0.5f) : -1.f;
			Check(TEXT("P10: ...and a narrow digit is centred in the tabular cell"),
				bLaid && Quads.Num() == 1 && FMath::IsNearlyEqual(Centre, Cell * 0.5f, 0.05f),
				FString::Printf(TEXT("'1' centre %.2f in a %.2f cell"), Centre, Cell));
		}

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[MenuKit] ===== PASS — one renderer: the state table is the spec's, the plate is the "
				     "one all three screens drew, and an unready texture never reaches the canvas ====="));
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[MenuKit] ===== *** FAIL *** %d check(s) — see above ====="), Failures);
		}
	}

	static FAutoConsoleCommand CmdKitVerify(
		TEXT("Trace.UI.Kit.Verify"),
		TEXT("Checks the shared handmade-kit Canvas renderer (UI/Widgets/Menu/TraceMenuKit.h): the ")
		TEXT("per-state table, that its 9-slice draws exactly what the three screens' old copies drew, ")
		TEXT("and that a texture with no render resource is refused. Run in any game world."),
		FConsoleCommandDelegate::CreateStatic(&RunVerify));
}
#endif // !UE_BUILD_SHIPPING
