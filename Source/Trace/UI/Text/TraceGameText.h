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
//   NOTHING CAN EVER BE BLANK. Delete the document, delete one line of it, typo a key, ship a build
//   with no document at all — every one of those falls back to the words in the code, which are the
//   words the game shipped with. There is no state in which the player sees an empty label, and that
//   is worth more than any amount of tooling around a file that might not be there.
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
// slots; they cannot invent one the code will not fill or delete one it will.
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
// One hash lookup per string per draw, against a map that is built once. The HUD draws on the order
// of a hundred strings a frame, so this is a few microseconds. Values are stored by the map and
// returned by reference, so no string is copied and no allocation happens after load.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

namespace TraceGameText
{
	/**
	 * THE ONE LOOKUP. Returns the document's wording for @p Key, or @p DefaultText when the document
	 * has nothing usable for it.
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
	 * The formatting half of Get(): looks the key up, then fills its slots from @p Args.
	 *
	 * Runtime-safe by construction — see the PLACEHOLDERS note at the top of this file for why this
	 * cannot be FString::Printf in UE 5.8, and why that is the better outcome.
	 */
	TRACE_API FString Format(const TCHAR* Key, const TCHAR* DefaultText,
		const FStringFormatOrderedArguments& Args);
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
 * THE ONE LEGITIMATE EXCEPTION IS A DATA TABLE, and it is worth naming because it costs something.
 * TraceUserSettings' crosshair palette carries the key in a column beside the wording, so its call
 * reads TraceGameText::Get(Stop.TextKey, Stop.Name) and there is no literal for a scanner to find.
 * Those keys are real and they work; they simply do not appear in a document written from the
 * SOURCE, only in one written from a RUNNING GAME (Trace.Text.Dump), because that is the only place
 * the key exists. The script counts them and says so rather than quietly writing a short file.
 */
#define TRACE_TEXT(Key, Default) TraceGameText::Get(TEXT(Key), TEXT(Default))

/**
 * The same, for a string with slots in it. The braced list is the arguments, in slot order.
 *
 *     TRACE_TEXTF("HUD.CARRIER", "{0} HAS THE CORE", { PlayerName })
 *     TRACE_TEXTF("HUD.RESPAWN_IN", "RESPAWN IN {0}", { FString::Printf(TEXT("%.0f"), Seconds) })
 *
 * Returns FString BY VALUE — it is a freshly composed string, unlike TRACE_TEXT which hands back a
 * reference to the stored one.
 */
#define TRACE_TEXTF(Key, Default, ...) TraceGameText::Format(TEXT(Key), TEXT(Default), __VA_ARGS__)
