// Trace — the three commands that make the game text document usable.
//
// =================================================================================================
//   Trace.Text.Dump     write the document out from what the game has registered, keeping edits
//   Trace.Text.Reload   re-read it without restarting
//   Trace.Text.Verify   say whether the document and the game agree, and where they do not
// =================================================================================================
//
// WHY DUMP IS THE ENTRY POINT AND NOT A HAND-WRITTEN FILE. The document has to list every string the
// game draws, and there are several hundred of them; a hand-maintained list is out of date the first
// time somebody adds a label, and out-of-date in the one direction that matters — a missing line is
// a string the owner cannot edit and has no way to discover. So the game writes the document from
// the keys it has actually registered, and the writer preserves existing wording, which makes
// regenerating it safe to do at any time.
//
// THE ONE THING TO KNOW ABOUT DUMP: keys register LAZILY, on the first draw. A dump taken at the
// title screen therefore contains the title screen's strings and nothing else. Both Dump and Verify
// say so, with the number, rather than letting a short file look like a complete one.
//
// Dev-only. Retail players do not need a way to rewrite the game's words, and the document they are
// given is whatever the build was made with.
// =================================================================================================

#if !UE_BUILD_SHIPPING

#include "UI/Text/TraceGameText.h"
#include "UI/Text/TraceText.h"          // CanDraw / DrawsInOwnFace — the charset lives there, not here

#include "Core/TraceCharacterRoster.h"   // the card prose is a layer over the roster; see below

#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

#include "Trace.h"

namespace
{
	/**
	 * Which of the three tiers a character falls into. Named after what the PLAYER would see, because
	 * that is the only thing the owner can act on.
	 *
	 * ASKED OF TraceText RATHER THAN OF THE METRICS HEADER. The first version of this file read
	 * TraceFontAtlasMetrics' FirstCode/LastCode and FallbackIndexOf directly, which quietly broke the
	 * rule that module states about itself: "this module is still the only thing that includes
	 * TraceFontAtlasMetrics.h", which is what keeps the charset in ONE place. A second reader would
	 * have kept working right up until somebody changed a sheet.
	 */
	enum class ECharTier : uint8
	{
		Native,      // in the drawing face: exactly the typeface the screen is designed in
		Fallback,    // not in the drawing face, but drawable: renders in the fallback face
		Substituted, // in neither: renders as "?"
	};

	ECharTier ClassifyChar(TCHAR Code)
	{
		if (!TraceText::CanDraw(Code))
		{
			return ECharTier::Substituted;
		}
		return TraceText::DrawsInOwnFace(Code) ? ECharTier::Native : ECharTier::Fallback;
	}

	/** Worst tier in the string, plus the first character that reached it. */
	ECharTier ClassifyString(const FString& Text, TCHAR& OutWorstChar)
	{
		ECharTier Worst = ECharTier::Native;
		OutWorstChar = TEXT('\0');

		for (const TCHAR Code : Text)
		{
			const ECharTier Tier = ClassifyChar(Code);
			if (Tier > Worst)
			{
				Worst = Tier;
				OutWorstChar = Code;
			}
		}
		return Worst;
	}

	FAutoConsoleCommand CmdDump(
		TEXT("Trace.Text.Dump"),
		TEXT("Write Config/TraceGameText.ini from every string the game has drawn so far, keeping any "
		     "wording already in it. Optional argument: a different path to write to."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const FString Path = (Args.Num() > 0)
				? FPaths::ConvertRelativePathToFull(Args[0])
				: TraceGameText::GetDefaultDocumentPath();

			FString Error;
			if (!TraceGameText::WriteDocument(Path, Error))
			{
				UE_LOG(LogTraceGame, Error, TEXT("[GameText] dump failed: %s"), *Error);
				return;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] wrote %d string(s) to %s"),
				TraceGameText::GetRegisteredCount(), *Path);
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] REMEMBER: a string registers the first time it is DRAWN, so this file "
				     "holds the screens this session has actually shown. Visit the menus, the character "
				     "select and a round, then dump again to pick up the rest."));
		}));

	FAutoConsoleCommand CmdReload(
		TEXT("Trace.Text.Reload"),
		TEXT("Re-read the game text document. Takes effect immediately, including on screens already "
		     "on screen."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			TraceGameText::LoadDocument();

			// THE CHARACTER CARDS NEED A SECOND NUDGE, and this is the honest place for it. Their five
			// strings are resolved once into an overlay over the roster (TraceCharacterRoster::All)
			// rather than asked for on every draw, so re-reading the document is not on its own enough
			// to change them. A command called Reload has to reload everything it says it does.
			TraceCharacterRoster::ForceReload();

			const FString Path = TraceGameText::GetLoadedPath();
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] reloaded. %d of %d registered string(s) are using the document%s%s"),
				TraceGameText::GetOverriddenCount(), TraceGameText::GetRegisteredCount(),
				Path.IsEmpty() ? TEXT("") : TEXT(" at "), Path.IsEmpty() ? TEXT(" (none found)") : *Path);
		}));

	// =============================================================================================
	// Trace.Text.SelfTest — THE SAFETY RULE, DRIVEN RATHER THAN DESCRIBED.
	//
	// Trace.Text.Verify can only report on keys the session has actually DRAWN, which makes it a poor
	// instrument for the one property that must never regress: that an edit which invents or deletes
	// a slot is refused. A headless run proved that the hard way — a deliberately broken override was
	// planted in the document and Verify said "PASS", correctly, because the HUD string it belonged to
	// never appeared on screen and so was never validated. A check that cannot see the failure is not
	// evidence about it.
	//
	// So this drives the shipped rule directly, over a table of edits an owner might really make, and
	// asserts the verdict for each. It needs no world, no pawn and no screen.
	// =============================================================================================
	FAutoConsoleCommand CmdSelfTest(
		TEXT("Trace.Text.SelfTest"),
		TEXT("Prove the game text document's safety rule: which edits are accepted and which are "
		     "refused. No world needed."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			struct FCase
			{
				const TCHAR* Label;
				const TCHAR* Default;
				const TCHAR* Edit;
				bool bShouldBeAccepted;
			};

			static const FCase Cases[] =
			{
				{ TEXT("plain reword"),                TEXT("PLAY"),            TEXT("START GAME"),          true  },
				{ TEXT("reword around a slot"),        TEXT("RESPAWN IN {0}"),  TEXT("BACK IN {0} SECONDS"), true  },
				{ TEXT("reorder two slots"),           TEXT("{0} KILLED {1}"),  TEXT("{1} WAS KILLED BY {0}"), true },
				{ TEXT("invent a slot the code will not fill"),
				                                       TEXT("HOLD [{0}]"),      TEXT("HOLD [{0}] FOR {1}S"), false },
				{ TEXT("delete a slot the code fills"),TEXT("RESPAWN IN {0}"),  TEXT("RESPAWNING"),          false },
				{ TEXT("add a slot to a plain string"),TEXT("PLAY"),            TEXT("PLAY {0}"),            false },
				{ TEXT("braces that are not a slot"),  TEXT("SET {MODE}"),      TEXT("CHOOSE {MODE}"),       true  },

				// "KEY =" IS HOW A LINE IS REMOVED, and it has to work on a line with a number in it
				// too. The rule used to refuse the second of these ("expected {0}, found (none)"),
				// which put the built-in wording straight back: blanking WEAPON_DRAWING or
				// CROSSHAIR_THROW_READY_IN was impossible from the document.
				{ TEXT("remove a plain line (empty)"), TEXT("PLAY ALSO HOSTS"), TEXT(""),                    true  },
				{ TEXT("remove a line with a slot (empty)"),
				                                       TEXT("{0}  DRAWING"),    TEXT(""),                    true  },
			};

			int32 Failures = 0;
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] ===== safety rule: which edits are allowed ====="));

			for (const FCase& Case : Cases)
			{
				// THE SHIPPED RULE, not a copy of it: ApplyDocumentTo accepts an override exactly when
				// the two slot signatures are equal — or the edit is empty, which removes the line —
				// and this asks the same function (AcceptsOverride) the same question.
				const FString Want = TraceGameText::ExtractSlots(Case.Default);
				const FString Got = TraceGameText::ExtractSlots(Case.Edit);
				FString RefusalReason;
				const bool bAccepted = TraceGameText::AcceptsOverride(Case.Default, Case.Edit, RefusalReason);

				const bool bPass = (bAccepted == Case.bShouldBeAccepted);
				Failures += bPass ? 0 : 1;

				UE_LOG(LogTraceGame, Display,
					TEXT("[GameText]   %-4s %-38s \"%s\" -> \"%s\"  slots %s vs %s  = %s"),
					bPass ? TEXT("ok") : TEXT("FAIL"), Case.Label, Case.Default, Case.Edit,
					Want.IsEmpty() ? TEXT("(none)") : *Want,
					Got.IsEmpty() ? TEXT("(none)") : *Got,
					bAccepted ? TEXT("ACCEPTED") : TEXT("REFUSED, built-in wording used"));
			}

			// AND THE OTHER HALF OF THE SAFETY CLAIM: that a slot with no argument is an ugly label
			// rather than a crash. This is the property that made braces the right choice over printf
			// in the first place, so it is checked rather than asserted.
			const FString Underfilled = FString::Format(TEXT("HOLD [{0}] FOR {1}S"),
				FStringFormatOrderedArguments{ FStringFormatArg(FString(TEXT("E"))) });
			const bool bSurvived = Underfilled.Contains(TEXT("HOLD [E]"));
			Failures += bSurvived ? 0 : 1;
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText]   %-4s %-38s a slot with no argument renders as \"%s\" — no crash, no "
				     "garbage, just a visible blank."),
				bSurvived ? TEXT("ok") : TEXT("FAIL"), TEXT("missing argument is survivable"), *Underfilled);

			// ---- THE DOCUMENT ROUND TRIP: what a line shows, and what a dump writes back ------------
			//
			// The table above proves the RULE. This proves the two things the co-developer's cuts
			// actually ran into: a line has to be removable from the document (an empty value, even
			// on a line with a {0}), and regenerating the document must not change a line he wrote —
			// not a trailing space, not a refused edit, and not an empty value.
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] ===== the document: removing a line, and a dump keeping every edit ====="));
			TArray<FString> RoundTripLines;
			const bool bRoundTripOk = TraceGameText::SelfTestDocument(RoundTripLines);
			for (const FString& ReportLine : RoundTripLines)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[GameText]   %s"), *ReportLine);
			}
			Failures += bRoundTripOk ? 0 : 1;

			// ---- P11: the text layout's fast path against the algorithm it replaced ---------------
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] ===== text layout: the fast path against the pre-P11 algorithm ====="));
			TArray<FString> LayoutLines;
			const bool bLayoutOk = TraceText::SelfTestLayout(LayoutLines);
			for (const FString& ReportLine : LayoutLines)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[GameText]   %s"), *ReportLine);
			}
			Failures += bLayoutOk ? 0 : 1;

			UE_LOG(LogTraceGame, Display, TEXT("[GameText] ===== %s ====="),
				(Failures == 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
		}));

	FAutoConsoleCommand CmdVerify(
		TEXT("Trace.Text.Verify"),
		TEXT("Check the game text document against the game: unmatched keys, refused placeholder "
		     "edits, and characters the font cannot draw."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			const FString Path = TraceGameText::GetLoadedPath();

			UE_LOG(LogTraceGame, Display, TEXT("[GameText] ===== document check ====="));
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] document: %s"),
				Path.IsEmpty()
					? TEXT("NONE FOUND — every string is using the wording built into the game, which is "
					       "a correct build, not a broken one. Trace.Text.Dump writes one.")
					: *Path);

			TArray<TraceGameText::FEntry> Entries;
			TraceGameText::GetAllEntries(Entries);

			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] %d string(s) registered so far, %d of them using the document's wording."),
				Entries.Num(), TraceGameText::GetOverriddenCount());
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] (a string registers when it is first DRAWN, so this counts the screens "
				     "this session has shown — not the whole game.)"));

			int32 Failures = 0;

			// ---- refused overrides: the placeholder rule, which is the safety one -----------------
			TArray<TPair<FString, FString>> Rejected;
			TraceGameText::GetRejectedOverrides(Rejected);
			if (Rejected.Num() > 0)
			{
				Failures += Rejected.Num();
				UE_LOG(LogTraceGame, Error,
					TEXT("[GameText] %d line(s) were REFUSED and are using the built-in wording:"),
					Rejected.Num());
				for (const TPair<FString, FString>& Pair : Rejected)
				{
					UE_LOG(LogTraceGame, Error, TEXT("[GameText]   %-40s %s"), *Pair.Key, *Pair.Value);
				}
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[GameText]   ok   every line's placeholders match the code."));
			}

			// ---- keys in the document that nothing asked for --------------------------------------
			TArray<FString> Unmatched;
			TraceGameText::GetUnmatchedDocumentKeys(Unmatched);
			if (Unmatched.Num() > 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[GameText] %d line(s) matched no string the game has drawn yet. Either the key is "
					     "misspelled, or that screen has not been opened this session:"),
					Unmatched.Num());
				for (int32 Index = 0; Index < FMath::Min(Unmatched.Num(), 20); ++Index)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[GameText]   %s"), *Unmatched[Index]);
				}
				if (Unmatched.Num() > 20)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[GameText]   ... and %d more"), Unmatched.Num() - 20);
				}
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[GameText]   ok   every line in the document matches a string the game asked for."));
			}

			// ---- strings with NO line in the document, and lines the document removes -------------
			//
			// THE FIRST LIST IS HOW A DELETION IS CAUGHT. Deleting a line does not remove its text —
			// the game falls back to the wording built into it — and that is how twenty lines the
			// co-developer deleted to get rid of them all stayed on screen, unchanged, with nothing
			// anywhere saying so. Every string listed here is showing words nobody wrote into the
			// document. Keys whose built-in wording is itself empty are left out; they show nothing
			// either way. Informational, not a failure: a brand-new label, or one of the table-driven
			// keys the source scanner cannot see, lands here too until the next dump.
			{
				TArray<const TraceGameText::FEntry*> KeysWithoutLine;
				TArray<const TraceGameText::FEntry*> KeysRemovedByDocument;
				for (const TraceGameText::FEntry& Entry : Entries)
				{
					if (Entry.DefaultText.IsEmpty())
					{
						continue;
					}
					if (!Entry.bInDocument)
					{
						KeysWithoutLine.Add(&Entry);
					}
					else if (Entry.bOverridden && Entry.Text.IsEmpty())
					{
						KeysRemovedByDocument.Add(&Entry);
					}
				}

				if (KeysWithoutLine.Num() > 0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[GameText] %d string(s) on screen have NO line in the document, so they show the "
						     "wording built into the game. A line that was DELETED comes back like this - to "
						     "remove one, keep the line and leave nothing after the \"=\":"),
						KeysWithoutLine.Num());
					for (int32 Index = 0; Index < FMath::Min(KeysWithoutLine.Num(), 30); ++Index)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[GameText]   %-40s \"%s\""),
							*KeysWithoutLine[Index]->Key, *KeysWithoutLine[Index]->DefaultText);
					}
					if (KeysWithoutLine.Num() > 30)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[GameText]   ... and %d more"), KeysWithoutLine.Num() - 30);
					}
				}
				else
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[GameText]   ok   every string on screen has a line in the document."));
				}

				if (KeysRemovedByDocument.Num() > 0)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[GameText] %d line(s) are removed by the document (left empty) and show nothing:"),
						KeysRemovedByDocument.Num());
					for (const TraceGameText::FEntry* RemovedEntry : KeysRemovedByDocument)
					{
						UE_LOG(LogTraceGame, Display, TEXT("[GameText]   %s"), *RemovedEntry->Key);
					}
				}
			}

			// ---- characters the font cannot draw in the screen's own typeface ---------------------
			//
			// TWO TIERS, because they are two different problems. A character the drawing face lacks
			// still appears — the wider fallback face draws it — but in a different typeface, which on
			// a title in Sofachrome is visible and on a body line usually is not. A character NEITHER
			// face has becomes a literal "?" and is a defect anybody would notice.
			int32 FallbackCount = 0;
			int32 SubstitutedCount = 0;
			for (const TraceGameText::FEntry& Entry : Entries)
			{
				TCHAR Worst = TEXT('\0');
				const ECharTier Tier = ClassifyString(Entry.Text, Worst);
				if (Tier == ECharTier::Substituted)
				{
					++SubstitutedCount;
					++Failures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[GameText]   %-40s contains U+%04X, which NO font in the build can draw. It "
						     "will appear as \"?\"."),
						*Entry.Key, static_cast<uint32>(Worst));
				}
				else if (Tier == ECharTier::Fallback)
				{
					++FallbackCount;
					UE_LOG(LogTraceGame, Warning,
						TEXT("[GameText]   %-40s contains U+%04X, which draws in the FALLBACK typeface "
						     "rather than the screen's own. Legible, but it will not match the letters "
						     "around it. An ordinary keyboard character avoids this."),
						*Entry.Key, static_cast<uint32>(Worst));
				}
			}
			if (FallbackCount == 0 && SubstitutedCount == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[GameText]   ok   every string draws in the screen's own typeface."));
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] ===== %s ====="),
				(Failures == 0)
					? TEXT("PASS — the document and the game agree")
					: TEXT("*** PROBLEMS FOUND *** — see the lines above"));
		}));
}

// =================================================================================================
// P11 — Trace.Text.Bench [passes]: what one string costs, lookup and measurement, in nanoseconds.
//
// THE NUMBERS THE PERFORMANCE PASS QUOTES, and the check that it changed nothing on screen. Every
// Canvas screen asks the document for its words and TraceText for their widths on every frame, so
// the per-call cost of those two is multiplied by a few hundred calls a frame. This times them over
// REGISTERED keys (so it registers nothing) and a fixed corpus of HUD-shaped strings, and prints a
// CHECKSUM of every width and every glyph quad: two builds whose checksums agree lay the corpus out
// to the same pixel, which is what "faster, and nothing moved" has to mean.
//
// Named, not anonymous: this module is a unity build (Scripts/check-jumbo-build-collisions.py).
// =================================================================================================
namespace TraceGameTextBenchFile
{
	/** The corpus: short labels, clocks, a joined address, a status chip, two lines, a Latin-1 name. */
	static const TCHAR* const Corpus[] =
	{
		TEXT("DASH"),
		TEXT("PARRY"),
		TEXT("00:34"),
		TEXT("RESPAWN IN 3"),
		TEXT("HOSTING  192.168.1.20:7777"),
		TEXT("VULNERABLE  x3  +35%"),
		TEXT("KEEPING YOUR TEAM IN 9"),
		TEXT("PLAYER ONE > PLAYER TWO"),
		TEXT("FIRST LINE\nSECOND, LONGER LINE"),
		TEXT("TRAILING BREAK\n"),
		TEXT("\n"),
		TEXT("BJÖRN  ÉLAN"),
		TEXT("TAB\tSEPARATED"),
	};

	static double NsPer(uint64 Cycles, int64 Calls)
	{
		return (Calls > 0) ? (FPlatformTime::ToSeconds64(Cycles) * 1.0e9 / static_cast<double>(Calls)) : 0.0;
	}

	static void Run(const TArray<FString>& Args)
	{
		const int32 Passes = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 1000000) : 2000;

		// ---- 1. The document lookup ------------------------------------------------------------
		//
		// TraceGameText::Get with the key and default a call site passes: exactly what TRACE_TEXT
		// expanded to before P11, and what a table-driven call (a data row carrying its key) still
		// does today. Registered keys only, so the bench adds nothing to a later Trace.Text.Dump.
		TArray<TraceGameText::FEntry> Entries;
		TraceGameText::GetAllEntries(Entries);
		const int32 KeyCount = FMath::Min(64, Entries.Num());

		uint64 LookupCycles = 0;
		int64 LookupCalls = 0;
		int32 LookupSink = 0;
		if (KeyCount > 0)
		{
			for (int32 Index = 0; Index < KeyCount; ++Index)
			{
				LookupSink += TraceGameText::Get(*Entries[Index].Key, *Entries[Index].DefaultText).Len();
			}
			const uint64 Start = FPlatformTime::Cycles64();
			for (int32 Pass = 0; Pass < Passes; ++Pass)
			{
				for (int32 Index = 0; Index < KeyCount; ++Index)
				{
					LookupSink += TraceGameText::Get(*Entries[Index].Key, *Entries[Index].DefaultText).Len();
				}
			}
			LookupCycles = FPlatformTime::Cycles64() - Start;
			LookupCalls = static_cast<int64>(Passes) * KeyCount;
		}

		// The same keys through a remembered call site: what every TRACE_TEXT line does since P11
		// (TraceGameText::FCallSite). The first pass fills the sites; the timed passes are the steady
		// state a HUD frame sees.
		uint64 SiteCycles = 0;
		if (KeyCount > 0)
		{
			TArray<TraceGameText::FCallSite> Sites;
			Sites.SetNum(KeyCount);
			for (int32 Index = 0; Index < KeyCount; ++Index)
			{
				LookupSink += TraceGameText::GetAt(Sites[Index], *Entries[Index].Key, *Entries[Index].DefaultText).Len();
			}
			const uint64 SiteStart = FPlatformTime::Cycles64();
			for (int32 Pass = 0; Pass < Passes; ++Pass)
			{
				for (int32 Index = 0; Index < KeyCount; ++Index)
				{
					LookupSink += TraceGameText::GetAt(Sites[Index], *Entries[Index].Key, *Entries[Index].DefaultText).Len();
				}
			}
			SiteCycles = FPlatformTime::Cycles64() - SiteStart;
		}

		// ---- 2. Measuring and laying out the corpus, in the HUD's face ----------------------------
		TraceText::FStyle Style(20.f, FLinearColor::White, ETraceTextWeight::Hud);
		Style.HAlign = TraceText::EHAlign::Center;
		TraceText::FStyle Tabular = Style;
		Tabular.bTabularDigits = true;
		Tabular.Tracking = 1.5f;
		Tabular.HAlign = TraceText::EHAlign::Right;

		const int32 CorpusCount = UE_ARRAY_COUNT(Corpus);
		TArray<FString> Strings;
		for (int32 Index = 0; Index < CorpusCount; ++Index)
		{
			Strings.Add(Corpus[Index]);
		}

		// The checksum first, from one untimed pass: every width, every quad's position, size, UVs and
		// sheet, for both styles. Printed with the timings so a before/after pair can be diffed.
		uint32 Checksum = 0;
		float WidthSum = 0.f;
		int32 QuadCount = 0;
		TArray<TraceText::FGlyphQuad> Quads;
		for (const TraceText::FStyle* Each : { &Style, &Tabular })
		{
			for (const FString& Text : Strings)
			{
				const float Width = TraceText::MeasureWidth(Text, *Each);
				const FVector2f Block = TraceText::Measure(Text, *Each);
				WidthSum += Width;
				Checksum = HashCombine(Checksum, GetTypeHash(Width));
				Checksum = HashCombine(Checksum, GetTypeHash(Block.X));
				Checksum = HashCombine(Checksum, GetTypeHash(Block.Y));
				const bool bLaidOut = TraceText::LayoutString(Text, *Each, Quads);
				Checksum = HashCombine(Checksum, GetTypeHash(bLaidOut));
				for (const TraceText::FGlyphQuad& Quad : Quads)
				{
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.Pos.X));
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.Pos.Y));
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.Size.X));
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.Size.Y));
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.UVMin.X));
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.UVMin.Y));
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.UVSize.X));
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.UVSize.Y));
					Checksum = HashCombine(Checksum, GetTypeHash(Quad.bFallback));
				}
				QuadCount += Quads.Num();
			}
		}

		float MeasureSink = 0.f;
		uint64 Start = FPlatformTime::Cycles64();
		for (int32 Pass = 0; Pass < Passes; ++Pass)
		{
			for (const FString& Text : Strings)
			{
				MeasureSink += TraceText::MeasureWidth(Text, Style);
			}
		}
		const uint64 MeasureCycles = FPlatformTime::Cycles64() - Start;

		int32 LayoutSink = 0;
		Start = FPlatformTime::Cycles64();
		for (int32 Pass = 0; Pass < Passes; ++Pass)
		{
			for (const FString& Text : Strings)
			{
				TArray<TraceText::FGlyphQuad> PerCall;   // what the Canvas blitter did per string
				TraceText::LayoutString(Text, Style, PerCall);
				LayoutSink += PerCall.Num();
			}
		}
		const uint64 LayoutCycles = FPlatformTime::Cycles64() - Start;
		const int64 CorpusCalls = static_cast<int64>(Passes) * CorpusCount;

		UE_LOG(LogTraceGame, Display, TEXT("[TextBench] ===== %d pass(es); atlas %s ====="),
			Passes, TraceText::IsAtlasActive() ? TEXT("live") : TEXT("DOWN (Lato fallback)"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[TextBench]   document lookup, key+default (%d registered keys): %8.1f ns/call"),
			KeyCount, NsPer(LookupCycles, LookupCalls));
		UE_LOG(LogTraceGame, Display,
			TEXT("[TextBench]   document lookup through a TRACE_TEXT call site (P11):  %8.1f ns/call"),
			NsPer(SiteCycles, LookupCalls));
		UE_LOG(LogTraceGame, Display,
			TEXT("[TextBench]   MeasureWidth over the %d-string corpus:              %8.1f ns/call"),
			CorpusCount, NsPer(MeasureCycles, CorpusCalls));
		UE_LOG(LogTraceGame, Display,
			TEXT("[TextBench]   LayoutString over the corpus (fresh quad array):     %8.1f ns/call"),
			NsPer(LayoutCycles, CorpusCalls));
		UE_LOG(LogTraceGame, Display,
			TEXT("[TextBench]   CHECKSUM %08x  (%d quads, width sum %.4f) - equal checksums lay the corpus out identically"),
			Checksum, QuadCount, WidthSum);
		UE_LOG(LogTraceGame, Verbose, TEXT("[TextBench] sinks %d %.1f %d"), LookupSink, MeasureSink, LayoutSink);
	}

	static FAutoConsoleCommand CmdBench(
		TEXT("Trace.Text.Bench"),
		TEXT("Trace.Text.Bench [passes]: nanoseconds per document lookup, per MeasureWidth and per "
		     "LayoutString, plus a layout checksum for before/after comparison. Registers nothing. Dev only."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}

#endif   // !UE_BUILD_SHIPPING
