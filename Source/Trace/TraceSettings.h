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
 * SPEC v14 §2 CHANGED WHICH ONE IS DEFAULT. ThrownCoreAndGoals (mode B) is now the default
 * everywhere — settings, menu, fresh install. EndzoneStatusCore (mode A) is FROZEN: it must keep
 * working exactly as it does, it takes no characters and no abilities, and every player in it is
 * the default characterless Mannequin.
 *
 *   EndzoneStatusCore     Endzones spanning the full field width. The Core is a STATUS, not an
 *                         object: it cannot exist on the ground, LMB starts the 0.5 s hover-pass,
 *                         and possession moves on kill / trace break / completed pass.
 *   ThrownCoreAndGoals    CIRCULAR goals set into the back walls (spec v6 §4.3) instead of
 *                         endzones — a hoop of diameter GoalWidthFieldFraction of the field width,
 *                         its bottom raised 1.5 player heights off the floor. The Core is a
 *                         physical entity:
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
	// FireInterval is 0.315789s AS OF SPEC v24 §4 (190 RPM, up from 150 = 0.40s) and a body shot is
	// 40, so a held trigger on target kills in 0.63s — and a single head shot still kills instantly.
	// Nothing about reaction time or aim error survives that. Bursting is the DPS dial, and it is
	// the one that makes a fight readable: you can hear the gap and move in it.
	//
	// READ THIS BEFORE TUNING A BURST NUMBER. At 0.3158s between rounds a burst of 0.20-0.38s holds
	// ONE round at the short end and TWO at the long end (0.38 / 0.3158 = 1.2), where at the 0.40s
	// gun every burst held exactly one and at the old 0.16s gun they held two or three. So the 190
	// RPM pass hands the bots a little of their lost DPS back at the top of the roll, and the duty
	// cycle still does not describe bot DPS on its own — the ROUND COUNT does.
	// These profiles are deliberately left alone in the fire-rate pass: the Easy baseline (~0.72
	// human deaths per minute while engaged) was MEASURED, and moving the rate and the profiles in
	// the same pass would make the next measurement uninterpretable. If bots read as harmless after
	// this change, raise BurstDurationMin/Max to hold more than one round, or cut the rests.
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
	//   3. The BotDifficulty config property below, which defaults to NORMAL as of spec v9 §9
	//      (and is pinned to the same value in Config/DefaultGame.ini, which wins).
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

	// ------------------------------------------------------------------------------------------
	// DEFERRED HALF TIME / FULL TIME (spec v9 §11, new)
	//
	// Verbatim: "Change halftime to trigger after the time runs out and the current play ends, so
	// that it doesn't cut people off in the middle of a run E.g. the ball drops in game mode b, any
	// turnover of the core happens between teams, or a goal is scored, then half time triggers."
	//
	// Both knobs are read AT THE POINT OF USE by ATraceGameMode, so they retune with a match
	// running: flipping the switch mid-half changes what the next expiry does, and moving the cap
	// mid-defer is honoured by the next arm. Neither is latched.
	//
	// THE MERCY RULE IS NOT ROUTED THROUGH EITHER OF THESE. It is a mercy, not a play boundary, and
	// still ends the match on the frame the lead reaches MercyRuleLead — mid-run if that is where it
	// falls. See ATraceGameMode::FinishMatch, which drops any pending whistle on its way past.
	// ------------------------------------------------------------------------------------------

	/**
	 * ON: the clock expiring ARMS the whistle instead of blowing it. Play continues, the HUD shows
	 * the clock at 0:00 with "HALF/MATCH ENDS AT NEXT DEAD BALL", and the period actually ends at
	 * the next goal, between-team turnover, or (mode B) Core coming down.
	 *
	 * OFF restores the pre-v9 behaviour exactly: the period ends on the tick the clock does, in the
	 * middle of whatever was happening. Kept as a switch because "the half ended late" and "the half
	 * ended mid-run" are opposite complaints and this is the one line that swaps between them.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Defer Half/Full Time To The Next Dead Ball"))
	bool bDeferPeriodEndToPlayBreak = true;

	/**
	 * THE GUARD RAIL. Seconds a deferred whistle will wait for a dead ball before ending the period
	 * regardless. Zero disables the cap entirely — DO NOT SHIP IT AT ZERO.
	 *
	 * Without this, two teams that neither score nor turn the Core over play past full time forever;
	 * a stalemate is exactly the situation in which nobody is willing to be the one to concede
	 * possession, so it is not a hypothetical. 60 s is one long possession's worth of grace
	 * ([ASSUMPTION], spec §11) — long enough that a genuine run finishes on its own terms, short
	 * enough that a hung match is over within a minute of the clock.
	 *
	 * Read live by ATraceGameMode::BeginPendingPeriodEnd, so an edit lands on the next expiry.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match", meta = (DisplayName = "Max Defer Before Forcing Half/Full Time (s, 0 = no cap)", ClampMin = "0.0", ClampMax = "600.0", UIMin = "0.0", UIMax = "180.0"))
	float PeriodEndMaxDeferSeconds = 60.f;

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
	 * Which ruleset the next match plays.
	 *
	 * *** SPEC v14 §2: MODE B IS NOW THE DEFAULT. *** Verbatim: "Change game mode b to the default
	 * game mode." This property is THE fresh-install default and it is the only thing that had to
	 * change to make that true everywhere:
	 *
	 *   settings default   this line, mirrored in Config/DefaultGame.ini (and the ini wins).
	 *   menu default       ATraceMenuHUD::BeginPlay READS the mode from here rather than holding its
	 *                      own copy ("The mode goes the other way: it is READ from the settings"),
	 *                      so the title screen comes up on B without a UI change.
	 *   match default      ATraceGameMode::ResolveScoringMode starts from this value and only a
	 *                      travel-URL "?mode=a" or "-TraceScoringMode=a" overrides it.
	 *
	 * MODE A IS FROZEN, NOT DELETED (§2): it must keep working exactly as it does, and selecting it
	 * still plays the shipped endzone game — with no characters and no abilities, because
	 * UTraceAbilityComponent::AreCharactersEnabled returns false in mode A.
	 *
	 * NOT a live knob in the way the rest of this page is: read it at match start, not at the point
	 * of use. See the latching note above.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match|Scoring Mode", meta = (DisplayName = "Scoring Mode (A/B test) [v14 §2: B IS THE DEFAULT]"))
	ETraceScoringMode ScoringMode = ETraceScoringMode::ThrownCoreAndGoals;

	/**
	 * MODE B ONLY. Width of each goal as a fraction of the FULL field width.
	 *
	 * Verbatim (v4): "The goal should not be the entire width of the map, like the endzone."
	 * Verbatim (v5 §4): "For game mode b ONLY ... decrease the size of the goal (reduce height and
	 * width)."
	 *
	 * 0.3333 -> 0.2083 THIS PASS: a 3200 uu goal mouth on the 9600 uu wide field becomes 2000 uu
	 * ([ASSUMPTION], spec v5 §4). Still wide enough to throw at from an angle — 2000 uu is about six
	 * character-widths of margin either side of a defender — and now narrow enough that ONE defender
	 * standing in front of it is a real obstacle, which is the point of shrinking it.
	 *
	 * Ignored entirely in mode A, where the endzone spans the full width by design. Keep it under
	 * ~0.6 or the distinction the spec is asking for stops existing; below ~0.12 (1150 uu) a thrown
	 * Core has to be threaded and mode B stops scoring at all.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match|Scoring Mode", meta = (DisplayName = "Goal Width (fraction of field width) [mode B]", ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.1", UIMax = "0.6"))
	float GoalWidthFieldFraction = 0.2083f;

	/**
	 * MODE B ONLY. Height of the goal volume, uu, measured from the floor.
	 *
	 * REPOINTED BY SPEC v6 §4.3 — READ THIS BEFORE TUNING IT. Until v6 this was the height of a
	 * free-standing goal between the goal line and the end wall. There is no such goal any more: the
	 * goal is a CIRCLE set into the back wall, its diameter given by GoalWidthFieldFraction and its
	 * height off the floor by the arena builder's GoalRingRaisePlayerHeights. Rather than leave a
	 * slider that moves nothing — this project's rule is that a dead knob is worse than no knob —
	 * this is now the dial on the GOAL APPROACH RAMP, the run-up that makes carrying the Core
	 * through a raised hoop possible at all (ATraceArenaBuilder::GoalRampTopZ, which clamps it to
	 * one step below the hoop).
	 *
	 * At its shipped 440 the clamp wins and the ramp sits exactly where the ring puts it. Lower it
	 * and the run-up gets lower, which makes carrying one in harder; 0 removes the ramp entirely and
	 * leaves throwing as the only way to score.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Match|Scoring Mode", meta = (DisplayName = "Goal Approach Ramp Height (uu) [mode B]", ClampMin = "50.0", ClampMax = "5000.0", UIMin = "200.0", UIMax = "2000.0"))
	float GoalHeightUU = 440.f;

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

	// ------------------------------------------------------------------------------------------
	// HEALTH REGENERATION (spec v13 §1) LIVES ON UTraceHealthSettings, NOT HERE.
	//
	// *** THE TWO PROPERTIES THAT WERE HERE — HealthRegenDelaySeconds AND HealthRegenPerSecond —
	// *** ARE DELETED, AND DELETING THEM IS THE FIX RATHER THAN A TIDY-UP. ***
	//
	// They were declared on this page in the same pass that the health slice declared
	// `bRegenEnabled` / `RegenDelaySeconds` / `RegenRatePerSecond` on its own UTraceHealthSettings
	// page (Gameplay/TraceHealthComponent.h), and Config/DefaultGame.ini shipped BOTH blocks with
	// the same numbers in them. Two config sections, four keys, one mechanic.
	//
	// Nothing ever read the pair on this page. namespace TraceHealthRegen is the only reader of the
	// regeneration numbers anywhere in the project, and it reads UTraceHealthSettings — so these two
	// were live-looking, ini-backed, EditAnywhere sliders that moved nothing at all. That is exactly
	// the "a misnamed knob silently does nothing" failure this file's whole verification apparatus
	// exists to catch, except that both knobs existed and were spelled correctly; they were simply
	// not the ones with a reader behind them. Worse than merely dead: a designer retuning the delay
	// to 6 s here would have seen the game keep using 9, with no error anywhere.
	//
	// The surviving page is the one with the reader, per the precedent UTraceMeleeSettings and
	// UTraceDamageSettings already set — a mechanic's numbers live beside the code that consumes
	// them. Trace.VerifyKnobs reaches across to it by /Script path (see FKnobSpec::OwnerPath) and
	// Trace.DumpSettings prints it, so nothing was lost by the move except the second copy.
	//
	// DO NOT RE-ADD THEM HERE OUT OF SYMMETRY WITH MaxHealth ABOVE. If regeneration ever does need a
	// knob on this page, it needs the READER moved with it in the same commit.
	// ------------------------------------------------------------------------------------------

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
	 * *** 36000 -> 39600 THIS PASS, AND IT IS SPEC v28 §8's BILL, NOT A RETUNE. *** §8 put a 2400 uu
	 * hockey pocket behind each goal, so ATraceArenaBuilder::FieldLength went 33600 -> 38400 and the
	 * wall-to-wall diagonal went 34944 -> sqrt(38400^2 + 9600^2) = 39581 uu. The shipped 36000 was
	 * 3581 uu SHORT of that — a shot from one back pocket down the long diagonal to the other would
	 * have died in mid-air short of a target the player could plainly see, which is the exact failure
	 * the 28000 -> 36000 move already fixed once. 39600 covers 39581 with 19 uu of margin, and the
	 * margin is deliberately thin: this number is a REACH, not a range budget, and every uu of it is
	 * paid for by the trace. The §8 owner found this and could not fix it — this file is not theirs.
	 *
	 * THE PAIRING RULE, restated because it has now been missed twice: this value is DERIVED from
	 * ATraceArenaBuilder::FieldLength and FieldWidth. It is not independent of them and it must move
	 * whenever they do. Trace.Arena.VerifyHitscanReach asserts exactly that against the running
	 * builder, so the next lengthening fails a harness instead of silently shortening the guns.
	 *
	 * Raising it does NOT make the bots deadlier (see below) and does NOT change any damage. There is
	 * no distance falloff anywhere in UTraceWeaponComponent's hitscan — damage is chosen by HIT ZONE
	 * alone (head / body / leg) and the trace length never enters the number — so a 39600 uu trace
	 * hits for exactly what a 36000 uu one did. The only difference is that shots stop expiring in
	 * the air short of a target that is on screen.
	 *
	 * Raising it does NOT make the bots deadlier: they are limited by FTraceBotProfile::
	 * MaxEngagementRange (4200 Easy / 4800 Normal / 6000 Hard), far below either value. This only
	 * restores the human's ability to shoot what they can see.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Hitscan Range (uu)", ClampMin = "100.0", ClampMax = "200000.0", UIMin = "5000.0", UIMax = "50000.0"))
	float HitscanRange = 39600.f;

	/**
	 * SECONDS BETWEEN SHOTS — this is the inverse of the fire RATE, so a BIGGER number is a SLOWER
	 * gun. The server validates a client's claimed fire rate against this with a tolerance.
	 *
	 * *** 0.40 -> 0.315789 THIS PASS (spec v24 §4). *** "Gun fire rate 150 -> 190 RPM."
	 * 60/190 = 0.315789 s between shots, i.e. 3.17 shots/second, up from 2.5 (150 RPM).
	 * The pass before this one took it 0.16 -> 0.40 for spec v5 §5's 150 RPM.
	 *
	 * *** THIS IS THE BASE EVERY FIRE-RATE ABILITY IS RELATIVE TO (spec v24 §0). *** Nothing else in
	 * the project stores an RPM or an interval for the gun: Roxie's MODDED and Slimeball's stuck
	 * passive are stored as RATE MULTIPLIERS (RoxieModdedFireRateMultiplier,
	 * SlimeballStuckFireRateBonus) and reach the gun through
	 * UTraceAbilityComponent::GetFireIntervalScaleFor(), which returns a scale ON this number. Move
	 * this line and both characters move with it, automatically and in the same proportion — that is
	 * the §0 rule, and it is why this pass re-tuned nothing on either character.
	 *
	 * WHAT IT COSTS:
	 *   * three body shots (40 each) now take 0.63 s of sustained fire on target, down from 0.80 s;
	 *   * a head shot (100) still kills in one, so the head/body gap is 3:1 in rounds and 0.63 s in
	 *     wall-clock — aiming high is still most of the gun, by slightly less;
	 *   * a 30-round clip is now 9.16 s of continuous fire instead of 11.6 s (both measured from the
	 *     FIRST shot, which is free), so reloads come round ~21% sooner. See ClipSize.
	 *   * bot bursts (BurstDurationMin/Max, 0.20-0.38 s) still contain one round at the short end and
	 *     now two at the long end. Those profiles are deliberately NOT retuned here; see the note in
	 *     the Burst block above.
	 *
	 * Sane range 0.08 (twice as fast as the old gun, very lethal) to 0.60 (a bolt-action feel).
	 * Below ~0.05 the server's rate validation starts rejecting legitimate client shots.
	 *
	 * ALSO SET IN Config/DefaultGame.ini, AND THE INI WINS. Both were moved; verify from a running
	 * game with Trace.DumpSettings or Trace.FireRate.Measure rather than from this line.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Fire Interval (s between shots)", ClampMin = "0.02", ClampMax = "2.0", UIMin = "0.05", UIMax = "0.8"))
	float FireInterval = 0.315789f;

	// ==========================================================================================
	// COMBAT — UPWARDS RECOIL  (spec v5 §6, new — TURNED OFF BY SPEC v25 §5)
	//
	// *** SPEC v25 §5 — "Remove gun recoil, keep the firing rate." bRecoilEnabled IS NOW FALSE. ***
	//
	// WHICH RECOIL WENT, AND WHICH DID NOT. There are two separate things on this gun and only one
	// of them is "recoil" in the sense the notes mean:
	//
	//   1. THE VIEW / AIM PUNCH — this block. It writes UPWARD PITCH INTO THE PLAYER'S CONTROL
	//      ROTATION, so the crosshair, the camera and the next shot's ray all climb. It is the one
	//      that MOVES YOUR AIM, it is what "recoil" means in every shooter, and it is what §5
	//      removes. Applied by UTraceWeaponComponent::ApplyRecoilKick().
	//
	//   2. THE VIEWMODEL KICK — ATraceCharacter::NotifyWeaponFired() / ViewModelKick, in
	//      Source/Trace/Core/TraceCharacter.cpp. It jolts the FIRST-PERSON GUN MESH back and up and
	//      lets it settle. It is a cosmetic animation on the rig: it does not touch the control
	//      rotation, it does not move the crosshair, and it cannot change where a round goes. It is
	//      NOT touched by §5 — removing it would take the gun's animation away without removing any
	//      aim penalty at all, which is the opposite of what was asked for. The muzzle flash and the
	//      tracer stay for the same reason.
	//
	// THE FIRE RATE IS UNTOUCHED. FireInterval stays at 0.315789 s = 190 RPM (spec v24 §4), which is
	// the line immediately above this block. Nothing in the recoil path reads or scales it.
	//
	// WHY A MASTER-SWITCH FLIP AND NOT A DELETION. The kick is applied through exactly one call
	// (ApplyRecoilKick, which returns immediately on !bRecoilEnabled), so the flag is a real removal
	// and not a mask: with it off nothing ever adds pitch, RecoilAppliedPitch never leaves 0, and
	// TickRecoil early-outs on its first line. The tuning knobs below are left at their v5 values on
	// purpose rather than being zeroed — zeroing them would leave the master switch as a control that
	// turns nothing back on, which is this file's "dead knob" failure wearing the other hat. One flag
	// is the whole difference between the two arms, so `Trace.TestRecoil` can measure them both.
	//
	// Verbatim (v5 §6, the request this block was built for): "Add upwards recoil, mimicking 100
	// upwards recoil from destiny 2".
	//
	// [ASSUMPTION] A recoil-DIRECTION stat of 100 in Destiny 2 means the muzzle climbs on a perfectly
	// vertical line: no horizontal drift, no left/right bias, no randomness in the direction. So this
	// block has a pitch term and NOTHING ELSE. There is deliberately no yaw knob, not even one
	// defaulted to zero — same reasoning that deleted SpreadDegrees: a horizontal term that exists
	// can be reintroduced by a stale .ini or by a modified client, and "purely vertical" is a rule
	// rather than a default.
	//
	// WHERE IT IS APPLIED, AND WHY THAT CANNOT DESYNC THE SHOT
	// UTraceWeaponComponent applies the kick to the LOCAL player controller's control rotation,
	// AFTER the shot's direction has been sampled and sent. Three consequences, all of them the
	// point:
	//   * the round that produced the kick goes exactly where the crosshair was when the trigger
	//     broke — recoil never bends the bullet that caused it;
	//   * the SERVER is told the direction, not the rotation (UTraceWeaponComponent::ServerFire
	//     carries Origin + Direction), so the authority resolves the same ray the shooter saw. There
	//     is no recoil state to replicate and therefore nothing to disagree about;
	//   * the camera and the aim ray are both pure functions of the control rotation
	//     (ATraceCharacter::ResolveAimRotation), so moving it moves them together and
	//     Trace.DebugViewProbe still measures aimErr = 0.0000 deg.
	//
	// BOTS DO NOT GET RECOIL, and that is a decision rather than an oversight. A bot's control
	// rotation is overwritten every frame by ATraceBotController's RInterpConstantTo slew toward its
	// own aim point, so a kick would be erased within a frame or two and would tune nothing while
	// adding jitter to a system whose difficulty ladder was measured. The bots' DPS dial is the
	// burst duty cycle; this is the human's.
	// ==========================================================================================

	/**
	 * Master switch for view recoil. OFF restores the perfectly static muzzle the gun shipped with.
	 *
	 * *** FALSE AS OF SPEC v25 §5. This one line is the whole of "remove gun recoil". ***
	 *
	 * It was already the A/B switch — it exists so the gun feel could be compared against its
	 * absence from one binary — so the removal is the arm that was already built and measured, not a
	 * new code path. `Trace.TestRecoil` reports peak climb, residual pitch and yaw drift; with this
	 * false every one of them must read 0.000, and that is the acceptance evidence for §5.
	 *
	 * Set it back to True (here AND in DefaultGame.ini, which wins) to restore the v5 §6 gun exactly:
	 * every tuning knob below is still at its shipped value and nothing else was edited.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Upwards Recoil Enabled"))
	bool bRecoilEnabled = false;

	/**
	 * Degrees of UPWARD pitch added to the local player's view by the FIRST shot of a burst.
	 *
	 * THE HEADLINE KNOB. At arena distances one degree is about 70 uu of vertical travel per 4000 uu
	 * of range, and the character capsule is 176 uu tall — so 0.8 deg moves the point of aim by
	 * roughly a third of a body per shot at a long engagement, and by almost nothing across a room.
	 * That distance dependence is what makes recoil a range-dependent skill test rather than a flat
	 * accuracy tax.
	 *
	 * Sane range 0.4 (barely felt) to 1.5 (a hand cannon). Above ~2 the second round of a burst is
	 * over the target's head at any range and the gun becomes single-shot in practice.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Pitch Kick Per Shot (deg)", ClampMin = "0.0", ClampMax = "15.0", UIMin = "0.0", UIMax = "3.0"))
	float RecoilPitchPerShot = 0.80f;

	/**
	 * Fraction of the base kick ADDED for each further consecutive shot, so sustained fire climbs.
	 *
	 * Shot n of a burst kicks PitchKickPerShot * (1 + this * n), n counting from 0. At 0.18 a burst
	 * goes 0.80, 0.94, 1.09, 1.23 ... which is the Destiny read: the first round is nearly free and
	 * the pattern opens up on you if you hold the trigger. 0 makes every shot in a burst identical.
	 *
	 * Sane range 0 to 0.35. The total is bounded by Max Accumulated Pitch below whatever this is.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Growth Per Consecutive Shot (fraction)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.5"))
	float RecoilPitchGrowthPerShot = 0.18f;

	/**
	 * Ceiling, in degrees, on how far above the original aim the un-recovered climb may take the
	 * view. The kick is truncated at the ceiling rather than the ceiling being enforced afterwards,
	 * so the view never overshoots and snaps back.
	 *
	 * This is what stops a held trigger from walking the crosshair into the sky, and it is the knob
	 * to lower if long bursts stop being aimable at all. Sane range 3 to 8.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Max Accumulated Pitch (deg)", ClampMin = "0.0", ClampMax = "45.0", UIMin = "1.0", UIMax = "12.0"))
	float RecoilMaxPitchDegrees = 6.0f;

	/**
	 * Seconds after the LAST shot before recovery starts pulling the view back down.
	 *
	 * KEEP THIS AT OR ABOVE FireInterval (now 0.3158, spec v24 §4) OR THE GUN NEVER CLIMBS. Recovery
	 * that starts between two shots of a held burst undoes each kick before the next one lands, which
	 * produces a muzzle that twitches and returns instead of a pattern the player can learn and
	 * pre-aim against. 0.45 s was "one fire interval plus a beat" at the old 0.40 s gun; at 0.3158 it
	 * is 1.43 intervals, so the constraint is satisfied with MORE room than before and the recoil is
	 * still something that happens when you stop shooting.
	 *
	 * *** DELIBERATELY NOT MOVED IN THE 150 -> 190 RPM PASS. *** Its rule is a FLOOR ("at or above
	 * FireInterval"), not a proportion, and the faster gun only moves it further inside that floor.
	 * Re-deriving it as FireInterval x 1.125 would have shortened it to 0.355 s and changed the feel
	 * of a knob nobody asked to change. If FireInterval ever goes ABOVE 0.45 this must move with it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Recovery Delay (s after last shot)", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.0"))
	float RecoilRecoveryDelaySeconds = 0.45f;

	/**
	 * Proportional recovery rate, per second (an FInterpTo speed): the view gives back this fraction
	 * of the REMAINING climb every second, so the return decelerates as it lands.
	 *
	 * Proportional rather than linear because a constant-rate return reads as the camera being
	 * driven by something rather than settling. At 6 the first half of the climb is gone in about
	 * 0.12 s and the tail takes ~0.5 s. Sane range 3 (languid) to 12 (snappy).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Recovery Speed (proportional, per s)", ClampMin = "0.0", ClampMax = "40.0", UIMin = "1.0", UIMax = "16.0"))
	float RecoilRecoverySpeed = 6.0f;

	/**
	 * Floor, in degrees per second, under the proportional recovery above.
	 *
	 * A purely proportional return has an infinite tail: it takes as long to go 0.2 deg -> 0.1 deg
	 * as 2 deg -> 1 deg, so the view hangs a fraction of a degree high for a second or more and the
	 * player's next burst starts from somewhere they did not choose. This term is what actually
	 * lands it. Sane range 2 to 8; 0 restores the infinite tail.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Recovery Floor (deg/s)", ClampMin = "0.0", ClampMax = "90.0", UIMin = "0.0", UIMax = "20.0"))
	float RecoilRecoveryMinRateDegrees = 4.0f;

	/**
	 * A gap of this many seconds between shots resets the per-shot GROWTH back to the first-shot
	 * kick. It does not touch the climb already accumulated, which recovers on its own schedule.
	 *
	 * Without it, ten single aimed shots spread over a minute would each kick harder than the last.
	 * Keep it above FireInterval (now 0.3158, spec v24 §4) or a held burst resets its own growth
	 * every shot and the growth term does nothing. Sane range 1.5x to 3x FireInterval, which at the
	 * new gun is 0.474 to 0.947 — 0.75 sits inside it, so this was left alone in the 190 RPM pass.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Burst Reset Gap (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.1", UIMax = "2.0"))
	float RecoilBurstResetSeconds = 0.75f;

	/**
	 * True: pulling the mouse DOWN during recovery cancels an equal amount of the pending return,
	 * so the player's own compensation is not paid back to them as a second, downward kick when the
	 * gun settles.
	 *
	 * This is the difference between recoil a player can fight and recoil that fights back. With it
	 * off, a player who drags down 3 degrees to hold the crosshair on a chest gets those 3 degrees
	 * subtracted AGAIN by the recovery and ends up aiming at the floor. Every shooter with learnable
	 * recoil does this; it is off only as a diagnostic.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Player Compensation Cancels Recovery"))
	bool bRecoilPlayerCompensationCancels = true;

	// ==========================================================================================
	// SPEC v29 §2e — ROXIE'S MODDED PUTS THE RECOIL BACK, FOR HER, WHILE IT IS UP
	//
	// Verbatim: "Roxie's modded should add recoil now" — "it is her trade for the higher fire rate".
	//
	// Demo 22 (spec v25 §5) removed the aim punch globally and bRecoilEnabled above is FALSE. It stays
	// false: this is not a partial revert of that decision. MODDED adds a kick ON TOP of whatever the
	// global switch is worth, so:
	//
	//     shipped        recoil off for everyone, ON for a Roxie with MODDED up, and only while it is
	//     bRecoilEnabled=True (the v5 gun)  everyone kicks, and Roxie under MODDED kicks
	//                    (1 + RoxieModdedRecoilScale) times as hard — still her trade, still relative
	//
	// *** IT IS A MULTIPLE OF RecoilPitchPerShot AND NOT A NUMBER OF DEGREES (standing rule). *** The
	// value MODIFIES the base per-shot kick, so it is stored relative to it: retune RecoilPitchPerShot
	// and Roxie's recoil moves with it, in proportion, with nothing to re-derive. A "RoxieRecoilDegrees
	// = 0.8" would have been a second, silent definition of the same fact and would have gone stale the
	// first time the base moved — which is precisely how this project's fire-rate numbers went stale
	// before spec v24 §0 made them ratios.
	//
	// EVERY OTHER TERM OF THE MODEL IS SHARED AND DELIBERATELY NOT DUPLICATED: the per-shot growth, the
	// climb ceiling, the recovery delay/speed/floor, the burst reset and the compensation rule are the
	// eight knobs above. Roxie scales the KICK; she does not get her own recoil model. That is what
	// makes "her trade" tunable from one place.
	// ==========================================================================================

	/**
	 * Degrees of per-shot kick MODDED adds, AS A MULTIPLE OF RecoilPitchPerShot.
	 *
	 * 1.0 = exactly the v5 gun's kick (0.80 deg on the first round of a burst, growing 18% a round to
	 * the 6 deg ceiling) while MODDED is up, and nothing at all when it is not.
	 *
	 * *** 0.0 IS THE RED ARM: MODDED with no recoil, i.e. the Demo 24 build. *** Trace.Weapons.V29
	 * fires her at 0.0 and at the shipped value and compares the measured climb, because an arm that
	 * only ever ran with the feature on cannot tell a working kick from a coincidence.
	 *
	 * WHY IT IS ADDITIVE ON THE GLOBAL SWITCH RATHER THAN AN OVERRIDE: an override would mean turning
	 * bRecoilEnabled back on made Roxie's recoil identical to everybody else's, i.e. deleted her trade
	 * at the exact moment the designer was tuning recoil. Adding keeps the sentence "MODDED costs you
	 * recoil" true in both worlds.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Recoil", meta = (DisplayName = "Roxie MODDED: added recoil (x Pitch Kick Per Shot) [v29 §2e: 1.0; 0 = the Demo 24 build]", ClampMin = "0.0", ClampMax = "8.0", UIMin = "0.0", UIMax = "3.0"))
	float RoxieModdedRecoilScale = 1.f;

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
	 *
	 * SPEC v13 §3: 1.30 -> 1.22. Verbatim: "Update carrier speed to match the new knife speed."
	 *
	 * THIS CLOSES THE PARITY BREAK v12 §3 OPENED AND FLAGGED. The whole reason this number was ever
	 * 1.30 is an older request to "increase core carrier speed to match knife speed"; v12 §3 then cut
	 * the knife to 1.22 and deliberately left the carrier alone, so for one pass the CARRIER was the
	 * faster of the two (1.30 vs 1.22, +6.6%) and a defender who drew the knife to chase a carrier
	 * could not run them down in a straight line. The user has now made the call, and this number
	 * exists only to equal KnifeMoveSpeedMultiplier.
	 *
	 * *** THE INVARIANT, STATED SO IT SURVIVES THE NEXT KNIFE RETUNE: THIS VALUE TRACKS
	 * *** KnifeMoveSpeedMultiplier. They are two properties rather than one because they multiply
	 * *** different things for different reasons and a designer must be able to break parity on
	 * *** purpose — but if the knife moves and this does not, the parity the user has now twice asked
	 * *** for is broken again. Both read 1.22 -> 800 x 1.22 = 976 uu/s. Trace.VerifyKnobs prints them
	 * *** next to each other and flags any disagreement.
	 *
	 * The two DO NOT STACK: a carrier holding the knife gets this multiplier only, because
	 * TraceMelee::ShouldUseKnifeMovementProfile returns false for a carrier (the knife is stowed, not
	 * active). With both at 1.22 that clause no longer changes any speed — but it still matters, and
	 * must stay, because it is what stopped 1.30 x 1.22 = 1.59x and is the reason parity means one
	 * number rather than two multiplied.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Walk", meta = (DisplayName = "Carrier Speed Multiplier [v13 §3: = knife 1.22]", ClampMin = "0.5", ClampMax = "2.0", UIMin = "1.0", UIMax = "1.4"))
	float CarrierSpeedMultiplier = 1.22f;

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
	 *
	 * LEFT AT 1600 THIS PASS, ON PURPOSE. Spec v5 §1's hard cap is AirStrafeHardCapSpeed below, and
	 * the movement component takes the TIGHTER of the two, so lowering this as well would quietly
	 * make it the operative ceiling and leave the v5 knob doing nothing. This is the model-wide
	 * ceiling; that one is the strafe-accumulation ceiling. Keep this at or above it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Max Air Speed (uu/s, model ceiling)", ClampMin = "50.0", ClampMax = "8000.0", UIMin = "400.0", UIMax = "3000.0"))
	float MaxAirSpeed = 1600.f;

	// --- Air-strafe diminishing returns (spec v5 §1, new) --------------------------------------
	//
	// Verbatim: "The air strafing feels incredible, but its too powerful with how much momentum can
	// be gained. I think we need a hard cap on it or an exponential scale in order to make it harder
	// and harder to gain momentum past a certain point."
	//
	// THIS IS PRAISE WITH A CEILING ON IT, not a request to undo the movement model. What is capped
	// is ACCUMULATION — how much speed a strafe may ADD — and nothing else. Turning without losing
	// speed is untouched, because the falloff scales the ADD (which is zero for input along the
	// direction you are already travelling) and never scales the velocity the pawn already has.
	//
	// THE CURVE. With planar speed S, soft cap C and hard cap H:
	//     t     = clamp((S - C) / max(1, H - C), 0, 1)      -- 0 at the soft cap, 1 at the hard cap
	//     Scale = (1 - t) ^ AirStrafeFalloffExponent
	//     the air-accel step's computed add is multiplied by Scale
	// Below the soft cap Scale is 1 and the air model is bit-for-bit what it was. Between the caps
	// each further uu/s costs more input than the last; at the hard cap the add is zero.
	//
	// Read at the point of use inside ApplySourceAirAcceleration — i.e. inside the physics sub-step,
	// on the client, on the server and on every replayed move — so it is a pure function of
	// (Velocity, these values) and adds no saved-move state. Every knob here is safe to drag in PIE.
	//
	// NAMES ARE LOAD-BEARING. UTraceCharacterMovementComponent resolves all four BY NAME through
	// FindPropertyByName (TraceMoveKnob::Float / ::Bool) because it was written in the same pass as
	// this page and could not declare the UPROPERTYs itself. A rename on either side does not fail
	// the build — it silently falls back to the literal at the call site and the panel slider stops
	// doing anything. Trace.VerifyKnobs and the movement component's own bind report both print the
	// answer at startup; keep the spellings identical.

	/**
	 * Master switch for the diminishing-returns curve. OFF leaves only the hard cap, which is the
	 * "or" in the spec's "a hard cap OR an exponential scale" — both are implemented so the design
	 * owner can play them separately and keep the one that feels right.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Strafe Gain Falloff"))
	bool bAirStrafeGainFalloff = true;

	/**
	 * Planar speed, uu/s, at which strafing STOPS being free and starts paying diminishing returns.
	 * Below this the air model is exactly what it was.
	 *
	 * 950 sits just under the measured baseline (a continuous strafe turn reaches ~1036 uu/s from
	 * 835), so today's strafe is left almost entirely intact and it is the accumulation PAST it that
	 * gets expensive — which is what "harder and harder to gain momentum past a certain point" asks
	 * for. Lower it toward WalkSpeed (800) to make air speed mostly a function of what you jumped in
	 * with. Must stay below the hard cap or the curve has no room to act.
	 *
	 * THIS IS THE BASE, NOT THE EFFECTIVE CAP. Spec v9 §8's "+10%" is NOT baked in here — it is
	 * applied on top by AirStrafeAsymptoteScale, which the movement component multiplies into both
	 * caps. Effective soft cap = this x AirStrafeAsymptoteScale = 950 x 1.10 = 1045. Editing this to
	 * 1045 as well is the double-application trap: it would ship 1149.5 with nothing saying so.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Strafe Soft Cap BASE (uu/s, x asymptote scale)", ClampMin = "50.0", ClampMax = "8000.0", UIMin = "400.0", UIMax = "2000.0"))
	float AirStrafeSoftCapSpeed = 950.f;

	/**
	 * Shape of the falloff between the soft cap and the hard cap: Scale = (1 - t) ^ this.
	 *
	 * 1 is a straight line — every uu/s of headroom costs the same. 2 (the default) is the
	 * "exponential scale" reading: it keeps most of the strafe's value until well past the soft cap
	 * and then collapses it, so the speed asymptotes visibly short of the hard cap and a player
	 * feels the ceiling arrive rather than hitting it. Above ~4 the gain dies almost at the soft cap
	 * and the soft cap IS the cap. Sane range 1.5 to 3; never 0, which would be a silent no-op that
	 * still reported itself as enabled.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Strafe Falloff Exponent", ClampMin = "0.1", ClampMax = "8.0", UIMin = "1.0", UIMax = "4.0"))
	float AirStrafeFalloffExponent = 2.0f;

	/**
	 * Master switch for the absolute strafe ceiling — the "hard cap on it" half of the spec's
	 * request. OFF leaves only the falloff curve (and MaxAirSpeed, which is the model-wide ceiling
	 * and is not a v5 knob).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Strafe Hard Cap Enabled"))
	bool bAirStrafeHardCap = true;

	/**
	 * The hard ceiling, uu/s, on planar speed that air input may reach. THE BACKSTOP.
	 *
	 * 1250 is ~20% above the measured 1036 baseline: with the falloff on, the curve asymptotes well
	 * short of it and this is only ever reached by input patterns the curve did not anticipate,
	 * which is what a backstop is for. Speed CARRIED into the air (a dash, a slide-jump) is not
	 * clamped by it — the movement component takes max(this, the speed you left the ground with), so
	 * a cap can never confiscate momentum a player already had, which is the spec v3 §2.4 rule.
	 *
	 * Must stay above AirStrafeSoftCapSpeed and at or below MaxAirSpeed.
	 *
	 * ALSO A BASE, for the same reason as the soft cap above: AirStrafeAsymptoteScale multiplies it,
	 * so the effective hard cap is 1250 x 1.10 = 1375 and MaxAirSpeed (1600) is still the wider of
	 * the two. Both caps move together on purpose — see AirStrafeAsymptoteScale.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Strafe Hard Cap BASE (uu/s, x asymptote scale)", ClampMin = "50.0", ClampMax = "8000.0", UIMin = "600.0", UIMax = "2500.0"))
	float AirStrafeHardCapSpeed = 1250.f;

	/**
	 * SPEC v9 §8 — "Move the asymptote on momentum slightly higher, to allow for slightly faster
	 * speeds." ONE scalar over BOTH air-strafe caps.
	 *
	 * The soft cap is where the gain starts to taper and the hard cap is where it reaches zero: they
	 * are two points on one curve, so moving only one of them changes the SHAPE of the falloff
	 * instead of its position. This slides the whole asymptote up and leaves the shape identical.
	 * [ASSUMPTION] +10%, per spec §8 — 950 -> 1045 and 1250 -> 1375.
	 *
	 * A NUDGE, NOT A REMOVAL. The Demo 5 ceiling the design owner asked for is still here; it just
	 * sits 10% further out. MaxAirSpeed (1600) is deliberately not scaled, so the hard cap remains
	 * the tighter of the two and spec v5 §1 still governs.
	 *
	 * NAME IS LOAD-BEARING: UTraceCharacterMovementComponent::GetAirStrafeAsymptoteScale() resolves
	 * it BY NAME as "AirStrafeAsymptoteScale" and clamps to 0.5..2. A rename here does not fail to
	 * compile; it silently reverts to the component's built-in 1.10 and the ini stops driving it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Strafe Asymptote Scale (x both caps)", ClampMin = "0.5", ClampMax = "2.0", UIMin = "0.8", UIMax = "1.5"))
	float AirStrafeAsymptoteScale = 1.10f;

	/**
	 * SPEC v18 §1a — "when you go the opposite direction during the jump ... your momentum doesn't
	 * change at all. We want it so doing so slows down your momentum." This is how fast it slows, in
	 * uu/s^2, at a DEAD 180-degree reversal.
	 *
	 * SCALED BY HOW OPPOSED THE INPUT IS, which is the spec's own [ASSUMPTION] and the thing that
	 * makes it feel like braking rather than like a wall: the brake is multiplied by the NEGATIVE part
	 * of dot(wish direction, direction of travel), so it is EXACTLY ZERO at 90 degrees and at every
	 * angle inside it. Air-strafing — turning INTO your direction of travel — is therefore untouched
	 * as a float equality, not as a tolerance, which is what §1's "do NOT break air-strafing itself"
	 * demands. 5 degrees past square bleeds 9% of a full reversal, 120 degrees 50%, 180 degrees 100%.
	 *
	 * 2200 IS 28% OF AirAcceleration (8000), so a reversal bleeds at roughly a quarter of the rate a
	 * strafe builds and 1000 uu/s dies in 0.45 s. The component ceilings it at AirAcceleration, so it
	 * can never be tuned hard enough to read as hitting a wall — the outcome §1a rules out by name.
	 *
	 * NAME IS LOAD-BEARING. UTraceCharacterMovementComponent::GetAirStrafeOpposingDeceleration()
	 * resolves it BY NAME through TraceMoveKnob as "AirStrafeOpposingDeceleration". A rename here does
	 * not fail to compile: the component silently falls back to its own built-in 2200 and the ini
	 * stops driving it. Its BeginPlay report prints MOVEKNOB BOUND or FALLBACK for this name every
	 * run, and Trace.VerifyKnobs lists it, precisely so the two sides cannot drift apart in silence.
	 * `Trace.Move.AirOpposingDecel <value>` overrides it live for tuning, with no rebuild.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Air Strafe Opposing Deceleration (uu/s^2 at a full reversal)", ClampMin = "0.0", ClampMax = "20000.0", UIMin = "0.0", UIMax = "6000.0"))
	float AirStrafeOpposingDeceleration = 2200.f;

	/**
	 * SPEC v9 §8 — "LESS FLOATY". Multiplier on world gravity for the PLAYER, and only the player.
	 *
	 * Verbatim: "Increase gravity by 12%, to make players feel less floaty when air strafing."
	 * 1.12 is that number, expressed as the engine's UCharacterMovementComponent::GravityScale so it
	 * applies to jumps, falls, wall jumps and the dash arc alike without touching world gravity —
	 * which would also have retuned the mode-B throw (UTraceSettings::CoreThrowGravityScale reads
	 * the world value), and spec §8 asks for the PLAYER to feel heavier, not the ball.
	 *
	 * KNOCK-ON EFFECTS, because §8 asks for them to be reported rather than silently compensated:
	 * apex height scales 1/1.12 (-10.7%) and hang time 1/sqrt(1.12) (-5.5%) for the same jump
	 * impulse. Everything that measures itself against a jump — the wall jump's
	 * WallJumpVerticalMultiplier, DashExitVerticalSpeedMultiplier, SlideJumpZMultiplier — is
	 * expressed as a MULTIPLE of the jump rather than an absolute height, so all of them shrink
	 * together and stay in proportion. The one thing that does NOT move with them is the arena
	 * geometry: the cover tier is still 176 uu tall, and a jump that used to clear a lip with 10%
	 * to spare now clears it with none.
	 *
	 * SPEC v12 §5 MAKES THAT LAST SENTENCE MATTER MORE, NOT LESS. It used to be softened by the
	 * mantle, which caught a player who came up short at a lip and pulled them over it anyway. The
	 * mantle is gone, so a jump that only just clears an edge now simply only just clears it — this
	 * is the first knob to look at if "hitting the top edge of an obstacle" reads badly after the
	 * removal, ahead of LedgeGroundGraceSeconds.
	 *
	 * NAME IS LOAD-BEARING, AND IT IS NOT THE OBVIOUS ONE. UTraceCharacterMovementComponent resolves
	 * this BY NAME through TraceMoveKnob as "MovementGravityScale" — NOT "PlayerGravityScale", which
	 * is what this property was called for most of this pass and which bound to nothing: the
	 * component's MOVEKNOB report said "MovementGravityScale FALLBACK", the game ran at 1.0 gravity,
	 * and neither the build nor the settings panel said a word. A rename here does not fail to
	 * compile. Trace.VerifyKnobs lists it, and the component's bind report names it on the other
	 * side, precisely so the pair can never drift apart again.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Air", meta = (DisplayName = "Player Gravity Scale (x world gravity)", ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.8", UIMax = "1.6"))
	float MovementGravityScale = 1.12f;

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
	 * DASH REACH = DashSpeed * DashDuration. At 3300 x 0.18 that is 594uu (v16: 3000 -> 3300). Bots' BotTrailDashRange
	 * must stay comfortably under that number or the signature trail-crossing kill stops landing —
	 * if you change this, check that one. Sane range 2200 to 3600.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "Dash Speed (uu/s)", ClampMin = "100.0", ClampMax = "10000.0", UIMin = "1000.0", UIMax = "5000.0"))
	float DashSpeed = 3300.f;

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
	 * DEMO 17 item 7, verbatim: "Add a toggle to test dash cooldown refreshing on every kill."
	 *
	 * *** AN EXPERIMENT, AND OFF IS THE SHIPPED ANSWER. *** Demo 17 asks for a way to TRY it, not for
	 * the behaviour — so this defaults to false and the game plays exactly as it did with it there.
	 * Turning it on hands the killer one dash charge back on every kill, which is the same grant
	 * (UTraceCharacterMovementComponent::RefundDashCharge) that a successful parry already makes, and
	 * therefore the same "one charge, and a full pool is not an error" semantics:
	 *
	 *     0 of 2 charges -> 1 of 2, the refill clock already running keeps running for the second;
	 *     1 of 2         -> 2 of 2, pool full, clock cleared;
	 *     2 of 2         -> nothing, and that is not a failure.
	 *
	 * *** IT IS NOW WIRED. *** UTraceAbilityComponent::NotifyKill honours it — the framework's kill
	 * notification, so it works for all ten characters from one call site and no character file knows
	 * it exists. Turning it on in the settings panel or the ini therefore takes effect.
	 *
	 * Two things the call site does, both copied from the parry's grant rather than reinvented: it is
	 * SERVER ONLY (RefundDashCharge refuses off the authority anyway, and mirrors itself to the owning
	 * client so the HUD meter moves on the same frame), and it refuses a SELF-KILL — otherwise a
	 * player farms charges off their own out-of-bounds deaths, which spec v19 §4.1 has just made a
	 * routine way to die.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "EXPERIMENT: Refresh A Dash Charge On Every Kill (Demo 17 item 7 - default OFF)"))
	bool bRefreshDashChargeOnKill = false;

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

	/**
	 * Ceiling on the UPWARD velocity a dash hands back when it ends, as a multiple of JumpZVelocity.
	 *
	 * SPEC v7 §5 made the dash a true 3D ray, so a straight-up dash now exists — and the air-strafe
	 * cap does NOT bound it, being planar by construction. Unclamped, a vertical dash was 594uu on
	 * rails PLUS 3000uu/s of exit velocity, about 4592uu of coast, straight through the arena's
	 * 1640uu ceiling. At 1.0 a dash may hand back at most one jump's worth of climb, making the
	 * straight-up total ~749uu once, then a fall, with the next dash a full cooldown away.
	 *
	 * DOWNWARD exit velocity is deliberately untouched by this: a dive is not a climb.
	 * Zero forbids a dash from adding any upward exit speed at all.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Dash", meta = (DisplayName = "Dash Exit Vertical Speed (multiple of jump)", ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0"))
	float DashExitVerticalSpeedMultiplier = 1.0f;

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

	// --- Slide is an ABILITY as of spec v5 §3 ---------------------------------------------------
	//
	// Verbatim: "Rather than making it a slide you can hold down, have it trigger once, like an
	// ability, with a hidden cooldown to prevent spamming it."
	//
	// THERE IS NO SWITCH FOR THIS AND THERE IS NO NEW KNOB. The movement component implements the
	// ability model unconditionally — a slide is committed the instant it starts and runs for
	// exactly SlideDuration — so a "one-shot ability" toggle on this page would be a slider that
	// moves nothing, which is the failure this file keeps deleting. What makes it an ability is the
	// two properties already here:
	//   * SlideDuration        — the fixed length of the ability (1.8 s). One press buys all of it.
	//   * SlideCooldownSeconds — THE HIDDEN COOLDOWN (0.8 s, the spec's starting value, measured
	//                            from the slide's END). Enforced, and deliberately NOT drawn on the
	//                            HUD: the spec asks for it to be felt, not read. Do not add a UI.
	// SlideInputBufferSeconds still applies, and still only charges on a fresh PRESS, so a held key
	// cannot chain slides.
	//
	// SlideMinCommitSeconds ("Min Commit (s, uncancellable)") WAS HERE AND IS DELETED. It described
	// the window in which releasing crouch could not cancel a slide, and a one-shot ability has no
	// partial commit — the whole slide is committed the moment it starts. Deleted at the movement
	// component's request (see the note where GetSlideMinCommitSeconds() used to be) rather than
	// left defaulted, because a property nothing reads is exactly the silently-dead knob this
	// project keeps getting caught by. Its DefaultGame.ini line went with it.

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
	 * With the gentle deceleration below, this is what actually ends most slides — so this is THE
	 * "max slide length" dial. Sane range 0.8 to 2.5.
	 *
	 * THE BASE, NOT THE SHIPPED LENGTH. Spec v9 §6's "-30%" lives in SlideMaxLengthScale below and
	 * the accumulated "-0.4 s" (v24 §8) + "-0.2 s" (v25 §6) lives in SlideDurationTrimSeconds below
	 * that; both are applied on top by the movement component:
	 *
	 *     effective duration = this x 0.7 - 0.6 = 1.26 - 0.6 = 0.66 s      (v24 shipped 0.86 s)
	 *
	 * Do not fold either of them into this number — cutting this to 1.26 ships 0.88 s, a 51% cut, and
	 * nothing on either side says so; cutting it by the trim's 0.6 ships 0.84 s, which is 0.42 s of
	 * the 0.6 s the owner asked for because the x0.7 lands on the cut as well.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Duration BASE (s, x max length scale)", ClampMin = "0.1", ClampMax = "6.0", UIMin = "0.3", UIMax = "3.0"))
	float SlideDuration = 1.8f;

	/**
	 * SPEC v9 §6 — "Reduce max slide length by 30%." x0.7, and this is the whole change.
	 *
	 * LENGTH IS SCALED THROUGH DURATION, on purpose. Since spec v4 §1 a slide's speed is purely what
	 * the player carried in, so the maximum distance one can cover is entry speed integrated over
	 * SlideDuration — scale the clock and the distance scales with it. With SlideDeceleration at
	 * 260 uu/s² the decay term is quadratic, so the distance actually falls slightly short of a clean
	 * 30%: ~1815 uu over 1.8 s becomes ~1300 uu over 1.26 s, -28.4% in distance for -30% in duration.
	 *
	 * DELIBERATELY NOT DONE BY RAISING SlideDeceleration OR CAPPING SlideMaxSpeed. Either would
	 * shorten the slide by CONFISCATING MOMENTUM the player brought in, which is the exact behaviour
	 * Demo 4 asked to have removed (spec v4 §1). Ending the slide earlier leaves them travelling at
	 * the speed they earned.
	 *
	 * Knock-on: SlideJumpWindowSeconds (0.20 s) is measured from the slide's END and does not move,
	 * so the well-timed window is now 16% of the slide rather than 11% — the slide-jump got slightly
	 * EASIER to time, not harder. (SlideDurationTrimSeconds takes 0.6 s off the product and takes
	 * that further still: 0.20 s of the shipped 0.66 s slide is 30%. The window's CLOSE is pinned to
	 * the slide's end throughout — only the share of the slide it covers moves.)
	 *
	 * NAME IS LOAD-BEARING: bound BY NAME as "SlideMaxLengthScale" by
	 * UTraceCharacterMovementComponent::GetSlideDuration(), clamped there to 0.05..4.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Max Length Scale (x duration)", ClampMin = "0.05", ClampMax = "4.0", UIMin = "0.4", UIMax = "1.5"))
	float SlideMaxLengthScale = 0.7f;

	/**
	 * Seconds taken off the SHIPPED slide, AFTER SlideMaxLengthScale. The accumulator for every
	 * "make the slide shorter" request, and the single knob that also moves the slide-jump window.
	 *
	 *     spec v24 §8: "shorten the duration of the slide by .4seconds as well"     -> 0.4
	 *     spec v25 §6: "decrease slide duration by .2seconds"                       -> 0.6
	 *
	 *     shipped duration = SlideDuration x SlideMaxLengthScale - this
	 *                      = 1.80 x 0.70 - 0.60 = 0.66 s      (v24 shipped 0.86 s, v23 1.26 s)
	 *
	 * A SEPARATE KNOB RATHER THAN AN EDIT TO SlideDuration, AND THAT IS SPEC v24 §0, NOT TIDINESS.
	 * The owner's seconds are seconds of the slide he plays — the 1.26 s product — not of the 1.80 s
	 * base. Cutting the base by 0.6 ships 1.20 x 0.70 = 0.84 s, i.e. 0.42 s of the 0.6 s he asked
	 * for. Re-typing the base as 0.9429 (0.66 / 0.70) delivers 0.66 s today and quietly becomes a
	 * different cut the first time anybody re-tunes the v9 §6 scale, because an absolute buried under
	 * a multiplier stops meaning what it was written to mean. As a trim over the finished length it
	 * stays "0.6 s shorter than the slide would otherwise be" whatever the base and the scale do next.
	 *
	 * =============================================================================================
	 * THIS KNOB IS ALSO THE WHOLE OF "MAKE THE WINDOW FOR SLIDE JUMPING .2 SECONDS EARLIER".
	 * =============================================================================================
	 *
	 * SlideJumpWindowSeconds is anchored to the slide's END, not to its start —
	 * IsSlideJumpWellTimed() asks `GetSlideTimeLeft() <= window`, never "t >= some offset". So
	 * shortening the slide by 0.2 s drags the window's OPEN 0.2 s earlier and its CLOSE 0.2 s earlier
	 * with it, and both halves of §6 are delivered by this one line:
	 *
	 *     v24 shipped   slide 0.860 s   window open 0.660 s   close 0.860 s (= the end)
	 *     v25 shipped   slide 0.660 s   window open 0.460 s   close 0.660 s (= the end)
	 *                                   ^^^^^^^^^^^^^^^^^^^   0.200 s earlier, as asked
	 *
	 * *** THE §6 ACCEPTANCE CRITERION — "the window should be right at the end of the slide" — IS
	 * SATISFIED BY CONSTRUCTION, AND THAT IS WHY IT IS SATISFIED. *** The window is DEFINED as the
	 * last N seconds of the slide, so its close and the slide's end are the same instant: offset
	 * 0.000 s, not "close to the end". It was at the end before this change and it is at the end
	 * after it, at every value this knob can take. Verify rather than believe: -TraceSlideDebug
	 * prints a V24WINDOW line off live slides with the measured open and close.
	 *
	 * DO NOT ALSO ADD 0.2 TO SlideJumpWindowSeconds. That is the reading that breaks the acceptance
	 * criterion instead of meeting it: a 0.40 s window on a 0.66 s slide opens 0.26 s in — 61% of
	 * every slide is then "well timed", which is not a window and is not "right at the end". The
	 * same trap was documented and avoided for v24's 0.4 s; the arithmetic is just starker now that
	 * the slide is shorter.
	 *
	 * Set to 0 for the pre-v24 slide. `Trace.V24LegacySlide 1` / -TraceV24LegacySlide does exactly
	 * that at runtime and is the A/B arm for this item.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide", meta = (DisplayName = "Duration Trim (s, off the scaled length)", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.0"))
	float SlideDurationTrimSeconds = 0.6f;

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
	 *
	 * *** SPEC v24 §8 DELIBERATELY DID NOT MOVE THIS, AND NEITHER DOES SPEC v25 §6. *** The reasoning
	 * belongs next to the number so that no pass "finishes the job" by adding the seconds twice.
	 *
	 * Both specs ask for the same pair: the bonus window earlier in the slide, and the slide shorter,
	 * by the same amount. BECAUSE THIS WINDOW IS ANCHORED TO THE SLIDE'S END, THE SECOND DELIVERS THE
	 * FIRST. IsSlideJumpWellTimed() asks `GetSlideTimeLeft() <= this`, so the window is defined as the
	 * slide's last N seconds and it slides earlier in lockstep with the end it hangs off:
	 *
	 *     v23        slide 1.26 s   window opens 1.06 s   closes 1.26 s   (window = 16% of the slide)
	 *     v24 §8     slide 0.86 s   window opens 0.66 s   closes 0.86 s   (-0.4 s, 23%)
	 *     v25 §6     slide 0.66 s   window opens 0.460 s  closes 0.660 s  (-0.2 s, 30%)
	 *
	 * Measured on a live pawn, not asserted: grep V24WINDOW under -TraceSlideDebug.
	 *
	 * ADDING THE SECONDS HERE AS WELL IS THE FAILURE, AND IT GETS WORSE EACH PASS BECAUSE THE SLIDE
	 * KEEPS SHRINKING. At v24 it would have opened the window 0.26 s into a 0.86 s slide (70% of it);
	 * at v25 a 0.40 s window opens 0.26 s into a 0.66 s slide — 61% of every slide "well timed". That
	 * is the double-application this file's SlideDuration note warns about, wearing the window's hat,
	 * and it also breaks v25 §6's acceptance criterion ("the window should be right at the end of the
	 * slide") in the act of pretending to satisfy it.
	 *
	 * THE ACCEPTANCE CRITERION IS ABOUT THE CLOSE, AND THE CLOSE IS THE SLIDE'S END EXACTLY — the two
	 * are the same instant by construction, at every value this knob can hold. What this knob does
	 * control is how WIDE the window is behind that end: 0.20 s of a 0.66 s slide is the final 30%.
	 * If a future pass wants the window tighter against the end than 30%, THAT is the change this
	 * number is for, and it is a separate design decision from either §6 sentence.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Timing Window (s before slide end)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.6"))
	float SlideJumpWindowSeconds = 0.20f;

	/**
	 * Extra horizontal speed multiplier for a slide-jump taken inside the timing window above.
	 *
	 * 1.10 -> 1.25 (spec v5 §3) -> 1.3125 THIS PASS (spec v8 §8).
	 *
	 * v8 §8 verbatim: "Increase momentum from slide jump by 5%". [ASSUMPTION] read as 1.25 x 1.05 =
	 * 1.3125 — a 5% increase OF the bonus, since "momentum from slide jump" names the thing the
	 * multiplier produces. THE ALTERNATIVE READING IS 1.25 + 0.05 = 1.30, five percentage points
	 * rather than five percent. The two differ by 0.0125, i.e. ~14 uu/s on a 1100 uu/s slide, which
	 * is below what a player can feel — so this is a one-line correction in the ini if they meant
	 * the other one, and nothing else in the kit has to move either way.
	 *
	 * Now that the slide is a fixed-length ability on a cooldown (bSlideIsOneShotAbility above), the
	 * timed exit is the ONLY skill expression sliding has left, so it has to be worth reaching for:
	 * 1.3125 on a ~1100 uu/s slide is ~344 uu/s, which is a third of a walk speed and is plainly a
	 * different jump rather than a nicer one.
	 *
	 * The air-strafe falloff (Movement|Air) is what stops this compounding into free arena-crossing
	 * speed: the launch speed is granted once, and everything past AirStrafeSoftCapSpeed is bought
	 * against the curve. Check the two together if chained slide-jumps start looking like a
	 * traversal exploit.
	 *
	 * THE BASE, NOT THE SHIPPED MULTIPLIER. Spec v9 §7's "+30% on the bonus" is applied on top by
	 * SlideJumpBonusScale below — effective 1 + (1.3125 - 1) x 1.50 = 1.46875 (v16: the bonus, i.e.
	 * the GAIN, raised 10%; v28 §5 takes the scale to 1.50) — so this line stays
	 * the designer's v8 number and the two re-tunings never fight.
	 *
	 * 1.0 turns the window into a no-op without disabling the move.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Well-Timed Speed Bonus BASE (x, x bonus scale)", ClampMin = "1.0", ClampMax = "2.0", UIMin = "1.0", UIMax = "1.8"))
	float SlideJumpWindowSpeedBonus = 1.3125f;

	/**
	 * SPEC v9 §7 — "Increase the bonus of timing a slide jump right by 30%." x1.30, and this plus the
	 * switch below is the whole change.
	 *
	 * Applied to SlideJumpWindowSpeedBonus by
	 * UTraceCharacterMovementComponent::GetSlideJumpWindowSpeedBonus(), bound BY NAME as
	 * "SlideJumpBonusScale" and clamped there to 0.1..4.
	 *
	 * *** SPEC v28 §5 — 1.43 -> 1.50. *** Verbatim: "change well timed bonus scale to 1.5". This is
	 * the knob that carries that name (Project Settings > Movement|Slide Jump > "Well-Timed Bonus
	 * Scale"), so the number is typed here and in Config/DefaultGame.ini and NOWHERE ELSE — in
	 * particular NOT folded into SlideJumpWindowSpeedBonus (v8 §8's base) or SlideJumpMomentumScale
	 * (v26 §3a's -20%), which carry different decisions and would silently double or delete this one.
	 *
	 * What it ships, with every other knob where it already was:
	 *     well-timed multiplier = 1 + ((1 + (1.3125 - 1) x 1.50) - 1) x 0.80 = 1.375   (was 1.3575)
	 * i.e. the gain a perfectly timed hop buys goes from +35.75% to +37.5% of the speed carried in.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Well-Timed Bonus Scale (v9 §7, x)", ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.8", UIMax = "2.0"))
	float SlideJumpBonusScale = 1.5f;

	/**
	 * WHICH READING OF §7 IS SHIPPED. The spec asks for both to be flagged and for the choice to be
	 * one number; it is this bool.
	 *
	 * TRUE (shipped, and the spec's [ASSUMPTION]): "the bonus" is the part ABOVE 1.0 — the thing the
	 * timing actually buys. 1 + (1.3125 - 1) x 1.30 = 1.40625. A well-timed hop at 1900 uu/s carries
	 * 2672 uu/s instead of 2494 uu/s.
	 *
	 * FALSE (the alternative): "the bonus" is the whole multiplier. 1.3125 x 1.30 = 1.70625, and the
	 * same hop carries 3336 uu/s — past DashSpeed's own 3300 uu/s. A slide-hop faster than a dash
	 * inverts the game's counterplay (the dash is the only answer to a carrier), which is why this
	 * reading is not the default.
	 *
	 * Bound BY NAME as "bSlideJumpBonusScalesGainOnly".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Bonus Scale Applies To The Gain Only (v9 §7 reading)"))
	bool bSlideJumpBonusScalesGainOnly = true;

	/**
	 * Extra JUMP HEIGHT multiplier for a slide-jump taken inside the timing window. New in spec v5
	 * §3, and the other half of "increase the multiplier gained by perfectly timing a jump".
	 *
	 * Height is the channel a player can actually SEE. 25% more planar speed is deniable — it looks
	 * like a good jump — whereas clearing a box you could not clear a second ago is unambiguous
	 * feedback that the timing landed. That legibility is the whole reason the window exists, so the
	 * bonus is paid in both currencies.
	 *
	 * Small on purpose: 1.12 is about a fifth of a character height on a standard jump, enough to
	 * change what geometry is reachable without turning the move into a jetpack. 1.0 makes the
	 * window purely a speed bonus again.
	 *
	 * NAME IS LOAD-BEARING: the movement component resolves it BY NAME as "SlideJumpWindowZBonus".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Well-Timed Height Bonus (x normal jump)", ClampMin = "1.0", ClampMax = "2.0", UIMin = "1.0", UIMax = "1.4"))
	float SlideJumpWindowZBonus = 1.12f;

	// ==========================================================================================
	// SPEC v26 §3 — THE SLIDE-JUMP IS 20% WEAKER, AND CHAINING IT HAS A CEILING
	//
	// Verbatim: "Reduce slide jump momentum boost by 20%. Add a ceiling to slide jump momentum
	// boosts, so that you can't chain them over and over to go faster and faster. Right now, if you
	// do three slide jump boosts in a row you can zip down the whole field. For now, lets cap it at
	// what the momentum is after you do two consecutive slide boosts. Let me change this in project
	// settings"
	//
	// FOUR KNOBS, AND THE LAST SENTENCE IS WHY THEY ARE KNOBS AT ALL. "Let me change this in project
	// settings" is a requirement, not a preference: the ceiling has to be editable HERE, in the
	// Project Settings UI, with no rebuild. Everything below is `config, EditAnywhere` and has a
	// matching line in Config/DefaultGame.ini (which wins over these header defaults — this project
	// has shipped a header-only retune that did nothing at least once).
	// ==========================================================================================

	/**
	 * SPEC v26 §3a — "Reduce slide jump momentum boost by 20%." x0.80 ON THE GAIN.
	 *
	 * *** RELATIVE, NOT REWRITTEN, AND THAT IS THE WHOLE POINT OF IT BEING A SEPARATE NUMBER. ***
	 * The obvious edit was to retype SlideJumpWindowSpeedBonus from 1.3125 to something 20% smaller,
	 * or SlideJumpBonusScale from 1.43 to 1.144. Both would have baked v26's cut into a number that
	 * belongs to a different decision (v8 §8's base, v9 §7's +30% as re-raised by v16 §0), so the
	 * next person to retune EITHER of those would silently delete or double this one. This file has
	 * been bitten by exactly that (see SlideJumpWindowSeconds' long note on the double-applied 0.2 s),
	 * so v26's cut is its own named factor, applied last, and it stays -20% whatever the base becomes.
	 *
	 * WHAT "THE BOOST" IS. The slide-jump multiplies your planar speed by
	 *
	 *     SlideJumpHorizontalRetention (1.0)  x  the well-timed bonus (1.46875)
	 *
	 * and only the part ABOVE 1.0 is a boost — 1.0 is pure preservation, which is what escaping
	 * ground friction is worth and is not something the note asks to cut. So the 20% comes off the
	 * GAIN, which is the same reading bSlideJumpBonusScalesGainOnly already ships for v9 §7:
	 *
	 *     1 + (1.46875 - 1) x 0.80 = 1.375
	 *
	 * *** THE TWO NUMBERS ABOVE WERE 1.446875 AND 1.3575 UNTIL THIS PASS, AND BOTH WERE STALE. ***
	 * They were correct against SlideJumpBonusScale 1.43; spec v28 §5 raised it to 1.50 and nothing
	 * re-derived the worked example here, so this comment claimed a shipped multiplier the game had
	 * not used for two passes. The derivation is written out in full rather than restated as a pair
	 * of results, so the next retune of SlideJumpWindowSpeedBonus (1.3125) or SlideJumpBonusScale
	 * (1.50) makes the arithmetic visibly wrong instead of quietly wrong:
	 *
	 *     1 + (1.3125 - 1) x 1.50  = 1.46875     v8 §8 base, x v9 §7's scale on the GAIN
	 *     1 + (1.46875 - 1) x 0.80 = 1.375       then v26 §3a's -20%, also on the GAIN
	 *
	 * 1.375000 is what the movement audit measures at the SLIDE-JUMP (windowed) row, and it is the
	 * number Elle's passive scales.
	 *
	 * A MISTIMED SLIDE-JUMP IS UNCHANGED, and that follows rather than being a carve-out: at
	 * retention 1.0 it has no gain, so 80% of nothing is nothing. The 20% is paid entirely by the
	 * well-timed hop, which is the only place the slide-jump manufactures speed at all.
	 *
	 * Applied by UTraceCharacterMovementComponent::GetSlideJumpWindowSpeedBonus(), BEFORE the
	 * character seam — so Elle's passive (+30% as of Patch 28 §3) scales the reduced gain rather than
	 * fighting it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Momentum Boost Scale (v26 §3, x the GAIN)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.5", UIMax = "1.5"))
	float SlideJumpMomentumScale = 0.80f;

	/**
	 * SPEC v26 §3b — the master switch for the chain ceiling. OFF restores the uncapped compounding.
	 *
	 * Exists for the same reason bSlideJumpEnabled does: "is chaining still a traversal exploit"
	 * has to be answerable from one binary without a rebuild. It is ALSO the designer-facing half of
	 * the A/B arm — the dev-only console arm is Trace.V26LegacySlideJump, which reverts the -20% as
	 * well, whereas this reverts the ceiling alone.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Chain Ceiling Enabled (v26 §3)"))
	bool bSlideJumpChainCapEnabled = true;

	/**
	 * *** THE KNOB THE NOTE ASKS FOR. *** "For now, lets cap it at what the momentum is after you do
	 * two consecutive slide boosts." This is the TWO.
	 *
	 * HOW IT IS SPENT. A CHAIN is a run of slide-jumps taken without ever giving the momentum back
	 * (see the reset multiplier below for what "giving it back" means). The component watches the
	 * launch speed of the chain's first N boosts, keeps the highest, and every boost after that is
	 * clamped to it. So at 2:
	 *
	 *     boost 1   launches normally
	 *     boost 2   launches normally; the chain's ceiling is now the faster of the two
	 *     boost 3   still fires — still ends the slide, still escapes ground friction, still pays the
	 *               well-timed HEIGHT bonus — but its planar speed may not exceed that ceiling
	 *     boost 4+  the same
	 *
	 * which is the note's "a third and fourth may still be performed, they just must not go faster",
	 * exactly.
	 *
	 * *** THE CEILING IS MEASURED, NOT COMPUTED, AND THAT IS DELIBERATE. *** The tempting version is
	 * a formula — entry speed x multiplier^2 — and it is WRONG here, because a slide DECAYS while
	 * you wait for the well-timed window (SlideDeceleration, 260 uu/s²). The speed you actually have
	 * after two boosts is therefore a function of how long each of your slides ran, which no formula
	 * on this page knows. Recording the chain's own launch speed answers the note's sentence
	 * literally instead of approximating it, and it is RELATIVE by construction: it moves with the
	 * boost knobs, with a character's passive, and with whatever speed you brought into the chain,
	 * because it IS one of those launches.
	 *
	 * 1 caps a chain at its first boost. Raise it to 3 to allow one more compounding step.
	 *
	 * *** SPEC v28 §5 TOOK EXACTLY THAT STEP: 2 -> 3. *** Verbatim: "Change consecutive chain ceiling
	 * slide boosts to 3". So the ceiling is now the highest of the chain's FIRST THREE launches, and
	 * it is boost 4 — not boost 3 — that is the first one clamped:
	 *
	 *     boost 1, 2, 3   launch normally; the ceiling is the fastest of the three
	 *     boost 4+        still fires, still ends the slide, still pays the well-timed HEIGHT bonus,
	 *                     but its planar speed may not exceed that ceiling
	 *
	 * NOTHING ELSE MOVES WITH IT, and that is the point of the cap being a count rather than a speed:
	 * the ceiling is still one of the chain's own measured launches, so raising the count changes WHICH
	 * launch it is and never what a launch is worth. Anything that compares hops (the audit's verdict
	 * rows in TraceMovementAuditV16.cpp) reads this knob rather than a typed 2 — see the note there.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Chain Ceiling (consecutive boosts before the cap)", ClampMin = "1", ClampMax = "8", UIMin = "1", UIMax = "4"))
	int32 SlideJumpChainCapBoosts = 3;

	/**
	 * When a chain ENDS, as a multiple of the pawn's own live max ground speed.
	 *
	 * *** RELATIVE TO A BASE THAT MOVES. *** Not "reset below 800 uu/s". The pawn's ground speed
	 * ceiling is WalkSpeed folded through CarrierSpeedMultiplier (1.22), the knife profile (1.30) and
	 * every ability speed passive, so a carrier's baseline is 976 and a knife-carrier's is higher
	 * still. Written as a multiple, this threshold follows every one of those without being told, and
	 * retuning WalkSpeed cannot silently make the chain immortal (or unstartable).
	 *
	 * WHY THE RESET IS A SPEED AND NOT A TIMER. Two reasons, and the second is the load-bearing one:
	 *
	 *   1. It is what "consecutive" MEANS. Boosts are consecutive while you never gave the momentum
	 *      back; the instant you are back to running pace the next slide-jump starts from scratch and
	 *      has nothing to compound.
	 *   2. A timer inside a client-predicted move is a prediction hazard. The movement component's
	 *      state is replayed frame-by-frame after a server correction, and a rule keyed on world time
	 *      would resolve differently on the replay than it did live. A rule keyed on the pawn's own
	 *      planar speed is a pure function of state the saved move already carries.
	 *
	 * The check is only made while the pawn is ON ITS FEET — grounded, not sliding, not dashing —
	 * because during a slide or a dash the planar speed is that ability's number and not the player's.
	 *
	 * Above 1.0 makes the chain harder to break (it survives a slower jog); below 1.0 makes it
	 * easier, and at 0 the chain only ever ends at a dead stop.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Slide Jump", meta = (DisplayName = "Chain Reset Speed (x the pawn's max ground speed)", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.5", UIMax = "1.5"))
	float SlideJumpChainResetSpeedMultiplier = 1.0f;

	// ==========================================================================================
	// MOVEMENT — LEDGES  (spec v12 §5. WAS "MANTLE", spec v5 §7 — THE MANTLE IS GONE)
	//
	// v12 verbatim: "Remove mantling from the game, keep wall jumping. Make sure there's no bug when
	// a player hits the top edge of an obstacle."
	//
	// NINE KNOBS WERE DELETED HERE, NOT DEPRECATED: bMantleEnabled, MantleReachUU,
	// MantleMinHeightUU, MantleMaxHeightUU, MantleDurationSeconds, MantleUpPhaseFraction,
	// MantleCooldownSeconds, MantleMinForwardSpeed, and (in the wall-jump block below)
	// WallJumpMantleLockoutSeconds. Nothing reads them any more. They are removed from
	// Trace.VerifyKnobs, from Trace.DumpSettings and from Config/DefaultGame.ini in the same change,
	// because a knob that survives the code that read it is the "slider that moves nothing" failure
	// this project has already shipped whole families of — and a mantle knob left on the panel after
	// the mantle is gone is worse than a typo, it advertises a feature that does not exist.
	//
	// WHY LedgeGroundGraceSeconds STAYS, AND WHY IT IS NOW THE WHOLE SECTION. The v5 mantle was
	// added as the fix for "when jumping on the edge of a raised section, it's glitchy and feels like
	// rubber banding". A mantle does not fix a prediction desync, it HIDES one — the disagreement was
	// still there on every ledge the mantle did not trigger on, and v12 asks for the underlying bug
	// outright ("make sure there's no bug when a player hits the top edge"). Deleting the mantle
	// hands the original complaint straight back unless the desync half is handled, and this knob IS
	// that half. It was never a mantle setting; it only lived in the mantle block because that is the
	// pass that added it.
	// ==========================================================================================

	/**
	 * Seconds of grace during which a pawn that has just lost ground contact is still treated as
	 * grounded. THE LEDGE-EDGE FIX (spec v5 §7's second half, and spec v12 §5's actual request).
	 *
	 * "Feels like rubber banding" at a ledge is a one- or two-frame contact blip as the capsule
	 * crosses a lip: the client and the server disagree about whether the pawn is walking or falling
	 * for those frames, and the correction that follows is the rubber band. This swallows the blip
	 * without swallowing anything a player would notice. 0.08 s is about five frames at 60 Hz — far
	 * too short to let a pawn run off a roof and keep its footing.
	 *
	 * WITH THE MANTLE REMOVED THIS IS THE ONLY THING STANDING BETWEEN THE PLAYER AND THE DEMO 5
	 * COMPLAINT, so do not treat it as a leftover. 0 restores the exact Demo 5 behaviour, which is
	 * what the desync was measured against, so keep it as an A/B rather than deleting it.
	 * Sane range 0.05 to 0.12.
	 *
	 * NAME IS LOAD-BEARING: resolved BY NAME as "LedgeGroundGraceSeconds" by
	 * UTraceCharacterMovementComponent through TraceMoveKnob. A rename here does not fail to
	 * compile — it silently reverts to the component's own literal. Trace.VerifyKnobs lists it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Ledges", meta = (DisplayName = "Ledge Ground Grace (s) [v12 §5]", ClampMin = "0.0", ClampMax = "0.5", UIMin = "0.0", UIMax = "0.2"))
	float LedgeGroundGraceSeconds = 0.08f;

	// ==========================================================================================
	// MOVEMENT — WALL JUMP  (spec v8 §7, new)
	//
	// "Can you add a wall jump mechanic, where players can press jump right as they hit a wall to
	// carry momentum in a new direction?"
	//
	// ALL SEVEN ARE RESOLVED BY NAME by UTraceCharacterMovementComponent (TraceMoveKnob), so the
	// spellings below are load-bearing — a rename here silently reverts the mechanic to the built-in
	// fallback rather than failing to compile. BeginPlay's MOVEKNOB report prints BOUND or FALLBACK
	// for each one every run, and Trace.VerifyKnobs lists them in the table.
	// ==========================================================================================

	/**
	 * Master switch for the wall jump. Off restores the pre-v8 air game exactly: DoJump() refuses
	 * every mid-air press and JumpMaxCount stays at 1.
	 *
	 * It exists because "is the wall jump making the arena feel worse" has to be answerable with one
	 * ini edit rather than a rebuild — this is a brand-new traversal verb in a map that was tuned
	 * without it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Wall Jump Enabled"))
	bool bWallJumpEnabled = true;

	/**
	 * Seconds after touching a wall in which a jump press still counts as a wall jump.
	 *
	 * "Press jump RIGHT AS they hit a wall" is the request, so this is a reaction window and not a
	 * wall-cling. 0.25 s is about four frames of slack at 60 Hz plus the human reaction floor.
	 *
	 * THE BASE, NOT THE SHIPPED WINDOW. Spec v9 §5's shortening lives in WallJumpWindowScale below:
	 * effective window = 0.25 x 0.6 = 0.15 s. Cutting this to 0.15 as well ships 0.09 s, which is
	 * shorter than a 40 ms client's own latency — the move would simply stop existing online.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Contact Window BASE (s, x window scale)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.05", UIMax = "0.4"))
	float WallJumpWindowSeconds = 0.25f;

	/**
	 * SPEC v9 §5 — "Make the window of time for performing a wall jump shorter and make the action
	 * happen faster." x0.6 on the contact window: 0.25 s -> 0.15 s.
	 *
	 * The "sticking to the wall for a second" the note describes IS this window — the time between
	 * contact and launch during which the pawn is committed to a wall it has not left yet.
	 * [ASSUMPTION] a 40% cut: still ~4 frames wider than a 40 ms client's own latency (spec v8 §0), so
	 * the move stays performable online. Do not take the effective window below ~0.10 s.
	 *
	 * IT USED TO SHORTEN THE MANTLE DEFERRAL AS WELL — the component reused this scaled window when
	 * deciding how long an auto-mantle should stand aside for a live wall-jump opportunity. Spec v12
	 * §5 removed the mantle, so that second effect is gone and this knob now does exactly one thing.
	 *
	 * Bound BY NAME as "WallJumpWindowScale", clamped in the component to 0.05..1 — it can only ever
	 * shorten the window, never lengthen it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Contact Window Scale (v9 §5, x)", ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.3", UIMax = "1.0"))
	float WallJumpWindowScale = 0.6f;

	/**
	 * Fraction of the incoming planar SPEED the reflected launch keeps. 1.0 is pure preservation.
	 *
	 * "Carry momentum in a new direction" means the speed survives the bounce and only the direction
	 * changes, so this sits near 1 by design: 0.95 charges a small toll so a corridor of walls is not
	 * a frictionless pinball table.
	 *
	 * THE BASE, NOT THE SHIPPED RETENTION. Spec v9 §5's -10% lives in WallJumpMomentumScale below:
	 * effective retention = 0.95 x 0.9 = 0.855. Setting this to 0.855 as well ships 0.7695, which is
	 * below the ~0.8 floor at which the move stops reading as carrying momentum at all.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Speed Retention BASE (x, x momentum scale)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.7", UIMax = "1.0"))
	float WallJumpSpeedRetention = 0.95f;

	/**
	 * SPEC v9 §5 — "Reduce momentum gained from wall jumping by 10%." x0.9 on the retention, which is
	 * the spec's own [ASSUMPTION] stated verbatim ("scale the retention knob by 0.9").
	 *
	 * WHY THE KNOB AND NOT THE OUTCOME. Measured end-to-end retention (launch speed / entry speed) was
	 * ~104.6-105.4%, because the outward impulse and the vertical add on top of the 0.95 — the move
	 * was a net GAIN. Reading "the momentum gained" as only the part above 100% would make the cut
	 * 1.050 -> 1.045, which is invisible. Scaling the knob lands measured retention near ~95%, which
	 * is the change a player will actually feel.
	 *
	 * TO SWITCH READINGS: set this to 1.0 and cut WallJumpOutwardImpulse instead — it is the only
	 * other term in the launch.
	 *
	 * Bound BY NAME as "WallJumpMomentumScale", clamped in the component to 0.1..1 so a wall can never
	 * MANUFACTURE speed (the same rule spec v4 §1 imposed on the slide).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Momentum Scale (v9 §5, x retention)", ClampMin = "0.1", ClampMax = "1.0", UIMin = "0.7", UIMax = "1.0"))
	float WallJumpMomentumScale = 0.9f;

	// SPEC v12 §5 — WallJumpMantleLockoutSeconds (v9 §5, "if a player inputs a wall jump, that
	// overrides a mantle") IS DELETED. It existed for one purpose: to stop the mantle vacuuming a
	// player back onto the lip they had just wall-jumped away from. With the mantle gone there is
	// nothing to hold off, so the knob is removed rather than left at 0 — a knob whose only reader
	// has been deleted is a dead slider, and this page has shipped those before.
	//
	// THE WALL JUMP ITSELF IS UNCHANGED AND MUST STAY THAT WAY: v12 §5 is "remove mantling, KEEP wall
	// jumping". The priority code that made a wall jump beat a mantle goes away with the mantle, so
	// the thing to verify after this removal is that the wall jump still fires cleanly at a wall on
	// its own — it no longer has a competitor to win against, and it must not have lost its trigger
	// along with it.

	/**
	 * SPEC v10 §5 — the SECOND -10% on wall-jump momentum. Verbatim: "Reduce the momentum boost from
	 * wall jumping by 10%".
	 *
	 * A THIRD MULTIPLIER RATHER THAN AN EDIT TO THE OTHER TWO, and that is the same rule spec v9 §§5-8
	 * set for itself: a pass's change is its own named scalar over the designer's base, so reverting
	 * one pass is one line and two passes never fight over the same number. Shipped retention is
	 * therefore WallJumpSpeedRetention x WallJumpMomentumScale x this = 0.95 x 0.90 x 0.90 = 0.7695.
	 *
	 * READ THAT NUMBER BEFORE TUNING IT FURTHER. 0.7695 is below the ~0.8 floor the v9 comment above
	 * names as the point where the move stops reading as carrying momentum at all — v10 asked for the
	 * cut anyway, and it is exactly what "reduce the boost" means twice in a row, so it ships. If a
	 * playtest says a wall jump now feels like a stop, this is the knob to raise, not the other two.
	 *
	 * Bound BY NAME as "WallJumpMomentumScaleV10" by UTraceCharacterMovementComponent and clamped
	 * there to 0.1..1, so it can never MANUFACTURE speed. A misspelling here does not fail to compile
	 * — the component silently falls back to its own 0.90 literal and the ini stops driving it, which
	 * is precisely what Trace.VerifyKnobs exists to catch. Trace.V10LegacyWallJump 1 is the A/B arm.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Momentum Scale (v10 §5, x v9 scale)", ClampMin = "0.1", ClampMax = "1.0", UIMin = "0.7", UIMax = "1.0"))
	float WallJumpMomentumScaleV10 = 0.90f;

	/**
	 * SPEC v10 §5, THE STICKINESS ITSELF — seconds after a wall-jump launch during which the player's
	 * held movement input cannot steer the pawn back into the wall it just left.
	 *
	 * THIS IS THE REPEAT COMPLAINT'S ACTUAL CAUSE, and it is why v10 does not simply shave the input
	 * window again the way v9 did. A wall jump is nearly always performed while HOLDING the stick or
	 * the key toward the wall — that is how you got to the wall. The launch reflects the velocity
	 * outward correctly, and then, on the very next move tick, the still-held toward-the-wall input
	 * is added back as acceleration and drags the pawn into the face it just left. The pawn is
	 * genuinely airborne and genuinely moving away; it just does not LOOK like it, because the
	 * separation for the first fraction of a second is a few uu. "Sticking to the wall for a moment
	 * too long" is exactly what that produces, and no amount of trimming the INPUT window touches it.
	 *
	 * 0.20 s is long enough to clear the face at any launch speed the kit produces and short enough
	 * that a player never notices having lost steering — they are watching the launch, not steering
	 * during it. 0 restores the v9 behaviour exactly, which is the A/B arm.
	 *
	 * Bound BY NAME as "WallJumpControlLockoutSeconds", clamped in the component to 0..1.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Control Lockout After Launch (s) [v10 §5]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5"))
	float WallJumpControlLockoutSeconds = 0.20f;

	/**
	 * SPEC v10 §5 — seconds a jump press is remembered and re-offered to the wall-jump test.
	 *
	 * The other half of "it feels sticky". v9 cut the CONTACT window to 0.15 s, which made the wall
	 * jump harder to buy AND did nothing for the feel, because a press that lands a frame or two
	 * BEFORE contact was simply thrown away — the player pressed jump, nothing happened, and they
	 * were still against the wall when it did not. A buffer converts that miss into a launch on the
	 * first frame contact is legal, which is the difference between "I hit it late" and "the wall
	 * held me".
	 *
	 * 0.12 s is a shade over seven frames at 60 Hz, the standard fighting-game buffer length, and is
	 * deliberately SHORTER than the contact window so a buffered press can never outlive the contact
	 * it was meant for. 0 disables buffering.
	 *
	 * Bound BY NAME as "WallJumpInputBufferSeconds", clamped in the component to 0..0.5.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Jump Input Buffer (s) [v10 §5]", ClampMin = "0.0", ClampMax = "0.5", UIMin = "0.0", UIMax = "0.3"))
	float WallJumpInputBufferSeconds = 0.12f;

	// ==========================================================================================
	// MOVEMENT — SURF  (Patch 28 §5, new)
	//
	// "Players should be able to accelerate using curved ramps, in accordance with source movement
	// standards (kind of like surfing in CS:GO)."
	//
	// WHY THESE FIVE EXIST AS A LATER EDIT THAN THE MECHANIC THEY TUNE, stated plainly because it is
	// the failure this page has shipped more than any other. The surf pass could not put its
	// UPROPERTYs here — this file was not on its ownership line — so the whole feature went out
	// name-bound against properties that did not exist, and the movement component's own report said
	// so out loud on every run:
	//
	//     MOVEKNOB summary: 27 bound, 5 on built-in defaults
	//
	// The game PLAYED correctly the entire time (every accessor falls back to the shipped literal),
	// but no ini line and no Project Settings slider could reach any part of the newest movement
	// mechanic in the kit. These five properties are what closes that, and the summary line above is
	// the acceptance test for them: it must read 32 bound, 0 on built-in defaults.
	//
	// ALL FIVE ARE RESOLVED BY NAME by UTraceCharacterMovementComponent (TraceMoveKnob), exactly like
	// the wall-jump block above, so the spellings below are load-bearing — a rename here silently
	// reverts surf to the component's own literals rather than failing to compile. BeginPlay prints
	// BOUND or FALLBACK for each one every run, MOVECFG-P28 prints the resolved band and ceiling next
	// to the numbers they derive from, and Trace.VerifyKnobs lists all five in its table.
	//
	// EVERY ACCESSOR CLAMPS ON READ, so nothing in this block can break the movement model. The
	// clamps are stated on each knob and are the component's, not this page's: the ClampMin/ClampMax
	// meta below only bounds the Project Settings spinner, and an ini file edited by hand does not go
	// near it. The one that matters most is SurfMinNormalZ, which is clamped strictly BELOW the live
	// walkable limit — so no value typed here can turn a floor into a surf plane.
	// ==========================================================================================

	/**
	 * Master switch for surf. Off restores the pre-Patch-28 air game exactly: ComputeSlideVector,
	 * LimitAirControl and CanStepUp all defer to the engine on every surface, and a steep ramp goes
	 * back to being the scrape UE ships.
	 *
	 * It exists for the reason bWallJumpEnabled does, and rather more urgently: surf is a brand-new
	 * traversal verb in an arena that was laid out, timed and bot-tested without it, and "is the surf
	 * making this map worse" has to be answerable with one ini edit rather than a rebuild.
	 *
	 * NOTE FOR ANYONE TESTING THIS ON THE SHIPPED MAP. ServerDefaultMap is /Game/Maps/Arena_Baked,
	 * and the four surf rails are built by ATraceArenaBuilder::BuildSurfRails(), which a baked level
	 * skips. Until somebody bakes again there is nothing in the DEFAULT map to ride, so turning this
	 * off there changes nothing you can see — launch /Game/Maps/Arena to A/B the mechanic.
	 *
	 * Bound BY NAME as "bSurfEnabled".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Surf", meta = (DisplayName = "Surf Enabled [Patch 28 §5]"))
	bool bSurfEnabled = true;

	/**
	 * FLOOR OF THE SURF BAND, as a surface normal Z. A face is surfable when
	 *
	 *     this  <  Normal.Z  <  GetWalkableFloorZ()
	 *
	 * i.e. 0.45 .. 0.710 at the shipped walkable limit, which is slopes of 44.765° .. 63.256°.
	 *
	 * *** ONLY THE LOWER BOUND IS A KNOB, AND THAT IS THE POINT. *** The upper bound is
	 * GetWalkableFloorZ() read LIVE — the same number the engine uses to decide the pawn is standing
	 * — so a walkable face can never be surfed by construction rather than by agreement, and retuning
	 * the walkable limit moves the surf band with it. There is no second literal to forget.
	 *
	 * IT IS DELIBERATELY ABOVE GetWallJumpMaxNormalZ() (0.40). A face is a wall, a surf plane or a
	 * floor and never two at once, which matters because the wall jump WANTS the engine's
	 * HandleSlopeBoosting left on (it is what stops a player climbing a corner) and surf needs it
	 * off. Lowering this into the wall band is the one edit on this page that could make the two
	 * mechanics argue; the component's clamp does not stop it, because 0.05 is a legitimate value for
	 * a build that has retuned the wall jump too.
	 *
	 * THE ARENA'S RAILS ARE CUT FROM THIS NUMBER, not typed: BuildSurfRails() asks
	 * GetSurfSlopeBandDegrees() what the build will surf and cuts its five facets inside the answer
	 * with a 2° margin. Retune this and the ramps re-cut themselves on the next build of the
	 * procedural map — a builder that had typed "46 to 61 degrees" would be one retune away from
	 * shipping a ramp nobody can surf, with nothing to say so.
	 *
	 * Bound BY NAME as "SurfMinNormalZ", clamped in the component to 0.05 .. (WalkableFloorZ - 0.01),
	 * so a bad value can only ever make the band NARROWER.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Surf", meta = (DisplayName = "Surf Band FLOOR (surface normal Z; the ceiling is the live walkable limit) [Patch 28 §5]", ClampMin = "0.05", ClampMax = "0.9", UIMin = "0.3", UIMax = "0.7"))
	float SurfMinNormalZ = 0.45f;

	/**
	 * Source's PM_ClipVelocity overbounce, applied when velocity is clipped against a surf plane:
	 *
	 *     v' = v - n * (v·n) * this
	 *
	 * 1.0 is Source's own player value and makes the clip a PURE PLANE PROJECTION — the pawn is
	 * neither pushed off the ramp nor allowed to sink into it, which is exactly the behaviour that
	 * lets gravity's along-plane component do all the work and the normal force do none.
	 *
	 * Above 1 the ramp bounces the player off it (Source uses 1.001 for world brushes to keep solvers
	 * out of surfaces, and much more than that is a trampoline); below 1 the pawn sinks in and the
	 * next sweep has to dig it out, which reads as a ramp made of glue. Neither is a tuning direction
	 * anybody wants — this knob exists so the Source constant is visible and adjustable, not because
	 * it is expected to move.
	 *
	 * Bound BY NAME as "SurfOverbounce", clamped in the component to 0.9 .. 1.2.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Surf", meta = (DisplayName = "Surf Overbounce (1.0 = Source's pure plane projection) [Patch 28 §5]", ClampMin = "0.9", ClampMax = "1.2", UIMin = "0.95", UIMax = "1.05"))
	float SurfOverbounce = 1.0f;

	/**
	 * How long "I am surfing" survives after the last contact with a surf plane, in seconds.
	 *
	 * IT IS A JOINT-CROSSER, NOT A COYOTE TIME. A curved rail is a FAN OF FLAT FACETS, and a pawn
	 * crossing from one facet to the next genuinely leaves the surface for a frame or two. Without a
	 * grace the surf state would flicker off and on at every joint — and the speed ceiling, which
	 * hangs off IsSurfing(), would flicker with it, so a rider would be capped and uncapped several
	 * times per ride.
	 *
	 * 0.10 s is about six frames at 60 Hz: long enough to bridge a facet joint, far short of the time
	 * it takes to fall clear of a ramp, and therefore not long enough to be a mechanic of its own.
	 * Raising it materially would start granting the surf ceiling to players who have already left
	 * the ramp.
	 *
	 * PREDICTED STATE, not a local timer: the remaining grace is one of the five surf fields carried
	 * in the saved move. It has to be — a replayed frame that lost it runs UNCAPPED where the server
	 * capped.
	 *
	 * Bound BY NAME as "SurfContactGraceSeconds", clamped in the component to 0 .. 0.5.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Surf", meta = (DisplayName = "Surf Contact Grace (s, bridges a facet joint) [Patch 28 §5]", ClampMin = "0.0", ClampMax = "0.5", UIMin = "0.0", UIMax = "0.25"))
	float SurfContactGraceSeconds = 0.10f;

	/**
	 * The surf speed ceiling, as a MULTIPLE of the air-strafe hard cap:
	 *
	 *     GetSurfSpeedCeiling() = max(entry speed, GetAirStrafeHardCapSpeed() x this)
	 *
	 * At the shipped 1375 uu/s hard cap that is 1719 uu/s with a gun, and 2160 uu/s with a knife out
	 * (GetAirStrafeHardCapSpeed() already folds in KnifeAirStrafeHardCapMultiplier, so a knife surfer
	 * gets the same bonus the rest of their kit gets, automatically).
	 *
	 * *** IT IS A MULTIPLIER AND NOT A SPEED, WHICH IS THE PROJECT'S DEMO 21 RULE. *** Retune the air
	 * ceiling and the surf ceiling moves with it; there is no second uu/s number on this page that
	 * can be left behind. That was proved live rather than asserted — forcing the air hard cap down
	 * to 1100 in an ini override moved the measured surf peaks with it, with no edit here.
	 *
	 * THERE HAS TO BE A CEILING AT ALL because gravity along the plane is a CONSTANT acceleration
	 * that the clip never removes: integrated on the steepest shipped facet with no friction, an
	 * unclamped surfer passes 29,000 uu/s in 30 s and is still gaining ~980 uu/s every second. A ramp
	 * long enough would grow the vector without limit, so "fastest thing in the kit" has to be a
	 * number rather than an asymptote.
	 *
	 * THE ENTRY SPEED IS THE FLOOR, exactly like every other ceiling in the movement component: a
	 * player who arrives on a rail above the cap (out of a dash, or off a slide-jump chain) keeps
	 * every unit of what they brought. The ceiling only ever refuses to let the RAMP make more.
	 * Clamped at the BOTTOM to 1 for that reason — a value under 1 would make a surf plane a speed
	 * PENALTY for a fast entry, which inverts the mechanic silently rather than loudly.
	 *
	 * Bound BY NAME as "SurfSpeedCeilingMultiplier", clamped in the component to 1 .. 3.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Surf", meta = (DisplayName = "Surf Speed Ceiling (x the air-strafe HARD cap; entry speed is the floor) [Patch 28 §5]", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float SurfSpeedCeilingMultiplier = 1.25f;

	// ==========================================================================================
	// KNIFE MOVEMENT  (spec v10 §1)
	//
	// Verbatim: "Players should move 30% faster with a knife, as well as have a higher momentum
	// ceiling."
	//
	// WHY THESE THREE LIVE HERE AND THE REST OF THE KNIFE DOES NOT. Damage, angles, cooldowns, swap
	// time and reach are the WEAPON, and they live in UTraceMeleeSettings (Gameplay/TraceMelee.h) —
	// their own Project Settings page, their own ini section, edited by whoever is tuning the knife.
	// These three are the MOVEMENT KIT: they are read by UTraceCharacterMovementComponent through
	// TraceMoveKnob alongside every other movement scalar, they multiply values (WalkSpeed,
	// AirStrafeSoftCapSpeed, AirStrafeHardCapSpeed) that live on this page, and a mobility number
	// tuned two files away from the mobility it modifies is how a base and its multiplier end up
	// fighting. Same reasoning as the v9 §§5-8 scalars above.
	// ==========================================================================================

	/**
	 * Ground speed multiplier while the knife is out. Spec v12 §3: 1.30 -> 1.22.
	 *
	 * Verbatim (v12 §3): "Reduce max speed with the knife from the previous 30% increase to 22% and
	 * adjust momentum accordingly." v10 §1 asked for the 30% ("Players should move 30% faster with a
	 * knife, as well as have a higher momentum ceiling"); v12 walks it back to 22%.
	 *
	 * x WalkSpeed, so the knife is a MOBILITY CHOICE and not merely a weapon — which is the
	 * interesting half of the design and the reason bots are told to swap to it to close distance.
	 *
	 * *** PARITY WITH THE CARRIER IS RESTORED (spec v13 §3, "update carrier speed to match the new
	 * *** knife speed"). v12 §3 moved only the knife and left CarrierSpeedMultiplier at 1.30, which
	 * *** made the CARRIER the faster of the two for one pass; the user has since called it, and
	 * *** CarrierSpeedMultiplier is now 1.22 as well. THE TWO MUST MOVE TOGETHER: if this number is
	 * *** retuned again, retune CarrierSpeedMultiplier with it or the same break comes back. They do
	 * *** not stack — a carrier's knife is stowed, see ShouldUseKnifeMovementProfile.
	 *
	 * Bound BY NAME as "KnifeMoveSpeedMultiplier", clamped in the component to 1..3. It is clamped at
	 * the BOTTOM as well as the top on purpose: a value under 1 would make the knife a mobility
	 * PENALTY, which inverts the mechanic silently rather than loudly.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Knife", meta = (DisplayName = "Knife Ground Speed (x walk) [v12 §3]", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float KnifeMoveSpeedMultiplier = 1.22f;

	/**
	 * The "higher momentum ceiling", soft half. x AirStrafeSoftCapSpeed while the knife is out.
	 *
	 * TWO CEILING KNOBS, NOT ONE, and they are deliberately different numbers. The soft cap is where
	 * air-strafe gain starts falling off and the hard cap is where it stops; raising both by the same
	 * factor would move the ceiling without widening the band a skilled player actually plays in.
	 * The pair raises the ceiling AND opens the band, which is what "a higher momentum ceiling"
	 * buys a player who can already air-strafe.
	 *
	 * SPEC v12 §3, "adjust momentum accordingly": 1.25 -> 1.1833. THE BONUS IS SCALED, NOT THE
	 * MULTIPLIER. Scaling the multiplier itself (1.25 x 22/30 = 0.917) would put the knife BELOW the
	 * gun's own air cap, i.e. turn the mobility kit into a penalty. The bonus is the part above 1:
	 * 1 + 0.25 x (22/30) = 1.18333. Shipped soft cap with a knife = AirStrafeSoftCapSpeed 1045 x
	 * 1.18333 = ~1237 uu/s (was ~1306).
	 *
	 * Tunable separately from the base caps, as spec v10 §1 requires. Bound BY NAME as
	 * "KnifeAirStrafeSoftCapMultiplier", clamped in the component to 1..3.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Knife", meta = (DisplayName = "Knife Air Soft Cap (x base) [v12 §3]", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float KnifeAirStrafeSoftCapMultiplier = 1.183333f;

	/**
	 * The "higher momentum ceiling", hard half. x AirStrafeHardCapSpeed while the knife is out.
	 *
	 * KEEP THIS AT OR ABOVE KnifeAirStrafeSoftCapMultiplier. The component clamps each independently
	 * and does not cross-check them, so a hard multiplier below the soft one would put the hard cap
	 * under the soft cap and air-strafe gain would be cut off before the falloff ever began — the
	 * knife would feel SLOWER in the air than the gun, which is the opposite of the request. The
	 * v12 rescale below preserves that ordering because it scales both BONUSES by the same 22/30,
	 * and 0.35 > 0.25 before the scale means 0.2567 > 0.1833 after it.
	 *
	 * SPEC v12 §3, "adjust momentum accordingly": 1.35 -> 1.2567, i.e. 1 + 0.35 x (22/30). Shipped
	 * hard cap with a knife = AirStrafeHardCapSpeed 1375 x 1.25667 = ~1728 uu/s (was ~1856).
	 *
	 * Bound BY NAME as "KnifeAirStrafeHardCapMultiplier", clamped in the component to 1..3.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Knife", meta = (DisplayName = "Knife Air Hard Cap (x base) [v12 §3]", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float KnifeAirStrafeHardCapMultiplier = 1.256667f;

	/**
	 * Flat uu/s pushed straight out along the wall normal, on top of the reflection.
	 *
	 * Without it a player who slides ALONG a wall reflects almost nothing (their velocity is nearly
	 * parallel to the face) and the wall jump does visibly nothing. This is the floor that makes a
	 * glancing wall jump still a wall jump.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Outward Impulse (uu/s)", ClampMin = "0.0", ClampMax = "2000.0", UIMin = "150.0", UIMax = "800.0"))
	float WallJumpOutwardImpulse = 360.f;

	/**
	 * Vertical launch, as a MULTIPLE OF JumpZVelocity — the unit every other launch in this kit uses,
	 * so it tracks the jump automatically if the jump is ever retuned.
	 *
	 * Slightly over 1 so a wall jump clears a little more than a standing jump and is legible as its
	 * own move. Combined with the consecutive cap below, 1.05 cannot climb a single flat wall.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Vertical (x jump)", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.7", UIMax = "1.3"))
	float WallJumpVerticalMultiplier = 1.05f;

	/**
	 * Consecutive wall jumps allowed without touching the ground. THE ANTI-LADDER CAP, and spec v8 §7
	 * asks for it by name: "cap consecutive jumps without touching ground, or two close walls become
	 * an infinite ladder".
	 *
	 * 2 buys the one thing the move is for — a wall, then a second wall, then you are somewhere new —
	 * and refuses the third, which is where a corridor becomes a staircase to the skybox. Raising it
	 * is the single most likely way to break the arena's sightlines, so raise it deliberately.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Max Consecutive (no ground)", ClampMin = "1", ClampMax = "4", UIMin = "1", UIMax = "3"))
	int32 WallJumpMaxConsecutive = 2;

	/**
	 * Largest |Normal.Z| still counted as a wall. Above it the surface is a ramp, not a face.
	 *
	 * GetWalkableFloorZ() is 0.71 at the default 45-degree slope limit, so 0.4 leaves a clear band
	 * between "wall" and "slope you could simply have walked up" — without it a shallow ramp becomes
	 * a trampoline and the arena's approach geometry starts launching people.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Movement|Wall Jump", meta = (DisplayName = "Max Wall Normal Z", ClampMin = "0.0", ClampMax = "0.9", UIMin = "0.1", UIMax = "0.6"))
	float WallJumpMaxNormalZ = 0.4f;

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
	 * 1.0 -> 0.4 (spec v3 §1) -> 0.5 (spec v4 §5, which asks for exactly 0.4 x 1.25) -> 0.75 THIS
	 * PASS (spec v10 §3). The grace exists so a turnover does not instantly wrap the new carrier in
	 * lethal trace laid on top of the scrum they just won it in; at a full second it also meant the
	 * counter-attack got a free run with no trace behind it at all. 0.75 s is about 645uu of travel
	 * at carrier speed — enough to clear the pile, short enough that the trace is a threat again
	 * before anyone has crossed open ground.
	 *
	 * SPEC v10 §3 IS A CONDITIONAL, AND THIS IS THE BRANCH IT TOOK. Verbatim: "The grace period on
	 * turnovers doesn't seem to be working. Test it on both modes, fix it if needed. If it IS
	 * working, increase to .75seconds." IT IS WORKING — measured, in both modes, by
	 * Trace.Trail.GraceTest, which drives a real turnover through ATraceCore::GrantTo and times the
	 * new holder's first laid point on the shared clock: mode A 0.506 s and mode B 0.512 s against
	 * 0.500 configured, with a same-team pass at 0.009 / 0.012 s (no grace, as designed) and the v9
	 * §3 instant clear composing with both. So this is the "increase" branch, not the "fix it" one.
	 *
	 * IT DELAYS FORMATION, NOT LETHALITY, and that distinction is the most likely reason a player
	 * would report it as not working: trace already on the ground kills throughout the window.
	 * Raising this number does not change that and never will.
	 *
	 * Applies only when the Core changes SIDE. A pass between teammates has no grace, by design —
	 * and in mode B (ScoringMode = ThrownCoreAndGoals) the same rule holds for a thrown Core:
	 * intercepted by an enemy = this grace, recovered by a teammate = none. Spec v4 §7.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core", meta = (DisplayName = "Turnover Trace Grace (s)", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.0", UIMax = "2.0"))
	float CoreTurnoverGraceSeconds = 0.75f;

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
	 * cannot be intercepted by anyone, which deletes the whole point of mode B.
	 *
	 * *** PATCH 28 ITEM 4: 3300 -> 2900. *** Verbatim: "Reduce core throw speed max to 2900uu/s,
	 * adjusting Mortimer accordingly." THE "ADJUSTING MORTIMER ACCORDINGLY" HALF IS ALREADY DONE BY
	 * ARITHMETIC AND ALWAYS WAS — this is the one case where the project's own rule paid out.
	 * Mortimer's passive (MortimerThrowChargeHoldScale 2.0, MortimerThrowChargePastFullScale 0.6)
	 * is a pair of scalars on ATraceCore::GetThrowChargeScaleForHold's shared curve; it produces a
	 * POWER, and the launch is Direction x CoreThrowSpeed x Power / sqrt(CoreMassScale). He holds no
	 * uu/s of his own anywhere in the tree, so moving this line moves him with it. Verified by
	 * Trace.Mortimer.ThrowTest, which throws four real Cores and measures them.
	 *
	 * THIS IS THE BASE, BEFORE WEIGHT. Spec v5 §4's "increase the weight of the core" is
	 * CoreMassScale below, which divides this by sqrt(M) at the point of use — it is NOT re-applied
	 * here, so this line is still the pre-weight number the panel says it is.
	 *
	 * *** WHAT PATCH 28's 2900 ACTUALLY SHIPS, at CoreMassScale 1.8 (sqrt = 1.341641),
	 * CoreThrowChargeFloorFraction 0.15, CoreThrowGravityScale 0.55 and world gravity -980: ***
	 *
	 *     full charge, anybody      2900 / 1.341641           = 2161.5 uu/s   (was 2459.7)
	 *     an instant click (x0.15)                            =  324.2 uu/s   (was  369.0)
	 *     full EXTENDED charge, MORTIMER ONLY (Power x1.51)   = 3263.9 uu/s   (was 3714.1)
	 *
	 * and, flat, from a standing thrower (up bias 0.12 x 1.8^1.5 = 0.289794, gravity x 1.8 x 0.55 =
	 * -970.2 uu/s^2, so range = 2 x v^2 x bias / g):
	 *
	 *     full charge, anybody      3614 uu -> 2791 uu        (x0.772 = (2900/3300)^2)
	 *     full charge, MORTIMER     8241 uu -> 6364 uu        (x2.2801 = 1.51^2 of the above, both)
	 *
	 * Range goes as the SQUARE of the speed, so a 12.1% speed cut is a 22.8% range cut. That is the
	 * number to quote if anybody asks whether "2900" felt bigger than it reads.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Speed (uu/s, before weight) [mode B; Patch 28 §4: 3300 -> 2900]", ClampMin = "100.0", ClampMax = "20000.0", UIMin = "500.0", UIMax = "8000.0"))
	float CoreThrowSpeed = 2900.f;

	/**
	 * Fraction of the throw speed added as upward velocity, so a throw arcs instead of running flat
	 * along the floor and catching on the first piece of cover.
	 *
	 * Small: at 0.12 a 3000 uu/s throw leaves with 360 uu/s of lift, which is a shallow arc a player
	 * can lead. Raising it makes throws lobbed and easy to read; zero makes them hitscan-flat — and
	 * since spec v6 §4.3 raised the hoop off the floor, a flat throw cannot reach the goal at all.
	 *
	 * NAME IS LOAD-BEARING: ATraceCore resolves this by reflection under exactly this spelling
	 * (TraceModeBTuning::ThrowUpBias). It was briefly declared as "CoreThrowUpwardBias", which the
	 * lookup could not find, so the panel slider moved nothing and the CVar default was played
	 * instead. Do not rename either half alone.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Upward Bias (fraction of speed) [mode B]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5"))
	float CoreThrowUpBias = 0.12f;

	/**
	 * How much of the THROWER'S OWN velocity the Core keeps when it leaves their hands, as a fraction.
	 * Spec v8 §4, and the whole of it.
	 *
	 * Verbatim: "When jumping and throwing the core, the core doesn't seem to keep momentum. Make
	 * sure that the core has the momentum from the throw and also carries momentum from the player."
	 * Before v8 the launch was impulse and nothing else, so a throw taken at a dead sprint and a
	 * throw taken standing still left at exactly the same speed — and a throw taken at the top of a
	 * jump threw away the jump, which is the case the note actually names.
	 *
	 * [ASSUMPTION] 1.0, full inheritance, per the spec's own instruction to expose a fraction rather
	 * than hardcode a fudge. VERTICAL IS INCLUDED: that is the jumping throw.
	 *
	 * DELIBERATELY NOT SCALED BY CoreMassScale. Mass says how hard the Core is to THROW and already
	 * divides the impulse; momentum is transferred by carrying it, so a heavier Core leaves the same
	 * jump with the same velocity. Applying the weight to both would apply it twice.
	 *
	 * 0 replays the pre-v8 throw exactly, which is the A/B to reach for if this proves too strong.
	 * 0.6 is the middle setting that keeps a jumping throw feeling like a jumping throw while cutting
	 * the dash-throw tail case (dash 3300 + impulse ~= 5460 uu/s after Patch 28 §4; it was ~5760 at
	 * the old 3300 base) by 40%.
	 *
	 * NAME IS LOAD-BEARING: ATraceCore resolves this by reflection under exactly this spelling, and
	 * says so at runtime — the mode-B binding check prints "NO UTraceSettings PROPERTY FOUND FOR:"
	 * with the name if it is renamed here alone.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Velocity Inheritance (fraction) [mode B]", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0"))
	float CoreThrowVelocityInheritance = 1.0f;

	/**
	 * Spec v31 §2. Multiplier applied to CoreThrowVelocityInheritance for the DOWNWARD part of the
	 * thrower's velocity only. 0 (default): a player who throws while falling no longer has the fall
	 * subtracted from the launch — the Core leaves with the same Z a standing throw gives it, and
	 * still carries the whole horizontal motion. 1: the pre-v31 behaviour ("the core just drops").
	 * Rising velocity is never touched — spec v8 §4's jumping throw still carries the jump in full.
	 *
	 * Declared here in the release-hygiene pass so the knob is a real settings slider; the default
	 * matches the Trace.ModeB.ThrowVelocityInheritanceDown console default it previously lived under,
	 * so shipped behaviour is identical.
	 *
	 * NAME IS LOAD-BEARING: ATraceCore resolves this by reflection under exactly this spelling, same
	 * as CoreThrowVelocityInheritance above.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Velocity Inheritance, Downward Multiplier [mode B]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float CoreThrowVelocityInheritanceDown = 0.0f;

	// ------------------------------------------------------------------------------------------
	// THE CHARGE-UP THROW (spec v13 §6, new). Four knobs, and the spec asked for all four by name.
	//
	// Verbatim, in order:
	//   "When a player RELEASES the throw button, the core should instantly be released."
	//   "The longer the player holds down, the more momentum the core has."
	//   "Start by making a one second charge up time to reach the current core throw momentum"
	//   "Charge time to throw momentum should be a linear correlation"
	//   "So if the player just clicks the throw button it will throw with very low momentum"
	//
	// *** THE ARITHMETIC, WRITTEN ONCE AND HERE, BECAUSE FOUR KNOBS AND TWO EXISTING ONES MEET IN
	// *** ONE EXPRESSION AND EVERY MISREADING OF IT IS A PLAUSIBLE-LOOKING WRONG THROW:
	// ***
	// ***     t     = HeldSeconds / CoreThrowChargeSeconds                 (0 at a click)
	// ***     tCap  = bCoreThrowChargeClampsAtFull ? min(t, 1)
	// ***                                          : min(t, CoreThrowChargeMaxFraction)
	// ***     Power = CoreThrowChargeFloorFraction
	// ***             + (1 - CoreThrowChargeFloorFraction) * tCap          (floor..1 linearly)
	// ***     Launch = Direction * CoreThrowSpeed * Power / sqrt(CoreMassScale)
	// ***              + ThrowerVelocity * CoreThrowVelocityInheritance
	// ***
	// *** CHARGE SCALES THE IMPULSE ONLY. The inherited velocity is added ON TOP, unscaled, exactly
	// *** as it is today (spec §6 asks for this to be stated explicitly). A tap-throw from a sprint
	// *** therefore still carries the sprint — which is right: the player's momentum is not something
	// *** they charge up, and scaling it too would make a tapped throw while running come out
	// *** backwards relative to the runner.
	// ***
	// *** WEIGHT STILL APPLIES AFTER CHARGE, unchanged: CoreMassScale divides the impulse and nothing
	// *** here touches that ordering. Charge is a fraction of the throw the designer tuned, not a new
	// *** throw.
	//
	// RELEASE IS INSTANT — that is a code rule, not a knob, and it is the first thing the user asked
	// for: the throw leaves on the release frame with whatever power has accumulated. There is
	// deliberately no minimum hold and no wind-up-on-release knob to add one, because either would
	// contradict "the core should instantly be released" while looking like tuning.
	//
	// THE CHARGE INDICATOR IS LOCAL AND INSTANT (spec §6). The bar is drawn from the client's own
	// held-time, which needs no knob and no replication; the THROW stays server-authoritative and
	// recomputes Power from the server's own held-time, so a client cannot ask for more power than
	// they held for.
	//
	// BOTS MUST CHARGE (spec §6): a bot that always taps throws at the floor fraction and mode B
	// looks broken. That is bot logic, not a knob — the bot picks a hold time from the distance it
	// wants, using exactly the expression above.
	// ------------------------------------------------------------------------------------------

	/**
	 * Seconds of holding the throw button to reach FULL momentum, i.e. the throw the game has today.
	 *
	 * 0.6 as of v17 ("Reduce max charge time from .8 to .6 seconds, but keep max velocity the
	 * same"); v16 had already taken it 1.0 -> 0.8. The
	 * original 1.0 was the user's own starting number; the linear scale is unchanged, only the time
	 * to reach full. This is the anchor
	 * the other three are defined against: at exactly this hold, Power is 1 and the Core leaves at
	 * CoreThrowSpeed, so nothing about the existing throw changes for a player who holds for a second.
	 *
	 * NEVER 0. A zero charge time makes every throw full power the instant it is pressed, which is
	 * the pre-v13 behaviour wearing a charge bar — the clamp below starts at 0.05 for that reason
	 * rather than at 0, so "I disabled charging" is done by raising the floor fraction to 1, which
	 * says what it means.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Charge Time (s) [v13 §6]", ClampMin = "0.05", ClampMax = "10.0", UIMin = "0.25", UIMax = "3.0"))
	float CoreThrowChargeSeconds = 0.6f;

	/**
	 * Momentum an INSTANT CLICK throws with, as a fraction of full. [ASSUMPTION] 0.15.
	 *
	 * The user asked for "very low momentum" on a click and did not give a number. 0.15 is very low
	 * — 3000 x 0.15 = 450 uu/s, a lob of a few metres — while staying visibly a THROW.
	 *
	 * IT IS NOT ZERO, AND THAT IS THE WHOLE REASON THIS KNOB EXISTS. A zero-momentum throw drops the
	 * Core at the thrower's feet, inside their own pickup radius, and reads as "the throw button is
	 * broken" rather than as "I did not charge". It would also interact badly with
	 * CoreThrowerPickupLockoutSeconds, leaving a Core sitting untouchable at the feet of the player
	 * who just dropped it.
	 *
	 * 1.0 is legal and disables charging entirely (every throw full power) — the A/B arm for "is the
	 * charge-up an improvement", and the honest way to turn the feature off.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Charge Floor (fraction of full) [v13 §6]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.05", UIMax = "0.5"))
	float CoreThrowChargeFloorFraction = 0.15f;

	/**
	 * Does holding PAST the charge time add anything? [ASSUMPTION] true — it clamps at full.
	 *
	 * The user said one second reaches "the current core throw momentum" and said nothing about
	 * beyond, so the safe reading is that a second is the maximum and the current throw is still the
	 * strongest throw in the game. Clamping also keeps the charge bar honest: a bar that fills and
	 * then keeps mattering is a bar that lies.
	 *
	 * Set false to allow OVERCHARGE up to CoreThrowChargeMaxFraction below, which is the pair of
	 * knobs spec §6 asked for as "max multiplier, and whether it clamps".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Charge Clamps At Full [v13 §6]"))
	bool bCoreThrowChargeClampsAtFull = true;

	/**
	 * The ceiling on t when the clamp above is OFF, as a multiple of a full charge. IGNORED while
	 * bCoreThrowChargeClampsAtFull is true.
	 *
	 * 1.0 — the default — makes the two settings agree, so flipping the clamp off changes nothing
	 * until this is also raised. That is deliberate: an overcharge that appears the moment somebody
	 * unticks a box is an overcharge nobody chose. At 1.5 a 1.5 s hold throws at 1.5x
	 * CoreThrowSpeed, and the linear rule above is simply extrapolated.
	 *
	 * RAISING THIS RE-TUNES MODE B, not just the throw: CoreThrowGravityScale, the goal distance and
	 * the 450 uu catch radius were all tuned against a base throw ceiling that has since moved twice
	 * (3000, then 3300, and 2900 as of Patch 28 §4). Treat it as an experiment knob, not a difficulty
	 * dial.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Charge Max (x full, clamp off) [v13 §6]", ClampMin = "1.0", ClampMax = "4.0", UIMin = "1.0", UIMax = "2.0"))
	float CoreThrowChargeMaxFraction = 1.0f;

	/**
	 * *** SPEC v28 §7 — HOW LONG A FULL CHARGE MAY BE SAT ON BEFORE THE SERVER THROWS IT. ***
	 *
	 * Verbatim: "A player can hold a core at full throw charge indefinitely. Make a second little red
	 * ring which fills on the inside of the green one on a .6seconds timer. This timer should start
	 * right when throw charge reaches full. When the second timer completes, the core is released at
	 * full charge automatically, if a player doesn't throw it before then."
	 *
	 * THE RED RING IS THIS NUMBER. ATraceHUD draws its fill as arithmetic on the SERVER's stamp of
	 * the instant the charge reached full, against this window, so the ring and the throw are the
	 * same subtraction and cannot disagree. Move this and the ring moves with it — there is no second
	 * place to edit.
	 *
	 * 0 SWITCHES THE WHOLE OF §7 OFF: no red ring, no auto-release, a full charge held forever. That
	 * is the pre-v28 game and it is the section's red arm.
	 *
	 * *** WHY THIS PROPERTY WAS MISSING UNTIL THE INTEGRATION PASS, AND WHY THAT MATTERED LITTLE BUT
	 * NOT NOTHING. *** The §7 owner shipped the whole feature bound BY NAME to this exact string
	 * through ATraceCore's Resolve(), and could not add the property: TraceSettings.h was being
	 * edited by three other agents in the same pass and they refused to risk a clobber. Resolve()
	 * answers with the CVar when the name is missing, so the shipped behaviour was already 0.6 s and
	 * correct. What was missing is everything a property buys: the Project Settings row, the ini key
	 * a designer can retune without a console, and the fact that a knob bound by name to nothing is
	 * indistinguishable from a MISSPELLED one. This project has been caught by exactly that before —
	 * see the HitscanRange note at the top of Config/DefaultGame.ini.
	 *
	 * 0.6 s IS THE OWNER'S NUMBER, quoted above, and it is deliberately shorter than
	 * CoreThrowChargeSeconds (also 0.6): the whole point is that sitting on a full charge is not a
	 * free option, so the grace period must not exceed the work it took to earn it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Full-Charge Auto-Release Window (s, 0 = off) [v28 §7]", ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.0", UIMax = "2.0"))
	float CoreThrowFullChargeAutoReleaseSeconds = 0.6f;

	/**
	 * World gravity multiplier applied to a Core in flight and rolling loose, BEFORE weight.
	 *
	 * Below 1 because the field is 33600 uu long: at full gravity a throw that a player can aim
	 * ploughs into the floor inside a couple of thousand uu, which turns every throw into a short
	 * dribble and makes interception trivial. 0.55 lets a throw cross useful ground while still
	 * arcing enough to be read and cut off.
	 *
	 * UNCHANGED THIS PASS, like CoreThrowSpeed and for the same reason: CoreMassScale multiplies it
	 * at the point of use, so moving both would apply spec v5 §4's weight twice.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Throw Gravity Scale (before weight) [mode B]", ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "1.5"))
	float CoreThrowGravityScale = 0.55f;

	/**
	 * MODE B ONLY. HOW HEAVY THE CORE IS IN FLIGHT, relative to the light Core that shipped before
	 * spec v5. 1.0 is exactly the old flight. THIS IS THE "WEIGHT" KNOB (spec v5 §4).
	 *
	 * Verbatim: "For game mode b ONLY, increase the weight of the core".
	 *
	 * WHY A SCALE AND NOT A MASS IN KILOGRAMS. The Core has no rigid body and no projectile movement
	 * component — ATraceCore integrates it by hand — so there is no engine mass field for a kg value
	 * to feed. "Weight" is therefore a derived model, and it has to be derived rather than faked by
	 * slowing the throw down, because the note is explicit: "Tune mass/gravity scale, not just
	 * speed." A Core that is merely slower reads as a bad throw; a Core that is HEAVY falls faster,
	 * arcs more, carries less far and stops dead when it lands.
	 *
	 * So this one number drives five, all inside ATraceCore's mode-B tuning block:
	 *     gravity scale  x M          the dominant "this thing is dense" cue
	 *     throw speed    / sqrt(M)    you cannot hurl a heavy object as fast
	 *     up bias        x M^1.5      the thrower lofts it, so shorter becomes shorter AND arced
	 *     bounce         / M          heavy things thud instead of skittering to midfield
	 *     rest speed     x M          and they settle sooner
	 *
	 * At 1.8, a flat throw measures — AT THE 3000 BASE THIS WAS TUNED AGAINST, which is the honest
	 * frame for a WEIGHT comparison: launch 3000 -> 2236 uu/s, gravity 539 -> 970 uu/s^2, flat range
	 * ~5000 -> ~3400 uu, apex ~120 -> ~215 uu. The base is 2900 as of Patch 28 §4, where the same
	 * M = 1.8 gives launch 2161.5 uu/s and flat range ~2791 uu; gravity and apex do not depend on it.
	 *
	 * INERT IN MODE A, which has no thrown Core at all. NAME IS LOAD-BEARING: ATraceCore resolves it
	 * BY NAME as "CoreMassScale". Sane range 1.2 to 2.5; 1.0 restores the pre-v5 flight exactly,
	 * which is the A/B the design owner needs to judge the feel.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Core Weight (mass scale, 1 = pre-v5) [mode B]", ClampMin = "0.25", ClampMax = "6.0", UIMin = "1.0", UIMax = "3.0"))
	float CoreMassScale = 1.8f;

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

	// ------------------------------------------------------------------------------------------
	// CATCH ZONE  (spec v6 §4.1, mode B only)
	//
	// Verbatim: "create a small invisible radius around players that acts as a 'catch zone,' so that
	// when the core enters that area, it curves towards the player like a magnet. This is intended
	// to make catching feel fluid and clean."
	//
	// All three are read BY NAME by TraceModeBTuning in Gameplay/TraceCore.cpp (Resolve()), which is
	// why the spelling below is load-bearing and why Trace.VerifyKnobs lists them: a typo here is a
	// slider that moves nothing, not a build error.
	// ------------------------------------------------------------------------------------------

	/**
	 * Radius, uu, of the invisible catch zone around EVERY player. A loose Core entering it is
	 * steered toward that player. 0 disables the magnet.
	 *
	 * Steers DIRECTION ONLY — speed is preserved, so this makes a near-miss into a catch without
	 * making the Core faster or slower than the throw that produced it. Sized well above
	 * CorePickupRadius (120): the pickup radius is where possession changes, this is the funnel
	 * that feeds it. Sane range 300 to 800; past ~1000 the Core visibly homes and interceptions
	 * stop feeling earned.
	 *
	 * SPEC v12 §4: 500 -> 450. Verbatim: "Reduce the 'magnet' radius for catching in game mode b by
	 * 10%". Mode A never reads this block at all (ServerApplyCatchZone is only reached from the
	 * mode-B loose-Core tick), so the cut is mode B only by construction rather than by a mode test.
	 *
	 * WHAT THE 10% ACTUALLY COSTS, because it is not 10% of anything a player experiences. The pull
	 * is a falloff cone — full strength on the capsule, ZERO at the boundary — so shrinking the
	 * radius does not merely clip the outer ring off, it steepens the whole falloff: at any given
	 * distance the Core is now closer to the (nearer) edge and therefore steered LESS. It also
	 * removes ~19% of the zone's area and ~27% of its volume, and it costs the fastest Cores the
	 * most: a throw that inherits a jumping thrower's velocity (up to ~3357 uu/s, see
	 * CoreThrowVelocityInheritance) crosses the zone in fewer frames, and the magnet integrates its
	 * correction per frame. Measured catch rate across the speed band is in the v12 report; if a
	 * fast Core stops being catchable, RAISE CoreCatchCurveStrength rather than putting this back —
	 * strength is what the fast case is short of, radius is what the user asked to cut.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Catch Zone Radius (uu) [mode B]", ClampMin = "0.0", ClampMax = "3000.0", UIMin = "0.0", UIMax = "1200.0"))
	float CoreCatchRadius = 450.f;

	/**
	 * How hard the catch zone bends the Core, as an exponential approach rate (frame-rate
	 * independent). 0 disables the magnet outright; the pull already falls off to zero at the edge
	 * of the zone, so this scales the curve at point-blank range rather than at the boundary.
	 *
	 * Too high and a throw at open ground snaps sideways into whoever is nearest, which reads as
	 * aim assist rather than a catch. Sane range 3 to 10.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Catch Zone Curve Strength [mode B]", ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.0", UIMax = "12.0"))
	float CoreCatchCurveStrength = 6.f;

	/**
	 * Seconds the THROWER alone is excluded from their own catch zone. Without it a throw curves
	 * straight back into the hands it left and the throw looks like it never happened — the same
	 * failure CoreThrowerPickupLockoutSeconds exists to prevent, one step earlier in the chain.
	 *
	 * Every other player, friend or enemy, is magnetised from the first frame: interception is a
	 * feature (spec v6 §4.1 [ASSUMPTION]).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Catch Zone Thrower Lockout (s) [mode B]", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.0", UIMax = "1.5"))
	float CoreCatchThrowerLockoutSeconds = 0.5f;

	/**
	 * SPEC v13 §5 — A CONTESTED MAGNET RESOLVES TO THE NEAREST PLAYER, and this is the number that
	 * stops it flickering. Verbatim: "If the core is within the 'magnet' zone of two or more players
	 * from opposite teams, it should go to the player closest to the core."
	 *
	 * THE RULE ITSELF NEEDS NO KNOB — nearest wins, full stop, and it is applied to ANY contested
	 * set rather than only to opposing teams (spec §5 [ASSUMPTION]: the quote describes the
	 * interesting case, not a restriction; two teammates contesting deserve the same determinism).
	 * What needs a knob is the TIE, because "nearest" is a float comparison made every server tick
	 * on two moving players. Two chasers a few uu apart swap the lead several times a second, the
	 * curve target changes with them, and the Core visibly wobbles between two people — the
	 * oscillation §5 asks to avoid.
	 *
	 * THE RULE THIS NUMBER EXPRESSES: the CURRENT target keeps the Core until a rival is closer by
	 * MORE than this many uu. It is a margin on the challenger, not on the holder, so it is stable
	 * without being sticky — a genuine overtake of half a metre still switches immediately.
	 *
	 * 50 uu against the 450 uu magnet radius is ~11% of the zone and about one and a half capsule
	 * radii (34 uu). Small enough that no player can win a contest they are visibly losing; large
	 * enough to swallow the frame-to-frame jitter of two sprinting capsules and of network-smoothed
	 * positions on a client's view.
	 *
	 * 0 DISABLES THE HYSTERESIS and gives the raw nearest-wins rule — which is exactly the arm to
	 * run when asking "is the flicker really gone, or did I just hide it". That A/B is the reason
	 * this is a knob rather than a constant.
	 *
	 * SERVER-AUTHORITATIVE, like the catch zone it modifies: the decision must be made once, on the
	 * server, from the server's positions. A client that ran this rule on its own smoothed copies of
	 * two pawns would reach a different answer at the boundary and draw the Core curving to the
	 * wrong player.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Catch Contest Hysteresis (uu) [v13 §5]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "0.0", UIMax = "150.0"))
	float CoreCatchContestHysteresisUU = 50.f;

	/**
	 * SPEC v7 §4. The angle from straight up, in degrees, at which a surface stops being something the
	 * Core can come to rest ON and starts being a wall it must bounce OFF.
	 *
	 * Verbatim: "Sometimes the core gets stuck up top of an object in gamemode b. This should also
	 * count as a turnover. Walls should not, the core should bounce off those."
	 *
	 * The arena is generic static meshes, so "is this a wall?" has to come from geometry, never from
	 * actor type: a Core at rest is probed downward and the surface holding it up is classified by its
	 * normal. Within this many degrees of straight up = a floor OR the top of any structure, and the
	 * Core turns over to the nearest enemy exactly as the v6 ground rule did. Steeper = a wall: the
	 * Core keeps its downward component, stays live, falls, and turns over wherever it finally lands.
	 *
	 * 45 is the v7 [ASSUMPTION]. Raising it toward 89 makes near-vertical faces count as resting
	 * places; lowering it toward 0 demands an almost perfectly flat surface to turn a Core over.
	 * Trace.ModeB.SurfaceMaxSlopeDegrees overrides it live.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Surface Max Slope (deg) [mode B]", ClampMin = "0.0", ClampMax = "89.0", UIMin = "10.0", UIMax = "60.0"))
	float CoreSurfaceMaxSlopeDegrees = 45.f;

	/**
	 * SPEC v13 §8 — HOW STEEPLY THE CORE MUST ARRIVE FOR A CONTACT TO COUNT AS A LANDING.
	 *
	 * Verbatim: "Sometimes the core is thrown and it turns over before it touches the ground."
	 *
	 * THE DIAGNOSED CAUSE was that the contact test read, in full, `Normal.Z >= SurfaceUpNormalZ()`:
	 * ANY upward-facing normal was a landing. Every structure in this arena has a flat top and the
	 * corner coves are terraced into flat treads, so a Core grazing across cover at 1400 uu/s and
	 * 350 uu above the floor presented a perfect V(Z=1.00) and was handed to the nearest enemy in
	 * mid-flight. Measured on the legacy rule: 7 of 7 graze shots turned over in the air.
	 *
	 * THIS IS THE ANGLE OF ARRIVAL, NOT THE ANGLE OF THE SURFACE — the two are separate knobs on
	 * purpose and CoreSurfaceMaxSlopeDegrees above is the other one. A tread can be perfectly flat
	 * (surface test passes) while the Core is travelling almost parallel to it (this test fails),
	 * and that combination is precisely the bug. The test is |velocity · -normal| / speed >= sin(this).
	 *
	 * 20 degrees is the [ASSUMPTION]. A Core genuinely dropping onto a crate arrives at 25-90
	 * degrees, so 20 clears every real landing with room to spare, while the graze that caused the
	 * bug measured 11.2 degrees. Raising it toward 45 demands a steeper drop and will start refusing
	 * shallow but genuine landings on open floor; lowering it toward 0 restores the pre-v13 rule
	 * exactly, which is the A/B arm — and that is why 0 is legal rather than clamped away.
	 *
	 * IT IS NOT THE ONLY CONDITION. The shipped rule is upward-facing AND (came to rest on it OR
	 * arrived at >= this angle AND had cleared the thrower), so a Core that lands shallow and STOPS
	 * still turns over on the frame it settles — 132 ms later in the measured case. This knob only
	 * decides whether a still-moving contact counts, which is the only case the bug was about.
	 *
	 * Trace.ModeB.LandingMinDescentDegrees overrides it live.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Landing Min Descent (deg) [v13 §8]", ClampMin = "0.0", ClampMax = "89.0", UIMin = "0.0", UIMax = "45.0"))
	float CoreLandingMinDescentDegrees = 20.f;

	// ==========================================================================================
	// SPEC v25 §2 — THE TURNOVER WINDOW AND THE MAGNET PULL  (goals mode only)
	//
	// Verbatim: "Instead of automatically going to the other team, a turnover is registered and the
	// core stays on the ground where it landed. / Players from the opposite team can hold right
	// mouse while hovering over the core to pull it to them, like a magnet. / Pulling the core
	// requires holding down right click while hovering mouse over it (with line of sight) for
	// .3seconds / Players from the team who dropped the core are locked out of picking it up for 5
	// seconds / After the 5 seconds are up, the opposite team loses the pull ability and either team
	// can pick up the core by running over it / the beam of light coming from the core should change
	// colors to the opposite team and be larger for the 5 seconds".
	//
	// NOTE WHAT IS *NOT* HERE: a pull speed. "It travels towards the player who completed the pull
	// first at full core thrown velocity" names an existing constant, so the delivery reads
	// ATraceCore::GetThrowSpeed() — CoreThrowSpeed after the weight model — and there is deliberately
	// no second number to disagree with it. Retune the throw and the pull follows.
	//
	// The TURNOVER CRITERIA are also not here and are unchanged (CoreSurfaceMaxSlopeDegrees,
	// CoreLandingMinDescentDegrees and the settle above still decide WHETHER and WHEN a turnover
	// fires). This block is only about what happens afterwards.
	// ==========================================================================================

	/**
	 * SPEC v25 §2. Seconds the team that DROPPED the Core is locked out of picking it up.
	 *
	 * For this window, and only this window: the OPPOSING team alone may pull the Core or run over
	 * it, and the beam wears their colour at CoreTurnoverBeamScale. When it expires the pull ability
	 * goes away and either team may take it by touch — which is exactly what a Core that was never
	 * turned over already does, so the window is cleared rather than kept as a third state.
	 *
	 * 5 is the note's own number. 0 turns the window off entirely, which is NOT the pre-v25 rule (a
	 * turnover would simply become a loose Core anybody may take); the A/B arm for the old behaviour
	 * is Trace.ModeB.TurnoverPull 0.
	 *
	 * THE LOCKOUT IS ON THE TEAM, NOT THE PLAYER. CoreThrowerPickupLockoutSeconds above is the other
	 * one and is a different thing: 0.35 s on ONE pawn so a throw is not caught by the hand it left.
	 *
	 * Trace.ModeB.TurnoverLockoutSeconds overrides it live.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Turnover Lockout (s) [v25 §2]", ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "15.0"))
	float CoreTurnoverLockoutSeconds = 5.f;

	/**
	 * SPEC v25 §2. Seconds of CONTINUOUS right mouse + hover + line of sight that complete a pull.
	 *
	 * CONTINUOUS is the whole of it: losing the hover, losing line of sight or releasing the button
	 * CANCELS the fill outright and the next attempt starts from zero. It does not pause, and there
	 * is no credit carried across a blink — a pull that could be accumulated in fragments would let a
	 * player pull the Core through a wall by flicking across it.
	 *
	 * 0.3 is the note's own number. Server-measured: the client sends only the button, so the ring a
	 * player watches fill is the server's clock rather than their own (spec v25: "Do not let a client
	 * decide it won a race"). Trace.ModeB.PullHoldSeconds overrides it live.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Pull Hold (s) [v25 §2]", ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.1", UIMax = "1.5"))
	float CorePullHoldSeconds = 0.3f;

	/**
	 * SPEC v25 §2. Half-angle of the cone that counts as "hovering the mouse over the core" at range.
	 *
	 * The pull's hover test is the same shape as the pass's (PassAimConeDegrees / PassAimSlack) and
	 * for the same reason: EITHER the aim is inside this cone OR the aim ray passes within
	 * CorePullAimSlackUU of the orb. A pure ray test is unusable at range — the drawn orb is 20 uu
	 * across and subtends 0.14 degrees at 8000 uu — and a pure cone is unusable point-blank, where a
	 * few degrees is several hundred uu of forgiveness.
	 *
	 * 4 degrees is tighter than the pass's 9, on purpose: a pass picks a teammate out of a moving
	 * crowd, and a pull picks the one stationary object on the floor. Trace.ModeB.PullAimConeDegrees
	 * overrides it live.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Pull Aim Cone (deg) [v25 §2]", ClampMin = "0.0", ClampMax = "45.0", UIMin = "1.0", UIMax = "15.0"))
	float CorePullAimConeDegrees = 4.f;

	/**
	 * SPEC v25 §2. How far, in uu, the aim ray may miss the ORB'S SURFACE and still count as a hover.
	 *
	 * Added to the DRAWN orb radius (20 uu), not to the larger sphere the loose Core sweeps with: the
	 * player is aiming at a ball they can see, so the forgiveness is measured off that ball.
	 * Trace.ModeB.PullAimSlackUU overrides it live.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Pull Aim Slack (uu) [v25 §2]", ClampMin = "0.0", ClampMax = "400.0", UIMin = "0.0", UIMax = "150.0"))
	float CorePullAimSlackUU = 60.f;

	/**
	 * SPEC v25 §2. Optional ceiling on how far away a pull may be STARTED, uu. 0 = no limit.
	 *
	 * 0 IS THE SHIPPED VALUE, and that is a decision rather than an oversight: the note states no
	 * range, and inventing one would be adding a rule. It is not needed to keep the mechanic sane
	 * either — a pull already requires the crosshair on a 20 uu ball WITH clear line of sight, which
	 * across a 33600 uu pitch is its own range limit. The knob exists so a playtest that disagrees
	 * can put a number on it without a rebuild. Trace.ModeB.PullMaxRangeUU overrides it live.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Pull Max Range (uu, 0 = none) [v25 §2]", ClampMin = "0.0", ClampMax = "60000.0", UIMin = "0.0", UIMax = "8000.0"))
	float CorePullMaxRangeUU = 0.f;

	/**
	 * SPEC v25 §2/§3. How much LARGER the Core's beam is during the turnover window.
	 *
	 * *** A MULTIPLIER OF THE NORMAL BEAM WIDTH, NEVER A WIDTH. *** This is Demo 21's standing rule
	 * applied literally: the note asks for a beam that is "larger", which is a statement ABOUT the
	 * normal beam, so it has to move when the normal beam moves. A stored width would be "larger"
	 * today and quietly wrong the first time the base beam was retuned — and nothing would say so.
	 *
	 * 1 = no change, which is the A/B for judging whether the size cue is doing any work at all.
	 * 2.2 is an [ASSUMPTION]: the note says "larger" without a number, and at 2.2 the shaft reads as
	 * a different object from across the field rather than as the same one slightly closer.
	 * Trace.ModeB.TurnoverBeamScale overrides it live.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Core|Mode B", meta = (DisplayName = "Turnover Beam Scale (x normal) [v25 §2]", ClampMin = "0.1", ClampMax = "10.0", UIMin = "1.0", UIMax = "4.0"))
	float CoreTurnoverBeamScale = 2.2f;

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
	 * Seconds of trace invulnerability granted by a parry. Spec v10 §4: 0.175 (v8 §3 put it at 0.2,
	 * v3 §3 at 0.1).
	 *
	 * THE WINDOW AND THE INVULNERABILITY ARE THE SAME NUMBER, and spec v10 §4 asks the question
	 * outright, so here is the answer in the one place a tuner will look. This codebase has no
	 * separate "press window": a parry is not a state you can be caught inside, it is an
	 * instantaneous grant. UTraceTrailComponent::BeginParry stamps ParryEndServerTime = now + this,
	 * and that single deadline is simultaneously (a) how long the trace is invulnerable, (b) how long
	 * the trace is tinted red, and (c) the test an arriving dash is measured against in
	 * IsParryActive(). "Parry time", "parry window" and "parry invulnerability" all mean this
	 * property. The only other parry timer is ParryCooldown, which answers a different question — how
	 * soon you may parry AGAIN — and v10 §4 deliberately does not touch it.
	 *
	 * THE ENTIRE MECHANIC IS THIS NUMBER. At 0.175 s a parry is still a read of the incoming dash; at
	 * 0.4 s it is a panic button, and the dash — the only counterplay the defence has against a
	 * carrier — stops being reliable. Raise it further only if playtesting says the window is
	 * unhittable at real latency, and raise the cooldown with it. Sane range 0.08 to 0.30.
	 *
	 * NOTE FOR TUNING AT LATENCY: a joined client does not lose window LENGTH to the network, they
	 * lose its ALIGNMENT — see Gameplay/TraceParry.h's v8 §3 header. Widening this number is not the
	 * fix for that and never was; it only makes the misalignment cheaper to survive.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Parry", meta = (DisplayName = "Parry Duration (s)", ClampMin = "0.02", ClampMax = "2.0", UIMin = "0.05", UIMax = "0.5"))
	float ParryDuration = 0.175f;

	/**
	 * Seconds before the carrier may parry again, measured from the parry's START.
	 *
	 * [ASSUMPTION] — the spec does not give one. 1.5 s against v8's 0.2 s window is a ~13% uptime,
	 * which keeps it a reaction check: spamming it covers almost nothing, so a defender's dash timing
	 * still beats a carrier's button mashing. Drop it toward 0.5 and the carrier can simply hold
	 * the lane covered; the mechanic then reads as "the carrier is immune", which is not the game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Parry", meta = (DisplayName = "Parry Cooldown (s)", ClampMin = "0.0", ClampMax = "30.0", UIMin = "0.2", UIMax = "6.0"))
	float ParryCooldown = 1.5f;

	/**
	 * Spec v6 §3: "Perfectly timed parries should kill the enemy dashing." A parry that actually
	 * intercepts a dash which WOULD have killed the carrier now kills the dasher instead of merely
	 * saving the carrier. False restores the pre-v6 behaviour (the parry is purely protective).
	 *
	 * Only a dash that would genuinely have been lethal is punished — a parry thrown at empty air
	 * kills nobody, and under a TrailLethality tuning where the trace does not kill the carrier
	 * there is nothing to punish. Read by Gameplay/TraceParry.cpp; the console override is
	 * Trace.Parry.KillsDasher, which is a cheat cvar and loses to nothing but itself.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Parry", meta = (DisplayName = "Parry Kills the Dasher"))
	bool bParryKillsDasher = true;

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
	 * NO LONGER THE EXPIRY RULE. SPEC v7 §1 deleted time-based trail expiry outright; a trail point
	 * now leaves the tail ONLY when new trace at the head pushes the path past TrailMaxLengthUU.
	 *
	 * A carrier who stands still therefore keeps their entire trace indefinitely — that is the whole
	 * point of the change. The reported exploit was that a stationary carrier's trace timed out and
	 * left them literally unkillable, the trace being the only way to kill a carrier.
	 *
	 * WHAT THIS VALUE STILL DOES, and the only two things it does:
	 *   1. It is the DERIVATION INPUT for TrailMaxLengthUU when that knob is left at or below zero
	 *      (TrailLifetime x WalkSpeed x 0.75). Set TrailMaxLengthUU directly and this stops mattering.
	 *   2. It is the age-fade reference for the LEGACY renderer arm (Trace.Trail.Renderer 0), which
	 *      is a measurement arm, not the shipping look.
	 *
	 * Do not reintroduce an expiry read of this value. A surviving timer anywhere in the retirement
	 * path restores the stand-still exploit, which is the one thing spec v7 §1 exists to kill.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Trail Lifetime (s) [derivation/legacy fade only]", ClampMin = "0.2", ClampMax = "4.0", UIMin = "0.5", UIMax = "4.0"))
	float TrailLifetime = 2.0f;

	/**
	 * SPEC v7 §§1-2. THE MAXIMUM LENGTH OF A TRACE, IN UU — and the rule that retires trail points.
	 *
	 * Points are appended at the head as the carrier moves and dropped from the tail only while the
	 * total path length exceeds this. Nothing else retires a point. No clock is involved, so the
	 * trace behind a stationary carrier is permanent until they move again.
	 *
	 * 1200 = the v7 §2 conversion of the old timer: TrailLifetime 2.0s x WalkSpeed 800 = 1600uu, of
	 * which the request was "lower by 25%". This is now the number the whole mechanic hangs on.
	 *
	 * At or below zero re-derives it from TrailLifetime x WalkSpeed x 0.75 so the pair cannot silently
	 * disagree; the console override Trace.Trail.MaxLength beats both for a headless measurement run.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Trail Max Length (uu)", ClampMin = "0.0", ClampMax = "8000.0", UIMin = "400.0", UIMax = "3000.0"))
	float TrailMaxLengthUU = 1200.f;

	/**
	 * Distance the carrier must travel before a new point is appended, in uu.
	 *
	 * Smaller is a smoother, more expensive trace. Sane range 40 to 100; this multiplied by
	 * MaxTrailPoints is the longest trace that can exist.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Point Spacing (uu)", ClampMin = "10.0", ClampMax = "500.0", UIMin = "30.0", UIMax = "200.0"))
	float TrailPointSpacing = 60.f;

	/**
	 * HALF the trace's width in uu — the LETHAL radius and the DRAWN radius, which are one number.
	 *
	 * SPEC v7 §3: 45 (the full player-model width) -> 22.5, "it doesn't need to be the full width of
	 * the player model". Both the trip test and every renderer arm resolve this through
	 * UTraceTrailComponent::GetTraceTrailRadius(), so shrinking the drawing without shrinking the kill
	 * volume ("I dashed past it and died anyway") is not expressible. Trace.Trail.Radius overrides it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Segment Radius (uu)", ClampMin = "5.0", ClampMax = "500.0", UIMin = "10.0", UIMax = "150.0"))
	float TrailRadius = 22.5f;

	/**
	 * The trace's height in uu, centred on the carrier's mid-model — LETHAL AND DRAWN TOGETHER.
	 *
	 * SPEC v7 §3: 190 -> 63, "get rid of the top and bottom third of the trace, so that it's just the
	 * middle section, in order to make visibility around the trace better". Resolved through
	 * UTraceTrailComponent::GetTraceTrailHeight(); Trace.Trail.Height overrides it.
	 *
	 * NOTE, and this changed in v7: this NO LONGER drives the third-person carry camera.
	 * ATraceCharacter::GetThirdPersonPivotZ used to derive its height from this so the camera cleared
	 * the trail wall, which at 63 would have dropped the approved carry framing by ~68uu as a side
	 * effect of a visibility change. The camera now has its own floor — see
	 * TraceCharacterLayout::CarryPivotZ.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Segment Height (uu)", ClampMin = "10.0", ClampMax = "1000.0", UIMin = "40.0", UIMax = "400.0"))
	float TrailHeight = 63.f;

	// ------------------------------------------------------------------------------------------
	// TRACE WALL CLEARANCE — THE CORNER FITTER  (spec v12 §6, new)
	//
	// Verbatim: "The trace is clipping into walls sometimes, when a model runs close to a
	// corner/structure."
	//
	// [DIAGNOSED] The ribbon meshes are NoCollision, so they never interact with the world. But the
	// cause is not the drawing: points are laid every TrailPointSpacing (60 uu) of travel and the
	// trace — ribbon AND trip test alike — is the STRAIGHT CHORD between consecutive points. Rounding
	// a pillar the carrier's capsule follows an arc, 60 uu of which is most of the turn, so the two
	// points straddle the corner and the chord between them cuts through the pillar. The lethal
	// volume takes the identical shortcut, which is why this was never a cosmetic bug.
	//
	// THE FIX IS SUBDIVISION, NOT DISPLACEMENT: UTraceTrailComponent keeps the positions the carrier
	// really occupied and inserts them until no segment passes through the level. Nothing is moved
	// off the route the player ran — wherever a 34 uu capsule legally stood there is room for a
	// 22.5 uu trace. The push knob below only cleans up a point that was already inside something.
	//
	// *** THE STANDING INVARIANT SURVIVES BY CONSTRUCTION: THE LETHAL VOLUME MATCHES THE DRAWN
	// *** VOLUME. Both are built from TrailPoints and the fitter edits TrailPoints, so there is one
	// *** polyline and no pair of geometry paths to keep in agreement by hand. Any future version of
	// *** this fix that touches the RIBBON instead would create an invisible kill volume, which is
	// *** the worse of the two failures this project has already had a version of.
	//
	// NAMES ARE LOAD-BEARING AND THEY ARE THE COMPONENT'S OWN. Each of the four below pairs with the
	// Trace.Trail.WallFit* console variable of the same meaning, and must be resolved the way
	// GetTraceTrailRadius() resolves TrailRadius: settings property by default, CVar when the CVar is
	// explicitly set. A knob declared here that the component never reads is a dead slider, which is
	// the failure Trace.VerifyKnobs exists for — it lists all four.
	// ------------------------------------------------------------------------------------------

	/**
	 * Master switch for the corner fitter. Pairs with Trace.Trail.WallFit.
	 *
	 * OFF restores the exact pre-v12 straight chord — i.e. it reproduces the reported bug on demand,
	 * which is what makes it worth keeping rather than deleting once the fix lands. "Is the clipping
	 * fixed" and "did the fix change where the trace kills" are two questions, and this is how they
	 * get asked one at a time.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Wall Fit Enabled [v12 §6]"))
	bool bTrailWallFitEnabled = true;

	/**
	 * Clearance in uu the trace asks for OVER its own half width when testing a segment against the
	 * level. Pairs with Trace.Trail.WallFitMargin.
	 *
	 * The ribbon legitimately reaches a little past TrailRadius at joints (interior joints overlap by
	 * one radius so the wedge on the outside of a corner closes) and a box's corner is further from
	 * its axis than its face. A few uu covers that without pretending the trace is fatter than it is.
	 *
	 * KEEP IT WELL UNDER THE CARRIER'S OWN 34 uu CAPSULE RADIUS. Ask for more clearance than a body
	 * needs and there are legal routes no polyline can satisfy: the fitter then spends its whole
	 * insert budget every step, fails, and falls back to the chord — the bug returns while the knob
	 * says the fix is on. Sane range 2 to 10.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Wall Fit Margin (uu over half width) [v12 §6]", ClampMin = "0.0", ClampMax = "34.0", UIMin = "0.0", UIMax = "12.0"))
	float TrailWallFitMarginUU = 4.f;

	/**
	 * Largest horizontal nudge, in uu, applied to a trail point that is ALREADY inside level
	 * geometry. Pairs with Trace.Trail.WallFitMaxPush. 0 disables the nudge and leaves subdivision.
	 *
	 * Deliberately small. Subdivision does the real work; this is residue cleanup, and it must never
	 * become a mechanism that slides the trace off the route the player ran — the trace is where the
	 * carrier went, and a knob that can move it 100 uu is a knob that can put a kill volume somewhere
	 * nobody ran. The component additionally refuses any nudge that does not reduce penetration or
	 * that breaks line of sight to the original point, so a thin wall cannot be tunnelled through.
	 *
	 * *** SPEC v13 §7 RAISES IT 12 -> 44, AND THE OLD 12 IS THE MEASURED REASON THE v12 FIX DID NOT
	 * *** LAND. v13 §7 re-measured the carried-over bug with the symptom forced out: fitter OFF gave
	 * *** 26.1 uu inside cover on 35.9% of frames, fitter ON gave 24.2 uu on 35.3% — it routed twice
	 * *** in 1312 frames. A 12 uu allowance cannot clear a 26 uu penetration, so the push was capped
	 * *** below the problem it was aimed at and the number said "fix on" the whole time.
	 * ***
	 * *** WHAT 44 IS, AND WHY IT IS NOT 30. An intermediate 30 was tried during v13 and was STILL
	 * *** below the real requirement, for the reason the whole §7 diagnosis turns on: the trace's
	 * *** drawn reach is NOT TrailRadius. PlaceRibbon overlaps interior joints by another TrailRadius
	 * *** along the element axis, so a box corner stands Radius*sqrt(2) = 31.8 uu from the joint, and
	 * *** the Catmull-Rom resample overshoots a tight corner by a further 0.0741 * TrailPointSpacing
	 * *** = 4.4 uu. UTraceTrailComponent::GetTraceDrawnHalfReach() derives that as 36.3 uu; plus
	 * *** TrailWallFitMarginUU (4) the fitter asks for 40.3 uu of clearance. 30 could not deliver it,
	 * *** so WallFitMaxPush()'s derived floor was silently the thing deciding — and a knob that has
	 * *** stopped being the thing deciding is the failure this file exists to prevent. 44 sits above
	 * *** the requirement with headroom, so THE SETTING IS AGAIN THE AUTHORITY and the floor never
	 * *** bites. If TrailRadius or TrailPointSpacing move, re-derive: this must stay >= reach+margin.
	 * ***
	 * *** THE 34 uu CAPSULE CEILING BELONGS TO TrailWallFitMarginUU, NOT TO THIS KNOB, and conflating
	 * *** them is what capped this at 30 in the first place. The margin is CLEARANCE ASKED FOR — ask
	 * *** for more room than the carrier's own body needs and legal routes become unsatisfiable, so
	 * *** that one is genuinely bounded by the capsule. This is the DISTANCE A BURIED POINT MAY BE
	 * *** MOVED to reach that clearance, which is a different quantity and is necessarily larger: a
	 * *** point 36 uu inside a lip needs a >36 uu push to get out of it, whatever the capsule is.
	 * ***
	 * *** THE INVARIANT IS UNCHANGED AND IS WHY THIS IS SAFE TO RAISE: the fitter edits TrailPoints,
	 * *** and the trip test and every renderer arm are both built from TrailPoints, so a point pushed
	 * *** out of a wall moves the LETHAL volume and the DRAWN volume in the same operation. Pushing
	 * *** further cannot create a "visible but not lethal" or "lethal but not visible" trace; it can
	 * *** only move both. Anything that ever edits the RIBBON alone breaks that and must not.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Wall Fit Max Push (uu) [v13 §7: 12 -> 44]", ClampMin = "0.0", ClampMax = "64.0", UIMin = "0.0", UIMax = "64.0"))
	float TrailWallFitMaxPushUU = 44.f;

	/**
	 * SPEC v13 §7 — CLEAR THE TRACE BY ITS OWN HALF WIDTH WHENEVER THE PATH RUNS NEAR GEOMETRY,
	 * rather than only when a chord is BLOCKED. Pairs with Trace.Trail.WallClearance.
	 *
	 * THIS IS THE SHAPE OF THE FIX, WHICH IS WHY IT IS A SWITCH AND NOT A DISTANCE. v12 §6 diagnosed
	 * corner-cutting — two points straddling a pillar with the chord between them passing through it
	 * — and fixed exactly that: the router triggers on a blocked chord. But the reported bug is the
	 * ribbon's own HALF WIDTH pressed against a wall the carrier legally ran alongside, where no
	 * chord is blocked at all because the centre line is outside the wall the whole way. That is why
	 * the fitter routed twice in 1312 frames while 35% of frames were 24 uu inside cover: it was
	 * never asked. ON makes the near-geometry test a SWEEP of the trace's half width plus
	 * TrailWallFitMarginUU along each segment, so a segment that merely runs too close is corrected
	 * as well as one that is cut through.
	 *
	 * OFF IS THE v12 BEHAVIOUR EXACTLY — blocked chords only — and it is the A/B arm that reproduces
	 * the residual clipping on demand. Between this and bTrailWallFitEnabled (which turns the whole
	 * fitter off, i.e. the pre-v12 straight chord) there are three arms to measure rather than two,
	 * which is what it takes to tell "the fix does nothing" apart from "the fix is aimed at the
	 * wrong thing" — the mistake this pass is correcting.
	 *
	 * THE CLEARANCE DISTANCE IS DELIBERATELY NOT A KNOB OF ITS OWN. It is TrailRadius +
	 * TrailWallFitMarginUU, derived at the point of use, because TrailRadius is ALSO the half width
	 * of the lethal volume and of the drawn ribbon. A second, independently editable copy of the
	 * trace's half width is precisely how "lethal == drawn" gets broken by a well-meaning tune: set
	 * the clearance below the real half width and the ribbon is pulled out by less than it sticks
	 * out, leaving the bug; set it above and the fitter starts refusing legal routes. One number,
	 * one meaning, and TrailWallFitMarginUU is the tuning surface on top of it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Wall Clearance By Half Width [v13 §7]"))
	bool bTrailWallClearanceEnabled = true;

	/**
	 * Most extra points the fitter may insert for ONE appended trail point. Pairs with
	 * Trace.Trail.WallFitMaxInsert.
	 *
	 * A 60 uu chord subdivided by the carrier's real path needs one or two extra points around a
	 * pillar, so 6 is generous. It exists so a pathological case — a carrier standing inside
	 * geometry, a mesh no capsule can clear — degrades to the old chord instead of flooding the
	 * replicated fast array, which is a bandwidth cliff and an ordering hazard on clients.
	 *
	 * Raising it costs replication on exactly the frames that already cost the most. If the fitter
	 * reports unroutable appends, look at TrailWallFitMarginUU first — an over-large margin is a far
	 * likelier cause than a budget that is genuinely too small.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Wall Fit Max Inserted Points [v12 §6]", ClampMin = "0", ClampMax = "64", UIMin = "0", UIMax = "16"))
	int32 TrailWallFitMaxInsert = 6;

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
	//
	// ------------------------------------------------------------------------------------------
	// READ THIS BEFORE TOUCHING THE FOUR "[legacy]" KNOBS. Spec v6 §2 replaced the character-shaped
	// after-images with one continuous curved RIBBON, which is what ships and what the renderer
	// draws at Trace.Trail.Renderer 1 (the default). Four of the five knobs below —
	// TraceGhostSpacingUU, MaxTraceGhosts, TraceSmearGlowScale, TraceGhostForcedLOD — now affect
	// ONLY the legacy arm, i.e. only if somebody sets Trace.Trail.Renderer 0 to A/B the old look.
	//
	// They are kept rather than deleted because that A/B is how the performance claim in this pass
	// was measured and is the only way to reproduce it, and they are RELABELLED rather than left
	// alone because a settings panel that offers a live-looking slider for a renderer nobody is
	// running is the same silent lie as a knob bound to a misspelled name. The fifth,
	// TraceGhostGlow, is genuinely live: it drives the ribbon's brightness.
	//
	// Do not "fix" a look problem with these. If the ribbon is wrong, the ribbon's own knobs are
	// Trace.Trail.RibbonStep, Trace.Trail.RibbonWidthScale and Trace.Trail.OwnerHideCameraRadius.
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
	UPROPERTY(config, EditAnywhere, Category = "Trail|Legacy Renderer", meta = (DisplayName = "[legacy] Ghost Spacing (uu along path)", ClampMin = "20.0", ClampMax = "2000.0", UIMin = "60.0", UIMax = "500.0"))
	float TraceGhostSpacingUU = 220.f;

	/**
	 * Cap on pooled posed-Mannequin after-images PER TRACE. 0 disables ghosts and leaves the trace as
	 * the continuous smear alone. Mirrors the Trace.Trail.GhostMaxCount CVar; keep the two equal.
	 *
	 * This is the number that decides whether spec v4 §2 is affordable, so the arithmetic is worth
	 * restating now that spec v7 §§1-2 changed it: a trace is TrailMaxLengthUU long WHATEVER the
	 * carrier is doing — 1200 uu — capped as ever by MaxTrailPoints x TrailPointSpacing. At 220 uu
	 * spacing that is ~6 ghosts, and dashing no longer stretches the trace the way a time window did
	 * (a dash covers the length faster, it does not make it longer). 20 is now generous headroom.
	 *
	 * Past the cap the OLDEST ghosts are released, never the newest: the newest are the ones an
	 * approaching enemy is judging their dash against. The smear still covers the tail, so the cap
	 * degrades the look and never the continuity — which is the rule the whole feature turns on.
	 *
	 * Only the Core holder emits, so the worst realistic case (a residual trace during a turnover
	 * while the new holder lays a fresh one) is ~2x this in the world, not 10x.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail|Legacy Renderer", meta = (DisplayName = "[legacy] Max Ghosts Per Trace (0 = smear only)", ClampMin = "0", ClampMax = "64", UIMin = "0", UIMax = "40"))
	int32 MaxTraceGhosts = 20;

	/**
	 * THE LIVE ONE. Emissive strength of the brightest layer of the trace on M_TraceNeon — which
	 * since spec v6 §2 is the RIBBON, not a posed after-image. Mirrors Trace.Trail.GhostGlow.
	 *
	 * The name is legacy and the meaning is not: "emissive strength of the brightest layer" is
	 * exactly what it always was, so the number a designer already tuned still means what they tuned
	 * it to. It is deliberately NOT renamed to TraceRibbonGlow, because the four knobs below it ARE
	 * dead weight on the shipping renderer and renaming this one would blur the line between the two
	 * groups at the exact moment that line is the useful information.
	 *
	 * Above roughly 3.5 the team colour clips toward white and you can no longer tell WHOSE trace you
	 * are looking at, which matters more than prettiness — the trace is only lethal to the other team,
	 * so misreading its colour is misreading whether you may run through it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Trail", meta = (DisplayName = "Trace Ribbon Glow", ClampMin = "0.0", ClampMax = "8.0", UIMin = "0.5", UIMax = "4.0"))
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
	UPROPERTY(config, EditAnywhere, Category = "Trail|Legacy Renderer", meta = (DisplayName = "[legacy] Smear Glow (fraction of ribbon glow)", ClampMin = "0.02", ClampMax = "4.0", UIMin = "0.1", UIMax = "1.5"))
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
	UPROPERTY(config, EditAnywhere, Category = "Trail|Legacy Renderer", meta = (DisplayName = "[legacy] Ghost Forced LOD (0 = auto)", ClampMin = "0", ClampMax = "4"))
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
	 * with no travel URL. This is what an unattended run, an automated test and a first-time player
	 * all get.
	 *
	 * EASY -> NORMAL THIS PASS (spec v9 §9, verbatim: "Set default startup bot difficulty to
	 * normal"). Easy was chosen when the bots could kill an idle human in under two seconds and the
	 * curve was being retuned; the curve has since been derived rather than guessed, and Normal is
	 * the intended out-of-the-box opponent.
	 *
	 * PINNED IN Config/DefaultGame.ini AS WELL (BotDifficulty=Normal under
	 * [/Script/Trace.TraceSettings]) AND THE INI WINS. This literal is what a reader sees; the ini
	 * is what a match uses. Keep the two equal, and verify from a running game with
	 * Trace.VerifyKnobs rather than by reading this line.
	 *
	 * THE TITLE SCREEN HAS ITS OWN DEFAULT and it is NOT this property: TraceDifficulty::Default in
	 * UI/TraceMatchOptions.h is the value the menu row starts on, and the menu then writes
	 * "?difficulty=" onto the travel URL, which outranks everything here. Both have to say Normal
	 * for §9 to be satisfied.
	 *
	 * LATCHED PER MATCH. Editing this mid-PIE does nothing until the next map load; edit the profile
	 * that is already in force instead, which retunes immediately.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots", meta = (DisplayName = "Default Bot Difficulty"))
	EBotDifficulty BotDifficulty = EBotDifficulty::Normal;

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
	// commit band, wall clearance — stay absolute, because a dash is 594uu no matter how big the
	// arena is.
	// ------------------------------------------------------------------------------------------

	/**
	 * Perpendicular distance to the trail line inside which a hunting bot commits its dash, in uu.
	 *
	 * Must stay comfortably under the dash's own reach (DashSpeed * DashDuration, ~594uu by
	 * default) or the dash stops short of the trail and the signature kill never lands. ABSOLUTE,
	 * not field-relative: it is a property of the dash. Sane range 0.6 to 0.85 of dash reach.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Trail Dash Commit Range (uu)", ClampMin = "10.0", ClampMax = "3000.0", UIMin = "100.0", UIMax = "1000.0"))
	float BotTrailDashRange = 380.f;

	/** How far past the trail an interceptor aims, in uu, so the dash sweeps THROUGH it and not up to it. */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Trail Cross Overshoot (uu)", ClampMin = "0.0", ClampMax = "3000.0", UIMin = "50.0", UIMax = "800.0"))
	float BotTrailCrossOvershoot = 320.f;

	/**
	 * How much of the OLDEST end of the trace a hunting bot refuses to aim at, expressed as the
	 * seconds of walking that stretch of trace represents (x WalkSpeed = the uu skipped from the tail).
	 *
	 * A bot that commits to a point which vanishes before it arrives is a bot that spends the whole
	 * carry running at ghosts — one of the two reasons trail kills were once 1.3% of deaths.
	 *
	 * SPEC v7 §1 CHANGED WHAT THIS MEANS, AND THE INTEGRATOR CHANGED THE FILTER TO MATCH. Points no
	 * longer expire by age at all, so the old reading — "points with less than this many seconds of
	 * LIFE LEFT" — became unsatisfiable: for a stationary carrier every point is older than any age
	 * cutoff, so the bots discarded the whole trace and never planned an intercept. The filter in
	 * ATraceBotController is now a DISTANCE margin measured from the tail:
	 *
	 *     skip = BotTrailMinPointLifeRemaining x WalkSpeed   (0.40 x 800 = 320uu)
	 *
	 * which against the 1200uu TrailMaxLengthUU preserves exactly the calibrated "discard the oldest
	 * ~20-25% of the trace" this number has always meant. It stays a fraction of the trace written as
	 * an absolute — so if TrailMaxLengthUU moves, move this with it, the same standing rule as before.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots|Intercept", meta = (DisplayName = "Min Point Life Remaining (s, x WalkSpeed = uu skipped from tail)", ClampMin = "0.0", ClampMax = "4.0", UIMin = "0.0", UIMax = "2.0"))
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
	 * How long a bot holds the crouch input once it decides to slide, as an UPPER BOUND.
	 *
	 * *** THE "MUST TRACK SlideDuration" RULE IS NOW ENFORCED IN CODE, NOT IN THIS COMMENT
	 * (spec v24 §0). *** This used to say "keep at or under SlideDuration" and nothing checked it,
	 * so when spec v24 §8 cut the effective slide from 1.26 s to 0.86 s a bot carried on holding
	 * crouch for the full 1.60 s and crouch-walked for 0.74 s after its slide had ended.
	 *
	 * Its only reader (TraceBotController.cpp, the slide branch) now clamps this against the live
	 * UTraceCharacterMovementComponent::GetSlideDuration(), so the effective hold is
	 * min(this, the real slide) and re-tuning SlideDuration / SlideMaxLengthScale /
	 * SlideDurationTrimSeconds moves the bots automatically. Deliberately NOT re-typed to 0.86:
	 * re-tuning an absolute is exactly what §0 forbids, and leaving it high keeps its meaning as
	 * "a bot rides its slide to the end" whatever the slide's length becomes.
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
	// ABILITIES — spec v14 §§4, 5 and 6
	//
	// EVERY constant from every character in spec §6 is here, categorised, clamped and tooltipped,
	// and every one of them is checked by Trace.VerifyKnobs. Nothing in an ability may be a header
	// literal: the user's own framing for this pass is "all of this will need extensive tuning",
	// which is a request for knobs first and balance second.
	//
	// THREE RULES FOR THIS BLOCK.
	//
	// 1. DERIVE, DO NOT DUPLICATE. Where §6 gives a number that is really a fraction of an existing
	//    one, the knob here is the FRACTION and the base stays where it already lives. Mace's magnet
	//    is "+30% ... The base is now 450 uu, so Mace's is 585 uu. Derive it, do not hardcode" —
	//    so MaceMagnetRadiusBonus is 0.30 and CoreCatchRadius stays the only place 450 is written.
	//    Same for the Spike's pull speed (the air-strafe hard cap) and the Ripple's dash speed.
	//
	// 2. NOTHING HERE CAN TURN OFF THE CARRIER RULE. There is no "abilities may damage carriers"
	//    knob and there must never be one — spec §4 is an invariant, not a tuning value. The single
	//    exception is bCarrierImmuneToAbilityControl, which is the knob for the §4 [ASSUMPTION]
	//    about slows and pulls, and it is clearly marked as such.
	//
	// 3. THE INI WINS. Every value below is also written in Config/DefaultGame.ini. Read them back
	//    from a running game with Trace.DumpSettings or Trace.VerifyKnobs, never from this header.
	// ==========================================================================================

	// ------------------------------------------------------------------------------------------
	// Framework
	// ------------------------------------------------------------------------------------------

	/**
	 * SPEC §3, verbatim: "Include a toggle in game settings to turn off all characters."
	 *
	 * Off means everybody plays the default characterless Mannequin — no select screen, no passives,
	 * no E, no V. The whole framework becomes inert rather than partially active, which is the only
	 * safe meaning for a switch a playtest will flip mid-session.
	 *
	 * NOTE THIS IS NOT THE ONLY THING THAT DISABLES CHARACTERS. Mode A does too, unconditionally and
	 * with no knob (spec §2 freezes mode A). UTraceAbilityComponent::AreCharactersEnabled answers
	 * for both, and is the only correct way to ask.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Framework", meta = (DisplayName = "Characters And Abilities Enabled [v14 §3]"))
	bool bCharactersEnabled = true;

	/**
	 * *** THE SPEC §4 [ASSUMPTION], AND THE ONE KNOB THAT TOUCHES THE CARRIER RULE. ***
	 *
	 * Damage to a carrier is an invariant with no knob. This is about the OTHER half: slows, pulls
	 * and knockbacks. Spec §4, verbatim: "slows, pulls and knockbacks do NOT apply to a carrier
	 * either — the doc explicitly exempts the carrier from Chut's bash, and a carrier who can be
	 * yanked by Oyster's Pickler or slowed by poison is functionally disabled without being damaged,
	 * which is against the spirit. Flag this clearly as an assumption they may want to reverse
	 * per-ability."
	 *
	 * True (shipped) = a carrier cannot be bashed, pulled or slowed by any ability.
	 * False = every ability's control effect applies to carriers again. Damage is unaffected either
	 * way — nothing about this knob can make an ability damage a carrier.
	 *
	 * IT IS ONE SWITCH, NOT PER-ABILITY, and that is deliberate: the assumption is global, and five
	 * per-ability switches would be five places for it to be half-reversed. If the answer comes back
	 * "per-ability", the right change is an argument on ETraceAbilityEffect, not four more bools.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Framework", meta = (DisplayName = "Carrier Immune To Ability Slows/Pulls/Knockbacks [v14 §4 ASSUMPTION]"))
	bool bCarrierImmuneToAbilityControl = true;

	/**
	 * The cooldown an activated (E) ability costs when its character does not override it.
	 *
	 * 20 s is what §6 gives for Rocco's Ripple and Chut's Chud outright, and is the [ASSUMPTION]
	 * cooldown for Mace's Spike and Oyster's Pickler, which the doc leaves unspecified. X's Sting is
	 * the one stated exception at 25 s.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Framework", meta = (DisplayName = "Default Activated Cooldown (s)", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float AbilityDefaultCooldownSeconds = 20.f;

	/**
	 * Seconds a player has on the character select screen before one is assigned for them.
	 *
	 * Spec §3 [ASSUMPTION]: "a timeout that auto-assigns a free character, so one idle player cannot
	 * stall the match." Zero disables the timeout, which is a legitimate thing to want in a private
	 * playtest and a bad idea anywhere else.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Framework", meta = (DisplayName = "Character Select Timeout (s, 0 = never) [v14 §3]", ClampMin = "0.0", ClampMax = "300.0", UIMin = "0.0", UIMax = "60.0"))
	float CharacterSelectTimeoutSeconds = 30.f;

	// ------------------------------------------------------------------------------------------
	// ROCCO — spec §6
	//
	// Passive: "3% speed boost from headshot kills for 1 second, stacking, each kill extends the
	// timer on the entire boost."  *** SPEC v24 §11 MADE THAT ONE SECOND THREE. *** Verbatim:
	// "Change Rocco's passive to last for 3s rather than 1s, keep everything else intact" — so
	// RoccoHeadshotSpeedDurationSeconds moved and NOTHING else did: still +3% a stack, still capped
	// at 10, still ONE shared timer that each kill refreshes for the whole boost.
	//                    Movement: "a very small second jump, which allows Rocco to change
	// direction midair, instantly."  Activated (Ripple): a dash on its own cooldown leaving rings
	// any character of either team — INCLUDING THE CORE CARRIER — may ride, for 4 s, 20 s cooldown.
	// ------------------------------------------------------------------------------------------

	/** +3% max speed per headshot kill. The whole stack shares ONE timer, refreshed by each kill. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Headshot Speed Bonus Per Stack (fraction)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.15"))
	float RoccoHeadshotSpeedBonusPerStack = 0.03f;

	/**
	 * Cap on the stack. Spec §6 [ASSUMPTION]: "cap the stack (say 10 = +30%) — unbounded is a bug
	 * waiting to happen; make the cap a knob." This is that knob. At the shipped 3% and 10 it is
	 * +30% max, which is roughly the carrier's own speed multiplier and therefore a sane ceiling.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Headshot Speed Stack Cap [v14 §6 ASSUMPTION]", ClampMin = "1", ClampMax = "50", UIMin = "1", UIMax = "20"))
	int32 RoccoHeadshotSpeedStackMax = 10;

	/**
	 * The single timer covering the whole stack. Each headshot kill refreshes it — it never splits.
	 *
	 * *** SPEC v24 §11: 3 s, was 1 s. THE ONLY THING §11 CHANGES. *** The stacking behaviour and the
	 * "each kill extends the timer on the entire boost" rule are untouched — they live in
	 * UTraceAbilitySetRocco::OnKill, which reads this knob and assigns (never adds) the new deadline,
	 * so a longer window is genuinely a longer window and not a window that now accumulates.
	 *
	 * WHAT IT COSTS IN PRACTICE: at 1 s a second headshot had to land inside one second to keep a
	 * stack alive, which in a 150 uu/s duel is most of a magazine's worth of luck. At 3 s the stack
	 * survives a reload, so the cap (10 = +30%) is now reachable in an ordinary fight rather than in
	 * theory — which is why the cap is a knob and why it did NOT move with this.
	 *
	 * Mirrored in Config/DefaultGame.ini, and the ini is the one that decides.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Headshot Speed Duration (s) [v24 §11: 3, was 1]", ClampMin = "0.05", ClampMax = "30.0", UIMin = "0.5", UIMax = "5.0"))
	float RoccoHeadshotSpeedDurationSeconds = 3.f;

	/**
	 * The second jump's upward speed. "A VERY SMALL second jump" — this is deliberately a fraction
	 * of a real jump, because §6 says "the direction change is the point, not the height."
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Second Jump Z Velocity (uu/s)", ClampMin = "0.0", ClampMax = "1200.0", UIMin = "0.0", UIMax = "500.0"))
	float RoccoSecondJumpZVelocity = 260.f;

	/**
	 * How completely the second jump replaces horizontal velocity with the new wish direction.
	 * 1 = "change direction midair, INSTANTLY" (the spec's word). 0 = no redirect at all, which
	 * would make the ability pointless — this exists so the instantaneous version can be softened
	 * if it feels teleporty, not so it can be switched off.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Second Jump Direction Change (0-1, 1 = instant)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.5", UIMax = "1.0"))
	float RoccoSecondJumpRedirectFraction = 1.f;

	/** Ripple lifetime. v14 §6 said "Lasts 4 s"; Demo 17 raised it to 5.5 s. Effects and visuals
	    still vanish together at expiry. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Lifetime (s)", ClampMin = "0.25", ClampMax = "30.0", UIMin = "1.0", UIMax = "10.0"))
	float RoccoRippleLifetimeSeconds = 5.5f;

	/** §6: "20 s cooldown", and it is SEPARATE from the standard dash's cooldown. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Cooldown (s)", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float RoccoRippleCooldownSeconds = 20.f;

	/** Ripple dash speed, as a multiple of Movement|Dash > Dash Speed. DERIVED — see rule 1 above. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Dash Speed (x Dash Speed)", ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0"))
	float RoccoRippleDashSpeedMultiplier = 0.636364f;

	/** Ripple dash duration, as a multiple of Movement|Dash > Dash Duration. Sets the path's length. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Dash Duration (x Dash Duration)", ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0"))
	float RoccoRippleDashDurationMultiplier = 1.f;

	/** Speed a RIDER is propelled along the path, as a multiple of Dash Speed. Riders may shoot. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Ride Speed (x Dash Speed)", ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0"))
	float RoccoRippleRideSpeedMultiplier = 0.636364f;

	/** How close to the START ring a player must be to be picked up. §6: entry is at the start only. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Entry Radius (uu)", ClampMin = "20.0", ClampMax = "1000.0", UIMin = "60.0", UIMax = "300.0"))
	float RoccoRippleEntryRadiusUU = 140.f;

	/** Spacing of the rings drawn along the path. §6: "a short series of rings along the path". */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Ring Spacing (uu)", ClampMin = "20.0", ClampMax = "2000.0", UIMin = "100.0", UIMax = "500.0"))
	float RoccoRippleRingSpacingUU = 220.f;

	/** Radius of each ring. Cosmetic, but it must read as an entrance rather than a decal. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Ring Radius (uu)", ClampMin = "10.0", ClampMax = "500.0", UIMin = "50.0", UIMax = "200.0"))
	float RoccoRippleRingRadiusUU = 90.f;

	/**
	 * §6: "the starting ring in a different colour so it is obvious where to take it."
	 *
	 * ROCCO'S ACCENT — acid gold #E7FF89 — and the reason it is HIS and not a generic "entrance"
	 * colour is bible §6.2 invariant 2: an ability's world actor wears its owner's accent. The bible
	 * blesses this exact split in as many words ("rings neutral-pale with amber start ring is
	 * acceptable — the start ring must remain 'a different color', Demo.13"), so the entrance says
	 * both "Rocco laid this" and "get on here".
	 *
	 * *** THIS IS A COPY OF THE ACCENT, AND IT IS THE ONE KIND OF COPY THAT IS ALLOWED. ***
	 * THE SOURCE OF TRUTH IS TraceCharacterRoster::All()[Rocco].Accent (Core/TraceCharacterRoster.cpp
	 * — which is also what generates DA_Character_Rocco and stamps MI_Body_Rocco_Accent). It is
	 * duplicated here ONLY because a UPROPERTY(config) default cannot be a function call and because
	 * this is a DESIGNER KNOB: the point of the property is that somebody can set the start ring to
	 * something that is not Rocco's accent. Every OTHER copy of an accent in the FX code has been
	 * deleted in favour of a live read (ATraceFxBurst::HueFor, ATraceMaceSpike, ATraceElleGate,
	 * Lily's flight, the Slimewall dressing) precisely because those had no such excuse.
	 *
	 * *** IF THE ACCENT IS RE-TUNED, THIS LINE AND ITS Config/DefaultGame.ini OVERRIDE MOVE BY HAND.
	 *     THEY DID NOT, ONCE, AND THAT IS WHY THIS PARAGRAPH EXISTS. *** The v25 re-space rotated all
	 * ten accents off the two team hues (Rocco #FFEF89 -> #E7FF89, amber -> acid gold, +20.3 deg) and
	 * this default and the ini line kept the old amber, so the ripple a player stepped onto was a
	 * different colour from the Rocco who laid it. Trace.VerifyCharacterData does not cover this
	 * pair; a grep for the accent literal is what catches it.
	 *
	 * *** A PLAIN 0..1 HUE, AND IT MUST STAY ONE. *** ATraceRippleActor pushes this into M_TraceNeon,
	 * whose emissive is Color x Glow, and a material instance clamps vector parameters to [0,1] —
	 * brightness put in here would not brighten the ring, it would silently rewrite its hue. The
	 * brightness knob is the ring's Glow (bible §3.2 tier T2, 3.5), which lives in the actor. The new
	 * accent's brightest channel is 1.00, exactly as the old one's was, so no glow moved with it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple START Ring Colour [v14 §6]"))
	FLinearColor RoccoRippleStartRingColor = FLinearColor(0.80f, 1.00f, 0.25f, 1.f);

	/**
	 * Every ring after the first. NeonNeutralPale #8CEBFF — the arena's own neutral neon, so the path
	 * reads as a piece of the world that has been switched on rather than as a second coloured thing
	 * competing with the entrance. One hue per effect (bible §6.2): the amber IS the effect's colour
	 * and the trail is deliberately the neutral it is measured against.
	 *
	 * Same 0..1 rule as the start ring above.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Rocco", meta = (DisplayName = "Ripple Trail Ring Colour"))
	FLinearColor RoccoRippleTrailRingColor = FLinearColor(0.55f, 0.92f, 1.f, 1.f);

	// ------------------------------------------------------------------------------------------
	// CHUT — spec §6
	//
	// Passive: knife deals 50 from the front (vs the standard 30); the 60° back zone stays 100.
	// Movement: bash — the END of his standard dash knocks a player in his direction of travel,
	// with NO EFFECT ON THE CORE CARRIER (the doc's own words, and the reason the choke point has a
	// Control class at all).  Activated (Chud): 30% less damage from body shots and melees for 10 s,
	// refreshed by a knife kill, does not stack, 20 s cooldown.
	// ------------------------------------------------------------------------------------------

	/** Chut's front knife damage. The standard is UTraceMeleeSettings::FrontDamage (30). */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Knife Front Damage [v14 §6: 50 vs the standard 30]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "10.0", UIMax = "120.0"))
	float ChutKnifeFrontDamage = 50.f;

	/**
	 * Chut's back-stab damage. §6: "back damage stays the standard number."
	 *
	 * *** NO READER SINCE 2026-08-24 (RESTRUCTURE C6). *** UTraceAbilitySetChut::ModifyOutgoingDamage
	 * now PASSES THE BASE NUMBER THROUGH for a back-stab instead of returning this copy of it, so
	 * that retuning the standard back-stab (UTraceMeleeSettings::BackstabDamage) carries Chut with it
	 * — which is what "stays the standard" means. Retained rather than deleted for config
	 * compatibility: an existing DefaultGame.ini or a user ini may still set it, and removing a
	 * UPROPERTY this close to release churns ini handling for no gain. Setting it now does nothing.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Knife Back Damage [v14 §6 ASSUMPTION: unchanged at 100]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "50.0", UIMax = "200.0"))
	float ChutKnifeBackDamage = 100.f;

	/**
	 * Speed the bash imparts, along Chut's direction of travel. NEVER applies to a Core carrier.
	 *
	 * *** 1000 -> 1250 -> 1118 THIS PASS, AND THE MIDDLE VALUE IS THE INTERESTING ONE. *** Spec v16
	 * §0 asked for *"Increase the distance which Chut's bash knocks players by 25%"* — a DISTANCE —
	 * and this knob is a SPEED. 1250 was the naive +25% on the speed, and §0 flagged it for
	 * measurement rather than shipping it. Trace.Move.AuditV16.Bash measured it twice, in two
	 * separate worlds, through the shipped UTraceAbilitySetChut::TryBash with the victim frozen:
	 *
	 *   run A   1000 uu/s -> air  584.2 uu, total  875.6 uu | 1250 uu/s -> air 907.1 uu, total 1452.0 uu
	 *   run B   1000 uu/s -> air  647.8 uu, total 1530.4 uu | 1103 uu/s -> air 779.4 uu, total 1776.8 uu
	 *
	 * so +25% SPEED bought +65.8% DISTANCE and the naive 1250 had to go.
	 *
	 * *** TUNE THE AIRBORNE LEG, BECAUSE IT IS THE ONLY PART THAT IS REPRODUCIBLE. *** Compare the
	 * two runs at the SAME 1000 uu/s: the air leg agrees to 11% (584 vs 648) but the TOTAL differs by
	 * 75% (876 vs 1530), because the ground slide after touchdown is 291 uu in one world and 883 uu
	 * in the other. The slide is a function of where the bash happened — slope, cover, what the
	 * victim's feet found — not of this knob. Its fitted exponent swung 2.267 -> 1.523 between the
	 * two runs, i.e. the total cannot pick a number and would have picked a different one each time.
	 *
	 * The airborne leg is the one with a closed form — d_air = (2 x ChutBashUpBias / g) x Speed^2 —
	 * so the value is the quadratic solve, 1000 x sqrt(1.25) = **1118 uu/s**.
	 *
	 * *** WHAT 1118 ACTUALLY DELIVERS, MEASURED AND NOT ASSUMED: +21.5% and +22.8% on the airborne
	 * leg over two runs, against the +25% asked. *** It lands slightly under, and the reason is
	 * physical rather than noise: the victim is launched from standing height and lands on the floor,
	 * so the arc is asymmetric and its flight time grows a little slower than the pure form predicts.
	 * The fitted exponent is therefore below 2 — but it came out 1.74, 1.84, 1.89 and 1.97 across
	 * four samples, so solving for it gives a different answer (1103 / 1126 / 1133 / 1158) every time
	 * it is asked. 1118 is the value that does NOT depend on which sample you happened to take, and
	 * ~22% against a target of 25% is inside that spread. Chasing the last two points here means
	 * fitting noise.
	 *
	 * WHAT THIS DOES NOT PROMISE: the TOTAL knock will not be uniformly +25%, because the ground
	 * bleed is terrain's to decide. A single speed cannot make it so. Re-measure with
	 * Trace.Move.AuditV16.Bash (~40 s) before moving this; read the AIRBORNE LEG line, not the total,
	 * and take more than one sample.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Bash Knockback Speed (uu/s) [v16 §0: tuned for +25% DISTANCE, not +25% speed]", ClampMin = "0.0", ClampMax = "5000.0", UIMin = "300.0", UIMax = "2000.0"))
	float ChutBashKnockbackSpeed = 1118.f;

	/** Upward component of the bash, as a fraction of the knockback speed. A little pop reads better. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Bash Upward Bias (fraction of knockback)", ClampMin = "0.0", ClampMax = "1.5", UIMin = "0.0", UIMax = "0.6"))
	float ChutBashUpBias = 0.3f;

	/**
	 * What counts as "the END of his standard dash": the last this fraction of the dash's duration.
	 * §6 is specific about the end rather than the whole dash, so this must never default to 1.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Bash Window (final fraction of the dash)", ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.1", UIMax = "0.6"))
	float ChutBashEndFraction = 0.35f;

	/** How close the bash reaches. Roughly a capsule and a half at the shipped value. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Bash Radius (uu)", ClampMin = "20.0", ClampMax = "800.0", UIMin = "60.0", UIMax = "250.0"))
	float ChutBashRadiusUU = 130.f;

	/** §6: "30% less damage from body shots and melees for 10 s." This is the 0.30. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Chud Damage Reduction (fraction)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.6"))
	float ChudDamageReduction = 0.3f;

	/** §6: 10 s. Does not stack; a knife kill REFRESHES this window rather than adding to it. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Chud Duration (s)", ClampMin = "0.5", ClampMax = "60.0", UIMin = "2.0", UIMax = "20.0"))
	float ChudDurationSeconds = 10.f;

	/** §6: 20 s. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Chud Cooldown (s)", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float ChudCooldownSeconds = 20.f;

	/** §6: "The timer refreshes on a knife kill." Off makes Chud a plain 10 s window. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Chut", meta = (DisplayName = "Chud Refreshes On Knife Kill"))
	bool bChudRefreshesOnKnifeKill = true;

	// ------------------------------------------------------------------------------------------
	// MACE — spec §6
	//
	// Passive: +30% magnet radius for the Core arcing to her. DERIVED from CoreCatchRadius.
	// Movement: hold V in the air to suspend for up to 1.25 s, gravity off, lateral movement capped
	// at 550 uu/s; releasing V cancels immediately.
	// Activated (Spike): a roped spike; embeds in a WALL for 2 s; reactivating pulls her at the
	// momentum ceiling; ANY movement input cancels; she obeys physics and can shoot and be shot.
	// ------------------------------------------------------------------------------------------

	/**
	 * §6: "+30% magnet radius ... The base is now 450 uu (reduced 10% in Demo 12), so Mace's is
	 * 585 uu. DERIVE IT, DO NOT HARDCODE." So this is the 0.30 and the base stays CoreCatchRadius
	 * under Core|Mode B. 450 x 1.30 = 585. If somebody retunes the base, Mace follows for free.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Magnet Radius Bonus (fraction of Core Catch Radius) [v14 §6: derive, do not hardcode]", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.6"))
	float MaceMagnetRadiusBonus = 0.3f;

	/** §6: "suspend for up to 1.25 s". A hard cap, not a resource — releasing V ends it early. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Suspend Max Duration (s)", ClampMin = "0.05", ClampMax = "10.0", UIMin = "0.25", UIMax = "3.0"))
	float MaceSuspendMaxSeconds = 1.25f;

	/** §6: "she may move laterally capped at 550 uu/s" while suspended. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Suspend Lateral Speed Cap (uu/s)", ClampMin = "0.0", ClampMax = "3000.0", UIMin = "200.0", UIMax = "1200.0"))
	float MaceSuspendLateralSpeedCap = 550.f;

	/**
	 * DEMO 17 item 6, verbatim: "Add a 3 second cooldown to Mace's V. Time it from when she releases V
	 * or the suspend expires, not from when it started. Do not show it on the HUD."
	 *
	 * *** TIMED FROM THE END, AND THAT IS WHERE THE CODE ALREADY PUTS IT. *** It is stamped by
	 * UTraceAbilitySetMace::StopSuspend(), which is the ONE exit every way out of a suspend goes
	 * through — the key coming up, the 1.25 s cap elapsing, landing, a pull starting, dying. So a
	 * player who holds the full 1.25 s waits 3 s from the moment she drops, i.e. 4.25 s after the
	 * press, and a player who taps V waits 3 s from the tap. Timing it from the START would have made a
	 * long hold cost less than a short one, which is backwards.
	 *
	 * *** HIDDEN. *** Nothing draws it: ATraceHUD's Mace branch draws a SUSPENDED chip while
	 * IsSuspending() is true and nothing at all when it is not, and this character does not override
	 * GetCharacterOwnedCooldownRemaining(), so the E ring never learns about it either. The player is
	 * meant to find it by feel, which is what Demo 17 asked for.
	 *
	 * §6 specified no cooldown at all, so this was 0 until Demo 17. 0 restores that and is the red arm.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Suspend Cooldown (s, from RELEASE or expiry; NOT drawn on the HUD) [Demo 17: 3]", ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "10.0"))
	float MaceSuspendCooldownSeconds = 3.f;

	/**
	 * §6: "throws a roped spike in her aim direction for a MEDIUM distance."
	 *
	 * v15 §6: "expand the range for its use 200%". [ASSUMPTION] "200% more" read as x3, so 2200 -> 6600.
	 * MEASURED WHY IT MATTERED: at 2200 uu, Trace.Mace.SpikeConsistency's arena census found a wall on
	 * only 21.6% of 648 aim directions taken from real player positions — three times out of four,
	 * looking somewhere and pressing E did nothing at all. That is most of what "inconsistent" was.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Spike Range (uu) [v15 §6: 2200 -> 6600, +200%]", ClampMin = "100.0", ClampMax = "20000.0", UIMin = "800.0", UIMax = "8000.0"))
	float MaceSpikeRangeUU = 6600.f;

	/**
	 * How fast the spike travels to its anchor. Fast enough to feel like a grapple, not a grenade.
	 *
	 * v15 §6: "make it 300% faster". [ASSUMPTION] "300% faster" read as x4, so 5000 -> 20000. At the
	 * raised range that is a 0.33 s flight for a maximum throw, against 1.32 s if the old speed had
	 * been kept — the range change on its own would have made the ability SLOWER to use.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Spike Travel Speed (uu/s) [v15 §6: 5000 -> 20000, +300%]", ClampMin = "100.0", ClampMax = "30000.0", UIMin = "2000.0", UIMax = "24000.0"))
	float MaceSpikeTravelSpeed = 20000.f;

	/**
	 * THE WALL TEST. "Largest |Normal.Z| the spike will still call a wall."
	 *
	 * This USED to read WallJumpMaxNormalZ (0.40), on the reasoning that the project should have one
	 * definition of a wall. MEASURED, that was wrong for the spike: 0.40 is a surface 66 degrees off
	 * horizontal, so everything between the 45-degree walkable limit and 66 degrees was refused —
	 * geometry she cannot stand on, cannot walk up, and could not spike either. Trace.Mace.
	 * SpikeConsistency scored 0/14 on plates at |normal.Z| 0.50 and 0.64 with that threshold.
	 *
	 * 0.70 is the walkable floor limit (cos 45 degrees = 0.707), so the rule is now exactly "if she
	 * cannot walk on it, she can spike it" — and a ramp at |normal.Z| 0.82 is still refused, which
	 * the same harness proves in its FIZZLE arm.
	 *
	 * SEPARATE FROM WallJumpMaxNormalZ ON PURPOSE. The wall jump's 0.40 is a movement feel decision
	 * that has been tuned against; moving it to fix the spike would have retuned the wall jump by
	 * accident, and the two rules have no reason to be the same number.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Spike Wall Test: max |normal.Z| [v15 §6]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.2", UIMax = "0.9"))
	float MaceSpikeMaxSurfaceNormalZ = 0.7f;

	/**
	 * Radius of the FALLBACK sweep the throw runs when the exact aim ray finds nothing to stick to.
	 * 0 disables it and leaves the throw a pure line trace, which is what it was before v15 §6.
	 *
	 * WHY IT EXISTS. The throw resolved its anchor with an infinitely thin line, so a 40 uu pillar at
	 * 1200 uu was missed by one degree of aim error and a 16 uu railing by half a degree — while the
	 * crosshair was visibly still on them. Measured: 5/7 and 3/7. The exact ray is still tried FIRST
	 * and still wins whenever it lands, so nothing about a clean shot changes; this only catches the
	 * ones that used to be silent nothings.
	 *
	 * 16 uu is the spike head's own radius (the cone mesh at 0.28 scale), which is the honest answer
	 * to "how forgiving should it be" — as forgiving as the thing being thrown is wide.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Spike Aim Forgiveness Radius (uu, 0 = exact line only) [v15 §6]", ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "32.0"))
	float MaceSpikeTraceRadiusUU = 16.f;

	/** §6: "On hitting a wall it embeds for 2 s." After that it is gone whether she used it or not. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Spike Embed Duration (s)", ClampMin = "0.1", ClampMax = "30.0", UIMin = "0.5", UIMax = "6.0"))
	float MaceSpikeEmbedSeconds = 2.f;

	/**
	 * Pull speed as a multiple of the momentum ceiling. v14 §6: "pulls her toward it AT THE MOMENTUM
	 * CEILING (the air-strafe hard cap)". DERIVED — the ceiling is AirStrafeHardCapSpeed x
	 * AirStrafeAsymptoteScale under Movement|Air, and it stays the only place that number lives.
	 *
	 * 2.0, NOT the 1.0 v14 derived, and the reason is measured. v15 §6 made the throw 4x faster and
	 * the range 3x longer, and verification then showed the PULL had become the slow part: it runs
	 * at 1375 uu/s, so crossing the new 6600 uu range takes 4.8 s against a 0.33 s throw. The user's
	 * complaint was "her ability is way too slow", and at 1.0 that complaint would have been only
	 * half answered — the spike would arrive quickly and then she would crawl to it.
	 *
	 * This deliberately breaks v14 §6's "at the momentum ceiling" derivation, because that rule was
	 * written when the range was 2200 uu. The air cap itself is untouched; only Mace's pull scales.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Spike Pull Speed (x air-strafe hard cap) [v15 §6: 1.0 -> 2.0, the pull became the slow part]", ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.5", UIMax = "3.0"))
	float MaceSpikePullSpeedMultiplier = 2.f;

	/** How close to the spike counts as arrived, ending the pull. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Spike Arrive Radius (uu)", ClampMin = "20.0", ClampMax = "600.0", UIMin = "60.0", UIMax = "250.0"))
	float MaceSpikeArriveRadiusUU = 150.f;

	/** §6 [ASSUMPTION]: "cooldown 20 s to match the others; flag it as unspecified." Flagged. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mace", meta = (DisplayName = "Spike Cooldown (s) [v14 §6 ASSUMPTION: unspecified]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float MaceSpikeCooldownSeconds = 20.f;

	// ------------------------------------------------------------------------------------------
	// OYSTER — spec §6
	//
	// Passive: a poison jar at the START OF EVERY DASH, including while carrying the Core. An enemy
	// touching a jar breaks it: 3 damage every 0.5 s for 4 s and −30% speed for 4 s to nearby
	// enemies. Jars last 4 s on the ground, max 3, a fourth despawns the oldest.
	// Movement: jumping while stood on one of his jars breaks it and boosts him upward.
	// Activated (Pickler): a lobbed jar that ON LANDING deals 30 area damage and pulls enemies in a
	// small radius toward it — AND THEN PERSISTS as a normal jar (the doc's own clarification).
	//
	// EVERY ONE OF THESE EFFECTS IS SUBJECT TO THE CHOKE POINT. Spec §4 names Oyster's poison ticks
	// and Pickler's 30 area damage as specific risks: neither may touch a Core carrier, and neither
	// may slow or pull one while the §4 [ASSUMPTION] stands.
	// ------------------------------------------------------------------------------------------

	/** §6: jars "last 4 s on the ground". */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Jar Lifetime (s)", ClampMin = "0.25", ClampMax = "60.0", UIMin = "1.0", UIMax = "10.0"))
	float OysterJarLifetimeSeconds = 4.f;

	/** §6: "Max 3; a fourth despawns the oldest." */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Max Live Jars", ClampMin = "1", ClampMax = "20", UIMin = "1", UIMax = "6"))
	int32 OysterMaxJars = 3;

	/** How close an enemy must come to break a jar by touching it. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Jar Break Radius (uu)", ClampMin = "20.0", ClampMax = "500.0", UIMin = "50.0", UIMax = "200.0"))
	float OysterJarBreakRadiusUU = 100.f;

	/** §6: "3 damage every 0.5 s for 4 s". This is the 3. Damage, so it never reaches a carrier. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Poison Damage Per Tick", ClampMin = "0.0", ClampMax = "100.0", UIMin = "1.0", UIMax = "15.0"))
	float OysterPoisonDamagePerTick = 3.f;

	/** §6: every 0.5 s. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Poison Tick Interval (s)", ClampMin = "0.05", ClampMax = "5.0", UIMin = "0.1", UIMax = "1.5"))
	float OysterPoisonTickIntervalSeconds = 0.5f;

	/** §6: for 4 s. At the shipped tick that is 8 ticks and 24 damage total. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Poison Duration (s)", ClampMin = "0.25", ClampMax = "30.0", UIMin = "1.0", UIMax = "10.0"))
	float OysterPoisonDurationSeconds = 4.f;

	/** §6: "−30% speed for 4 s". A CONTROL effect — blocked on carriers by the §4 [ASSUMPTION]. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Poison Slow (fraction) [CONTROL: see the §4 assumption]", ClampMin = "0.0", ClampMax = "0.95", UIMin = "0.0", UIMax = "0.6"))
	float OysterPoisonSlowFraction = 0.3f;

	/** How far the burst reaches. §6 says "poisoning nearby enemies" and leaves the radius open. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Poison Burst Radius (uu)", ClampMin = "50.0", ClampMax = "3000.0", UIMin = "200.0", UIMax = "800.0"))
	float OysterPoisonRadiusUU = 380.f;

	/** §6: "jumping while stood on one of his jars breaks it and boosts him upward." */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Jar-Jump Z Velocity (uu/s)", ClampMin = "0.0", ClampMax = "3000.0", UIMin = "400.0", UIMax = "1600.0"))
	float OysterJarJumpZVelocity = 1050.f;

	/** How close to a jar Oyster must be standing for the jar-jump to trigger instead of a jump. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Jar-Jump Stand Radius (uu)", ClampMin = "20.0", ClampMax = "400.0", UIMin = "50.0", UIMax = "200.0"))
	float OysterJarJumpRadiusUU = 110.f;

	/** §6: Pickler "deals 30 damage in an area" on landing. Damage — never reaches a carrier. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Pickler Impact Damage", ClampMin = "0.0", ClampMax = "300.0", UIMin = "5.0", UIMax = "80.0"))
	float OysterPicklerDamage = 30.f;

	/** The area that 30 covers. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Pickler Damage Radius (uu)", ClampMin = "50.0", ClampMax = "3000.0", UIMin = "200.0", UIMax = "800.0"))
	float OysterPicklerDamageRadiusUU = 420.f;

	/** §6: "pulls enemies within a SMALL radius toward it" — smaller than the damage radius. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Pickler Pull Radius (uu) [CONTROL]", ClampMin = "20.0", ClampMax = "2000.0", UIMin = "100.0", UIMax = "500.0"))
	// SPEC v19 §4.4 "a greater pull radius": 260 -> 380, matching the poison burst radius the game
	// already draws, so the ring a player SEES is the ring that grabs them. The C++ default and
	// Config/DefaultGame.ini's pinned value are kept equal deliberately — when they disagreed the ini
	// silently won and the header lied to anyone reading it for the shipped number.
	float OysterPicklerPullRadiusUU = 380.f;

	/** How hard the pull is. A CONTROL effect — blocked on carriers by the §4 [ASSUMPTION]. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Pickler Pull Speed (uu/s) [CONTROL]", ClampMin = "0.0", ClampMax = "5000.0", UIMin = "300.0", UIMax = "2000.0"))
	float OysterPicklerPullSpeed = 1300.f;

	/**
	 * SPEC v26 §6b: "the E jar should EXPLODE once the pull animation finishes, rather than waiting
	 * for a jump to trigger them."
	 *
	 * How long the Pickler jar lies there after its impact before it detonates, expressed as a
	 * MULTIPLE OF THE PULL'S OWN TRAVEL TIME rather than as a number of seconds. The pull is what
	 * this delay is waiting for, so the pull is the base it is stored against: someone caught at the
	 * very edge of OysterPicklerPullRadiusUU and launched at OysterPicklerPullSpeed arrives in
	 * radius/speed seconds, which at the shipped 380 uu and 1300 uu/s is 0.29 s. Retune either of
	 * those and the fuse follows on its own — a fuse typed here as "0.29" would not, and the jar
	 * would start going off before or after the thing it is named for.
	 *
	 * 1.0 = detonate exactly as the last pulled enemy arrives. Below 1 it goes off under them on the
	 * way in; above 1 it gives them a moment to stand in it. ATraceOysterJar::GetPicklerDetonateDelaySeconds()
	 * does the arithmetic and clamps the result into (one frame .. the jar's own ground lifetime), so
	 * no setting of this can produce a Pickler jar that expires before it explodes.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Pickler Detonate Delay (x pull travel time)", ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.5", UIMax = "3.0"))
	float OysterPicklerDetonateDelayScale = 1.f;

	/** How fast the jar is lobbed. It is a LOB — it arcs, lands, fires its impact, then detonates. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Pickler Throw Speed (uu/s)", ClampMin = "100.0", ClampMax = "10000.0", UIMin = "800.0", UIMax = "3000.0"))
	float OysterPicklerThrowSpeed = 1900.f;

	/** Upward bias on the lob, as a fraction of the throw speed. Zero would be a bullet, not a lob. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Pickler Throw Up Bias (fraction)", ClampMin = "0.0", ClampMax = "1.5", UIMin = "0.0", UIMax = "0.8"))
	float OysterPicklerThrowUpBias = 0.35f;

	/** §6 [ASSUMPTION]: "cooldown 20 s; unspecified." Flagged. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Oyster", meta = (DisplayName = "Pickler Cooldown (s) [v14 §6 ASSUMPTION: unspecified]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float OysterPicklerCooldownSeconds = 20.f;

	// ------------------------------------------------------------------------------------------
	// X — spec §6
	//
	// Passive: orbited by five mechanical bees; an enemy hit by one becomes VULNERABLE for 2 s,
	// taking +25% damage from all sources. Does not stack; a new application RESETS the timer.
	// Movement: +15% speed while ANY enemy is vulnerable (v14 §6 said 10%; Demo 18 raised it).
	// Activated (Sting): loads the 5 bees into his gun; his next five bullets apply vulnerable at
	// NORMAL damage; when all five are fired the bees resume orbiting. 25 s cooldown.
	// ------------------------------------------------------------------------------------------

	/** §6: five bees. The same five are the Sting's five bullets, so this is one number, not two. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Bee Count", ClampMin = "1", ClampMax = "20", UIMin = "1", UIMax = "8"))
	int32 XBeeCount = 5;

	/** How far out the bees orbit. §6 [ASSUMPTION]: "the orbiting bees hit on contact — X's body is the delivery mechanism." */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Bee Orbit Radius (uu)", ClampMin = "20.0", ClampMax = "600.0", UIMin = "60.0", UIMax = "250.0"))
	float XBeeOrbitRadiusUU = 120.f;

	/** How fast they go round. Cosmetic except that it sets which bee is where when a body touches. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Bee Orbit Speed (deg/s)", ClampMin = "0.0", ClampMax = "2000.0", UIMin = "60.0", UIMax = "500.0"))
	float XBeeOrbitSpeedDegPerSecond = 240.f;

	/** A bee's own touch radius, on top of the victim's capsule. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Bee Hit Radius (uu)", ClampMin = "5.0", ClampMax = "300.0", UIMin = "20.0", UIMax = "100.0"))
	float XBeeHitRadiusUU = 50.f;

	/** §6: vulnerable for 2 s. "Does not stack; a new application RESETS the timer." */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Vulnerable Duration (s)", ClampMin = "0.1", ClampMax = "30.0", UIMin = "0.5", UIMax = "6.0"))
	float XVulnerableDurationSeconds = 2.f;

	/**
	 * v14 §6 said "+25% damage from ALL sources". *** SPEC v24 §9 RAISED THE FIRST STACK TO +35%. ***
	 * Verbatim: "Increase X's base vulnerable to 35% extra damage, keeping the 5% extra per stack
	 * after one intact, and all other aspects intact." So this knob — and ONLY this knob — moved:
	 * XVulnerableStackBonus is still 0.05, XVulnerableMaxStacks is still 5, XVulnerableDurationSeconds
	 * is still 2, and the refresh rule is untouched. At the cap the multiplier is now x1.55 (0.35 +
	 * 4 x 0.05), where it used to be x1.45.
	 *
	 * RELATIVE BY CONSTRUCTION (spec v24 §0). This is a FRACTION added to 1, never an absolute damage
	 * number — TraceVulnerable::GetDamageMultiplier() returns 1 + this, and every damage source in the
	 * project is multiplied by it. Retune the gun, the knife or an ability and the mark keeps meaning
	 * "+35% of whatever that hit was worth", with nothing to re-derive.
	 *
	 * NOTE FOR WHOEVER BUILDS X: this is an amplifier on damage, and spec §4 says the vulnerable
	 * mark "must not become a damage path". Applying the mark is a CONTROL effect and is refused on
	 * a carrier; the amplifier itself is harmless because nothing can damage a carrier anyway.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Vulnerable Damage Bonus (fraction) [v24 §9: 0.35, was 0.25]", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.0"))
	float XVulnerableDamageBonus = 0.35f;

	/** Movement passive: speed while ANY enemy is vulnerable — any, not the one he marked.
	    v14 §6 specified 10%; Demo 18 raised it to 15%. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Speed Bonus While Any Enemy Vulnerable (fraction)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.3"))
	float XVulnerableSpeedBonus = 0.15f;

	/** §6: "25 s cooldown" — the one activated ability that is not 20. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Sting Cooldown (s) [v14 §6: 25, not 20]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float XStingCooldownSeconds = 25.f;

	/**
	 * How many loaded bullets carry the mark. §6 says five, and says the bees resume orbiting when
	 * all five are fired — so this and XBeeCount are the same five and should move together.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Sting Loaded Bullets [keep equal to Bee Count]", ClampMin = "1", ClampMax = "20", UIMin = "1", UIMax = "8"))
	int32 XStingBulletCount = 5;

	// ==========================================================================================
	// SPEC v18 §2 — ROXIE, ELLE AND SLIMEBALL
	//
	// *** EVERY KNOB BELOW EXISTS BEFORE THE ABILITY THAT READS IT. THAT IS THE POINT. ***
	//
	// Three character agents write these three characters IN PARALLEL after this lands, and NONE of
	// them may edit this header. Three simultaneous edits to one 4500-line file is three merge
	// conflicts and a knob quietly lost in the resolution — so the plumbing pass declares the whole
	// tuning surface up front, from the numbers §2 states plus the ones any honest implementation of
	// §2 obviously needs (a projectile has to expire; a slow has to linger or a 176 uu slab is
	// imperceptible; a pair of portals has to stop bouncing a player between them).
	//
	// THE THREE RULES OF THE v14 ABILITIES BLOCK STILL APPLY, unchanged, and rule 2 is the one this
	// pass leans on hardest: NOTHING HERE CAN TURN OFF THE CARRIER RULE. Roxie's flat 100, Elle's
	// teleport and Slimeball's 35% slow are the three most dangerous additions this game has taken,
	// and every one of them is a call to UTraceAbilityComponent::CanAffectTarget. There is no row
	// below that could bypass it and there must never be one.
	//
	// EVERY ONE OF THESE IS IN Trace.VerifyKnobs (TraceSettings.cpp). Until the three character files
	// land they will all report OK while moving nothing, which is the one blind spot that table has
	// by construction — it proves a knob is REACHABLE, never that anybody reached for it.
	//
	// THEY ARE NOT YET IN Config/DefaultGame.ini, and on this project THE INI WINS wherever it has a
	// key. With no key, the initialisers below ARE the shipped values — the same state the v16 ammo
	// knobs shipped in for a pass. Mirror them into DefaultGame.ini beside the Rocco/Chut/Mace/
	// Oyster/X block when the tuning starts, and read the live numbers back with Trace.VerifyKnobs
	// rather than trusting either file.
	// ==========================================================================================

	// ------------------------------------------------------------------------------------------
	// ROXIE — spec v18 §2
	//
	// Passive: jumps 15% higher.
	// Movement (V): a rocket that launches her backwards, fast and far. 100 damage on impact
	// ANYWHERE on the body — no headshot/body distinction — and it WOBBLES in flight, deliberately
	// inaccurate and hard to aim. 35 s cooldown.
	// Activated (Modded): a modded clip — full auto at x1.65 fire rate, for one clip OR 5 s,
	// whichever comes first. 25 s cooldown.
	//
	// §2 says of the rocket: "tuning to come after first implementation - so make every part of it a
	// knob and do not agonise over the values." Everything below that is not 100, 35 or 25 is a first
	// guess that is MEANT to move.
	// ------------------------------------------------------------------------------------------

	/**
	 * §2: "jumps 15% higher". THE FRACTION IS A HEIGHT, AND HEIGHT IS NOT VELOCITY.
	 *
	 * *** READ THIS BEFORE YOU MULTIPLY JumpZVelocity BY 1.15. *** Apex height under gravity is
	 * v^2 / 2g, so it goes as the SQUARE of launch speed: the velocity scale for a +15% apex is
	 * sqrt(1.15) = 1.0724, and 1.15 on the velocity would buy +32.25% height — more than double what
	 * was asked for.
	 *
	 * This project has already shipped that exact mistake once and had to measure its way back out:
	 * spec v16 §0 asked for "+25% DISTANCE" on Chut's bash, the naive +25% on the SPEED knob bought
	 * +65.8% distance, and the shipped value is the quadratic solve (see ChutBashKnockbackSpeed).
	 * The knob is named for what the designer asked for; the code does the square root.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Jump HEIGHT Bonus (fraction; velocity scales as sqrt(1+this)) [v18 §2]", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.5"))
	float RoxieJumpHeightBonus = 0.15f;

	/**
	 * §2: "The rocket deals 100 damage on impact, ANYWHERE ON THE BODY - no headshot/body distinction."
	 *
	 * *** THE SINGLE MOST DANGEROUS NUMBER IN THIS FILE. *** 100 is a full health bar, it ignores hit
	 * zones, and the game's founding invariant is that NO ABILITY MAY DAMAGE A CORE CARRIER. It must
	 * be dealt through UTraceCharacterAbilitySet::DealDamage, which routes to
	 * UTraceAbilityComponent::CanAffectTarget — never by reaching for UTraceHealthComponent, and
	 * never behind a carrier test written in Roxie's own file.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Damage [v18 §2: flat 100, no hit zones. DAMAGE - never a carrier]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "20.0", UIMax = "200.0"))
	float RoxieRocketDamage = 100.f;

	/**
	 * How fast the rocket flies. DELIBERATELY WELL UNDER a thrown Core (2900 base, Patch 28 §4) and nowhere near
	 * hitscan: §2 wants something "deliberately inaccurate and hard to aim", and a projectile that
	 * arrives instantly cannot be either. 2600 uu/s is roughly a second to cross the gap between two
	 * midfield cover blocks.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Speed (uu/s)", ClampMin = "200.0", ClampMax = "20000.0", UIMin = "800.0", UIMax = "6000.0"))
	float RoxieRocketSpeed = 2600.f;

	/** How long before the rocket gives up. Speed x this is the effective range: 2600 x 3 = 7800 uu. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Lifetime (s)", ClampMin = "0.1", ClampMax = "30.0", UIMin = "0.5", UIMax = "8.0"))
	float RoxieRocketLifetimeSeconds = 3.f;

	/**
	 * The rocket's OWN touch radius, on top of the victim's capsule — the same idea as XBeeHitRadiusUU.
	 * IT IS NOT A SPLASH RADIUS. §2 says "on impact", once, for 100; an area-of-effect rocket would be
	 * a different and much stronger ability than the doc describes.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Hit Radius (uu, NOT splash)", ClampMin = "1.0", ClampMax = "300.0", UIMin = "10.0", UIMax = "120.0"))
	float RoxieRocketHitRadiusUU = 45.f;

	/**
	 * §2: it "WOBBLES in flight, deliberately inaccurate and hard to aim". This is how far off the
	 * straight line the path swings, at its widest.
	 *
	 * ZERO IS THE RED ARM: a rocket with no wobble is a straight, easily-aimed 100-damage projectile,
	 * which is a materially different (and much better) ability. Any verification of "hard to aim"
	 * should be able to A/B against 0.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Wobble Amplitude (uu, 0 = flies straight) [v18 §2]", ClampMin = "0.0", ClampMax = "1000.0", UIMin = "0.0", UIMax = "300.0"))
	float RoxieRocketWobbleAmplitudeUU = 120.f;

	/**
	 * DEMO 17 item 3, verbatim: "make the model of the rocket bigger, so it is easy to see."
	 * PATCH 28 ITEM 1, verbatim: "Make Roxie's rocket larger." 1.0 -> 1.6.
	 *
	 * A multiplier on the DRAWN body, whose base size is RoxieRocketHitRadiusUU above.
	 *
	 * *** WHAT DEMO 17 ACTUALLY LANDED, BEFORE RAISING THIS AGAIN. *** Demo 17 did not add a scale on
	 * top of an already-large rocket: it replaced a FIXED 13 uu dart with "the hit radius", i.e. 45 uu,
	 * a 3.46x widening, and this knob shipped at 1.0 as the honest default. Then FX_AUDIO_PLAN §2.3
	 * (W5-KITS-E) found the second, larger half of "it is hard to see" — the body had never actually
	 * been EMBER. The Demo 17 code wrapped /Engine/BasicShapes/Cone's own LIT BasicShapeMaterial and
	 * wrote EmissiveColor / Glow / EmissiveStrength / EmissivePower onto it; all four were silent
	 * no-ops, so the rocket flew as a MATTE cone, which on this arena's black floor is a dark cone.
	 * That is fixed and the body is emissive ember now. So this patch is the third and smallest of
	 * three visibility changes, not the first — which is why it is 1.6 and not 3.
	 *
	 *     1.0  ->  45.0 uu drawn radius,  90 uu across, 135 uu long
	 *     1.6  ->  72.0 uu drawn radius, 144 uu across, 216 uu long
	 *
	 * *** WHY 1.6 IS THE LARGEST HONEST NUMBER, AND WHY IT IS NOT A DRAWN/LETHAL DRIFT. *** The
	 * rocket kills a PAWN whose capsule comes within RoxieRocketHitRadiusUU of the flight line, and a
	 * pawn's capsule is 34 uu of radius — so the volume in which this rocket kills a player is
	 * 45 + 34 = 79 uu about the line, not 45. A drawn body of 72 uu is still INSIDE that 79 uu, so the
	 * ember skin the player watches STILL never claims a kill the rocket does not have. Above ~1.76
	 * (= 79/45) it would, and that is the number to respect if this is raised again.
	 * Trace.Roxie.RocketFlightTest asserts it against the LIVE capsule rather than against 34.
	 *
	 * Against GEOMETRY the sweep is a 45 uu sphere, so the drawn cone is 27 uu wider than the sphere
	 * that detonates it on a wall and will visibly touch the wall a frame before the burst. That is
	 * the ONE deliberate difference between drawn and lethal on this actor, it is in the harmless
	 * direction (the rocket looks like it reached the wall it did in fact reach), and it is stated
	 * here rather than discovered.
	 *
	 * Raise it to exaggerate the rocket further; it changes NOTHING about what the rocket hits. Body
	 * length, launch-flash radius, trail length and both trail radii are named fractions of
	 * TraceRoxieRocket::GetVisualRadiusUU(), so they all moved with this one edit.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket: Drawn Size (x its own hit radius) [Demo 17; Patch 28 §1: 1.0 -> 1.6]", ClampMin = "0.1", ClampMax = "6.0", UIMin = "0.5", UIMax = "3.0"))
	float RoxieRocketVisualScale = 1.6f;


	/** Wobbles per second. With the amplitude above, these two ARE "hard to aim" — tune them together. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Wobble Frequency (Hz)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.5", UIMax = "8.0"))
	float RoxieRocketWobbleFrequencyHz = 3.f;

	/**
	 * §2: the rocket "launches her BACKWARDS, fast and far". Applied to ROXIE, opposite her aim.
	 *
	 * Worth reading against the two speeds this game already has in the hands: the dash is 3300 uu/s
	 * and a wall jump throws 360 uu/s outward. 2200 is firmly a movement ability rather than a nudge,
	 * and it is on a 35 s cooldown for that reason.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Self-Launch Impulse (uu/s, backwards)", ClampMin = "0.0", ClampMax = "8000.0", UIMin = "500.0", UIMax = "4000.0"))
	float RoxieRocketSelfLaunchImpulse = 2200.f;

	/**
	 * Upward share of that impulse. "FAR" needs air time — a purely horizontal shove on the ground is
	 * eaten by friction in half a second. Same shape and the same reason as ChutBashUpBias.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Self-Launch Up Bias (fraction of the impulse)", ClampMin = "0.0", ClampMax = "1.5", UIMin = "0.0", UIMax = "0.8"))
	float RoxieRocketSelfLaunchUpBias = 0.35f;

	/**
	 * §2: "35 s cooldown". SEPARATE from the E ability's — this one is on V, exactly as Rocco's
	 * Ripple is separate from the standard dash.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Rocket Cooldown (s) [v18 §2: 35]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float RoxieRocketCooldownSeconds = 35.f;

	/**
	 * §2: Modded makes the gun fire "x1.65".
	 *
	 * IT IS A RATE, AND FireInterval IS A PERIOD, so this DIVIDES: 0.3158 s / 1.65 = 0.1914 s, i.e.
	 * 190 RPM becomes 314. Multiplying FireInterval by 1.65 would make her fire SLOWER, which is the
	 * kind of inversion that reads as "the ability does nothing" in a playtest rather than as a bug.
	 *
	 * *** THIS KNOB IS THE §0 EXAMPLE THE OWNER GAVE, AND IT IS ALREADY RELATIVE. *** It is a
	 * MULTIPLIER on the base and holds no RPM of its own, so the 150 -> 190 pass moved her from
	 * 248 to 314 RPM without this line changing: 1.65x the base, whatever the base is. Never replace
	 * it with an absolute interval or an absolute RPM — that is the bug §0 was written to prevent.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Modded Fire Rate (x; DIVIDES Fire Interval) [v18 §2: 1.65]", ClampMin = "0.1", ClampMax = "5.0", UIMin = "1.0", UIMax = "3.0"))
	float RoxieModdedFireRateMultiplier = 1.65f;

	/** §2: "the gun becomes FULL AUTO" for the duration. Off leaves it semi-auto but still faster. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Modded Makes The Gun Full Auto [v18 §2]"))
	bool bRoxieModdedFullAuto = true;

	/** §2: "lasts one clip OR 5 seconds, whichever comes first". This is the 5 s half of that. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Modded Duration (s) [v18 §2: 5, or one clip]", ClampMin = "0.1", ClampMax = "60.0", UIMin = "1.0", UIMax = "15.0"))
	float RoxieModdedDurationSeconds = 5.f;

	/**
	 * The "one clip" half. §2 [ASSUMPTION]: "one clip" means the clip that was loaded when Modded
	 * started, so a reload ends the effect.
	 *
	 * A knob rather than an omission because the other reading — Modded survives reloads and only the
	 * 5 s stops it — is defensible and much stronger, and it should be one tick box to try rather than
	 * a code change. At 30 rounds and 0.242 s a full clip is 7.3 s of fire, so with this OFF the 5 s
	 * timer is what ends it in practice and the "one clip" clause never fires.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Modded Ends On Reload [v18 §2 ASSUMPTION: 'one clip' = the one loaded when it started]"))
	bool bRoxieModdedEndsOnReload = true;

	/** §2: "25 s cooldown". The card prints this too — Trace.VerifyCharacterData compares the pair. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Roxie", meta = (DisplayName = "Modded Cooldown (s) [v18 §2: 25]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float RoxieModdedCooldownSeconds = 25.f;

	// ------------------------------------------------------------------------------------------
	// ELLE — spec v18 §2
	//
	// Passive: "right after passing or throwing the trace, Elle automatically cloaks themselves for
	// 3 seconds" — semi-transparent and hard to see or aim at. [ASSUMPTION] "the trace" means THE
	// CORE (a trail is not passed or thrown), the cloak is VISUAL ONLY (no hitbox or aim-assist
	// change), and it drops early if she picks the Core back up.
	// Passive 2: +40% on well-timed slide-jump momentum boosts — *** CUT TO +30% BY PATCH 28 ITEM 3,
	// "Reduce Elle's bonus slide boost from 40 to 30%". *** The +40% above is what §2 ASKED for and is
	// left in the banner because this block is a transcript of the spec; ElleSlideJumpGainBonus below
	// is what ships, and it is 0.30.
	//
	// *** HER CARD SAID 40% FOR TWO PASSES. FIXED IN W9-UIFIX; THE WARNING THAT USED TO STAND HERE
	// *** IS KEPT AS A DESCRIPTION OF THE TRAP, BECAUSE THE TRAP IS STILL OPEN FOR EVERY OTHER NUMBER.
	// The card's MOVEMENT line comes from TraceCharacterRoster.cpp's C++ table and now reads
	// "WELL-TIMED SLIDE JUMPS GIVE HER 30% MORE OF THE MOMENTUM BOOST THAN ANYONE ELSE";
	// DA_Character_Elle.uasset was regenerated from it (Scripts/generate-data-assets.py) and carries
	// the same sentence, and Trace.VerifyCharacterData is green — 31 checks, 0 failed.
	//
	// WHAT HAS NOT CHANGED: section D compares the card against the C++ table, and the asset is
	// GENERATED from that table, so the two agree with each other whatever they say. Nothing in the
	// build compares either of them to the knob beside this comment. Retune ElleSlideJumpGainBonus
	// and you must retune the roster string by hand and re-run the generator, or the card lies again
	// and every check stays green. See W9-UIFIX.md §1 for the check that would close the class and
	// why it was judged too wide to author in the final wave.
	// Activated (Snap): a portal gate where she stands; reactivate within 4 s for its pair; players
	// teleport between them; both expire 8 s after the pair is complete. 35 s cooldown.
	// ------------------------------------------------------------------------------------------

	/** §2: "cloaks themselves for 3 seconds" after passing or throwing the Core. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Cloak Duration (s) [v18 §2: 3]", ClampMin = "0.1", ClampMax = "30.0", UIMin = "0.5", UIMax = "8.0"))
	float ElleCloakDurationSeconds = 3.f;

	/**
	 * How see-through she is while cloaked. 0 is fully invisible, 1 is not cloaked at all.
	 *
	 * §2 says "SEMI-transparent and hard to see or aim at", not invisible, and the difference matters
	 * to the other team: a target that cannot be seen at all cannot be counterplayed, and this game
	 * has no detection tools. 1.0 IS THE RED ARM for any "is she harder to see" verification.
	 *
	 * *** VISUAL ONLY. *** [ASSUMPTION] the cloak changes no hitbox, no lag-compensation pose and no
	 * aim assist. Anything that made her physically harder to HIT rather than harder to SEE would be
	 * a defensive ability the doc did not ask for, and it would interact with the lag-comp rewind in
	 * ways nobody has designed.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Cloak Opacity (0 = invisible, 1 = no cloak) [VISUAL ONLY]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.6"))
	float ElleCloakOpacity = 0.22f;

	/** §2 [ASSUMPTION]: picking the Core back up drops the cloak early. Off leaves the full 3 s. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Cloak Drops If She Retakes The Core [v18 §2 ASSUMPTION]"))
	bool bElleCloakEndsOnCorePickup = true;

	/**
	 * §2: "+40% on well-timed slide-jump momentum boosts", *** CUT TO +30% BY PATCH 28 ITEM 3 ***
	 * ("Reduce Elle's bonus slide boost from 40 to 30%"). IT SCALES THE GAIN, NOT THE WHOLE
	 * MULTIPLIER.
	 *
	 * *** THE WORKED NUMBERS, RE-DERIVED AT PATCH 28 BECAUSE THE OLD ONES HAD ROTTED. *** This
	 * comment used to say the global well-timed multiplier was x1.446875 (gain 0.446875). It has not
	 * been that since spec v26 §3a and v28 §5 landed, and nothing re-read it — exactly the stale-copy
	 * failure the abilities block warns about, living in a comment instead of in code. What
	 * UTraceCharacterMovementComponent::GetSlideJumpWindowSpeedBonus() actually ships today is:
	 *
	 *     SlideJumpWindowSpeedBonus 1.3125, x SlideJumpBonusScale 1.50 on the GAIN  -> 1.46875
	 *     then x SlideJumpMomentumScale 0.80 on the GAIN (v26 §3a)                  -> 1.375000
	 *
	 * so EVERYBODY'S well-timed slide jump is x1.375000, i.e. 1 + 0.375000 of GAIN. Against that:
	 *
	 *     at 0.40 (before Patch 28)   1 + 0.375000 x 1.40 = x1.525000
	 *     at 0.30 (SHIPPED)           1 + 0.375000 x 1.30 = x1.487500
	 *
	 * Her edge over the other nine therefore goes from +15.00% of entry speed to +11.25%. On a
	 * 1900 uu/s approach that is 2897.5 uu/s -> 2826.3 uu/s, against everybody's 2612.5 uu/s.
	 * Scaling the whole multiplier instead would give x1.7875 — an ability nearly twice the size of
	 * the one asked for, and one that would beat DashSpeed off a fast slide.
	 *
	 * That reading is not invented here: it is how EVERY previous slide-jump change on this project
	 * was read, and the global switch that says so out loud is bSlideJumpBonusScalesGainOnly under
	 * Movement|Slide. DERIVE from the shipped bonus, do not hardcode 1.4875 — if the base is retuned,
	 * Elle follows for free, which is rule 1 of the abilities block. NOTHING DERIVES FROM THIS KNOB
	 * in turn: UTraceAbilitySetElle::GetSlideJumpWindowSpeedBonusForElle is its only runtime reader,
	 * and TraceElleVerify.cpp recomputes the same expression from the same property to check it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Slide-Jump GAIN Bonus (fraction of the gain, not the multiplier) [v18 §2 +40%, Patch 28 §3 +30%: x1.375 -> x1.4875]", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.0"))
	float ElleSlideJumpGainBonus = 0.3f;

	/** §2: "she may reactivate within 4 s to place a second gate". No second gate and the first expires. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Snap: Second-Gate Window (s) [v18 §2: 4]", ClampMin = "0.25", ClampMax = "30.0", UIMin = "1.0", UIMax = "10.0"))
	float ElleSnapSecondGateWindowSeconds = 4.f;

	/** §2: "with both placed, both expire after 8 s". The clock starts when the PAIR completes. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Snap: Pair Lifetime (s) [v18 §2: 8]", ClampMin = "0.5", ClampMax = "60.0", UIMin = "2.0", UIMax = "20.0"))
	float ElleSnapPairLifetimeSeconds = 8.f;

	/** How close to a gate counts as stepping into it. Sized like Rocco's ripple entry (140 uu). */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Snap: Gate Radius (uu)", ClampMin = "20.0", ClampMax = "1000.0", UIMin = "60.0", UIMax = "300.0"))
	float ElleSnapGateRadiusUU = 130.f;

	/**
	 * Seconds a player who has just teleported is ignored by BOTH gates.
	 *
	 * SINCE DEMO 17 THIS IS THE BACKSTOP AND NOT THE MECHANISM. A gate now fires on the frame somebody
	 * STEPS IN — an edge, not a proximity poll (see ATraceElleGate::InsideLastLook) — so an arrival is
	 * no longer an entry and standing in a mouth does nothing at all. That is what stops the pair being
	 * a trap; before it, this lockout was the only thing between a stationary player and being thrown
	 * across the map once a second for the pair's whole life. What is left for this knob is the fast
	 * in-out-in case, and 1 s is long enough to walk clear of a 130 uu radius at any speed in this game.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Snap: Re-Entry Lockout (s) [a backstop since Demo 17; the step-in edge is the rule]", ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.25", UIMax = "3.0"))
	float ElleSnapTeleportLockoutSeconds = 1.f;

	/**
	 * *** THE §2 [ASSUMPTION] THE USER IS MOST LIKELY TO REVERSE, AND §2 SAYS SO ITSELF. ***
	 *
	 * §2 verbatim: "'allowing players to teleport' means ANY player, both teams - it says players, not
	 * teammates, and Rocco's Ripple set the precedent that a placed thing is usable by everyone. Flag
	 * it prominently; it is the most reversible-by-them decision in this doc."
	 *
	 * True (shipped) = either team may use either gate. False = Elle's team only.
	 * It is one switch and it is here so the answer is a tick box rather than a pass.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Snap: Usable By BOTH Teams [v18 §2 ASSUMPTION - the most reversible call in the doc]"))
	bool bElleSnapUsableByBothTeams = true;

	/**
	 * May a CORE CARRIER step through a gate on purpose?
	 *
	 * *** THIS KNOB IS ONLY ABOUT THE VOLUNTARY CASE. *** A carrier being MOVED by an enemy gate is
	 * ETraceAbilityEffect::Control applied to a carrier, and UTraceAbilityComponent::CanAffectTarget
	 * already refuses that while the §4 [ASSUMPTION] stands — no knob, no exception, and nothing in
	 * Elle's file should re-implement the test. §2 is explicit: "a Core carrier must not be teleported
	 * by an enemy gate".
	 *
	 * True (shipped) = a carrier who walks into a FRIENDLY gate is carried through, classified
	 * Beneficial, exactly as §6 lets a carrier ride Rocco's Ripple. False = gates ignore carriers
	 * entirely, which is the safer reading if a portal-assisted carry proves too strong.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Snap: A Carrier May Use A Gate VOLUNTARILY (an ENEMY gate can never move one) [v18 §2]"))
	bool bElleSnapCarrierMayUseGate = true;

	/** §2: "35 s cooldown". The card prints this too — Trace.VerifyCharacterData compares the pair. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Snap Cooldown (s) [v18 §2: 35]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float ElleSnapCooldownSeconds = 35.f;

	/**
	 * DEMO 17 item 1. How far the SECOND mouth must be from the first, in uu, before Snap will accept
	 * it. Below this the reactivation is refused, is charged NOTHING, and the 4 s window keeps running
	 * so she can step away and finish the cast.
	 *
	 * *** THIS IS HALF OF "IT DOESN'T WORK AT ALL", AND IT IS THE HALF A PLAYER CAUSES THEMSELVES. ***
	 * A player whose first press seems to do nothing presses again immediately, standing exactly where
	 * they were. That used to place both mouths inside one gate radius, which is not a portal: it is
	 * one blob that teleports you to where you already are, and it burned the whole 35 s cooldown on
	 * it. Measured that way in a two-process run before this landed.
	 *
	 * Defaults to 2x the gate radius (260 uu at the shipped 130), which is the smallest separation at
	 * which the two mouths cannot overlap at all. 0 disables the rule and restores the old behaviour.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Elle", meta = (DisplayName = "Snap: Minimum Distance Between The Two Mouths (uu, 0 = allow both on one spot)", ClampMin = "0.0", ClampMax = "2000.0", UIMin = "0.0", UIMax = "600.0"))
	float ElleSnapMinimumMouthSeparationUU = 260.f;

	// ------------------------------------------------------------------------------------------
	// SLIMEBALL — spec v18 §2
	//
	// Movement (hold V): sticks to walls while held.
	// Passive: while stuck, +30% fire rate and 30% damage reduction on body shots and FRONT knife
	// stabs. [ASSUMPTION] headshots and BACK stabs are unreduced — it names body shots and front
	// stabs, exactly as Chut's Chud names body shots and melees.
	// Activated (Slimewall): a wall in his aim direction, one player height tall and wide and about
	// the length of a standard in-game box — "(for now, make this changeable)", so all three
	// dimensions are knobs. It can be SHOT THROUGH but obstructs vision, and moving through it slows
	// enemies by 35%. Lasts 4 s, 25 s cooldown.
	// ------------------------------------------------------------------------------------------

	/**
	 * THE WALL TEST for the stick: largest |Normal.Z| a surface may have and still be a wall.
	 *
	 * 0.70 is cos(45 degrees), the walkable floor limit, so the rule reads exactly "if he cannot stand
	 * on it, he can stick to it". That is the same value and the same argument as
	 * MaceSpikeMaxSurfaceNormalZ — and it is a SEPARATE knob for the same reason that one is separate
	 * from WallJumpMaxNormalZ (0.40): the wall jump's threshold is a movement-feel decision that has
	 * been tuned against, and moving it to fix a different ability would retune the wall jump by
	 * accident.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Wall Stick: max |normal.Z| (0.70 = the walkable limit)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.2", UIMax = "0.9"))
	float SlimeballWallStickMaxSurfaceNormalZ = 0.7f;

	/** How close to a wall he must be for hold-V to grab it. The capsule's own radius is 34 uu. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Wall Stick: Reach (uu)", ClampMin = "10.0", ClampMax = "500.0", UIMin = "40.0", UIMax = "200.0"))
	float SlimeballWallStickRangeUU = 90.f;

	/**
	 * How fast he creeps down the wall while stuck. 0 = welded in place, which is what "sticks to
	 * walls" says; a small positive value is the classic slide if being motionless proves too safe.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Wall Stick: Slide Speed (uu/s, 0 = fully stuck)", ClampMin = "0.0", ClampMax = "2000.0", UIMin = "0.0", UIMax = "400.0"))
	float SlimeballWallStickSlideSpeed = 0.f;

	/**
	 * Hard cap on one stick. §2 gives none, so the shipped value is 0 = as long as V is held.
	 *
	 * A knob rather than a literal for the same reason MaceSuspendCooldownSeconds is one: "he can hang
	 * on a wall indefinitely, firing 30% faster and taking 30% less" is exactly the kind of thing a
	 * playtest reverses, and reversing it should be a number rather than a pass.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Wall Stick: Max Duration (s, 0 = while held; UNSPECIFIED in §2)", ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "10.0"))
	float SlimeballWallStickMaxSeconds = 0.f;

	/** Cooldown between sticks. §2 gives none; 0 ships, exactly as Mace's suspend does. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Wall Stick: Cooldown (s, 0 = none; UNSPECIFIED in §2)", ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "10.0"))
	float SlimeballWallStickCooldownSeconds = 0.f;

	/**
	 * DEMO 17 item 2, verbatim: "Slimeball should be able to cancel the wall stick with jump, and
	 * perform a wall jump off the wall."
	 *
	 * How long after that jump he may NOT grab the same wall again, in seconds. Nothing to do with the
	 * ordinary stick cooldown above: this exists only so that a player who is still HOLDING V — which
	 * everyone is, because the stick is a hold — does not have the 20 Hz re-stick sweep glue him
	 * straight back onto the face he just launched off. That is precisely the wall-jump stickiness spec
	 * v10 §5 removed for everybody else, and it must not come back through one character.
	 *
	 * 0.35 s is deliberately a little longer than the movement layer's own into-wall control lockout
	 * (WallJumpControlLockoutSeconds, 0.20 s): the launch clears the 90 uu stick reach in about a
	 * quarter of a second at the shipped outward impulse, and the sweep only looks 20 times a second,
	 * so a lockout equal to the movement one could still catch on its last look. 0 = no lockout, which
	 * is the RED setting rather than a legitimate one.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Wall Stick: Re-Grab Lockout After A Wall Jump (s) [Demo 17]", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.1", UIMax = "1.0"))
	float SlimeballWallJumpRestickLockoutSeconds = 0.35f;

	/**
	 * DEMO 17 item 2. Whether the jump that cancels the stick launches him off the wall at all.
	 *
	 * True (shipped) = jump is a real wall jump: he leaves along the surface normal with the shipped
	 * wall-jump numbers. False = jump merely lets go and he falls, which is the smaller reading of the
	 * same sentence and one tick box away.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Wall Stick: Jump Performs A WALL JUMP (off = jump only lets go) [Demo 17]"))
	bool bSlimeballWallStickJumpLaunches = true;

	/**
	 * §2: "+30% fire rate" WHILE STUCK ONLY.
	 *
	 * A RATE bonus over a PERIOD knob, like Roxie's: FireInterval is divided by (1 + this), so
	 * 0.3158 s becomes 0.2429 s — 190 RPM becomes 247. Multiplying instead would slow him down while
	 * claiming to speed him up.
	 *
	 * RELATIVE BY CONSTRUCTION (spec v24 §0), exactly like RoxieModdedFireRateMultiplier: it is a
	 * FRACTION of the base, never an interval or an RPM, so the 150 -> 190 pass carried him from
	 * 195 to 247 RPM with this line untouched.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Stuck: Fire Rate Bonus (fraction; DIVIDES Fire Interval) [v18 §2: 0.30]", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.0"))
	float SlimeballStuckFireRateBonus = 0.3f;

	/**
	 * §2: "30% damage reduction on body shots and front knife stabs" while stuck.
	 *
	 * [ASSUMPTION] HEADSHOTS AND BACKSTABS ARE UNREDUCED, because the doc names body shots and front
	 * stabs specifically — the same reading, and the same shape of implementation, as Chut's Chud
	 * (ChudDamageReduction, applied in ModifyIncomingDamage off FTraceAbilityDamageContext's
	 * bHeadshot / bMelee). Copy Chud rather than inventing a second way to read a hit zone.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Stuck: Damage Reduction (fraction, body + FRONT stabs only) [v18 §2: 0.30]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.6"))
	float SlimeballStuckDamageReduction = 0.3f;

	/**
	 * SLIMEWALL, DIMENSION 1 of 3 — HEIGHT, straight up from the floor.
	 *
	 * §2: "one player height tall". A Trace pawn is a 34 x 88 capsule, so one player height is 176 uu,
	 * and that is the value here. §2 also says, of all three dimensions, "(for now, make this
	 * changeable)" — which is why there are three knobs rather than one hardcoded box.
	 *
	 * *** THE AXIS EACH KNOB MEANS IS AN [ASSUMPTION], and it is the one to check first if the wall
	 * comes out the wrong shape. *** The doc's "one player height tall and wide, and about the length
	 * of a standard in-game box" does not say which of its words is the span and which is the
	 * thickness. The reading here: HEIGHT is vertical, WIDTH is the span ACROSS his aim, and LENGTH
	 * runs ALONG his aim, away from him.
	 *
	 * *** PATCH 28 ITEM 2 TURNED THE WALL 90° AND MOVED NONE OF THESE THREE NUMBERS. *** Verbatim:
	 * "Slimeball's slimewall should be placed forward instead of laterally in front of him." Until
	 * that patch LENGTH was the span across his aim — an 1100 uu barrier facing him, 176 uu thick,
	 * the wall you hide BEHIND — and WIDTH was the thickness along his aim. They have swapped roles
	 * without swapping values: the wall is now a 176 x 176 uu cross-section (one player tall, one
	 * player wide, which is what §2 asks for in as many words) running 1100 uu away from him. It is
	 * the wall you run BESIDE, and it splits a lane instead of capping it.
	 *
	 * WHAT DID NOT CHANGE, because it is the reason WIDTH is a knob at all: 176 uu is still how far
	 * an enemy travels while inside the slab, since an enemy meets a forward wall by CROSSING the
	 * lane it divides. The 35% slow is still non-instantaneous for the same arithmetic it always was
	 * (176 uu at 800 uu/s = 0.22 s inside).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall HEIGHT (uu, vertical) [v18 §2: 176 = one player height]", ClampMin = "20.0", ClampMax = "3000.0", UIMin = "80.0", UIMax = "600.0"))
	float SlimewallHeightUU = 176.f;

	/**
	 * SLIMEWALL, DIMENSION 2 of 3 — WIDTH, the span ACROSS his aim direction, i.e. the slab's
	 * thickness as seen by somebody crossing the lane it divides.
	 *
	 * This is how far an enemy travels while inside the wall, so it is the knob that decides whether
	 * the 35% slow is felt at all. 176 uu is the doc's "and wide", read as one player height.
	 *
	 * PATCH 28 ITEM 2 CHANGED THIS KNOB'S AXIS AND NOT ITS JOB — see the [ASSUMPTION] block on
	 * SlimewallHeightUU above. It used to be the thickness ALONG his aim, because the wall stood
	 * across the lane; the wall now runs down the lane and this is the width of the thing you cross.
	 * The number, the meaning ("how long an enemy is inside it") and the 0.22 s that falls out of it
	 * are all unchanged. It is handed to ATraceSlimewall::ResolveForwardRun as its ThicknessUU.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall WIDTH (uu, span ACROSS aim - what enemies cross) [v18 §2: 176; Patch 28 §2 turned the axis]", ClampMin = "10.0", ClampMax = "2000.0", UIMin = "50.0", UIMax = "500.0"))
	float SlimewallWidthUU = 176.f;

	/**
	 * SLIMEWALL, DIMENSION 3 of 3 — LENGTH, the run ALONG his aim direction, away from him.
	 *
	 * §2: "about the length of a standard in-game box". The arena's one-player-height cover blocks are
	 * 1100 x 1100 (TraceArenaConstants::ApproachCover, entry D — the low diamond), so 1100 is what "a
	 * standard in-game box" measures on this map.
	 *
	 * *** PATCH 28 ITEM 2: THIS IS NO LONGER "THE PART YOU HIDE BEHIND". *** It was the span across
	 * his aim — 1100 uu of barrier facing him — and it now points down the lane, so 1100 uu is how
	 * far the wall REACHES rather than how wide a shield it is. Combined with SlimewallRangeUU below,
	 * the shipped wall occupies 400 uu to 1500 uu ahead of him along his aim.
	 *
	 * IT SHORTENS RATHER THAN RETREATING when something is in the way (a forward wall's long axis
	 * points INTO the obstacle, so pulling back would drag the whole run over his own head to buy
	 * 176 uu of clearance). A run shorter than the WIDTH above is refused as a free fizzle instead of
	 * being placed as a block, so in tight spots this reads as a shorter wall, never a dead ability.
	 * Whether 1100 is still the right number now that it points down a lane is an open tuning
	 * question for the owner, and it is this one line.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall LENGTH (uu, run ALONG aim - how far it reaches) [v18 §2: ~1 cover block; Patch 28 §2 turned the axis]", ClampMin = "50.0", ClampMax = "5000.0", UIMin = "300.0", UIMax = "2000.0"))
	float SlimewallLengthUU = 1100.f;

	/**
	 * How far in front of him the wall goes up. Far enough to not be inside his own capsule.
	 *
	 * *** PATCH 28 ITEM 2 CHANGED THIS KNOB'S MEANING AND NOT ITS VALUE. *** It used to be the
	 * distance to the wall's CENTRE; it is now the distance to its NEAR END, so "the wall starts
	 * 400 uu in front of me" still reads the same and still ships 400. What moved is everything
	 * behind that: the centre is now 400 + 1100/2 = 950 uu ahead and the far end 1500 uu ahead,
	 * where the lateral wall's far edge was 400 + 176/2 = 488 uu ahead. Anyone reading the old
	 * meaning off the name will be off by half a wall.
	 *
	 * The near end is the end that never moves, which is what makes the sentence above survive the
	 * blocked-lane case: ResolveForwardRun shortens the FAR end against an obstacle.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall: Placement Distance (uu, to its NEAR END) [Patch 28 §2 changed the meaning, not the value]", ClampMin = "0.0", ClampMax = "5000.0", UIMin = "100.0", UIMax = "1500.0"))
	float SlimewallRangeUU = 400.f;

	/**
	 * §2: "moving through it slows enemies by 35%".
	 *
	 * A CONTROL effect — so it goes through UTraceAbilityComponent::CanAffectTarget and does NOT apply
	 * to a Core carrier while the §4 [ASSUMPTION] stands, exactly like Oyster's poison slow. 0 leaves
	 * a wall that only blocks sight, which is the RED arm for any "does it slow" verification.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall: Slow (fraction) [v18 §2: 0.35. CONTROL - see the §4 assumption]", ClampMin = "0.0", ClampMax = "0.95", UIMin = "0.0", UIMax = "0.6"))
	float SlimewallSlowFraction = 0.35f;

	/**
	 * [ASSUMPTION] how long the slow lasts after leaving the slab. §2 does not say.
	 *
	 * IT CANNOT SENSIBLY BE ZERO. At 176 uu thick and 800 uu/s walk speed an enemy is inside the wall
	 * for 0.22 s; a slow that ended on the far side would be a rounding error nobody could feel, and
	 * the ability would read as broken rather than as weak. 0.75 s is long enough that walking through
	 * is a decision.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall: Slow Lingers For (s, after leaving) [v18 §2 ASSUMPTION]", ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.0", UIMax = "3.0"))
	float SlimewallSlowLingerSeconds = 0.75f;

	/** §2: "lasts 4 s". */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall Duration (s) [v18 §2: 4]", ClampMin = "0.25", ClampMax = "60.0", UIMin = "1.0", UIMax = "12.0"))
	float SlimewallDurationSeconds = 4.f;

	/**
	 * How solid the wall LOOKS. §2: it "obstructs vision".
	 *
	 * *** THIS IS APPEARANCE ONLY AND IT MUST STAY THAT WAY. *** §2 is explicit that the wall "can be
	 * shot through", so it must have no blocking collision on the weapon/trace channels — a wall that
	 * ate bullets would silently break every hitscan crossing it, including shots that never meant to
	 * involve Slimeball. Opacity is what makes it a sight blocker; collision is what would make it a
	 * bullet blocker, and it does not get any.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall Opacity (LOOK ONLY - it never blocks bullets) [v18 §2]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.3", UIMax = "1.0"))
	float SlimewallOpacity = 0.9f;

	/** §2 [ASSUMPTION]: it does not slow Slimeball or his own team. On makes it a hazard for everyone. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall Slows His Own Team Too [v18 §2 ASSUMPTION: false]"))
	bool bSlimewallSlowsOwnTeam = false;

	/** §2: "25 s cooldown". The card prints this too — Trace.VerifyCharacterData compares the pair. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Slimeball", meta = (DisplayName = "Slimewall Cooldown (s) [v18 §2: 25]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float SlimewallCooldownSeconds = 25.f;

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

	// ==========================================================================================
	// AMMO  (spec v16 §1, new)
	//
	// Verbatim: "Add ammo to the guns. 30 bullets per clip, then the gun reloads. R to reload."
	// "Reloading takes .5seconds"
	//
	// APPENDED AT THE END OF THE CLASS RATHER THAN FILED UNDER "Combat" BESIDE FireInterval, and
	// that is a file-position choice only — the `Category` meta below is what decides where these
	// appear in Project Settings, so they land next to Fire Interval in the editor regardless. The
	// pass that added them owned this header only for appends.
	//
	// BOTH ARE MIRRORED IN `Config/DefaultGame.ini` under `[/Script/Trace.TraceSettings]`, beside
	// FireInterval, and *** THE INI IS THE ONE THAT DECIDES *** — it is layered over these
	// initialisers at startup, so editing only this header changes nothing at runtime. Move both or
	// neither, and read the live numbers back with `Trace.Ammo.Dump` rather than trusting either
	// file. (They were header-only for the length of the pass that added them, which is why the
	// pass's own evidence quotes `Trace.Ammo.Dump` everywhere instead of the ini.)
	// ==========================================================================================

	/**
	 * ROUNDS PER CLIP. Spec v16 §1: "30 bullets per clip".
	 *
	 * THE RESERVE IS INFINITE — there is deliberately no carried-ammo knob beside this one. The
	 * document never mentions carried ammo, and a shooter that can run permanently dry is a much
	 * bigger design change than the line asks for. Only the clip is finite, and a reload always
	 * refills it completely.
	 *
	 * Worth reading in the units of the gun that spends it: FireInterval is 0.315789 s as of spec
	 * v24 §4, so a held trigger empties a full clip in 29 x 0.3158 = 9.16 s (the first round is
	 * free), down from 11.6 s at the old 150 RPM gun — reloads come round about 21% sooner and that
	 * is the whole ammo-side consequence of the faster gun. At 40 damage a body shot it is still 750
	 * potential damage between reloads: the CLIP did not change, only how fast it is spent.
	 * Characters that modify the fire rate spend it faster still, in the same proportion — a stuck
	 * Slimeball empties it in 7.04 s and Roxie under MODDED would need 5.55 s, except that MODDED
	 * ends after one clip. Lowering this is the single strongest lever on how often a fight is
	 * interrupted; it is not a cosmetic number.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Clip Size (rounds) [v16 §1]", ClampMin = "1", ClampMax = "999", UIMin = "5", UIMax = "60"))
	int32 ClipSize = 30;

	/**
	 * SECONDS THE RELOAD TAKES, during which the gun refuses to fire. Spec v16 §1: "Reloading takes
	 * .5seconds".
	 *
	 * It is a DEADLINE on the shared clock at runtime, not a countdown (see
	 * UTraceWeaponComponent::ReloadEndServerTime), for the same reason the knife's 0.2 s pullout is:
	 * one float that means the same instant on the client that predicted it and the server that
	 * validates against it.
	 *
	 * The knife is unaffected — CanSwing() does not consult ammo at all, so a player caught mid
	 * reload can still swing. That is deliberate and it is the counterplay to an empty clip.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat", meta = (DisplayName = "Reload Time (s) [v16 §1]", ClampMin = "0.05", ClampMax = "10.0", UIMin = "0.2", UIMax = "3.0"))
	float ReloadSeconds = 0.5f;

	// ==========================================================================================
	// X's VULNERABLE, NOW STACKING  (spec v16 §4, new)
	//
	// Verbatim: "Change X's vulnerable to stack with each hit. The first stack still causes 25%
	// extra damage, but each additional stack only adds 5%. Whenever the timer runs out, all stacks
	// disappear."
	//
	// The FIRST stack is still XVulnerableDamageBonus above; the two knobs below are the
	// per-extra-stack term and the ceiling. N stacks resolve to 1 + XVulnerableDamageBonus +
	// (N-1) * XVulnerableStackBonus — written that way, and not as literals, because spec v24 §9
	// has just moved the first term from 0.25 to 0.35 and left the other two alone. Today that is
	// 1 + 0.35 + (N-1) * 0.05.
	//
	// Mirrored in Config/DefaultGame.ini beside XVulnerableDamageBonus, and the ini is the one that
	// decides — see the AMMO note above. `Trace.Health.DumpSettings` prints what this process
	// resolved.
	// ==========================================================================================

	/**
	 * What each stack after the first adds. §4: "each additional stack only adds 5%".
	 *
	 * UNCHANGED BY SPEC v24 §9, which says "keeping the 5% extra per stack after one intact".
	 * With the first stack now at +35% (XVulnerableDamageBonus), 1 stack = +35%, 2 = +40%,
	 * 3 = +45%, and so on up to the cap below. (It used to read +25/+30/+35.)
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Vulnerable: Bonus Per Extra Stack (fraction) [v16 §4]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.2"))
	float XVulnerableStackBonus = 0.05f;

	/**
	 * HARD CEILING ON THE STACK COUNT. §4 does not give one; [ASSUMPTION], and this is the knob it
	 * was made a knob for.
	 *
	 * FIVE, and the reason is X's own arithmetic rather than a round number: five is the bee count
	 * (XBeeCount) and the Sting clip (XStingBulletCount), so it is exactly the number of marks X can
	 * deliver in one engagement without waiting for a cooldown. A cap below five would make the last
	 * bees of a Sting clip do nothing; a cap above five could only be reached by two Xs or by a
	 * target standing inside the orbiting swarm, which is not a case worth designing a damage cliff
	 * around.
	 *
	 * WHAT IT BUYS THE TARGET, in the units of the gun, RESTATED FOR SPEC v24 §9's +35% first stack:
	 * at the cap the multiplier is now x1.55 (was x1.45), so a 25-damage leg shot becomes 38.75 and
	 * kills in three instead of four. (The 40-damage body shot already dropped from three rounds to
	 * two at ONE stack — 40 x 1.35 = 54 — so the body cliff is not what this cap is protecting.)
	 * Unbounded stacking would put a headshot-equivalent on the fourth leg shot, which is the bug
	 * this exists to prevent. §9 raised the FIRST stack only; the cap is one of the "all other
	 * aspects" it says to leave intact.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|X", meta = (DisplayName = "Vulnerable: Max Stacks [v16 §4, capped at 5]", ClampMin = "1", ClampMax = "50", UIMin = "1", UIMax = "10"))
	int32 XVulnerableMaxStacks = 5;

	// ==========================================================================================
	// MORTIMER  (spec v19 §3, new — roster 8 -> 10)
	//
	// Verbatim, Demo 18:
	//   PASSIVE   "his dash is 75% shorter" AND "he can charge the core up to 2x as long as anyone
	//             else, on the same linear scale, so he throws it twice as far"
	//   MOVEMENT  "he can mantle onto objects, 30% more generous than the old in-game mantle"
	//   ACTIVATED "only while carrying the core AND standing on the ground or the top of an object,
	//             a blast that knocks nearby enemies away"
	//
	// DEMO 21 amends the passive again and finally delivers the movement line:
	//
	//   ITEM 6   "Add a mantle for Mortimer, as the original instructions requested. It doesn't have
	//            to be the old mantle — make a new one which acts the same, but for just him."
	//   ITEM 7   "after the original 100% charge window has passed, add a .6x modifier to the linear
	//            scaling of his throw charge"
	//
	// *** WHICH OF THESE KNOBS IS ACTUALLY READ, AS OF DEMO 21. *** Say it here rather than in a
	// report, because this comment is what the next person reads. EVERY LINE NAMES A CALL SITE:
	//
	//     MortimerDashDistanceScale     LIVE. UTraceCharacterMovementComponent::GetDashSpeed()
	//                                   multiplies by TraceAbilityTraits::GetDashDistanceScale().
	//     MortimerDashCooldownScale     LIVE since v23. GetDashCooldown() multiplies by
	//                                   TraceAbilityTraits::GetDashCooldownScale(). (This list said
	//                                   NOT LIVE for a whole demo after the line landed. Fixed.)
	//     MortimerThrowChargeHoldScale  LIVE. ATraceCore::GetThrowChargeScaleForHold() (x2 call sites).
	//     MortimerThrowChargePastFull-  LIVE. Same function — DEMO 21 ITEM 7.
	//       Scale
	//     bMortimerCanMantle            LIVE as of DEMO 21 ITEM 6. UTraceAbilitySetMortimer::
	//     MortimerMantleGenerosity      OnJumpPressed() -> TryMantle(), gated on
	//     MortimerMantleReachRadii      TraceAbilityTraits::IsMantleAllowed(). Measured by
	//     MortimerMantleApexReach       Trace.Mortimer.MantleTest, red arm Trace.Mortimer.Mantle 0.
	//     MortimerMantleCooldownSeconds
	//
	// Demo 18's version of this block claimed the dash scale and the throw cap were both inert. They
	// were wired in the same pass and the comment was never corrected, which is how Demo 20 arrived
	// with an integrator quoting "the dash is not live" as fact. If you wire a knob, correct this list
	// in the same edit — and if you find a line here that is stale, fix it before you write anything
	// else, because somebody is about to quote it.
	//
	// THE ini PINS EVERY KNOB DEMO 20 AND DEMO 21 MOVED. Config/DefaultGame.ini beats these
	// initialisers, so MortimerDashDistanceScale, MortimerDashCooldownScale, the four mantle numbers
	// and MortimerThrowChargePastFullScale are written in BOTH places and must be changed in both.
	// ==========================================================================================

	/**
	 * MULTIPLIER ON DASH REACH.
	 *
	 * *** DEMO 20 ITEM 2, VERBATIM: "Change mortimer's dash to be 40% of a normal one instead of
	 * 25%". *** 0.25 -> 0.40. §3's original sentence was "his dash is 75% shorter"; the owner has
	 * revised that to 60% shorter, and this is the whole of that half of the item.
	 *
	 * IT SCALES THE DASH'S SPEED, NOT ITS DURATION, and that is a decision worth stating. Reach is
	 * DashSpeed x DashDuration (3300 x 0.18 = 594 uu), so either factor would shorten it — but the
	 * DURATION is what the trace, the parry window and the dash-hit sweep are all measured against
	 * (see UTraceCharacterMovementComponent::IsDashing), and a Mortimer whose dash window was a
	 * quarter as long would be a quarter as parryable. Scaling the speed leaves every timing in the
	 * game where it is and moves only the distance, which is the sentence the doc actually wrote.
	 *
	 * 594 uu -> 238 uu, up from 149 uu. Still not an escape; now genuinely a repositioning step.
	 *
	 * THE CHARACTER CARD STILL SAYS "A QUARTER". Core/TraceCharacterRoster.cpp's Mortimer row reads
	 * "HIS DASH COVERS ONLY A QUARTER OF THE NORMAL DISTANCE" and is now false. That file is outside
	 * this pass's ownership; it is named in the report as a required follow-up.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Dash Reach Scale (0.40 = 60% shorter) [Demo 20 item 2]", ClampMin = "0.05", ClampMax = "2.0", UIMin = "0.1", UIMax = "1.0"))
	float MortimerDashDistanceScale = 0.40f;

	/**
	 * MULTIPLIER ON HIS DASH COOLDOWN. Everybody else's is UTraceSettings::DashCooldown (3.5 s).
	 *
	 * *** DEMO 20 ITEM 2, VERBATIM: "but increase mortimer's dash cooldown by 25%". *** 1.25, i.e.
	 * 3.5 s -> 4.375 s for him and 3.5 s for the other nine. Written as a MULTIPLIER and not as the
	 * number 4.375 for the same reason LilyExtraDashCharges is an addend: a retune of the shared
	 * DashCooldown must carry him with it instead of leaving him pinned to a stale absolute.
	 *
	 * *** THIS KNOB IS LIVE. *** It was not when this paragraph was first written, and the paragraph
	 * went on claiming otherwise for a whole demo after the call site landed. The line is in
	 * UTraceCharacterMovementComponent::GetDashCooldown():
	 *
	 *     return FMath::Max(0.f, UTraceSettings::Get().DashCooldown
	 *         * TraceAbilityTraits::GetDashCooldownScale(CharacterOwner));
	 *
	 * The trait is 1.0 for every other character and for any pawn with no ability component, so that
	 * line is arithmetically identity for everybody but Mortimer — the same shape GetDashSpeed()
	 * already uses one function above it. Trace.Mortimer.DashTest MEASURES it on a live pawn rather
	 * than trusting this sentence; believe the command, not the comment.
	 *
	 * GetDashRechargeWindow() is GetDashDuration() + GetDashCooldown(), so the HUD's dash meter
	 * follows automatically and no second edit is needed for it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Dash Cooldown Scale (1.25 = 25% longer than everybody) [Demo 20 item 2]", ClampMin = "0.25", ClampMax = "4.0", UIMin = "1.0", UIMax = "2.0"))
	float MortimerDashCooldownScale = 1.25f;

	/**
	 * HOW MANY TIMES LONGER HE MAY USEFULLY HOLD A CORE THROW. §3: "up to 2x as long as anyone else".
	 *
	 * It multiplies the CAP on t = HeldSeconds / CoreThrowChargeSeconds, and nothing else. With the
	 * shared charge at 0.6 s (v18 §0) his ceiling is 1.2 s, and the shipped line
	 *     Power = CoreThrowChargeFloorFraction + (1 - Floor) x t
	 * is extrapolated to t = 2 rather than given a multiplier of its own — "ON THE SAME LINEAR SCALE"
	 * is the doc's phrase and this is what it means arithmetically.
	 *
	 * *** SO THE HONEST NUMBER IS x1.85 SPEED, NOT x2.00. *** Floor is 0.15, so a full 0.6 s hold is
	 * 0.15 + 0.85 = 1.00 and a full 1.2 s hold is 0.15 + 1.70 = 1.85. The doc says "twice as far";
	 * on the same line the answer is 1.85x the launch speed, and launch speed is not distance — a
	 * flat-ground throw's range goes as the SQUARE of speed, so 1.85x speed is about 3.4x the range,
	 * which overshoots "twice as far" rather than falling short of it. Flagged in the report as the
	 * one place the doc's two sentences cannot both be exactly true.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Core Throw: Charge Hold Scale (2 = may hold twice as long) [v19 §3]", ClampMin = "1.0", ClampMax = "6.0", UIMin = "1.0", UIMax = "3.0"))
	float MortimerThrowChargeHoldScale = 2.f;

	/**
	 * *** DEMO 21 ITEM 7, VERBATIM: "after the original 100% charge window has passed, add a .6x
	 * modifier to the linear scaling of his throw charge". ***
	 *
	 * WHAT IT MULTIPLIES, PRECISELY: the charge t accumulated PAST the original 100% point, and
	 * nothing before it. Up to t = 1 Mortimer is byte-identical to the other nine — this knob cannot
	 * touch that half of the line, because the term it scales is zero there.
	 *
	 *     EffectiveT = min(t, 1)  +  0.6 x clamp(t - 1, 0, MortimerThrowChargeHoldScale - 1)
	 *     Power      = CoreThrowChargeFloorFraction + (1 - Floor) x EffectiveT
	 *
	 * A FRACTION OF THE SHARED LINE'S OWN SLOPE, NEVER A SECOND SLOPE OF HIS OWN (spec v24 §0). It is
	 * written as 0.6 against "whatever the base curve does past 100%" rather than as a Mortimer-only
	 * uu/s or a Mortimer-only gradient, so a retune of CoreThrowChargeFloorFraction, of
	 * CoreThrowChargeSeconds or of the hold cap moves his extended half with everybody's base half,
	 * automatically. 1.0 here reproduces the pre-Demo-21 behaviour exactly and is the arithmetic red
	 * arm; the CVar red arm is Trace.Mortimer.ThrowPastFull 0.
	 *
	 * WHAT IT DOES TO THE NUMBERS: at hold scale 2 and floor 0.15 his full extended charge was
	 * 0.15 + 0.85 x 2.00 = x1.85 launch speed and is now 0.15 + 0.85 x 1.60 = x1.51. Range goes as
	 * the SQUARE of launch speed (the loose Core is integrated under gravity with no drag — see
	 * ATraceCore::ServerTickLooseCore), so his ceiling drops from about 3.42x a full-charge throw to
	 * about 2.28x it. He still out-throws everybody, which is the item's own wording, and the card's
	 * "about twice as far" becomes nearly true instead of a 70% overstatement.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Core Throw: Past-100% Charge Scale (0.6 = extra charge counts 60%) [Demo 21 item 7]", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.2", UIMax = "1.0"))
	float MortimerThrowChargePastFullScale = 0.6f;

	/**
	 * *** THE MANTLE GATE, AND IT IS OFF FOR EVERY OTHER CHARACTER BY CONSTRUCTION. ***
	 *
	 * The mantle was added in `dffea7c` and deleted in `d2319b2`; the deletion commit MEASURES why
	 * (shipped 5/5 ledge contacts at 0.00 corrections and 0.00 uu worst error, against the legacy
	 * mantle's 1.00 corrections per contact and 88.11 uu). Spec v19 §3 asks for it back for Mortimer
	 * alone, so this is a per-character switch and not a global one: nobody but Mortimer ever reaches
	 * the probe, which is what makes "bringing it back for one character must not bring the ledge bug
	 * back for everyone" structural rather than hoped-for.
	 *
	 * Setting this false disables Mortimer's mantle and restores today's shipped movement for all ten
	 * characters — i.e. it is also the red arm and the panic switch.
	 *
	 * *** DEMO 21 ITEM 6: THIS IS NO LONGER A GATE ON NOTHING. *** It is read by
	 * TraceAbilityTraits::IsMantleAllowed(), which UTraceAbilitySetMortimer::TryMantle() asks FIRST.
	 * The mantle itself lives in his own ability set (not in Source/Trace/Movement/): it is driven by
	 * the JUMP key through the existing UTraceCharacterAbilitySet::OnJumpPressed hook, which
	 * ATracePlayerController runs locally AND re-runs on the server, and it is two LaunchCharacter
	 * impulses rather than a per-frame MOVE_Flying pull-up. See the header block in
	 * Abilities/Characters/TraceAbilitySetMortimer.h for why that shape and not the old one.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Mantle Enabled (MORTIMER ONLY - nobody else can ever mantle) [v19 §3 / Demo 21 item 6]"))
	bool bMortimerCanMantle = true;

	/**
	 * HOW MUCH MORE GENEROUS HIS MANTLE IS THAN THE OLD IN-GAME ONE. §3: "30% more generous".
	 *
	 * Applied to the three numbers that decide whether a ledge is climbable at all:
	 *     reach            x this   (MortimerMantleReachRadii, below)
	 *     height ceiling   x this   (MortimerMantleApexReach, below)
	 *     floor            / this   (a LOWER floor is more generous)
	 *     facing cone      x this   (a WIDER cone is more generous)
	 * The pull-up's own feel — how high he arrives, how hard he is pushed over the lip — is NOT
	 * scaled: "more generous" is a statement about what counts as a ledge, not about the climb.
	 *
	 * AGAINST THE LEGACY NUMBERS `git show dffea7c` restores (reach 70 + capsule, floor 55, ceiling
	 * 230), 1.30 lands within a few uu of the old 91 / 42.3 / 299 at today's capsule, jump and
	 * gravity — but the two companion knobs below are now MULTIPLES OF A LIVE BASE rather than uu, so
	 * they follow the capsule and the jump instead of pinning him to a stale absolute (spec v24 §0).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Mantle Generosity (1.30 = 30% more generous than the old one) [v19 §3]", ClampMin = "1.0", ClampMax = "3.0", UIMin = "1.0", UIMax = "2.0"))
	float MortimerMantleGenerosity = 1.3f;

	/**
	 * HOW FAR AHEAD HE REACHES FOR A LEDGE FACE, IN CAPSULE RADII, BEFORE GENEROSITY.
	 *
	 * *** A MULTIPLE OF THE CAPSULE, NOT A NUMBER OF uu (spec v24 §0). *** The legacy mantle stored
	 * `MantleReachUU = 70` and traced `CapsuleRadius + 70`, which meant a capsule retune silently
	 * changed how far past his own shoulder Mortimer could grab. Reach is a fact about how far in
	 * front of the BODY the hands are, so it is expressed against the body: the probe runs
	 *
	 *     CapsuleRadius x MortimerMantleReachRadii x MortimerMantleGenerosity
	 *
	 * from the capsule axis. At today's 34 uu radius that is 34 x 2.8 x 1.30 = 124 uu, against the
	 * legacy 34 + 70 x 1.30 = 125 uu — the same reach, now derived.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Mantle Reach (CAPSULE RADII, before generosity) [Demo 21 item 6]", ClampMin = "1.0", ClampMax = "8.0", UIMin = "1.5", UIMax = "4.0"))
	float MortimerMantleReachRadii = 2.8f;

	/**
	 * THE TALLEST LEDGE HE MAY MANTLE, AS A MULTIPLE OF HIS OWN JUMP APEX, BEFORE GENEROSITY.
	 *
	 * *** A MULTIPLE OF THE JUMP, NOT A NUMBER OF uu (spec v24 §0), AND THIS IS THE ONE THAT MATTERS
	 * MOST. *** The mantle's whole meaning is "a ledge he could not otherwise get onto", so the thing
	 * it modifies is the JUMP, and the jump is JumpZVelocity under the pawn's own gravity:
	 *
	 *     apex    = JumpZVelocity^2 / (2 x |GravityZ|)
	 *     ceiling = apex x MortimerMantleApexReach x MortimerMantleGenerosity
	 *
	 * The legacy knob was the flat number 230 uu. Had anybody retuned JumpZVelocity or
	 * MovementGravityScale — both of which this project has moved more than once — the mantle window
	 * would have stayed at 230 while the jump moved underneath it, and "he mantles what he cannot
	 * jump" would have quietly become false in one direction or the other. At today's 640 uu/s jump
	 * and 1.12 gravity scale the apex is ~187 uu, so 1.23 x 1.30 x 187 = ~298 uu — the legacy
	 * 230 x 1.30 = 299 uu, now derived. The GROUND press additionally refuses any ledge at or below
	 * the apex, because a jump already gets him there and stealing the jump key for it would be a
	 * regression, so on the ground the live window is (apex, ceiling].
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Mantle Ceiling (JUMP APEXES, before generosity) [Demo 21 item 6]", ClampMin = "0.5", ClampMax = "4.0", UIMin = "1.0", UIMax = "2.0"))
	float MortimerMantleApexReach = 1.23f;

	/**
	 * SECONDS BEFORE HE MAY MANTLE AGAIN. 0.35, the legacy MantleCooldownSeconds.
	 *
	 * NOT SCALED BY GENEROSITY — see that knob. It is a rate limit, not a window: it exists so a held
	 * jump key cannot re-fire the probe every frame, and so the "ladder to the sky" the legacy mantle
	 * measured (289 mantles in 25 s, a bot team carried from Z=313 to Z=4097) cannot come back by a
	 * route the geometry guards missed.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Mantle Cooldown (s) [Demo 21 item 6]", ClampMin = "0.0", ClampMax = "5.0", UIMin = "0.0", UIMax = "1.0"))
	float MortimerMantleCooldownSeconds = 0.35f;

	/**
	 * QUAKE'S REACH, in uu, measured centre to centre.
	 *
	 * 600 is a little under the dash reach everybody else has (594 uu) and a lot more than Chut's
	 * bash (130 uu), which is the right shape for the two abilities: the bash is a contact effect on
	 * a moving player, and this is a "get off me" a stationary carrier pays a 20 s cooldown for.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Radius (uu) [v19 §3]", ClampMin = "50.0", ClampMax = "3000.0", UIMin = "200.0", UIMax = "1200.0"))
	float MortimerBlastRadiusUU = 600.f;

	/**
	 * HOW HARD QUAKE THROWS A VICTIM, in uu/s, at the centre of the blast.
	 *
	 * Same units and the same launch shape as Chut's bash (1118 uu/s), a little stronger because it
	 * is on a 20 s cooldown and costs the carrier their whole aim for the frame. Like the bash it is
	 * an OVERRIDE and not an addition, so a sprinting enemy is thrown exactly as far as a standing
	 * one — see the LaunchCharacter call.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Knockback Speed (uu/s) [v19 §3]", ClampMin = "0.0", ClampMax = "6000.0", UIMin = "400.0", UIMax = "2500.0"))
	float MortimerBlastKnockbackSpeed = 1300.f;

	/** Upward part of the launch, as a fraction of the speed. Same shape and reason as ChutBashUpBias. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Up Bias (fraction of the knockback speed)", ClampMin = "0.0", ClampMax = "1.5", UIMin = "0.0", UIMax = "0.8"))
	float MortimerBlastUpBias = 0.35f;

	/**
	 * FALLS OFF WITH DISTANCE, or is flat.
	 *
	 * True (default) scales the launch linearly from full at his feet to
	 * MortimerBlastMinFalloffScale at the rim. A flat blast makes the rim a cliff — one uu decides
	 * between full knockback and none — which reads as inconsistent rather than as a radius.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Falls Off With Distance [v19 §3 ASSUMPTION: true]"))
	bool bMortimerBlastFallsOff = true;

	/** What the launch is worth at the very rim, as a fraction of the centre value. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Knockback At The Rim (fraction of centre)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.1", UIMax = "1.0"))
	float MortimerBlastMinFalloffScale = 0.35f;

	/**
	 * REQUIRE AN UNBLOCKED LINE TO THE VICTIM.
	 *
	 * §3 says "nearby enemies" and says nothing about walls; true is the [ASSUMPTION], because a
	 * blast that reaches through a solid wall is the kind of thing that reads as a bug rather than as
	 * a big radius, and Mortimer is stood still on the ground when he casts it — the geometry between
	 * him and his victim is exactly the counterplay.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Needs Line Of Sight [v19 §3 ASSUMPTION: true]"))
	bool bMortimerBlastNeedsLineOfSight = true;

	/**
	 * §3 does not give one. [ASSUMPTION] 20 s, the same as Chut's, Rocco's and Mace's — it is a
	 * defensive reset with no damage attached, so it sits at the roster's baseline rather than with
	 * the 25-35 s abilities that end fights.
	 *
	 * The card PRINTS this number too (Core/TraceCharacterRoster.cpp) and Trace.VerifyCharacterData
	 * section D fails if the two ever drift.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Cooldown (s) [v19 §3 ASSUMPTION: 20]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float MortimerBlastCooldownSeconds = 20.f;

	// --- QUAKE'S COSMETIC (Demo 20 item 3) --------------------------------------------------------
	//
	// DEMO 20, VERBATIM: "Mortimer's quake isn't working". IT WAS WORKING. It launched every enemy
	// inside 600 uu and started its 20 s cooldown, and it did so with NO particle, NO Niagara, NO
	// sound, NO camera shake and NO debug draw anywhere in TraceAbilitySetMortimer — so a press with
	// nobody standing within 600 uu of him produced literally nothing on screen, and a press that was
	// REFUSED (not carrying the Core, or airborne) produced nothing either, because
	// UTraceAbilityComponent::TryActivate() discards the FText reason CanActivate() fills in.
	//
	// The three knobs below are the shockwave that makes a cast visible whether or not it hits
	// anybody. They are cosmetic ONLY: ATraceMortimerQuakeWave has no collision, applies no effect
	// and is never asked about by any rule. Nothing about the knockback moved.

	/**
	 * How long the shockwave ring takes to reach MortimerBlastRadiusUU and fade out, in seconds.
	 *
	 * 0.90 rather than the ~0.35 an impact effect usually gets. The ability is on a 20 s cooldown and
	 * is a "get off me" the caster spends their whole aim on; it should read as an event, and a
	 * third of a second at the far end of a 600 uu radius is a frame and a half of visible ring at
	 * 60 Hz. It is also the window a screenshot has to land in for anybody to be able to VERIFY this,
	 * which is exactly how the last three "it does nothing" reports went unanswered.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Shockwave Duration (s) [Demo 20 item 3]", ClampMin = "0.1", ClampMax = "5.0", UIMin = "0.3", UIMax = "2.0"))
	float MortimerQuakeWaveSeconds = 0.90f;

	/**
	 * The shockwave's colour: MORTIMER'S ACCENT — cold patinated steel #5DB5A2. The ring, the ground
	 * cracks and the dust all wear it — one hue per effect (bible §6.2).
	 *
	 * *** THE SOURCE OF TRUTH IS TraceCharacterRoster::All()[Mortimer].Accent
	 *     (Core/TraceCharacterRoster.cpp), NOT ART_BIBLE §2.3 AND NOT THIS LINE. *** It is copied
	 * here only because a UPROPERTY(config) default cannot call a function and because this is a
	 * designer knob that is allowed to hold something other than his accent. Re-tune the accent and
	 * THIS LINE MOVES BY HAND — it did not, once: the v25 re-space took him from slate #A6BFED
	 * (7.2 deg from team Blue, which is why it had to move) to #5DB5A2, and this default kept the old
	 * slate, so his quake rang out in one colour while his body wore another for a whole wave.
	 *
	 * *** THE DARKER ACCENT COSTS THE RING 3.4% OF ITS BRIGHTNESS AND NOTHING ELSE, AND THAT IS
	 *     ARITHMETIC RATHER THAN HOPE. *** ATraceMortimerQuakeWave scales its glow so that the
	 * brightest channel x Glow lands on DefaultHueHeadroom = 2.0, capped at scale 1. Old slate:
	 * brightest 0.85 x PeakGlow 4.2 = 3.57, scaled by 0.560 to hit exactly 2.00. New steel: brightest
	 * 0.46 x 4.2 = 1.93, which is already under the headroom, so the scale saturates at 1.0 and the
	 * achieved product is 1.93 instead of 2.00. Still well above the arena floor grid's ~1.5 (§3.2
	 * T0), so the telegraph still out-reads the floor it is drawn on. It also clips LATER: red now
	 * reaches 1.0 only at a product of 4.18, where slate's reached it at 2.24, so the white-ring
	 * failure this knob has a paragraph about is further away than it was, not closer.
	 *
	 * *** IT USED TO BE (0.45, 0.70, 1.6) AND THAT WAS A BUG, NOT A BRIGHTER SLATE — a historical
	 *     note, from back when the accent this tracks WAS slate. *** The old
	 * comment here said the ring was "the same hue pushed past 1.0 so it clears the bloom threshold",
	 * and that is not what a channel above 1 does. M_TraceNeon computes Emissive = Color x Glow and a
	 * material instance CLAMPS vector parameters to [0,1], so the 1.6 clamped to 1.0 and the ring's
	 * actual hue became (0.45, 0.70, 1.00) — a paler, bluer colour than slate, which was then
	 * multiplied by a Glow of 6 and came back very nearly white. Bloom is cleared by GLOW and only by
	 * Glow (UTraceFxShapes::SetGlow documents the same trap for every other effect in the project);
	 * the wave's is now bible §3.2's FX-transient ceiling, 4.2, in ATraceMortimerQuakeWave.
	 *
	 * ATraceMortimerQuakeWave::BuildIfNeeded normalises anything above 1 back into a hue, so a value
	 * set here in the old style still means what its ratios say rather than turning white.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Shockwave Colour [Demo 20 item 3]"))
	FLinearColor MortimerQuakeWaveColor = FLinearColor(0.11f, 0.46f, 0.36f, 1.f);

	/**
	 * Thickness of the ring's beads, uu. The ring is drawn as a circle of overlapping cylinders — the
	 * same construction as Rocco's Ripple rings, for the same reason: it needs no generated content
	 * and degrades to /Engine/BasicShapes on an install that never ran Scripts/generate_content.py.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Mortimer", meta = (DisplayName = "Quake Shockwave Thickness (uu) [Demo 20 item 3]", ClampMin = "2.0", ClampMax = "80.0", UIMin = "8.0", UIMax = "40.0"))
	float MortimerQuakeWaveThicknessUU = 22.f;

	// ==========================================================================================
	// LILY  (spec v19 §3, new — roster 8 -> 10)
	//
	// Verbatim, Demo 18:
	//   MOVEMENT  "an extra dash" — 2 normally, 3 while carrying the Core — and "only 60 health"
	//   PASSIVE   "+30% wall-jump momentum bonus"   (hers alone)
	//   ACTIVATED "Zip (30 s): for 5 s she can fly. Jump goes up at walking speed, slide/crouch goes
	//             down, all other movement mechanics apply as usual. With the core the duration is
	//             halved. If she activates it and then picks up the core, the remaining duration is
	//             halved."
	//
	// ALL OF THESE ARE LIVE. The extra charge, the 60 health and the wall-jump bonus are surfaced
	// through TraceAbilityTraits and the three owning slices now call them (GetMaxDashCharges,
	// TraceHealthComponent::GetMaxHealth, TryWallJump's retention term). Zip is written entirely
	// inside her own ability set. An older note here said three of them were inert; it was true when
	// written and stopped being true without being edited.
	//
	// DEMO 19 changed two things here: the two Zip speed scales halve (item 4), and the extra dash
	// charge is now conditional on NOT carrying the Core (item 8 — the condition is in her ability
	// set, not in this knob).
	// ==========================================================================================

	/**
	 * EXTRA dash charges, ON TOP of everybody's pool. §3: "an extra dash — 2 normally, 3 while
	 * carrying the Core", against the shipped BaseDashCharges 1 + CarrierExtraDashCharges 1.
	 *
	 * AN ADDEND, NOT A TOTAL, deliberately: written as "2" it would stop tracking a retune of
	 * BaseDashCharges and Lily would silently stop being "one more than everyone".
	 *
	 * *** DEMO 19 ITEM 8 GATES IT ON NOT CARRYING. *** The knob is still 1 and still an addend; the
	 * CONDITION lives in UTraceAbilitySetLily::GetExtraDashCharges(), which returns 0 while she has
	 * the Core. Two dashes free-running, two while carrying — the carrier's own extra charge is what
	 * she gets then, not hers stacked on top of it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Lily", meta = (DisplayName = "Extra Dash Charges (ON TOP of everyone's pool) [v19 §3]", ClampMin = "0", ClampMax = "5", UIMin = "0", UIMax = "2"))
	int32 LilyExtraDashCharges = 1;

	/**
	 * HER MAX HEALTH. §3: "she has only 60 health", against UTraceSettings::MaxHealth's 100.
	 *
	 * AN ABSOLUTE, NOT A FRACTION, because the doc states an absolute: a "0.6 multiplier" would stop
	 * being 60 the moment somebody retuned MaxHealth, and 60 is the number the card prints.
	 *
	 * WHAT IT MEANS IN THE UNITS OF THE GUN: two 40-damage body shots (80) kill her outright where
	 * they leave everybody else at 20, and one 100-damage backstab or headshot always did. She is the
	 * only character in the game who dies to two body shots.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Lily", meta = (DisplayName = "Max Health (ABSOLUTE, not a fraction of MaxHealth) [v19 §3: 60]", ClampMin = "1.0", ClampMax = "1000.0", UIMin = "20.0", UIMax = "200.0"))
	float LilyMaxHealth = 60.f;

	/**
	 * +30% ON THE WALL JUMP'S MOMENTUM. §3: "+30% wall-jump momentum bonus (hers alone; the global
	 * wall-jump numbers must not move)".
	 *
	 * IT SCALES THE RETENTION TERM ONLY — the part of the launch that is the speed she ARRIVED with
	 * (WallJumpSpeedRetention x WallJumpMomentumScale x WallJumpMomentumScaleV10) — and never the
	 * flat WallJumpOutwardImpulse or WallJumpVerticalMultiplier. "Momentum" is the carried speed;
	 * the outward impulse is a fixed shove that a player who arrived at a standstill also gets, and
	 * scaling that would make her standing wall jumps stronger rather than her fast ones.
	 *
	 * THE AIR-STRAFE HARD CAP STILL APPLIES ON TOP. TryWallJump()'s ceiling is
	 * max(entry speed, AirStrafeHardCapSpeed), so this can hand her back more of what she arrived
	 * with but can never let her BUILD speed in the air — the whole point of spec v5 §1 survives.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Lily", meta = (DisplayName = "Wall-Jump Momentum Bonus (fraction; HERS ALONE) [v19 §3: 0.30]", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "0.6"))
	float LilyWallJumpMomentumBonus = 0.3f;

	/** §3: "for 5 s she can fly". The full-length Zip, taken without the Core. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Lily", meta = (DisplayName = "Zip Duration (s) [v19 §3: 5]", ClampMin = "0.25", ClampMax = "60.0", UIMin = "1.0", UIMax = "15.0"))
	float LilyZipDurationSeconds = 5.f;

	/**
	 * *** THE HALVING. ONE KNOB, TWO CLAUSES, AND THAT IS THE POINT. ***
	 *
	 * §3 says both "with the core the duration is halved" AND "if she activates it and then picks up
	 * the core, the remaining duration is halved". Those are the same 0.5 applied at two different
	 * moments — at the cast if she is already carrying, and to WHAT IS LEFT if the Core arrives
	 * mid-flight. Writing it once is what stops the second clause being implemented as "re-clamp to
	 * 2.5 s", which is the misreading the spec explicitly calls out: 4 s in with 1 s left, picking up
	 * the Core must leave 0.5 s, not 2.5 s.
	 *
	 * It applies once per PICKUP, not once per Zip: drop the Core and take it again and the remaining
	 * time halves again. That is the literal reading and it is the one that cannot be farmed.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Lily", meta = (DisplayName = "Zip: Core Halves It (at the cast, AND halves what is LEFT on a mid-Zip pickup) [v19 §3]", ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.25", UIMax = "1.0"))
	float LilyZipCarrierDurationScale = 0.5f;

	/**
	 * CLIMB RATE WHILE JUMP IS HELD, as a multiple of WalkSpeed.
	 *
	 * §3 said "jump goes up at walking speed" (1.0). *** DEMO 19 ITEM 4 HALVES IT: "half the speed she
	 * moves vertically at" *** — so 0.5, i.e. 400 uu/s against the shipped WalkSpeed of 800.
	 *
	 * STILL A MULTIPLE OF WalkSpeed AND NOT THE NUMBER 400, for the reason it was derived in the first
	 * place: the flight should follow a retune of the walk rather than drift away from it.
	 *
	 * Also in Config/DefaultGame.ini, which BEATS this default. Change both or change nothing.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Lily", meta = (DisplayName = "Zip Climb Speed (x WalkSpeed) [Demo 19 item 4: 0.5 = half a walk]", ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.25", UIMax = "2.0"))
	float LilyZipClimbSpeedScale = 0.5f;

	/**
	 * DESCENT RATE WHILE CROUCH/SLIDE IS HELD, as a multiple of WalkSpeed.
	 *
	 * §3 says only "slide/crouch goes down" and gives no rate; [ASSUMPTION] the same magnitude as the
	 * climb, so the control is symmetric and a player learns one number rather than two. Demo 19 item
	 * 4 asks for "half the speed she moves VERTICALLY at" — vertically, not upwards — so the descent
	 * halves with the climb and the assumption is preserved rather than quietly abandoned.
	 *
	 * Also in Config/DefaultGame.ini, which BEATS this default. Change both or change nothing.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Lily", meta = (DisplayName = "Zip Descend Speed (x WalkSpeed) [Demo 19 item 4 ASSUMPTION: same as the climb]", ClampMin = "0.1", ClampMax = "4.0", UIMin = "0.25", UIMax = "2.0"))
	float LilyZipDescendSpeedScale = 0.5f;

	/** §3: "Zip (30 s)". The card prints this too — Trace.VerifyCharacterData compares the pair. */
	UPROPERTY(config, EditAnywhere, Category = "Abilities|Lily", meta = (DisplayName = "Zip Cooldown (s) [v19 §3: 30]", ClampMin = "0.0", ClampMax = "180.0", UIMin = "5.0", UIMax = "60.0"))
	float LilyZipCooldownSeconds = 30.f;

	// ==========================================================================================
	// THE SMG  (spec v28 §9, new)
	//
	// Verbatim: "Add a full auto smg which does 33 to the head, 18 to the body, 12 to the leg. Make
	// the fire rate 600rpm, and give the gun 40 ammo. The reload time for the smg should be
	// .8seconds. Allow players to swap between the pistol and smg with the current weapon pullout
	// time."
	//
	//                     SMG          pistol (the six knobs above / UTraceDamageSettings)
	//   head/body/leg     33/18/12     100/40/25
	//   fire interval     0.100 s      0.315789 s
	//   RPM               600          190
	//   clip              40           30
	//   reload            1.3 s        0.5 s   (0.8 -> 1.3, spec v29 §2c)
	//
	// *** SIX ABSOLUTE NUMBERS AND NOT ONE RELATIVE ONE, AND THAT IS THE CORRECT READING OF THE
	// STANDING RULE. *** "A value that MODIFIES a base must be stored RELATIVE to that base." None of
	// these modifies the pistol: the owner gave six independent numbers for a second weapon, and
	// storing 33 as "0.33 x HeadDamage" would mean retuning the pistol's head shot silently retuned
	// the SMG's, which is the opposite of what a second weapon is for. They are siblings, not
	// derivatives.
	//
	// *** THE TWO NUMBERS THAT *ARE* RELATIVE ARE THE ONES THAT ARE NOT HERE, AND THAT IS THE POINT:
	//
	//   THE PULLOUT. "swap [...] with the current weapon pullout time" — so the SMG has NO pullout
	//   knob. UTraceWeaponComponent::RequestEquip reads UTraceMeleeSettings::SwapSeconds for every
	//   weapon, pistol, SMG and knife alike, exactly as it did before this pass. Retune that one
	//   number and all three move together. A SmgSwapSeconds duplicate is precisely what the standing
	//   rule forbids and is the single easiest mistake this item could have shipped.
	//
	//   THE PER-CHARACTER FIRE RATE. Roxie's Modded (x1.65) and a stuck Slimeball (+30%) reach the gun
	//   as a SCALE on the interval through UTraceAbilityComponent::GetFireIntervalScaleFor(), and the
	//   SMG multiplies by the identical call in the identical two places the pistol does (CanFire and
	//   ServerFire). So Roxie's SMG is 0.100 / 1.65 = 0.0606 s = 990 RPM and nothing had to know that
	//   in advance. Read it back from a running game with Trace.Smg.Dump.
	//
	// MIRRORED IN Config/DefaultGame.ini under [/Script/Trace.TraceSettings], beside FireInterval and
	// ClipSize, and *** THE INI IS THE ONE THAT DECIDES ***. Change both or change nothing, and read
	// the live numbers back with Trace.Smg.Dump rather than trusting either file.
	// ==========================================================================================

	/**
	 * SECONDS BETWEEN SMG ROUNDS. 600 RPM = 60/600 = 0.1 s exactly.
	 *
	 * A PERIOD, like FireInterval, so a SMALLER number is a FASTER gun — the same trap the pistol's
	 * ini block warns about. The ability scales DIVIDE into this through GetFireIntervalScaleFor().
	 *
	 * Worth reading against the clip beside it: 40 rounds at 0.1 s empties in 39 x 0.1 = 3.9 s (the
	 * first round is free), then 1.3 s of reload (spec v29 §2c, was 0.8) — a 75% duty cycle against
	 * the pistol's 95%, down from 83%. At 18
	 * a body shot that is 720 potential damage per magazine and 3 s of continuous fire to spend it,
	 * versus the pistol's 750 over 9.2 s. The SMG's whole identity is that it front-loads the same
	 * magazine into a third of the time.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Fire Interval (s) [v28 §9: 0.1 = 600 RPM]", ClampMin = "0.02", ClampMax = "2.0", UIMin = "0.05", UIMax = "0.5"))
	float SmgFireInterval = 0.1f;

	/** "give the gun 40 ammo". Rounds per SMG clip; the reserve is infinite, exactly as the pistol's is. */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Clip Size (rounds) [v28 §9: 40]", ClampMin = "1", ClampMax = "999", UIMin = "10", UIMax = "80"))
	int32 SmgClipSize = 40;

	/**
	 * SMG RELOAD. *** 0.8 -> 1.3 THIS PASS (spec v29 §2c). ***
	 *
	 * A deadline on the shared clock at runtime, not a countdown, so the client that predicts it and
	 * the server that validates it mean the same instant.
	 *
	 * What the extra half second costs, in the units of the gun that spends it: 40 rounds at 0.1 s
	 * empties in 39 x 0.1 = 3.9 s (the first round is free), so the duty cycle falls from 3.9/4.7 =
	 * 83% to 3.9/5.2 = 75%. Sustained DPS at 18 a body shot goes 153 -> 138. The pistol's 0.5 s is
	 * untouched, so the reload is now the SMG's clearest downside where it used to be a rounding
	 * difference — which is the point of the change.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Reload Time (s) [v29 §2c: 1.3, was 0.8]", ClampMin = "0.05", ClampMax = "10.0", UIMin = "0.2", UIMax = "3.0"))
	float SmgReloadSeconds = 1.3f;

	/**
	 * "33 to the head". THREE HEAD SHOTS TO KILL against 100 health, where the pistol's one does.
	 * That is the trade the owner's numbers describe and it is worth stating: the SMG never one-shots.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Head Damage [v28 §9: 33]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "5.0", UIMax = "120.0"))
	float SmgHeadDamage = 33.f;

	/** "18 to the body". Six body shots to kill = 0.5 s of held trigger on target. */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Body Damage [v28 §9: 18]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "5.0", UIMax = "120.0"))
	float SmgBodyDamage = 18.f;

	/** "12 to the leg". Nine leg shots to kill = 0.8 s. */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Leg Damage [v28 §9: 12]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "5.0", UIMax = "120.0"))
	float SmgLegDamage = 12.f;

	// ==========================================================================================
	// SPEC v29 §2d — THE SMG'S DAMAGE FALLOFF
	//
	// Verbatim: "The values should drop to 24, 15, 10 after a certain range. 800 uu falloff"
	//
	// *** IT IS A CLIFF, NOT A RAMP, AND THAT WAS A CHOICE — THE SPEC SAYS TO PICK ONE AND SAY SO. ***
	// Inside SmgFalloffStartUU the SMG pays 33/18/12; past it, 24/15/10, with nothing in between.
	//
	// WHY THE CLIFF WON. The owner gave exactly two damage tables and exactly one distance. A ramp
	// needs a SECOND distance that nobody specified, and it would mean the stated 24/15/10 exist only
	// at some invented far range while every shot between 800 uu and that range pays a number the
	// owner never wrote down. The cliff is the only reading under which both of the owner's tables are
	// literally true and testable at any distance: 799 uu pays 33, 801 uu pays 24, and a harness can
	// assert both. It also keeps the SMG's shots-to-kill countable in the head — 3/6/9 close, 5/7/10
	// far — which a ramp destroys.
	//
	// WHAT THE CLIFF COSTS, STATED HONESTLY: a 27% swing in payout across two uu of range. If that
	// reads badly in a playtest, SmgFalloffRampUU below is the fix and it is a live number, not a
	// rebuild — that is exactly why it exists rather than the cliff being hard-coded.
	//
	// *** SMG ONLY. *** The pistol resolves through FTraceHitZoneModel::DamageForZone and
	// UTraceDamageSettings and reaches none of this; §2d says "Only the SMG".
	// ==========================================================================================

	/**
	 * Master switch for the falloff. FALSE is the RED ARM — the v28 SMG, one flat damage table at
	 * every range — which is what Trace.Weapons.V29 measures against so that a passing far-range
	 * check cannot be an accident of some other rule.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Damage Falloff Enabled [v29 §2d]"))
	bool bSmgDamageFalloff = true;

	/**
	 * "800 uu falloff". Shots at or inside this range pay the near table; past it, the far table.
	 *
	 * Measured muzzle-to-impact along the bullet, on the SERVER, from the same rewound resolve that
	 * decided the zone — so the range that is priced is the range the server agrees the shot travelled,
	 * not a client-supplied number.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Falloff Start (uu) [v29 §2d: 800]", ClampMin = "0.0", ClampMax = "40000.0", UIMin = "200.0", UIMax = "5000.0"))
	float SmgFalloffStartUU = 800.f;

	/**
	 * LENGTH of the blend past the start, in uu. *** 0 = THE CLIFF, WHICH IS WHAT SHIPS. ***
	 *
	 * *** STORED AS A LENGTH FROM THE START, NEVER AS AN ABSOLUTE END DISTANCE (standing rule). ***
	 * The end of the ramp is SmgFalloffStartUU + this. Storing "SmgFalloffEndUU = 1600" instead would
	 * mean moving the 800 to 1200 silently shortened the ramp from 800 uu to 400, and moving it past
	 * the end would invert it into a damage BOOST with distance — a modifier that does not move with
	 * the base it modifies is exactly the bug this project's standing rule exists to prevent.
	 *
	 * Set it to e.g. 400 and the drop becomes linear from 800 uu to 1200 uu, with the far table
	 * reached at 1200 and held beyond. Nothing else has to change; the harness prints the resolved
	 * curve either way.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Falloff Ramp Length (uu past the start; 0 = a cliff) [v29 §2d]", ClampMin = "0.0", ClampMax = "40000.0", UIMin = "0.0", UIMax = "2000.0"))
	float SmgFalloffRampUU = 0.f;

	/**
	 * "drop to 24, 15, 10". THE FAR TABLE, HELD AT EVERY RANGE PAST THE FALLOFF.
	 *
	 * *** THREE ABSOLUTE NUMBERS AND NOT THREE FRACTIONS, AND THAT IS THE CORRECT READING OF THE
	 * STANDING RULE. *** The rule is that a value which MODIFIES a base must be relative to it. These
	 * do not modify the near table: the owner gave six independent numbers for two tables, and storing
	 * 24 as "0.727 x SmgHeadDamage" would mean retuning the close-range head shot silently retuned the
	 * long-range one by a ratio nobody chose. They are siblings, exactly as the SMG's six numbers are
	 * siblings of the pistol's rather than derivatives of them (see the v28 §9 block above, which made
	 * the identical argument for the identical reason).
	 *
	 * 24 = five head shots to kill past 800 uu instead of three.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Head Damage past the falloff [v29 §2d: 24]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "5.0", UIMax = "120.0"))
	float SmgFarHeadDamage = 24.f;

	/** "15" past the falloff. Seven body shots to kill instead of six. */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Body Damage past the falloff [v29 §2d: 15]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "5.0", UIMax = "120.0"))
	float SmgFarBodyDamage = 15.f;

	/** "10" past the falloff. Ten leg shots to kill instead of nine. */
	UPROPERTY(config, EditAnywhere, Category = "Combat|SMG", meta = (DisplayName = "SMG Leg Damage past the falloff [v29 §2d: 10]", ClampMin = "0.0", ClampMax = "500.0", UIMin = "5.0", UIMax = "120.0"))
	float SmgFarLegDamage = 10.f;

	// ==========================================================================================
	// SPEC v29 §2b — FIRE MODE, PER WEAPON
	//
	// Verbatim: "The pistol is NOT full auto. It must fire once per trigger press. The SMG stays
	// full auto."
	//
	// Until this pass there was no fire-MODE anywhere in the codebase and "full auto" was an emergent
	// property of UTraceWeaponComponent::TickComponent re-firing every frame a held trigger passed
	// CanFire() — see the note that used to sit in that function and in TraceAbilitySetRoxie.h. Both
	// guns were therefore automatic, including the one the owner has now said must not be.
	//
	// TWO KNOBS, ONE PER WEAPON, rather than one "bPistolSemiAuto" exception. A single knob would make
	// one gun the rule and the other the special case, and the third weapon would arrive with nowhere
	// to say what it is. The knife has no fire mode and reads neither.
	//
	// *** THIS IS ALSO WHAT FINALLY GIVES ROXIE'S MODDED ITS "full auto" CLAUSE SOMETHING TO DO. ***
	// Spec v18 §2 says MODDED makes "the gun becomes full auto", and UTraceAbilitySetRoxie has carried
	// IsFullAutoForced() since then as a statement of intent that nothing read, precisely because the
	// base gun was already automatic. With the pistol semi-automatic, a Roxie with MODDED up holds the
	// trigger on a PISTOL and it runs — for the first time that clause is a mechanic and not a comment.
	// ==========================================================================================

	/**
	 * *** FALSE, AND THAT IS THE WHOLE OF §2b. *** One round per trigger press: the tick's repeat is
	 * refused for the pistol, so releasing and pressing again is the only way to fire the next round.
	 *
	 * TRUE is the RED ARM (the v28 pistol) and Trace.Weapons.V29 measures both.
	 *
	 * The fire INTERVAL still applies on top: a press inside 0.3158 s of the last round is refused by
	 * the same rate gate it always was. Semi-automatic is a ceiling on how the trigger repeats, not a
	 * second cadence.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Fire Mode", meta = (DisplayName = "Pistol is FULL AUTO [v29 §2b: FALSE — one shot per press]"))
	bool bPistolFullAuto = false;

	/** "The SMG stays full auto." A held trigger keeps feeding it at SmgFireInterval. */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Fire Mode", meta = (DisplayName = "SMG is FULL AUTO [v29 §2b: TRUE — unchanged from v28]"))
	bool bSmgFullAuto = true;

	// ==========================================================================================
	// SPEC v29 §2f — THE 537 RPM BUG, AND THE KNOB THAT FIXES IT
	//
	// The SMG measured 537 RPM against a 600 RPM knob over 42 held rounds: a mean interval of
	// 0.1117 s against 0.1000 s. Nothing between the knob and the trigger was scaling anything.
	//
	// *** IT WAS FRAME QUANTISATION, AND THE ARITHMETIC IDENTIFIES THE FRAME RATE EXACTLY. *** The
	// fire poll runs on UTraceWeaponComponent::TickComponent and the gate is
	// (Now - LastLocalFireTime) >= Interval, with LastLocalFireTime then set to NOW — the frame's
	// time, not the time the shot was due. Every round therefore rounds UP to the next frame boundary
	// and the error is thrown away rather than carried, so the real cadence is
	// dt * ceil(Interval / dt), never the interval itself:
	//
	//     dt = 1/53.7 s  ->  0.018617 * ceil(0.1 / 0.018617) = 0.018617 * 6 = 0.11170 s = 537 RPM
	//
	// That is the measured number to four decimal places, and it also explains why the pistol never
	// looked broken: 0.315789 / 0.018617 = 16.96, which rounds up to 17 frames = 0.3165 s, an error of
	// 0.2% instead of 12%. THE FASTER THE GUN, THE WORSE THE QUANTISATION — which is why a 600 RPM
	// weapon is what finally made a bug that has been in this file since v5 visible.
	//
	// THE FIX IS THIS KNOB: the shot stamp keeps the leftover instead of discarding it, so the next
	// round is due one exact interval after the one that was DUE rather than after the frame that
	// happened to deliver it, and the mean converges on the knob at any frame rate.
	// ==========================================================================================

	/**
	 * How much of an interval the fire clock may carry from one round to the next, AS A FRACTION OF
	 * THE FIRE INTERVAL.
	 *
	 * *** A FRACTION AND NEVER A NUMBER OF SECONDS (standing rule). *** It modifies the fire interval,
	 * so it is stored relative to it: 0.2 means "up to a fifth of an interval", which is 0.02 s for the
	 * SMG and 0.063 s for the pistol without either weapon being named here. A seconds knob would have
	 * been tuned against the SMG and would silently under-correct the pistol, and would have to be
	 * re-tuned the next time either gun's rate moves.
	 *
	 * *** 0.0 IS THE RED ARM: the shipped 537 RPM bug, exactly. *** Trace.Weapons.V29 measures 40+
	 * rounds at 0.0 and at the shipped value and prints both, because two arms that agree would mean
	 * the harness is not measuring this rule.
	 *
	 * *** WHY 0.2 IS THE CEILING AS WELL AS THE DEFAULT. *** UTraceWeaponComponent::FireRateTolerance
	 * is the fraction of an interval the SERVER forgives an early round (0.2). A client that carried
	 * more than that could ask for a round the server would reject as rate-limited, which reads in
	 * game as the gun eating bullets. The code clamps this knob to that constant rather than trusting
	 * the ini — the two numbers are one rule and must not be able to drift apart.
	 *
	 * Simulated across 20-160 fps with and without frame jitter: at 0.2 the mean lands within 0.1% of
	 * the knob at every frame rate above ~30 fps and NO interval ever falls below the server's gate.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Combat|Fire Mode", meta = (DisplayName = "Fire Clock Carry (fraction of the fire interval) [v29 §2f: 0.2; 0 = the 537 RPM bug]", ClampMin = "0.0", ClampMax = "0.2", UIMin = "0.0", UIMax = "0.2"))
	float FireIntervalCarryFraction = 0.2f;
};
