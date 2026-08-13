// Trace — see TraceCanvasText.h.

#include "UI/Text/TraceCanvasText.h"

#include "CanvasItem.h"             // FCanvasTextItem / FCanvasTileItem
#include "CanvasTypes.h"            // FCanvas — the DPI scale behind the UCanvas
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "GameFramework/HUD.h"
#include "UObject/UObjectGlobals.h"

#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

// Named after the file for the unity/jumbo build; see Scripts/check-jumbo-build-collisions.py.
namespace TraceCanvasTextFile
{
	/** See the header. Cached weakly: the object outlives a frame but not necessarily a level load. */
	static UCanvas* FindGameCanvas()
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

		// Only accepted mid-draw: a UCanvas between frames still exists but has no FCanvas behind it,
		// and drawing into that is a null deref rather than an invisible no-op.
		if (Found != nullptr && Found->Canvas != nullptr && Found->SizeX > 0 && Found->SizeY > 0)
		{
			return Found;
		}
		return nullptr;
	}

	/**
	 * The Lato path, used whenever the atlas is not live.
	 *
	 * FCanvasTextItem rather than AHUD::DrawText because DrawText takes a UFont and a SCALE FACTOR,
	 * and scaling a runtime font is not the same thing as asking for it at a size — it magnifies a
	 * cached raster. The item takes an FSlateFontInfo, which is also what TraceText measures with, so
	 * the width this draws matches the width MeasureWidth() reported. That agreement is the whole
	 * reason this branch is not one line of AHUD::DrawText.
	 */
	static float DrawFallback(UCanvas* Surface, const FString& Text, float X, float Y,
		const TraceText::FStyle& Style)
	{
		const FVector2f Block = TraceText::Measure(Text, Style);
		const FVector2f Offset = TraceText::AlignOffset(Block, Style, Style.Size);

		if (Surface != nullptr && !Text.IsEmpty())
		{
			FCanvasTextItem Item(
				FVector2D(X + Offset.X, Y + Offset.Y),
				FText::FromString(Text),
				TraceMenuArtStyle::MenuFont(Style.Size),
				Style.Color);

			// In DPI-scaled pixels, like everything else inside the item.
			const float Dpi = FMath::Max(0.01f, Surface->Canvas->GetDPIScale());
			Item.HorizSpacingAdjust = Style.Tracking * Dpi;
			Surface->DrawItem(Item);
		}

		return Block.X;
	}

	/**
	 * The atlas path: one tile per glyph.
	 *
	 * FCanvasTileItem's UV0/UV1 are NORMALISED, which is what AHUD::DrawTexture forwards them as and
	 * what TraceText::LayoutString already produces, so there is no conversion here and no second
	 * opinion about where a cell is.
	 *
	 * SE_BLEND_Translucent, not the default: the sheet is white ink in the alpha channel and Style's
	 * colour is a tint over it. Anything else draws opaque black boxes with letters cut out of them.
	 */
	static float DrawAtlas(UCanvas* Surface, const FString& Text, float X, float Y,
		const TraceText::FStyle& Style)
	{
		UTexture2D* Atlas = TraceText::AtlasTexture();
		if (Surface == nullptr || Atlas == nullptr || Atlas->GetResource() == nullptr)
		{
			return 0.f;
		}

		TArray<TraceText::FGlyphQuad> Quads;
		if (!TraceText::LayoutString(Text, Style, Quads))
		{
			return 0.f;
		}

		for (const TraceText::FGlyphQuad& Quad : Quads)
		{
			FCanvasTileItem Tile(
				FVector2D(X + Quad.Pos.X, Y + Quad.Pos.Y),
				Atlas->GetResource(),
				FVector2D(Quad.Size.X, Quad.Size.Y),
				FVector2D(Quad.UVMin.X, Quad.UVMin.Y),
				FVector2D(Quad.UVMin.X + Quad.UVSize.X, Quad.UVMin.Y + Quad.UVSize.Y),
				Style.Color);
			Tile.BlendMode = SE_BLEND_Translucent;
			Surface->DrawItem(Tile);
		}

		return TraceText::MeasureWidth(Text, Style);
	}
}

UCanvas* TraceCanvasText::GameCanvas()
{
	return TraceCanvasTextFile::FindGameCanvas();
}

float TraceCanvasText::Draw(UCanvas* Canvas, const FString& Text, float X, float Y,
	const TraceText::FStyle& Style)
{
	if (Canvas == nullptr || Text.IsEmpty() || Style.Size <= 0.f)
	{
		return 0.f;
	}

	return TraceText::IsAtlasActive()
		? TraceCanvasTextFile::DrawAtlas(Canvas, Text, X, Y, Style)
		: TraceCanvasTextFile::DrawFallback(Canvas, Text, X, Y, Style);
}

float TraceCanvasText::Draw(AHUD* HUD, const FString& Text, float X, float Y,
	const TraceText::FStyle& Style)
{
	// The HUD is taken as proof that a draw pass is running, not as a drawing surface: everything
	// below needs the UCanvas underneath it, which AHUD does not expose.
	if (HUD == nullptr)
	{
		return 0.f;
	}
	return Draw(TraceCanvasTextFile::FindGameCanvas(), Text, X, Y, Style);
}

float TraceCanvasText::Draw(AHUD* HUD, const FString& Text, float X, float Y,
	float Size, const FLinearColor& Color)
{
	return Draw(HUD, Text, X, Y, TraceText::FStyle(Size, Color));
}

float TraceCanvasText::DrawCentered(AHUD* HUD, const FString& Text, float CenterX, float Y,
	float Size, const FLinearColor& Color)
{
	TraceText::FStyle Style(Size, Color);
	Style.HAlign = TraceText::EHAlign::Center;
	return Draw(HUD, Text, CenterX, Y, Style);
}
