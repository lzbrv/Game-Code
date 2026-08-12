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
	 * Not a UPROPERTY and it does not need to be: /Engine/EngineFonts/Roboto is an engine asset that
	 * is already rooted for the life of the process, and this file keeps no reference across a GC that
	 * could unload it. Storing an FSlateFontInfo (which holds a soft path plus a weak object) rather
	 * than a raw UFont* keeps that true.
	 */
	static bool bResolved = false;
	static bool bFontFound = false;
	static bool bTypefaceFound = false;
	static FSlateFontInfo Resolved;

	static void Resolve()
	{
		if (bResolved)
		{
			return;
		}
		bResolved = true;

		UFont* Font = LoadObject<UFont>(nullptr, TraceMenuArtStyle::MenuFontAsset);
		if (Font == nullptr)
		{
			// Slate draws an empty FSlateFontInfo as NOTHING — no glyphs, no error, an empty screen.
			// A menu in the wrong font is recoverable; a menu with no words in it is not.
			UE_LOG(LogTraceGame, Error,
				TEXT("[MenuArt] %s did not load. Every menu label will fall back to the engine default ")
				TEXT("font. This is the only place a font is named; fix it here."),
				TraceMenuArtStyle::MenuFontAsset);
			Resolved = FSlateFontInfo();
			return;
		}
		bFontFound = true;

		// The composite font's typeface list is the authority on whether "Light" exists. Asking rather
		// than assuming, because a typeface name that is not there does not fail — Slate silently
		// serves the first face in the list, which is Regular, and nobody notices the weight is wrong.
		const FName Wanted(TraceMenuArtStyle::MenuFontTypeface);
		for (const FTypefaceEntry& Entry : Font->CompositeFont.DefaultTypeface.Fonts)
		{
			if (Entry.Name == Wanted)
			{
				bTypefaceFound = true;
				break;
			}
		}

		if (!bTypefaceFound)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[MenuArt] %s has no typeface called '%s'; the menu is drawing in Regular instead. ")
				TEXT("This is a stand-in for the artist's face either way — see TraceMenuArtStyle.h."),
				TraceMenuArtStyle::MenuFontAsset, TraceMenuArtStyle::MenuFontTypeface);
		}

		Resolved = FSlateFontInfo(Font, 24, bTypefaceFound ? Wanted : FName(TEXT("Regular")));
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
	return TraceMenuArtStyleFile::bFontFound && TraceMenuArtStyleFile::bTypefaceFound;
}

FString TraceMenuArtStyle::DescribeMenuFont()
{
	TraceMenuArtStyleFile::Resolve();
	if (!TraceMenuArtStyleFile::bFontFound)
	{
		return FString::Printf(TEXT("NONE — %s did not load"), TraceMenuArtStyle::MenuFontAsset);
	}
	return FString::Printf(
		TEXT("%s '%s'%s — a STAND-IN. The artist's typeface is not in the project; the sheet is a PNG ")
		TEXT("and cannot supply one. Only PLAY / SETTINGS / KEYBIND / KEY are the real letterforms."),
		TraceMenuArtStyle::MenuFontAsset,
		TraceMenuArtStyleFile::bTypefaceFound ? TraceMenuArtStyle::MenuFontTypeface : TEXT("Regular"),
		TraceMenuArtStyleFile::bTypefaceFound ? TEXT("") : TEXT(" (asked for 'Light', it is not there)"));
}
