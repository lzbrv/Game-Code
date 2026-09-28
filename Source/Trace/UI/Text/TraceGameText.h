// Trace — THE GAME TEXT DOCUMENT.
//
// =================================================================================================
// WHAT THIS IS
// =================================================================================================
// One editable file, Config/TraceGameText.ini, holding every word the player reads. Edit it, run the
// game, and the new wording is on screen. Nothing to compile, no editor, no asset to open.
//
// The owner's request, verbatim: "please make some sort of document that is connected to all the
// written text in the game (menus, character descriptions, etc), so that we can go in and easily
// edit it all to say what we would like and then when we run the game automatically use what we
// have written".
//
// =================================================================================================
// THE ONE DESIGN DECISION EVERYTHING ELSE FOLLOWS FROM: THE DEFAULT LIVES AT THE CALL SITE
// =================================================================================================
// A call site does not ask the document for a string. It asks for a string AND says what the words
// are if the document has nothing to say:
//
//     TRACE_TEXT("MENU.PLAY", "PLAY")
//
// That one shape buys four properties that a plain lookup table cannot have:
//
//   NOTHING GOES BLANK BY ACCIDENT. Delete the document, delete one line of it, typo a key, ship a
//   build with no document at all — every one of those falls back to the words in the code, which
//   are the words the game shipped with.
//
//   THE ONE WAY TO SHOW NOTHING IS TO ASK FOR IT: "KEY =" with nothing after the equals sign. That
//   is the supported way to REMOVE a line, and it is accepted for every key, including one with
//   {0} slots (FString::Format of an empty string is an empty string). Deleting the line does NOT
//   remove it — it brings the built-in wording back, which is exactly how the co-developer's first
//   twenty cuts came to change nothing on screen. So a caller that draws chrome AROUND a string (a
//   pill behind a banner, a note row, a status chip, a footer band) must skip that chrome when the
//   string is empty, and the shared helpers that do most of this drawing already do.
//
//   THE DOCUMENT IS GENERATED, NOT MAINTAINED BY HAND. Every call registers its key and its default,
//   so the game knows the complete set. Trace.Text.Dump writes the file out from that set, which
//   means the document cannot drift out of sync with the code and nobody has to remember to add a
//   line when they add a label. Regenerating it preserves whatever the owner has already edited.
//
//   IT LANDS INCREMENTALLY. Converting a call site is a one-line change with no counterpart edit
//   anywhere else. A half-converted build is a working build; the unconverted half simply is not
//   editable yet. There is no flag day.
//
//   IT DOCUMENTS ITSELF. The default beside the key is the string's own definition, so a reader of
//   the code sees the words without opening the document, and a reader of the document sees a key
//   that says where the words appear.
//
// =================================================================================================
// WHAT THE DOCUMENT LOOKS LIKE
// =================================================================================================
//     # Lines starting with # are comments and are ignored.
//     # Edit only what is to the RIGHT of the "=". Leave the key on the left alone.
//
//     [MENU]
//     PLAY  = PLAY
//     JOIN  = JOIN
//
//     [CHARACTER.ROCCO]
//     NAME            = ROCCO
//     ACTIVATED.NAME  = RIPPLE
//     ACTIVATED.DESC  = DASH ON ITS OWN COOLDOWN, LEAVING A RIPPLE FOR 4S.\nANYONE CAN RIDE IT.
//
// A key is "SECTION.NAME". Sections are a convenience for reading; the key is the whole thing.
// "\n" in a value becomes a line break, which is the one escape the format has.
//
// =================================================================================================
// THE TWO THINGS A BAD EDIT COULD DO, AND WHY IT CANNOT DO THEM
// =================================================================================================
// PLACEHOLDERS. Some strings have a slot the game fills in — "RESPAWN IN {0}". They are written
// with NUMBERED BRACES rather than printf's %s and %.0f, and that is forced rather than chosen:
//
//     UE 5.8's FString::Printf takes UE::Core::TCheckedFormatString, whose ONLY constructor is
//     `template<int32 N> consteval TCheckedFormatStringPrivate(const CharType (&Fmt)[N])` — a
//     reference to a COMPILE-TIME ARRAY (Engine/Source/Runtime/Core/Public/String/FormatStringSan.h).
//     A TCHAR* taken out of a string that was read from a file at runtime cannot bind to it. The
//     obvious implementation of this whole feature — read a format string from the document, hand it
//     to Printf — DOES NOT COMPILE in this engine.
//
// That is a gift rather than an obstacle. FString::Format(const TCHAR*, FStringFormatOrderedArguments)
// does take a runtime pointer, it is what the engine itself uses for runtime-composed text, and it
// cannot read an argument that was never passed: a slot with no argument renders as the literal
// "{0}" instead of dereferencing whatever happened to be on the stack. The failure mode of a bad
// edit is therefore an ugly label, never a crash — which is exactly the trade this document wants.
//
// The numbers also read better in a file a non-programmer is editing: "{0}" is visibly a slot, where
// "%.0f" is line noise you have to be told not to touch. Any precision or width stays in the CODE,
// where it is a compile-time literal and still checked:
//
//     TRACE_TEXTF("HUD.RESPAWN_IN", "RESPAWN IN {0}", { FString::Printf(TEXT("%.0f"), Seconds) })
//
// An override whose set of slots does not match the default's is REFUSED and the default is used,
// with an error naming the key. The owner can rewrite every word around a slot and can reorder the
// slots; they cannot invent one the code will not fill or delete one it will. The single exception
// is the EMPTY override, which removes the line outright and fills nothing — see AcceptsOverride.
//
// CHARACTERS. The HUD and menus draw through TraceText's bitmap atlas. Its drawing faces carry
// ASCII 32..126 and nothing else; a wider fallback face covers Latin-1 and the common typographic
// punctuation, and anything outside BOTH is drawn as "?". Neither is a crash and neither is blank,
// so a stray character is a cosmetic defect rather than a failure — but it is one nobody would think
// to look for, so Trace.Text.Verify reports it: characters outside the drawing face by name (they
// render in the fallback typeface, which looks subtly wrong next to the rest of a line) and
// characters outside the fallback face as errors (they render as "?").
//
// =================================================================================================
// WHERE THE FILE LIVES, MEASURED RATHER THAN ASSUMED
// =================================================================================================
// Config/TraceGameText.ini in the repository, which is where the owner edits it.
//
// In a PACKAGED build the project's Config directory is staged INSIDE the pak — verified by listing
// Trace-Mac.pak, which carries Trace/Config/DefaultGame.ini and its three siblings — so the document
// ships with the game and a playtester gets whatever wording the build was made with. It is read
// through IFileManager, which resolves through the mounted pak, so the same code path works in the
// editor, in a listen server and in a shipped .app.
//
// A LOOSE FILE BESIDE THE BINARY WINS IF PRESENT. FPaths::ProjectDir() is a real directory on disk
// even in a packaged build (it holds Binaries/ and Content/), so dropping TraceGameText.ini there
// overrides the packaged copy without a rebuild. That is the path for "try different wording during
// a playtest"; it is checked first and is absent in every normal build.
//
// =================================================================================================
// COST
// =================================================================================================
// ONE LOOKUP PER CALL SITE PER PROCESS, then a pointer read. The HUD draws on the order of a hundred
// strings a frame, so the per-call cost is multiplied by that every frame, and it used to be real:
// P11 measured TraceGameText::Get at ~210 ns a call (Trace.Text.Bench) — it builds the key as an
// FString, trims and upper-cases it (three heap allocations) and hashes it, on every draw, for a key
// it had already registered. TRACE_TEXT now remembers its answer at the call site (FCallSite, below):
// the first call registers and looks up exactly as before, and every later call returns the same
// stored string. That cannot go stale, and it is the property the whole store was built for: the
// entries never move, and a reload rewrites their words IN PLACE, so the remembered reference shows
// the new wording on the very next draw. Trace.Text.SelfTest proves that on a live entry.
//
// A table-driven call (TraceGameText::Get with a key from a data row) still does the full lookup;
// there is no call site to remember it at.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

namespace TraceGameText
{
	/**
	 * THE ONE LOOKUP. Returns the document's wording for @p Key, or @p DefaultText when the document
	 * has nothing usable for it.
	 *
	 * The result is EMPTY when the document removes the line ("KEY =" with no value). Draw code that
	 * puts anything around the words — a backing pill, a row, a chip — skips it on an empty string, so
	 * a removed line leaves no gap behind.
	 *
	 * Registers (Key, DefaultText) the first time it sees them, which is what lets Trace.Text.Dump
	 * write a complete document. Call it with a literal key and a literal default — see TRACE_TEXT
	 * below, which is the shape every call site should use.
	 *
	 * The reference is stable for the life of the process: values live in a map that is only ever
	 * added to, and a reload rewrites the strings in place rather than rehoming them. Safe to hold
	 * for the duration of a draw call; do not cache it across a Trace.Text.Reload.
	 *
	 * Thread: game thread only. Nothing else draws text.
	 */
	TRACE_API const FString& Get(const TCHAR* Key, const TCHAR* DefaultText);

	/**
	 * Loads (or reloads) the document. Called once automatically on first use, so nothing has to
	 * remember to initialise this; Trace.Text.Reload calls it again.
	 *
	 * Never fails in a way a caller has to handle: a missing or unreadable document leaves every key
	 * on its compiled-in default, which is the shipped wording.
	 */
	TRACE_API void LoadDocument();

	/** Absolute path of the document that was actually read, or empty when none was found. */
	TRACE_API FString GetLoadedPath();

	/** Keys the code has registered so far — i.e. every string that has been asked for this session. */
	TRACE_API int32 GetRegisteredCount();

	/** Of those, how many the document is currently overriding. */
	TRACE_API int32 GetOverriddenCount();

	/**
	 * Writes a complete document to @p Path from everything registered so far, PRESERVING the
	 * wording already in the file for keys it still contains.
	 *
	 * The preservation is the point: regenerating after adding a new label must not throw away an
	 * afternoon of the owner's rewriting. New keys arrive at their compiled-in default; edited keys
	 * keep the edit; keys that no longer exist in the code are kept too, in a clearly labelled
	 * section at the end, because deleting somebody's words is worse than leaving a stale line.
	 *
	 * "KEEP THE EDIT" MEANS BYTE FOR BYTE. A line already in the document is written back exactly as
	 * it was typed — trailing spaces, an empty value, even a line the game is REFUSING because its
	 * {0} blanks do not match (Trace.Text.Verify keeps naming that one until it is fixed; replacing
	 * it with the built-in wording would quietly throw the owner's sentence away).
	 *
	 * @return true when the file was written.
	 */
	TRACE_API bool WriteDocument(const FString& Path, FString& OutError);

	/** The document path this build reads by default (the repo/staged Config copy). */
	TRACE_API FString GetDefaultDocumentPath();

	/** One registered string, for the verifier and the writer. */
	struct FEntry
	{
		FString Key;
		FString DefaultText;
		FString Text;          // what Get() hands out — the override when there is one
		bool bOverridden = false;
		bool bInDocument = false;   // the document has a line for this key (used, or refused)
	};

	/** Every registered key, sorted, for Trace.Text.Verify and Trace.Text.Dump. */
	TRACE_API void GetAllEntries(TArray<FEntry>& Out);

	/**
	 * Document entries that matched no registered key when the document was read.
	 *
	 * Almost always a typo in the document or a key the code has not asked for YET (keys register
	 * lazily, on first use, so a screen nobody has opened this session has not registered anything).
	 * Trace.Text.Verify says which of those two it is by saying how many screens have been visited.
	 */
	TRACE_API void GetUnmatchedDocumentKeys(TArray<FString>& Out);

	/** Overrides refused because their placeholders did not match the code's. Key -> reason. */
	TRACE_API void GetRejectedOverrides(TArray<TPair<FString, FString>>& Out);

	/**
	 * The slot numbers in @p Format, sorted and deduplicated — "{0}{2}" for "HOLD {2} FOR {0}".
	 *
	 * SORTED, because reordering slots is a legitimate edit: "{0} KILLED {1}" and "{1} WAS KILLED BY
	 * {0}" fill the same two blanks and a rule that refused the second would be refusing grammar.
	 * What must not change is WHICH slots exist.
	 *
	 * Exposed because the verifier prints it when it refuses an override, and because a harness
	 * should drive the shipped rule rather than re-implement it.
	 */
	TRACE_API FString ExtractSlots(const FString& Format);

	/**
	 * THE RULE ITSELF: whether the document's @p Candidate may replace @p DefaultText. The loader
	 * asks exactly this, and so does Trace.Text.SelfTest, so the harness drives the shipped rule
	 * rather than a copy of it.
	 *
	 *   - An EMPTY candidate is always accepted. It removes the line, and it cannot misfill a slot
	 *     because it has none to fill.
	 *   - Otherwise the two must carry the same set of {N} slots (any order).
	 *
	 * @param OutReason  on a refusal, why — worded for the owner, not for a programmer.
	 */
	TRACE_API bool AcceptsOverride(const FString& DefaultText, const FString& Candidate, FString& OutReason);

#if !UE_BUILD_SHIPPING
	/**
	 * Trace.Text.SelfTest's second half: feeds a small in-memory document through the SHIPPED parser
	 * and the SHIPPED apply step, and checks what each line would show and what a dump would write
	 * back — an empty value removes a line (with or without a {0}), a trailing space survives a dump,
	 * a refused edit is kept rather than overwritten, a deleted line brings the default back.
	 *
	 * Registers nothing and restores the live document before returning, so it is safe mid-session
	 * and a later Trace.Text.Dump cannot pick up its keys. @return true when every check passed.
	 */
	TRACE_API bool SelfTestDocument(TArray<FString>& OutLines);
#endif

	/**
	 * The formatting half of Get(): looks the key up, then fills its slots from @p Args.
	 *
	 * Runtime-safe by construction — see the PLACEHOLDERS note at the top of this file for why this
	 * cannot be FString::Printf in UE 5.8, and why that is the better outcome.
	 */
	TRACE_API FString Format(const TCHAR* Key, const TCHAR* DefaultText,
		const FStringFormatOrderedArguments& Args);

	/** The formatting half alone, for a pattern already looked up: what TRACE_TEXTF expands to. */
	TRACE_API FString FormatPattern(const FString& Pattern, const FStringFormatOrderedArguments& Args);

	/**
	 * Bumped by every LoadDocument (the first load and each Trace.Text.Reload). A cache of anything
	 * DERIVED from the words — a measured width, a wrapped paragraph — keys on this so a reload
	 * re-derives it. The words themselves need no such key: see FCallSite.
	 */
	TRACE_API uint32 GetGeneration();

	/**
	 * P11 — ONE TRACE_TEXT CALL SITE'S ANSWER, REMEMBERED.
	 *
	 * Every TRACE_TEXT expansion owns one of these as a function-local static. The first call asks
	 * Get() — registering the key exactly as before — and keeps the address of the stored string;
	 * every later call returns that string without touching the map. Safe for the life of the
	 * process because Get()'s reference is (see Get: entries are only ever added, never moved, and a
	 * reload rewrites the words in place). Constant-initialised, so the static costs no guard.
	 *
	 * Game thread only, like Get().
	 */
	struct FCallSite
	{
		const FString* Value = nullptr;
	};

	FORCEINLINE const FString& GetAt(FCallSite& Site, const TCHAR* Key, const TCHAR* DefaultText)
	{
		if (Site.Value == nullptr)
		{
			Site.Value = &Get(Key, DefaultText);
		}
		return *Site.Value;
	}
}

/**
 * THE SHAPE EVERY CALL SITE USES.
 *
 *     TraceCanvasText::Draw(HUD, TRACE_TEXT("MENU.PLAY", "PLAY"), X, Y, Style);
 *     FString::Printf(*TRACE_TEXT("HUD.RESPAWN_IN", "RESPAWN IN %.0f"), Seconds);
 *
 * A macro rather than a function so both arguments are string LITERALS at the call site: that is
 * what lets Scripts/dump-game-text.py read the pair straight out of the source, and what stops
 * anybody passing a runtime-built key by accident, which would register a new entry every frame.
 *
 * AND SO EACH EXPANSION CAN REMEMBER ITS ANSWER (P11). The macro is a lambda with its own
 * FCallSite static: one lookup the first time the line runs, a pointer read every time after. The
 * result is still `const FString&` into the store, so a reload still shows on the next draw.
 *
 * THE ONE LEGITIMATE EXCEPTION IS A DATA TABLE, and it is worth naming because it costs something.
 * TraceUserSettings' crosshair palette carries the key in a column beside the wording, so its call
 * reads TraceGameText::Get(Stop.TextKey, Stop.Name) and there is no literal for a scanner to find.
 * Those keys are real and they work; they simply do not appear in a document written from the
 * SOURCE, only in one written from a RUNNING GAME (Trace.Text.Dump), because that is the only place
 * the key exists. The script counts them and says so rather than quietly writing a short file.
 */
#define TRACE_TEXT(Key, Default) \
	([]() -> const FString& \
	{ \
		static TraceGameText::FCallSite TraceGameTextSite; \
		return TraceGameText::GetAt(TraceGameTextSite, TEXT(Key), TEXT(Default)); \
	}())

/**
 * The same, for a string with slots in it. The braced list is the arguments, in slot order.
 *
 *     TRACE_TEXTF("HUD.CARRIER", "{0} HAS THE CORE", { PlayerName })
 *     TRACE_TEXTF("HUD.RESPAWN_IN", "RESPAWN IN {0}", { FString::Printf(TEXT("%.0f"), Seconds) })
 *
 * Returns FString BY VALUE — it is a freshly composed string, unlike TRACE_TEXT which hands back a
 * reference to the stored one.
 */
#define TRACE_TEXTF(Key, Default, ...) TraceGameText::FormatPattern(TRACE_TEXT(Key, Default), __VA_ARGS__)
