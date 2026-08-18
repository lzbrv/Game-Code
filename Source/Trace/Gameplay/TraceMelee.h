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
// AMENDED BY SPEC v12, and the amendments are quoted where they land:
//   §1  the back zone narrows from a 180-degree rear hemisphere to a 60-degree cone (see BACK OR
//       FRONT below, and BackstabHalfAngleDegrees, which is the HALF-angle and is therefore 30).
//   §2  the first-person motion becomes a STAB, not a swipe, and the 3D slash line no longer draws
//       in the swinger's own view. Both live in UTraceWeaponComponent; both are PROCEDURAL, because
//       the imported Mannequin set has no melee sequence of any kind.
//   §3  the movement bonus drops from +30% to +22%, with the air-strafe bonuses scaled by 22/30 to
//       match. See the Movement note in UTraceMeleeSettings.
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
//      are full, and a carrier who could hold a knife would collect the +22% movement bonus on top
//      of UTraceSettings::CarrierSpeedMultiplier — 1.30 x 1.22 = 1.59x — which would make the Core
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
//     the knife usable while sprinting past somebody at 976 uu/s.
//
//   THE LETHAL ARC IS STILL 140 DEGREES AFTER SPEC v12 §2 MADE THE ANIMATION A STAB, and that is a
//   decision. §2 asked for the ANIMATION to change ("Can you make the knife animation a stab instead
//   of a swipe?"); it did not ask for the weapon's reach or hit envelope to change, and narrowing the
//   sampled arc to match a thrust would be a silent nerf nobody requested. The viewmodel thrusts; the
//   blade still cuts the same volume it cut yesterday. If the user wants the envelope tightened to
//   match the new motion, SwingArcDegrees is the one number, and SwingSamples must come down with it
//   (Trace.Knife.AngleTest checks the chord arithmetic).
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
//     back-stab  <=>  angle(A, F) <= BackstabHalfAngleDegrees        (default 30 degrees)
//
// SPEC v12 §1 NARROWED THIS FROM 90 TO 30. Verbatim: "Change the 'back' zone for knife damage being
// 100 from a boundary at 90 degrees to 60 degrees, with the center of the 60 angle being at the
// center of the model." BackstabHalfAngleDegrees is a HALF-angle about the rear axis, so the cone
// the user described — 60 degrees wide, centred on directly-behind, i.e. ±30 — is this number at 30.
// It used to be 90, which made the back zone the entire rear HEMISPHERE: standing at a victim's
// shoulder scored 100. Now it is a genuine flank. Everything outside the cone, the whole 300 degrees
// of it, is the 30-damage front hit; there is still no third "side" tier, because the user has never
// asked for one.
//
// The threshold stays a tunable (see UTraceMeleeSettings, the .ini, and Trace.Knife.BackstabAngle),
// and it is pinned by MEASUREMENT rather than by inspection: TraceRunMeleeSelfTest sweeps 24 azimuths
// around the victim and then binary-searches the flip to 0.05 degrees, so the boundary the running
// game actually has is compared against the boundary the settings claim. Reading the constant is not
// evidence — the .ini wins over the initialiser below, and has silently disagreed with it before.
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
 *
 * *** SPEC v28 §9 APPENDS Smg AND DOES NOT RENUMBER. *** This enumerator travels on the wire
 * (UTraceWeaponComponent::EquippedWeapon is replicated) and is what
 * ServerRequestEquip_Validate range-checks, so inserting anything above Knife would make two
 * builds disagree about what a byte on the wire means. Appended, never inserted — the same rule
 * ETraceInputAction states at length for its own table.
 *
 * "Gun" IS THE PISTOL, and it keeps the name rather than being renamed to Pistol. Every existing
 * caller that asks for ETraceEquippedWeapon::Gun means "the ordinary firearm", and a rename would
 * be a repo-wide edit across four ownership slices for no behavioural gain. Use
 * UTraceWeaponComponent::IsFirearmEquipped() when the question is "any gun at all", which is what
 * almost every gate actually wants.
 */
UENUM()
enum class ETraceEquippedWeapon : uint8
{
	/** The pistol. 100 / 40 / 25, 190 RPM, 30-round clip, 0.5 s reload. */
	Gun   = 0,

	/** The knife. Under dual-wield (spec v28 §10) NO pawn is ever in this state — see UTraceMeleeSettings::bDualWieldKnife. */
	Knife = 1,

	/** SPEC v28 §9. Full-auto SMG: 33 / 18 / 12, 600 RPM, 40-round clip, 0.8 s reload. */
	Smg   = 2
};

/** "GUN" / "KNIFE" / "SMG". Logs and HUD only. */
TRACE_API const TCHAR* LexToString(ETraceEquippedWeapon Weapon);

/** True for a weapon that fires bullets, i.e. everything except the knife. One definition. */
inline bool TraceIsFirearm(ETraceEquippedWeapon Weapon)
{
	return Weapon == ETraceEquippedWeapon::Gun || Weapon == ETraceEquippedWeapon::Smg;
}

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
	 * HALF-angle, in degrees, of the rear cone that counts as a back-stab — measured between the
	 * victim's forward vector and the direction from the attacker to the victim.
	 *
	 * 30, i.e. a 60-DEGREE CONE centred on directly-behind (spec v12 §1: "from a boundary at 90
	 * degrees to 60 degrees, with the center of the 60 angle being at the center of the model").
	 * The full width of the back zone is therefore 2x this number, which is the number the user
	 * quoted — if you are reconciling this against the spec, remember the factor of two.
	 *
	 * It was 90, which made the back zone the whole rear hemisphere: a hit from directly beside a
	 * player scored 100. At 30 a back-stab is a genuine flank. Everything outside the cone is
	 * FrontDamage; the difference never goes to a third number, because there is no side tier.
	 *
	 * DO NOT VERIFY THIS BY READING IT. The .ini wins, and the azimuth sweep in TraceRunMeleeSelfTest
	 * (Trace.Knife.AngleTest) measures where the boundary really is on the running build to 0.05 deg.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Damage", meta = (ClampMin = "5.0", ClampMax = "179.0"))
	float BackstabHalfAngleDegrees = 30.f;

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
	// "Players should move 30% faster with a knife, as well as have a higher momentum ceiling" —
	// RETUNED BY SPEC v12 §3 to +22% ("Reduce max speed with the knife from the previous 30% increase
	// to 22% and adjust momentum accordingly") — is implemented in UTraceCharacterMovementComponent,
	// whose knobs live on UTraceSettings:
	//
	//     KnifeMoveSpeedMultiplier            1.22     ground speed x   (was 1.30)
	//     KnifeAirStrafeSoftCapMultiplier     1.18333  air-strafe soft cap x   (was 1.25)
	//     KnifeAirStrafeHardCapMultiplier     1.25667  air-strafe hard cap AND MaxAirSpeed x (was 1.35)
	//
	// THE TWO AIR MULTIPLIERS ARE THE "adjust momentum accordingly" HALF, and the arithmetic is one
	// rule applied three times: every knife BONUS is scaled by 22/30, so the mobility package stays
	// proportionate instead of the ground speed dropping while the ceilings keep their +30% values.
	//     ground  0.30 x 22/30 = 0.220   -> 1.220
	//     soft    0.25 x 22/30 = 0.18333 -> 1.18333   (1045 -> 1237, was 1306)
	//     hard    0.35 x 22/30 = 0.25667 -> 1.25667   (1375 -> 1728, was 1856)
	// The base caps there already include UTraceSettings::AirStrafeAsymptoteScale (x1.10); see
	// Trace.Knife.DumpSettings, which prints the scaled bases for exactly this reason.
	//
	// *** FLAGGED, NOT FIXED: THIS BREAKS THE CARRIER/KNIFE PARITY THE USER ASKED FOR. ***
	// CarrierSpeedMultiplier is 1.30 because the user previously asked to "increase core carrier speed
	// to match knife speed". Dropping the knife to 1.22 leaves the CARRIER FASTER THAN THE KNIFE
	// (1.30 vs 1.22). Spec v12 §3 asks only for the knife change, so only the knife changed. It is one
	// number either way and it is the user's call — do not silently drag CarrierSpeedMultiplier down.
	//
	// That is the right home for them and this file deliberately does not keep a second copy. The
	// bit is saved-move state round-tripped through FSavedMove_Trace, so a server correction replays
	// each move under the profile that move actually ran with — something a multiplier read from
	// here, outside the move pipeline, could not have given. A duplicate 1.22 in this table would be
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
	 * The +22% makes the knife the correct chase tool, so the bot's rule is a range band rather than
	 * a state: inside this it is worth trading the gun for the speed, outside BotDisengageRangeUU it
	 * is not. The gap between the two is hysteresis — without it a bot at exactly the boundary
	 * swap-thrashes and spends its whole life in a 0.2 s pullout.
	 *
	 * 500, NOT 700, AND THE DIFFERENCE IS A BOT-DPS REGRESSION. A bot holding the knife cannot
	 * shoot, so this range is exactly how far a bot voluntarily disarms itself for. The blade
	 * reaches 180 uu; at the knife's 976 uu/s, 500 uu is 0.51 s of closing, which reads as a
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

	// =============================================================================================
	// ***                      T H E   O N E   S W I T C H   (spec v28 §10)                      ***
	// =============================================================================================
	//
	// Spec v28 §10, verbatim: "Knives are being changed so that you can dual wield them with a gun.
	// Gun in one hand, knife in the other. [...] But you no longer have to swap between knife and
	// gun, just the different guns. Make these knife changes easy to revert with one prompt."
	//
	// *** THIS BOOL IS THAT ONE PROMPT. *** Set it false — here, in Config/DefaultGame.ini, with
	// `Trace.Knife.DualWield 0`, or by launching with `-TraceLegacyKnife` — and the build is byte for
	// byte the knife-as-a-separate-weapon game that shipped in v27. Nothing else has to be touched
	// and nothing has to be found: every site that behaves differently is marked with the literal
	// tag  [DUALWIELD]  in a comment, so
	//
	//     grep -rln '\[DUALWIELD\]' Source/Trace Config
	//
	// is the complete inventory of the change. It lands in SIX source files and one .ini, and all
	// seven are inside this pass's own ownership slice — TraceMelee.{h,cpp},
	// TraceWeaponComponent.{h,cpp}, TraceCharacter.{h,cpp} (viewmodel only) and
	// Config/DefaultGame.ini. Nothing outside that set changes behaviour on this flag, which is what
	// makes the revert a read of one list rather than an archaeology dig.
	//
	// ---------------------------------------------------------------------------------------------
	// EXACTLY WHAT FLIPPING IT DOES. TRUE (shipped) on the left, FALSE (the v27 revert) on the right.
	// ---------------------------------------------------------------------------------------------
	//
	//   THE SELECTOR
	//     true   ETraceEquippedWeapon::Knife is never entered by any path. RequestEquip(Knife)
	//            succeeds as a no-op (the blade is already in hand) and costs no pullout. The
	//            selector only ever holds Gun (pistol) or Smg.
	//     false  the selector holds Gun or Knife exactly as it did in v27, with the 0.2 s pullout
	//            between them. (Smg is still reachable — §9 is a separate feature and does NOT
	//            depend on this switch.)
	//
	//   THE 1 AND 2 KEYS  (TraceMelee::RequestEquipIfDifferent)
	//     true   1 = pistol, 2 = SMG. Weapon switching is between GUNS only.
	//     false  1 = knife, 2 = gun, exactly as spec v13 §2 specified.
	//
	//   MELEE
	//     true   TraceMelee::HandleMeleeInput is the melee verb, on its own bind (right mouse by
	//            default — see the hand-off note on that function). Mouse 1 always SHOOTS.
	//            CanSwing() no longer requires the knife to be the selected weapon.
	//     false  mouse 1 is "attack" and dispatches to a swing when the knife is selected, which is
	//            the v10 §1 design. HandleMeleeInput refuses with WrongWeapon unless the knife is
	//            actually equipped, so a stray right-mouse bind cannot melee with the gun out.
	//
	//   SHOOTING LOCKOUT
	//     true   CanFire() is false for SwingAnimSeconds after a swing starts (spec v28 §10:
	//            "Meleeing should lock the player out of shooting for the length of the animation").
	//     false  no lockout exists, because with the knife out you could not shoot anyway.
	//
	//   MOVEMENT
	//     true   TraceMelee::ShouldUseKnifeMovementProfile() is ALWAYS false. This is the single most
	//            important consequence of the switch and it is deliberate: the +22% ground speed and
	//            the two air-cap multipliers (spec v12 §3) were the PRICE of holding a weapon that
	//            cannot shoot. A knife that is always in your off hand would hand every player that
	//            bonus permanently and for free, which is a movement rebalance nobody asked for.
	//     false  the v12 §3 profile applies whenever the knife is the selected weapon.
	//
	//   PRESENTATION
	//     true   the knife rig is drawn in the OFF hand alongside the gun, always, and the gun is
	//            never hidden. ATraceCharacter's left hand comes off the foregrip.
	//     false  the knife replaces the gun on screen, one weapon at a time (v12 §7's rule).
	//
	//   BOTS
	//     true   bots never swap weapons for melee; they keep the gun and swing when a target walks
	//            into blade range.
	//     false  the v10 §1 range band: swap to the knife inside BotEngageRangeUU, back out at
	//            BotDisengageRangeUU.
	//
	// ---------------------------------------------------------------------------------------------
	// WHAT IT DOES NOT TOUCH, so a revert is not mistaken for a wider undo: §9's SMG, the damage
	// model, the back/front cone, the swing cooldown, the wind-up, the reach, carrier immunity, and
	// every number in this class above this line. Reverting the knife does not revert the SMG.
	// ---------------------------------------------------------------------------------------------
	//
	// THE .INI WINS over this initialiser, as it does for every property in this class — see the
	// class comment. Read the live value back with `Trace.Knife.DumpSettings`, never from this line.
	UPROPERTY(config, EditAnywhere, Category = "Dual wield", meta = (DisplayName = "Dual-wield the knife with a gun [v28 §10 — THE REVERT SWITCH]"))
	bool bDualWieldKnife = true;
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
	//     Trace.Knife.BackstabAngle     -1     HALF-angle in degrees; the cone the user quotes is 2x it
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
	 * *** THE SPEC v28 §10 REVERT SWITCH, RESOLVED. The only reader of bDualWieldKnife. ***
	 *
	 * Three inputs, in this order of precedence, so a headless run and a designer can both reach it:
	 *
	 *   1. `-TraceLegacyKnife` on the command line forces FALSE (the v27 behaviour). Sampled once.
	 *   2. `Trace.Knife.DualWield` — 0 or 1 forces that answer; the shipped -1 defers.
	 *   3. UTraceMeleeSettings::bDualWieldKnife, which Config/DefaultGame.ini sets.
	 *
	 * Read UTraceMeleeSettings::bDualWieldKnife's comment for the complete list of what flipping it
	 * changes. Every call site of this function carries the tag [DUALWIELD] so the whole change is
	 * one grep.
	 */
	TRACE_API bool IsDualWieldEnabled();

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

	/**
	 * True when the knife is the SELECTED weapon, i.e. the gun is stowed and cannot fire.
	 *
	 * *** UNDER DUAL-WIELD THIS IS ALWAYS FALSE, AND THAT IS THE CORRECT ANSWER. *** Every caller of
	 * this predicate outside the melee slice uses it to mean "this pawn cannot shoot right now" —
	 * X's Sting, Roxie's Modded, the ability component's reload hook, ShouldShowAmmo, the HUD's
	 * weapon row. With the knife in the off hand a pawn CAN always shoot, so answering true would
	 * switch every one of those off for the whole match.
	 *
	 * Use IsKnifeInHand() below when the question is "is there a blade available to swing".
	 */
	TRACE_API bool IsKnifeEquipped(const AActor* Character);

	/**
	 * "Is there a blade in this pawn's hands at all?" — the question the SWING gate asks.
	 *
	 * [DUALWIELD] true for any living pawn when the switch is on (the knife is always held), and
	 * identical to IsKnifeEquipped() when it is off. This is the split that keeps the revert honest:
	 * one predicate means "the gun is stowed" and the other means "the knife is available", and
	 * before v28 they were the same sentence so one name did for both.
	 */
	TRACE_API bool IsKnifeInHand(const AActor* Character);

	/**
	 * Seconds of shooting lockout still owed by a swing. 0 when the gun is free.
	 *
	 * Spec v28 §10: "Meleeing should lock the player out of shooting for the length of the
	 * animation." The length IS UTraceMeleeSettings::SwingAnimSeconds — it is not a second number
	 * that happens to equal it, so retuning the animation moves the lockout with it.
	 */
	TRACE_API float GetShootLockoutRemaining(const AActor* Character);

	// ---------------------------------------------------------------------------------------------
	// THE MELEE BIND  (spec v28 §10)
	// ---------------------------------------------------------------------------------------------

	/** What one press of the melee bind actually did. Returned so the caller can log it honestly. */
	enum class EMeleeInputResult : uint8
	{
		/** A swing started. */
		Swing,

		/**
		 * The press went to the CORE PULL instead, because the player was being shown the pull
		 * circle at that instant. Spec v28 §10, verbatim: "If and only if a player is looking at a
		 * pullable core, and being shown the circle icon to pull, this keybind should override the
		 * melee keybind."
		 */
		CorePull,

		/** Neither. @p OutRefusal says which melee rule turned it down. */
		Refused
	};

	/**
	 * *** THE WHOLE RIGHT-MOUSE VERB, IN ONE CALL. Bind the melee key to this and nothing else. ***
	 *
	 * @param Pawn      the pressing player's pawn.
	 * @param bPressed  true on the press edge, false on the release edge. BOTH MUST BE DELIVERED —
	 *                  the Core pull is a HOLD, so a press that never gets its release leaves a ring
	 *                  filling on the server behind a pause menu. The release is always forwarded to
	 *                  the Core (a release on a latch that was never set is a documented no-op), so
	 *                  the caller does not have to remember which verb the press went to.
	 *
	 * THE PRECEDENCE IS A STATE TEST, NOT A BLANKET RULE, and that is the exact wording of the
	 * request. The pull wins if and only if ATraceCore::CanPullNow() is true for this pawn on this
	 * frame — which is the SAME predicate ATraceHUD uses to decide whether to draw the circle
	 * (TraceHUD.cpp, "const bool bEligible = Core->CanPullNow(LocalChar, &Reason)"). Asking the same
	 * function is what makes "being shown the circle icon" and "the button pulls" one fact instead of
	 * two that can drift. In every other state — no turnover, wrong team, out of range, no line of
	 * sight, looking away, already carrying — the circle is not on screen and the press melees.
	 *
	 * Safe on null, safe on any machine, and it dispatches the pull itself
	 * (ATraceCore::RequestPullInput, which handles the client relay), so there is nothing left for
	 * the caller to do.
	 */
	TRACE_API EMeleeInputResult HandleMeleeInput(ATraceCharacter* Pawn, bool bPressed,
		ETraceMeleeRefusal* OutRefusal = nullptr);

	/**
	 * The precedence test on its own, exported so the keybind slice and the HUD can ask it without
	 * pressing anything — e.g. to label the right-mouse prompt "PULL" rather than "MELEE".
	 */
	TRACE_API bool ShouldCorePullOverrideMelee(const ATraceCharacter* Pawn);

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
	 * plain reading of the movement component's own contract. It also stops 1.30 x 1.22 = 1.59x
	 * from quietly retiring the one number the carrier's speed was ever tuned with.
	 */
	TRACE_API bool ShouldUseKnifeMovementProfile(const AActor* Character);

	/** Seconds until @p Character may swing again; 0 when it may swing now. HUD. */
	TRACE_API float GetSwingCooldownRemaining(const AActor* Character);

	/** Seconds left in the 0.2 s pullout; 0 when the weapon is up. HUD. */
	TRACE_API float GetDeployRemaining(const AActor* Character);

	// ---------------------------------------------------------------------------------------------
	// ENTRY POINTS. Wire the weapon binds, the bots and any debug command to these.
	//
	// SPEC v15 §5 — "Switch weapon keybind so that it's only switch to knife/switch to gun." THE
	// PLAYER-FACING ENTRY POINT IS NOW RequestEquipIfDifferent AND NOTHING ELSE: no key is bound to
	// the toggle any more, and ETraceInputAction has no SwapWeapon enumerator to bind. The two
	// unguarded verbs below stay because the bots, the dev console and the v13 §2 harness's red arm
	// all still need "the other one" / "this one, cost included" — they are simply no longer
	// reachable from a keyboard.
	// ---------------------------------------------------------------------------------------------

	/**
	 * THE SWAP VERB. Toggles gun <-> knife on @p Pawn, both directions.
	 *
	 * NO LONGER BOUND TO A KEY (spec v15 §5). Its callers are Trace.Knife.Swap / Trace.Knife.SwapIn
	 * and anything else that genuinely means "give me the other one" without naming it.
	 *
	 * Safe from any machine: on the server it swaps directly, on an owning client it predicts the
	 * swap locally and asks the server. Returns true if a swap started.
	 */
	TRACE_API bool RequestSwapWeapon(ATraceCharacter* Pawn, ETraceMeleeRefusal* OutRefusal = nullptr);

	/**
	 * As above but explicit about the destination — what the bots want, since they are not toggling.
	 *
	 * UNGUARDED: asking for the weapon already in hand re-anchors the 0.2 s pullout, by design. That
	 * is the whole difference from RequestEquipIfDifferent below, and it is what the v13 §2 harness's
	 * red arm substitutes to prove its assertions can fail.
	 */
	TRACE_API bool RequestEquip(ATraceCharacter* Pawn, ETraceEquippedWeapon Desired,
		ETraceMeleeRefusal* OutRefusal = nullptr);

	/**
	 * DIRECT SELECT (spec v13 §2) — the 1 and 2 binds, and as of spec v15 §5 the only weapon input
	 * there is. Asking for the weapon already in hand does nothing at all and, crucially, does NOT
	 * restart the 0.2 s pullout.
	 *
	 * Use this for a key that names a weapon; use RequestEquip above for a bot deciding what it
	 * needs. The difference is documented in full on UTraceWeaponComponent::RequestEquipIfDifferent,
	 * which is where the gate lives.
	 *
	 * *** [DUALWIELD] THIS IS WHERE THE 1 AND 2 KEYS ARE REMAPPED, AND IT IS THE ONLY PLACE. ***
	 * Spec v28 §10: "you no longer have to swap between knife and gun, just the different guns." The
	 * two direct-select binds still exist and still name a slot; with the switch on, the SLOT they
	 * name changes:
	 *
	 *     key      action id           v27 meaning     v28 dual-wield meaning
	 *     1        EquipKnife          the knife       slot 1 = the PISTOL
	 *     2        EquipGun            the gun         slot 2 = the SMG
	 *
	 * The remap lives on THIS function — the verb documented as "for a key that names a weapon" —
	 * and deliberately not on RequestEquip, which is the bots' and the console's verb and must keep
	 * meaning exactly what it says. It is one switch statement; see the definition.
	 *
	 * HAND-OFF, STATED PLAINLY: the two rows on the keybind page still READ "SWITCH TO KNIFE" and
	 * "SWITCH TO GUN". Those display strings live in FTraceInputActionInfo (Settings/
	 * TraceUserSettings.cpp), which is spec v28 §3's ownership slice, not this one. The binds work;
	 * the labels are stale. Renaming them to "WEAPON 1 (PISTOL)" / "WEAPON 2 (SMG)" is a two-string
	 * edit there and must NOT change either ConfigId, or every returning player loses the bind.
	 */
	TRACE_API bool RequestEquipIfDifferent(ATraceCharacter* Pawn, ETraceEquippedWeapon Desired,
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
