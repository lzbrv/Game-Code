// Trace — every runtime-tunable number in one place.
//
// UTraceSettings is a UDeveloperSettings, so it shows up under Project Settings > Game >
// "Trace Gameplay" and persists to Config/DefaultGame.ini (defaultconfig). Read it from anywhere
// with UTraceSettings::Get().
//
// Rule for the rest of the codebase: never hardcode a gameplay constant that lives in this
// table. Read it through Get() at the point of use — designers change these live, and the CDO
// is refreshed by the config system, so caching a copy in a constructor will go stale.
//
// =================================================================================================
// LIVE EDITING DURING PIE — HOW IT WORKS, AND WHAT YOU MUST NOT DO
// =================================================================================================
//
// The Project Settings details panel edits the CLASS DEFAULT OBJECT of this class in place. Get()
// returns exactly that CDO. So any system that calls Get() at the point of use picks a changed
// value up on the very next frame, with PIE still running and no restart. That is not an accident
// of the implementation, it is the whole reason Get() returns a reference to the CDO instead of a
// cached snapshot, and it is why the "read at point of use" rule above is a rule and not a style
// preference.
//
// A value only fails to update live when somebody has COPIED it out of here into a member — at
// BeginPlay, in a constructor, or into an engine field like UCharacterMovementComponent::
// MaxWalkSpeed, which the movement system reads directly and which no amount of re-reading Get()
// can refresh on its own.
//
// For those cases there are two hooks, both below:
//
//   1. UTraceSettings::OnSettingsChanged() — a multicast delegate broadcast from
//      PostEditChangeProperty. Systems that genuinely must cache (because the value feeds an engine
//      field, or because the read is on a hot path) subscribe once and re-copy on the broadcast.
//
//   2. UTraceSettings::ApplyLiveMovementTuning() — called by the same PostEditChangeProperty, and
//      also safe to call by hand. It walks every live character movement component in every game
//      world and re-asserts the engine-owned speed fields from this table. This exists so that
//      WalkSpeed responds to a slider drag TODAY, without the movement component having to
//      subscribe to anything. If the movement component later grows its own subscription, this
//      stays correct: it writes the same values BeginPlay writes, so the two agree by construction.
//
// Both are editor-only (WITH_EDITOR). In a cooked build nothing edits the CDO, so there is nothing
// to react to.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Engine/DeveloperSettings.h"   // module: DeveloperSettings
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"     // GetDefault<>

#include "TraceTypes.h"                 // ETrailLethality

#include "TraceSettings.generated.h"

/**
 * Broadcast after any UTraceSettings property is edited in the Project Settings panel.
 *
 * Declared at file scope rather than inside the UCLASS so UnrealHeaderTool never has to parse a
 * delegate macro inside a generated-body class. The parameter is the edited property's name, or
 * NAME_None when the panel could not name one (a struct-wide paste, an "import defaults").
 * Listeners that only care about a couple of values should treat NAME_None as "re-read everything".
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FTraceSettingsChanged, FName /*ChangedPropertyName*/);

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
 *
 * LIVE: ATraceBotController calls UTraceSettings::GetBotProfile() at the point of use on every
 * decision tick, so every number in here retunes with PIE running. The only thing latched for the
 * duration of a match is WHICH of the three profiles is selected (see the difficulty latch).
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

	/**
	 * Seconds a bot must hold ONE target continuously before it is allowed to pull the trigger.
	 *
	 * Sane range 0.2 (Hard, near-instant) to 1.2 (Easy, visibly slow). Above ~1.5 most engagements
	 * end before the bot is ever allowed to fire and the bots read as broken rather than as easy.
	 */
	UPROPERTY(EditAnywhere, Category = "Reaction", meta = (DisplayName = "Reaction Time (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.1", UIMax = "1.5"))
	float ReactionTimeSeconds = 0.90f;

	/**
	 * +/- fraction of ReactionTimeSeconds rolled per acquisition, so five bots never fire as one.
	 *
	 * Sane range 0.2 to 0.5. Zero makes a squad fire in perfect unison, which reads as scripted.
	 */
	UPROPERTY(EditAnywhere, Category = "Reaction", meta = (DisplayName = "Reaction Jitter (fraction)", ClampMin = "0.0", ClampMax = "0.95", UIMin = "0.0", UIMax = "0.6"))
	float ReactionJitterFraction = 0.40f;

	/**
	 * Seconds a bot is blind after its target dies or leaves its engagement envelope.
	 *
	 * Without this a bot kills you and is already on your teammate in the same frame, which is what
	 * turned the opening seconds of a match into a simultaneous ten-way wipe. Sane range 0.3 to 1.4.
	 */
	UPROPERTY(EditAnywhere, Category = "Reaction", meta = (DisplayName = "Reacquire Delay (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.0", UIMax = "2.0"))
	float ReacquireDelaySeconds = 1.20f;

	// ----------------------------------------------------------------------------------------
	// Aim
	//
	// Error is applied to the aim point the bot slews toward, and the trigger gate is measured
	// against THAT point rather than against the true one. So the error is a real miss, not merely
	// a delay before a perfect shot. (The old code gated on the true aim, which meant a bot only
	// ever fired once it was pointed at you: raising the error made bots quieter, never worse.)
	// ----------------------------------------------------------------------------------------

	/**
	 * Base half-width of the aim error cone, in degrees, at point-blank range against a still target.
	 *
	 * Sane range 1.5 (Hard) to 6 (Easy). This is the SMALLEST the error ever gets; the range and
	 * cross-speed terms below are added on top and dominate at arena distances.
	 */
	UPROPERTY(EditAnywhere, Category = "Aim", meta = (DisplayName = "Aim Error Base (deg)", ClampMin = "0.0", ClampMax = "45.0", UIMin = "0.0", UIMax = "12.0"))
	float AimErrorDegrees = 5.0f;

	/**
	 * Extra degrees of error per 1000uu of range. This is the "bots are lasers across the map" fix.
	 *
	 * Sane range 1.0 (Hard) to 2.5 (Easy). The dominant term at arena distances: at 4000uu it is
	 * worth four times the base error.
	 */
	UPROPERTY(EditAnywhere, Category = "Aim", meta = (DisplayName = "Aim Error per 1000uu Range (deg)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "5.0"))
	float AimErrorPerThousandRange = 2.6f;

	/**
	 * Extra degrees of error per 1000uu/s of the target's speed across the bot's line of sight.
	 *
	 * This is what pays a player for strafing. Sane range 1.0 (Hard) to 4.0 (Easy).
	 */
	UPROPERTY(EditAnywhere, Category = "Aim", meta = (DisplayName = "Aim Error per 1000uu/s Cross Speed (deg)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "6.0"))
	float AimErrorPerThousandCrossSpeed = 3.0f;

	/**
	 * Hard ceiling on total aim error, so a distant sprinter does not make a bot spin in circles.
	 *
	 * Sane range 12 (Hard) to 22 (Easy). Must stay above AimErrorDegrees or the base is unreachable.
	 */
	UPROPERTY(EditAnywhere, Category = "Aim", meta = (DisplayName = "Aim Error Ceiling (deg)", ClampMin = "0.5", ClampMax = "90.0", UIMin = "5.0", UIMax = "40.0"))
	float AimErrorMaxDegrees = 22.f;

	/**
	 * Degrees per second the bot's aim slews. A finite rate is what makes strafing counterplay.
	 *
	 * Sane range 150 (Easy) to 450 (Hard). Anything above ~700 is instant snap at arena distances.
	 */
	UPROPERTY(EditAnywhere, Category = "Aim", meta = (DisplayName = "Aim Turn Rate (deg/s)", ClampMin = "10.0", ClampMax = "2000.0", UIMin = "80.0", UIMax = "600.0"))
	float AimTurnRateDegrees = 190.f;

	/**
	 * How close to its OWN (error-offset) aim point a bot must be before it fires.
	 *
	 * Sane range 2.5 to 4.5. This is a trigger gate, not an accuracy dial — widening it makes bots
	 * fire sooner, not more accurately.
	 */
	UPROPERTY(EditAnywhere, Category = "Aim", meta = (DisplayName = "Fire Cone (deg)", ClampMin = "0.1", ClampMax = "45.0", UIMin = "1.0", UIMax = "10.0"))
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
	 *
	 * Sane range: 0 on Easy, ~0.3 on Normal, ~0.6 on Hard. 1.0 is an execution machine.
	 */
	UPROPERTY(EditAnywhere, Category = "Aim", meta = (DisplayName = "Headshot Aim Chance (0-1)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float HeadshotAimFraction = 0.f;

	// ----------------------------------------------------------------------------------------
	// Engagement envelope
	// ----------------------------------------------------------------------------------------

	/**
	 * Bots are unaware of enemies beyond this, even with clear line of sight.
	 *
	 * Sane range 5000 to 8000 on a 24000 x 12000 field. Must stay above MaxEngagementRange or a bot
	 * can never legally shoot at anything.
	 */
	UPROPERTY(EditAnywhere, Category = "Engagement", meta = (DisplayName = "Sight Range (uu)", ClampMin = "100.0", ClampMax = "40000.0", UIMin = "2000.0", UIMax = "12000.0"))
	float SightRange = 4500.f;

	/**
	 * Hard cap on the range a bot will SHOOT at. Deliberately far below SightRange: a bot that
	 * knows you exist but will not open up across the arena is what gives a player room to move.
	 *
	 * Sane range 4000 (Easy) to 6000 (Hard). Below ~3000 on this field size the bots read as
	 * ignoring the player entirely — that was measured.
	 */
	UPROPERTY(EditAnywhere, Category = "Engagement", meta = (DisplayName = "Max Engagement Range (uu)", ClampMin = "100.0", ClampMax = "40000.0", UIMin = "1000.0", UIMax = "10000.0"))
	float MaxEngagementRange = 2600.f;

	/**
	 * Range a bot tries to hold while duelling. Longer = its aim error costs it more.
	 *
	 * Sane range 1400 to 2000. Should sit well inside MaxEngagementRange.
	 */
	UPROPERTY(EditAnywhere, Category = "Engagement", meta = (DisplayName = "Preferred Combat Range (uu)", ClampMin = "100.0", ClampMax = "20000.0", UIMin = "600.0", UIMax = "5000.0"))
	float PreferredCombatRange = 1900.f;

	// ----------------------------------------------------------------------------------------
	// Burst fire
	//
	// FireInterval is 0.16s and a body shot is 40 (spec §6), so a held trigger on target kills in
	// 0.32s — and a single head shot kills instantly.
	// Nothing about reaction time or aim error survives that. Bursting is the DPS dial, and it is
	// the one that makes a fight readable: you can hear the gap and move in it.
	// ----------------------------------------------------------------------------------------

	/** Seconds of continuous fire before the bot lets go of the trigger. Sane range 0.2 to 0.45. */
	UPROPERTY(EditAnywhere, Category = "Burst", meta = (DisplayName = "Burst Duration Min (s)", ClampMin = "0.02", ClampMax = "5.0", UIMin = "0.1", UIMax = "1.0"))
	float BurstDurationMin = 0.20f;

	/** Upper end of the rolled burst length. Keep at or above BurstDurationMin. */
	UPROPERTY(EditAnywhere, Category = "Burst", meta = (DisplayName = "Burst Duration Max (s)", ClampMin = "0.02", ClampMax = "5.0", UIMin = "0.1", UIMax = "1.0"))
	float BurstDurationMax = 0.38f;

	/**
	 * Seconds the trigger stays released after a burst, whatever the bot can see.
	 *
	 * Burst / (burst + rest) is the duty cycle, and the duty cycle is the real DPS dial: ~28% on
	 * Easy, ~38% on Normal, ~58% on Hard. Sane range 0.3 to 1.0.
	 */
	UPROPERTY(EditAnywhere, Category = "Burst", meta = (DisplayName = "Burst Rest Min (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.1", UIMax = "2.0"))
	float BurstRestMin = 0.70f;

	/** Upper end of the rolled rest. Keep at or above BurstRestMin. */
	UPROPERTY(EditAnywhere, Category = "Burst", meta = (DisplayName = "Burst Rest Max (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.1", UIMax = "2.0"))
	float BurstRestMax = 1.30f;

	// ----------------------------------------------------------------------------------------
	// Tempo and objective play
	// ----------------------------------------------------------------------------------------

	/**
	 * Seconds between bot state re-evaluations. Steering and aim still run every frame.
	 *
	 * Sane range 0.15 (Hard) to 0.32 (Easy). This is a cost dial as much as a skill one — ten bots
	 * at 0.05 is a measurable frame cost for no visible gain.
	 */
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Decision Interval (s)", ClampMin = "0.02", ClampMax = "2.0", UIMin = "0.1", UIMax = "0.6"))
	float DecisionInterval = 0.26f;

	/** 0..1. How eagerly a bot spends its dash — to escape while carrying, or to commit at a trail. */
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Aggression (0-1)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float Aggression = 0.55f;

	/**
	 * How many of the carrier's opponents peel off to hunt the trail.
	 *
	 * NOT scaled down on Easy. The trail-dash kill is the identity of the game; if it only shows up
	 * on Hard then most players never see the mechanic the whole design is built around. Easy is
	 * made easy by the shooting model above, not by hiding the signature play.
	 *
	 * InterceptorCount + PunisherCount must fit inside a five-man side, or the punisher slots never
	 * get filled — that was measured on Hard at 4 + 2, which logged zero punisher ticks in a 260s
	 * match.
	 */
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Interceptors (trace hunters)", ClampMin = "0", ClampMax = "5", UIMin = "0", UIMax = "5"))
	int32 InterceptorCount = 3;

	/** Per-decision probability that an interceptor in range actually commits its dash at the trail. */
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Trail Dash Commit Chance (0-1)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float TrailDashCommitChance = 0.92f;

	/** Probability a carrying bot passes on an evaluation tick that found a legal receiver. */
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Pass Chance (0-1)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
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
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Punishers (pass hunters)", ClampMin = "0", ClampMax = "5", UIMin = "0", UIMax = "5"))
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
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Pass Caution (0-1)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float PassCaution = 0.30f;

	/**
	 * 0..1. How much of the movement kit (slide, boost, crouch fast-fall) this bot uses.
	 *
	 * Not a skill dial so much as a legibility one: nine bots all sliding at once is noise, and the
	 * point of the kit is that it reads as intent. Kept well above zero even on Easy, because a
	 * mechanic no bot performs is a mechanic that has never been tested.
	 */
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Movement Tech Chance (0-1)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float MovementTechChance = 0.55f;
};

/**
 * Gameplay tuning for Trace.  (Project Settings > Game > Trace Gameplay)
 *
 * Every member is a `config` property: values ship in DefaultGame.ini under
 * [/Script/Trace.TraceSettings] and can be overridden per-platform or per-user without a rebuild.
 *
 * Every member is also EditAnywhere with clamps and a tooltip, and — because Get() reads the CDO
 * the details panel writes to — every member that is read at its point of use retunes with PIE
 * running. See the LIVE EDITING block at the top of this file for the handful of values that need
 * the PostEditChangeProperty hook instead, and why.
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
	 *
	 * This is also what makes live editing work: the Project Settings panel edits this exact
	 * object, so a value read through here is never more than a frame behind the slider.
	 */
	static const UTraceSettings& Get();

	/** Groups the page under "Game" in Project Settings rather than the default bucket. */
	virtual FName GetCategoryName() const override;

#if WITH_EDITOR
	/**
	 * Fires after any property on this page is edited.
	 *
	 * Subscribe from any system that CANNOT read Get() at its point of use — typically because the
	 * value has to be copied into an engine-owned field. Do not subscribe merely to cache: reading
	 * Get() is a pointer dereference and is cheaper than the bookkeeping.
	 *
	 * Editor-only. Nothing edits the CDO in a cooked build, so a shipped listener would be dead
	 * code that still cost a delegate slot.
	 */
	static FTraceSettingsChanged& OnSettingsChanged();

	/**
	 * Re-asserts the engine-owned movement fields (UCharacterMovementComponent::MaxWalkSpeed and
	 * MaxWalkSpeedCrouched) from this table, on every live pawn in every game world.
	 *
	 * WHY THIS EXISTS. The movement component copies WalkSpeed into MaxWalkSpeed once, in BeginPlay,
	 * because MaxWalkSpeed is what CalcVelocity clamps against — it is read by engine code that has
	 * never heard of UTraceSettings, so "read at point of use" is not available for this one value.
	 * Without this function, WalkSpeed is the single most feel-critical number on the page and also
	 * the only one that needed a PIE restart.
	 *
	 * Idempotent, and deliberately writes exactly what BeginPlay writes, so it cannot disagree with
	 * the movement component's own initialisation. Safe to call at any time; a no-op outside PIE.
	 */
	static void ApplyLiveMovementTuning();

	/** Broadcasts OnSettingsChanged and pushes the engine-owned copies. See the file header. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

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
	//
	// NOTE FOR LIVE TUNING: the latch covers WHICH PROFILE is in force, not the numbers inside it.
	// Editing BotEasy while an Easy match is running retunes the bots immediately; editing
	// BotDifficulty mid-match does nothing until the next map load, by design.
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

	// ==========================================================================================
	// MATCH
	// ==========================================================================================

	/**
	 * Captures needed to win outright.
	 *
	 * NOT usually match-ending: ATraceGameMode::bEndMatchAtScoreToWin is off by default, because
	 * "first to 5" would cut the second half — and the side switch that justifies it — out of most
	 * matches. Sane range 3 to 10.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Score To Win", ClampMin = "1", ClampMax = "50", UIMin = "1", UIMax = "15"))
	int32 ScoreToWin = 5;

	/** Target roster size per team; used to balance teams on login. 5 is the designed game. */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Players Per Team", ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "8"))
	int32 PlayersPerTeam = 5;

	/** Connected players required before the match leaves WaitingForPlayers. */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Min Players To Start", ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "10"))
	int32 MinPlayersToStart = 2;

	/**
	 * Seconds between death and respawn. Spec §1 sets this to 3.
	 *
	 * NOT AUTHORITATIVE. ATraceGameMode::RespawnDelay is what actually schedules the respawn; this
	 * is the client-side fallback the death panel counts down from during the frame or two before
	 * ATracePlayerState::RespawnEndServerTime (the real, replicated deadline) has arrived. Keep the
	 * two equal or the panel briefly disagrees with the game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Respawn Delay (s, HUD fallback)", ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.0", UIMax = "10.0"))
	float RespawnDelay = 3.f;

	/** Countdown after MinPlayersToStart is met, before the match goes InProgress. */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Warmup Duration (s)", ClampMin = "0.0", ClampMax = "120.0", UIMin = "0.0", UIMax = "30.0"))
	float WarmupDuration = 5.f;

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
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Match", meta = (DisplayName = "Match Duration (s) [DEAD]", ClampMin = "0.0", ClampMax = "7200.0"))
	float MatchDuration = 600.f;

	// ==========================================================================================
	// COMBAT
	// ==========================================================================================

	/** Starting and maximum health. Three body shots (40 each) is the designed time-to-kill. */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Max Health", ClampMin = "1.0", ClampMax = "1000.0", UIMin = "25.0", UIMax = "250.0"))
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
	 * MaxEngagementRange (4200 Easy / 4800 Normal / 6000 Hard), far below either value. This only
	 * restores the human's ability to shoot what they can see.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Hitscan Range (uu)", ClampMin = "100.0", ClampMax = "200000.0", UIMin = "5000.0", UIMax = "50000.0"))
	float HitscanRange = 28000.f;

	/**
	 * SECONDS BETWEEN SHOTS — this is the inverse of the fire RATE, so a BIGGER number is a SLOWER
	 * gun. The server validates a client's claimed fire rate against this with a tolerance.
	 *
	 * 0.16 is 6.25 shots/second: three body shots (40 damage each) kills a full-health target in
	 * 0.32s of sustained fire. Sane range 0.08 (twice as fast, very lethal) to 0.30 (a marksman
	 * rifle). Below ~0.05 the server's rate validation starts rejecting legitimate client shots.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Fire Interval (s between shots)", ClampMin = "0.02", ClampMax = "2.0", UIMin = "0.05", UIMax = "0.5"))
	float FireInterval = 0.16f;

	/** Off by design: teammates never damage each other. Flip only for tuning experiments. */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Friendly Fire"))
	bool bFriendlyFire = false;

	/**
	 * LEGACY — read by nothing.
	 *
	 * Spec §6: "There is no movement inaccuracy. Set spread to 0." UTraceWeaponComponent::FireOnce
	 * no longer rolls a cone at all; the shot IS the aim ray. The roll was removed rather than
	 * configured to zero so that a stale .ini cannot quietly reintroduce inaccuracy the design has
	 * deleted — and so a modified client cannot roll itself a zero nobody else gets.
	 */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Combat", meta = (DisplayName = "Spread (deg) [DEAD]", ClampMin = "0.0", ClampMax = "0.0"))
	float SpreadDegrees = 0.f;

	// ==========================================================================================
	// MOVEMENT
	//
	// The most feel-critical block on the page, and the one designers drag sliders on while PIE is
	// running. Everything here IS live: the movement component reads DashSpeed / DashDuration /
	// DashCooldown / the whole slide block / BoostZVelocity / BoostCooldown at the point of use, and
	// WalkSpeed — the one value the engine copies into its own field — is pushed by
	// ApplyLiveMovementTuning() from PostEditChangeProperty. See the file header.
	// ==========================================================================================

	/**
	 * Base ground speed, in uu/s.
	 *
	 * Copied into UCharacterMovementComponent::MaxWalkSpeed, which is what the physics step actually
	 * clamps against; ApplyLiveMovementTuning() re-pushes it on every edit so the slider is felt
	 * immediately. Sane range 600 (heavy) to 1000 (frantic). The whole slide and dash block is
	 * expressed as multiples of this, so moving it moves the kit with it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Walk", meta = (DisplayName = "Walk Speed (uu/s)", ClampMin = "50.0", ClampMax = "5000.0", UIMin = "300.0", UIMax = "1500.0"))
	float WalkSpeed = 820.f;

	/**
	 * WalkSpeed multiplier while carrying the Core — the carrier is slightly faster.
	 *
	 * The carrier cannot shoot and is hunted by five people, so the speed edge is what makes
	 * carrying playable. Sane range 1.0 to 1.2; above ~1.3 nobody can catch a carrier at all.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Walk", meta = (DisplayName = "Carrier Speed Multiplier", ClampMin = "0.5", ClampMax = "2.0", UIMin = "1.0", UIMax = "1.4"))
	float CarrierSpeedMultiplier = 1.08f;

	// --- Dash --------------------------------------------------------------------------------

	/**
	 * Speed clamp while dashing, in uu/s.
	 *
	 * DASH REACH = DashSpeed * DashDuration. At 3000 x 0.18 that is 540uu. Bots' BotTrailDashRange
	 * must stay comfortably under that number or the signature trail-crossing kill stops landing —
	 * if you change this, check that one. Sane range 2200 to 3600.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "Dash Speed (uu/s)", ClampMin = "100.0", ClampMax = "10000.0", UIMin = "1000.0", UIMax = "5000.0"))
	float DashSpeed = 3000.f;

	/**
	 * How long a dash lasts, in seconds. Multiplied by DashSpeed this is the dash's reach.
	 *
	 * Sane range 0.12 to 0.28. Below ~0.08 the dash becomes a one-or-two-frame teleport that the
	 * trail trip test can step straight over, which silently breaks the game's core counterplay.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "Dash Duration (s)", ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.08", UIMax = "0.4"))
	float DashDuration = 0.18f;

	/**
	 * Cooldown measured from dash start, per charge, in seconds.
	 *
	 * This is the only counterplay against a carrier, so it is a strong dial in both directions:
	 * shorter means the defence can commit more often, longer means the carrier's run is safer.
	 * Sane range 2.5 to 5. Keep UTraceSettings::BotDashCooldownSeconds equal to it, or the bots'
	 * shadow charge model will think they have a dash they do not.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "Dash Cooldown (s)", ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.5", UIMax = "10.0"))
	float DashCooldown = 3.5f;

	/**
	 * Dash charges everybody has. The pool refills one charge at a time, each on DashCooldown.
	 *
	 * A pool rather than a timer because the carrier bonus below has to be able to appear and
	 * disappear mid-cooldown without the answer being arbitrary — see the movement component header.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "Base Dash Charges", ClampMin = "1", ClampMax = "5", UIMin = "1", UIMax = "3"))
	int32 BaseDashCharges = 1;

	/** Extra dash charges granted for as long as the pawn is carrying the Core (contract §5). */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "Carrier Extra Dash Charges", ClampMin = "0", ClampMax = "5", UIMin = "0", UIMax = "3"))
	int32 CarrierExtraDashCharges = 1;

	// --- Slide (crouch on the ground) -------------------------------------------------------
	//
	// A slide SPENDS MOMENTUM YOU ALREADY HAVE: it starts from your current speed, boosts it once
	// and then bleeds it off slowly. It never resizes the capsule — the capsule is the single source
	// of truth for hit resolution, lag compensation and the trail trip test.
	//
	// The numbers below were retuned this pass for "longer, and it must preserve the player's
	// momentum": the decay is now gentle enough that a slide carries most of its entry speed all the
	// way to its natural end, and the duration is long enough for that to be a traversal tool rather
	// than a flourish. SlideCooldown moved with SlideDuration on purpose — the cooldown is measured
	// from slide START, so a cooldown shorter than the duration would let a player chain slides
	// end-to-end and never walk again.

	/**
	 * Fraction of WalkSpeed you must already be moving at before crouch will start a slide.
	 * Stops "tap crouch from a standstill" being free speed. Sane range 0.4 to 0.7.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Entry Speed (fraction of Walk Speed)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.2", UIMax = "1.0"))
	float SlideEntrySpeedFraction = 0.55f;

	/**
	 * Entry speed multiplier applied to max(current planar speed, WalkSpeed).
	 *
	 * Because it multiplies your CURRENT speed, a slide out of a fast approach is faster than a
	 * slide out of a walk — that is the momentum preservation. Sane range 1.1 to 1.5; 1.0 is a pure
	 * "keep exactly what I had" slide.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Entry Speed Multiplier", ClampMin = "0.5", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float SlideSpeedMultiplier = 1.35f;

	/**
	 * Hard ceiling on slide entry speed, in uu/s.
	 *
	 * This is the one knob that can DESTROY momentum rather than preserve it: a slide entered out of
	 * a dash is clamped to this, so setting it near WalkSpeed means a fast player is slowed by
	 * sliding. Kept well above walking pace for that reason. Sane range 1.5x to 2.5x WalkSpeed.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Max Entry Speed (uu/s)", ClampMin = "100.0", ClampMax = "6000.0", UIMin = "800.0", UIMax = "3000.0"))
	float SlideMaxSpeed = 1900.f;

	/**
	 * Longest a slide may last, in seconds, even if it has not decayed to the exit speed.
	 *
	 * With the gentle deceleration below, this is what actually ends most slides — so this is the
	 * "make the slide longer" dial. Sane range 0.8 to 2.5. Keep SlideCooldown at or above it, or
	 * slides chain end-to-end.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Duration (s)", ClampMin = "0.1", ClampMax = "6.0", UIMin = "0.3", UIMax = "3.0"))
	float SlideDuration = 1.8f;

	/**
	 * uu/s bled off the slide every second. THIS IS THE MOMENTUM DIAL — lower preserves more.
	 *
	 * At 260 a 1.8s slide sheds ~470uu/s in total, so a slide entered at ~1100 still exits above
	 * walking pace. The old 750 stripped 750uu/s and ended most slides early on the exit-speed
	 * check, which is what made the slide feel like a brake. Sane range 150 to 500; 0 is a
	 * frictionless rail.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Deceleration (uu/s per s)", ClampMin = "0.0", ClampMax = "4000.0", UIMin = "0.0", UIMax = "1500.0"))
	float SlideDeceleration = 260.f;

	/**
	 * Fraction of WalkSpeed at which a decaying slide gives up and hands the player back.
	 *
	 * Raise it and slides end early; lower it and a slide runs until SlideDuration expires. Sane
	 * range 0.4 to 0.8.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Exit Speed (fraction of Walk Speed)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.2", UIMax = "1.0"))
	float SlideExitSpeedFraction = 0.5f;

	/** Degrees per second a slide may be steered. 0 = a slide is a rail. Sane range 90 to 180. */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Turn Rate (deg/s)", ClampMin = "0.0", ClampMax = "720.0", UIMin = "0.0", UIMax = "360.0"))
	float SlideTurnRateDegrees = 130.f;

	/**
	 * Cooldown measured from slide START, in seconds, so chained slides cannot sustain above walking
	 * speed.
	 *
	 * MEASURED FROM START, NOT FROM END — so a value below SlideDuration means no cooldown at all in
	 * practice. Keep it at or above SlideDuration plus the recovery you want between slides.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Cooldown (s, from slide start)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "6.0"))
	float SlideCooldown = 2.4f;

	/**
	 * How long a crouch press that could not slide yet (mid-dash, or airborne) stays queued.
	 *
	 * This is what makes "dash, then slide out of it" and "press crouch just before you land" work.
	 * Only a fresh PRESS charges it, so holding the key can never chain slides. Sane range 0.15 to
	 * 0.35; much above that and the slide fires long after the player stopped asking for it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Input Buffer (s)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5"))
	float SlideInputBufferSeconds = 0.25f;

	/**
	 * Seconds at the start of a slide during which releasing crouch will NOT cancel it.
	 *
	 * Without this a slide is only as long as the player holds the key, so lengthening SlideDuration
	 * changes nothing for anyone who taps crouch. A dash still overrides the commit window. Sane
	 * range 0.3 to 0.8; at or above SlideDuration the slide becomes entirely uncancellable.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Min Commit (s, uncancellable)", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.5"))
	float SlideMinCommitSeconds = 0.55f;

	/**
	 * Fraction of the slide's LIVE speed carried into normal movement on exit. 1 = all of it.
	 *
	 * This is the "preserve momentum" contract: the old exit only ever clamped DOWN, so a slide
	 * handed the player back below walking pace and made them re-accelerate. Sane range 0.8 to 1.0.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Exit Speed Retention (fraction of slide speed)", ClampMin = "0.0", ClampMax = "1.5", UIMin = "0.5", UIMax = "1.0"))
	float SlideExitSpeedRetention = 1.0f;

	/**
	 * Floor on the exit speed as a fraction of WalkSpeed, so a slide can never end slower than a run.
	 *
	 * Below 1 a decayed slide still dumps the player out under walking pace. Sane range 0.9 to 1.0.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Exit Floor (fraction of Walk Speed)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.5", UIMax = "1.2"))
	float SlideExitMinSpeedFraction = 1.0f;

	/**
	 * Ceiling on the exit speed as a multiple of max speed. KEEP THIS AT 1.0 unless you want overspeed.
	 *
	 * Above 1, CalcVelocity's input branch clamps to the CURRENT speed once it exceeds max, so any
	 * overspeed handed back is kept for as long as a movement key is held — slide-cancel spam then
	 * becomes the fastest way to cross the arena. The knob exists to make that a deliberate choice.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Exit Ceiling (multiple of Max Speed)", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float SlideExitMaxSpeedMultiplier = 1.0f;

	// --- Boost (ground-only vertical launch, a separate bind from jump) ----------------------

	/**
	 * +Z velocity granted by a boost, in uu/s.
	 *
	 * APEX SCALES WITH THE SQUARE: height = v^2 / 2g. Against the default 980 gravity, 813 gives an
	 * apex of ~337uu, against ~209uu for the 640 jump. Halving this value would QUARTER the height,
	 * not halve it — to halve the height, scale the velocity by sqrt(0.5) (which is exactly how 1150
	 * became 813). Sane range 700 to 1400.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Boost", meta = (DisplayName = "Boost Z Velocity (uu/s)", ClampMin = "100.0", ClampMax = "4000.0", UIMin = "400.0", UIMax = "2000.0"))
	float BoostZVelocity = 813.f;

	/**
	 * Boost cooldown in seconds. Contract §5: twelve seconds.
	 *
	 * Long on purpose — boost is a commitment, not mobility. Keep BotBoostCooldownSeconds equal.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Boost", meta = (DisplayName = "Boost Cooldown (s)", ClampMin = "0.0", ClampMax = "60.0", UIMin = "1.0", UIMax = "20.0"))
	float BoostCooldown = 12.f;

	// ==========================================================================================
	// CORE
	// ==========================================================================================

	/** Launch speed of a thrown/passed Core, in uu/s. Sane range 1800 to 3200. */
	UPROPERTY(config, EditAnywhere, Category = "Core", meta = (DisplayName = "Pass Speed (uu/s)", ClampMin = "100.0", ClampMax = "10000.0", UIMin = "800.0", UIMax = "5000.0"))
	float PassSpeed = 2400.f;

	/** Fraction of the throw direction added as +Z, so passes arc instead of skimming the floor. */
	UPROPERTY(config, EditAnywhere, Category = "Core", meta = (DisplayName = "Pass Upward Bias (fraction)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5"))
	float PassUpwardBias = 0.14f;

	/**
	 * Radius within which a character may pick the Core up, in uu.
	 *
	 * Roughly three capsule radii. Much larger and passes catch themselves off a near miss; much
	 * smaller and a moving receiver has to be threaded through.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core", meta = (DisplayName = "Pickup Radius (uu)", ClampMin = "10.0", ClampMax = "1000.0", UIMin = "50.0", UIMax = "400.0"))
	float PickupRadius = 110.f;

	/** How long the thrower is blocked from re-catching their own pass, in seconds. */
	UPROPERTY(config, EditAnywhere, Category = "Core", meta = (DisplayName = "Self-Catch Lockout (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.0", UIMax = "1.5"))
	float PickupLockoutAfterThrow = 0.35f;

	/** A loose, untouched Core returns to centre after this many seconds. Sane range 8 to 25. */
	UPROPERTY(config, EditAnywhere, Category = "Core", meta = (DisplayName = "Loose Core Reset Time (s)", ClampMin = "1.0", ClampMax = "120.0", UIMin = "5.0", UIMax = "40.0"))
	float CoreResetTime = 15.f;

	// ==========================================================================================
	// TRAIL  (the "trace")
	// ==========================================================================================

	/** Who dies when the trail is tripped. Default rule: the carrier does. */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Trail Lethality"))
	ETrailLethality TrailLethality = ETrailLethality::KillsCarrier;

	/** True: teammates of the carrier pass through the trail harmlessly. Flipping this changes the game. */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Only Enemies Trip Trail"))
	bool bOnlyEnemiesTripTrail = true;

	/** True: only a dashing player trips the trail. This is the core counterplay rule. */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Require Dash To Trip Trail"))
	bool bRequireDashToTripTrail = true;

	/**
	 * Seconds a trail point survives after being laid — i.e. how much lethal trace is on the field
	 * behind a carrier at any moment.
	 *
	 * CEILING OF 4 SECONDS, ENFORCED IN CODE. UTraceTrailComponent::GetTraceLifetimeSeconds() takes
	 * min(this, 4.0), so a larger value here is silently ignored; the clamp below makes that visible
	 * in the panel instead of hiding it. Sane range 2 to 4.
	 *
	 * Shortening it is a real nerf to the trail-hunting bots, whose BotTrailMinPointLifeRemaining
	 * filter is a fraction of this window written as an absolute — move the two together.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Trail Lifetime (s)", ClampMin = "0.2", ClampMax = "4.0", UIMin = "0.5", UIMax = "4.0"))
	float TrailLifetime = 2.8f;

	/**
	 * Distance the carrier must travel before a new point is appended, in uu.
	 *
	 * Smaller is a smoother, more expensive trace. Sane range 40 to 100; this multiplied by
	 * MaxTrailPoints is the longest trace that can exist.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Point Spacing (uu)", ClampMin = "10.0", ClampMax = "500.0", UIMin = "30.0", UIMax = "200.0"))
	float TrailPointSpacing = 60.f;

	/** Collision/visual radius of a trail segment, in uu. The visual is derived from the lethal volume. */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Segment Radius (uu)", ClampMin = "5.0", ClampMax = "500.0", UIMin = "20.0", UIMax = "150.0"))
	float TrailRadius = 45.f;

	/**
	 * Collision/visual height of a trail segment, in uu.
	 *
	 * Also drives the third-person camera pivot (ATraceCharacter::GetThirdPersonPivotZ reads this so
	 * the camera clears the wall), so raising it lifts the carry camera with it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Segment Height (uu)", ClampMin = "10.0", ClampMax = "1000.0", UIMin = "60.0", UIMax = "400.0"))
	float TrailHeight = 190.f;

	/** Hard cap on replicated trail points; oldest are dropped first. A bandwidth dial. */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Max Trail Points", ClampMin = "8", ClampMax = "1024", UIMin = "32", UIMax = "512"))
	int32 MaxTrailPoints = 256;

	/**
	 * Newest N points are exempt from the trip test so the carrier never kills themselves.
	 *
	 * At the default spacing this is only ~180uu of exemption. Sane range 2 to 6; zero lets a
	 * carrier die to the trace coming out of their own feet.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Head Grace Points", ClampMin = "0", ClampMax = "32", UIMin = "0", UIMax = "10"))
	int32 TrailHeadGracePoints = 3;

	// ==========================================================================================
	// BOTS
	//
	// Everything here is read live by ATraceGameMode and ATraceBotController, so the whole bot
	// difficulty curve is tunable with PIE running. The URL option "?bots=0" overrides
	// bFillTeamsWithBots for one session (see ATraceGameMode::AreBotsEnabled).
	// ==========================================================================================

	/** Master switch: top both teams up to PlayersPerTeam with AI so one human gets a full 5v5. */
	UPROPERTY(config, EditAnywhere, Category = "Bots", meta = (DisplayName = "Fill Teams With Bots"))
	bool bFillTeamsWithBots = true;

	/**
	 * Bots to add per team, or -1 for "however many it takes to reach PlayersPerTeam".
	 * -1 is what makes the fill self-correcting when a second human joins a listen server.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots", meta = (DisplayName = "Bots Per Team (-1 = auto fill)", ClampMin = "-1", ClampMax = "16", UIMin = "-1", UIMax = "8"))
	int32 BotsPerTeam = -1;

	/** Hard safety cap on total bots, so a silly PlayersPerTeam cannot spawn a hundred pawns. */
	UPROPERTY(config, EditAnywhere, Category = "Bots", meta = (DisplayName = "Max Bots (safety cap)", ClampMin = "0", ClampMax = "64", UIMin = "0", UIMax = "32"))
	int32 MaxBots = 16;

	/**
	 * Difficulty used when nothing else resolved one — i.e. a direct launch straight into the arena
	 * with no travel URL. EASY on purpose: this is what an unattended run, an automated test and a
	 * first-time player all get.
	 *
	 * LATCHED PER MATCH. Editing this mid-PIE does nothing until the next map load; edit the profile
	 * that is already in force instead, which retunes immediately.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots", meta = (DisplayName = "Default Bot Difficulty"))
	EBotDifficulty BotDifficulty = EBotDifficulty::Easy;

	/**
	 * The knob sets, one per difficulty. Values are assigned by name in the UTraceSettings
	 * constructor rather than braced here: twenty-one positional floats in a row is a data-entry
	 * accident waiting to happen, and the constructor lets each number sit next to its reason.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Difficulty", meta = (DisplayName = "Easy Profile"))
	FTraceBotProfile BotEasy;

	/** The Normal skill curve. Roughly a competent human. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Difficulty", meta = (DisplayName = "Normal Profile"))
	FTraceBotProfile BotNormal;

	/** The Hard skill curve. Punishing: you must use cover and you must keep moving. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Difficulty", meta = (DisplayName = "Hard Profile"))
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
	// commit band, wall clearance — stay absolute, because a dash is 540uu no matter how big the
	// arena is.
	// ------------------------------------------------------------------------------------------

	/**
	 * Perpendicular distance to the trail line inside which a hunting bot commits its dash, in uu.
	 *
	 * Must stay comfortably under the dash's own reach (DashSpeed * DashDuration, ~540uu by
	 * default) or the dash stops short of the trail and the signature kill never lands. ABSOLUTE,
	 * not field-relative: it is a property of the dash. Sane range 0.6 to 0.85 of dash reach.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Trail Dash Commit Range (uu)", ClampMin = "10.0", ClampMax = "3000.0", UIMin = "100.0", UIMax = "1000.0"))
	float BotTrailDashRange = 380.f;

	/** How far past the trail an interceptor aims, in uu, so the dash sweeps THROUGH it and not up to it. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Trail Cross Overshoot (uu)", ClampMin = "0.0", ClampMax = "3000.0", UIMin = "50.0", UIMax = "800.0"))
	float BotTrailCrossOvershoot = 320.f;

	/**
	 * Trail points with less than this many seconds of life left are not worth running at.
	 *
	 * A bot that commits to a point which expires before it arrives is a bot that spends the whole
	 * carry running at ghosts — one of the two reasons trail kills were 1.3% of deaths.
	 *
	 * A FRACTION OF TrailLifetime WRITTEN AS AN ABSOLUTE, so the two must move together. It is
	 * calibrated to discard the oldest ~20% of the trace: at TrailLifetime 4 that was 0.8, and at
	 * the current 2.8 it is 0.56. Leaving it at 0.8 against a 2.8s trace would discard the oldest
	 * 29% instead — the same mistake, in the same direction, that cut the trail-kill share from a
	 * measured 37.5% of kills to 25.9% when TrailLifetime went 6 -> 4. Interceptors were not worse
	 * at crossing; there was simply less legal trace for them to aim at.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Min Point Life Remaining (s)", ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0"))
	float BotTrailMinPointLifeRemaining = 0.56f;

	/**
	 * Fraction of the field HALF-LENGTH inside which a bot will accept the interceptor role.
	 *
	 * Capped by the difficulty profile's SightRange, so an interceptor has to be able to SEE the
	 * carrier to peel off after them. Without the cap the role radius was pure geometry and a
	 * defender on the far side of a 24000uu arena would set off after a carrier it had no way of
	 * knowing about — measurably fatal: the carrier died 1-3 seconds after every pickup and the
	 * match ran a full minute without a single capture.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Intercept Radius (fraction of field half-length)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.1", UIMax = "1.0"))
	float BotInterceptRadiusFieldFraction = 0.70f;

	/**
	 * Trail points closer than this to the carrier are not intercept targets, in uu.
	 *
	 * The server exempts the newest TrailHeadGracePoints from the trip test, but at the default
	 * spacing that is only ~180uu — close enough that a defender standing where the carrier just
	 * was could stab the trail at their heels the instant they picked up. That is not the play the
	 * mechanic is for. Requiring established trail turns the intercept back into a chase and gives
	 * the carrier the separation a run needs to be worth attempting.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Min Trail Distance From Carrier (uu)", ClampMin = "0.0", ClampMax = "8000.0", UIMin = "0.0", UIMax = "3000.0"))
	float BotTrailMinDistanceFromCarrier = 900.f;

	/**
	 * Fraction of the field HALF-LENGTH a screening escort holds ahead of the carrier.
	 *
	 * Must stay comfortably ABOVE BotPassMinGoalAdvantageFieldFraction, or no escort is ever a legal
	 * receiver and bots silently never pass. That was measured: zero passes in fifty runs.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning", meta = (DisplayName = "Escort Lead (fraction of field half-length)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5"))
	float BotEscortLeadFieldFraction = 0.18f;

	/** Fraction of the field HALF-LENGTH short of the goal a deep receiver parks at. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning", meta = (DisplayName = "Deep Runner Standoff (fraction of field half-length)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.6"))
	float BotDeepRunnerStandoffFieldFraction = 0.22f;

	/** Fraction of the field HALF-WIDTH used to spread bots sideways so they do not stack up. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning", meta = (DisplayName = "Formation Spread (fraction of field half-width)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
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
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning", meta = (DisplayName = "Attack Lane Spread (fraction of field half-width)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float BotAttackLaneFieldFraction = 0.30f;

	/** How far inside the endzone the goal point sits, in uu. Must stay under the endzone depth (900uu). */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning", meta = (DisplayName = "Goal Inset From Wall (uu)", ClampMin = "0.0", ClampMax = "900.0", UIMin = "0.0", UIMax = "900.0"))
	float BotGoalInsetFromWall = 300.f;

	/** How close to a wall the steering repulsion field starts pushing back, in uu. Absolute: dash reach. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning", meta = (DisplayName = "Wall Avoid Margin (uu)", ClampMin = "1.0", ClampMax = "4000.0", UIMin = "50.0", UIMax = "1500.0"))
	float BotWallAvoidMargin = 500.f;

	/** An enemy inside this radius (uu) makes a carrying bot spend its dash to break away. Absolute. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning", meta = (DisplayName = "Carrier Panic Radius (uu)", ClampMin = "0.0", ClampMax = "8000.0", UIMin = "0.0", UIMax = "3000.0"))
	float BotCarrierPanicRadius = 900.f;

	/**
	 * Seconds between re-asking the ATraceEndzone actors which end this team attacks.
	 *
	 * Bots MUST NOT cache "Blue attacks +X". Teams switch sides at half time, and a bot still
	 * running at the first-half endzone in the second half is both the most likely bug in this pass
	 * and one that looks exactly like bad pathing. Polling is a two-actor scan, so it is cheap
	 * enough to simply keep doing forever rather than trying to detect the switch.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Positioning", meta = (DisplayName = "Endzone Re-resolve Interval (s)", ClampMin = "0.05", ClampMax = "30.0", UIMin = "0.1", UIMax = "5.0"))
	float BotEndzoneResolveInterval = 1.f;

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
	UPROPERTY(config, EditAnywhere, Category = "Bots|Targeting", meta = (DisplayName = "Human Target Bias (squared-distance weight)", ClampMin = "0.01", ClampMax = "1.0", UIMin = "0.05", UIMax = "1.0"))
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
	UPROPERTY(config, EditAnywhere, Category = "Bots|Targeting", meta = (DisplayName = "Target Switch Threshold (fraction)", ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.1", UIMax = "1.0"))
	float BotTargetSwitchFraction = 0.45f;

	// ------------------------------------------------------------------------------------------
	// Bots — passing
	// ------------------------------------------------------------------------------------------

	/** Seconds between pass evaluations by a carrying bot. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing", meta = (DisplayName = "Pass Eval Interval (s)", ClampMin = "0.05", ClampMax = "5.0", UIMin = "0.1", UIMax = "1.5"))
	float BotPassEvalInterval = 0.35f;

	/**
	 * Shortest throw a bot will bother with, in uu.
	 *
	 * Below this the "pass" is a handoff to somebody already inside PickupRadius — the Core is
	 * caught in the same frame it is thrown, which gains nothing and reads in the log as a bug.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing", meta = (DisplayName = "Min Pass Distance (uu)", ClampMin = "0.0", ClampMax = "10000.0", UIMin = "100.0", UIMax = "3000.0"))
	float BotPassMinDistance = 700.f;

	/** Floor on pass range, in uu. The effective range is the larger of this and the fraction below. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing", meta = (DisplayName = "Pass Range Floor (uu)", ClampMin = "0.0", ClampMax = "30000.0", UIMin = "500.0", UIMax = "10000.0"))
	float BotPassMinRange = 3200.f;

	/** Fraction of the field HALF-LENGTH a bot will throw the Core across. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing", meta = (DisplayName = "Pass Range (fraction of field half-length)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.1", UIMax = "1.0"))
	float BotPassRangeFieldFraction = 0.55f;

	/**
	 * How much closer to the goal a receiver must be, as a fraction of the field HALF-LENGTH.
	 *
	 * Pairs with BotEscortLeadFieldFraction: if a screening escort's own station is not further
	 * forward than this threshold, then by construction no escort is ever a legal receiver and
	 * passing silently never happens. That is exactly what was measured — zero passes in fifty
	 * runs. Keep BotEscortLeadFieldFraction comfortably above this value.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing", meta = (DisplayName = "Min Receiver Goal Advantage (fraction of field half-length)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5"))
	float BotPassMinGoalAdvantageFieldFraction = 0.10f;

	/**
	 * An enemy this close (uu) to a carrying bot turns passing into a reflex rather than a plan: the
	 * advantage requirement drops to zero and any open teammate becomes a legal outlet.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Passing", meta = (DisplayName = "Pass Pressure Radius (uu)", ClampMin = "0.0", ClampMax = "10000.0", UIMin = "0.0", UIMax = "4000.0"))
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
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Hold Margin (s)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.75"))
	float BotPassHoldMargin = 0.20f;

	/** Bot-side MIRROR of the rule's pass dwell. Keep equal to the pawn's hold requirement. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Hold Duration (s) [mirror of the rule]", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.1", UIMax = "2.0"))
	float BotPassHoldSeconds = 0.50f;

	/** Bot-side MIRROR of the rule's 2 s post-pass cooldown, plus a little so bots never spam it. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Pass Cooldown (s) [mirror of the rule]", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "6.0"))
	float BotPassCooldownSeconds = 2.30f;

	/**
	 * How close the aim must be to the receiver before the bot commits the input, in degrees.
	 *
	 * The bot lines up with the shield still UP (ETraceBotPassPhase::Lining), which costs nothing,
	 * and only presses once it is inside this cone. Pressing early would burn the vulnerable window
	 * on slewing rather than on the dwell.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Aim Tolerance (deg)", ClampMin = "0.1", ClampMax = "45.0", UIMin = "1.0", UIMax = "15.0"))
	float BotPassAimToleranceDegrees = 4.f;

	/**
	 * Seconds a bot will spend lining up before giving up on this receiver.
	 *
	 * A carrier that spends four seconds slewing at a teammate who keeps moving behind cover is a
	 * carrier not running at the goal.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Max Line-Up Time (s)", ClampMin = "0.05", ClampMax = "10.0", UIMin = "0.2", UIMax = "3.0"))
	float BotPassMaxLineUpSeconds = 1.10f;

	/**
	 * Radius (uu) inside which an enemy with line of sight counts as "covering" the carrier, and so
	 * as a reason not to start a pass. Scaled by FTraceBotProfile::PassCaution.
	 *
	 * This is the risk half of the spec's central loop expressed as a number: it is the distance at
	 * which a gun is close enough to convert half a second of dropped shield into a kill.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Safe Radius (uu)", ClampMin = "0.0", ClampMax = "20000.0", UIMin = "0.0", UIMax = "6000.0"))
	float BotPassSafeRadius = 2200.f;

	// ------------------------------------------------------------------------------------------
	// Bots — punishing the passer
	// ------------------------------------------------------------------------------------------

	/**
	 * Range (uu) at which a punisher will hold a bead on the enemy carrier.
	 *
	 * Also the radius used by CountEnemiesCoveringMe(), so the carrier's idea of "someone can shoot
	 * me" and the defender's idea of "I can shoot the carrier" are the same number by construction.
	 * If those two ever disagree, one side of the pass loop is playing a different game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Punish", meta = (DisplayName = "Punish Range (uu)", ClampMin = "100.0", ClampMax = "20000.0", UIMin = "500.0", UIMax = "8000.0"))
	float BotPunishRange = 2600.f;

	/**
	 * Standoff a punisher tries to hold from the carrier, as a fraction of BotPunishRange.
	 *
	 * Deliberately not zero: a punisher that walks onto the carrier is inside its own trace-hunting
	 * teammates' crossing lanes, and is also the first thing a boosting carrier escapes past.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Punish", meta = (DisplayName = "Punish Standoff (fraction of Punish Range)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float BotPunishStandoffFraction = 0.55f;

	// ------------------------------------------------------------------------------------------
	// Bots — positional aim  (mechanics spec §6: head 100 / body 40 / leg 25)
	//
	// Offsets from the character's actor origin (capsule centre), in uu. The capsule is 88 uu half
	// height, so the head sits near the top of it and the legs near the bottom. These are the points
	// a bot's aim converges on; FTraceBotProfile::HeadshotAimFraction chooses between them.
	// ------------------------------------------------------------------------------------------

	/** Head aim point, +Z from the capsule centre. The capsule half-height is 88, so ~62 is the skull. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Aim", meta = (DisplayName = "Head Aim Offset Z (uu)", ClampMin = "-200.0", ClampMax = "200.0", UIMin = "0.0", UIMax = "100.0"))
	float BotAimHeadOffsetZ = 62.f;

	/** Body (centre mass) aim point, +Z from the capsule centre. Also the point the LOS test uses. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Aim", meta = (DisplayName = "Body Aim Offset Z (uu)", ClampMin = "-200.0", ClampMax = "200.0", UIMin = "-50.0", UIMax = "80.0"))
	float BotAimBodyOffsetZ = 20.f;

	/**
	 * Where a bot aims when it is deliberately going low.
	 *
	 * Not currently chosen on purpose by any difficulty — it exists because vertical aim error
	 * around the body point already produces leg hits, and naming the zone makes that visible in the
	 * damage numbers instead of looking like a miss that happened to land.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Aim", meta = (DisplayName = "Leg Aim Offset Z (uu)", ClampMin = "-200.0", ClampMax = "200.0", UIMin = "-100.0", UIMax = "20.0"))
	float BotAimLegOffsetZ = -50.f;

	// ------------------------------------------------------------------------------------------
	// Bots — movement kit  (mechanics spec §5)
	//
	// Dash, slide, crouch fast-fall and boost. Each has ONE job below; none of them fire randomly.
	// The cooldowns here are bot-side mirrors of the rule values for the same reason the pass
	// durations are (see above) — the authoritative cooldown lives on the movement component.
	// ------------------------------------------------------------------------------------------

	/** Bot-side MIRROR of DashCooldown, used by the shadow charge model. Keep the two equal. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Dash Cooldown (s) [mirror of Movement|Dash]", ClampMin = "0.05", ClampMax = "30.0", UIMin = "0.5", UIMax = "10.0"))
	float BotDashCooldownSeconds = 3.5f;

	/** Bot-side MIRROR of BoostCooldown. Keep the two equal. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Boost Cooldown (s) [mirror of Movement|Boost]", ClampMin = "0.05", ClampMax = "60.0", UIMin = "1.0", UIMax = "20.0"))
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
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Boost-When-Stuck Delay (s)", ClampMin = "0.1", ClampMax = "10.0", UIMin = "0.3", UIMax = "4.0"))
	float BotBoostStuckSeconds = 1.30f;

	/** Minimum planar speed (uu/s) before a slide is worth starting. Sliding from a standstill is a crouch. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Slide Min Speed (uu/s)", ClampMin = "0.0", ClampMax = "3000.0", UIMin = "100.0", UIMax = "1500.0"))
	float BotSlideMinSpeed = 480.f;

	/**
	 * How long a bot holds the crouch input once it decides to slide. Keep at or under SlideDuration.
	 *
	 * MUST TRACK SlideDuration. Bots release crouch after exactly this long, so at the old 0.70
	 * against a 1.8s slide every bot slide ended in the commit window and the longer slide was
	 * invisible in bot play. 1.60 lets a bot slide run essentially to its duration.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Slide Hold (s)", ClampMin = "0.05", ClampMax = "6.0", UIMin = "0.1", UIMax = "3.0"))
	float BotSlideHoldSeconds = 1.60f;

	/** Bot-side pacing on slides, so a bot does not spend the whole match on its side. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Slide Cooldown (s)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.5", UIMax = "8.0"))
	float BotSlideCooldownSeconds = 3.20f;

	/**
	 * Minimum height above the floor at which a bot will crouch to kill its upward momentum.
	 *
	 * The fast-fall exists to get back to the ground quickly — a bot in the air cannot dash usefully
	 * and cannot change direction, which is a bad place to be while being shot at. Below this height
	 * the fall is over before the input would matter.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Fast-Fall Min Height (uu)", ClampMin = "0.0", ClampMax = "2000.0", UIMin = "0.0", UIMax = "600.0"))
	float BotFastFallMinHeight = 140.f;

	// ------------------------------------------------------------------------------------------
	// Bots — legacy scalars
	//
	// Superseded by the FTraceBotProfile above, which is what ATraceBotController actually reads.
	// They survive because UI/TraceMatchOptions.cpp writes to them to express its own difficulty
	// curve, and deleting them would not compile. Treat them as read-only history: changing them
	// no longer changes how a bot plays.
	//
	// Marked AdvancedDisplay so the panel folds them out of the way rather than presenting seven
	// dead knobs alongside the live ones.
	// ------------------------------------------------------------------------------------------

	/** DEAD — superseded by FTraceBotProfile::ReactionTimeSeconds. */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Bots|Legacy", meta = (DisplayName = "Reaction Time [DEAD]", ClampMin = "0.0", ClampMax = "5.0"))
	float BotReactionTime = 0.32f;

	/** DEAD — superseded by FTraceBotProfile::AimErrorDegrees. */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Bots|Legacy", meta = (DisplayName = "Aim Error [DEAD]", ClampMin = "0.0", ClampMax = "45.0"))
	float BotAimErrorDegrees = 4.5f;

	/** DEAD — superseded by FTraceBotProfile::Aggression. */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Bots|Legacy", meta = (DisplayName = "Aggression [DEAD]", ClampMin = "0.0", ClampMax = "1.0"))
	float BotAggression = 0.75f;

	/** DEAD — superseded by FTraceBotProfile::DecisionInterval. */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Bots|Legacy", meta = (DisplayName = "Decision Interval [DEAD]", ClampMin = "0.02", ClampMax = "2.0"))
	float BotDecisionInterval = 0.2f;

	/** DEAD — superseded by FTraceBotProfile::SightRange. */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Bots|Legacy", meta = (DisplayName = "Sight Range [DEAD]", ClampMin = "0.0", ClampMax = "40000.0"))
	float BotSightRange = 6000.f;

	/** DEAD — superseded by FTraceBotProfile::PreferredCombatRange. */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Bots|Legacy", meta = (DisplayName = "Preferred Combat Range [DEAD]", ClampMin = "0.0", ClampMax = "20000.0"))
	float BotPreferredCombatRange = 1600.f;

	/** DEAD — superseded by FTraceBotProfile::PassChance. */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Bots|Legacy", meta = (DisplayName = "Pass Chance [DEAD]", ClampMin = "0.0", ClampMax = "1.0"))
	float BotPassChance = 0.35f;

	// ==========================================================================================
	// NET
	// ==========================================================================================

	/** Master switch for server-side rewind on hitscan. Off = resolve against present-day poses. */
	UPROPERTY(config, EditAnywhere, Category = "Net", meta = (DisplayName = "Enable Lag Compensation"))
	bool bEnableLagCompensation = true;

	/**
	 * Upper bound on how far back the server will rewind, in seconds.
	 *
	 * 0.25 covers ~250ms RTT; beyond that a shooter's claim is rejected rather than honoured, which
	 * is what stops high-ping players from shooting into the past. Must stay BELOW
	 * LagCompHistoryDuration with headroom, or the rewind target falls off the end of the buffer.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Net", meta = (DisplayName = "Max Rewind Time (s)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5"))
	float MaxRewindTime = 0.25f;

	/**
	 * How much pose history each character keeps, in seconds. MUST EXCEED MaxRewindTime.
	 *
	 * This is a memory dial: ten characters at 60Hz for one second is 600 stored poses.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Net", meta = (DisplayName = "Lag Comp History (s)", ClampMin = "0.05", ClampMax = "5.0", UIMin = "0.25", UIMax = "2.0"))
	float LagCompHistoryDuration = 1.f;

	/** Draws the rewound capsules the server actually tested against. Noisy; dev only. */
	UPROPERTY(config, EditAnywhere, Category = "Net", meta = (DisplayName = "Draw Server Rewind Debug"))
	bool bDrawServerRewindDebug = false;

	// ==========================================================================================
	// CONTROLS  (legacy)
	// ==========================================================================================

	/**
	 * DEAD. NO LONGER THE PLAYER'S SENSITIVITY.
	 *
	 * The human look scale is now a persisted per-player setting: UTraceUserSettings::
	 * LookSensitivity (default 1.50), plus a separate vertical multiplier and an invert-Y toggle,
	 * all rebuilt into the Look mapping's modifier scalars by
	 * ATracePlayerController::ApplyControlSettings(). Editing this value does not move a human's
	 * crosshair.
	 *
	 * Nothing reads it any more. Its last caller was the -TraceWalkHuman debug harness in
	 * AI/TraceBotController.cpp, which divided by it to convert a desired yaw into synthetic mouse
	 * delta; that was fixed at integration to read UTraceUserSettings::GetLookScaleX(), i.e. the
	 * scalar actually installed in the Look mapping. Kept only so an existing DefaultGame.ini that
	 * still carries the key does not warn. Do not wire anything new to it.
	 */
	UPROPERTY(config, EditAnywhere, AdvancedDisplay, Category = "Controls", meta = (DisplayName = "Look Sensitivity [DEAD — see TraceUserSettings]", ClampMin = "0.01", ClampMax = "20.0"))
	float LookSensitivity = 2.5f;
};
