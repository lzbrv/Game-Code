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
 * Which of the two scoring rulesets the match plays. Spec v4 §7, verbatim: "Create a toggle which
 * can switch between the current game state(a) ... to a game state (b) where the core can be thrown
 * and intercepted, so that we can test which feels better."
 *
 * This is an A/B TESTING TOGGLE, not a game type ladder — the whole point is that one build can play
 * both so the two can be compared back to back. It lives here (and is mirrored on the main menu,
 * which writes "?mode=a|b" onto the travel URL) rather than being a second game mode class, because
 * a second class would double every rule that the two modes share.
 *
 * EndzoneStatusCore is mode A and is the DEFAULT: it is the shipped game and must not regress.
 *
 *   EndzoneStatusCore     Endzones spanning the full field width. The Core is a STATUS, not an
 *                         object: it cannot exist on the ground, LMB starts the 0.5 s hover-pass,
 *                         and possession moves on kill / trace break / completed pass.
 *   ThrownCoreAndGoals    Discrete goal volumes (GoalWidthFieldFraction of the field width,
 *                         GoalHeightUU tall) instead of endzones. The Core is a physical entity:
 *                         LMB THROWS it at CoreThrowSpeed, and the first player of either team to
 *                         come within CorePickupRadius takes it. A Core left on the ground for
 *                         CoreLooseResetSeconds returns to play so it cannot be lost forever.
 *
 * Grace rules are shared: possession crossing TEAMS costs CoreTurnoverGraceSeconds of trace grace in
 * both modes, possession moving WITHIN a team costs none in both modes.
 */
UENUM()
enum class ETraceScoringMode : uint8
{
	/** Mode A — endzones, Core is a status, LMB is the hover-pass. The shipped game. */
	EndzoneStatusCore = 0 UMETA(DisplayName = "A - Endzones, Core is a status"),

	/** Mode B — goals, Core is a physical thrown/intercepted object, LMB throws. */
	ThrownCoreAndGoals = 1 UMETA(DisplayName = "B - Goals, Core is thrown")
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
	 * Sane range 5000 to 8000 on the 33600 x 9600 field. Must stay above MaxEngagementRange or a bot
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

	// DEAD PROPERTY REMOVED: ScoreToWin, with ATraceGameMode::bEndMatchAtScoreToWin.
	//
	// Spec v4 §6, verbatim: "Remove score to win 5 — Keep the match timer as the win condition, but
	// add a mercy rule". The clock decides the match; MercyRuleLead below is the only early exit.
	//
	// Deleted rather than left defaulted-off, and the game mode's switch went with it. A property
	// that can reintroduce a deleted win condition is exactly the thing an old DefaultGame.ini
	// switches back on, and the symptom — matches ending at 5-4 with half the format unplayed —
	// looks like a bug in the clock rather than a stale config key.

	/** Target roster size per team; used to balance teams on login. 5 is the designed game. */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Players Per Team", ClampMin = "1", ClampMax = "16", UIMin = "1", UIMax = "8"))
	int32 PlayersPerTeam = 5;

	/** Connected players required before the match leaves WaitingForPlayers. */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Min Players To Start", ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "10"))
	int32 MinPlayersToStart = 2;

	/**
	 * Seconds between death and respawn. 3 -> 2 THIS PASS (spec v4 §5).
	 *
	 * NOT AUTHORITATIVE. ATraceGameMode::RespawnDelay is what actually schedules the respawn; this
	 * is the client-side fallback the death panel counts down from during the frame or two before
	 * ATracePlayerState::RespawnEndServerTime (the real, replicated deadline) has arrived. Keep the
	 * two equal or the panel briefly disagrees with the game.
	 *
	 * BOTH COPIES MOVED. The enforcing one is now pinned from DefaultGame.ini under
	 * [/Script/Trace.TraceGameMode] (RespawnDelay=2.0) as well as in the game mode header, because
	 * this project has twice shipped a "changed" value that the ini quietly overrode.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Respawn Delay (s, HUD fallback)", ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.0", UIMax = "10.0"))
	float RespawnDelay = 2.f;

	/** Countdown after MinPlayersToStart is met, before the match goes InProgress. */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Warmup Duration (s)", ClampMin = "0.0", ClampMax = "120.0", UIMin = "0.0", UIMax = "30.0"))
	float WarmupDuration = 5.f;

	/**
	 * MERCY RULE (spec v4 §6, new). The moment one team's lead reaches this many points the match
	 * ends immediately and that team wins. ZERO DISABLES IT.
	 *
	 * Verbatim: "Keep the match timer as the win condition, but add a mercy rule, where the game will
	 * end if one team leads by 8 points (granting an immediate win to the team leading)."
	 *
	 * It is a LEAD, not a score: 9-1 ends the match, 12-6 does not. [ASSUMPTION] it is checked across
	 * the whole match including mid-first-half, and it ends the MATCH outright rather than ending the
	 * half — so a blowout does not get dragged through a side switch and a second ten minutes.
	 *
	 * THIS IS WHAT REPLACES ScoreToWin. That property and the game mode's bEndMatchAtScoreToWin
	 * switch were the old "first to N wins outright" condition, and spec v4 §6 removes it; both are
	 * deleted rather than left inert. The clock and this rule are now the only two ways a match can
	 * end, and the post-match screen must be able to say which one did it.
	 *
	 * Sane range 6 to 12 against a two-half format. Below ~5 a single good possession run ends the
	 * match; at 0 only the clock ever does.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Mercy Rule Lead (points, 0 = off)", ClampMin = "0", ClampMax = "50", UIMin = "0", UIMax = "15"))
	int32 MercyRuleLead = 8;

	// ==========================================================================================
	// SCORING MODE  —  the A/B toggle (spec v4 §7)
	//
	// Two complete rulesets in one binary so they can be played back to back and compared. See
	// ETraceScoringMode above for what each mode actually changes; the three knobs here are the
	// mode selector and the geometry mode B needs and mode A does not.
	//
	// LATCHING: the mode must be read ONCE at match start and held for the match. It changes what
	// the Core IS (a status versus an actor) and what the scoring volumes ARE, so flipping it with a
	// carrier mid-run is not a live retune, it is a mid-air rules change. The menu writes
	// "?mode=a|b" onto the travel URL; this property is what a direct launch into the arena uses.
	// ==========================================================================================

	/**
	 * Which ruleset the next match plays. A is the shipped game and the default.
	 *
	 * NOT a live knob in the way the rest of this page is: read it at match start, not at the point
	 * of use. See the latching note above.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match|Scoring Mode", meta = (DisplayName = "Scoring Mode (A/B test)"))
	ETraceScoringMode ScoringMode = ETraceScoringMode::EndzoneStatusCore;

	/**
	 * MODE B ONLY. Width of each goal as a fraction of the FULL field width.
	 *
	 * Verbatim: "The goal should not be the entire width of the map, like the endzone."
	 * [ASSUMPTION] one third. At a 9600 uu wide field that is a 3200 uu goal mouth centred on the
	 * end line, which is wide enough to throw at from an angle and narrow enough that a defender can
	 * meaningfully cover it — the two properties that make it a goal rather than a repainted endzone.
	 *
	 * Ignored entirely in mode A, where the endzone spans the full width by design. Keep it under
	 * ~0.6 or the distinction the spec is asking for stops existing.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match|Scoring Mode", meta = (DisplayName = "Goal Width (fraction of field width) [mode B]", ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.1", UIMax = "0.6"))
	float GoalWidthFieldFraction = 0.3333f;

	/**
	 * MODE B ONLY. Height of the goal volume, uu, measured from the floor.
	 *
	 * FINITE ON PURPOSE — "height finite so it reads as a goal rather than a wall". A goal you can
	 * throw over is a goal you can defend by standing in front of. 700 is about four character
	 * heights (the capsule is 176 uu), so a lobbed Core clears it and a flat throw does not.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match|Scoring Mode", meta = (DisplayName = "Goal Height (uu) [mode B]", ClampMin = "50.0", ClampMax = "5000.0", UIMin = "200.0", UIMax = "2000.0"))
	float GoalHeightUU = 700.f;

	// DEAD PROPERTY REMOVED: MatchDuration.
	//
	// Spec §1 replaced the single timed match with TWO HALVES. The enforced length of each half is
	// ATraceGameMode::HalfDuration (config=Game on the game mode, [/Script/Trace.TraceGameMode]).
	// Nothing read MatchDuration; a slider that silently does nothing is worse than no slider, so it
	// is gone rather than folded away under AdvancedDisplay.
	//
	// The score cap that used to sit beside it (ScoreToWin here, bEndMatchAtScoreToWin on the game
	// mode) has since gone the same way — spec v4 §6 removes "first to N" as a win condition. What
	// ends a match now is the clock, or MercyRuleLead above.

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
	 * field is 33600 x 9600 (spec v4 §3 lengthened it from 24000 for the 3.5:1 proportion), so the
	 * diagonal is 34944 uu; 36000 covers it with 1056 uu of margin. The 28000 that preceded this
	 * covered the OLD 24000-long field and fell 6944 uu short of the new one — a shot down the spine
	 * expired in mid-air short of a target the player could plainly see. If ATraceArenaBuilder::
	 * FieldLength or FieldWidth changes again, recompute sqrt(L^2 + W^2) and raise this with it.
	 *
	 * Raising it does NOT make the bots deadlier: they are limited by FTraceBotProfile::
	 * MaxEngagementRange (4200 Easy / 4800 Normal / 6000 Hard), far below either value. This only
	 * restores the human's ability to shoot what they can see.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Hitscan Range (uu)", ClampMin = "100.0", ClampMax = "200000.0", UIMin = "5000.0", UIMax = "50000.0"))
	float HitscanRange = 36000.f;

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
	 *
	 * 820 -> 800 THIS PASS (spec v4 §5). Note there is a THIRD copy of this number:
	 * UTraceCharacterMovementComponent's constructor seeds MaxWalkSpeed so a pawn is sane for the
	 * frames before BeginPlay overwrites it from here. It is not authoritative, but it should be
	 * moved with this one so a breakpoint in the constructor does not read a stale figure.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Walk", meta = (DisplayName = "Walk Speed (uu/s)", ClampMin = "50.0", ClampMax = "5000.0", UIMin = "300.0", UIMax = "1500.0"))
	float WalkSpeed = 800.f;

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
	// SPEC v4 §1 CLOSED THE [CONFLICT] SPEC v3 LEFT OPEN. The design owner ruled the flat momentum
	// boost OUT: "entry speed determines slide velocity", Source-style, and nothing tops it up.
	//   * SlideImpulse (a flat uu/s additive on entry) is DELETED. Not defaulted to zero — removed.
	//   * SlideExitMinSpeedFraction (a floor on the exit speed, measured granting a slow slide a 73%
	//     speed GAIN) is DELETED. It was the exit-side spelling of the same flat boost.
	//   * SlideEntrySpeedMultiplier stays at 1.0 and is now the ONLY knob that can scale entry speed.
	//   * The payoff for sliding is the SLIDE-JUMP block at the end of this section, which preserves
	//     momentum through the jump instead of manufacturing it.
	//   * The cooldown is 0.8 s and is measured from the slide's END rather than its START, which is
	//     why it is a differently named property (SlideCooldownSeconds).
	//
	// If a merge ever reintroduces SlideImpulse or SlideExitMinSpeedFraction, delete them again.

	/**
	 * Fraction of WalkSpeed you must already be moving at before crouch will start a slide.
	 * Stops "tap crouch from a standstill" being free speed. Sane range 0.4 to 0.7.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Entry Speed (fraction of Walk Speed)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.2", UIMax = "1.0"))
	float SlideEntrySpeedFraction = 0.55f;

	/**
	 * Entry speed multiplier applied to the ACTUAL entry speed. Slide velocity = entry speed x this.
	 *
	 * Not max(entry speed, WalkSpeed) — that was the old implementation and the old wording, and the
	 * floor is exactly the flat boost spec v4 §1 rules out: it made a slide entered at walking pace
	 * come out 35% faster than the walk. It multiplies what you actually arrived with, nothing else.
	 *
	 * SHIPPED AT 1.0, WHICH IS NOW THE DECISION AND NOT A DEFAULT. Spec v4 §1: "the flat momentum
	 * boost should be ruled out, going with the source-style movement system instead". At 1.0 the
	 * slide is exactly the speed you brought into it, friction bleeds it, and nothing tops it up.
	 * Above 1.0 this is the ONE remaining way to make a slide grant speed, and it is at least a
	 * proportional grant rather than a flat one — but the design owner has ruled that out, so raising
	 * it is a deliberate reversal of their call, not a tuning nudge.
	 *
	 * Because it multiplies your CURRENT speed, a slide out of a fast approach is faster than a
	 * slide out of a walk — that is the momentum preservation, and it holds at 1.0.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Entry Speed Multiplier", ClampMin = "0.5", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float SlideEntrySpeedMultiplier = 1.0f;

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

	// SlideExitMinSpeedFraction ("Exit Floor") WAS HERE AND IS DELETED (spec v4 §1, verbatim: "you
	// can remove the slideexitminspeedfraction value"). At 1.0 it handed every slide back at exactly
	// WalkSpeed no matter how slowly it was going — measured at a 73% speed GAIN for a slide that had
	// decayed. That is the flat momentum boost the design owner ruled out, wearing an exit-side hat.
	// A slide now ends at whatever the friction left it with, and the SLIDE-JUMP is the payoff.

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

	// --- Slide-jump (spec v4 §1, new) ---------------------------------------------------------
	//
	// Verbatim: "Sliding, however, doesn't feel like it does much; is it possible to add a slide-jump
	// mechanic, also attempting to feel like apex legends."
	//
	// This is the payoff move, and after spec v4 §1 deleted the flat slide boost it is the ONLY thing
	// that makes sliding worth doing: the slide itself now returns exactly the momentum you brought
	// to it, so the reason to slide has to be what you can do at the end of one.
	//
	// The Apex slide-hop is three properties, and each is a knob below:
	//   1. jumping out of a slide keeps your horizontal speed instead of clamping it to walk pace
	//      (SlideJumpHorizontalRetention),
	//   2. it launches you properly rather than at a standing jump's height (SlideJumpZMultiplier),
	//   3. hitting it near the END of the slide is worth more than mashing it at the start, which is
	//      what makes it a skill rather than a second jump button (SlideJumpWindowSeconds /
	//      SlideJumpWindowSpeedBonus).
	//
	// All four are read at the point of use by the movement component, so the whole feel of the move
	// retunes with PIE running. Note the retention is applied to the SLIDE's live speed, which is
	// already the momentum you carried in — this is not a place a flat boost can hide.

	/**
	 * Master switch for the slide-jump. OFF makes a jump out of a slide behave like any other jump.
	 *
	 * Exists so the move can be A/B'd against its absence from one binary, which matters here: the
	 * slide has just lost its flat boost, and whether sliding "does something" now is exactly the
	 * question this mechanic is answering.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Slide Jump Enabled"))
	bool bSlideJumpEnabled = true;

	/**
	 * Fraction of the slide's live horizontal speed carried into the jump. 1.0 = all of it.
	 *
	 * THE HEADLINE KNOB. At 1.0 a slide-jump is a pure momentum-preserving launch and the slide
	 * becomes a way to convert a fast approach into distance; below ~0.85 the move stops being worth
	 * the setup and players will simply jump. Above 1.0 it is a boost — which is exactly what spec v4
	 * §1 ruled out for the slide itself, so raise it only deliberately and only as an experiment.
	 *
	 * Sane range 0.9 to 1.1.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Horizontal Retention (fraction of slide speed)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.5", UIMax = "1.3"))
	float SlideJumpHorizontalRetention = 1.0f;

	/**
	 * Multiplier on the pawn's normal jump velocity when the jump comes out of a slide.
	 *
	 * Kept at 1.0 by default and deliberately so: in Apex the slide-hop's value is DISTANCE, not
	 * height, and a taller jump mostly buys hang time during which you cannot dash, cannot turn hard
	 * and are an easy target. Lower it toward 0.85 for a flatter, faster-feeling launch; raise it if
	 * playtesting wants the move to clear the arena's cover.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Vertical Multiplier (x normal jump)", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.5", UIMax = "1.6"))
	float SlideJumpZMultiplier = 1.0f;

	/**
	 * Seconds before a slide would END during which the jump counts as well timed. 0 = no window,
	 * every slide-jump is worth the same.
	 *
	 * This is the "reward good timing" half of the request. A window measured from the slide's END
	 * rather than its start is what makes it readable: the player can see and feel the slide running
	 * out, so the input has something to be timed AGAINST. Timing it against the start would just
	 * mean "press both keys at once", which is not a skill.
	 *
	 * Sane range 0.15 to 0.30. Below ~0.1 it is unhittable at real latency; above ~0.5 against a 1.8s
	 * slide it covers so much of the slide that it stops being a window at all.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Timing Window (s before slide end)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.6"))
	float SlideJumpWindowSeconds = 0.20f;

	/**
	 * Extra horizontal speed multiplier for a slide-jump taken inside the timing window above.
	 *
	 * Small on purpose. 1.10 on a ~1100 uu/s slide is about 110 uu/s — plainly felt over a long jump,
	 * but not enough that a player who never learns the timing is playing a different game. This is
	 * the one place in the movement kit where a flat-ish gain is intended, and it is gated behind a
	 * read rather than being free, which is the distinction spec v4 §1 is actually drawing.
	 *
	 * 1.0 turns the window into a no-op without disabling the move. Do not go far past ~1.2 or
	 * chained slide-jumps become the fastest way to cross the arena.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Well-Timed Speed Bonus (x)", ClampMin = "1.0", ClampMax = "2.0", UIMin = "1.0", UIMax = "1.3"))
	float SlideJumpWindowSpeedBonus = 1.10f;

	// --- Slide pose (spec v4 §1) — PROCEDURAL. THERE IS NO STOCK MANNEQUIN SLIDE ANIMATION. -----
	//
	// The notes ask for "unreal's default slide animation for the mannequins". IT DOES NOT EXIST, and
	// this block is not it. The Mannequin set Scripts/import-mannequin.sh brings in ships Death, Jump,
	// Pistol, Rifle and Unarmed locomotion (BS_Idle_Walk_Run, MM_Idle) — no slide and no crouch,
	// anywhere in Templates/TemplateResources or Engine/Content. So the pose is APPROXIMATED from what
	// the skeleton already has: the whole mesh reclines and drops over its own feet, and the
	// locomotion blend space is slowed almost to a stop so the legs stop sprinting. A real slide
	// animation would have to be authored or bought.
	//
	// Visual only. The capsule never resizes — it is the single source of truth for hitscan, for the
	// pose history the server rewinds and for the trail trip test — and nothing here feeds the
	// simulation, so none of it can desync prediction or move a hit zone.

	/**
	 * Degrees the body reclines into a slide. Positive = leaning BACK, feet leading, which is the
	 * Apex/Titanfall read and the thing that makes a slide legible at a distance.
	 *
	 * Sane range 20 to 40. Past ~50 the Mannequin's heels lift off the deck, because the mesh pivots
	 * about its own origin (the bottom of the capsule) rather than about a hip joint.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Pose", meta = (DisplayName = "Recline (deg)", ClampMin = "0.0", ClampMax = "70.0", UIMin = "0.0", UIMax = "45.0"))
	float SlidePoseLeanDegrees = 30.f;

	/**
	 * How far, in uu, the whole mesh drops toward the deck while sliding.
	 *
	 * The capsule deliberately does NOT shrink, so this is the only thing that makes a sliding player
	 * look low from the outside. Modest on purpose: the feet sit at the mesh origin, so a large drop
	 * buries them in the floor. Sane range 15 to 35.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Pose", meta = (DisplayName = "Body Drop (uu)", ClampMin = "0.0", ClampMax = "80.0", UIMin = "0.0", UIMax = "45.0"))
	float SlidePoseDropUU = 20.f;

	/**
	 * Degrees of roll — one shoulder led into the slide. Breaks the symmetry so the pose reads as a
	 * body committed to a direction rather than a mannequin tipped back on a hinge. 0 is a clean
	 * recline; sane range 6 to 15.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Pose", meta = (DisplayName = "Roll (deg)", ClampMin = "-40.0", ClampMax = "40.0", UIMin = "-20.0", UIMax = "20.0"))
	float SlidePoseRollDegrees = 9.f;

	/**
	 * Animation play rate while sliding, as a fraction of normal.
	 *
	 * THE MOST IMPORTANT ONE. Without it the pawn keeps playing BS_Idle_Walk_Run at slide speed, so a
	 * sliding player's legs churn at a full sprint cadence while the torso lies back — which is the
	 * one thing that unambiguously reads as a bug rather than a move. Near zero freezes the legs
	 * mid-stride, close enough to "extended into a slide" to sell it. 1.0 disables the effect.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Pose", meta = (DisplayName = "Anim Rate While Sliding", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float SlidePoseAnimRateScale = 0.12f;

	/**
	 * How fast the pose blends in and out (FInterpTo speed). Fast enough to land with the slide, slow
	 * enough to be a move rather than a cut. Sane range 7 to 14.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Pose", meta = (DisplayName = "Blend Speed", ClampMin = "1.0", ClampMax = "30.0", UIMin = "4.0", UIMax = "16.0"))
	float SlidePoseBlendSpeed = 9.f;

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
	 * 1.0 -> 0.4 (spec v3 §1) -> 0.5 THIS PASS (spec v4 §5, which asks for exactly 0.4 x 1.25). The
	 * grace exists so a turnover does not instantly wrap the new carrier in lethal trace laid on top
	 * of the scrum they just won it in; at a full second it also meant the counter-attack got a free
	 * run with no trace behind it at all. 0.5 s is about 430uu of travel at carrier speed — enough to
	 * clear the pile, short enough that the trace is a threat again before anyone has crossed open
	 * ground.
	 *
	 * Applies only when the Core changes SIDE. A pass between teammates has no grace, by design —
	 * and in mode B (ScoringMode = ThrownCoreAndGoals) the same rule holds for a thrown Core:
	 * intercepted by an enemy = this grace, recovered by a teammate = none. Spec v4 §7.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core", meta = (DisplayName = "Turnover Trace Grace (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.0", UIMax = "2.0"))
	float CoreTurnoverGraceSeconds = 0.5f;

	// ==========================================================================================
	// CORE — MODE B ONLY  (the thrown, interceptable Core; spec v4 §7)
	//
	// INERT IN MODE A. Nothing below is read unless ScoringMode is ThrownCoreAndGoals, because in
	// mode A the Core is a status and cannot be thrown, dropped or stood on. They are grouped in
	// their own category and named "[mode B]" so that is obvious from the panel — a knob that does
	// nothing in the mode you are playing is the same trap as a knob that does nothing at all,
	// unless the panel says so.
	//
	// This is NOT the physical Core the project deleted in spec v2 being reverted. It is a second
	// possession model behind the mode enum, sharing the trace, parry and grace logic with mode A.
	// ==========================================================================================

	/**
	 * Launch speed, uu/s, of a Core thrown with LMB in mode B.
	 *
	 * Verbatim: "The carrier should be able to throw the core forward by left clicking."
	 * Fast enough to be a pass across a lane, slow enough that the "first player to contact the core
	 * picks it up" rule has something to work with — a Core that crosses the field in half a second
	 * cannot be intercepted by anyone, which deletes the whole point of mode B. 3000 covers ~3000uu
	 * in the first second before drag and gravity, roughly a dash's worth of reach per second.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Speed (uu/s) [mode B]", ClampMin = "100.0", ClampMax = "20000.0", UIMin = "500.0", UIMax = "8000.0"))
	float CoreThrowSpeed = 3000.f;

	/**
	 * Fraction of the throw speed added as upward velocity, so a throw arcs instead of running flat
	 * along the floor and catching on the first piece of cover.
	 *
	 * Small: at 0.12 a 3000 uu/s throw leaves with 360 uu/s of lift, which is a shallow arc a player
	 * can lead. Raising it makes throws lobbed and easy to read; zero makes them hitscan-flat and
	 * makes the goal's finite height (GoalHeightUU) meaningless.
	 *
	 * NAME IS LOAD-BEARING: ATraceCore resolves this by reflection under exactly this spelling
	 * (TraceModeBTuning::ThrowUpBias). It was briefly declared as "CoreThrowUpwardBias", which the
	 * lookup could not find, so the panel slider moved nothing and the CVar default was played
	 * instead. Do not rename either half alone.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Upward Bias (fraction of speed) [mode B]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5"))
	float CoreThrowUpBias = 0.12f;

	/**
	 * World gravity multiplier applied to a Core in flight and rolling loose.
	 *
	 * Below 1 because the field is 33600 uu long: at full gravity a throw that a player can aim
	 * ploughs into the floor inside a couple of thousand uu, which turns every throw into a short
	 * dribble and makes interception trivial. 0.55 lets a throw cross useful ground while still
	 * arcing enough to be read and cut off.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Gravity Scale [mode B]", ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "1.5"))
	float CoreThrowGravityScale = 0.55f;

	/**
	 * Seconds the THROWER alone may not re-take their own throw. Everybody else may take it on the
	 * first frame — "the first player to contact the core should pick it up" is unconditional for
	 * all nine other players.
	 *
	 * Without this a throw is a no-op: the Core leaves from inside the thrower's own pickup radius,
	 * so the thrower's proximity poll re-takes it on the very next tick and the throw looks like it
	 * never happened.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Thrower Re-Pickup Lockout (s) [mode B]", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.0", UIMax = "1.5"))
	float CoreThrowerPickupLockoutSeconds = 0.35f;

	/**
	 * Seconds after TAKING the Core before it may be thrown again. Stops a pickup and a throw
	 * landing on the same frame, which reads as the Core bouncing off a player rather than being
	 * caught, and lets an interception be seen before it is undone.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Cooldown After Pickup (s) [mode B]", ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.0", UIMax = "2.0"))
	float CoreThrowCooldownSeconds = 0.35f;

	/**
	 * Restitution of a loose Core against world geometry: 0 = dead stop on contact, 1 = perfect
	 * bounce. Low by default so a missed throw settles somewhere a player can contest rather than
	 * pinballing off the banks into a corner.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Loose Core Bounce [mode B]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float CoreThrowBounce = 0.35f;

	/**
	 * Radius, uu, inside which a player takes possession of a loose Core. "The first player to
	 * contact the core should pick it up" — enemy or teammate, whoever gets there first.
	 *
	 * Comfortably wider than the character capsule (34 uu) because this is a contact test against a
	 * moving object sampled once a frame: at 3000 uu/s a Core travels 50 uu between frames at 60Hz,
	 * so a radius near the capsule's would let a thrown Core tunnel straight through the player who
	 * was standing in its path. That failure reads as "interception is broken", not as "I was too
	 * slow". Sane range 90 to 200.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Pickup Radius (uu) [mode B]", ClampMin = "10.0", ClampMax = "1000.0", UIMin = "50.0", UIMax = "400.0"))
	float CorePickupRadius = 120.f;

	/**
	 * Seconds a Core may lie untouched on the ground before it resets to a neutral position.
	 * 0 disables the reset and lets it lie there forever.
	 *
	 * [ASSUMPTION] from spec v4 §7: "a thrown Core that lands untouched stays live on the ground and
	 * is picked up by first contact; add a reset timer so it cannot be lost forever." The failure it
	 * exists to prevent is a Core thrown out of bounds, into a gap in the arena geometry, or simply
	 * ignored by ten players who all went to fight — any of which stalls the match until the half
	 * ends. Long enough that a genuine scramble for a loose ball is never cut short; short enough
	 * that a lost Core costs one possession rather than a half.
	 *
	 * 0 GENUINELY MEANS NEVER, and that took a fix: ATraceCore used to clamp this to a floor of 1
	 * second, so entering 0 to disable the reset produced the FASTEST possible reset instead of no
	 * reset at all — the exact opposite of what this tooltip promises. The floor is gone and the
	 * use site now treats <= 0 as "leave it lying there".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Loose Core Reset (s, 0 = never) [mode B]", ClampMin = "0.0", ClampMax = "120.0", UIMin = "0.0", UIMax = "30.0"))
	float CoreLooseResetSeconds = 12.f;

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
	// TRACER  —  the railgun shot effect  (spec v4 §4)
	//
	// Verbatim: "Remove the sphere from the end of the bullet tracer hitscan animation, so it's just
	// a bullet trace, which makes it easier to see where your shots are going. Reduce the radius of
	// the cross section of the bullet tracers in order to make them thinner."
	//
	// THE IMPACT SPHERE IS GONE — deleted, not disabled. ATraceTracer no longer has an ImpactFlash
	// component at all, so there is no knob here to bring it back. It was a 10 -> 52 uu expanding
	// emissive ball sitting exactly on the point the player is trying to look at, which is precisely
	// the complaint: it announced the hit at the cost of hiding where the shot landed.
	//
	// WHY THE WIDTH IS PROPORTIONAL TO THE SHOT'S LENGTH, AND WHY THAT SURVIVED THE THINNING.
	// A fixed-radius cylinder is the wrong model for a beam that has to read at every range in a
	// 24000+ uu arena: any radius that is sane across a corridor is a sub-pixel thread across the
	// field, and any radius that is visible across the field is a rod at knife range. Radius
	// proportional to the shot's own length holds the beam at roughly constant ANGULAR width from the
	// shooter's eye, which is what a real camera-facing beam material would do. So "thinner" is
	// implemented by halving the proportion and both clamps, NOT by replacing the model with a
	// constant — that would have made it thinner in a corridor and invisible across the map.
	//
	// Read fresh by every shot (ATraceTracer::InitTracer calls Get()), so all six retune with PIE
	// running and Trace.TestBeam will show the change immediately.
	// ==========================================================================================

	/**
	 * Core beam RADIUS per uu of shot length. Halved this pass: 0.0031 -> 0.00155.
	 *
	 * A 4000 uu shot is now 6.2 uu in radius where it was 12.4. Clamped at both ends by the two
	 * values below, and this is the middle of the three — most in-arena shots land between the
	 * clamps, so this is the number to move if the beam is broadly too fat or too thin.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tracer", meta = (DisplayName = "Beam Radius per uu of Shot Length", ClampMin = "0.0", ClampMax = "0.05", UIMin = "0.0002", UIMax = "0.01"))
	float TracerRadiusPerLength = 0.00155f;

	/**
	 * Floor on the core beam radius, uu. Halved this pass: 2.5 -> 1.75.
	 *
	 * NOT halved quite as far as the rest, and deliberately. This governs SHORT shots, which are the
	 * ones already fired at point-blank range in a scrum, and it is the value that decides whether a
	 * beam is a line or a sub-pixel shimmer. Below ~1.2 uu a short shot stops resolving on a 1280-wide
	 * back buffer at all, and "thinner" turns into "gone" — which is the failure this task is
	 * explicitly told to avoid.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tracer", meta = (DisplayName = "Beam Radius Min (uu)", ClampMin = "0.1", ClampMax = "100.0", UIMin = "0.5", UIMax = "10.0"))
	float TracerRadiusMinUU = 1.75f;

	/**
	 * Ceiling on the core beam radius, uu. Halved this pass: 13.0 -> 6.5.
	 *
	 * Governs LONG shots — anything past ~4200 uu is on this clamp — so this is what a cross-map shot
	 * actually looks like, and it is the single biggest contributor to "the tracers are too thick".
	 * The angular size it produces is what matters: 6.5 uu at 8000 uu away is about 0.09 degrees,
	 * roughly 2.5 px at 1280 wide and a 95 degree field of view, plus the sheath's halo around it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tracer", meta = (DisplayName = "Beam Radius Max (uu)", ClampMin = "0.5", ClampMax = "200.0", UIMin = "2.0", UIMax = "30.0"))
	float TracerRadiusMaxUU = 6.5f;

	/**
	 * The soft additive team-coloured halo is this many times the core's radius.
	 *
	 * DELIBERATELY LEFT AT ITS OLD VALUE WHILE THE CORE WAS HALVED, which is what keeps a thin beam
	 * legible at distance. The halo is additive, writes no depth and is heavily bloomed, so it costs
	 * the player nothing in occlusion — it is what your eye finds across the arena, and the hard thin
	 * core inside it is what tells you exactly where the shot went. Thinning both together is how a
	 * "thinner tracer" change ends up as an invisible one.
	 *
	 * 1.0 removes the halo entirely and leaves the bare core.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tracer", meta = (DisplayName = "Halo Radius (multiple of beam radius)", ClampMin = "1.0", ClampMax = "12.0", UIMin = "1.0", UIMax = "6.0"))
	float TracerSheathRadiusRatio = 3.2f;

	/**
	 * Draw the small bright sphere at the MUZZLE end of the beam. ON by default.
	 *
	 * This is not the sphere spec v4 §4 asks to remove — that one was at the impact end and is
	 * deleted outright. This one sits on the viewmodel, 120 uu from the shooter's own eye, and it is
	 * a large part of why a first-person shot is visible at all: the beam runs almost straight away
	 * from the camera and projects to nearly a point, so the flash at the near end is what says "you
	 * fired". It is a knob rather than an assumption because "the sphere" is ambiguous in the note,
	 * and this is the one that can be answered without a rebuild.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tracer", meta = (DisplayName = "Muzzle Flash"))
	bool bTracerMuzzleFlash = true;

	/**
	 * Muzzle flash RADIUS at the instant of firing, uu. Halved with the beam: 5.0 -> 2.5.
	 *
	 * Kept small on purpose, and the reason is not subtlety. Unlit emissive does not attenuate with
	 * distance, so a flash this close to the eye arrives at full intensity: measured larger and
	 * hotter first, it bloomed into a blob that ate the middle of the screen on every shot — a
	 * flashbang, not a muzzle flash. Do not push it far past ~6.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Tracer", meta = (DisplayName = "Muzzle Flash Radius (uu)", ClampMin = "0.1", ClampMax = "60.0", UIMin = "0.5", UIMax = "15.0"))
	float TracerMuzzleRadiusUU = 2.5f;

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

	// ------------------------------------------------------------------------------------------
	// Trace GHOSTS (spec v4 §2, new look)
	//
	// Verbatim: "Change the look of the trace to match the new mannequin models, rather than the old
	// cylinder models." The trace goes back to its original description — "a blur created where your
	// character model has passed through" — so the visual is a run of character-shaped after-images
	// rather than a tube.
	//
	// THE ONE RULE THESE KNOBS MUST NOT BREAK: the kill volume is what the player sees. Ghosts are
	// SPARSER than trail points for performance (a skeletal snapshot per point does not scale to ten
	// carriers), and the ribbon between them is what keeps the trip geometry continuous. So
	// TraceGhostSpacingPoints is a VISUAL density dial only — it must never be allowed to thin the
	// collision, or the trip test and the picture disagree and the whole game feels broken.
	// ------------------------------------------------------------------------------------------

	/**
	 * uu ALONG THE PATH between consecutive posed after-images. Mirrors the
	 * Trace.Trail.GhostSpacing CVar; keep the two equal.
	 *
	 * A DISTANCE, not a count of trail points, and that is the right unit: point spacing changes with
	 * TrailPointSpacing and the ghosts want to be a fixed distance apart however finely the path
	 * happens to be sampled.
	 *
	 * The tension it resolves: a ghost at every trail point (60 uu) is the most beautiful version and
	 * costs ~27 skinned draws per trace; a ghost every 400 uu is nearly free and reads as a row of
	 * statues with holes between them. 220 uu is a little over one body-depth of gap, so consecutive
	 * mannequins nearly touch and the eye joins them into one blur — and the smear ribbon covers the
	 * gap regardless, which is what keeps the picture continuous where the collision is.
	 *
	 * COSMETIC ONLY. Nothing here may thin the lethal volume; the trip test runs on the trail points,
	 * not on the ghosts. Sane range 120 to 300.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail|Ghosts", meta = (DisplayName = "Ghost Spacing (uu along path)", ClampMin = "20.0", ClampMax = "2000.0", UIMin = "60.0", UIMax = "500.0"))
	float TraceGhostSpacingUU = 220.f;

	/**
	 * Cap on pooled posed-Mannequin after-images PER TRACE. 0 disables ghosts and leaves the trace as
	 * the continuous smear alone. Mirrors the Trace.Trail.GhostMaxCount CVar; keep the two equal.
	 *
	 * This is the number that decides whether spec v4 §2 is affordable, so the arithmetic is worth
	 * stating: a trace is TrailLifetime x speed long, capped by MaxTrailPoints x TrailPointSpacing —
	 * 2.0 s x 800 uu/s = 1600 uu at a walk, ~3200 uu through a sustained dash — so at 220 uu spacing
	 * that is ~8 ghosts walking and ~15 dashing. 20 covers the dashing case with headroom.
	 *
	 * Past the cap the OLDEST ghosts are released, never the newest: the newest are the ones an
	 * approaching enemy is judging their dash against. The smear still covers the tail, so the cap
	 * degrades the look and never the continuity — which is the rule the whole feature turns on.
	 *
	 * Only the Core holder emits, so the worst realistic case (a residual trace during a turnover
	 * while the new holder lays a fresh one) is ~2x this in the world, not 10x.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail|Ghosts", meta = (DisplayName = "Max Ghosts Per Trace (0 = smear only)", ClampMin = "0", ClampMax = "64", UIMin = "0", UIMax = "40"))
	int32 MaxTraceGhosts = 20;

	/**
	 * Emissive strength of a posed after-image on M_TraceNeon. Mirrors Trace.Trail.GhostGlow.
	 *
	 * Above roughly 3.5 the team colour clips toward white and you can no longer tell WHOSE trace you
	 * are looking at, which matters more than prettiness — the trace is only lethal to the other team,
	 * so misreading its colour is misreading whether you may run through it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail|Ghosts", meta = (DisplayName = "Ghost Glow", ClampMin = "0.0", ClampMax = "8.0", UIMin = "0.5", UIMax = "4.0"))
	float TraceGhostGlow = 2.6f;

	/**
	 * Brightness of the continuous smear as a FRACTION of the ghost glow. Mirrors
	 * Trace.Trail.SmearGlowScale.
	 *
	 * The smear is the part of the drawing that is continuous, and the trip volume is continuous, so
	 * this is the knob that decides whether the picture still agrees with the rule. Turned down, the
	 * mannequins read as the bright thing and the smear as the blur joining them; turned to 1 it is
	 * the old solid-fence look. DO NOT take it low enough that the gaps between ghosts read as
	 * passable — they are not passable, and a player who learns otherwise learns a lie.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail|Ghosts", meta = (DisplayName = "Smear Glow (fraction of ghost glow)", ClampMin = "0.02", ClampMax = "4.0", UIMin = "0.1", UIMax = "1.5"))
	float TraceSmearGlowScale = 0.5f;

	/**
	 * Forced LOD on the after-images: 0 = automatic (screen-size driven), 1 = LOD0, 2 = LOD1, ...
	 * Mirrors Trace.Trail.GhostForcedLOD.
	 *
	 * Automatic by default because ghosts are exactly what the LOD system is good at — static, often
	 * distant, and with no animation whose popping would give a transition away. Exposed because
	 * forcing a low LOD is the single biggest perf lever here if a ten-player capture ever measures
	 * the skinned draws as expensive.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail|Ghosts", meta = (DisplayName = "Ghost Forced LOD (0 = auto)", ClampMin = "0", ClampMax = "4"))
	int32 TraceGhostForcedLOD = 0;

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
	// 8000 x 4000 field; on the 33600 x 9600 field the same numbers put every escort on top of the
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
