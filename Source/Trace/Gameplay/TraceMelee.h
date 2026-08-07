// Trace — THE KNIFE (spec v10 §1).
//
// Verbatim from the design notes:
//
//     "Add a melee, which players can swap to from the gun (and vice versa). This melee should look
//      and act like a knife. Players should move 30% faster with a knife, as well as have a higher
//      momentum ceiling. The knife should do 100 damage if hitting a player in the back. It should
//      do 30 damage in the front. It should take .5seconds after a knife swing before a player can
//      knife again. Knife and gun pullout time should be roughly .2seconds."
//
// WHERE THE PIECES LIVE, AND WHY
//
// This header is the *policy and the geometry*: the tunables, the pure damage/angle model, the
// swept-arc resolver, and the read-only queries the movement component, the bots and the HUD want.
// The *state* — which weapon is in your hands, when the pullout finishes, when the last swing was —
// lives on UTraceWeaponComponent, deliberately, and for exactly the reasons TraceParry gives for
// putting the parry window on the trail component:
//
//   * the weapon component already exists on every ATraceCharacter, already replicates, and already
//     owns the other half of the same decision ("can this pawn attack right now"), so there is no
//     new dynamically-attached replicated subobject and no second object to keep in agreement;
//   * gun and knife are ONE selector, not two systems. A separate melee component would mean two
//     objects agreeing about which weapon is out — this codebase already has a four-writer bug of
//     exactly that shape logged against ATracePlayerState::bIsCarrier;
//   * the first-person viewmodel is a private rig on ATraceCharacter and the only outside handle on
//     it is the public ViewModelRoot; whatever swaps the visible weapon has to be the thing that
//     also decides which weapon is equipped, or the two desynchronise on the frame a swap lands.
//
// ==============================================================================================
// *** THE KNIFE CANNOT HURT THE CORE CARRIER. READ THIS BEFORE "FIXING" IT. ***  [USER-CONFIRMED]
// ==============================================================================================
//
// The next reader will assume a knife should bypass a shield that stops bullets — a blade is not a
// bullet, and "shielded against gunfire" is a sentence that invites exactly that inference. It does
// not bypass it here, and this was asked of the user directly and confirmed:
//
//     "THE KNIFE CANNOT HURT THE CORE CARRIER. The carrier's shield blocks the knife exactly as it
//      blocks bullets, and the trace remains the only way to kill a carrier."
//
// The design reason, which is stronger than the fiction: a 100-damage back-stab would make dashing
// the trace pointless. The whole of contract §3 is that a carrier is untouchable except by an enemy
// who dashes through the trail behind them — a skill test with a counter (the parry). Give the
// knife a one-hit kill on the carrier and every hunt collapses into "walk up behind them", the
// trace becomes decoration and the parry has nothing to parry.
//
// TWO THINGS FOLLOW, AND BOTH ARE IMPLEMENTED EXPLICITLY RATHER THAN INHERITED:
//
//   1. ResolveSwing() SKIPS ANY CORE HOLDER, including one whose shield is currently SUPPRESSED by
//      a pass (ATraceCore::IsShieldSuppressedFor). This is deliberately STRICTER than the bullet
//      rule, and the asymmetry is the point. Bullets may kill a passing carrier — that is spec §4's
//      risk beat, and it costs the shooter a full magazine of accuracy at range. A back-stab would
//      cash the same 0.5 s commitment for one keypress at contact range, which is not the same
//      trade at all and is not what "the trace remains the only way to kill a carrier" means.
//      UTraceLagCompensationComponent::ResolveHitscan already skips UNsuppressed holders, so the
//      knife would have been immune-by-accident for most of the match and lethal for the half
//      second that matters most. Immunity that holds only when nobody is looking is not immunity.
//
//   2. THE CARRIER CANNOT SWING, and cannot swap at all. They cannot shoot (spec §4), their hands
//      are full, and a carrier who could hold a knife would collect the +30% movement bonus on top
//      of UTraceSettings::CarrierSpeedMultiplier — 1.08 x 1.30 = 1.40x — which would make the Core
//      uncatchable and quietly retire the one number the carrier's speed was ever tuned with.
//      GetGroundSpeedMultiplierFor() therefore ALSO returns 1.0 for a carrier, as defence in depth:
//      if a future change ever lets a knife be equipped before a pickup, the bonus still does not
//      survive the pickup.
//
// There is deliberately NO SETTING that re-enables carrier damage. A knob to restore a rule the
// user ruled out is a knob to restore the bug, and this file follows TraceTracer's precedent
// ("DELETED, NOT DISABLED") on exactly that point.
//
// ==============================================================================================
// THE SWING IS A SWEEP, NOT A POINT
// ==============================================================================================
//
// Spec §1: "the swing itself should read as a swing (an arc/sweep, not a hitscan point)". So the
// resolver fans SwingSamples rays across SwingArcDegrees in the attacker's view plane and takes the
// nearest body any of them reaches. Two consequences worth stating:
//
//   * WHAT THE PLAYER FEELS. You do not have to have the crosshair on a body. Anything inside the
//     arc, at knife range, in line of sight, is cut — which is what a slash is, and it is what makes
//     the knife usable while sprinting past somebody at 1040 uu/s.
//   * WHAT IT COSTS. 13 samples across 140 degrees at 180 uu puts the outermost chord spacing at
//     ~34 uu, i.e. one capsule radius, so a body cannot slip between two adjacent rays anywhere in
//     the arc. Thirteen candidate loops once per 0.5 s per player is nothing next to a 150 RPM gun.
//
// Each sample goes through UTraceLagCompensationComponent::ResolveHitscan — the SAME resolver the
// gun uses, not a copy of it. That is not laziness, it is the whole lag-compensation story: the
// knife inherits the rewound pose history, the world-geometry occlusion (a blade does not cut
// through a wall), the friendly-fire rule and the dead/self exclusions, and it inherits them as the
// same code rather than as the same intention. Spec §1: "a melee that misses on the client's screen
// but connects on the server is worse than a gun that does" — the fix for that is one resolver.
//
// ==============================================================================================
// BACK OR FRONT, AND NOTHING IN BETWEEN
// ==============================================================================================
//
// 100 from behind, 30 from the front, decided by the angle between the attacker's APPROACH and the
// victim's FACING. Let F be the victim's planar forward and A the planar direction from the
// attacker to the victim. If the attacker stands behind the victim then A points the same way F
// does, so:
//
//     back-stab  <=>  angle(A, F) <= BackstabHalfAngleDegrees        (default 90 degrees)
//
// At the default the two hemispheres exactly bisect the victim: rear half 100, front half 30. The
// spec asks for "within ~90 degrees of directly behind" and explicitly refuses a side tier, so the
// threshold is one number with no third case, and it is exposed (see UTraceMeleeSettings).
//
// THE VICTIM'S FACING HAS TO BE REWOUND TOO, and this is the one place the knife could not reuse
// the gun's machinery. FTraceLagCompFrame records a capsule — centre, half height, radius, posture
// — and no rotation at all, because a bullet does not care which way you are looking. A back-stab
// cares about nothing else. Under 40 ms of lag a spinning player's yaw is stale by 80 ms round
// trip, which at a brisk 400 deg/s turn is 32 degrees — enough to flip the verdict, and it would
// flip it in the direction the attacker cannot see. So UTraceWeaponComponent keeps its own tiny
// server-side yaw ring (one float per frame per pawn, trimmed by the same
// UTraceSettings::LagCompHistoryDuration window) and ResolveSwing asks the victim's component for
// its yaw AT THE REWOUND INSTANT. See UTraceWeaponComponent::GetFacingYawAtTime.
//
// ACTOR yaw, not control yaw, and that is a deliberate choice: the attacker is aiming at a MESH,
// and the mesh faces the actor. A human in first person has the two equal (bUseControllerRotationYaw);
// a bot is on orient-to-movement, so its control rotation points at whatever it is shooting while
// its body points where it is running — and the body is what you can see to get behind.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"   // module: DeveloperSettings
#include "Math/UnrealMathUtility.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"     // GetDefault<>

#include "Gameplay/TraceHitZones.h"     // ETraceHitZone, carried through for the hitmarker

#include "TraceMelee.generated.h"

class AActor;
class ATraceCharacter;
class UWorld;

/**
 * Which weapon a pawn currently has in its hands.
 *
 * Gun is 0 so a default-constructed pawn, a freshly replicated proxy and a pawn whose weapon
 * component has not begun play all agree on "the gun", which is what every other system in the
 * build assumes when it asks whether somebody can shoot.
 */
UENUM()
enum class ETraceEquippedWeapon : uint8
{
	Gun   = 0,
	Knife = 1
};

/** "GUN" / "KNIFE". Logs and HUD only. */
TRACE_API const TCHAR* LexToString(ETraceEquippedWeapon Weapon);

/** Why a swap or a swing was turned down. Reported to the log, and available to the HUD and bots. */
enum class ETraceMeleeRefusal : uint8
{
	/** Not refused. */
	None = 0,

	/** No pawn, or the pawn has no weapon component. */
	NoPawn,

	/** Dead players do not swing and do not swap. */
	Dead,

	/**
	 * Carrying the Core. See this file's header: the carrier cannot shoot, cannot swing and cannot
	 * swap, and the knife's movement bonus does not survive a pickup either.
	 */
	Carrying,

	/** Spec §6: no attacking during a dash. Released the instant the dash ends, not on a cooldown. */
	Dashing,

	/** Inside the 0.2 s pullout. Neither weapon works while one is being brought up. */
	Deploying,

	/** The 0.5 s between swings has not elapsed. */
	OnCooldown,

	/** Asked to swing with the gun out (or to fire with the knife out). */
	WrongWeapon,

	/** The input path was driven on a machine that does not own this pawn. */
	NotLocallyControlled
};

/** Human-readable form of @p Refusal, for logs and the HUD. */
TRACE_API const TCHAR* LexToString(ETraceMeleeRefusal Refusal);

/**
 * Everything one resolved swing produced. Filled by TraceMelee::ResolveSwing on both the predicting
 * client and the authoritative server, so the two can be compared line for line.
 */
struct FTraceMeleeHit
{
	/** Nearest eligible body any sample reached, or null. */
	ATraceCharacter* Victim = nullptr;

	/** Where the winning sample entered the body. Drives the arc effect's terminus. */
	FVector ImpactPoint = FVector::ZeroVector;

	/** True when the approach landed inside BackstabHalfAngleDegrees of the victim's forward. */
	bool bBackstab = false;

	/** BackstabDamage or FrontDamage. Zero when nothing was hit. */
	float Damage = 0.f;

	/** angle(approach, victim forward) in degrees. 180 means "no victim". Debug and logs only. */
	double ApproachAngleDegrees = 180.0;

	/** The victim yaw the verdict was reached with, and whether it came from recorded history. */
	float VictimYawDegrees = 0.f;
	bool bVictimYawRewound = false;

	/** Which zone the winning sample entered. Carried only so the hitmarker can stay positional. */
	ETraceHitZone Zone = ETraceHitZone::None;

	/** Index of the winning sample across the arc; 0 is the leftmost. -1 when nothing connected. */
	int32 SampleIndex = -1;

	/**
	 * A Core holder stood in the arc and stopped the blade. Not a hit and never damage — see the
	 * carrier section of this file's header. Recorded so the log can say WHY a visible contact paid
	 * nothing, which is the single most likely "the knife is broken" report.
	 */
	bool bBlockedByCarrierShield = false;
};

/**
 * The knife's numbers, as config.
 *
 * A SEPARATE settings page rather than more properties on UTraceSettings, for the same file
 * ownership reason UTraceDamageSettings gives: UTraceSettings belongs to another slice. Folding
 * these in later is a mechanical move — nothing outside this header reads these properties
 * directly, because the clamped accessors in namespace TraceMelee are the only readers.
 *
 * *** THE .INI WINS, AND IT NOW EXISTS. READ VALUES FROM A RUNNING GAME, NOT FROM HERE. ***
 *
 * `Config/DefaultGame.ini` carries a live `[/Script/Trace.TraceMeleeSettings]` block that sets all
 * thirteen properties below. The project rule is "values live in the header AND the Config .ini
 * files, and THE INI WINS" — so every initialiser in this class is a DEFAULT-OF-LAST-RESORT that the
 * shipped game never actually uses. Verified at the close of this pass: all thirteen ini keys agree
 * with the initialisers here, so the two do not currently disagree about anything.
 *
 * THAT AGREEMENT IS A FACT ABOUT TODAY, NOT AN INVARIANT. Changing a number here and not there
 * changes nothing about the running game and is the exact trap the project rule was written for.
 * Change BOTH, and read the result back with `Trace.Knife.DumpSettings` from a running game.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Trace Melee (Knife)"))
class TRACE_API UTraceMeleeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UTraceMeleeSettings();

	/** CDO, already populated by the config system. Cheap enough to call per swing. */
	static const UTraceMeleeSettings& Get();

	virtual FName GetCategoryName() const override;

	// --- Damage (spec §1) ------------------------------------------------------------------------

	/**
	 * "100 damage if hitting a player in the back". MaxHealth is 100, so this is a kill outright and
	 * must stay >= UTraceSettings::MaxHealth or the headline promise of the weapon stops being true.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Damage", meta = (ClampMin = "1.0", ClampMax = "500.0"))
	float BackstabDamage = 100.f;

	/**
	 * "30 damage in the front". Four front swings to kill at 0.5 s apart is 1.5 s of standing in
	 * somebody's face, which is the point: the knife's front hit is a punish for being caught out,
	 * not a duelling option against a gun that kills in three body shots.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Damage", meta = (ClampMin = "1.0", ClampMax = "500.0"))
	float FrontDamage = 30.f;

	/**
	 * Half-angle, in degrees, of the rear cone that counts as a back-stab — measured between the
	 * victim's forward vector and the direction from the attacker to the victim.
	 *
	 * 90 exactly bisects the victim: rear hemisphere 100, front hemisphere 30, no side tier, which
	 * is what spec §1 asks for ("within ~90 degrees of directly behind"; "There is no side tier").
	 * Lower it to make back-stabs demand a truer angle; the difference goes to FrontDamage, never to
	 * a third number.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Damage", meta = (ClampMin = "5.0", ClampMax = "179.0"))
	float BackstabHalfAngleDegrees = 90.f;

	// --- Timing (spec §1) ------------------------------------------------------------------------

	/**
	 * "It should take .5seconds after a knife swing before a player can knife again." Measured from
	 * the START of a swing, not from its resolve, so the interval a player can actually observe —
	 * press to press — is exactly this number.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (ClampMin = "0.05", ClampMax = "5.0"))
	float SwingCooldownSeconds = 0.5f;

	/**
	 * "Knife and gun pullout time should be roughly .2seconds." One number for both directions —
	 * the user gave one number, and a knife that comes up faster than it goes away is a second
	 * mechanic nobody asked for. Neither shooting nor swinging is possible for this long after a
	 * swap, on the server as well as on the client.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SwapSeconds = 0.2f;

	/**
	 * Delay from the press to the instant the blade is resolved, i.e. how far into the animation the
	 * edge passes through the target.
	 *
	 * This is what makes the swing read as a SWING. Resolving on the press would put the damage on
	 * frame one of a wind-up, so the victim would die before the blade had visibly moved — the exact
	 * "hitscan point" spec §1 rejects. It is charged against SwingCooldownSeconds rather than added
	 * to it (the cooldown runs from the press), so raising it does not slow the weapon down, it only
	 * moves the moment of truth later inside the same 0.5 s.
	 *
	 * Kept short. Every millisecond here is a millisecond the victim gets to walk out of the arc
	 * after the attacker has already committed, and the swing is lag-compensated to the resolve
	 * instant, not the press, so a long wind-up is a real cost to the attacker.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (ClampMin = "0.0", ClampMax = "0.4"))
	float SwingWindupSeconds = 0.10f;

	/** How long the whole swing animation runs. Cosmetic only; never gates anything. */
	UPROPERTY(config, EditAnywhere, Category = "Timing", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float SwingAnimSeconds = 0.32f;

	// --- Reach (spec §1: an arc, not a point) ----------------------------------------------------

	/**
	 * How far the blade reaches from the muzzle point, in uu.
	 *
	 * 180 is a bit over one capsule diameter (68) plus a capsule radius of standoff, so you connect
	 * at the range you would expect to be able to touch somebody and not appreciably further. Note
	 * this is measured from GetMuzzleLocation(), which already sits ~22 uu ahead of the eye.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Reach", meta = (ClampMin = "20.0", ClampMax = "1000.0"))
	float SwingRangeUU = 180.f;

	/**
	 * Total width of the swept arc in degrees, centred on the aim direction.
	 *
	 * 140 is wide enough that a passing slash connects without the crosshair being on the body,
	 * which is what separates a slash from a very short shotgun.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Reach", meta = (ClampMin = "5.0", ClampMax = "270.0"))
	float SwingArcDegrees = 140.f;

	/**
	 * How many rays the arc is sampled with. Keep it ODD so one sample always runs dead ahead.
	 *
	 * THE RULE FOR CHOOSING IT, and it is arithmetic rather than taste: the chord between adjacent
	 * rays at maximum reach,
	 *
	 *     chord = 2 * SwingRangeUU * sin( radians(SwingArcDegrees) / (2 * (SwingSamples - 1)) )
	 *
	 * must stay at or under the 34 uu capsule radius, or a body can sit between two rays and take
	 * nothing from a swing that visibly passed through it.
	 *
	 * MEASURED AND CORRECTED. This shipped at 13 on the strength of an estimate written into this
	 * comment — and Trace.Knife.AngleTest's coverage check, which computes the chord rather than
	 * estimating it, immediately reported 36.6 uu against a 34 uu limit. 15 gives 31.4 uu, which
	 * clears it. That is the whole reason the coverage check is IN the self-test instead of being a
	 * remark in a header: a 2.6 uu gap in a 140 degree arc is not something anybody would ever find
	 * by playing, and it would have read as "the knife sometimes just misses".
	 */
	UPROPERTY(config, EditAnywhere, Category = "Reach", meta = (ClampMin = "1", ClampMax = "51"))
	int32 SwingSamples = 15;

	// --- Movement -------------------------------------------------------------------------------
	//
	// *** THE KNIFE'S MOVEMENT NUMBERS ARE NOT HERE, AND MUST NOT BE ADDED HERE. ***
	//
	// "Players should move 30% faster with a knife, as well as have a higher momentum ceiling" is
	// implemented in UTraceCharacterMovementComponent, whose knobs live on UTraceSettings:
	//
	//     KnifeMoveSpeedMultiplier            1.30   ground speed x
	//     KnifeAirStrafeSoftCapMultiplier     1.25   air-strafe soft cap x
	//     KnifeAirStrafeHardCapMultiplier     1.35   air-strafe hard cap AND MaxAirSpeed x
	//
	// That is the right home for them and this file deliberately does not keep a second copy. The
	// bit is saved-move state round-tripped through FSavedMove_Trace, so a server correction replays
	// each move under the profile that move actually ran with — something a multiplier read from
	// here, outside the move pipeline, could not have given. A duplicate 1.30 in this table would be
	// two objects agreeing about one fact, which is the failure this codebase logs by name.
	//
	// THE MELEE SLICE OWNS THE BIT, NOT THE NUMBER. UTraceWeaponComponent::RefreshMovementProfile()
	// pushes UTraceCharacterMovementComponent::SetKnifeMovementProfileActive() on every machine
	// whenever the selector or the carrier state changes. Which pawns are fast is decided here; how
	// fast they are is decided there.

	// --- Bots (spec §1: "Bots must use it, or it will not be playtested") ------------------------

	/**
	 * Distance at or inside which a bot swaps to the knife to close on its target.
	 *
	 * The +30% makes the knife the correct chase tool, so the bot's rule is a range band rather than
	 * a state: inside this it is worth trading the gun for the speed, outside BotDisengageRangeUU it
	 * is not. The gap between the two is hysteresis — without it a bot at exactly the boundary
	 * swap-thrashes and spends its whole life in a 0.2 s pullout.
	 *
	 * 500, NOT 700, AND THE DIFFERENCE IS A BOT-DPS REGRESSION. A bot holding the knife cannot
	 * shoot, so this range is exactly how far a bot voluntarily disarms itself for. The blade
	 * reaches 180 uu; at the knife's 1040 uu/s, 500 uu is 0.31 s of closing, which reads as a
	 * commitment to a finisher. The first measured pass used 700 and produced bots that spent most
	 * of an engagement unarmed inside comfortable gun range. If bot lethality moves this pass, this
	 * is the first number to look at.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Bots", meta = (ClampMin = "0.0", ClampMax = "5000.0"))
	float BotEngageRangeUU = 500.f;

	/** Range at which a knife-carrying bot gives up and puts the gun back. Must exceed the engage range. */
	UPROPERTY(config, EditAnywhere, Category = "Bots", meta = (ClampMin = "0.0", ClampMax = "6000.0"))
	float BotDisengageRangeUU = 750.f;

	/** Fraction of SwingRangeUU a bot must be inside before it swings. Under 1 so it does not flail. */
	UPROPERTY(config, EditAnywhere, Category = "Bots", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float BotSwingRangeFraction = 0.85f;
};

/**
 * The knife, as seen by everything that is not the weapon component.
 *
 * Every function here is safe to call on any machine and on any actor, including null.
 */
namespace TraceMelee
{
	// ---------------------------------------------------------------------------------------------
	// TUNABLES.
	//
	// These accessors are the ONLY readers of UTraceMeleeSettings' properties. They apply the clamps
	// and the console overrides, so a caller cannot accidentally read an un-clamped value or miss an
	// override that a headless run set.
	//
	// The overrides all default to a negative sentinel meaning "defer to the setting". They exist
	// for console experiments during a -RenderOffScreen run, where there is no Project Settings
	// panel to open — they are not a second definition of the defaults:
	//
	//     Trace.Knife.BackstabDamage    -1
	//     Trace.Knife.FrontDamage       -1
	//     Trace.Knife.BackstabAngle     -1     half-angle in degrees
	//     Trace.Knife.Cooldown          -1     seconds between swings
	//     Trace.Knife.SwapSeconds       -1     pullout time, both directions
	//     Trace.Knife.Range             -1     uu
	//     Trace.Knife.SpeedMultiplier   -1     ground speed while the knife is out
	//     Trace.Knife.BotAuto            1     bots swap to the knife to close and swing in range
	//     Trace.Knife.Debug              0     one line per swap, swing and resolution
	// ---------------------------------------------------------------------------------------------

	TRACE_API float GetBackstabDamage();
	TRACE_API float GetFrontDamage();
	TRACE_API float GetBackstabHalfAngleDegrees();
	TRACE_API float GetSwingCooldownSeconds();
	TRACE_API float GetSwapSeconds();
	TRACE_API float GetSwingWindupSeconds();
	TRACE_API float GetSwingAnimSeconds();
	TRACE_API float GetSwingRangeUU();
	TRACE_API float GetSwingArcDegrees();
	TRACE_API int32 GetSwingSamples();
	TRACE_API float GetBotEngageRangeUU();
	TRACE_API float GetBotDisengageRangeUU();
	TRACE_API float GetBotSwingRangeFraction();
	TRACE_API bool  IsBotAutoKnifeEnabled();
	TRACE_API bool  IsDebugLoggingEnabled();

	/**
	 * Kill-feed causes. Two, not one, so the feed can tell a back-stab from a front swipe — the same
	 * argument UTraceWeaponComponent makes for passing "Headshot" instead of inferring it from the
	 * victim's previous health. The zone is known EXACTLY here and nowhere after.
	 *
	 * UI/TraceKillFeed.cpp maps both to their own glyphs (Knife, Backstab) — a blade and a blade
	 * with a rear chevron — so the feed distinguishes a 30-damage swipe from a 100-damage back-stab.
	 */
	TRACE_API FName GetBackstabKillCause();
	TRACE_API FName GetKnifeKillCause();

	/**
	 * How many times ResolveSwing has returned a CORE CARRIER as the victim, since process start.
	 *
	 * THIS MUST BE ZERO IN ANY SHIPPED BUILD. [USER-CONFIRMED] the knife cannot hurt the carrier, so
	 * a non-zero count means the rule has been broken — it is not a tuning signal, it is an alarm.
	 *
	 * It exists because measuring the rule through the victim's HEALTH does not work in a live
	 * match: the first staged attempt read a 100-point drop and reported the rule broken, when the
	 * carrier had simply been shot by somebody else during the 250 ms the harness was watching.
	 * Health is a shared resource with many writers; this counter has exactly one writer, at the one
	 * line where the rule is decided, so it cannot be confused by ambient combat.
	 */
	TRACE_API int32 GetCarrierKnifeHitCount();

	// ---------------------------------------------------------------------------------------------
	// THE DAMAGE MODEL — pure functions, no world, no actors. This is what the self-test exercises.
	// ---------------------------------------------------------------------------------------------

	/**
	 * "Did this approach land in the victim's back?"
	 *
	 * @param AttackerLocation    the attacker, world space. Only the planar component is used.
	 * @param VictimLocation      the victim, world space, at the REWOUND instant.
	 * @param VictimYawDegrees    the victim's actor yaw at the REWOUND instant.
	 * @param OutAngleDegrees     optional; angle(approach, victim forward), 0 = dead behind them.
	 * @return                    true for a back-stab.
	 *
	 * Degenerate input (the two positions planar-coincident) resolves to FRONT. That is the safe
	 * direction: a coincident pair means the attacker is standing inside the victim, which is not an
	 * approach anybody earned, and defaulting a one-hit kill to "yes" on a division by zero is how a
	 * 100-damage bug ships.
	 */
	TRACE_API bool IsBackstab(const FVector& AttackerLocation, const FVector& VictimLocation,
		float VictimYawDegrees, double* OutAngleDegrees = nullptr);

	/** BackstabDamage or FrontDamage. The only place the two numbers are chosen between. */
	TRACE_API float DamageForApproach(bool bBackstab);

	// ---------------------------------------------------------------------------------------------
	// RESOLUTION. Server-authoritative; also run by the predicting client for its own feedback.
	// ---------------------------------------------------------------------------------------------

	/**
	 * The axis the arc is swept about, for a given attacker and aim direction.
	 *
	 * EXPORTED SO THERE IS EXACTLY ONE OF IT. ResolveSwing builds the sweep around this, and
	 * ATraceMeleeArc draws the sweep around it — if the two derived it separately then the arc a
	 * player watches and the arc the server cut would be two different swings that merely usually
	 * agree, and the day they stopped agreeing nothing would say so.
	 *
	 * The arc lies in the VIEW plane, not the world plane: look up a ramp at somebody and the sweep
	 * tilts with the view instead of scything the floor under them. Degenerates only when the aim is
	 * exactly vertical, and falls back to the attacker's own right vector there.
	 */
	TRACE_API FVector GetSwingAxis(const ATraceCharacter* Attacker, const FVector& AimDirection);

	/**
	 * Sweeps the arc and returns the nearest body it reaches.
	 *
	 * @param World                 any game world; the resolver is pure with respect to it.
	 * @param Attacker              the swinging pawn; always excluded, never null in practice.
	 * @param Origin                blade origin — ATraceCharacter::GetMuzzleLocation().
	 * @param AimDirection          centre of the arc; normalised internally.
	 * @param RewindToServerTime    shared-clock instant to rewind to. CLAMP IT BEFORE CALLING, the
	 *                              same way UTraceWeaponComponent::ServerFire does; ResolveHitscan
	 *                              re-clamps defensively but the caller owns the intent.
	 * @param OutHit                always written, including on a miss.
	 * @return                      OutHit.Victim, for callers that want it inline.
	 *
	 * Runs on the client too (for the local arc effect and the predicted hitmarker) and applies no
	 * damage of any kind — the server owns that entirely, exactly as it does for the gun.
	 */
	TRACE_API ATraceCharacter* ResolveSwing(
		UWorld* World,
		ATraceCharacter* Attacker,
		const FVector& Origin,
		const FVector& AimDirection,
		float RewindToServerTime,
		FTraceMeleeHit& OutHit);

	// ---------------------------------------------------------------------------------------------
	// CROSS-SLICE QUERIES.
	//
	// The movement component, the bot controller and the HUD live in other ownership slices and must
	// never have to reach into UTraceWeaponComponent's state directly. These are the whole contract
	// surface, they are all safe on null, and they are all pure functions of REPLICATED state — so a
	// client and the server compute the same answer, which is what makes the movement bonus
	// prediction-safe (the same argument UTraceCharacterMovementComponent::GetMaxSpeed already makes
	// for UTraceSettings::CarrierSpeedMultiplier).
	// ---------------------------------------------------------------------------------------------

	/** True when @p Character currently has the knife out. False for null, a corpse, or the gun. */
	TRACE_API bool IsKnifeEquipped(const AActor* Character);

	/**
	 * "Should this pawn be running the knife's movement profile right now?"
	 *
	 * THE FACT, not the multiplier — see the Movement note above. This is what
	 * UTraceWeaponComponent pushes into UTraceCharacterMovementComponent::
	 * SetKnifeMovementProfileActive(), and it is exported so the movement slice, the HUD or a test
	 * can ask the same question and get the same answer.
	 *
	 * Knife equipped AND not carrying the Core. The carrier clause is defence in depth: a swap is
	 * already refused while carrying, so a carrier only ever reaches this by picking the Core up
	 * with the knife already out — and at that moment the knife is STOWED, not active. They cannot
	 * swing with it and cannot shoot at all, so "the knife is the active weapon" is false by the
	 * plain reading of the movement component's own contract. It also stops 1.08 x 1.30 = 1.40x
	 * from quietly retiring the one number the carrier's speed was ever tuned with.
	 */
	TRACE_API bool ShouldUseKnifeMovementProfile(const AActor* Character);

	/** Seconds until @p Character may swing again; 0 when it may swing now. HUD. */
	TRACE_API float GetSwingCooldownRemaining(const AActor* Character);

	/** Seconds left in the 0.2 s pullout; 0 when the weapon is up. HUD. */
	TRACE_API float GetDeployRemaining(const AActor* Character);

	// ---------------------------------------------------------------------------------------------
	// ENTRY POINTS. Wire the swap bind, the bots and any debug command to these.
	// ---------------------------------------------------------------------------------------------

	/**
	 * THE SWAP. Toggles gun <-> knife on @p Pawn, both directions, one bind.
	 *
	 * Safe from any machine: on the server it swaps directly, on an owning client it predicts the
	 * swap locally and asks the server. Returns true if a swap started.
	 */
	TRACE_API bool RequestSwapWeapon(ATraceCharacter* Pawn, ETraceMeleeRefusal* OutRefusal = nullptr);

	/** As above but explicit about the destination — what the bots want, since they are not toggling. */
	TRACE_API bool RequestEquip(ATraceCharacter* Pawn, ETraceEquippedWeapon Desired,
		ETraceMeleeRefusal* OutRefusal = nullptr);

	/** THE SWING. Starts one if the rules allow; the blade resolves SwingWindupSeconds later. */
	TRACE_API bool RequestSwing(ATraceCharacter* Pawn, ETraceMeleeRefusal* OutRefusal = nullptr);

	/**
	 * "Is @p Target close enough to be worth swinging at?" — BotSwingRangeFraction of the reach,
	 * measured centre to centre and slackened by both capsule radii. Bots and debug only; the
	 * authority on whether a swing connects is ResolveSwing and nothing else.
	 */
	TRACE_API bool IsInSwingRange(const ATraceCharacter* Attacker, const AActor* Target);
}

/**
 * Deterministic self-test of the back/front angle model, run by `Trace.Knife.AngleTest`.
 *
 * Exposed rather than file-static so it can also be driven from the input harness or an automated
 * run. Logs one line per case at Display plus a summary, and returns the failure count — so 0 means
 * the model matches the contract in this header. Needs no world and no game running.
 */
TRACE_API int32 TraceRunMeleeSelfTest();
