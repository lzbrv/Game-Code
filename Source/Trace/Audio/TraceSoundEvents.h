// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — THE SOUND EVENT TABLE (spec v26 §9, extended by v29 §1)
// ===================================================================================================
//
// Twenty-eight sounds, and the ONE thing about them that is a design decision rather than a file:
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
// ---------------------------------------------------------------------------------------------------
// SPEC v29 §1 — WHAT THIS PATCH ADDED, AND WHAT IT DELIBERATELY DID NOT TOUCH
// ---------------------------------------------------------------------------------------------------
// §1a: "replace the old sounds ... keeping the same sounds client side vs global". SEVEN WAVs were
// replaced on disk (CorePickup, CoreTurnover, Dash, Headshot, Jump, Parry, WallJump) and NOT ONE ROW
// BELOW MOVED. The side is code, the file is data, and a re-import cannot reach this table — which is
// the whole reason the split lives here. Trace.Audio.Sides prints the nine original rows against the
// v26 spec text so "we kept them" is checkable rather than asserted.
//
// §1e: every gunshot is World. "Gunshots should be global ... so other players hear where a shot came
// from" is a gameplay signal, not decoration.
//
// §1b: every footstep is World, for the same reason — hearing somebody else's steps is information.
// Their VOLUME is the thing that makes them background: see UTraceAudioSettings::FootstepVolumeScale,
// which is a multiplier on the master volume and is measured, not asserted (Trace.Audio.Loudness).
//
// §1f: Goal, Kill and RoccoRipple arrived with no stated trigger. The choices, and why:
//
//   Goal         World.   A goal is the whole room's event; the owner's own "surely global".
//   Kill         Client.  The KILLER's confirmation, and it is the same class of feedback as the
//                         hitmarker sounds it rides beside — Bodyshot and Headshot are already
//                         client-side and already play from ATracePlayerController::ClientNotifyHit,
//                         which already carries `bKilled`. Making it World would announce every kill
//                         in the arena to everyone, which is a scoreboard's job, not a sound's, and
//                         would mean the person who died hears their own killer's reward.
//   RoccoRipple  World.   An ability going off in the world, exactly like Dash and Parry, which are
//                         already game-side. An enemy who cannot hear a Ripple being laid cannot
//                         react to it, and the Ripple is a thing OTHER players ride.
//
// ---------------------------------------------------------------------------------------------------
// An event NOT in this table (a WAV somebody drops into Art/Sounds/ tomorrow) is not an error:
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

/**
 * What KIND of sound an event is, for the two things that need to treat a group as a group.
 *
 * This is NOT a second side enum and it never decides who hears anything. It exists because §1b gives
 * footsteps their own volume knob ("ensure they are quieter than other sounds"), and a knob that is
 * applied by matching name prefixes at the play site is a knob that silently stops applying the day
 * somebody adds Step12 or renames one. The family is declared on the row, once, beside the side.
 *
 * Gunshot earns its place for the ladder rather than for volume: Trace.Audio.GunLadder needs to ask
 * "was that shot sound a shot sound" without carrying its own copy of the five names.
 */
enum class ETraceSoundFamily : uint8
{
	/** Everything that is simply itself. */
	Default = 0,
	/** Step1..Step11. Carries FootstepVolumeScale. */
	Footstep = 1,
	/** PistolShoot1..4 and SmgShoot1. */
	Gunshot = 2,
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

	/** Default, Footstep or Gunshot. See ETraceSoundFamily. */
	ETraceSoundFamily Family = ETraceSoundFamily::Default;
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

	/** You registered a kill. SPEC v29 §1f — the killer's own confirmation. */
	TRACE_API extern const FName Kill;

	// ---------------------------------------------------------------------------------------------
	// SPEC v29 §1e — GUNSHOTS, GAME-SIDE, AT THE MUZZLE.
	// ---------------------------------------------------------------------------------------------

	/** The pistol's four-shot ladder. Shot 1, 2, 3, then 4 for the fourth and every one after. */
	TRACE_API extern const FName PistolShoot1;
	TRACE_API extern const FName PistolShoot2;
	TRACE_API extern const FName PistolShoot3;
	TRACE_API extern const FName PistolShoot4;

	/** The SMG. Every round, no ladder (§1d). */
	TRACE_API extern const FName SmgShoot1;

	// ---------------------------------------------------------------------------------------------
	// SPEC v29 §1b — FOOTSTEPS. Game-side, randomised, and much quieter than everything else.
	// ---------------------------------------------------------------------------------------------

	/** How many footstep clips there are. Eleven. Read this, never the literal. */
	TRACE_API int32 FootstepCount();

	/**
	 * Footstep clip @p Index, 0-based: 0 is Step1 and FootstepCount()-1 is Step11.
	 *
	 * An out-of-range index answers NAME_None rather than asserting, and every caller already has to
	 * survive an event with no sound.
	 */
	TRACE_API FName FootstepAt(int32 Index);

	/**
	 * The pistol clip for the @p ShotNumber-th shot of a burst, 1-based, CLAMPED AT FOUR.
	 *
	 *     1 -> PistolShoot1   2 -> PistolShoot2   3 -> PistolShoot3   4, 5, 6, ... -> PistolShoot4
	 *
	 * The clamp is the spec's "then play pistol 4 for every shot" and it lives here rather than at the
	 * call site so the ladder and the harness that checks the ladder cannot disagree about it.
	 */
	TRACE_API FName PistolShotEvent(int32 ShotNumber);

	// ---------------------------------------------------------------------------------------------
	// SPEC v29 §1f — the three with no stated trigger. Reasoning is in the header comment.
	// ---------------------------------------------------------------------------------------------

	/** A goal is scored. Game-side, at the Core. */
	TRACE_API extern const FName Goal;

	/** Rocco lays a Ripple. Game-side, at the path's start ring. */
	TRACE_API extern const FName RoccoRipple;

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

	/** Which family @p Event belongs to. Default for anything not in the table. */
	TRACE_API ETraceSoundFamily FamilyOf(FName Event);

	/** "footstep" / "gunshot" / "-", for logs and for Trace.Audio.Report. */
	TRACE_API const TCHAR* FamilyName(ETraceSoundFamily Family);
}
