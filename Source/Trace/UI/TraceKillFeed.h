// Trace — the kill feed: capture, replication and the shared entry type.
//
// Spec v8 §6: "Add a kill feed please, with headshot, dash, and parry kill icons".
//
// ---------------------------------------------------------------------------------------------
// WHY THIS IS A SEPARATE FILE AND NOT SIX LINES IN THE GAME MODE
// ---------------------------------------------------------------------------------------------
// A kill feed is two things bolted together: a REPLICATED EVENT and a DRAWN LIST. The drawn list
// belongs to ATraceHUD. The replicated event does not — AHUD is a client-only actor, it does not
// exist at all on a dedicated server, and it cannot own an RPC that has to originate on the
// authority. So the event half lives here, in two small classes:
//
//   ATraceKillFeedRelay      a hidden, always-relevant, replicated actor. On the server it
//                            observes every death and pushes the row onto a small REPLICATED
//                            ring buffer, which every machine — host included — then draws.
//
//   UTraceKillFeedSubsystem  exists only to spawn the relay. A UWorldSubsystem is created
//                            automatically for every game world with no wiring in any file, which
//                            is what lets the whole feature land without editing the game mode,
//                            the character, the weapon or the parry (this pass splits file
//                            ownership, and those four belong to other slices).
//
// THE FAILURE MODE THIS IS BUILT AGAINST is the theme of the entire v8 pass: a feature that works
// for the host and not for the people joining. So the host is deliberately given NO special path.
// The host draws the very same replicated array a client draws, and OnRep_Entries() is invoked by
// hand on the server after every write (the same convention UTraceHealthComponent uses for
// OnRep_Health) so that both machines run identical code. There is no "if (HasAuthority()) do it
// directly" shortcut anywhere in this file, because that shortcut is exactly how a host-only feed
// happens — the host keeps working while the client's half rots unnoticed.
//
// ---------------------------------------------------------------------------------------------
// WHY REPLICATED STATE AND NOT A MULTICAST RPC — THIS WAS MEASURED, NOT ASSUMED
// ---------------------------------------------------------------------------------------------
// The first implementation fired one reliable NetMulticast per kill. It worked, and on a
// two-process join at 40 ms it delivered most rows. MOST. In a measured run the server announced
// five kills in six seconds and the client received three of them — the two it missed never
// arrived at all, and the client went on running healthily for another four hundred frames.
//
// That is the whole argument. An RPC is a one-shot: whatever the reason a particular connection
// misses it (a stalled client, a saturated send queue, a channel that was not open at that
// instant), it is gone forever and the row is silently absent for that player only. Replicated
// PROPERTY state is eventually consistent and self-healing — the array is re-sent from its
// current value on the next actor update, so a missed packet costs latency instead of content.
//
// "Late" is an acceptable failure for a kill feed. "Missing, but only for the people who joined"
// is the exact bug this pass exists to eliminate.
//
// The one thing property replication would otherwise get wrong is the late joiner: an array is
// state, so somebody arriving mid-match would be handed the last few kills of a fight they were
// not in. FTraceKillFeedEntry::ServerTime closes that — rows are aged against the replicated
// server clock, so a row that was already stale when it arrived is simply never drawn.
//
// ---------------------------------------------------------------------------------------------
// THE CAUSE TAXONOMY IS THE ONE THE GAME ALREADY HAS
// ---------------------------------------------------------------------------------------------
// No parallel enum of kill types. The relay listens on UTraceHealthComponent::OnDeath — the one
// funnel every death in the project passes through (ApplyDamage and Kill both end at
// BroadcastDeath) — and maps the FName cause that is already carried there:
//
//     "Parried"  (TraceParry::GetParryKillCause())  -> shield      the carrier killing a dasher
//     "Trail"    (UTraceTrailComponent)             -> chevrons    a trace kill
//     "Bullet"   (UTraceWeaponComponent)            -> skull       IF it was a head shot
//                                                   -> round       body / leg
//     "Fell"     (ATraceCharacter)                  -> cross       the arena took them
//
// See ResolveBulletIcon() in the .cpp for how the head shot is recovered, what it is exact about,
// and the one-line change in another slice's file that would make it exact in every case.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtr.h"

#include "TraceTypes.h"   // ETraceTeam

#include "TraceKillFeed.generated.h"

class AController;
class ATraceCharacter;
class UTraceHealthComponent;
class UWorld;

/**
 * Which glyph a feed row draws between the two names.
 *
 * A PRESENTATION choice derived from the death cause, not a second taxonomy: nothing in gameplay
 * stores one of these, and the mapping lives in exactly one function (IconForCause, in the .cpp).
 */
UENUM()
enum class ETraceKillIcon : uint8
{
	/** Body or leg shot. The default weapon glyph: a rifle round. */
	Bullet = 0,
	/** "Bullet" whose fatal damage can only have been the head zone. A skull. */
	Headshot = 1,
	/** Cause "Trail" — somebody dashed a trace. A double chevron. */
	Dash = 2,
	/** Cause "Parried" — the carrier killed the dasher. A shield. */
	Parry = 3,
	/** Cause "Fell", or any death with no killer. A cross. */
	World = 4,
	/** Cause "Knife" — a front swipe, 30 damage a swing. A blade. (spec v10 §1) */
	Knife = 5,
	/** Cause "Backstab" — 100 damage from the rear hemisphere. A blade with a rear chevron. */
	Backstab = 6
};

/**
 * One row of the feed, as it crosses the wire.
 *
 * NAMES AND TEAMS ARE SNAPSHOTTED INTO THE STRUCT rather than sent as APlayerState pointers, and
 * that is on purpose. An actor pointer in an RPC arrives null on any client that has not yet been
 * told about that actor, and the one moment a kill feed is most likely to fire for a player whose
 * state has not replicated is the moment a client joins mid-match — i.e. precisely the case this
 * pass exists to fix. Two short strings per kill is nothing; kills happen a few times a minute.
 */
USTRUCT()
struct FTraceKillFeedEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString KillerName;

	UPROPERTY()
	FString VictimName;

	UPROPERTY()
	ETraceTeam KillerTeam = ETraceTeam::None;

	UPROPERTY()
	ETraceTeam VictimTeam = ETraceTeam::None;

	UPROPERTY()
	ETraceKillIcon Icon = ETraceKillIcon::Bullet;

	/** False for suicides, world deaths and anything the server could not attribute. */
	UPROPERTY()
	bool bHasKiller = false;

	/**
	 * APlayerState::GetPlayerId() for each participant, or INDEX_NONE.
	 *
	 * ONLY for "was this row about me?", which decides the row's border colour. The names above
	 * cannot answer that: two players may share one, and the two-process test rig proves it rather
	 * than supposing it — a listen server and a client on the same Mac both default to the machine's
	 * name, so a row about the CLIENT's player was drawn red on the HOST as well, as if the host had
	 * died. PlayerId is server-assigned, replicated and unique, so it is the same small integer on
	 * every machine and it is right even when the names are identical.
	 *
	 * Deliberately NOT used for display: the names are still snapshotted strings, because an id is
	 * useless to a client whose copy of that APlayerState has not replicated (see the note above).
	 */
	UPROPERTY()
	int32 KillerPlayerId = INDEX_NONE;

	UPROPERTY()
	int32 VictimPlayerId = INDEX_NONE;

	/**
	 * When the kill happened, on AGameStateBase::GetServerWorldTimeSeconds() — the SAME replicated
	 * clock ATracePlayerState::RespawnEndServerTime counts against.
	 *
	 * This is what makes replicated state safe for a feed. Ageing a row by "when this machine
	 * first saw it" would show a late joiner five old kills as if they had just happened, and
	 * would restart the fade on any client that received the array a second time. Ageing it by a
	 * clock both machines agree on means a row is exactly as old as it really is, everywhere.
	 */
	UPROPERTY()
	float ServerTime = 0.f;

	/**
	 * Strictly increasing, assigned by the server. Ordering is already carried by array position,
	 * so this exists for VERIFICATION: OnRep_Entries logs the sequence numbers it has newly seen,
	 * which turns "did every kill reach this client?" into a question a log file answers exactly,
	 * rather than something inferred from a screenshot. It is how the RPC's dropped rows were
	 * caught in the first place.
	 */
	UPROPERTY()
	int32 Sequence = 0;
};

/**
 * The replicated half of the kill feed. One per world, spawned by UTraceKillFeedSubsystem.
 *
 * Hidden, collisionless and always relevant. bAlwaysRelevant is the load-bearing flag: a kill on
 * the far side of a 33600 uu field must reach a player who cannot see either participant, and a
 * relevancy-culled actor's multicasts simply never arrive.
 */
UCLASS(NotPlaceable, Transient)
class TRACE_API ATraceKillFeedRelay : public AActor
{
	GENERATED_BODY()

public:
	ATraceKillFeedRelay();

	//~ Begin AActor interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor interface

	/** The relay in @p World, or null. Cheap, but cache it — it is an actor iteration. */
	static ATraceKillFeedRelay* Find(const UWorld* World);

	/**
	 * Rows this machine knows about, NEWEST FIRST, capped at MaxStoredEntries.
	 *
	 * Byte-for-byte identical on the host and on every client, because it IS the replicated array
	 * rather than a local copy assembled from events. Ageing is the reader's business: rows stay
	 * until they fall off the end of the buffer, and ATraceHUD::DrawKillFeed() is what decides a
	 * row is too old to draw — against ServerTime, never against arrival.
	 */
	const TArray<FTraceKillFeedEntry>& GetEntries() const { return Entries; }

	/**
	 * Age of @p Entry in seconds on the replicated server clock, clamped at zero.
	 *
	 * Lives here rather than in the HUD so that the drawer, the console dump and any future reader
	 * cannot disagree about how old a row is. Falls back to local world time only when there is no
	 * GameState yet, which is a window in which there are also no kills.
	 */
	static float GetEntryAge(const FTraceKillFeedEntry& Entry, const UWorld* World);

	/** Longest a row is ever worth drawing, in seconds. Read by the HUD so both agree. */
	static constexpr float EntryLifetime = 6.0f;

	/** Rows fade out over this long at the end of EntryLifetime. */
	static constexpr float EntryFadeTime = 0.75f;

	/** How many rows the HUD will draw at once (spec: "capped at ~5 rows"). */
	static constexpr int32 MaxDrawnEntries = 5;

	/** Ring-buffer depth. A little deeper than MaxDrawnEntries so a burst is not lost mid-fade. */
	static constexpr int32 MaxStoredEntries = 8;

	/**
	 * Server: publish one kill to everyone. Safe to call with a null killer.
	 *
	 * Public because a future non-character kill source (a hazard, a mode-B goal) should announce
	 * itself here rather than growing a second path.
	 */
	void ServerAnnounceKill(FTraceKillFeedEntry Entry);

	/** Bound to every character's UTraceHealthComponent::OnDeath on the server. */
	UFUNCTION()
	void HandleDeath(AActor* DeadActor, AController* KillerController, FName Cause);

	/**
	 * Runs on every machine when the feed changes — INCLUDING the server, which calls it by hand
	 * after every write. That is deliberate: it is the only way to be sure the host is exercising
	 * the same code as a client rather than a privileged shortcut.
	 */
	UFUNCTION()
	void OnRep_Entries();

private:
	/**
	 * The feed, newest at index 0. Replicated STATE, not a stream of events — see the header note
	 * for the measurement that produced that decision.
	 *
	 * A plain array rather than a FFastArraySerializer, and the "inefficiency" is the point: any
	 * change re-sends the whole array, which is precisely the self-healing behaviour wanted here.
	 * Eight rows of two short names is a few hundred bytes, a few times a minute.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Entries)
	TArray<FTraceKillFeedEntry> Entries;

	/** Server: next value for FTraceKillFeedEntry::Sequence. */
	int32 NextSequence = 1;

	/** Highest sequence this machine has already reported, so OnRep logs each row exactly once. */
	int32 HighestReportedSequence = 0;

	/**
	 * SERVER ONLY. One record per living character, refreshed every tick.
	 *
	 * Two jobs, and neither can be done at the instant of death:
	 *
	 *  1. HEALTH BEFORE THE FATAL HIT. Damage is clamped at zero, so by the time OnDeath fires the
	 *     size of the killing blow is gone. The previous frame's health is what recovers it, and it
	 *     is what tells a head shot from a body shot (see ResolveBulletIcon in the .cpp).
	 *  2. NAME AND TEAM. A death runs through ATraceGameMode::NotifyCharacterDied, which may drop
	 *     the Core and detach things; caching the identity a frame early means the feed can never
	 *     print "<unknown>" because it asked one call too late.
	 */
	struct FTrackedCharacter
	{
		TWeakObjectPtr<ATraceCharacter> Character;
		TWeakObjectPtr<UTraceHealthComponent> HealthComponent;
		float PreviousHealth = 0.f;
		FString Name;
		ETraceTeam Team = ETraceTeam::None;
	};

	TArray<FTrackedCharacter> Tracked;

	/**
	 * Seconds between safety-net sweeps.
	 *
	 * A SAFETY NET, not the primary mechanism: OnActorSpawned below binds a pawn the instant it is
	 * created, which is what closes the hole a poll alone would leave — a player killed within one
	 * poll interval of respawning (i.e. spawn-camped, i.e. exactly the kill most worth reporting)
	 * would otherwise never have been bound at all. The sweep stays because a delegate that fails
	 * to fire once should degrade into a half-second delay rather than a missing row.
	 */
	static constexpr float RescanInterval = 0.5f;
	float TimeSinceRescan = 1000.f;

	/** Server: pick up newly spawned characters and bind their death delegate. */
	void RescanCharacters();

	/** Server: bind one character if it is not already tracked. Idempotent. */
	void TrackCharacter(ATraceCharacter* Candidate);

	/** Server: UWorld::AddOnActorSpawnedHandler, so a pawn is bound the moment it exists. */
	void HandleActorSpawned(AActor* SpawnedActor);
	FDelegateHandle ActorSpawnedHandle;

	/** Server: refresh PreviousHealth / Name / Team for everything still alive. */
	void SampleTrackedCharacters();

	/** Server: the cached record for @p DeadActor, or null. */
	const FTrackedCharacter* FindTracked(const AActor* DeadActor) const;
};

/**
 * Spawns the relay. That is its entire job.
 *
 * A UWorldSubsystem is instantiated automatically for every world that passes
 * ShouldCreateSubsystem, so this needs no call site in any other file — which is what makes the
 * kill feed a self-contained addition rather than an edit spread across four gameplay classes.
 */
UCLASS()
class TRACE_API UTraceKillFeedSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem / UWorldSubsystem interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End interface
};
