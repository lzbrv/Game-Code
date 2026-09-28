// Trace — see TraceMenuKit.h. The one Canvas renderer for the artist's kit.

#include "UI/Widgets/Menu/TraceMenuKit.h"

#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "TextureResource.h"            // FTextureResource::TextureRHI — the guard
#include "UObject/Package.h"            // GetTransientPackage — Trace.UI.Kit.Verify's unready texture

#include "Trace.h"                      // LogTraceGame
#include "UI/TraceHardwareCursor.h"
#include "UI/Text/TraceCanvasText.h"
#include "UI/Text/TraceText.h"

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
#endif

	static void IssueTexturedQuad(AHUD* HUD, UTexture2D* Texture, const FTraceKitQuad& Quad,
		const FLinearColor& Tint)
	{
		HUD->DrawTexture(Texture, Quad.X, Quad.Y, Quad.W, Quad.H, Quad.U, Quad.V, Quad.UW, Quad.VH,
			Tint, BLEND_Translucent);
#if !UE_BUILD_SHIPPING
		++GTexturedQuadsIssued;
#endif
	}

	/** A rect outline, @p Thick px, inside (X, Y, W, H). */
	static void StrokeRect(AHUD* HUD, float X, float Y, float W, float H, float Thick, const FLinearColor& Color)
	{
		const float T = FMath::Min(Thick, FMath::Min(W, H) * 0.5f);
		HUD->DrawRect(Color, X, Y, W, T);
		HUD->DrawRect(Color, X, Y + H - T, W, T);
		HUD->DrawRect(Color, X, Y + T, T, H - T * 2.f);
		HUD->DrawRect(Color, X + W - T, Y + T, T, H - T * 2.f);
	}

	static FLinearColor Scaled(const FLinearColor& InColor, float InTint)
	{
		return FLinearColor(InColor.R * InTint, InColor.G * InTint, InColor.B * InTint, InColor.A);
	}
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

	HUD->DrawRect(TraceMenuKitFile::Scaled(bDisabled ? TraceMenuArtStyle::DisabledFill : TraceMenuArtStyle::PlateFill, Tint),
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

	const FTraceKitVisuals Visuals = VisualsFor(State);
	const float PlateTint = PlateTintAt(Visuals, NowSeconds);
	const float Corner = (CornerHeight > 0.f) ? CornerHeight : H;

	if (!DrawPlate(HUD, Sprite(Visuals.Plate), TraceMenuArtStyle::ButtonFrame, X, Y, W, H, Corner,
		FLinearColor(PlateTint, PlateTint, PlateTint, 1.f)))
	{
		DrawFallbackPlate(HUD, State, X, Y, W, H, PlateTint);
	}
}

// =================================================================================================
// TEXT
// =================================================================================================

float TraceMenuKit::LabelSize(float PlateH, ETraceTextWeight Weight)
{
	return TraceText::SizeForCapHeight(FMath::Max(0.f, PlateH) * LabelCapFraction, Weight);
}

float TraceMenuKit::DrawLabel(AHUD* HUD, const FString& Text, float CenterX, float CenterY, float PlateH,
	const FLinearColor& Color, float MaxWidth, ETraceTextWeight Weight)
{
	if (HUD == nullptr || Text.IsEmpty() || PlateH <= 0.f)
	{
		return 0.f;
	}

	TraceText::FStyle Style(LabelSize(PlateH, Weight), Color, Weight);
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
	DrawLabel(HUD, Label, X + W * 0.5f, Y + H * 0.5f, WordPlateH, VisualsFor(State).Label,
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
		HUD->DrawRect(TraceMenuArtStyle::PlateFill, X, Y, W, H);
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
	DrawLabel(HUD, Text, X + W * 0.5f, Y + H * 0.5f, H, TextColor, W - H * LabelPadFraction * 2.f);
	return true;
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
		HUD->DrawRect(TraceMenuArtStyle::PlateFill, X, Y + H * TrackRailTopV, W, FMath::Max(2.f, H * TrackRailV));
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
	HUD->DrawRect(Tint, CenterX - BarW * 0.5f, static_cast<float>(Rect.Min.Y), BarW, static_cast<float>(Size.Y));
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
	DrawLabel(HUD, Key, X + W * 0.5f, Y + H * 0.5f, H, VisualsFor(State).Label, W - H * LabelPadFraction * 2.f);
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
		HUD->DrawRect(Background, 0.f, 0.f, ViewW, ViewH);
	}
}

void TraceMenuKit::DrawScrim(AHUD* HUD, float ViewW, float ViewH, float Alpha)
{
	if (HUD != nullptr)
	{
		HUD->DrawRect(FLinearColor(0.f, 0.f, 0.f, FMath::Clamp(Alpha, 0.f, 1.f)), 0.f, 0.f, ViewW, ViewH);
	}
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

		// ---- 8. PRIME LOADS EVERY SPRITE ---------------------------------------------------------
		Check(TEXT("Prime() loads every kit sprite"), TraceMenuKit::Prime() == SpriteCount,
			FString::Printf(TEXT("%d/%d"), TraceMenuKit::Prime(), SpriteCount));

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
