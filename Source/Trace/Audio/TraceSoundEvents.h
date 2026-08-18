// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — THE SOUND EVENT TABLE (spec v26 §9)
// ===================================================================================================
//
// Nine sounds, and the ONE thing about them that is a design decision rather than a file:
//
//     "Some should be client side, some should be game-side. ... Core Turnover, dash, and parry
//      should be game-side. The rest should be client side."
//
// GAME-SIDE means EVERYONE NEARBY HEARS IT — the server multicasts it at a world location.
// CLIENT-SIDE means ONLY THE LOCAL PLAYER HEARS IT — played on that machine, with no RPC at all.
//
// *** THE SIDE LIVES HERE, IN CODE, AND NOT IN THE ASSET. *** The WAV a name maps to is data (see
// Audio/TraceSoundBank.h — swap the file, re-run Scripts/import-sounds.sh, done, no rebuild). WHICH
// MACHINES HEAR IT is not data: it is a networking behaviour, it is the half of §9 a verifier checks
// by listening from a second viewpoint, and a data file that could silently turn a multicast into a
// local play is a way to break the owner's explicit design without touching any code. So the table
// below is the authority, TraceAudio::Play() reads it, and a call site therefore CANNOT be wired to
// the wrong side — there is no "play this locally" argument for it to get wrong.
//
// An event NOT in this table (a tenth WAV somebody drops into Art/Sounds/ tomorrow) is not an error:
// TraceSoundEvents::SideOf() answers Client for it, which is the safe default — the worst case is
// that one machine hears a new sound instead of all of them, never that a client-side sound is
// blasted across the whole server.
//
// APPEND to the table, never insert: nothing indexes it, but the report ordering is the spec's
// ordering and it is easier to diff against the PDF that way.
// ===================================================================================================

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "UObject/NameTypes.h"

/**
 * Which machines hear an event.
 *
 * Deliberately two values and no third. "Both" was considered and rejected: every one of the nine is
 * one or the other, and an event that played twice on a listen server (once locally, once through
 * its own multicast) is precisely the bug the two-value enum makes unrepresentable.
 */
enum class ETraceSoundSide : uint8
{
	/** Only the local player hears it. Played on this machine. NO RPC — not even a reliable one. */
	Client = 0,
	/** Everybody nearby hears it. The SERVER multicasts it at a world location. */
	World = 1,
};

/** One row of the table below. */
struct FTraceSoundEvent
{
	/** The event name. Also the WAV's stem in Art/Sounds/ and the bank key — see TraceSoundBank.h. */
	FName Name;

	/** Client or World. See the header comment: this is code, not data. */
	ETraceSoundSide Side = ETraceSoundSide::Client;

	/** What fires it, in the owner's words. Printed by Trace.Audio.Report. */
	const TCHAR* Trigger = TEXT("");
};

namespace TraceSoundEvents
{
	// ---------------------------------------------------------------------------------------------
	// GAME-SIDE — replicated, at a world location, everyone nearby hears it.
	// ---------------------------------------------------------------------------------------------

	/** A turnover is registered. ATraceCore::RegisterTurnover, at the Core. */
	TRACE_API extern const FName CoreTurnover;

	/** A dash starts. One per dash, on the authority, at the dashing pawn. */
	TRACE_API extern const FName Dash;

	/** A parry fires — the window actually OPENED, not merely that a key was pressed. */
	TRACE_API extern const FName Parry;

	// ---------------------------------------------------------------------------------------------
	// CLIENT-SIDE — local only, no RPC.
	// ---------------------------------------------------------------------------------------------

	/** Your shot hit a body. */
	TRACE_API extern const FName Bodyshot;

	/** Your shot hit a head. */
	TRACE_API extern const FName Headshot;

	/** You picked up the Core. */
	TRACE_API extern const FName CorePickup;

	/** You jumped. */
	TRACE_API extern const FName Jump;

	/** You wall-jumped. */
	TRACE_API extern const FName WallJump;

	/** A menu row was activated. */
	TRACE_API extern const FName ButtonPress;

	/** The whole table, in the spec's order. */
	TRACE_API TConstArrayView<FTraceSoundEvent> All();

	/**
	 * Which machines hear @p Event.
	 *
	 * An unknown name answers Client, on purpose — see the header. Never asserts: a typo at a call
	 * site must degrade to "one player heard it", not to a crash and not to a broadcast.
	 */
	TRACE_API ETraceSoundSide SideOf(FName Event);

	/** False for a name that is not in the table (a typo, or a WAV nobody has declared a side for). */
	TRACE_API bool IsKnown(FName Event);

	/** "client-side" / "game-side", for logs and for Trace.Audio.Report. */
	TRACE_API const TCHAR* SideName(ETraceSoundSide Side);
}
