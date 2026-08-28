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
// THAT COST HAS NOW BEEN PAID — FX_AUDIO_PLAN §6, AND THE PARAGRAPH THAT USED TO BE HERE IS WRONG.
//
// What it said was: "the sound is decided on the SERVER, so a remote client hears its own gunshot
// after half a round trip... Closing that gap needs the one line in ServerFire's caller, and the
// switch is ready for it." That line now exists. UTraceWeaponComponent::FireOnce plays the shooter's
// OWN copy locally, on the frame the trigger broke, with no round trip in it
// (TraceAudio::PlayPredictedLocal), and this observer's two announce sites went from PlayAt to
// PlayAtExcluding(the shooter's pawn) so that the one machine which already heard a predicted copy
// skips the multicast. One shot is still exactly one sound on every machine; only the shooter's own
// arrives on time now.
//
//   * The observer is NOT off. It is still what everybody ELSE hears, it still owns the pistol
//     ladder for those listeners, and Trace.Audio.ShotWatch 0 still silences the lot.
//   * Trace.Audio.PredictedShot (default 1) is the A/B against the pure-observer behaviour for
//     GUNSHOTS, MELEE SWINGS AND RELOADS, and it gates both of those halves off ONE read at the
//     announce site — see TraceAudioWatchFile::CVarPredictedShot and
//     TraceAudioWatch::IsPredictedShotEnabled(). At 0 the exclusion has to disarm too, or the
//     shooter is the one person who hears nothing. At 0 a shooter hears its own round exactly once,
//     half a round trip late; that arm was measured on a real client and it works.
//   * Bots exclude nobody: no machine locally PLAYER-controls a bot, so PlayAtExcluding degrades to
//     PlayAt for them and a bot's shot is heard exactly as it was before this pass.
//
// FOOTSTEPS GOT THE SAME TREATMENT, AND THE OLD "no such caveat" HERE WAS SIMPLY UNEXAMINED. A remote
// client heard its own footsteps at round-trip latency for the same reason its gunshots arrived late,
// and under load the multicast could drop one outright — so a player's own gait, which is the sound
// they hear most often in this game, was the least reliable thing in the mix. The stride accumulator
// is a pure function of the pawn's own ground speed, so it runs perfectly well on the machine that
// owns the pawn: this subsystem now also ticks on a CLIENT, over that machine's own player pawns and
// nothing else, and plays their steps locally while the authority excludes them from the multicast.
// See Tick() and AnnounceFootstep() for the split, and IsTickable() for what a client actually pays.
//
// *** W6 §F-1: "under load the multicast could drop one outright" WAS THE UNDERSTATEMENT OF THE PASS,
// AND FOOTSTEPS NOW HAVE THEIR OWN SWITCH. *** Measured on a listen host and a real client, both
// ledgers Verbose, matched position by position for the client's own pawn: with the prediction armed
// it hears 17 of its own 17 steps and sends zero RPCs; with it disarmed the authority announces and
// multicasts all 16 and THREE arrive. Not "could drop one" — drops thirteen. A gait asks the relay
// for a sound five times a second and the relay's multicast is unreliable by design, so the observer
// route is a working fallback for a gun and is not one for feet. Trace.Audio.PredictedShot therefore
// no longer carries footsteps: Trace.Audio.PredictedFootstep does, its 0 is a RED ARM rather than a
// fallback, and both switches' help text now says which it is.
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
 * FX_AUDIO_PLAN §6.4 — the predicted-shot switch, read through ONE function.
 *
 * The switch has to be legible from two files at once: this one, which decides whether the multicast
 * EXCLUDES the shooter, and UTraceWeaponComponent::FireOnce, which decides whether the shooter plays
 * a predicted copy at all. §6.4 is explicit that those two must be gated on the same read — "while
 * PredictedShot=0, the exclusion must also disarm (or the shooter hears nothing)" — so the CVar is
 * declared exactly once, in TraceAudioWatch.cpp, and every reader comes through here.
 *
 * A free function rather than a static on the subsystem because the weapon component must be able to
 * ask without a world, a subsystem or a pawn: the answer is a console variable, not per-world state.
 */
namespace TraceAudioWatch
{
	/** True when the shooter's own machine plays its own gunshot/swing/reload and the multicast skips it. */
	TRACE_API bool IsPredictedShotEnabled();

	/**
	 * The same question for FOOTSTEPS, and it is a SEPARATE question since W6 §F-1.
	 *
	 * §6.4's rule — the predicted copy and the multicast exclusion must come off one read — is
	 * unchanged and is now applied per family, because the two families do not have the same
	 * fallback. Turning the gunshot prediction off leaves a shooter hearing exactly one shot, late.
	 * Turning the footstep prediction off leaves a remote player hearing three of their own sixteen
	 * steps: the observer's route for footsteps is an unreliable multicast asked for a sound five
	 * times a second, and it drops most of them. So Trace.Audio.PredictedShot 0 is a supported
	 * fallback and Trace.Audio.PredictedFootstep 0 is a red arm, and they are different variables.
	 *
	 * Everything inside Source/Trace/Audio that decides a FOOTSTEP's route asks this; the weapon
	 * component and the clip watch still ask IsPredictedShotEnabled(). No caller may mix them.
	 */
	TRACE_API bool IsPredictedFootstepEnabled();

	/**
	 * True while Trace.Audio.ShotLog is on — one printed line per gunshot, from BOTH announcers.
	 *
	 * Shared for the same reason the switch above is: after §6 there are two places a gunshot is
	 * announced from, on two different machines, and a per-shot log that only covered one of them
	 * would make a two-process audit unreadable in exactly the case it is for. The observer's line
	 * says "this pawn's round left the clip and I multicast it, excluding their machine"; the
	 * predicted line says "I am that machine and I played it myself". Each names its shooter, so a
	 * match full of bots cannot confuse the count.
	 */
	TRACE_API bool IsShotLogEnabled();
}

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
 * The watcher. See the file header for why it exists in this shape.
 *
 * A TICKABLE world subsystem and not an actor: it owns no state that replicates, it must not appear in
 * a level, and it has to die with the world it is watching.
 *
 * *** IT IS NO LONGER AUTHORITY-ONLY, AND THE SPLIT IS NARROW ON PURPOSE (FX_AUDIO_PLAN §6). ***
 * On the authority it does what it always did: watches every pawn's clip and every pawn's stride and
 * announces both to the world. On a CLIENT it watches exactly one thing — the stride of the player
 * pawns this machine locally controls — so that a player's own footsteps are heard on the frame they
 * happen instead of half a round trip later, and are not at the mercy of an unreliable multicast. A
 * client runs no clip watch at all (the shooter's own gunshot is predicted at the trigger, in
 * UTraceWeaponComponent::FireOnce) and never looks at anybody else's pawn.
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
	 * the walk is not the part under test.
	 *
	 * WHICH ROUTE THE STEP TAKES IS DECIDED IN HERE, not by the caller, because the caller (a stride
	 * accumulator) has no business knowing about netmodes. Three cases, and the pair of them adds up
	 * to exactly one play per machine per step:
	 *
	 *   this machine's own player pawn   predicted local (no RPC) — and, if we are also the
	 *                                    authority, PlayAtExcluding(this pawn) for everybody else
	 *   a remote player's pawn, on the   PlayAtExcluding(that pawn): every machine but theirs, because
	 *   authority                        theirs is playing its own predicted copy
	 *   a bot, on the authority          plain PlayAt — there is no machine to exclude
	 *
	 * All three read Trace.Audio.PredictedFootstep, and ONLY that (W6 §F-1) — never the gunshot
	 * switch. At 0 the first two collapse into the third and the walker loses most of its own steps,
	 * which is what makes that value a red arm rather than a fallback.
	 */
	void AnnounceFootstep(ATraceCharacter* Pawn);

private:
	/**
	 * The client half of Tick: the stride accumulator, over this machine's own player pawns only.
	 *
	 * Separate from Tick's authority loop rather than folded into it with a flag, because the two are
	 * different jobs on different populations — "every pawn in the world, clip and stride" against
	 * "my pawn, stride" — and a shared loop with two netmode tests inside it is how the bot trap gets
	 * reintroduced.
	 */
	void TickLocalPlayerFootsteps(UWorld* World, float DeltaSeconds);

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
