#include "UI/Text/TraceGameText.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Trace.h"

namespace TraceGameText
{
namespace TraceGameTextFile
{
	/**
	 * STABLE ADDRESSES, AND THAT IS NOT A STYLE CHOICE.
	 *
	 * Get() hands back `const FString&` into an entry, and entries are created LAZILY — the first
	 * time each key is drawn. If the entries lived in a TMap's value storage, registering a new key
	 * would eventually rehash the map and move every value, invalidating a reference a caller was
	 * still holding. That is a use-after-free that would show up as one garbled label months from
	 * now, on whichever screen happened to register the key that tipped the map over its load factor.
	 *
	 * So the entries live in an array of unique pointers (whose pointees never move) and the map
	 * holds pointers into it. A reload rewrites FEntry::Text in place, which keeps the FString object
	 * and therefore keeps every outstanding reference valid.
	 */
	TArray<TUniquePtr<FEntry>> GEntries;
	TMap<FString, FEntry*> GByKey;

	/** Parsed document: normalised key -> raw value. Kept so a later Get() can apply it. */
	TMap<FString, FString> GDocument;

	/**
	 * The same lines EXACTLY AS TYPED — only the whitespace between "=" and the first character is
	 * dropped, and nothing at the end. GDocument's values are trimmed because trailing spaces are
	 * never meant to reach the screen; this copy exists only so Trace.Text.Dump can write a line back
	 * without changing a byte of it. A dump that "tidied" the owner's file would be a diff in every
	 * line he ever touched, and a reason not to trust the tool.
	 */
	TMap<FString, FString> GDocumentAsTyped;

	TArray<FString> GUnmatchedDocumentKeys;
	TArray<TPair<FString, FString>> GRejectedOverrides;

	FString GLoadedPath;
	bool bGLoaded = false;

	/** Set only while SelfTestDocument drives a deliberately refused line, so its log stays clean. */
	bool bGQuietRefusals = false;

	/** Keys are compared case-insensitively so the owner can type one in lower case and be right. */
	FString NormaliseKey(const FString& Key)
	{
		return Key.TrimStartAndEnd().ToUpper();
	}

	/**
	 * "\n" in the document becomes a real line break. THE ONLY ESCAPE THE FORMAT HAS, deliberately:
	 * every additional one is a rule the owner has to know before they can safely type a character,
	 * and the alternative to a rule here is a multi-line syntax, which is worse in a file people edit
	 * by hand. A literal backslash-n is written "\\n".
	 */
	FString ApplyEscapes(const FString& Raw)
	{
		FString Out = Raw;
		Out.ReplaceInline(TEXT("\\\\"), TEXT("\x01"), ESearchCase::CaseSensitive);   // guard real backslashes
		Out.ReplaceInline(TEXT("\\n"), TEXT("\n"), ESearchCase::CaseSensitive);
		Out.ReplaceInline(TEXT("\x01"), TEXT("\\"), ESearchCase::CaseSensitive);
		return Out;
	}

	/** The inverse, for the writer, so a round trip through the document is lossless. */
	FString EscapeForDocument(const FString& Text)
	{
		FString Out = Text;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
		Out.ReplaceInline(TEXT("\r\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
		Out.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
		return Out;
	}

	/**
	 * WHAT A DUMP WRITES TO THE RIGHT OF "=" FOR ONE KEY — the single decision Trace.Text.Dump makes
	 * per line, pulled out so Trace.Text.SelfTest can check the same function the writer calls.
	 *
	 * A line the document already has is written back AS TYPED: not from Entry.Text, which is
	 * trimmed, and which for a REFUSED line is the built-in wording rather than the owner's sentence.
	 * Only a key with no line at all is written from the code — which is also what a DELETED line
	 * gets, and why deleting a line never removed anything.
	 */
	FString DumpValueFor(const FEntry& Entry)
	{
		if (const FString* AsTyped = GDocumentAsTyped.Find(NormaliseKey(Entry.Key)))
		{
			return *AsTyped;
		}
		return EscapeForDocument(Entry.Text);
	}

	/**
	 * Applies the document to one entry, or refuses and says why.
	 *
	 * THE PLACEHOLDER RULE IS THE WHOLE OF THE SAFETY STORY. Call sites hand these strings to
	 * FString::Printf with positional arguments; a format whose specifiers do not match its
	 * arguments is undefined behaviour. "%s" where the code passes a float reads the float's bit
	 * pattern as a pointer, which is a crash on a good day. So the override's specifiers must match
	 * the default's EXACTLY and in ORDER, and an override that does not is dropped with the default
	 * left in place. Every word around a placeholder is the owner's to rewrite; the placeholders
	 * themselves are the code's.
	 */
	void ApplyDocumentTo(FEntry& Entry)
	{
		Entry.Text = Entry.DefaultText;
		Entry.bOverridden = false;
		Entry.bInDocument = false;

		const FString* Raw = GDocument.Find(NormaliseKey(Entry.Key));
		if (Raw == nullptr)
		{
			// NO LINE MEANS THE BUILT-IN WORDING, NOT "NOTHING". Deleting a line is therefore not how
			// a line is removed — an empty value is (see AcceptsOverride). Trace.Text.Verify lists
			// every key in this state so a deletion that brought a sentence back is visible.
			return;
		}

		Entry.bInDocument = true;
		const FString Candidate = ApplyEscapes(*Raw);

		FString Reason;
		if (!AcceptsOverride(Entry.DefaultText, Candidate, Reason))
		{
			GRejectedOverrides.Emplace(Entry.Key, Reason);

			if (!bGQuietRefusals)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[GameText] REFUSED the document's wording for %s: %s. The shipped wording is being "
					     "used instead. Fix the line and run Trace.Text.Reload."),
					*Entry.Key, *Reason);
			}
			return;
		}

		Entry.Text = Candidate;
		Entry.bOverridden = true;
	}

	FEntry& FindOrRegister(const TCHAR* Key, const TCHAR* DefaultText)
	{
		const FString KeyStr(Key);
		const FString Normalised = NormaliseKey(KeyStr);

		if (FEntry** Existing = GByKey.Find(Normalised))
		{
			return **Existing;
		}

		TUniquePtr<FEntry> Created = MakeUnique<FEntry>();
		Created->Key = KeyStr;
		Created->DefaultText = DefaultText;

		FEntry* Raw = Created.Get();
		GEntries.Add(MoveTemp(Created));
		GByKey.Add(Normalised, Raw);

		ApplyDocumentTo(*Raw);

		// A key that only NOW matched a document line is no longer unmatched. Keys register lazily,
		// so this list is only ever an approximation until every screen has been visited — which is
		// exactly what Trace.Text.Verify says out loud rather than letting a reader assume otherwise.
		GUnmatchedDocumentKeys.RemoveAll([&Normalised](const FString& Unmatched)
		{
			return NormaliseKey(Unmatched) == Normalised;
		});

		return *Raw;
	}

	/** Parses one document into GDocument. Returns the number of entries read. */
	int32 ParseDocument(const TArray<FString>& Lines, const FString& PathForMessages)
	{
		GDocument.Reset();
		GDocumentAsTyped.Reset();

		FString Section;
		int32 Count = 0;

		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			FString Line = Lines[Index].TrimStartAndEnd();
			if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT(";")))
			{
				continue;
			}

			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				Section = Line.Mid(1, Line.Len() - 2).TrimStartAndEnd();
				continue;
			}

			int32 Equals = INDEX_NONE;
			if (!Line.FindChar(TEXT('='), Equals))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[GameText] %s line %d is neither a comment, a [SECTION] nor a KEY = VALUE, so it "
					     "was skipped: \"%s\""),
					*PathForMessages, Index + 1, *Line);
				continue;
			}

			const FString Name = Line.Left(Equals).TrimStartAndEnd();
			// The value keeps its INTERNAL spacing and is trimmed only at the ends. Trailing spaces
			// are invisible in an editor and never intentional; internal ones are the wording.
			const FString Value = Line.Mid(Equals + 1).TrimStartAndEnd();

			if (Name.IsEmpty())
			{
				continue;
			}

			const FString FullKey = Section.IsEmpty() ? Name : (Section + TEXT(".") + Name);
			const FString Normalised = NormaliseKey(FullKey);

			if (GDocument.Contains(Normalised))
			{
				// LAST ONE WINS, and it says so. A duplicate is nearly always a copy-and-paste while
				// rewording, and silently keeping the first would mean the owner's newest edit is the
				// one that does nothing — the most confusing possible outcome.
				UE_LOG(LogTraceGame, Warning,
					TEXT("[GameText] %s line %d repeats the key %s. The LAST one wins; delete the earlier "
					     "line to stop this warning."),
					*PathForMessages, Index + 1, *FullKey);
			}

			GDocument.Add(Normalised, Value);

			// The as-typed copy, cut from the UNTRIMMED line. The trimmed Line above lost any trailing
			// space, which is the whole thing this copy is here to keep.
			{
				FString AsTyped;
				int32 RawEquals = INDEX_NONE;
				if (Lines[Index].FindChar(TEXT('='), RawEquals))
				{
					AsTyped = Lines[Index].Mid(RawEquals + 1).TrimStart();
					AsTyped.RemoveFromEnd(TEXT("\r"));
				}
				GDocumentAsTyped.Add(Normalised, AsTyped);
			}
			++Count;
		}

		return Count;
	}
} // namespace TraceGameTextFile

FString ExtractSlots(const FString& Format)
{
	// Finds every "{N}" and reports the DISTINCT indices, sorted. Sorted rather than in order of
	// appearance because reordering slots is a legitimate rewrite — "{0} KILLED {1}" and "{1} WAS
	// KILLED BY {0}" fill the same two blanks — and a rule that refused the second would be refusing
	// grammar. What must not change is which slots exist, because that is what the code passes.
	//
	// FString::Format also accepts named arguments ("{Name}"); this project passes ordered ones only,
	// and a brace run that is not a plain number is left alone so it cannot be mistaken for a slot.
	TArray<int32> Indices;

	// A single forward scan. The first version of this reached for FString::FindChar and then had to
	// work around it searching from index 0 rather than from the cursor — a loop that walked the
	// string to find the same brace every time. One pass with two positions is both shorter and the
	// thing the code was actually trying to say.
	const int32 Length = Format.Len();
	for (int32 Open = 0; Open < Length; ++Open)
	{
		if (Format[Open] != TEXT('{'))
		{
			continue;
		}

		int32 Close = Open + 1;
		while (Close < Length && Format[Close] != TEXT('}'))
		{
			++Close;
		}
		if (Close >= Length)
		{
			break;   // an unclosed brace: nothing after it can be a slot either
		}

		const FString Inner = Format.Mid(Open + 1, Close - Open - 1);
		if (!Inner.IsEmpty() && Inner.IsNumeric())
		{
			Indices.AddUnique(FCString::Atoi(*Inner));
		}

		Open = Close;   // the for-loop's ++ then steps past the closing brace
	}

	Indices.Sort();

	FString Out;
	for (const int32 Index : Indices)
	{
		Out += FString::Printf(TEXT("{%d}"), Index);
	}
	return Out;
}

bool AcceptsOverride(const FString& DefaultText, const FString& Candidate, FString& OutReason)
{
	OutReason.Reset();

	// EMPTY REMOVES THE LINE, and it is accepted for every key — including one with slots, which the
	// slot rule below would otherwise refuse ("expected {0}, found (none)") and so put the default
	// straight back. That refusal is what made blanking a line with a number in it impossible. It is
	// safe: FString::Format of an empty string is an empty string whatever arguments it is handed,
	// so there is no slot left to misfill.
	if (Candidate.IsEmpty())
	{
		return true;
	}

	const FString WantSlots = ExtractSlots(DefaultText);
	const FString GotSlots = ExtractSlots(Candidate);
	if (WantSlots == GotSlots)
	{
		return true;
	}

	OutReason = FString::Printf(
		TEXT("the slots must be the same ones the game fills: expected %s, found %s"),
		WantSlots.IsEmpty() ? TEXT("(none)") : *WantSlots,
		GotSlots.IsEmpty() ? TEXT("(none)") : *GotSlots);
	return false;
}

FString Format(const TCHAR* Key, const TCHAR* DefaultText, const FStringFormatOrderedArguments& Args)
{
	// FString::Format takes a plain TCHAR* and is therefore safe with a string read from a file —
	// which FString::Printf is not, in this engine version. See the header.
	//
	// A line the document removed stays removed: no format, no arguments, nothing to draw.
	const FString& Pattern = Get(Key, DefaultText);
	if (Pattern.IsEmpty())
	{
		return FString();
	}
	return FString::Format(*Pattern, Args);
}

FString GetDefaultDocumentPath()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::ProjectConfigDir() / TEXT("TraceGameText.ini"));
}

void LoadDocument()
{
	TraceGameTextFile::bGLoaded = true;
	TraceGameTextFile::GLoadedPath.Reset();
	TraceGameTextFile::GUnmatchedDocumentKeys.Reset();
	TraceGameTextFile::GRejectedOverrides.Reset();

	// A LOOSE FILE BESIDE THE GAME WINS. In a packaged build ProjectDir() is a real directory on
	// disk (it holds Binaries/ and Content/) while the Config copy is inside the pak, so this is the
	// path that lets wording be tried during a playtest without a rebuild. Absent in a normal build.
	const FString LoosePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("TraceGameText.ini"));

	const FString Candidates[] = { LoosePath, GetDefaultDocumentPath() };

	TArray<FString> Lines;
	for (const FString& Candidate : Candidates)
	{
		if (IFileManager::Get().FileExists(*Candidate)
			&& FFileHelper::LoadFileToStringArray(Lines, *Candidate))
		{
			TraceGameTextFile::GLoadedPath = Candidate;
			break;
		}
	}

	int32 ParsedCount = 0;
	if (TraceGameTextFile::GLoadedPath.IsEmpty())
	{
		TraceGameTextFile::GDocument.Reset();

		// NOT AN ERROR, AND THE LEVEL SAYS SO. Every key falls back to the wording compiled into the
		// game, which is the wording the build shipped with. A build with no document is a correct
		// build, not a degraded one.
		UE_LOG(LogTraceGame, Log,
			TEXT("[GameText] no document found (looked in %s and %s). Every string is using the wording "
			     "compiled into the game. Trace.Text.Dump writes a document you can edit."),
			*LoosePath, *GetDefaultDocumentPath());
	}
	else
	{
		ParsedCount = TraceGameTextFile::ParseDocument(Lines, TraceGameTextFile::GLoadedPath);
	}

	// Re-apply to everything already registered, so a reload takes effect on screens that are
	// already on screen rather than only on ones opened afterwards.
	for (const TUniquePtr<FEntry>& Entry : TraceGameTextFile::GEntries)
	{
		if (Entry.IsValid())
		{
			TraceGameTextFile::ApplyDocumentTo(*Entry);
		}
	}

	// Document keys that match nothing REGISTERED SO FAR. See GetUnmatchedDocumentKeys.
	for (const TPair<FString, FString>& Pair : TraceGameTextFile::GDocument)
	{
		if (!TraceGameTextFile::GByKey.Contains(Pair.Key))
		{
			TraceGameTextFile::GUnmatchedDocumentKeys.Add(Pair.Key);
		}
	}
	TraceGameTextFile::GUnmatchedDocumentKeys.Sort();

	if (!TraceGameTextFile::GLoadedPath.IsEmpty())
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("[GameText] read %d entr(ies) from %s. %d of %d registered string(s) are using the "
			     "document's wording."),
			ParsedCount, *TraceGameTextFile::GLoadedPath, GetOverriddenCount(), GetRegisteredCount());
	}
}

const FString& Get(const TCHAR* Key, const TCHAR* DefaultText)
{
	if (!TraceGameTextFile::bGLoaded)
	{
		LoadDocument();
	}

	return TraceGameTextFile::FindOrRegister(Key, DefaultText).Text;
}

FString GetLoadedPath()
{
	return TraceGameTextFile::GLoadedPath;
}

int32 GetRegisteredCount()
{
	return TraceGameTextFile::GEntries.Num();
}

int32 GetOverriddenCount()
{
	int32 Count = 0;
	for (const TUniquePtr<FEntry>& Entry : TraceGameTextFile::GEntries)
	{
		if (Entry.IsValid() && Entry->bOverridden)
		{
			++Count;
		}
	}
	return Count;
}

void GetAllEntries(TArray<FEntry>& Out)
{
	Out.Reset();
	Out.Reserve(TraceGameTextFile::GEntries.Num());
	for (const TUniquePtr<FEntry>& Entry : TraceGameTextFile::GEntries)
	{
		if (Entry.IsValid())
		{
			Out.Add(*Entry);
		}
	}
	Out.Sort([](const FEntry& A, const FEntry& B) { return A.Key < B.Key; });
}

void GetUnmatchedDocumentKeys(TArray<FString>& Out)
{
	Out = TraceGameTextFile::GUnmatchedDocumentKeys;
}

void GetRejectedOverrides(TArray<TPair<FString, FString>>& Out)
{
	Out = TraceGameTextFile::GRejectedOverrides;
}

bool WriteDocument(const FString& Path, FString& OutError)
{
	OutError.Reset();

	TArray<FEntry> Entries;
	GetAllEntries(Entries);

	if (Entries.Num() == 0)
	{
		OutError = TEXT("nothing has registered a string yet, so the document would be empty. Open the "
		                "menus and play a round first, then dump.");
		return false;
	}

	// Keys the document has that the code no longer asks for. Kept rather than dropped: deleting
	// somebody's rewritten sentence because a label was renamed is the one unrecoverable thing this
	// writer could do, and a clearly labelled section at the end costs nothing.
	TSet<FString> Registered;
	for (const FEntry& Entry : Entries)
	{
		Registered.Add(TraceGameTextFile::NormaliseKey(Entry.Key));
	}

	TArray<FString> Orphans;
	for (const TPair<FString, FString>& Pair : TraceGameTextFile::GDocument)
	{
		if (!Registered.Contains(Pair.Key))
		{
			Orphans.Add(Pair.Key);
		}
	}
	Orphans.Sort();

	FString Out;
	Out += TEXT("# =============================================================================\n");
	Out += TEXT("#  TRACE - GAME TEXT\n");
	Out += TEXT("#\n");
	Out += TEXT("#  Every word the player reads is in this file. Edit the text on the RIGHT of the\n");
	Out += TEXT("#  \"=\", save, and run the game. That is the whole workflow - nothing to compile.\n");
	Out += TEXT("#\n");
	Out += TEXT("#  THE FOUR RULES\n");
	Out += TEXT("#    1. Leave the KEY (left of the \"=\") alone. It is how the game finds the line.\n");
	Out += TEXT("#    2. Keep every {0}, {1} ... exactly as they are. They are the blanks the game fills\n");
	Out += TEXT("#       in with a name or a number. Reword freely AROUND them, and you may put them\n");
	Out += TEXT("#       in a different order if the sentence reads better that way - but do not add\n");
	Out += TEXT("#       one the game will not fill or delete one it will. A line whose blanks do not\n");
	Out += TEXT("#       match is ignored, and the built-in wording is used instead.\n");
	Out += TEXT("#    3. Write \\n where you want a line break.\n");
	Out += TEXT("#    4. Stick to ordinary keyboard characters. Run Trace.Text.Verify to check.\n");
	Out += TEXT("#\n");
	Out += TEXT("#  TO REMOVE A LINE FROM THE SCREEN, leave the right side empty:\n");
	Out += TEXT("#       TAGLINE                            =\n");
	Out += TEXT("#  The game then shows nothing in its place.\n");
	Out += TEXT("#\n");
	Out += TEXT("#  DELETING a line does NOT remove it. It brings back the wording built into the\n");
	Out += TEXT("#  game, and the next regenerate writes that wording back into this file.\n");
	Out += TEXT("#\n");
	Out += TEXT("#  Regenerate with Trace.Text.Dump - it keeps every line you have written exactly\n");
	Out += TEXT("#  as you wrote it and only adds lines for text that is new.\n");
	Out += TEXT("# =============================================================================\n");

	FString CurrentSection;
	for (const FEntry& Entry : Entries)
	{
		FString Section;
		FString Name = Entry.Key;

		int32 LastDot = INDEX_NONE;
		if (Entry.Key.FindLastChar(TEXT('.'), LastDot))
		{
			Section = Entry.Key.Left(LastDot);
			Name = Entry.Key.Mid(LastDot + 1);
		}

		if (Section != CurrentSection)
		{
			CurrentSection = Section;
			Out += TEXT("\n[");
			Out += CurrentSection;
			Out += TEXT("]\n");
		}

		// As typed when the document has the line; from the code only when it does not.
		Out += FString::Printf(TEXT("%-34s = %s\n"), *Name, *TraceGameTextFile::DumpValueFor(Entry));
	}

	if (Orphans.Num() > 0)
	{
		Out += TEXT("\n\n# =============================================================================\n");
		Out += TEXT("#  NOT ASKED FOR BY ANY SCREEN THIS SESSION SHOWED\n");
		Out += TEXT("#\n");
		Out += TEXT("#  These lines were in the document but nothing in the game asked for them. They\n");
		Out += TEXT("#  are kept here rather than deleted, because they may be your words. Either the\n");
		Out += TEXT("#  key was renamed, or the screen they belong to had not been opened when this\n");
		Out += TEXT("#  file was written. Delete them once you are sure.\n");
		Out += TEXT("# =============================================================================\n");
		// A BARE [] RESETS THE SECTION. These are FULL keys; under a named section the loader would
		// prefix them with it and they would stop matching, so parking a line would break the string
		// it was trying to preserve.
		Out += TEXT("\n[]\n");
		for (const FString& Orphan : Orphans)
		{
			const FString* Value = TraceGameTextFile::GDocumentAsTyped.Find(Orphan);
			Out += FString::Printf(TEXT("%-34s = %s\n"), *Orphan, Value != nullptr ? **Value : TEXT(""));
		}
	}

	if (!FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("could not write %s"), *Path);
		return false;
	}

	return true;
}

#if !UE_BUILD_SHIPPING
bool SelfTestDocument(TArray<FString>& OutLines)
{
	OutLines.Reset();

	// THE LIVE DOCUMENT IS PUT BACK EXACTLY AS IT WAS. This runs the shipped parser, which starts by
	// resetting the parsed maps, so they are copied out first and copied back last. The registered
	// entries are never touched: every probe below is a SCRATCH entry, applied and thrown away, so a
	// later Trace.Text.Dump cannot pick up a SELFTEST key.
	const TMap<FString, FString> SavedDocument = TraceGameTextFile::GDocument;
	const TMap<FString, FString> SavedAsTyped = TraceGameTextFile::GDocumentAsTyped;
	const TArray<TPair<FString, FString>> SavedRejected = TraceGameTextFile::GRejectedOverrides;

	// Written the way a person writes them: padded keys, a bare "=", and one line with a space
	// after the last word — copied from the co-developer's BANNER_PARRIED, which has exactly that.
	const TArray<FString> SelfTestLines =
	{
		TEXT("[SELFTEST]"),
		TEXT("REMOVED_PLAIN                      ="),
		TEXT("REMOVED_WITH_SLOT                  = "),
		TEXT("TRAILING_SPACE                     = PARRIED {0} "),
		TEXT("REFUSED_EDIT                       = RESPAWNING"),
	};
	TraceGameTextFile::ParseDocument(SelfTestLines, TEXT("(self-test document)"));

	struct FProbe
	{
		const TCHAR* Key;
		const TCHAR* Default;
		const TCHAR* WantShown;     // what Get() would hand the screen
		const TCHAR* WantWritten;   // what a dump writes to the right of "=" for this key
		const TCHAR* Meaning;
	};
	static const FProbe Probes[] =
	{
		{ TEXT("SELFTEST.REMOVED_PLAIN"), TEXT("PLAY ALSO HOSTS - EVERY MATCH IS JOINABLE"),
		  TEXT(""), TEXT(""), TEXT("an empty value removes the line") },
		{ TEXT("SELFTEST.REMOVED_WITH_SLOT"), TEXT("{0}  DRAWING"),
		  TEXT(""), TEXT(""), TEXT("...including a line with a {0} in it") },
		{ TEXT("SELFTEST.TRAILING_SPACE"), TEXT("PARRIED - {0} DASHED YOUR TRACE"),
		  TEXT("PARRIED {0}"), TEXT("PARRIED {0} "), TEXT("shown trimmed, written back byte for byte") },
		{ TEXT("SELFTEST.REFUSED_EDIT"), TEXT("RESPAWN IN {0}"),
		  TEXT("RESPAWN IN {0}"), TEXT("RESPAWNING"), TEXT("a refused edit is kept in the file, not overwritten") },
		{ TEXT("SELFTEST.DELETED_LINE"), TEXT("THE BUILT-IN WORDING"),
		  TEXT("THE BUILT-IN WORDING"), TEXT("THE BUILT-IN WORDING"),
		  TEXT("a DELETED line brings the built-in wording back") },
	};

	bool bAllPassed = true;
	TraceGameTextFile::bGQuietRefusals = true;
	for (const FProbe& Probe : Probes)
	{
		FEntry Scratch;
		Scratch.Key = Probe.Key;
		Scratch.DefaultText = Probe.Default;
		TraceGameTextFile::ApplyDocumentTo(Scratch);

		// THE WRITER'S OWN DECISION, not a re-derivation of it: WriteDocument calls DumpValueFor too.
		const FString Written = TraceGameTextFile::DumpValueFor(Scratch);
		const bool bShownOk = Scratch.Text.Equals(Probe.WantShown, ESearchCase::CaseSensitive);
		const bool bWrittenOk = Written.Equals(Probe.WantWritten, ESearchCase::CaseSensitive);
		const bool bPass = bShownOk && bWrittenOk;
		bAllPassed = bAllPassed && bPass;

		OutLines.Add(FString::Printf(TEXT("%-4s %-52s shows \"%s\", dump writes \"%s\""),
			bPass ? TEXT("ok") : TEXT("FAIL"), Probe.Meaning, *Scratch.Text, *Written));
	}
	TraceGameTextFile::bGQuietRefusals = false;

	TraceGameTextFile::GDocument = SavedDocument;
	TraceGameTextFile::GDocumentAsTyped = SavedAsTyped;
	TraceGameTextFile::GRejectedOverrides = SavedRejected;

	return bAllPassed;
}
#endif   // !UE_BUILD_SHIPPING

} // namespace TraceGameText
