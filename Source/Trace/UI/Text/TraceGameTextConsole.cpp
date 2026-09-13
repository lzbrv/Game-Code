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
			};

			int32 Failures = 0;
			UE_LOG(LogTraceGame, Display,
				TEXT("[GameText] ===== safety rule: which edits are allowed ====="));

			for (const FCase& Case : Cases)
			{
				// THE SHIPPED RULE, not a copy of it: ApplyDocumentTo accepts an override exactly when
				// the two slot signatures are equal, and this asks the same function the same question.
				const FString Want = TraceGameText::ExtractSlots(Case.Default);
				const FString Got = TraceGameText::ExtractSlots(Case.Edit);
				const bool bAccepted = (Want == Got);

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

#endif   // !UE_BUILD_SHIPPING
