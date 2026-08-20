// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — GUNSHOTS AND FOOTSTEPS (spec v29 §1b, §1c, §1d, §1e)
// ===================================================================================================
//
// Two decisions and one observer live here:
//
//   FTracePistolLadder        WHICH pistol clip a shot gets: 1, 2, 3, then 4 forever, with a 0.3 s
//                             silence resetting it — measured from the LAST SHOT.       (§1c)
//   FTraceFootstepPicker      WHICH of the eleven footstep clips a step gets: random, never the
//                             same one twice running.                                    (§1b)
//   UTraceAudioWatchSubsystem WHEN either of those happens: the authority-side observer that turns
//                             "a round left this pawn's clip" and "this pawn has walked a stride"
//                             into world sounds at the muzzle and at the feet.     (§1b, §1d, §1e)
//
// ---------------------------------------------------------------------------------------------------
// *** WHY AN OBSERVER AND NOT A LINE IN THE WEAPON COMPONENT. SAY THIS OUT LOUD. ***
// ---------------------------------------------------------------------------------------------------
// The one-line call site for a gunshot belongs in UTraceWeaponComponent::ServerFire, next to the code
// that decides a round was actually spent. THAT FILE IS OWNED BY ANOTHER AGENT THIS PASS (spec v29 §2
// names Source/Trace/Gameplay/TraceWeaponComponent.{h,cpp}), and this pass does not write into other
// people's files. So the gunshot is driven from the audio side instead, by watching the one piece of
// state that means "a round left the gun" — the AUTHORITATIVE clip count.
//
// That is not a euphemism for a guess. UTraceWeaponComponent::ConsumeRound is the ONLY thing that
// decrements ClipAmmo on the authority, and it is called once per round fired; RefillClip only ever
// raises it. The same technique is already how Trace.FireRate.Measure times the gun
// (Abilities/TraceAbilityComponent.cpp, phase 2: "watch the clip. Every round that leaves it is
// stamped"), and that harness's numbers are the ones spec v29 §2f quotes. The two edges this
// polling cannot see are handled explicitly below:
//
//   * A GUN SWAP EXCHANGES MAGAZINES (spec v28 §9 — each gun keeps its own clip), so the count can
//     fall by fifteen in one frame without a shot. The record notices the weapon changed and re-seeds
//     instead of firing fifteen sounds.
//   * TWO ROUNDS IN ONE FRAME cannot happen at any shipped fire rate (the SMG's 0.1 s interval is six
//     frames at 60 Hz), but if it ever did, the ladder advances once per round and the shots are
//     played at the same instant — which is what actually happened.
//
// THE COST OF DOING IT THIS WAY, STATED PLAINLY: the sound is decided on the SERVER, so a remote
// client hears its own gunshot after half a round trip rather than on the frame it pulled the
// trigger. §1e asks for a global sound at the muzzle and that is what this delivers, but a shooter's
// own report is normally predicted locally. Closing that gap needs the one line in ServerFire's
// caller, and the switch is ready for it: Trace.Audio.ShotWatch 0 turns this observer off without a
// rebuild, so whoever adds that call site does not have to unpick this one.
//
// Footsteps have no such caveat. There is no existing footstep call site anywhere in the project, no
// animation notify to hang one on, and a stride accumulated from the pawn's own ground speed is the
// mechanism a footstep system would want regardless of who owns which file.
// ===================================================================================================

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtr.h"

#include "Audio/TraceSoundEvents.h"
#include "Gameplay/TraceMelee.h"   // ETraceEquippedWeapon — the swap guard needs the selector

#include "TraceAudioWatch.generated.h"

class ATraceCharacter;

/**
 * SPEC v29 §1c — THE PISTOL'S FOUR-SHOT SEQUENCE, as a value type with no engine dependency.
 *
 *     "have the consecutive sounds play when the pistol is shot fast, then play pistol 4 for every
 *      shot. After a .3second break from shooting, reset the sounds back to pistol 1."
 *
 * One instance per shooter. It is a plain struct rather than a component so that the rule can be
 * exercised by a harness at a thousand shots a second without a world, a pawn or an audio device —
 * see Trace.Audio.GunLadder, which checks the pure rule first and only then drives the real gun.
 *
 * *** THE RESET IS MEASURED FROM THE LAST SHOT, NOT FROM THE TRIGGER RELEASE. *** The spec is
 * explicit and the difference is real: a player who holds the trigger through an empty clip, or
 * through a reload, has not fired for those seconds even though nothing was released. The clock is
 * stamped inside NextShot() and in no other place, so "time since firing" is the only quantity this
 * type can possibly be measuring.
 */
struct TRACE_API FTracePistolLadder
{
	/**
	 * The clip for the next shot, advancing the ladder.
	 *
	 * @param NowSeconds     any monotonic clock, in seconds. The caller picks it; the world's own
	 *                       time is what the subsystem passes, so time dilation moves the gap with
	 *                       the gun rather than against it.
	 * @param ResetSeconds   the silence that resets the ladder. 0.3 s, from
	 *                       UTraceAudioSettings::PistolLadderResetSeconds.
	 */
	FName NextShot(double NowSeconds, float ResetSeconds);

	/** Which shot of the current burst the LAST NextShot() call was, 1-based. 0 before the first. */
	int32 GetShotNumber() const { return ShotNumber; }

	/** Seconds since the last shot, or a very large number if there has not been one. */
	double SecondsSince(double NowSeconds) const { return NowSeconds - LastShotSeconds; }

	/** Back to "the next shot is shot 1", as if the pawn had never fired. */
	void Reset();

private:
	/** Shots so far in this burst. Clamped for the CLIP by PistolShotEvent, never here. */
	int32 ShotNumber = 0;

	/**
	 * When the last shot was, on the caller's clock.
	 *
	 * Seeded to a large negative so the very first shot of a match takes the reset branch and is
	 * PistolShoot1 — rather than depending on the burst counter starting at zero, which would be two
	 * facts that have to agree instead of one.
	 */
	double LastShotSeconds = -1.0e9;
};

/**
 * SPEC v29 §1b — "randomize it so that step sounds vary", with the guard that makes it read as varied.
 *
 * Pure random over eleven clips repeats one about 9% of the time, and a repeat is the ONE outcome a
 * listener notices: two identical steps in a row is exactly the artefact randomising was supposed to
 * remove. So this draws from the eleven and rejects the previous pick, which makes every step a
 * uniform draw over the OTHER ten.
 *
 * One instance per pawn, so two players walking side by side do not step in lockstep.
 */
struct TRACE_API FTraceFootstepPicker
{
	/** The next footstep clip, never the same one twice running. NAME_None if there are none. */
	FName Next();

	/** Which clip index was last handed out, or INDEX_NONE. For the harness's repeat check. */
	int32 GetLastIndex() const { return LastIndex; }

	/** Forget the last pick, so the next draw may be any of the eleven. */
	void Forget() { LastIndex = INDEX_NONE; }

private:
	int32 LastIndex = INDEX_NONE;
};

/** One shot the watcher announced, for the harnesses to read back. */
struct FTraceShotAudioRecord
{
	/** Which pawn fired. */
	FString Shooter;
	/** The clip that was played: PistolShoot1..4, or SmgShoot1. */
	FName Event;
	/** The world clock at the moment the round was seen to have left the clip. */
	double Time = 0.0;
	/** 1-based shot number within the burst. Always 1 for the SMG, which has no ladder. */
	int32 ShotNumber = 0;
	/** Seconds since this shooter's previous shot. Negative on the first shot of a run. */
	double SinceLast = -1.0;
	/** Which gun. */
	ETraceEquippedWeapon Weapon = ETraceEquippedWeapon::Gun;
};

/**
 * The authority-side watcher. See the file header for why it exists in this shape.
 *
 * A TICKABLE world subsystem and not an actor: it owns no state that replicates, it must not appear in
 * a level, and it has to die with the world it is watching. It ticks only on the authority and only
 * while there is something to watch, so a client pays nothing for it at all.
 */
UCLASS()
class TRACE_API UTraceAudioWatchSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	//~ End USubsystem

	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaSeconds) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	virtual bool IsTickableInEditor() const override { return false; }
	//~ End FTickableGameObject

	/** The watcher for @p WorldContext's world, or null. Null is legal everywhere. */
	static UTraceAudioWatchSubsystem* Get(const UObject* WorldContext);

	// ---------------------------------------------------------------------------------------------
	// READ-BACK FOR THE HARNESSES. Nothing in the game reads these.
	// ---------------------------------------------------------------------------------------------

	/** Every shot this watcher has announced since the log was last cleared, oldest first. */
	const TArray<FTraceShotAudioRecord>& GetShotLog() const { return ShotLog; }

	/** Clears the shot log, so a run measures its own burst. */
	void ResetShotLog() { ShotLog.Reset(); }

	/** Every footstep clip this watcher has chosen since the log was last cleared, oldest first. */
	const TArray<FName>& GetFootstepLog() const { return FootstepLog; }

	/** Clears the footstep log. */
	void ResetFootstepLog() { FootstepLog.Reset(); FootstepWalkers.Reset(); }

	/**
	 * How many footsteps logged at or after @p FromIndex were made by @p Pawn.
	 *
	 * *** THIS EXISTS BECAUSE THE LOG IS WORLD-WIDE AND A STRIDE MEASUREMENT IS NOT. ***
	 * Trace.Audio.Footsteps walks ONE pawn for three seconds and compares the steps that came out
	 * against the distance it covered. Reading the log's LENGTH to do that silently adds every other
	 * pawn's steps to the numerator: in a six-bot match the walk measured 43 steps for 2334 uu of
	 * walking against 13.3 due, and the extra thirty belonged to the bots. It is the more dangerous
	 * failure in the other direction — bots walking is enough to make the count "roughly right" even
	 * if the subject pawn's own accumulator were completely dead — so filtering is what turns this
	 * from a number into a measurement.
	 *
	 * A weak pointer per entry rather than a name: two pawns can share a name across a respawn, and
	 * comparing the pointer cannot be fooled by that.
	 */
	int32 CountFootstepsForSince(const ATraceCharacter* Pawn, int32 FromIndex) const;

	/**
	 * Announce one footstep for @p Pawn right now, choosing the clip through the pawn's own picker.
	 *
	 * The stride accumulator's own call, exposed so Trace.Audio.Footsteps can drive a hundred steps
	 * without walking a pawn a hundred strides — the RANDOMNESS is what that harness measures, and
	 * the walk is not the part under test. Authority only, like everything else here.
	 */
	void AnnounceFootstep(ATraceCharacter* Pawn);

private:
	/** What the watcher remembers about one pawn, between frames. */
	struct FWatchedPawn
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;

		/** The clip count last frame. INDEX_NONE until the first look, which never fires a sound. */
		int32 LastClipAmmo = INDEX_NONE;

		/** The selector last frame. A change re-seeds the clip rather than firing sounds for it. */
		ETraceEquippedWeapon LastWeapon = ETraceEquippedWeapon::Gun;

		/** Distance walked on the ground since the last footstep, in uu. */
		double StrideAccumulatorUU = 0.0;

		FTracePistolLadder Ladder;
		FTraceFootstepPicker Steps;
	};

	/** The record for @p Pawn, creating it on first sight. Never null. */
	FWatchedPawn& RecordFor(ATraceCharacter* Pawn);

	/** Drops records whose pawn has gone. Cheap, and it runs on the same tick as everything else. */
	void ForgetDeadRecords();

	/** §1c/§1d/§1e: the clip watch, for one pawn. */
	void TickGunshots(ATraceCharacter* Pawn, FWatchedPawn& Record, double NowSeconds);

	/** §1b: the stride accumulator, for one pawn. */
	void TickFootsteps(ATraceCharacter* Pawn, FWatchedPawn& Record, float DeltaSeconds);

	TArray<FWatchedPawn> Watched;

	/** See GetShotLog. Trimmed so a long session cannot grow it without bound. */
	TArray<FTraceShotAudioRecord> ShotLog;

	/** See GetFootstepLog. Trimmed the same way. */
	TArray<FName> FootstepLog;

	/**
	 * WHO made each entry of FootstepLog, in lockstep with it — same length, same order, always
	 * appended together. See CountFootstepsForSince for why the log needs an owner column at all.
	 */
	TArray<TWeakObjectPtr<const ATraceCharacter>> FootstepWalkers;
};
