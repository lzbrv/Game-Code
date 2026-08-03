// Trace — every runtime-tunable number in one place.
//
// UTraceSettings is a UDeveloperSettings, so it shows up under Project Settings and persists to
// Config/DefaultGame.ini (defaultconfig). Read it from anywhere with UTraceSettings::Get().
//
// Rule for the rest of the codebase: never hardcode a gameplay constant that lives in this
// table. Read it through Get() at the point of use — designers change these live, and the CDO
// is refreshed by the config system, so caching a copy in a constructor will go stale.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"   // module: DeveloperSettings
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"     // GetDefault<>

#include "TraceTypes.h"                 // ETrailLethality

#include "TraceSettings.generated.h"

/**
 * How hard the bots play. Easy is the default everywhere: a newcomer's first match must be
 * winnable, and the shipped baseline used to kill an idle human in under two seconds.
 *
 * The menu sets this by putting "?difficulty=easy|normal|hard" on the travel URL (see
 * TraceDifficulty::UrlOption). ATraceBotController reads that option off the game mode's
 * OptionsString once and calls UTraceSettings::SetBotDifficulty, so nothing in the UI layer has to
 * reach into this class — and a headless run can force a difficulty with "-difficulty=hard".
 */
UENUM()
enum class EBotDifficulty : uint8
{
	Easy   = 0,
	Normal = 1,
	Hard   = 2
};

/**
 * The whole bot skill curve, one struct per difficulty.
 *
 * WHY A STRUCT AND NOT A PILE OF Bot*_Easy SCALARS
 * There are eighteen knobs here and three difficulties. Written flat that would be fifty-four
 * properties, and every future knob would be three more edits in three places. As a struct the
 * config file reads as three lines — BotEasy=(ReactionTimeSeconds=0.90,...) — and adding a knob is
 * one field plus three numbers.
 *
 * Only EditAnywhere on the members: `config` belongs on the FTraceBotProfile *property* in
 * UTraceSettings, which is what makes the whole struct round-trip through DefaultGame.ini. Marking
 * the inner members config as well does nothing, because a USTRUCT is not a config container.
 */
USTRUCT()
struct TRACE_API FTraceBotProfile
{
	GENERATED_BODY()

	// ----------------------------------------------------------------------------------------
	// Reaction
	//
	// This is the single most important dial for "do the bots feel fair". A bot that acquires and
	// fires in one frame is indistinguishable from an aimbot no matter how badly it aims.
	// ----------------------------------------------------------------------------------------

	/** Seconds a bot must hold ONE target continuously before it is allowed to pull the trigger. */
	UPROPERTY(EditAnywhere, Category = "Reaction")
	float ReactionTimeSeconds = 0.90f;

	/** +/- fraction of ReactionTimeSeconds rolled per acquisition, so five bots never fire as one. */
	UPROPERTY(EditAnywhere, Category = "Reaction")
	float ReactionJitterFraction = 0.40f;

	/**
	 * Seconds a bot is blind after its target dies or leaves its engagement envelope.
	 *
	 * Without this a bot kills you and is already on your teammate in the same frame, which is what
	 * turned the opening seconds of a match into a simultaneous ten-way wipe.
	 */
	UPROPERTY(EditAnywhere, Category = "Reaction")
	float ReacquireDelaySeconds = 1.20f;

	// ----------------------------------------------------------------------------------------
	// Aim
	//
	// Error is applied to the aim point the bot slews toward, and the trigger gate is measured
	// against THAT point rather than against the true one. So the error is a real miss, not merely
	// a delay before a perfect shot. (The old code gated on the true aim, which meant a bot only
	// ever fired once it was pointed at you: raising the error made bots quieter, never worse.)
	// ----------------------------------------------------------------------------------------

	/** Base half-width of the aim error cone, in degrees, at point-blank range against a still target. */
	UPROPERTY(EditAnywhere, Category = "Aim")
	float AimErrorDegrees = 5.0f;

	/** Extra degrees of error per 1000uu of range. This is the "bots are lasers across the map" fix. */
	UPROPERTY(EditAnywhere, Category = "Aim")
	float AimErrorPerThousandRange = 2.6f;

	/** Extra degrees of error per 1000uu/s of the target's speed across the bot's line of sight. */
	UPROPERTY(EditAnywhere, Category = "Aim")
	float AimErrorPerThousandCrossSpeed = 3.0f;

	/** Hard ceiling on total aim error, so a distant sprinter does not make a bot spin in circles. */
	UPROPERTY(EditAnywhere, Category = "Aim")
	float AimErrorMaxDegrees = 22.f;

	/** Degrees per second the bot's aim slews. A finite rate is what makes strafing counterplay. */
	UPROPERTY(EditAnywhere, Category = "Aim")
	float AimTurnRateDegrees = 190.f;

	/** How close to its OWN (error-offset) aim point a bot must be before it fires. */
	UPROPERTY(EditAnywhere, Category = "Aim")
	float FireConeDegrees = 3.5f;

	/**
	 * Probability that a bot aims at the HEAD rather than the body when it acquires a target.
	 *
	 * Damage became positional this pass: head 100, body 40, leg 25. A head hit is an instant kill
	 * from full health, so this single number is now the biggest lever on bot lethality — bigger
	 * than reaction time and bigger than the aim error cone, both of which only change how often a
	 * bot connects, not what a connection is worth.
	 *
	 * ZERO ON EASY, AND THAT IS THE POINT. The measured Easy baseline is ~0.72 human deaths per
	 * minute while engaged, and it was judged reasonable. Nine bots each with a one-shot kill
	 * available would not survive contact with that number. Easy is kept beatable by refusing the
	 * instant kill, not by making the bots miss more — a bot that visibly sprays is read as broken,
	 * whereas a bot that shoots you in the chest three times is read as fair.
	 *
	 * Rolled once per acquisition, not per shot: a bot commits to the zone for the engagement rather
	 * than flickering between head and chest and hitting the gap between them.
	 */
	UPROPERTY(EditAnywhere, Category = "Aim")
	float HeadshotAimFraction = 0.f;

	// ----------------------------------------------------------------------------------------
	// Engagement envelope
	// ----------------------------------------------------------------------------------------

	/** Bots are unaware of enemies beyond this, even with clear line of sight. */
	UPROPERTY(EditAnywhere, Category = "Engagement")
	float SightRange = 4500.f;

	/**
	 * Hard cap on the range a bot will SHOOT at. Deliberately far below SightRange: a bot that
	 * knows you exist but will not open up across the arena is what gives a player room to move.
	 */
	UPROPERTY(EditAnywhere, Category = "Engagement")
	float MaxEngagementRange = 2600.f;

	/** Range a bot tries to hold while duelling. Longer = its aim error costs it more. */
	UPROPERTY(EditAnywhere, Category = "Engagement")
	float PreferredCombatRange = 1900.f;

	// ----------------------------------------------------------------------------------------
	// Burst fire
	//
	// FireInterval is 0.12s and a body shot is 40 (spec §6), so a held trigger on target kills in
	// 0.24s — and a single head shot kills instantly.
	// Nothing about reaction time or aim error survives that. Bursting is the DPS dial, and it is
	// the one that makes a fight readable: you can hear the gap and move in it.
	// ----------------------------------------------------------------------------------------

	/** Seconds of continuous fire before the bot lets go of the trigger. */
	UPROPERTY(EditAnywhere, Category = "Burst")
	float BurstDurationMin = 0.20f;

	UPROPERTY(EditAnywhere, Category = "Burst")
	float BurstDurationMax = 0.38f;

	/** Seconds the trigger stays released after a burst, whatever the bot can see. */
	UPROPERTY(EditAnywhere, Category = "Burst")
	float BurstRestMin = 0.70f;

	UPROPERTY(EditAnywhere, Category = "Burst")
	float BurstRestMax = 1.30f;

	// ----------------------------------------------------------------------------------------
	// Tempo and objective play
	// ----------------------------------------------------------------------------------------

	/** Seconds between bot state re-evaluations. Steering and aim still run every frame. */
	UPROPERTY(EditAnywhere, Category = "Tempo")
	float DecisionInterval = 0.26f;

	/** 0..1. How eagerly a bot spends its dash — to escape while carrying, or to commit at a trail. */
	UPROPERTY(EditAnywhere, Category = "Tempo")
	float Aggression = 0.55f;

	/**
	 * How many of the carrier's opponents peel off to hunt the trail.
	 *
	 * NOT scaled down on Easy. The trail-dash kill is the identity of the game; if it only shows up
	 * on Hard then most players never see the mechanic the whole design is built around. Easy is
	 * made easy by the shooting model above, not by hiding the signature play.
	 */
	UPROPERTY(EditAnywhere, Category = "Tempo")
	int32 InterceptorCount = 3;

	/** Per-decision probability that an interceptor in range actually commits its dash at the trail. */
	UPROPERTY(EditAnywhere, Category = "Tempo")
	float TrailDashCommitChance = 0.92f;

	/** Probability a carrying bot passes on an evaluation tick that found a legal receiver. */
	UPROPERTY(EditAnywhere, Category = "Tempo")
	float PassChance = 0.55f;

	/**
	 * How many of the carrier's opponents hold a bead on them instead of hunting their trace.
	 *
	 * These are the bots that punish a pass. Bullets do nothing to a shielded carrier, so a punisher
	 * spends most of its time apparently wasting shots — but the pass drops the shield for half a
	 * second, and a gun already pointed at the carrier converts that half second into a kill and a
	 * turnover. Without anyone in this role, passing is free and the entire risk half of the spec's
	 * central risk/reward loop never happens.
	 *
	 * Separate from InterceptorCount because the two jobs want different bots: an interceptor has to
	 * get physically across the trace, a punisher only has to keep line of sight.
	 */
	UPROPERTY(EditAnywhere, Category = "Tempo")
	int32 PunisherCount = 1;

	/**
	 * 0..1. How careful a carrying bot is about WHEN it starts a pass.
	 *
	 * Scales UTraceSettings::BotPassSafeRadius and the number of covering enemies tolerated. At 0 a
	 * bot passes whenever it has a receiver, drops its shield in front of a firing line and dies for
	 * it; at 1 it waits for a genuinely clean window.
	 *
	 * Low on Easy on purpose. A reckless pass is a turnover and a free kill for the player, which is
	 * one of the few ways to make Easy easier that does not involve making the bots look stupid.
	 */
	UPROPERTY(EditAnywhere, Category = "Tempo")
	float PassCaution = 0.30f;

	/**
	 * 0..1. How much of the movement kit (slide, boost, crouch fast-fall) this bot uses.
	 *
	 * Not a skill dial so much as a legibility one: nine bots all sliding at once is noise, and the
	 * point of the kit is that it reads as intent. Kept well above zero even on Easy, because a
	 * mechanic no bot performs is a mechanic that has never been tested.
	 */
	UPROPERTY(EditAnywhere, Category = "Tempo")
	float MovementTechChance = 0.55f;
};

/**
 * Gameplay tuning for Trace.
 *
 * Every member is a `config` property: values ship in DefaultGame.ini under
 * [/Script/Trace.TraceSettings] and can be overridden per-platform or per-user without a rebuild.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Trace Gameplay"))
class TRACE_API UTraceSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UTraceSettings(const FObjectInitializer& ObjectInitializer);

	/**
	 * The one accessor. Returns the class default object, which the config system has already
	 * populated — cheap enough to call per-frame, but do not hold the reference across a
	 * hot reload.
	 */
	static const UTraceSettings& Get();

	/** Groups the page under "Game" in Project Settings rather than the default bucket. */
	virtual FName GetCategoryName() const override;

	// ------------------------------------------------------------------------------------------
	// Bot difficulty resolution
	//
	// Three ways in, in priority order:
	//   1. SetBotDifficulty(), which the arena's first bot calls after reading the travel URL.
	//   2. "-difficulty=easy|normal|hard" on the command line, for headless measurement runs.
	//   3. The BotDifficulty config property below, which defaults to Easy.
	//
	// Resolution is latched for the duration of a match, so a mid-match config reload cannot change
	// the bots out from under a player. The latch is deliberately NOT process-lifetime: the title
	// screen can be returned to and a different difficulty chosen, so ATraceGameMode::InitGame
	// forces a fresh resolution on every map load. Without that force, the second match of a session
	// silently kept the first match's bots.
	// ------------------------------------------------------------------------------------------

	/** The difficulty currently in force. Never returns anything but a valid enumerator. */
	static EBotDifficulty GetBotDifficulty();

	/** Latches the session difficulty. Safe to call repeatedly; the last call wins. */
	static void SetBotDifficulty(EBotDifficulty InDifficulty);

	/**
	 * Parses a game mode OptionsString for "?difficulty=..." and latches the result.
	 *
	 * Falls back to "-difficulty=" on the command line, and then to the BotDifficulty config
	 * property.
	 *
	 * Idempotent by default: once resolved, later callers are no-ops, so a bot respawning ten
	 * minutes in cannot re-read a stale URL. Pass bForceReresolve when a NEW match is starting and
	 * the URL is therefore authoritative again — ATraceGameMode::InitGame is the one caller that
	 * does, because it runs exactly once per map load and before any bot exists.
	 */
	static void ResolveBotDifficultyFromOptions(const FString& Options, bool bForceReresolve = false);

	/** "EASY" / "NORMAL" / "HARD". Logs and HUD only. */
	static const TCHAR* BotDifficultyToString(EBotDifficulty InDifficulty);

	/** The full knob set for the difficulty currently in force. */
	static const FTraceBotProfile& GetBotProfile();

	// ------------------------------------------------------------------------------------------
	// Match
	// ------------------------------------------------------------------------------------------

	/** Captures needed to win outright. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	int32 ScoreToWin = 5;

	/**
	 * LEGACY — read by nothing in the rules.
	 *
	 * Spec §1 replaced the single timed match with TWO HALVES. The enforced length of each half is
	 * ATraceGameMode::HalfDuration (config=Game on the game mode), and the game mode also owns
	 * whether the score cap ends the match early (bEndMatchAtScoreToWin, off by default — "first to
	 * 5" would cut the second half, and the side switch with it, out of most matches).
	 *
	 * Kept so existing configs still load without warnings. Do not tune it and expect an effect.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	float MatchDuration = 600.f;

	/** Target roster size per team; used to balance teams on login. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	int32 PlayersPerTeam = 5;

	/** Connected players required before the match leaves WaitingForPlayers. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	int32 MinPlayersToStart = 2;

	/**
	 * Seconds between death and respawn. Spec §1 sets this to 3.
	 *
	 * NOT AUTHORITATIVE. ATraceGameMode::RespawnDelay is what actually schedules the respawn; this
	 * is the client-side fallback the death panel counts down from during the frame or two before
	 * ATracePlayerState::RespawnEndServerTime (the real, replicated deadline) has arrived. Keep the
	 * two equal or the panel briefly disagrees with the game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	float RespawnDelay = 3.f;

	/** Countdown after MinPlayersToStart is met, before the match goes InProgress. */
	UPROPERTY(config, EditAnywhere, Category = "Match")
	float WarmupDuration = 5.f;

	// ------------------------------------------------------------------------------------------
	// Combat
	// ------------------------------------------------------------------------------------------

	/** Starting and maximum health. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float MaxHealth = 100.f;

	// DEAD PROPERTIES REMOVED: HitscanDamage and HeadshotMultiplier.
	//
	// Spec §6 replaced flat damage + a headshot multiplier with three positional zone values (head
	// 100 / body 40 / legs 25). They live in UTraceDamageSettings (Gameplay/TraceHitZones.h), which
	// is deliberately the ONE definition shared by the client's predicted trace and the server's
	// rewound one — see that header. Nothing reads a base damage or a multiplier any more, and
	// leaving them here would invite somebody to retune a number the game ignores.

	/**
	 * Maximum hitscan distance in unreal units.
	 *
	 * Must span the arena diagonal or shots silently die in mid-air short of a visible target. The
	 * field is 24000 x 12000, so the diagonal is ~26833; 28000 covers it with margin. This was 15000
	 * — correct for the old 8000 x 4000 arena and barely half the field once it was scaled up.
	 *
	 * Raising it does NOT make the bots deadlier: they are limited by FTraceBotProfile::
	 * MaxEngagementRange (3000 Easy / 3400 Normal / 5000 Hard), far below either value. This only
	 * restores the human's ability to shoot what they can see.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float HitscanRange = 28000.f;

	/** Seconds between shots. The server validates fire rate against this with a tolerance. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float FireInterval = 0.12f;

	/** Off by design: teammates never damage each other. Flip only for tuning experiments. */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	bool bFriendlyFire = false;

	/**
	 * LEGACY — read by nothing.
	 *
	 * Spec §6: "There is no movement inaccuracy. Set spread to 0." UTraceWeaponComponent::FireOnce
	 * no longer rolls a cone at all; the shot IS the aim ray. The roll was removed rather than
	 * configured to zero so that a stale .ini cannot quietly reintroduce inaccuracy the design has
	 * deleted — and so a modified client cannot roll itself a zero nobody else gets.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat")
	float SpreadDegrees = 0.f;

	// ------------------------------------------------------------------------------------------
	// Bots (singleplayer)
	//
	// Everything here is read live by ATraceGameMode and ATraceBotController, so the whole bot
	// difficulty curve is tunable from Config/DefaultGame.ini without a rebuild. The URL option
	// "?bots=0" overrides bFillTeamsWithBots for one session (see ATraceGameMode::AreBotsEnabled).
	// ------------------------------------------------------------------------------------------

	/** Master switch: top both teams up to PlayersPerTeam with AI so one human gets a full 5v5. */
	UPROPERTY(config, EditAnywhere, Category = "Bots")
	bool bFillTeamsWithBots = true;

	/**
	 * Bots to add per team, or -1 for "however many it takes to reach PlayersPerTeam".
	 * -1 is what makes the fill self-correcting when a second human joins a listen server.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots")
	int32 BotsPerTeam = -1;

	/** Hard safety cap on total bots, so a silly PlayersPerTeam cannot spawn a hundred pawns. */
	UPROPERTY(config, EditAnywhere, Category = "Bots")
	int32 MaxBots = 16;

	/**
	 * Difficulty used when nothing else resolved one — i.e. a direct launch straight into the arena
	 * with no travel URL. EASY on purpose: this is what an unattended run, an automated test and a
	 * first-time player all get.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots")
	EBotDifficulty BotDifficulty = EBotDifficulty::Easy;

	/**
	 * The knob sets, one per difficulty. Values are assigned by name in the UTraceSettings
	 * constructor rather than braced here: twenty-one positional floats in a row is a data-entry
	 * accident waiting to happen, and the constructor lets each number sit next to its reason.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Difficulty")
	FTraceBotProfile BotEasy;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Difficulty")
	FTraceBotProfile BotNormal;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Difficulty")
	FTraceBotProfile BotHard;

	// ------------------------------------------------------------------------------------------
	// Bots — difficulty-independent tuning
	//
	// Everything below describes the ARENA or the RULES rather than the opponent's skill, so it is
	// shared by all three profiles.
	//
	// ARENA SCALING, AND WHY SO MANY OF THESE ARE FRACTIONS
	// The field is not a fixed size. Distances that describe *positioning on the pitch* (how far
	// ahead of the carrier a screen sits, how deep a receiver runs, how long a pass may be, how far
	// a defender will travel to reach a trail) were originally constants tuned against an
	// 8000 x 4000 field; on a 24000 x 12000 field the same numbers put every escort on top of the
	// carrier and made every pass illegal. Those are expressed as a fraction of the field half-
	// length or half-width, read from ATraceArenaBuilder::GetFieldBounds() at runtime.
	//
	// Distances that describe the CHARACTER rather than the pitch — dash reach, the trail dash
	// commit band, wall clearance — stay absolute, because a dash is 468uu no matter how big the
	// arena is.
	// ------------------------------------------------------------------------------------------

	/**
	 * Perpendicular distance to the trail line inside which a hunting bot commits its dash.
	 *
	 * Must stay comfortably under the dash's own reach (DashSpeed * DashDuration, ~468uu by
	 * default) or the dash stops short of the trail and the signature kill never lands. ABSOLUTE,
	 * not field-relative: it is a property of the dash.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept")
	float BotTrailDashRange = 380.f;

	/** How far past the trail an interceptor aims, so the dash sweeps THROUGH it and not up to it. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept")
	float BotTrailCrossOvershoot = 320.f;

	/**
	 * Trail points with less than this many seconds of life left are not worth running at.
	 *
	 * A bot that commits to a point which expires before it arrives is a bot that spends the whole
	 * carry running at ghosts — one of the two reasons trail kills were 1.3% of deaths.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept")
	// 1.2 -> 0.8: spec §3 cut TrailLifetime from 6 s to 4 s, and this filter is a FRACTION of that
	// window even though it is written as an absolute. At 1.2 s it discarded the oldest 30% of a 4 s
	// trace instead of the 20% it was tuned to discard, and the trail-kill share fell from a measured
	// 37.5% of kills to 25.9% in the first run under the new rules. Interceptors were not worse at
	// crossing; there was simply less legal trace for them to aim at.
	float BotTrailMinPointLifeRemaining = 0.8f;

	/**
	 * Fraction of the field HALF-LENGTH inside which a bot will accept the interceptor role.
	 *
	 * Capped by the difficulty profile's SightRange, so an interceptor has to be able to SEE the
	 * carrier to peel off after them. Without the cap the role radius was pure geometry and a
	 * defender on the far side of a 24000uu arena would set off after a carrier it had no way of
	 * knowing about — measurably fatal: the carrier died 1-3 seconds after every pickup and the
	 * match ran a full minute without a single capture.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept")
	float BotInterceptRadiusFieldFraction = 0.70f;

	/**
	 * Trail points closer than this to the carrier are not intercept targets.
	 *
	 * The server exempts the newest TrailHeadGracePoints from the trip test, but at the default
	 * spacing that is only ~180uu — close enough that a defender standing where the carrier just
	 * was could stab the trail at their heels the instant they picked up. That is not the play the
	 * mechanic is for. Requiring established trail turns the intercept back into a chase and gives
	 * the carrier the separation a run needs to be worth attempting.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept")
	float BotTrailMinDistanceFromCarrier = 900.f;

	/** Fraction of the field HALF-LENGTH a screening escort holds ahead of the carrier. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning")
	float BotEscortLeadFieldFraction = 0.18f;

	/** Fraction of the field HALF-LENGTH short of the goal a deep receiver parks at. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning")
	float BotDeepRunnerStandoffFieldFraction = 0.22f;

	/** Fraction of the field HALF-WIDTH used to spread bots sideways so they do not stack up. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning")
	float BotFormationSpreadFieldFraction = 0.35f;

	/**
	 * Fraction of the field HALF-WIDTH a bot offsets the point it attacks in the enemy endzone.
	 *
	 * An endzone spans the whole width of the field, so which part of the line a carrier crosses is
	 * free choice — and aiming every bot at its exact centre threw that away, funnelling every carry
	 * and every escort station down one corridor on a 12000uu-wide pitch. This gives each bot its own
	 * crossing point for the life of its pawn (FormationBias is uniform in [-1, 1] and fixed per
	 * life), which spreads carry routes, the trails they lay, and the escort screens that hang off
	 * them across the field instead of stacking them.
	 *
	 * DELIBERATELY MODEST, AND DELIBERATELY NOT APPLIED TO THE CHASE. The first attempt at this was
	 * 0.55 and also routed BehaviourChaseCore down per-bot lanes, on the theory that a spread-out
	 * team would find a player standing out on a wing. Measured over 200-second matches it made the
	 * whole game emptier rather than the wings fuller: total deaths fell 62 -> 33, damage to the human
	 * fell 150/min -> 11/min, and the mean player-to-nearest-bot distance ROSE from ~5000uu to
	 * ~12000uu. Contesting the Core together is what generates fights; only the endzone approach has
	 * width to give away.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning")
	float BotAttackLaneFieldFraction = 0.30f;


	/** How far inside the endzone the goal point sits. Must stay under the endzone depth (900uu). */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning")
	float BotGoalInsetFromWall = 300.f;

	/** How close to a wall the steering repulsion field starts pushing back. Absolute: dash reach. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning")
	float BotWallAvoidMargin = 500.f;

	/** An enemy inside this radius makes a carrying bot spend its dash to break away. Absolute. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning")
	float BotCarrierPanicRadius = 900.f;

	// ------------------------------------------------------------------------------------------
	// Bots — target selection
	//
	// Both of these are difficulty-independent on purpose. They do not describe how WELL a bot
	// shoots — that is the profile above — they describe WHO it shoots at, and getting that wrong
	// made every difficulty read as "the bots ignore the player" no matter how the skill knobs were
	// set. See ATraceBotController::FindBestShootTarget.
	// ------------------------------------------------------------------------------------------

	/**
	 * Weight applied to a HUMAN target's squared distance when a bot picks who to shoot. Below 1
	 * means preferred; 1.0 restores pure nearest-first.
	 *
	 * Nine of the ten players in a solo match are bots and they cluster on the objective, so
	 * nearest-first made the human the least-shot-at actor on the field. Measured on the shipped
	 * Easy profile: bots had the human inside engagement range with clear line of sight for 50.8% of
	 * bot-ticks in a contested window, but chose them as a target for only 19.3%.
	 *
	 * 0.25 in squared distance is 0.5 in linear distance: the human wins unless another enemy is at
	 * least twice as close. 0.45 (~0.67 linear) was tried first and measured too weak to matter —
	 * bots still spent 0% of a window targeting a player they had in range with clear line of sight
	 * 12% of the time, because nine bots packed around the Core are always the closer option.
	 * Deliberately not lower still: bots that visibly refuse to fight each other read as scripted.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Targeting")
	float BotHumanTargetBias = 0.25f;

	/**
	 * How much better a new target must score before a bot abandons the one it is already tracking,
	 * as a fraction of the current target's score. 1.0 disables the hysteresis.
	 *
	 * Switching target restarts the reaction clock. In a crowd the nearest enemy changes every
	 * second or two, which on Easy (a reaction delay of up to ~1.9s) meant the clock was re-rolled
	 * more often than it could ever elapse: 19.3% of bot-ticks aiming produced 1.9% firing, against
	 * the ~17% duty cycle the burst profile alone implies. Sticking to a target is what converts
	 * aim time into shots.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Targeting")
	float BotTargetSwitchFraction = 0.45f;

	/** Seconds between pass evaluations by a carrying bot. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing")
	float BotPassEvalInterval = 0.35f;

	/**
	 * Shortest throw a bot will bother with.
	 *
	 * Below this the "pass" is a handoff to somebody already inside PickupRadius — the Core is
	 * caught in the same frame it is thrown, which gains nothing and reads in the log as a bug.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing")
	float BotPassMinDistance = 700.f;

	/** Floor on pass range, in uu. The effective range is the larger of this and the fraction below. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing")
	float BotPassMinRange = 3200.f;

	/** Fraction of the field HALF-LENGTH a bot will throw the Core across. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing")
	float BotPassRangeFieldFraction = 0.55f;

	/**
	 * How much closer to the goal a receiver must be, as a fraction of the field HALF-LENGTH.
	 *
	 * Pairs with BotEscortLeadFieldFraction: if a screening escort's own station is not further
	 * forward than this threshold, then by construction no escort is ever a legal receiver and
	 * passing silently never happens. That is exactly what was measured — zero passes in fifty
	 * runs. Keep BotEscortLeadFieldFraction comfortably above this value.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing")
	float BotPassMinGoalAdvantageFieldFraction = 0.10f;

	/**
	 * An enemy this close to a carrying bot turns passing into a reflex rather than a plan: the
	 * advantage requirement drops to zero and any open teammate becomes a legal outlet.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing")
	float BotPassPressureRadius = 1400.f;

	// ------------------------------------------------------------------------------------------
	// Bots — hover passing  (mechanics spec §4)
	//
	// The pass stopped being a throw this pass. It is now a HELD input with a 0.5 s dwell on a
	// teammate, and the instant it starts the carrier's shield drops. So the bot side of it is a
	// commitment with an abort path, and these are the numbers that describe the commitment.
	//
	// The three durations below deliberately MIRROR the rule values rather than reading them: the
	// rule lives on the pawn's own settings, which are owned elsewhere, and a bot that guessed at
	// them would fail silently rather than loudly. If the rule moves, these move with it, and
	// [BotPass] telemetry (attempts vs completions) is what says whether they still agree.
	// ------------------------------------------------------------------------------------------

	/**
	 * Extra seconds a bot holds the pass input beyond the rule's dwell, as insurance against the
	 * hold and the dwell being clocked on slightly different frames. Cheap: the surplus is spent
	 * shielded-down, but only for a frame or two.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass")
	float BotPassHoldMargin = 0.20f;

	/** Bot-side mirror of the rule's pass dwell. Keep equal to the pawn's hold requirement. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass")
	float BotPassHoldSeconds = 0.50f;

	/** Bot-side mirror of the rule's 2 s post-pass cooldown, plus a little so bots never spam it. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass")
	float BotPassCooldownSeconds = 2.30f;

	/**
	 * How close the aim must be to the receiver before the bot commits the input, in degrees.
	 *
	 * The bot lines up with the shield still UP (ETraceBotPassPhase::Lining), which costs nothing,
	 * and only presses once it is inside this cone. Pressing early would burn the vulnerable window
	 * on slewing rather than on the dwell.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass")
	float BotPassAimToleranceDegrees = 4.f;

	/**
	 * Seconds a bot will spend lining up before giving up on this receiver.
	 *
	 * A carrier that spends four seconds slewing at a teammate who keeps moving behind cover is a
	 * carrier not running at the goal.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass")
	float BotPassMaxLineUpSeconds = 1.10f;

	/**
	 * Radius inside which an enemy with line of sight counts as "covering" the carrier, and so as a
	 * reason not to start a pass. Scaled by FTraceBotProfile::PassCaution.
	 *
	 * This is the risk half of the spec's central loop expressed as a number: it is the distance at
	 * which a gun is close enough to convert half a second of dropped shield into a kill.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass")
	float BotPassSafeRadius = 2200.f;

	// ------------------------------------------------------------------------------------------
	// Bots — punishing the passer
	// ------------------------------------------------------------------------------------------

	/**
	 * Range at which a punisher will hold a bead on the enemy carrier.
	 *
	 * Also the radius used by CountEnemiesCoveringMe(), so the carrier's idea of "someone can shoot
	 * me" and the defender's idea of "I can shoot the carrier" are the same number by construction.
	 * If those two ever disagree, one side of the pass loop is playing a different game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Punish")
	float BotPunishRange = 2600.f;

	/**
	 * Standoff a punisher tries to hold from the carrier, as a fraction of BotPunishRange.
	 *
	 * Deliberately not zero: a punisher that walks onto the carrier is inside its own trace-hunting
	 * teammates' crossing lanes, and is also the first thing a boosting carrier escapes past.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Punish")
	float BotPunishStandoffFraction = 0.55f;

	// ------------------------------------------------------------------------------------------
	// Bots — positional aim  (mechanics spec §6: head 100 / body 40 / leg 25)
	//
	// Offsets from the character's actor origin (capsule centre), in uu. The capsule is 88 uu half
	// height, so the head sits near the top of it and the legs near the bottom. These are the points
	// a bot's aim converges on; FTraceBotProfile::HeadshotAimFraction chooses between them.
	// ------------------------------------------------------------------------------------------

	UPROPERTY(config, EditAnywhere, Category = "Bots|Aim")
	float BotAimHeadOffsetZ = 62.f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Aim")
	float BotAimBodyOffsetZ = 20.f;

	/**
	 * Where a bot aims when it is deliberately going low.
	 *
	 * Not currently chosen on purpose by any difficulty — it exists because vertical aim error
	 * around the body point already produces leg hits, and naming the zone makes that visible in the
	 * damage numbers instead of looking like a miss that happened to land.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Aim")
	float BotAimLegOffsetZ = -50.f;

	// ------------------------------------------------------------------------------------------
	// Bots — movement kit  (mechanics spec §5)
	//
	// Dash, slide, crouch fast-fall and boost. Each has ONE job below; none of them fire randomly.
	// The cooldowns here are bot-side mirrors of the rule values for the same reason the pass
	// durations are (see above) — the authoritative cooldown lives on the movement component.
	// ------------------------------------------------------------------------------------------

	/** Bot-side mirror of the dash cooldown used by the shadow charge model. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement")
	float BotDashCooldownSeconds = 4.f;

	/** Bot-side mirror of the boost cooldown. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement")
	float BotBoostCooldownSeconds = 12.f;

	/**
	 * Seconds of being unable to make progress before a bot tries to BOOST over whatever is in the
	 * way, rather than only strafing around it.
	 *
	 * This is the single most useful job the boost has for an AI with no navmesh: the arena is full
	 * of waist-high neon furniture, and a bot wedged against a block used to sidestep along it for
	 * seconds at a time. Slightly longer than the strafe-evade trigger so the cheap fix is always
	 * tried first.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement")
	float BotBoostStuckSeconds = 1.30f;

	/** Minimum planar speed before a slide is worth starting. Sliding from a standstill is a crouch. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement")
	float BotSlideMinSpeed = 480.f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement")
	float BotSlideHoldSeconds = 0.70f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement")
	float BotSlideCooldownSeconds = 3.20f;

	/**
	 * Minimum height above the floor at which a bot will crouch to kill its upward momentum.
	 *
	 * The fast-fall exists to get back to the ground quickly — a bot in the air cannot dash usefully
	 * and cannot change direction, which is a bad place to be while being shot at. Below this height
	 * the fall is over before the input would matter.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement")
	float BotFastFallMinHeight = 140.f;

	// ------------------------------------------------------------------------------------------
	// Bots — endzone resolution  (mechanics spec §1: sides switch at half time)
	// ------------------------------------------------------------------------------------------

	/**
	 * Seconds between re-asking the ATraceEndzone actors which end this team attacks.
	 *
	 * Bots MUST NOT cache "Blue attacks +X". Teams switch sides at half time, and a bot still
	 * running at the first-half endzone in the second half is both the most likely bug in this pass
	 * and one that looks exactly like bad pathing. Polling is a two-actor scan, so it is cheap
	 * enough to simply keep doing forever rather than trying to detect the switch.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning")
	float BotEndzoneResolveInterval = 1.f;

	// ------------------------------------------------------------------------------------------
	// Bots — legacy scalars
	//
	// Superseded by the FTraceBotProfile above, which is what ATraceBotController actually reads.
	// They survive because UI/TraceMatchOptions.cpp writes to them to express its own difficulty
	// curve, and deleting them would not compile. Treat them as read-only history: changing them
	// no longer changes how a bot plays.
	// ------------------------------------------------------------------------------------------

	UPROPERTY(config, EditAnywhere, Category = "Bots|Legacy")
	float BotReactionTime = 0.32f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Legacy")
	float BotAimErrorDegrees = 4.5f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Legacy")
	float BotAggression = 0.75f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Legacy")
	float BotDecisionInterval = 0.2f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Legacy")
	float BotSightRange = 6000.f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Legacy")
	float BotPreferredCombatRange = 1600.f;

	UPROPERTY(config, EditAnywhere, Category = "Bots|Legacy")
	float BotPassChance = 0.35f;

	// ------------------------------------------------------------------------------------------
	// Controls
	// ------------------------------------------------------------------------------------------

	/**
	 * NO LONGER THE PLAYER'S SENSITIVITY. The human look scale is now a persisted per-player
	 * setting: UTraceUserSettings::LookSensitivity (default 1.50), plus a separate vertical
	 * multiplier and an invert-Y toggle, all rebuilt into the Look mapping's modifier scalars by
	 * ATracePlayerController::ApplyControlSettings(). Editing this value does not move a human's
	 * crosshair.
	 *
	 * DEAD. Nothing reads it any more. Its last caller was the -TraceWalkHuman debug harness in
	 * AI/TraceBotController.cpp, which divided by it to convert a desired yaw into synthetic mouse
	 * delta; that was fixed at integration to read UTraceUserSettings::GetLookScaleX(), i.e. the
	 * scalar actually installed in the Look mapping. Kept only so an existing DefaultGame.ini that
	 * still carries the key does not warn. Do not wire anything new to it.
	 *
	 * The old warning about bEnableLegacyInputScales is obsolete and was wrong in the opposite
	 * direction — see the header of Settings/TraceUserSettings.h for the corrected sign analysis
	 * (the Negate modifier on MouseY was the bug, and it is gone).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controls")
	float LookSensitivity = 2.5f;

	// ------------------------------------------------------------------------------------------
	// Movement
	// ------------------------------------------------------------------------------------------

	/** Base ground speed. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float WalkSpeed = 720.f;

	/** WalkSpeed multiplier while carrying the Core — the carrier is slightly faster. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float CarrierSpeedMultiplier = 1.08f;

	/** Speed clamp while dashing. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float DashSpeed = 2600.f;

	/** How long a dash lasts. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float DashDuration = 0.18f;

	/**
	 * Cooldown measured from dash start, per charge. Contract §5: four seconds.
	 *
	 * Was 3. Raising it is a real nerf to the only counterplay against a carrier, which is exactly
	 * why the carrier is simultaneously given a second charge — the pressure moves from "the
	 * defender can dash often" to "the carrier can dash twice".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float DashCooldown = 4.f;

	/**
	 * Dash charges everybody has. The pool refills one charge at a time, each on DashCooldown.
	 *
	 * A pool rather than a timer because the carrier bonus below has to be able to appear and
	 * disappear mid-cooldown without the answer being arbitrary — see the movement component header.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	int32 BaseDashCharges = 1;

	/** Extra dash charges granted for as long as the pawn is carrying the Core (contract §5). */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	int32 CarrierExtraDashCharges = 1;

	// --- Slide (crouch on the ground) -------------------------------------------------------
	//
	// A slide spends momentum you already have: it starts from your current speed, boosts it once
	// and then bleeds it off. It never resizes the capsule — the capsule is the single source of
	// truth for hit resolution, lag compensation and the trail trip test.

	/**
	 * Fraction of WalkSpeed you must already be moving at before crouch will start a slide.
	 * Stops "tap crouch from a standstill" being free speed.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideEntrySpeedFraction = 0.55f;

	/** Entry speed multiplier applied to max(current planar speed, WalkSpeed). */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideSpeedMultiplier = 1.45f;

	/** Hard ceiling on slide entry speed, so a slide out of a dash cannot compound. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideMaxSpeed = 1500.f;

	/** Longest a slide may last, even if it has not decayed to the exit speed. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideDuration = 1.f;

	/** uu/s^2 bled off the slide every second. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideDeceleration = 750.f;

	/** Fraction of WalkSpeed at which a decaying slide gives up and hands the player back. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideExitSpeedFraction = 0.6f;

	/** Degrees per second a slide may be steered. 0 = a slide is a rail. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideTurnRateDegrees = 130.f;

	/** Cooldown measured from slide start, so chained slides cannot sustain above walking speed. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideCooldown = 0.6f;

	/**
	 * How long a crouch press that could not slide yet (mid-dash, or airborne) stays queued.
	 *
	 * This is what makes "dash, then slide out of it" and "press crouch just before you land" work.
	 * Only a fresh PRESS charges it, so holding the key can never chain slides.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float SlideInputBufferSeconds = 0.25f;

	// --- Boost (ground-only vertical launch, a separate bind from jump) ----------------------

	/**
	 * +Z velocity granted by a boost. Apex is v^2/2g: at 1150 against the default 980 gravity that
	 * is ~675uu, against ~209uu for the 640 jump.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float BoostZVelocity = 1150.f;

	/** Contract §5: twelve seconds. Long on purpose — boost is a commitment, not mobility. */
	UPROPERTY(config, EditAnywhere, Category = "Movement")
	float BoostCooldown = 12.f;

	// ------------------------------------------------------------------------------------------
	// Core
	// ------------------------------------------------------------------------------------------

	/** Launch speed of a thrown/passed Core. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float PassSpeed = 2400.f;

	/** Fraction of the throw direction added as +Z, so passes arc instead of skimming the floor. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float PassUpwardBias = 0.14f;

	/** Radius within which a character may pick the Core up. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float PickupRadius = 110.f;

	/** How long the thrower is blocked from re-catching their own pass. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float PickupLockoutAfterThrow = 0.35f;

	/** A loose, untouched Core returns to centre after this many seconds. */
	UPROPERTY(config, EditAnywhere, Category = "Core")
	float CoreResetTime = 15.f;

	// ------------------------------------------------------------------------------------------
	// Trail
	// ------------------------------------------------------------------------------------------

	/** Who dies when the trail is tripped. Default rule: the carrier does. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	ETrailLethality TrailLethality = ETrailLethality::KillsCarrier;

	/** True: teammates of the carrier pass through the trail harmlessly. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	bool bOnlyEnemiesTripTrail = true;

	/** True: only a dashing player trips the trail. This is the core counterplay rule. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	bool bRequireDashToTripTrail = true;

	/** Seconds a trail point survives after being laid. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	float TrailLifetime = 4.f;

	/** Distance the carrier must travel before a new point is appended. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	float TrailPointSpacing = 60.f;

	/** Collision/visual radius of a trail segment. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	float TrailRadius = 45.f;

	/** Collision/visual height of a trail segment. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	float TrailHeight = 190.f;

	/** Hard cap on replicated trail points; oldest are dropped first. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	int32 MaxTrailPoints = 256;

	/** Newest N points are exempt from the trip test so the carrier never kills themselves. */
	UPROPERTY(config, EditAnywhere, Category = "Trail")
	int32 TrailHeadGracePoints = 3;

	// ------------------------------------------------------------------------------------------
	// Net
	// ------------------------------------------------------------------------------------------

	/** Master switch for server-side rewind on hitscan. Off = resolve against present-day poses. */
	UPROPERTY(config, EditAnywhere, Category = "Net")
	bool bEnableLagCompensation = true;

	/** Upper bound on how far back the server will rewind, in seconds. */
	UPROPERTY(config, EditAnywhere, Category = "Net")
	float MaxRewindTime = 0.25f;

	/** How much pose history each character keeps, in seconds. Must exceed MaxRewindTime. */
	UPROPERTY(config, EditAnywhere, Category = "Net")
	float LagCompHistoryDuration = 1.f;

	/** Draws the rewound capsules the server actually tested against. Noisy; dev only. */
	UPROPERTY(config, EditAnywhere, Category = "Net")
	bool bDrawServerRewindDebug = false;
};
