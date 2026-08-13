// Trace — see TraceMenuArtStyle.h. The only file in the project that loads a font.

#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

#include "Engine/Font.h"
#include "Fonts/CompositeFont.h"
#include "UObject/UObjectGlobals.h"

#include "Trace.h"   // LogTraceGame

// Named after the file rather than anonymous: two anonymous namespaces merged into one unity
// translation unit is MSVC C2084 on Windows only, and Scripts/check-jumbo-build-collisions.py gates
// on the name being here.
namespace TraceMenuArtStyleFile
{
	/**
	 * Resolved once, on first use.
	 *
	 * Not a UPROPERTY and it does not need to be: an FSlateFontInfo holds a soft path plus a weak
	 * object rather than a raw UFont*, and this file keeps no strong reference across a GC. The
	 * interim face is a /Game asset rather than an engine one, so it is NOT rooted for the life of
	 * the process the way /Engine/EngineFonts/Roboto is — but every consumer (the UMG text blocks
	 * and the Canvas screens) holds its own reference for as long as it is drawing with it, which is
	 * exactly the lifetime that matters.
	 */
	static bool bResolved = false;
	static bool bFontFound = false;
	static bool bTypefaceFound = false;
	static bool bUsingFallback = false;
	static FString ResolvedAsset;
	static FString ResolvedTypeface;
	static FSlateFontInfo Resolved;

	static UFont* LoadFont(const TCHAR* Path)
	{
		return (Path != nullptr && *Path != TEXT('\0')) ? LoadObject<UFont>(nullptr, Path) : nullptr;
	}

	/**
	 * The composite font's typeface list is the authority on whether a named face exists. Asking
	 * rather than assuming, because a typeface name that is not there does not fail — Slate silently
	 * serves the first face in the list and nobody notices the weight is wrong.
	 */
	static bool HasTypeface(const UFont& Font, const FName Wanted)
	{
		for (const FTypefaceEntry& Entry : Font.GetInternalCompositeFont().DefaultTypeface.Fonts)
		{
			if (Entry.Name == Wanted)
			{
				return true;
			}
		}
		return false;
	}

	/** The name Slate would fall back to on its own: the first face the font declares. */
	static FName FirstTypeface(const UFont& Font)
	{
		const TArray<FTypefaceEntry>& Fonts = Font.GetInternalCompositeFont().DefaultTypeface.Fonts;
		return Fonts.Num() > 0 ? Fonts[0].Name : NAME_None;
	}

	static void Resolve()
	{
		if (bResolved)
		{
			return;
		}
		bResolved = true;

		const TCHAR* AssetPath = TraceMenuArtStyle::MenuFontAsset;
		const TCHAR* TypefaceName = TraceMenuArtStyle::MenuFontTypeface;

		UFont* Font = LoadFont(AssetPath);
		if (Font == nullptr)
		{
			// The interim face is a PROJECT asset, so unlike an engine font it genuinely can be
			// absent: nobody re-ran Scripts/generate-menu-widgets.py after MenuFontSourceFile
			// changed, or the import failed, or /Game/Trace/UI was left out of the cook. Fall back
			// rather than draw nothing — and say which, loudly, because a fallback still looks like
			// a working menu.
			bUsingFallback = true;
			AssetPath = TraceMenuArtStyle::MenuFontFallbackAsset;
			TypefaceName = TraceMenuArtStyle::MenuFontFallbackTypeface;
			Font = LoadFont(AssetPath);

			UE_LOG(LogTraceGame, Error,
				TEXT("[MenuArt] %s did not load; the menu is falling back to %s. Re-run ")
				TEXT("Scripts/generate-menu-widgets.py inside the editor — it is what imports the ")
				TEXT("font named by TraceMenuArtStyle::MenuFontSourceFile."),
				TraceMenuArtStyle::MenuFontAsset, AssetPath);
		}

		if (Font == nullptr)
		{
			// Slate draws an empty FSlateFontInfo in its last-resort face — no crash, but no design
			// either. Both fonts being gone means something much larger is wrong than a typeface.
			UE_LOG(LogTraceGame, Error,
				TEXT("[MenuArt] Neither %s nor the %s fallback loaded. Every menu label will draw in ")
				TEXT("Slate's last-resort font. This is the only place a font is named; fix it here."),
				TraceMenuArtStyle::MenuFontAsset, TraceMenuArtStyle::MenuFontFallbackAsset);
			Resolved = FSlateFontInfo();
			return;
		}

		bFontFound = true;
		ResolvedAsset = AssetPath;

		const FName Wanted(TypefaceName);
		bTypefaceFound = HasTypeface(*Font, Wanted);

		FName Face = Wanted;
		if (!bTypefaceFound)
		{
			Face = FirstTypeface(*Font);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[MenuArt] %s has no typeface called '%s'; drawing in '%s' instead (its first ")
				TEXT("face, which is what Slate would have done silently). Fix ")
				TEXT("TraceMenuArtStyle::%s."),
				AssetPath, TypefaceName, *Face.ToString(),
				bUsingFallback ? TEXT("MenuFontFallbackTypeface") : TEXT("MenuFontTypeface"));
		}
		ResolvedTypeface = Face.ToString();

		Resolved = FSlateFontInfo(Font, 24, Face);
	}
}

FSlateFontInfo TraceMenuArtStyle::MenuFont(float InSize)
{
	TraceMenuArtStyleFile::Resolve();
	FSlateFontInfo Info = TraceMenuArtStyleFile::Resolved;
	Info.Size = InSize;
	return Info;
}

bool TraceMenuArtStyle::IsMenuFontResolved()
{
	TraceMenuArtStyleFile::Resolve();
	return TraceMenuArtStyleFile::bFontFound
		&& TraceMenuArtStyleFile::bTypefaceFound
		&& !TraceMenuArtStyleFile::bUsingFallback;
}

FString TraceMenuArtStyle::DescribeMenuFont()
{
	TraceMenuArtStyleFile::Resolve();
	if (!TraceMenuArtStyleFile::bFontFound)
	{
		return FString::Printf(TEXT("NONE — neither %s nor %s loaded"),
			TraceMenuArtStyle::MenuFontAsset, TraceMenuArtStyle::MenuFontFallbackAsset);
	}

	if (TraceMenuArtStyleFile::bUsingFallback)
	{
		return FString::Printf(
			TEXT("%s '%s' — THE FALLBACK, not the intended font. %s failed to load, so this menu is ")
			TEXT("drawing in the pre-v20 stand-in. Re-run Scripts/generate-menu-widgets.py."),
			*TraceMenuArtStyleFile::ResolvedAsset, *TraceMenuArtStyleFile::ResolvedTypeface,
			TraceMenuArtStyle::MenuFontAsset);
	}

	return FString::Printf(
		TEXT("%s '%s' (from %s) — THE FALLBACK FACE, and it resolved correctly. Since spec v22 §A1 ")
		TEXT("this is not normally what is on screen: the artist's face is Sofachrome, drawn from a ")
		TEXT("glyph atlas by Source/Trace/UI/Text because an offline UFont cannot drive UMG. Ask ")
		TEXT("`Trace.Text.Report` which of the two is actually drawing. Of the twelve fonts the owner ")
		TEXT("supplied, this one scored closest to the sheet's letterforms (IoU 0.5645) and is the ")
		TEXT("closest that is also open-licensed, which is why it is the one to degrade to."),
		*TraceMenuArtStyleFile::ResolvedAsset, *TraceMenuArtStyleFile::ResolvedTypeface,
		TraceMenuArtStyle::MenuFontSourceFile);
}
