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
	 * Sane range 5000 to 8000 on a 24000 x 9600 field. Must stay above MaxEngagementRange or a bot
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
	 * Scales the number of covering enemies tolerated inside UTraceSettings::BotPunishRange (which
	 * ATraceBotController::CountEnemiesCoveringMe uses as its radius). At 0 a bot passes whenever it
	 * has a receiver, drops its shield in front of a firing line and dies for it; at 1 it waits for
	 * a genuinely clean window.
	 *
	 * Low on Easy on purpose. A reckless pass is a turnover and a free kill for the player, which is
	 * one of the few ways to make Easy easier that does not involve making the bots look stupid.
	 */
	UPROPERTY(EditAnywhere, Category = "Tempo", meta = (DisplayName = "Pass Caution (0-1)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float PassCaution = 0.30f;

	/**
	 * 0..1. How much of the movement kit (slide, crouch fast-fall) this bot uses.
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

	// DEAD PROPERTY REMOVED: MatchDuration.
	//
	// Spec §1 replaced the single timed match with TWO HALVES. The enforced length of each half is
	// ATraceGameMode::HalfDuration (config=Game on the game mode, [/Script/Trace.TraceGameMode]),
	// and the game mode also owns whether the score cap ends the match early
	// (bEndMatchAtScoreToWin, off by default — "first to 5" would cut the second half, and the side
	// switch with it, out of most matches). Nothing read MatchDuration; a slider that silently does
	// nothing is worse than no slider, so it is gone rather than folded away under AdvancedDisplay.

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
	 * field is 24000 x 9600 (spec v3 §7 narrowed it from 12000 for the 2.5:1 proportion), so the
	 * diagonal is ~25849; 28000 still covers it with 2151 uu of margin. This was 15000 — correct for
	 * the old 8000 x 4000 arena and barely half the field once it was scaled up.
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

	// DEAD PROPERTY REMOVED: SpreadDegrees.
	//
	// Spec §6: "There is no movement inaccuracy. Set spread to 0." UTraceWeaponComponent::FireOnce
	// does not roll a cone at all; the shot IS the aim ray. The roll was removed rather than
	// configured to zero so that a stale .ini cannot quietly reintroduce inaccuracy the design has
	// deleted — and so a modified client cannot roll itself a zero nobody else gets. The knob has
	// now followed the code out of the file.

	// ==========================================================================================
	// MOVEMENT
	//
	// The most feel-critical block on the page, and the one designers drag sliders on while PIE is
	// running. Everything here IS live: the movement component reads DashSpeed / DashDuration /
	// DashCooldown / the whole slide block / the whole air block at the point of use, and
	// WalkSpeed — the one value the engine copies into its own field — is pushed by
	// ApplyLiveMovementTuning() from PostEditChangeProperty. See the file header.
	//
	// BOOST IS GONE (spec v3 §1: "remove boost from the game entirely"). BoostZVelocity,
	// BoostCooldown and the two bot-side boost knobs were deleted with the feature rather than left
	// behind as sliders that move nothing.
	//
	// THE MOVEMENT MODEL CHANGED THIS PASS (spec v3 §2, "mimicking apex legends movement and source
	// engine"). Three things follow from that, and all three are knobs in this block:
	//   * air control is a Source-style ACCELERATION PROJECTION (Movement|Air), not a lerp toward
	//     the input direction, so input perpendicular to travel turns you without costing speed;
	//   * landing no longer clamps horizontal speed to the ground maximum (Movement|Landing) —
	//     ground friction bleeds the excess instead;
	//   * a slide's velocity comes from the speed you entered it with (Movement|Slide).
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

	// --- Air control (Source/Quake air-accel) -------------------------------------------------
	//
	// THE MODEL, because these four knobs are meaningless without it.
	//
	// Every frame in the air the movement component takes the player's wish direction W (normalised
	// input, planar), and the speed the pawn ALREADY has along that direction:
	//
	//     CurrentAlongWish = Velocity . W
	//     AddSpeed         = min(MaxAirSpeed, AirMaxWishSpeed) - CurrentAlongWish
	//     if (AddSpeed <= 0)  -> no change at all this frame
	//     Accel            = min(AirAcceleration * dt, AddSpeed)
	//     Velocity        += Accel * W
	//
	// That projection — and specifically the fact that the cap applies to the component ALONG the
	// wish direction rather than to total speed — is the whole trick. Point the stick sideways to
	// where you are already travelling and CurrentAlongWish is ~0, so you get the full add, applied
	// at ninety degrees: the velocity VECTOR ROTATES and its magnitude barely changes. Point it
	// straight ahead and CurrentAlongWish is already your full speed, so AddSpeed is negative and
	// you get nothing. This is why the spec asks for the real formula and explicitly not a lerp
	// toward the input direction: a lerp bleeds speed on every turn, which is the opposite result.
	//
	// AirMaxWishSpeed is therefore the "how sharply can I turn in the air" dial, and it is what
	// the note "slightly increase efficacy of strafing in mid air" is asking for.
	//
	// Read by UTraceCharacterMovementComponent::ApplySourceAirAcceleration, which runs inside
	// CalcVelocity — i.e. inside the physics sub-step, on the client, on the server and on every
	// replayed move — and is a pure function of (Velocity, Acceleration, dt, these values), so it
	// adds no saved-move state and every one of these knobs is safe to drag mid-PIE.

	/**
	 * Master switch for the Source/Quake air model. OFF restores Unreal's stock AirControl lerp.
	 *
	 * Exists so the new movement can be A/B'd against the old feel from one binary rather than two.
	 * Turning it off does NOT restore the landing clamp — that is bPreserveLandingMomentum below.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Source-Style Air Acceleration"))
	bool bSourceAirAcceleration = true;

	/**
	 * Ceiling, in uu/s, on the velocity component ALONG the wish direction that air input will
	 * build. THIS IS THE AIR-STRAFE DIAL.
	 *
	 * Source ships the equivalent of ~57 uu/s here, which is deliberately tiny — it is what makes
	 * strafe-jumping a skill rather than a control scheme. The default here is a good deal higher
	 * because spec v3 asks for "full control authority to redirect velocity in air" and for
	 * strafing to be MORE effective, not less.
	 *
	 * Raising it turns the air into a plane you can steer freely on; lowering it toward ~60 gives
	 * the classic sharp Quake/Source feel where only perpendicular input does anything. It does NOT
	 * raise your top speed — MaxAirSpeed does that — it raises how much of a turn one frame of
	 * input can buy. Sane range 60 to 400.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Max Wish Speed (uu/s along wish dir)", ClampMin = "0.0", ClampMax = "2000.0", UIMin = "40.0", UIMax = "600.0"))
	float AirMaxWishSpeed = 160.f;

	/**
	 * How hard the air-accel step pushes, in uu/s^2.
	 *
	 * Together with the cap above this decides whether a turn takes one frame or several: at 8000
	 * and a 60Hz frame the step can add 133 uu/s, so the 160 cap is very nearly reached in a single
	 * frame and the model behaves as "cap per frame". Lower it and air control becomes gradual and
	 * floaty. Sane range 3000 to 12000.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Acceleration (uu/s^2)", ClampMin = "0.0", ClampMax = "50000.0", UIMin = "1000.0", UIMax = "20000.0"))
	float AirAcceleration = 8000.f;

	/**
	 * Ceiling on planar speed that AIR INPUT may build toward, in uu/s.
	 *
	 * NOT a hard clamp on the pawn's velocity, and deliberately so: speed carried into the air by a
	 * dash or a slide is left alone, because clamping it is exactly the "velocity gets reset by a
	 * state transition" complaint spec v3 §2.4 raises. This only bounds what holding a direction
	 * key in mid-air can ACCELERATE you to. Keep it at or above WalkSpeed; well above it lets a
	 * skilled player convert air time into speed, which is the Apex/Source reading.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Max Air Speed (uu/s)", ClampMin = "50.0", ClampMax = "8000.0", UIMin = "400.0", UIMax = "3000.0"))
	float MaxAirSpeed = 1600.f;

	/**
	 * Lateral drag in the air, per second (the engine's FallingLateralFriction). ZERO BY DESIGN.
	 *
	 * Source and Quake have no air drag at all, which is what makes a jump preserve the speed you
	 * took into it — the single most-requested property of this movement model. Any non-zero value
	 * here quietly undoes momentum preservation for every airborne frame, so it is a knob for
	 * experiments rather than a dial to season with. Sane range 0 to 0.5.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Friction (lateral, per s)", ClampMin = "0.0", ClampMax = "8.0", UIMin = "0.0", UIMax = "1.0"))
	float AirFriction = 0.f;

	/**
	 * Engine-owned: the fraction of input acceleration that survives into the falling state
	 * (UCharacterMovementComponent::AirControl). Pushed into the component by
	 * RefreshEngineTunablesFromSettings(), so it is live but is a COPY — the one air value that
	 * cannot be read at the point of use.
	 *
	 * 1.0, NOT the 0.45 the constructor used to set. Under the stock model AirControl WAS the air
	 * model, and 0.45 was how you stopped it being too strong. Under the Source model it sits in
	 * front of the real model: anything below 1 quietly scales the acceleration before
	 * ApplySourceAirAcceleration ever sees it, so the air block above would no longer mean what it
	 * says. Tune AirMaxWishSpeed instead; leave this at 1.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Control (engine acceleration scale)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float AirControl = 1.f;

	// --- Landing and carried momentum ----------------------------------------------------------
	//
	// Spec v3 §2.2, verbatim: "do not clamp horizontal velocity to ground max speed on landing.
	// Velocity carries over from air to ground."
	//
	// Unreal does not have a landing clamp you can switch off — it has CalcVelocity, which brakes
	// hard the moment IsExceedingMaxSpeed(MaxWalkSpeed) is true. GroundFriction 8 x
	// BrakingFrictionFactor 2, plus BrakingDecelerationWalking 2600, kills 1000 uu/s of carried
	// speed in about 60 ms, and 60 ms is indistinguishable from a clamp. So the movement component
	// takes over CalcVelocity for exactly the frames where planar speed exceeds the ground limit
	// and bleeds the EXCESS at the three much gentler numbers below.

	/**
	 * ON BY DEFAULT — this is spec v3 §2.2. Off restores the engine's hard brake on landing.
	 *
	 * It exists so the old feel can be A/B'd from one binary. Turning it off makes the three
	 * overspeed knobs below inert.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Landing", meta = (DisplayName = "Preserve Landing Momentum"))
	bool bPreserveLandingMomentum = true;

	/**
	 * Friction applied to the EXCESS above walk speed while on the ground, replacing the engine's
	 * GroundFriction (8) for those frames only. THE MAIN "how long does carried speed last" DIAL.
	 *
	 * Overspeed has to leave somehow, or a player who lands fast keeps that speed forever and walk
	 * speed stops meaning anything. The Source-style answer is that friction bleeds it over a short
	 * run-out rather than the game snapping it away in one frame. Lower = momentum is worth more;
	 * at 8 the excess is gone almost immediately and preserving it buys nothing. Sane range 1.5 to 4.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Landing", meta = (DisplayName = "Overspeed Ground Friction", ClampMin = "0.0", ClampMax = "16.0", UIMin = "0.5", UIMax = "8.0"))
	float GroundOverspeedFriction = 2.f;

	/**
	 * Flat deceleration, uu/s^2, applied to the excess above walk speed when there is NO input —
	 * the counterpart of BrakingDecelerationWalking (2600) for overspeed frames.
	 *
	 * Separate from the friction term because friction is proportional and a proportional bleed
	 * alone has a long tail: it takes the same time to go 2000 -> 1000 as 1000 -> 500, so a fast
	 * landing leaves the pawn hovering just above walk speed for an implausibly long time. This is
	 * what actually lands them. Sane range 200 to 800; at 2600 you have the engine's brake back.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Landing", meta = (DisplayName = "Overspeed Braking (uu/s^2)", ClampMin = "0.0", ClampMax = "5000.0", UIMin = "100.0", UIMax = "2000.0"))
	float GroundOverspeedBraking = 400.f;

	/**
	 * Degrees per second an overspeed pawn may steer while it bleeds down, on the ground.
	 *
	 * Unlimited steering while overspeed would let a player carry a landing's momentum around a
	 * corner at full value, which turns the whole model into free speed; zero would make a fast
	 * landing a rail you cannot correct on. This is the middle ground and it is the knob to reach
	 * for if landings feel either "on ice" (raise it) or "too free" (lower it). Sane range 90 to 360.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Landing", meta = (DisplayName = "Overspeed Turn Rate (deg/s)", ClampMin = "0.0", ClampMax = "720.0", UIMin = "0.0", UIMax = "360.0"))
	float GroundOverspeedTurnRate = 180.f;

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

	/**
	 * Multiple of the ground speed limit a dash hands back when it ends. 1.0 = the old behaviour.
	 *
	 * Spec v3 §2.4: "state transitions should preserve velocity vectors rather than resetting
	 * them". A dash ending by dumping the pawn at exactly walking pace is the most visible reset in
	 * the kit — you spend a cooldown, cross 540 uu, and arrive slower than a slide would have left
	 * you. Above 1 the surplus is handed back as real overspeed and then bleeds off through
	 * GroundOverspeedFriction like any other carried momentum, so this cannot become permanent
	 * free speed. Sane range 1.0 to 1.5.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "Dash Exit Speed (multiple of max speed)", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float DashExitSpeedMultiplier = 1.25f;

	// --- Slide (crouch on the ground) -------------------------------------------------------
	//
	// A slide SPENDS MOMENTUM YOU ALREADY HAVE: it starts from your current speed, boosts it once
	// and then bleeds it off slowly. It never resizes the capsule — the capsule is the single source
	// of truth for hit resolution, lag compensation and the trail trip test.
	//
	// The numbers below were retuned for "longer, and it must preserve the player's momentum": the
	// decay is gentle enough that a slide carries most of its entry speed all the way to its natural
	// end, and the duration is long enough for that to be a traversal tool rather than a flourish.
	//
	// SPEC v3 §2.3 CHANGED THREE THINGS HERE:
	//   * entry speed decides slide velocity, so SlideEntrySpeedMultiplier drops to 1.0;
	//   * SlideImpulse is new, and is the other (contradictory) reading of the same note — see the
	//     [CONFLICT] discussion on the multiplier below;
	//   * the cooldown is 0.8 s and is now measured from the slide's END rather than its START,
	//     which is why it is a differently named property (SlideCooldownSeconds).

	/**
	 * Fraction of WalkSpeed you must already be moving at before crouch will start a slide.
	 * Stops "tap crouch from a standstill" being free speed. Sane range 0.4 to 0.7.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Entry Speed (fraction of Walk Speed)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.2", UIMax = "1.0"))
	float SlideEntrySpeedFraction = 0.55f;

	/**
	 * Entry speed multiplier applied to the ACTUAL entry speed. Slide velocity = entry speed x this,
	 * plus SlideImpulse.
	 *
	 * Not max(entry speed, WalkSpeed) — that was the old implementation and the old wording, and the
	 * floor is exactly the flat boost §2.3 rules out: it made a slide entered at walking pace come
	 * out 35% faster than the walk. It multiplies what you actually arrived with, nothing else.
	 *
	 * ONE HALF OF A CONFLICT THE SPEC LEFT OPEN, AND THE HALF THAT IS SHIPPED ACTIVE.
	 * Spec v3 §2.3 says "entry speed determines slide velocity (NO FLAT MOMENTUM BOOST)", and a
	 * later line in the same notes says "have the slide INCREASE MOMENTUM". Those disagree, so both
	 * readings are knobs rather than a decision made on the designer's behalf:
	 *   * this multiplier at 1.0 is the §2.3 reading — the slide is exactly the speed you brought;
	 *   * SlideImpulse below, at any non-zero value, is the "increase momentum" reading.
	 * Shipped at 1.0 / 0.0, i.e. the §2.3 reading. Raise one or the other to pick the other one.
	 *
	 * Because it multiplies your CURRENT speed, a slide out of a fast approach is faster than a
	 * slide out of a walk — that is the momentum preservation, and it holds at 1.0. Sane range 1.0
	 * to 1.5. Was 1.35 before spec v3.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Entry Speed Multiplier", ClampMin = "0.5", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float SlideEntrySpeedMultiplier = 1.0f;

	/**
	 * Flat uu/s added to slide entry speed, on top of the multiplier. ZERO BY DEFAULT.
	 *
	 * The other half of the §2.3-versus-"increase momentum" conflict above. Unlike the multiplier
	 * this is a FLAT boost — it is worth the same whether you entered at a walk or out of a dash,
	 * which is precisely what "no flat momentum boost" rules out and what "increase momentum"
	 * asks for. Deliberately applied AFTER the SlideMaxSpeed clamp, so it is never silently eaten
	 * by the entry cap; if it were clamped, dialling it in from zero would appear to do nothing for
	 * anyone entering a slide fast.
	 *
	 * Try 150-300 for a noticeable kick without making crouch-spam the fastest way to travel.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Entry Impulse (flat uu/s)", ClampMin = "0.0", ClampMax = "2000.0", UIMin = "0.0", UIMax = "600.0"))
	float SlideImpulse = 0.f;

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
	 * "make the slide longer" dial. Sane range 0.8 to 2.5. Independent of SlideCooldownSeconds now
	 * that the cooldown is measured from the slide's end.
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
	 * The gap BETWEEN slides, in seconds. Spec v3 §2.3: "add a .8second buffer between slides".
	 *
	 * MEASURED FROM THE SLIDE'S END, and that is a change. The old SlideCooldown was measured from
	 * the slide's START, so "the buffer between slides" was a number you had to compute (cooldown
	 * minus duration) rather than one you could read — and a value below SlideDuration meant no
	 * cooldown at all, which is a trap. This is the number the spec asks for, directly.
	 *
	 * RENAMED (SlideCooldown -> SlideCooldownSeconds) ALONG WITH THE SEMANTIC CHANGE, deliberately:
	 * a stale .ini or a saved config still carrying the old START-measured 2.40 must not land on
	 * the new END-measured knob and silently produce a four-second gap between slides.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Cooldown (s, gap after a slide ends)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "6.0"))
	float SlideCooldownSeconds = 0.8f;

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
	 * Ceiling on the exit speed as a multiple of max speed.
	 *
	 * The old warning here — "above 1, overspeed is kept for as long as a movement key is held, and
	 * slide-cancel spam becomes the fastest way to cross the arena" — described the pre-spec-v3
	 * CalcVelocity, whose input branch clamped to the current speed and so could HOLD overspeed
	 * indefinitely. That branch is gone. The ground overspeed bleed replaces it and always bleeds the
	 * excess (GroundOverspeedFriction / GroundOverspeedBraking), so exit overspeed now decays no
	 * matter what is held. Raising this is a real knob again rather than an exploit.
	 *
	 * Note this is a ceiling, not the whole story: SlideExitMaxSpeed is taken as max(this x max
	 * speed, the slide's own speed), which is what stops slide->jump being a hard brake to walk pace.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Exit Ceiling (multiple of Max Speed)", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float SlideExitMaxSpeedMultiplier = 1.0f;

	// BOOST DELETED (spec v3 §1: "remove boost from the game entirely"). BoostZVelocity and
	// BoostCooldown lived here; the bot-side mirrors BotBoostCooldownSeconds and BotBoostStuckSeconds
	// went with them. Nothing on this page describes a boost any more.

	// ==========================================================================================
	// CORE  —  the hover pass
	//
	// FIVE DEAD KNOBS WERE REMOVED FROM THIS CATEGORY AND REPLACED BY THE SEVEN BELOW.
	//
	// The Core stopped being a thrown, catchable object: it is a STATUS that transfers by holding
	// the crosshair on a teammate (see the ATraceCore file header). So PassSpeed, PassUpwardBias,
	// PickupRadius, PickupLockoutAfterThrow and CoreResetTime described a projectile and a pickup
	// that no longer exist and were read by nothing. (ATraceCore::Throw and IsPickupLockedOutFor,
	// the two functions that used to make that argument concrete, have since been deleted as well —
	// they had zero callers between them.) There is no loose Core to reset. They are deleted rather than folded away: the user is tuning from this panel, and a
	// slider that silently does nothing is worse than no slider.
	//
	// What replaces them is the set of numbers the hover pass ACTUALLY runs on. Those were
	// compile-time constants in TraceCoreTuning (Gameplay/TraceCore.cpp), whose own comment asked
	// for exactly this promotion: "Every one of them is a designer knob and they should be promoted
	// to UTraceSettings (Category = "Core") verbatim". Read at the point of use, so all of them
	// retune with PIE running.
	// ==========================================================================================

	/**
	 * Seconds the carrier must hold the pass input with the crosshair on a legal receiver before
	 * the Core transfers. Spec §4.
	 *
	 * This is the risk beat: the carrier's shield drops the instant the input goes down and the
	 * trace goes invulnerable, and both are restored if the pass is cancelled. Longer means a
	 * bigger window for a punisher to convert; shorter means passing is close to free. Keep
	 * BotPassHoldSeconds equal to it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Pass Hold (s)", ClampMin = "0.05", ClampMax = "5.0", UIMin = "0.1", UIMax = "2.0"))
	float PassHoldSeconds = 0.50f;

	/**
	 * Seconds before another pass may be STARTED after one COMPLETES. A cancelled attempt spends
	 * PassCancelCooldownSeconds instead, which is much shorter — see below.
	 *
	 * Keep BotPassCooldownSeconds at or just above it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Pass Cooldown (s, on completion)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "6.0"))
	float PassCooldownSeconds = 2.0f;

	/**
	 * Seconds before another pass may be started after one was CANCELLED. A large part of the
	 * reported bug, and the reason it is now a separate number.
	 *
	 * This used to be PassCooldownSeconds — two full seconds — for a pass the player never chose to
	 * abandon. The sequence they experience: hold the input on a teammate, watch the ring fill,
	 * watch it vanish because the receiver clipped a rail, and then get NOTHING for two more
	 * seconds while still holding the button on a perfectly legal target. Twice in a row and the
	 * mechanic reads as broken, which is precisely the report.
	 *
	 * It cannot go to zero: the cancel path flips the shield and the trace invulnerability and
	 * forces a net update, so a permanently-illegal target would churn that every frame. 0.25
	 * bounds the churn to (grace + this) = 0.4 s per cycle while sitting well below the threshold
	 * at which a player perceives a lockout at all.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Pass Cooldown (s, on cancel)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "3.0"))
	float PassCancelCooldownSeconds = 0.25f;

	/**
	 * Longest pass the rules allow, in uu.
	 *
	 * The bots' own pass range works out at ~6600uu (BotPassRangeFieldFraction 0.55 of a 12000uu
	 * field HALF-LENGTH — that is half of 24000, not the width), so this comfortably contains every
	 * pass a bot attempts while still making a
	 * cross-map hail mary illegal. Keep it above the bots' figure or bots will start passes the
	 * rules then refuse.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Max Pass Range (uu)", ClampMin = "100.0", ClampMax = "40000.0", UIMin = "1000.0", UIMax = "20000.0"))
	float PassMaxRange = 8000.f;

	/**
	 * Half-angle of the pass acquisition cone, in degrees. "Hover the crosshair over a teammate",
	 * expressed as an angle.
	 *
	 * 6 -> 9 THIS PASS, and it was a prime suspect in the pass-inconsistency report. At 6 degrees a
	 * teammate 4000uu away had to be held inside a circle about 420uu across while both of you were
	 * running, which is a lot to ask on a field whose cover just got denser. There is a second,
	 * independent test (PassAimSlack) that covers the close-range case, so this only affects distant
	 * receivers — and it is still the first number to raise if passing feels like it is refusing you.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Aim Cone (deg)", ClampMin = "0.5", ClampMax = "45.0", UIMin = "2.0", UIMax = "20.0"))
	float PassAimConeDegrees = 9.f;

	/**
	 * Extra uu added to the receiver's capsule radius for the second, distance-based acquisition
	 * test: the aim ray counts as on-target if it passes within (CapsuleRadius + this) of the
	 * receiver's capsule axis.
	 *
	 * Either test acquiring is enough. The angular cone is what makes a distant teammate reachable;
	 * this is what stops a near one feeling sloppy — at point-blank range a 9 degree cone is
	 * narrower than the teammate's own body. 40 -> 70 this pass, so a near receiver is acquirable
	 * well off their silhouette rather than only dead centre.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Aim Slack (uu beyond capsule radius)", ClampMin = "0.0", ClampMax = "500.0", UIMin = "0.0", UIMax = "200.0"))
	float PassAimSlack = 70.f;

	/**
	 * Seconds an IN-FLIGHT pass survives a receiver who has momentarily stopped being legal. THE
	 * FIX FOR THE REPORTED PASS REGRESSION.
	 *
	 * A pass was measured dying 24 ms before it would have completed because the receiver crossed
	 * behind a lane rail for a handful of frames. Line of sight, "under the crosshair" and range
	 * are all instantaneous tests sampled once per frame against a field whose cover just got much
	 * denser, so a receiver who is RUNNING is guaranteed to blink out of legality repeatedly during
	 * any 0.5 s hold. Without a grace, every one of those blinks is a cancelled pass — which is
	 * exactly what "passing is inconsistent on this version" feels like from the inside.
	 *
	 * COVERS ONLY THE TRANSIENT GEOMETRIC TESTS. A receiver who dies, respawns onto the other team
	 * or stops existing cancels the pass instantly, with no grace at all.
	 *
	 * Not free: it also lets a pass complete through a thin sliver of cover. 0.15 is about one rail
	 * at a run. Above ~0.4 a receiver can duck fully behind a block and still catch.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Validation Grace (s)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.6"))
	float PassValidationGraceSeconds = 0.15f;

	/**
	 * Seconds ACQUISITION keeps returning the last receiver found after they stop passing the
	 * aim/LOS tests. The acquisition-side twin of the grace above.
	 *
	 * THIS IS THE HALF OF THE REPORT THAT IS A DISPLAY PROBLEM. The HUD polls for a legal receiver
	 * about twenty times a second to decide whether to show the pass highlight at all, so a target
	 * flickering in and out of legality makes the pass OPTION itself flicker — "sometimes the pass
	 * option doesn't show up" is at least partly that, rather than a failure to acquire. Identity,
	 * team, life and range are still re-checked every poll; only the two flickery geometric tests
	 * are held over.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Acquire Sticky (s)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0"))
	float PassAcquireStickySeconds = 0.20f;

	/**
	 * True: probe chest, head AND knees for line of sight and accept any clear one. False: chest
	 * only, which is the pre-fix behaviour.
	 *
	 * A single chest-height ray against the new 176 / 352 / 616 uu cover boxes is close to a coin
	 * flip — a receiver whose head and shoulders are plainly visible over a 1x block fails the test
	 * because the one point being probed is behind it. Any clear probe meaning "line of sight" is
	 * both more generous and much closer to what the player can actually see. The chest is probed
	 * first, so the common case still resolves in one trace.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Multi-Point Line Of Sight"))
	bool bPassMultiPointLos = true;

	/**
	 * Height above a candidate's actor origin that the aim point and the first LOS probe both
	 * target, uu.
	 *
	 * Chest, not feet. Both must use the same offset or the pass can be aimed at a point the LOS
	 * test is not checking, which produces exactly the "it says I can pass and then nothing
	 * happens" class of bug.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Pass", meta = (DisplayName = "Target Chest Offset Z (uu)", ClampMin = "-100.0", ClampMax = "200.0", UIMin = "0.0", UIMax = "100.0"))
	float PassTargetChestOffsetZ = 20.f;

	/**
	 * Seconds after the Core changes TEAM before the new carrier's trace starts forming.
	 *
	 * 1.0 -> 0.4 THIS PASS (spec v3 §1). The grace exists so a turnover does not instantly wrap the
	 * new carrier in lethal trace laid on top of the scrum they just won it in; at a full second it
	 * also meant the counter-attack got a free run with no trace behind it at all. 0.4 s is about
	 * 330uu of travel at carrier speed — enough to clear the pile, short enough that the trace is a
	 * threat again before anyone has crossed open ground.
	 *
	 * Applies only when the Core changes SIDE. A pass between teammates has no grace, by design.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core", meta = (DisplayName = "Turnover Trace Grace (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.0", UIMax = "2.0"))
	float CoreTurnoverGraceSeconds = 0.4f;

	// ==========================================================================================
	// PARRY  (spec v3 §3 — new mechanic)
	//
	// Verbatim: "Create a parry mechanic for the core carrier. Parrying gives your trace
	// invulnerability for .1seconds. It also makes the entire trace turn red for the duration of
	// the parry. If an enemy would break your trace with a dash, parrying as they dash protects the
	// trace."
	//
	// So it is a reaction check on the one thing that can kill a carrier. A 0.1 s window means the
	// carrier has to read the dash, not hold a button; the red tint is what makes that readable to
	// BOTH players, which is why it covers the entire trace rather than the nearest segments.
	//
	// Composes with, and does not replace, the pass-window trace invulnerability: a parry during a
	// pass is a no-op because the trace is already invulnerable, and the parry ending must not
	// clear an invulnerability the pass still owns.
	// ==========================================================================================

	/**
	 * Seconds of trace invulnerability granted by a parry. Spec §3: 0.1.
	 *
	 * THE ENTIRE MECHANIC IS THIS NUMBER. At 0.1 s a parry has to be a read of the incoming dash;
	 * at 0.4 s it is a panic button, and the dash — the only counterplay the defence has against a
	 * carrier — stops being reliable. Raise it only if playtesting says the window is unhittable at
	 * real latency, and raise the cooldown with it. Sane range 0.08 to 0.25.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Parry", meta = (DisplayName = "Parry Duration (s)", ClampMin = "0.02", ClampMax = "2.0", UIMin = "0.05", UIMax = "0.5"))
	float ParryDuration = 0.10f;

	/**
	 * Seconds before the carrier may parry again, measured from the parry's START.
	 *
	 * [ASSUMPTION] — the spec does not give one. 1.5 s against a 0.1 s window is a ~7% uptime, which
	 * keeps it a reaction check: spamming it covers almost nothing, so a defender's dash timing
	 * still beats a carrier's button mashing. Drop it toward 0.5 and the carrier can simply hold
	 * the lane covered; the mechanic then reads as "the carrier is immune", which is not the game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Parry", meta = (DisplayName = "Parry Cooldown (s)", ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.2", UIMax = "6.0"))
	float ParryCooldown = 1.5f;

	/**
	 * Colour the ENTIRE trace turns for the duration of a parry. Spec §3 says red.
	 *
	 * A gameplay tell, not decoration: it is how the dashing enemy learns their dash is about to be
	 * wasted and how the carrier confirms the parry registered.
	 *
	 * GREEN AND BLUE ARE NEAR ZERO ON PURPOSE AND MUST STAY THAT WAY. The trace is drawn on an
	 * unlit emissive material at glow values well above 1, so every channel with any weight in it
	 * clips to white at the tonemapper — the exact failure measured when the whole trace ran at
	 * glow 3.4 and became "a shapeless white slab". (1, 0.03, 0.06) stays unambiguously RED however
	 * hard the glow is pushed, which is the entire point.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Parry", meta = (DisplayName = "Parry Trace Tint"))
	FLinearColor ParryTintColor = FLinearColor(1.f, 0.03f, 0.06f, 1.f);

	/**
	 * Emissive multiplier on the red trace while parrying, on M_TraceNeon.
	 *
	 * Above the pass window's 1.90 on purpose: the two states must not be confusable at a glance,
	 * and the parry is the shorter and more decisive of the two. Red at 2.6 is a step-change in
	 * brightness as well as in hue, so the tell survives being seen edge-on, at range, or in the
	 * corner of the eye — which is the only way an enemy already committed to a dash will see it.
	 * Do not push it far past 3: that is where the whole trace clips to white.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Parry", meta = (DisplayName = "Parry Glow Scale", ClampMin = "0.0", ClampMax = "8.0", UIMin = "1.0", UIMax = "4.0"))
	float ParryGlowScale = 2.60f;

	// ==========================================================================================
	// HUD
	// ==========================================================================================

	/**
	 * How much larger the CENTRE crosshair is drawn while the third-person carry blend is fully in.
	 *
	 * There is always a crosshair at the exact centre of the screen, in both view modes — that rule
	 * is stated in ATraceHUD::DrawCrosshair and must not be undone. This only scales it. The reason
	 * it grows at all is that the third-person camera sits behind and above the pawn, so the same
	 * pixel size reads as a smaller fraction of the visible scene and gets lost against the neon.
	 *
	 * The bug this exists to prevent is real and was reported twice: a previous pass cross-FADED the
	 * centre cross out and drew the reticle on the projected pass ray, ~30 px low. The player looked
	 * at the middle of the screen, found nothing, and reported "there's still no crosshair in third
	 * person". They were right. 1.0 here is legal and simply means "same size in both modes" — it
	 * does NOT mean "no crosshair".
	 */
	UPROPERTY(config, EditAnywhere, Category = "HUD", meta = (DisplayName = "Third-Person Crosshair Scale", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.5"))
	float ThirdPersonCrosshairScale = 1.60f;

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
	 * THIS IS THE ONLY VALUE. UTraceTrailComponent::GetTraceLifetimeSeconds() used to take a min()
	 * against a hardcoded ceiling while the two slices were landing separately; that ceiling is gone,
	 * so what you set here is what the game plays. The ClampMax below is the real bound. Sane range
	 * 2 to 4.
	 *
	 * Shortening it is a real nerf to the trail-hunting bots, whose BotTrailMinPointLifeRemaining
	 * filter is a fraction of this window written as an absolute — move the two together.
	 *
	 * 2.8 -> 2.00 this pass (spec v3 §1). BotTrailMinPointLifeRemaining moved 0.56 -> 0.40 with it,
	 * holding the "discard the oldest ~20% of the trace" ratio the two are supposed to keep.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Trail Lifetime (s)", ClampMin = "0.2", ClampMax = "4.0", UIMin = "0.5", UIMax = "4.0"))
	float TrailLifetime = 2.0f;

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
	// 8000 x 4000 field; on a 24000 x 9600 field the same numbers put every escort on top of the
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
	 * calibrated to discard the oldest ~20% of the trace: at TrailLifetime 4 that was 0.8, at 2.8 it
	 * was 0.56, and at the current 2.00 it is 0.40. Leaving it behind when TrailLifetime shrinks
	 * discards a larger share of the trace instead — the same mistake, in the same direction, that
	 * cut the trail-kill share from a measured 37.5% of kills to 25.9% when TrailLifetime went
	 * 6 -> 4. Interceptors were not worse at crossing; there was simply less legal trace to aim at.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Min Point Life Remaining (s)", ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0"))
	float BotTrailMinPointLifeRemaining = 0.40f;

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
	 * and every escort station down one corridor. This gives each bot its own
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
	 * Shortest pass a bot will bother with, in uu.
	 *
	 * Below this the "pass" is a handoff to somebody standing on top of the carrier: it moves the
	 * Core almost nowhere, spends PassCooldownSeconds, and costs the carrier PassHoldSeconds of
	 * dropped shield for no positional gain at all. That is a bad trade for the bot and reads in the
	 * log as a bug.
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
	// The two durations below MIRROR the rule values rather than reading them. That was originally
	// because the rule lived as compile-time constants in TraceCore.cpp; as of this pass the rule IS
	// on this page (Core|Pass: PassHoldSeconds / PassCooldownSeconds), so the mirrors are now
	// redundant and are candidates for deletion once ATraceBotController reads the Core|Pass values
	// directly. Until then: keep each equal to the Core|Pass value it names, and watch [BotPass]
	// telemetry (attempts vs completions) for the two drifting apart.
	// ------------------------------------------------------------------------------------------

	/**
	 * Extra seconds a bot holds the pass input beyond the rule's dwell, as insurance against the
	 * hold and the dwell being clocked on slightly different frames. Cheap: the surplus is spent
	 * shielded-down, but only for a frame or two.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Hold Margin (s)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.75"))
	float BotPassHoldMargin = 0.20f;

	/** Bot-side MIRROR of Core|Pass "Pass Hold (s)". Keep equal to PassHoldSeconds. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Hold Duration (s) [mirror of Core|Pass]", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.1", UIMax = "2.0"))
	float BotPassHoldSeconds = 0.50f;

	/** Bot-side MIRROR of Core|Pass "Pass Cooldown (s)", plus a little so bots never spam it. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|HoverPass", meta = (DisplayName = "Pass Cooldown (s) [mirror of Core|Pass]", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "6.0"))
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

	// DEAD PROPERTY REMOVED: BotPassSafeRadius (an EIGHTH dead knob, missed by the audit that found
	// the other seven).
	//
	// It claimed to be the radius inside which an enemy with line of sight counts as "covering" the
	// carrier — the risk half of the spec's central loop expressed as a number. Nothing read it.
	// ATraceBotController::CountEnemiesCoveringMe uses BotPunishRange (Bots|Punish) instead, and
	// that is the better answer anyway: it makes the carrier's idea of "someone can shoot me" and
	// the defender's idea of "I can shoot the carrier" the same number by construction. Two knobs
	// for one distance is how those two ever come to disagree.

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
	 * teammates' crossing lanes, and is also the first thing a dashing carrier escapes past.
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
	// Dash, slide and crouch fast-fall. Each has ONE job below; none of them fire randomly.
	// The dash cooldown here is a bot-side mirror of the rule value for the same reason the pass
	// durations are (see above) — the authoritative cooldown lives on the movement component.
	//
	// BOOST DELETED (spec v3 §1). BotBoostCooldownSeconds went with it. The stuck-recovery job the
	// boost was doing for a navmesh-less AI is now BotStuckJumpSeconds below — a plain jump, which
	// clears the low neon trim responsible for most wedges and costs no new ability.
	// ------------------------------------------------------------------------------------------

	/**
	 * Seconds a bot may push into geometry without moving before it tries a jump to clear it.
	 *
	 * The bots have no navmesh, so their one failure mode is wedging against the arena's waist-high
	 * furniture and sidestepping along it. Strafe-evade handles most of it; this is the escalation
	 * when it does not, and it is deliberately NOT gated on MovementTechChance — a recovery that only
	 * sometimes fires is a bot that sometimes stands in a wall for the rest of the match.
	 *
	 * MATTERS MORE ON THE NEW MAP. The corner banks (spec §7) are terraced at a ~39 uu riser against
	 * a 45 MaxStepHeight, so they are walkable — but a bot steering straight at a riser corner has
	 * exactly the stall shape this catches. Raise it if bots look twitchy; lower it if they grind.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Stuck Jump After (s)", ClampMin = "0.1", ClampMax = "10.0", UIMin = "0.5", UIMax = "4.0"))
	float BotStuckJumpSeconds = 1.30f;

	/** Bot-side MIRROR of DashCooldown, used by the shadow charge model. Keep the two equal. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Movement", meta = (DisplayName = "Dash Cooldown (s) [mirror of Movement|Dash]", ClampMin = "0.05", ClampMax = "30.0", UIMin = "0.5", UIMax = "10.0"))
	float BotDashCooldownSeconds = 3.5f;

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
	// DELETED THIS PASS: the "Bots|Legacy" scalar block.
	//
	// BotReactionTime, BotAimErrorDegrees, BotAggression, BotDecisionInterval, BotSightRange,
	// BotPreferredCombatRange and BotPassChance were all superseded by FTraceBotProfile above,
	// which is what ATraceBotController actually reads. They had survived because
	// UI/TraceMatchOptions.cpp once multiplied them by a difficulty curve; it no longer touches
	// them (see the comment at TraceMatchOptions.cpp:63), so nothing in the project read any of the
	// seven. AdvancedDisplay hid them but did not make them harmless — the last two in that list
	// were on the standing "dead knobs" defect, because "Preferred Combat Range" and "Pass Chance"
	// are exactly the names a designer reaches for when bots hold the wrong range or never pass.
	//
	// Tune FTraceBotProfile (Bots|Difficulty > Easy/Normal/Hard Profile) instead. Every knob those
	// seven pretended to be is in there, and those ones are live.
	// ------------------------------------------------------------------------------------------

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
	// DELETED THIS PASS: the "Controls" category and its single LookSensitivity property.
	//
	// The human look scale is a persisted per-player setting: UTraceUserSettings::LookSensitivity
	// (default 1.50), plus a separate vertical multiplier and an invert-Y toggle, all rebuilt into
	// the Look mapping's modifier scalars by ATracePlayerController::ApplyControlSettings(). The
	// property here had not moved a crosshair since its last caller (the -TraceWalkHuman harness in
	// AI/TraceBotController.cpp) was repointed at UTraceUserSettings::GetLookScaleX().
	//
	// Look sensitivity belongs in the in-game options menu, not in Project Settings, and having it
	// in both is how somebody ends up tuning the copy that does nothing.
	// ==========================================================================================
};
