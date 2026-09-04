// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Map.h"
#include "Engine/EngineTypes.h"                  // FTimerHandle
#include "GameFramework/GameModeBase.h"
#include "Templates/SubclassOf.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "Core/TraceMatchTypes.h"                // ETraceMatchEndReason
#include "TraceTypes.h"                          // ETraceTeam

#include "TraceGameMode.generated.h"

class AActor;
class AController;
class APlayerController;
class ATraceArenaBuilder;
class ATraceBotController;
class ATraceCharacter;
class ATraceCore;
class ATraceEndzone;
class ATraceGameState;
class ATracePlayerState;
class ATraceTeamPlayerStart;

/**
 * Declared in Core/TracePlayerState.h. Forward declared here (legal: the underlying type is fixed)
 * because this header is included by half the module and does not need to drag the player state in
 * for one return type.
 */
enum class ETraceCharacterPickResult : uint8;

/**
 * Who gets the Core after a goal.
 *
 * The design doc specifies who starts each HALF but says nothing about what follows a score, so
 * this is an interpretation made configurable rather than a rule baked into code. The default is
 * American-football kickoff logic; changing the whole game's restart rule is one line in
 * DefaultGame.ini.
 */
UENUM()
enum class ECoreKickoffMode : uint8
{
	/**
	 * The restart favours the team that was scored on, at their own end. The shipped assumption.
	 *
	 * DEMO 29 §3 CHANGED WHAT "FAVOURS" MEANS AND THE NAME NO LONGER READS QUITE RIGHT. It used to
	 * GRANT the Core to that team. It now SPAWNS it on the floor in front of the goal they just
	 * conceded, held by nobody, with the scoring team locked out of it for the reset window — so the
	 * conceding side still gets it, but by walking onto it (or pulling it) rather than by having it
	 * appear in somebody's hands. See ATraceGameMode::KickoffCoreAfterGoal.
	 */
	ScoredOnTeam = 0,
	/** Strict alternation regardless of who scored, starting from the team that kicked off the half. */
	AlternateTeams = 1,
	/** Nobody is granted the Core; it returns to the centre of the field and is contested. */
	Neutral = 2
};

#if !UE_BUILD_SHIPPING
/**
 * Which scripted proof of spec v9 §11 Trace.HalfTime.Verify runs. See
 * ATraceGameMode::StartHalfTimeVerify.
 */
enum class ETraceHalfTimeVerifyScenario : uint8
{
	/**
	 * Arm the whistle, pass WITHIN the team (must NOT fire), then hand the Core to the other team
	 * (must fire). The negative half of this is the one that matters: a half that ends on a
	 * team-mate's pass is the same "cut off in the middle of a run" complaint §11 was written about.
	 */
	TurnoverVsPass = 0,

	/**
	 * Arm the whistle and hold every dead ball off — the Core is released on every step so nobody
	 * can hold it, score with it or drop it — and prove the hard cap ends the period anyway. Pair it
	 * with a short UTraceSettings::PeriodEndMaxDeferSeconds.
	 */
	HardCap = 1,

	/**
	 * Arm the whistle, then win by the mercy rule while it is still pending. §11 must NOT have made
	 * the mercy rule wait for a dead ball — it is a mercy, not a play boundary — and the whistle it
	 * pre-empts must not be left armed underneath the results screen.
	 */
	MercyWhilePending = 2
};
#endif

/**
 * Server-authoritative rules for a Trace match.
 *
 * Owns: arena/Core spawning, team balancing, spawn-point selection, the match phase machine,
 * respawn scheduling and scoring. AGameModeBase only ever exists on the server, so every mutation
 * here is authoritative by construction; the HasAuthority() guards that remain are on the entry
 * points other systems call, so a mistake elsewhere fails quietly instead of desyncing.
 *
 * The phase machine lives on ATraceGameState (that is the replicated half); this class is the only
 * thing allowed to advance it:
 *
 *     WaitingForPlayers --(MinPlayersToStart reached)--> [WarmupDuration countdown]
 *                       --(warm-up expires)------------> InProgress
 *                       --(final half's clock expires)-> PostMatch   [ETraceMatchEndReason::Clock]
 *                       --(a lead of MercyRuleLead)----> PostMatch   [ETraceMatchEndReason::Mercy]
 *
 * WIN CONDITIONS (spec v4 §6), because this changed and the old rule is gone:
 * THE CLOCK DECIDES THE MATCH. There is no score cap — UTraceSettings::ScoreToWin and the
 * bEndMatchAtScoreToWin switch that consumed it have both been deleted, not merely defaulted off.
 * The one early exit is the MERCY RULE: the instant either team's lead reaches
 * UTraceSettings::MercyRuleLead (8, or 0 to disable) the whole match ends and that team wins,
 * whichever half it happens in. See CheckMercyRule().
 *
 * NO SCORING MODE TO RESOLVE ANY MORE (spec v4 §7). This class used to work out which of the two
 * rulesets a map load was playing — travel URL, then command line, then Project Settings — and
 * publish the answer on the GameState for everything else to read. The endzone ruleset was removed
 * and goals is simply the game, so that whole resolve/publish/poll chain is gone.
 *
 * There is deliberately no separate "Warmup" state: the countdown is expressed through the shared
 * MatchEndServerTime deadline while the state stays WaitingForPlayers, which keeps ETraceMatchState
 * exactly as the contract defines it.
 *
 * HALVES AND SIDES (spec §1)
 * InProgress is not one period any more, it is HalvesPerMatch periods of HalfDuration separated by
 * an interval:
 *
 *     BeginMatch -> BeginHalf(1) --(half expires)--> BeginHalfTimeBreak
 *                                                     [sides switch HERE, immediately]
 *                                 --(interval ends)-> BeginHalf(2) --(expires)--> FinishMatch
 *
 * The interval reuses ETraceMatchState::InProgress and is flagged by ATraceGameState::
 * bHalfTimeBreak, so every existing "is the match running?" test keeps working unchanged while
 * anything that must genuinely pause can ask.
 *
 * The side switch is the reason ATraceGameState::TeamOnNegativeSide exists. This class is its only
 * writer, and ApplyTeamSides() is the only place that re-points the endzone triggers and the spawn
 * pads at their new owners. Nothing in this codebase may map a team to an end any other way.
 */
UCLASS(config = Game)
class TRACE_API ATraceGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ATraceGameMode();

	//~ Begin AGameModeBase interface

	/**
	 * The arena is built here, NOT in BeginPlay. UEngine::LoadMap runs:
	 *
	 *   InitializeActorsForPlay [ InitGame -> RouteActorInitialize -> PreInitializeComponents ]
	 *   -> SpawnPlayActor [ Login -> FindPlayerStart, PostLogin -> RestartPlayer -> FindPlayerStart ]
	 *   -> World::BeginPlay [ GameMode::BeginPlay ]
	 *
	 * Building in BeginPlay is two steps too late: no ATraceTeamPlayerStart exists yet, so
	 * AGameModeBase::FindPlayerStart logs "PATHS NOT DEFINED" and falls back to
	 * World->GetWorldSettings() — the world origin, which is inside the floor slab and the centre
	 * pedestal. The player still *gets* a pawn there, so every "has no pawn?" safety net passes
	 * while the pawn is depenetration-stuck, the camera is inside the pedestal (black screen) and
	 * every hitscan starts inside geometry.
	 *
	 * PreInitializeComponents rather than InitGame because Super() creates the GameState here, and
	 * the Core is published on it.
	 */
	virtual void PreInitializeComponents() override;

	/** Captures the URL options so "?bots=0" can turn the AI fill off for one session. */
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;

	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	/**
	 * AGameModeBase::FindPlayerStart short-circuits to AController::StartSpot (the spot used for
	 * the *first* spawn) whenever this returns true, which would pin every respawn to one pad.
	 * Returning false forces ChoosePlayerStart to run on every RestartPlayer, which is what the
	 * "furthest from any live enemy" rule needs.
	 */
	virtual bool ShouldSpawnAtStartSpot(AController* Player) override;
	//~ End AGameModeBase interface

	/**
	 * THE LENGTH OF THE MATCH-START COUNTDOWN, and the one thing about it a mode may answer
	 * differently. Seconds; zero or less means there is no countdown at all.
	 *
	 * UTraceSettings::WarmupDuration is the number for A MATCH — five seconds between "enough
	 * players" and the opening whistle, drawn as the HUD's "MATCH STARTS IN 5" banner. A MATCH is
	 * the only thing that number is about, and ATracePracticeGameMode returns 0 here (demo 27:
	 * "Don't have a match start timer in the practice range"). See the note on that override.
	 *
	 * *** A VIRTUAL AND NOT A UPROPERTY, DELIBERATELY. *** A per-mode config property is what
	 * ATracePracticeGameMode's HalfDuration already is, and it does not work: the constructor sets a
	 * fortnight, `[/Script/Trace.TraceGameMode] HalfDuration=480` in DefaultGame.ini is applied to
	 * the SUBCLASS too because config sections are inherited, and the range measurably runs 480 s
	 * halves — "One 480 s half" in its own start-up line. A virtual cannot be overridden by an ini,
	 * so the mode that says "no countdown" is the mode that gets no countdown.
	 *
	 * Public because it is a pure query with no side effects and the practice harness asserts on it:
	 * Trace.Practice.Verify compares the range's answer with GetDefault<ATraceGameMode>()'s, which
	 * is what stops "returns 0" passing because everything returns 0.
	 */
	virtual float GetWarmupSeconds() const;

	/**
	 * How long ONE HALF runs, in seconds. The match clock is built from this.
	 *
	 * *** A VIRTUAL FOR EXACTLY THE REASON GetWarmupSeconds() IS ONE, and this is the case that
	 * argument was originally written about. *** ATracePracticeGameMode's constructor sets
	 * HalfDuration to a fortnight so the range has no period structure, and it does not take:
	 * `[/Script/Trace.TraceGameMode] HalfDuration=480` in DefaultGame.ini is applied to the SUBCLASS
	 * too, because UE config sections are inherited. The range measurably ran 480 s halves - "One
	 * 480 s half" in its own start-up line - so it drew a counting-down match clock and would have
	 * ended after eight minutes. An ini cannot override a virtual, so the mode that says "no period
	 * structure" is the mode that gets none.
	 *
	 * The base still reads the config property, so `[/Script/Trace.TraceGameMode] HalfDuration` and
	 * the Trace.Match.HalfSeconds dev override both keep working for the real match exactly as before.
	 */
	virtual float GetHalfSeconds() const;

	/**
	 * Called by ATraceCharacter the moment it dies, while it is still possessed.
	 * Handles kill/death credit, dropping the Core, wiping the trail and scheduling the respawn.
	 * @param Cause "Bullet", "Trail" or "Fell".
	 */
	void NotifyCharacterDied(ATraceCharacter* Victim, AController* Killer, FName Cause);

	/**
	 * Where the Core may be dropped when its carrier leaves the match by any route.
	 *
	 * Spec v19 §4.1 made "where the carrier was" a place that can be off the map, so every drop
	 * that is triggered by the carrier disappearing has to ask this first. In bounds it is a no-op
	 * BY CONSTRUCTION — the clamp is gated on the bounds rule itself, never applied blind (the
	 * essay inside the function says why that distinction is load-bearing).
	 *
	 * Called from NotifyCharacterDied (death) and from Logout (disconnect/bot removal), which are
	 * the two ways a carrier stops existing. A third one added later belongs here too.
	 */
	FVector ClampCoreDropLocation(const FVector& Where) const;

	/** Called by ATraceEndzone when a carrier reaches the opposing endzone. */
	void NotifyScored(ETraceTeam ScoringTeam);

	// --- DEMO 29 §3. The Core is RETRIEVED at a restart, not handed out. --------------------------
	//
	// Two functions because the owner's note describes two different restarts, and conflating them is
	// the mistake they are shaped to prevent: a half starts CONTESTED at the centre, a goal restarts
	// with the conceding team favoured at their own end. Both funnel into
	// ATraceCore::KickoffContested, which is where the placement, the lockout and the replication
	// live; these two decide only WHERE and WHO.
	//
	// PUBLIC, ALONGSIDE NotifyScored, AND FOR THE SAME REASON. They are match-flow entry points, and
	// Trace.Core.KickoffProbe drives THESE rather than re-deriving "which end, which team" for
	// itself — a verification that computed its own expected answer would agree with itself forever.

	/**
	 * Half start (either half). Puts the Core on top of the centre octagon, held by nobody, with no
	 * team locked out — both sides climb for it.
	 *
	 * @p HalfIndex is read only for the mode-A fallback and the log: with no loose Core to leave
	 * lying about, mode A still grants to GetKickoffTeamForHalf() exactly as it always did.
	 */
	void KickoffCoreForHalf(int32 HalfIndex);

	/**
	 * After a goal. Puts the Core on the floor in front of the goal the team @p ScoringTeam just
	 * scored in defends, and locks @p ScoringTeam out of it for the reset window.
	 */
	void KickoffCoreAfterGoal(ETraceTeam ScoringTeam);

	// CheckEndzoneScoreForCarrier() WAS HERE, and it is gone with the endzone ruleset.
	//
	// It awarded a point when a character became the Core carrier while ALREADY standing inside an
	// endzone their team scores in — a completed pass to a teammate in the enemy endzone, or a kill
	// steal taken in there — because nothing overlaps-begins for a player who is not moving, so the
	// test had to be re-run against the POSSESSION event rather than the movement event. It read the
	// geometry off the ATraceEndzone volume itself, never from field bounds.
	//
	// That case still exists and is still covered: ATraceCore::GrantTo runs the same possession-event
	// test against the goal boxes (CheckGoalScore with From == To). The endzone volumes are furniture
	// now and never armed, so this function could only ever have found nothing.

	/**
	 * SPEC v25 §2. Gives @p NewPlayer the one-actor channel their pull button travels on.
	 *
	 * Called from PostLogin and destroyed in Logout. See ATraceCorePullRelay for why the message
	 * cannot go on ATraceCore itself: that actor is owned by the Core's HOLDER, and a pull is by
	 * definition sent by somebody who is not holding it. Idempotent — a reconnect that keeps its
	 * controller keeps its relay.
	 */
	void SpawnPullRelayFor(APlayerController* NewPlayer);

	void RegisterCharacter(ATraceCharacter* InCharacter);
	void UnregisterCharacter(ATraceCharacter* InCharacter);

	/**
	 * The server's roster of live characters. Used by the weapon's lag-compensated resolver and by
	 * the trail's trip test, so it is hot: entries are weak and may be stale — always .Get().
	 */
	const TArray<TWeakObjectPtr<ATraceCharacter>>& GetTrackedCharacters() const;

	ATraceCore* GetCore() const;

	/** Smaller team wins, ties go to Blue. Returns None when both teams are at PlayersPerTeam. */
	ETraceTeam PickTeamForNewPlayer() const;

	/** Living, non-spectating members of @p Team that currently possess an alive ATraceCharacter. */
	int32 CountLivingOnTeam(ETraceTeam Team) const;

	// ------------------------------------------------------------------------------------------
	// CHARACTER SELECTION (spec v14 §3)
	//
	// This class is the ONLY authority on who may have which character. Everything else — the select
	// screen, the player state's server RPC, the ability component — asks it. That is not style: the
	// rule is "no two players on the SAME TEAM may hold the same character", which is a statement
	// about the whole roster at one instant, and the only object that can see the whole roster at one
	// instant on the machine that decides things is the game mode.
	//
	// THE RACE, and why there is no lock. Two team-mates pressing ROCCO on the same frame send two
	// server RPCs. Those arrive as two ordinary function calls on the game thread, executed one after
	// the other. The first runs the uniqueness test against a roster in which Rocco is free, takes
	// it, and writes it. The second runs the same test against a roster in which Rocco is now taken,
	// and is refused. "First request wins" is therefore a property of the execution model rather than
	// of any tie-break code — which is exactly why it is implemented here and not in the UI, where
	// two clients cannot see each other at all.
	// ------------------------------------------------------------------------------------------

	/**
	 * Whether this match has characters at all.
	 *
	 * THREE ways to be off, and all three must be, because they answer different questions:
	 *   * MODE A (spec v14 §2, verbatim: "Do not implement abilities or characters into what was game
	 *     mode a") — frozen, and this is the hard gate that keeps it that way whatever the settings
	 *     say. Read from the GameState, so it follows a live A/B mode switch rather than latching.
	 *   * the SETTINGS TOGGLE (spec v14 §3, verbatim: "Include a toggle in game settings to turn off
	 *     all characters"), resolved once in InitGame from the travel URL / command line / the saved
	 *     user setting / this class's own config default. See ResolveCharactersEnabled.
	 *   * no authority. A client asking this is asking a question only the server can answer.
	 */
	bool AreCharactersEnabled() const;

	/**
	 * THE arbitration entry point. Server only. Called by ATracePlayerState::ServerRequestCharacter.
	 *
	 * Refuses — with a reason — rather than silently ignoring, because a select screen that gets no
	 * answer is a select screen the player presses again forever. On success the pick is written to
	 * the player state, locked, and the select screen is closed.
	 */
	ETraceCharacterPickResult RequestCharacter(ATracePlayerState* Requester, uint8 RequestedCharacter);

	/**
	 * True when ANY team-mate of @p Team already holds @p CharacterId. @p Except is skipped.
	 *
	 * Bots count. They used to be unable to hold anything (spec v14 §3) so this read "NON-BOT"; spec
	 * v15 §2 reverses that, and the underlying UTraceAbilityComponent::FindTeammateHolding has always
	 * walked every player state on the team rather than filtering — which is why the rule extended to
	 * bots without a line changing here.
	 */
	bool IsCharacterTakenOnTeam(ETraceTeam InTeam, uint8 CharacterId, const ATracePlayerState* Except) const;

	/**
	 * The lowest-numbered character no team-mate holds, for the HUMAN auto-assign. TraceCharacterRoster::
	 * NoneId when the team somehow has more players than there are characters.
	 *
	 * Deterministic rather than random on purpose: an auto-assign is already a thing the player did
	 * not choose, and making it also unpredictable makes it impossible to reproduce in a bug report.
	 *
	 * NOT what the spec v15 §2 bot fill uses. That one asks UTraceAbilityComponent::
	 * PickRandomFreeCharacterFor, because "randomly chosen" is the spec's own word for it and because
	 * five bots taking the roster in order every single match is the thing a player would notice
	 * first. The asymmetry is deliberate; see PollCharacterSelect.
	 */
	uint8 FindFreeCharacterForTeam(ETraceTeam InTeam, const ATracePlayerState* Except) const;

	/**
	 * Clears every player's activated cooldown. Server only, idempotent.
	 *
	 * Spec v14 §5 named half time and that is still where the NAME comes from, but D30-RESETS (b)
	 * widened the rule to "off cooldown at the beginning of each half", so this now runs from
	 * BeginHalf() as well as from BeginHalfTimeBreak(). The half-time call is not redundant: it is
	 * what makes the meter snap to ready at the top of the interval rather than twelve seconds later.
	 * The BeginHalf() call is what reaches an ability fired in WARM-UP, in front of a first half that
	 * has no interval before it.
	 */
	void ResetAbilityCooldownsForHalfTime();

#if !UE_BUILD_SHIPPING
	/**
	 * Trace.Characters.Verify — the scripted proof for spec v14 §3.
	 *
	 * Drives the three cases organic play will not produce on demand: two same-team requests for one
	 * character in the same frame, the same request from an ENEMY (which must SUCCEED — mirroring is
	 * legal), and the auto-assign timeout. Logs a PASS/FAIL per assertion.
	 *
	 * @param bRedArm true removes this slice's uniqueness test FOR THE DURATION OF THE RUN, so the
	 *                team-mate assertion must FAIL. A harness that cannot be made to go red proves
	 *                nothing, and this project has already lost two passes to exactly that.
	 *
	 * The arm is an ARGUMENT rather than a cvar set from the command line, and that is a measured
	 * decision: the first attempt shipped it as `-dpcvars=Trace.Characters.EnforceSelectRules=0`
	 * and the run came back reporting "uniqueness rule ENFORCED" — the value never reached the cvar,
	 * so the red arm silently ran green and would have been recorded as evidence. `Trace.Characters.
	 * Verify red` cannot fail that way: the flag is set inside the same function that reads it, and
	 * the header line prints which arm actually ran.
	 */
	void StartCharacterSelectVerify(bool bRedArm);

	/**
	 * Trace.Characters.BotVerify — the scripted proof for spec v15 §2 ("bots pick characters").
	 *
	 * Four things organic play will not show you inside one match: that a bot does NOT pick while a
	 * human on its team is still choosing, that it DOES pick once they are done, that its pick is
	 * unique on the team, and that an idle human resolves through the select timeout instead of
	 * deadlocking the bots behind them. Also asserts that the "disable characters" toggle puts bots
	 * back on the Mannequin.
	 *
	 * @param bRedArm true removes BOTH halves of the rule for the run — this class's ordering and
	 *                random-free fill, AND UTraceAbilityComponent's own refusals. Both, because the
	 *                rule is enforced twice and the v14 arm proved that reaching one of two
	 *                enforcement points produces a red run that reports green.
	 */
	void StartBotCharacterVerify(bool bRedArm);

	/** Trace.Characters.Dump — the whole roster, who holds what, and every cooldown. */
	void DumpCharacterState() const;
#endif

#if !UE_BUILD_SHIPPING
	/**
	 * Trace.HalfTime.Verify — the scripted proof for spec v9 §11.
	 *
	 * The organic evidence (start a short half, watch the clock expire, watch play continue, watch
	 * the whistle go on the next turnover) only ever exercises the case that FIRES. It says nothing
	 * about the two cases that must NOT fire, and those are where §11 can go wrong in the way a
	 * player would actually notice: a half that ends on a team-mate's pass is the same "cut off in
	 * the middle of a run" complaint the note was written about, and a cap that never fires turns a
	 * stalemate into a match that does not end.
	 *
	 * @param Scenario which of the three runs. See ETraceHalfTimeVerifyScenario.
	 */
	void StartHalfTimeVerify(ETraceHalfTimeVerifyScenario Scenario);
#endif

	// ------------------------------------------------------------------------------------------
	// Match structure (spec §1). All `config`, so DefaultGame.ini tunes the whole format.
	// ------------------------------------------------------------------------------------------

	/**
	 * Seconds of play in ONE half. Two of these is the match.
	 *
	 * 480 = 8 minutes (spec v10 §7, down from 10). THE INI WINS: Config/DefaultGame.ini carries
	 * `HalfDuration=480.000000` under [/Script/Trace.TraceGameMode] and that is the shipped value.
	 * This initialiser is only what a config-less run would get, and it is kept in agreement so the
	 * header cannot be read as documentation of a number the game does not use. Confirm from a
	 * running game ("1ST HALF started: 480s"), never from this line.
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	float HalfDuration = 480.f;

	/** Periods of play per match. 2 is the spec; 1 restores the old single-period behaviour. */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	int32 HalvesPerMatch = 2;

	/** Length of the interval between halves. Long enough to read the banner, short enough to sit through. */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	float HalfTimeBreakDuration = 12.f;

	/**
	 * Seconds between death and respawn. Spec v4 §5 sets this to 2 (it was 3).
	 *
	 * Config/DefaultGame.ini pins this under [/Script/Trace.TraceGameMode] as well, and the ini
	 * wins — so this literal is what a reader sees, not what a match uses. Keep the two equal.
	 *
	 * Deliberately here and not in UTraceSettings: the respawn delay is a RULE, this class is the
	 * rules, and UTraceSettings::RespawnDelay has no enforcement behind it. Clients no longer need
	 * to know it either — the deadline replicates on ATracePlayerState::RespawnEndServerTime.
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	float RespawnDelay = 2.f;

	/**
	 * "Team A" in the doc's phrase "the core starts with Team A in the first half". Team B (its
	 * opponent) starts the second half, and so on alternately if HalvesPerMatch is ever raised.
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	ETraceTeam FirstHalfCoreTeam = ETraceTeam::Blue;

	/** Who receives the Core after a goal. See ECoreKickoffMode. */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	ECoreKickoffMode KickoffMode = ECoreKickoffMode::ScoredOnTeam;

	/** Points awarded for wiping the whole enemy team at once (spec §1). */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	int32 WipeBonusPoints = 2;

	// bEndMatchAtScoreToWin AND ITS SCORE CAP ARE GONE (spec v4 §6, verbatim: "Remove score to win 5
	// — Keep the match timer as the win condition, but add a mercy rule").
	//
	// Deleted rather than defaulted off, along with UTraceSettings::ScoreToWin: a switch that can
	// reintroduce a deleted win condition is exactly the kind of thing that gets flipped on by an
	// old .ini and quietly ends matches at 5-4. The mercy rule replaces it, lives in
	// UTraceSettings::MercyRuleLead so it retunes live, and is enforced by CheckMercyRule().

	/**
	 * Spec §1: players respawn in their OWN team's endzone rather than on the generic pads in front
	 * of it. The pads are built by this class (BuildEndzoneSpawnPads) because their ownership has to
	 * flip at half time and the arena builder's are baked at build time.
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	bool bRespawnInOwnEndzone = true;

	// ------------------------------------------------------------------------------------------
	// Characters (spec v14 §3)
	// ------------------------------------------------------------------------------------------

	// NOTE: there is no bCharactersEnabled here. The toggle spec v14 §3 asks for lives on
	// UTraceSettings::bCharactersEnabled, which is what UTraceAbilityComponent::AreCharactersEnabled
	// reads and therefore what every ability in the game actually obeys. ResolveCharactersEnabled()
	// below resolves the URL / command line / saved setting and writes the answer THERE, exactly as
	// TraceScoring::ApplyToSettings does for the A/B mode. A second copy on this class would be a
	// switch that could disagree with the one the framework honours.

	/**
	 * Seconds a player gets to pick before one is assigned to them (spec v14 §3 [ASSUMPTION]:
	 * "a timeout that auto-assigns a free character, so one idle player cannot stall the match").
	 *
	 * Long enough to read five cards, short enough that an AFK player costs one warm-up. Zero or less
	 * disables the auto-assign entirely, which is a legitimate configuration for a private match and
	 * a terrible one for a public server.
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Characters")
	float CharacterSelectTimeout = 30.f;

protected:
	/** Spawned at the origin if the level does not already contain an ATraceArenaBuilder. */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Classes")
	TSubclassOf<ATraceArenaBuilder> ArenaBuilderClass;

	UPROPERTY(EditDefaultsOnly, Category = "Trace|Classes")
	TSubclassOf<ATraceCore> CoreClass;

	/** Possesses the same ATraceCharacter humans do, so every game rule applies to bots unchanged. */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Classes")
	TSubclassOf<ATraceBotController> BotControllerClass;

	/** A player start with a live character inside this radius counts as occupied. */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Match")
	float StartOccupiedRadius = 150.f;

	UPROPERTY(Transient)
	TObjectPtr<ATraceArenaBuilder> ArenaBuilder = nullptr;

	// --- Match phase machine ---------------------------------------------------------------
	void CheckMatchStartConditions();
	void StartWarmup();
	void CancelWarmup();
	void BeginMatch();

	/**
	 * Ends the match, records WHY, and starts the results-screen countdown.
	 *
	 * @param WinningTeam ETraceTeam::None is a genuine draw (only reachable on a clock finish).
	 * @param Reason      Clock or Mercy. Replicated to the results screen — spec §6 asks for the
	 *                    distinction explicitly, because a mercy win and a clock win produce the
	 *                    same scoreboard and are not the same result.
	 */
	void FinishMatch(ETraceTeam WinningTeam, ETraceMatchEndReason Reason);

	// --- Win conditions (spec v4 §6) -----------------------------------------------------------

	/**
	 * The MERCY RULE. Verbatim: "add a mercy rule, where the game will end if one team leads by 8
	 * points (granting an immediate win to the team leading)."
	 *
	 * Call this after ANY change to the scoreboard — a capture, a wipe bonus, anything added later.
	 * It is a state test on the current lead, not an event handler, so it cannot be desynchronised
	 * from the score by whoever forgot to pass it the right argument.
	 *
	 * SCOPE, per the spec's stated assumption: the whole match, including mid-first-half, and it
	 * ends the MATCH outright rather than ending only the half. A mercy win in the first half means
	 * the second half and its side switch never happen — that is the point of a mercy rule.
	 *
	 * The threshold is UTraceSettings::MercyRuleLead, read here at the point of use so it retunes
	 * with PIE running. Zero disables the rule entirely and the clock becomes the only end.
	 *
	 * @return true when the match was ended. Callers must not continue their own work (a kickoff, a
	 *         field reset) once this returns true — the match is over.
	 */
	bool CheckMercyRule(const TCHAR* Cause);

	// --- Halves ------------------------------------------------------------------------------

	/**
	 * Starts period @p HalfIndex: assigns sides, resets the field, restarts the clock and puts the
	 * Core on top of the centre octagon for both teams to climb for. Idempotent enough to be safe
	 * from a timer.
	 *
	 * IT NO LONGER HANDS THE CORE TO ANYBODY — DEMO 29 §3(a) replaced the grant with a placement.
	 * See KickoffCoreForHalf().
	 */
	void BeginHalf(int32 HalfIndex);

	/**
	 * The half clock ran out.
	 *
	 * SPEC v9 §11 CHANGED WHAT THIS DOES. It no longer ends the period. Unless deferral is switched
	 * off (UTraceSettings::bDeferPeriodEndToPlayBreak) it ARMS the period end and lets play carry on;
	 * the whistle goes in ResolvePendingPeriodEnd() at the next dead ball. EndPeriodNow() is the old
	 * body, and is still what every path eventually calls.
	 */
	void HandleHalfExpired();

	// --- Deferred half time / full time (spec v9 §11) ------------------------------------------
	//
	// Verbatim: "Change halftime to trigger after the time runs out and the current play ends, so
	// that it doesn't cut people off in the middle of a run E.g. the ball drops in game mode b, any
	// turnover of the core happens between teams, or a goal is scored, then half time triggers."
	//
	// WHAT COUNTS AS A DEAD BALL, and why each one:
	//   * A GOAL. Explicit in the note. Fired from NotifyScored(), after the score and after the
	//     mercy check, and instead of the kickoff — there is no point kicking off into a whistle.
	//   * A TURNOVER BETWEEN THE TEAMS. Also explicit. Detected as a change of the Core's holder
	//     TEAM, which is deliberately not the same thing as a change of holder: a completed pass to
	//     a team-mate keeps the team and play continues, which is the spec's stated assumption. A
	//     carrier's death that hands the Core to the other side changes the team and therefore is a
	//     break, which is the other stated assumption.
	//   * MODE B: THE CORE COMING TO REST ON THE GROUND. "the ball drops". Usually the same instant
	//     as the surface turnover (spec v7 §4), but tested separately so a landing that fails to
	//     find a receiver still ends the half rather than leaving the whistle waiting on a Core
	//     nobody is near.
	//
	// WHAT DOES NOT COUNT: a within-team pass, a within-team mode-B recovery, a kill that does not
	// move the Core, a wipe bonus. Play continues.
	//
	// THE MERCY RULE IS NOT AFFECTED and must not be. It is a mercy, not a play boundary: it still
	// ends the match on the frame the lead reaches the threshold, in the middle of a run if that is
	// where it happens. FinishMatch() clears any pending whistle on its way through, so a mercy win
	// taken while a half time was pending cannot leave a timer armed under the results screen.

	/** Arms the deferred whistle: pins the clock at 0:00, starts the poll and the hard cap. */
	void BeginPendingPeriodEnd();

	/** Clears the pending state and both of its timers. Idempotent; safe from anywhere. */
	void ClearPendingPeriodEnd();

	/**
	 * Blows the deferred whistle. Clears the pending state first, then ends the period.
	 * @param Cause "goal" / "turnover" / "core down" / "hard cap" — logged, and only logged.
	 */
	void ResolvePendingPeriodEnd(const TCHAR* Cause);

	/**
	 * Looks for a dead ball. Runs on a short repeating timer, and only while a whistle is pending.
	 *
	 * A POLL RATHER THAN A CALLBACK, on purpose: ATraceCore is another ownership slice this pass,
	 * and the possession events this needs (a grant, a landing) are not published as delegates. A
	 * 20 Hz read of two accessors on one actor, for at most PeriodEndMaxDeferSeconds once or twice
	 * a match, is cheaper than the coordination — and it cannot miss an event by being registered
	 * too late, which a callback added to a foreign file could.
	 */
	void PollPendingPeriodEnd();

	/** The old HandleHalfExpired body: the interval, or full time, depending on HalvesPerMatch. */
	void EndPeriodNow(const TCHAR* Cause);

	/**
	 * Stops the clock, switches ends IMMEDIATELY (so the interval is spent looking at the field you
	 * are about to attack) and puts everyone on their new spawn pads with no Core in play.
	 */
	void BeginHalfTimeBreak();

	/** Interval over: rolls into the next half. */
	void EndHalfTimeBreak();

	/** Sides for @p HalfIndex: Blue on -X in odd halves, swapped in even ones. */
	ETraceTeam GetNegativeSideTeamForHalf(int32 HalfIndex) const;

	/**
	 * The team associated with the start of @p HalfIndex (FirstHalfCoreTeam, alternating).
	 *
	 * DEMO 29 §3 DEMOTED THIS FROM A RULE TO A FALLBACK, and the old comment ("the team the Core is
	 * granted to") is no longer true in the shipped mode. A half now starts with the Core on top of
	 * the octagon and NOBODY holding it — the owner's rule is that teams climb for it. This answer is
	 * still what mode A grants to (mode A has no loose Core to climb for), what the contested kickoff
	 * falls back to if nobody reaches the deck at all, and what the half-start log line names.
	 */
	ETraceTeam GetKickoffTeamForHalf(int32 HalfIndex) const;

	/**
	 * Re-points every side-dependent actor at its new owner: the two ATraceEndzone triggers and
	 * every ATraceTeamPlayerStart, keyed off which side of the field's centre they physically sit
	 * on. Publishes the assignment on the GameState first, because that is what everything else
	 * (bots, HUD) reads.
	 */
	void ApplyTeamSides(ETraceTeam TeamOnNegativeSide);

	/**
	 * Spawns five respawn pads per side INSIDE each endzone, mid-depth, reusing the arena builder's
	 * own pads for lateral spread, height and facing. Tagged so ChoosePlayerStart can prefer them
	 * and so a rebuild never doubles them up. Idempotent.
	 */
	void BuildEndzoneSpawnPads();

	/** True when a pawn-sized capsule at @p Location would be stuck in world geometry. */
	bool IsSpawnLocationBlocked(const FVector& Location) const;

	// --- Core possession ----------------------------------------------------------------------
	//
	// EVERY call this class makes into ATraceCore goes through these two functions. The Core is
	// being rewritten from an actor with physics into a replicated possession status, so keeping the
	// coupling to exactly two call sites is what makes that swap a two-function edit here.

	/** Takes the Core out of play: no holder, parked at the arena centre. */
	void ReleaseCore();

	/** Kicks off to @p Team: the Core is released and granted to one of their living players. */
	void GrantCoreToTeam(ETraceTeam Team);


	// --- Wipe bonus (spec §1) ------------------------------------------------------------------

	/**
	 * Called after any death. Awards WipeBonusPoints to the opposing team the instant @p DeadTeam
	 * has nobody left alive, and latches so the same wipe cannot pay twice.
	 */
	void EvaluateWipeBonus(ETraceTeam DeadTeam);

	/** Clears @p Team's wipe latch once one of them is breathing again, re-arming the bonus. */
	void ClearWipeLatchIfAlive(ETraceTeam Team);

	bool IsWipeLatched(ETraceTeam Team) const;
	void SetWipeLatched(ETraceTeam Team, bool bLatched);

	/**
	 * Travels back to the title screen once the post-match results screen has had its time.
	 *
	 * The match used to end and simply sit there, which — with captures landing seconds apart —
	 * read to a player as the game restarting itself at random. PostMatch now has a defined exit,
	 * and TraceMatchFlow::PostMatchDuration is the single number both this timer and the HUD's
	 * on-screen countdown are driven from.
	 */
	void ReturnToMainMenu();

	// --- Helpers ---------------------------------------------------------------------------

	/** Finds or spawns the arena builder and makes it build. Idempotent; needs no GameState. */
	void EnsureArenaBuilt();

	/** Spawns the Core and publishes it on the GameState. Idempotent; requires the GameState. */
	void SpawnCoreIfNeeded();

	void AssignTeamIfNeeded(APlayerController* NewPlayer);

	/** Every controller with a pawn — humans AND bots — back onto a freshly chosen team pad. */
	void ResetPlayersToSpawns();

	/**
	 * D30-RESETS (a), verbatim: "momentum should reset when halves switch and when you respawn."
	 *
	 * Puts one controller's pawn back to the movement state a freshly spawned pawn has — velocity AND
	 * the whole predicted kit (dash, slide, ledge grace, wall-jump window, surf ride, surf exit carry)
	 * — ON BOTH MACHINES. See UTraceCharacterMovementComponent::ResetMomentum for why zeroing Velocity
	 * on the server is not a reset but a correction the owning client immediately fights, and why
	 * StopMovementImmediately() alone left a mid-dash pawn to put its own velocity straight back.
	 *
	 * Server only, idempotent, safe on a controller with no pawn. Called from exactly two places:
	 * ResetPlayersToSpawns() for the half switch and the kickoff/goal resets, and RestartPlayerFresh()
	 * for every respawn.
	 *
	 * @param Why short label for the [Resets] log line.
	 */
	void ResetMomentumFor(AController* Controller, const TCHAR* Why);

	// --- Bots --------------------------------------------------------------------------------

	/**
	 * Brings both teams up to (or back down to) PlayersPerTeam with AI.
	 *
	 * Idempotent and self-correcting in both directions, which is what makes it safe to call from
	 * BeginPlay, after a login and after a logout: a second human joining a listen server pushes a
	 * bot out rather than making an 11th player.
	 */
	void UpdateBotFill();

	/** Runs UpdateBotFill() next tick. PlayerArray is only truthful after Login/Logout complete. */
	void ScheduleBotFill();

	/** Config AND "?bots=0". The URL wins, so a session can be forced human-only without editing ini. */
	bool AreBotsEnabled() const;

	// --- Character selection -------------------------------------------------------------------

	/**
	 * Resolves the characters-on/off toggle for this map load and latches it. See bCharactersEnabled
	 * for the priority order. Called once, from InitGame, because that is the last point at which the
	 * raw travel URL is guaranteed to be what the session opened with.
	 */
	void ResolveCharactersEnabled(const FString& Options);

	/**
	 * The whole select-screen state machine, on a 4 Hz timer.
	 *
	 * A POLL RATHER THAN EVENTS, and deliberately so. Three separate things have to be true for a
	 * player to be offered a selection — they are human, they are on a team, and characters are on —
	 * and each of those becomes true on its own schedule: PostLogin, the balancer, a live A/B mode
	 * switch. An event-driven version would need a hook on each and would still miss the listen-server
	 * host, who logs in during map load before the GameState exists. This is idempotent and
	 * self-correcting in both directions (it closes screens as well as opening them), which is the
	 * same shape UpdateBotFill() has and for the same reason.
	 */
	void PollCharacterSelect();

	/** Spawns one bot on @p Team, names it, and gives it a pawn. Null on failure. */
	ATraceBotController* SpawnBotForTeam(ETraceTeam Team);

	/** Destroys one bot (and its pawn) on @p Team. Used when a human takes the slot back. */
	void RemoveOneBotFromTeam(ETraceTeam Team);

	/** Drops destroyed bots from the roster. */
	void CompactBots();

	/**
	 * If both teams are full of bots and a human is arriving, evict a bot so the human gets a team
	 * rather than spawning teamless (which would keep them out of scoring and friendly-fire tests).
	 */
	void FreeSlotForHuman();

	/** Living bots currently assigned to @p Team. */
	int32 CountBotsOnTeam(ETraceTeam Team) const;

#if !UE_BUILD_SHIPPING
	/** -TraceBotDebug: logs the whole roster with positions every few seconds. Verification aid. */
	void LogBotRoster();
#endif

	/** Respawns even when a dead pawn is still possessed; teleports the pawn if it is still alive. */
	void RestartPlayerFresh(AController* Controller);

	// NOTE: there used to be an EnsurePlayersHavePawns() here, meant to rescue the listen-server host
	// who logs in before the world begins play. It could never fire: the engine's WorldSettings
	// fallback always hands out a pawn, so "GetPawn() == nullptr" was never true for the case it was
	// written for. ResetPlayersToSpawns() replaces it — it relocates a misplaced *living* player as
	// well as restarting a pawnless one.

	/** Timer callback; the weak payload makes a disconnect during the respawn delay harmless. */
	void RespawnController(TWeakObjectPtr<AController> ControllerPtr);

	/** Cancels and forgets a controller's pending respawn, if it has one. */
	void ClearPendingRespawn(AController* Controller);

	int32 GetActivePlayerCount() const;
	ATraceGameState* GetTraceGameState() const;

	static ETraceTeam GetTeamForController(const AController* Controller);

	bool IsStartOccupied(const AActor* Start, const AController* ForPlayer) const;

	/** Squared distance to the closest living enemy, or a very large value when there are none. */
	float DistSqToNearestEnemy(const FVector& Location, ETraceTeam Team) const;

	void CompactTrackedCharacters();

private:
	TArray<TWeakObjectPtr<ATraceCharacter>> TrackedCharacters;

	/**
	 * At most one live respawn timer per controller.
	 *
	 * Without this, a player who dies, gets restarted early by a capture reset, then dies again
	 * would have two timers in flight, and the older (shorter-remaining) one would cut the second
	 * respawn short — RespawnDelay would silently stop being honoured. Setting a new timer clears
	 * the controller's previous handle first.
	 */
	TMap<TWeakObjectPtr<AController>, FTimerHandle> PendingRespawns;

	/** Every bot this GameMode has spawned. Weak: a bot destroyed elsewhere must not be resurrected. */
	TArray<TWeakObjectPtr<ATraceBotController>> Bots;

	/**
	 * Every controller Logout() has already run for. A second call for the same controller returns
	 * immediately.
	 *
	 * RemoveOneBotFromTeam calls Logout(Bot) by hand and then Bot->Destroy(), and
	 * AController::Destroyed() calls GameMode->Logout(this) a SECOND time for any controller
	 * carrying a PlayerState — see the essay at that call site. Note the shape: those two calls are
	 * sequential, not nested, so an "in flight" set that cleared itself on exit would catch nothing.
	 * The old defence was "every step happens to be idempotent", which is a property of today's body
	 * rather than a rule the next edit inherits. This set makes it a rule.
	 *
	 * Never cleared during a match: a controller that has logged out is destroyed and never logs in
	 * again, and stale weak pointers are pruned on insert. Weak because the second call arrives from
	 * AController::Destroyed(), i.e. while the controller is being torn down; the pointer is only
	 * ever compared, never dereferenced.
	 */
	TSet<TWeakObjectPtr<AController>> LogoutsInFlight;

	/** Monotonic, so two bots never share a scoreboard name even after churn. */
	int32 NextBotNumber = 1;

	/** From "?bots=N": -1 when the option is absent, 0 to disable, >0 as a total bot cap. */
	int32 BotCountFromURL = -1;

	/** The endzone respawn pads this class spawned, so a re-run does not double them. */
	TArray<TWeakObjectPtr<ATraceTeamPlayerStart>> EndzoneStarts;

	/**
	 * Wipe-bonus latches, indexed by team. A wipe pays once; the latch clears the moment one of the
	 * wiped team is alive again, which is what "do not re-award until at least one has respawned"
	 * means in code.
	 */
	bool bBlueWipeLatched = false;
	bool bOrangeWipeLatched = false;

	/** Team the Core was granted to at the last kickoff. Only read by ECoreKickoffMode::AlternateTeams. */
	ETraceTeam LastKickoffTeam = ETraceTeam::None;

	/**
	 * World time of the last score this class actually processed, for the one-score-at-a-time guard
	 * in NotifyScored.
	 *
	 * There are two ways in: the goal volume's own trigger/poll, and ATraceCore's swept crossing
	 * test. A carrier running through the mouth satisfies both within the same tenth of a second,
	 * and a capture must never pay twice. Seeded far in the past (world time starts at 0) so the
	 * first score of a match is never swallowed.
	 */
	float LastScoreProcessedWorldTime = -10000.f;

	/**
	 * Spec v9 §11. The Core holder's TEAM as of the last poll, so a change of team can be told from
	 * a change of holder. ETraceTeam::None means "nobody holds it right now" and is deliberately NOT
	 * treated as a turnover on its own — a throw passes through None on its way from one team to the
	 * next, and the loose flight in between is not a dead ball.
	 *
	 * Seeded when the whistle is armed, not at match start: it only ever has to describe the window
	 * during which a whistle is pending.
	 */
	ETraceTeam PendingEndLastHolderTeam = ETraceTeam::None;

	FTimerHandle WarmupTimerHandle;
	FTimerHandle MatchTimerHandle;
	FTimerHandle HalfTimeTimerHandle;

	/** Spec v9 §11. Looping while a whistle is pending; drives PollPendingPeriodEnd(). */
	FTimerHandle PendingPeriodEndPollHandle;

	/** Spec v9 §11. One-shot guard rail: fires the whistle if no dead ball ever arrives. */
	FTimerHandle PendingPeriodEndCapHandle;
	FTimerHandle BotFillTimerHandle;
	FTimerHandle ReturnToMenuTimerHandle;

	/** Drives PollCharacterSelect(). Looping, quarter second, for the whole session. */
	FTimerHandle CharacterSelectPollHandle;

	/**
	 * One-shot latch for "this team has more players than the roster has characters" (spec v15 §2's
	 * bot fill). Not shipped as a warning per poll: the fill re-enters that branch four times a
	 * second for as long as the bot has nothing, and a log line at 4 Hz for a whole match is how a
	 * real warning gets scrolled past. Server-only and never replicated.
	 */
	bool bWarnedBotRosterExhausted = false;

	/**
	 * One-shot latch for "the bot fill is waiting on a human and the select timeout is switched off".
	 * Same 4 Hz reasoning as bWarnedBotRosterExhausted. Server-only, never replicated.
	 */
	bool bWarnedBotFillHasNoTimeout = false;

	/**
	 * One-shot latch for spec v28 §9b's "the bot fill is waiting for the whistle" line. Same 4 Hz
	 * reasoning again — the warm-up is a few seconds of poll passes and every one of them would
	 * otherwise say it. Server-only, never replicated.
	 */
	bool bLoggedBotFillHeldForWarmup = false;

#if !UE_BUILD_SHIPPING
	FTimerHandle BotDebugTimerHandle;

	// --- -TraceTripTest: scripted verification of the two rules this pass fixed ------------------
	//
	// Off unless the switch is on the command line, and compiled out of a shipping build entirely.
	// It exists because both fixes are about cases that are rare in organic play but decide matches:
	// dashing through the trace a turnover left behind, and a pass completed to a teammate who is
	// already standing in the enemy endzone. Waiting for ten bots to stumble into them is not
	// evidence. This drives each one on purpose and logs the outcome.
	FTimerHandle VerifyTimerHandle;

	/** Step counter within one scripted scenario; see RunVerificationStep(). */
	int32 VerifyStep = 0;

	/** How many trace-dash scenarios have been run, so "reliably" means more than once. */
	int32 VerifyIteration = 0;

	/** The player whose trace is being dashed through, and the enemy doing the dashing. */
	TWeakObjectPtr<ATraceCharacter> VerifyTraceOwner;
	TWeakObjectPtr<ATraceCharacter> VerifyTripper;

	/** Far side of the scripted dash, applied one frame after the near side so the sweep crosses. */
	FVector VerifyDashEnd = FVector::ZeroVector;

	/** Score snapshot taken before the scripted endzone pass, so the point can be measured. */
	int32 VerifyScoreBefore = 0;

	/** Drives the scenarios. Fires every frame-ish; each call advances one step. */
	void RunVerificationStep();

	// --- Trace.HalfTime.Verify (spec v9 §11) ---------------------------------------------------

	/** One step of the deferred-whistle scenario. See StartHalfTimeVerify(). */
	void RunHalfTimeVerifyStep();

	FTimerHandle HalfTimeVerifyHandle;
	int32 HalfTimeVerifyStep = 0;
	int32 HalfTimeVerifyPassed = 0;
	int32 HalfTimeVerifyFailed = 0;

	/** Which of the three scenarios is running. See StartHalfTimeVerify(). */
	ETraceHalfTimeVerifyScenario HalfTimeVerifyScenario = ETraceHalfTimeVerifyScenario::TurnoverVsPass;

	/**
	 * How many times PollPendingPeriodEnd() has actually examined the Core. Monotonic for the
	 * process; only ever read as a DIFFERENCE.
	 *
	 * The scripted scenarios wait on this instead of on wall-clock. Several headless instances share
	 * one machine here, and the engine's frame-rate smoothing clamps the world delta, so a step timed
	 * at 0.2 s can arrive before the 20 Hz detector has run once: the positive case then reports a
	 * FAIL that is really "the detector never got a tick", and the negative case reports a PASS that
	 * proves nothing. Both of those have burned this project already.
	 */
	int32 PendingPeriodEndPollCount = 0;

	/** PendingPeriodEndPollCount as of the scripted event the current step is waiting on. */
	int32 HalfTimeVerifyPollMark = 0;

	/** Polls a scripted step will wait for before it gives a verdict. 3 = two clean detector passes. */
	static constexpr int32 HalfTimeVerifyPollsPerStep = 3;

	/** Steps a scripted step will spend waiting for those polls before failing on the timeout. */
	static constexpr int32 HalfTimeVerifyMaxWaitSteps = 100;

	/** Steps the current scripted step has spent waiting. Reset when the step number moves on. */
	int32 HalfTimeVerifyWaitSteps = 0;

	/** The step the wait budget above belongs to. See RunHalfTimeVerifyStep(). */
	int32 HalfTimeVerifyLastStep = -1;

	/** The carrier the scenario started from, so "a team-mate" and "an enemy" mean something. */
	TWeakObjectPtr<ATraceCharacter> HalfTimeVerifyHolder;

	// --- Trace.Characters.Verify (spec v14 §3) --------------------------------------------------

	int32 CharacterVerifyPassed = 0;
	int32 CharacterVerifyFailed = 0;

	/** One assertion of the scripted proof. Logs, counts, and returns @p bCondition unchanged. */
	bool ReportCharacterVerify(bool bCondition, const TCHAR* What);
#endif
};
