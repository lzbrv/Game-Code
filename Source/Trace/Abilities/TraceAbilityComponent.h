// Trace — the ability component (spec v14 §5), and the carrier choke point (spec v14 §4).
//
// ===================================================================================================
// THE INTERFACE SPEC §5 DECLARES, VERBATIM, AND THIS CLASS IMPLEMENTS EXACTLY
// ===================================================================================================
//
//     UENUM() enum class ETraceCharacterId : uint8 { None=0, Rocco, Chut, Mace, Oyster, X };
//
//     UCLASS() class UTraceAbilityComponent : public UActorComponent
//       // Replicated: CharacterId, ActivatedCooldownEndMatchTime, per-character transient state.
//       ETraceCharacterId GetCharacterId() const;
//       void   ServerSetCharacter(ETraceCharacterId);        // authority, respects per-team uniqueness
//       bool   TryActivate();                                 // the E ability; predicted locally
//       float  GetActivatedCooldownRemaining() const;         // HUD
//       void   OnHalfTime();                                  // resets every cooldown
//       bool   CanAffectTarget(const ATraceCharacter* Target) const;  // THE choke point from §4
//
// Those six signatures are load-bearing: five character agents are coding against them. Nothing here
// renames, re-orders or adds a required argument to any of them.
//
// ===================================================================================================
// WHY THIS COMPONENT LIVES ON THE PLAYERSTATE AND NOT ON THE PAWN
// ===================================================================================================
//
// Spec §5, verbatim: "Character ability cooldowns should continue to countdown while a player is
// dead. They should not automatically reset due to death or a goal (a player can spawn with an
// ability timer still counting down)."
//
// ATraceGameMode::RestartPlayerFresh DESTROYS the pawn and spawns a new one on every respawn
// (TraceGameMode.cpp — `CurrentPawn->Destroy()` before RestartPlayer). A component on the pawn is
// therefore destroyed and reconstructed on every death, and any cooldown it held would reset — which
// is precisely the behaviour the spec forbids. The PlayerState survives death, survives the goal
// reset, exists for bots, and is replicated to everybody, so it is the only correct home.
//
// The pawn-side hooks reach the component with UTraceAbilityComponent::Get(SomeCharacter), which
// resolves through the pawn's PlayerState. That is one indirection and it is always valid.
//
// ===================================================================================================
// THE COOLDOWN CONTRACT — READ THIS BEFORE TOUCHING ANY TIMER
// ===================================================================================================
//
// ActivatedCooldownEndMatchTime is an ABSOLUTE server timestamp on the match clock
// (AGameStateBase::GetServerWorldTimeSeconds), never a countdown and never a per-life timer.
// That single choice is what makes every clause of the spec true for free:
//
//   "continue to countdown while a player is dead"   the clock does not care about the pawn.
//   "not automatically reset due to death"           nothing in the death path writes it, and the
//                                                    component the value lives on is not destroyed.
//   "not automatically reset due to a goal"          nothing in the scoring path writes it.
//   "a player can spawn with an ability timer still
//    counting down"                                  the new pawn asks the surviving component.
//   "They should all reset at halftime."             OnHalfTime() zeroes it. That is the ONLY
//                                                    automatic reset in the whole framework.
//
// If you are ever tempted to add a second place that writes this value: don't. Add a reason to
// OnHalfTime() instead.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "Abilities/TraceAbilityTypes.h"
#include "TraceTypes.h"                 // ETraceTeam

#include "TraceAbilityComponent.generated.h"

class APlayerState;
class ATraceCharacter;
class UTraceCharacterAbilitySet;

/**
 * THE ALARM, and the twin of TraceMelee::GetCarrierKnifeHitCount.
 *
 * Counts the times ability damage actually reached a Core carrier's health component. On a correct
 * build this is zero for the entire life of the process; every increment also logs an Error naming
 * the ability. Trace.Ability.CarrierChokeTest reads it, and reading it is what makes the harness
 * evidence rather than an assertion about code somebody has already read.
 */
namespace TraceAbility
{
	TRACE_API int32 GetCarrierAbilityDamageHitCount();
	TRACE_API void  ResetCarrierAbilityDamageHitCount();
}

/**
 * ===================================================================================================
 * THE INTEGRATION SEAM — the cross-file calls that turn written abilities into played ones.
 * ===================================================================================================
 *
 * Five character agents shipped abilities whose passive halves were measured through the exact
 * functions they were meant to be called from, and then NOTHING CALLED THEM: nothing multiplied max
 * speed by GetMoveSpeedMultiplierFor, nothing multiplied the magnet by GetMagnetRadiusMultiplierFor,
 * nothing called NotifyKill, NotifyDashStarted or NotifyPawnSpawned, and no key reached
 * HandleActivatePressed. Each of those is now one call in somebody else's file:
 *
 *   UTraceCharacterMovementComponent::GetMaxSpeed    Rocco's stack, X's +10%, Oyster's poison slow
 *   UTraceCharacterMovementComponent::BeginDash /    Oyster's jar-per-dash, Chut's bash window
 *     the dash-exit branch of OnMovementUpdated
 *   ATraceCore::ServerApplyCatchZone                 Mace's +30% magnet
 *   ATraceGameMode::NotifyCharacterDied              Rocco's headshot stack, Chud's knife refresh
 *   ATraceGameMode::RestartPlayerFresh               OnPawnSpawned
 *   ATracePlayerController::OnJumpStarted /          Rocco's second jump, Oyster's jar jump,
 *     OnAbilityStarted / OnAbilitySecondary*         every activated ability, Mace's V
 *
 * THIS NAMESPACE EXISTS SO THAT SET OF CALLS CAN BE SHOWN FAILING. Every one of them is guarded by
 * IsEnabled(), and Trace.Ability.Integration 0 removes all of them at once — which is precisely the
 * build the project actually had before this pass. Trace.Integration.Verify runs that arm first and
 * reports INVALID if it does not reproduce, because a harness that cannot go red is not a harness.
 *
 * The counters are the other half: a passive can be correct, be called, and still be invisible in
 * any measurement of the outcome. Counting the calls at the call site distinguishes "the hook did
 * not fire" from "the hook fired and the ability declined", which are the two failures this class of
 * bug alternates between.
 */
namespace TraceAbilityIntegration
{
	/** False only under the Trace.Ability.Integration 0 red arm. Never ship 0. */
	TRACE_API bool IsEnabled();

	/** What the wired call sites have observed. Dev builds only; all zero at process start. */
	struct FCounters
	{
		int32 DashStarted = 0;
		int32 DashEnded = 0;
		/** Per-frame dash contacts delivered by the movement component's sweep (Chut's bash). */
		int32 DashHits = 0;
		int32 Kills = 0;
		int32 PawnSpawned = 0;
		int32 PawnDied = 0;
		int32 JumpConsumed = 0;
		int32 ActivatePressed = 0;
		int32 SecondaryEdges = 0;
		/** Frames on which a candidate's magnet radius was widened by a character passive. */
		int32 MagnetWidenedFrames = 0;
		/** Times GetMaxSpeed() was actually moved by an ability multiplier (buff or debuff). */
		int32 SpeedMultiplierApplied = 0;
	};

	TRACE_API FCounters& Counters();
	TRACE_API void ResetCounters();
}

/**
 * One player's character choice, cooldown and abilities. One per PlayerState, humans and bots alike —
 * and since spec v15 §2 that is literal rather than nominal: a bot holds a real character with a real
 * ability set. What differs is only HOW it gets one. A human is offered a select screen and asks; a
 * bot is assigned one by ATraceGameMode::PollCharacterSelect, and only once every human on its team
 * has settled. See ServerSetCharacter and AreHumansOnTeamSettled.
 */
UCLASS(ClassGroup = (Trace), meta = (BlueprintSpawnableComponent))
class TRACE_API UTraceAbilityComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTraceAbilityComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// =============================================================================================
	// THE SPEC §5 INTERFACE
	// =============================================================================================

	/** The character this player is. None = the default characterless Mannequin, and that is valid. */
	ETraceCharacterId GetCharacterId() const { return CharacterId; }

	/**
	 * AUTHORITY ONLY. Sets the player's character, respecting per-team uniqueness (spec §3: "Do not
	 * allow players to select a character who has already been chosen by a player on their team").
	 *
	 * Refuses, and logs, when:
	 *   - called without authority;
	 *   - characters are disabled (UTraceSettings::bCharactersEnabled false, or mode A — §2 freezes
	 *     mode A with no characters at all);
	 *   - this player is a BOT and a human on its team has not settled yet. Spec v15 §2's ORDERING
	 *     rule: "the computers should wait for any actual humans on its team to choose before all
	 *     loading in with randomly chosen characters". See AreHumansOnTeamSettled;
	 *   - a LIVING teammate already holds @p NewCharacter. First request wins; the loser keeps what
	 *     they had and IsCharacterAvailableFor() tells the select screen to re-pick. Bots are subject
	 *     to this exactly as humans are (spec v15 §2, "no two should be able to pick the same
	 *     characters") — there is deliberately no bot branch in the uniqueness test.
	 *
	 * IT NO LONGER REFUSES BOTS OUTRIGHT. Spec v14 §3 said "bots remain characterless, for now" and
	 * this function enforced it; spec v15 §2 reverses that and bots now pick. The refusal above is an
	 * ORDERING rule, not a ban.
	 *
	 * Setting ETraceCharacterId::None is ALWAYS allowed and never refused — it is the way back to the
	 * default Mannequin and the select screen's timeout needs it to be infallible.
	 *
	 * DOES NOT TOUCH THE COOLDOWN. Changing character mid-match does not hand you a free E; only
	 * half time clears cooldowns. The per-character transient state IS cleared, because it belongs
	 * to the character that just left.
	 */
	void ServerSetCharacter(ETraceCharacterId NewCharacter);

	/**
	 * Press E. Returns true if the ability fired.
	 *
	 * Called on the OWNING CLIENT (where it predicts locally and sends the RPC) and on the SERVER
	 * (where it validates and is authoritative). On a listen server both halves are the same call.
	 *
	 * Every refusal is centralised here so no character has to remember them: dead, no pawn, no
	 * character, characters disabled, match not live, half-time break, cooldown still running, and
	 * finally the character's own CanActivate(). Only then is ActivateAbility() called, and only a
	 * true return starts the cooldown.
	 */
	bool TryActivate();

	/** Seconds until E is ready, 0 when it is. Correct on clients — the deadline is replicated. */
	float GetActivatedCooldownRemaining() const;

	/**
	 * HALF TIME. Spec §5: "They should all reset at halftime." Zeroes the activated cooldown, wipes
	 * the per-character transient state and tells the character set to tear down its world actors.
	 *
	 * AUTHORITY ONLY. Called for every player by UTraceAbilityWorldSubsystem when it sees the
	 * GameState enter the half-time break; safe to call directly from the game mode instead.
	 */
	void OnHalfTime();

	/**
	 * *** THE CHOKE POINT. SPEC §4. THE MOST IMPORTANT FUNCTION IN THE ABILITY FRAMEWORK. ***
	 *
	 * Verbatim: "NO abilities damage carriers, carriers can still only be killed when an enemy
	 * dashes through their trace."
	 *
	 * Every ability's damage, and every ability's slow / pull / knockback, passes through here. It
	 * is ONE function rather than fifteen call sites each remembering, because spec §4 says the
	 * invariant now has fifteen new ways to break and names Oyster's poison, Pickler's area damage,
	 * Chut's bash and X's vulnerable as the specific risks.
	 *
	 * The single-argument form asks about DAMAGE, which is the question §4 actually poses, so a call
	 * site that forgets to think about the effect class gets the strictest answer.
	 *
	 * @return false for a Core carrier under Damage — unconditionally, with no knob and no exception.
	 *         false for a Core carrier under Control unless a designer reverses the §4 [ASSUMPTION]
	 *         via UTraceSettings::bCarrierImmuneToAbilityControl.
	 *         true for Beneficial on a carrier: §6 is explicit that "the Core carrier can use"
	 *         Rocco's Ripple.
	 *         Also false for the dead, for self, and for teammates while friendly fire is off.
	 */
	bool CanAffectTarget(const ATraceCharacter* Target, ETraceAbilityEffect Effect = ETraceAbilityEffect::Damage) const;

	// =============================================================================================
	// The same question, with its answer spelled out — for logs, the HUD and the harness
	// =============================================================================================

	/** CanAffectTarget with the reason. Identical logic; CanAffectTarget is a thin wrapper on it. */
	ETraceAbilityBlockReason CanAffectTargetDetailed(const ATraceCharacter* Target, ETraceAbilityEffect Effect) const;

	/**
	 * THE STATIC FORM, for call sites that have no component to hand — a jar actor ticking poison, a
	 * projectile, a bee. Resolves the instigator's component and asks it; when the instigator has no
	 * component at all it still applies the carrier rule, so an orphaned world effect cannot become
	 * the one path that damages a carrier.
	 *
	 * @param InstigatorActor  the pawn, controller or player state that owns the effect. May be null.
	 */
	static bool CanAffect(const AActor* InstigatorActor, const ATraceCharacter* Target,
	                      ETraceAbilityEffect Effect = ETraceAbilityEffect::Damage);

	/**
	 * The carrier half of the rule, on its own, with no instigator and no team test.
	 *
	 * This is the line spec §4 is actually about, and it is deliberately callable from anywhere:
	 * `if (!UTraceAbilityComponent::MayAbilityAffectCarrier(Target, Effect)) return;`
	 * Extends the knife's proven pattern rather than duplicating it — the predicate underneath is
	 * ATraceCore::IsCoreHolder, the same one TraceMelee::ResolveSwing uses.
	 */
	static bool MayAbilityAffectCarrier(const ATraceCharacter* Target, ETraceAbilityEffect Effect);

	/** True when @p Target is currently holding the Core. Works with or without a live pawn. */
	static bool IsCarrier(const ATraceCharacter* Target);

	// =============================================================================================
	// Damage, routed through the choke point and through both characters' passives
	// =============================================================================================

	/**
	 * SERVER ONLY. The one way an ability should ever deal damage.
	 *
	 *   1. CanAffectTarget(Target, Damage) — returns 0 and does nothing if it says no.
	 *   2. the INSTIGATOR's ModifyOutgoingDamage passive.
	 *   3. the TARGET's ModifyIncomingDamage passive (Chut's Chud).
	 *   4. the TARGET's GetIncomingDamageMultiplier (X's vulnerable).
	 *   5. UTraceHealthComponent::ApplyDamage.
	 *
	 * @return the damage actually applied. 0 means the choke point refused.
	 */
	float ApplyAbilityDamage(ATraceCharacter* Target, float Amount, FName Cause,
	                         bool bMelee = false, bool bHeadshot = false) const;

	/**
	 * The full passive pipeline WITHOUT touching health, for the weapon and melee slices to fold in.
	 * Applies the target's Chud reduction and vulnerable amplification to @p Damage.
	 *
	 * Static, and safe when either side has no component: a Mannequin returns @p Damage unchanged.
	 */
	static float ModifyDamageThroughPassives(float Damage, const FTraceAbilityDamageContext& Context);

	// =============================================================================================
	// Character set access — how a character file and the HUD reach the live abilities
	// =============================================================================================

	/** The live ability set, or null at ETraceCharacterId::None. Valid on every machine. */
	UTraceCharacterAbilitySet* GetAbilitySet() const { return AbilitySet; }

	/** Typed sugar: `if (UTraceAbilitySetMace* M = Comp->GetAbilitySetAs<UTraceAbilitySetMace>())`. */
	template <typename T>
	T* GetAbilitySetAs() const { return Cast<T>(AbilitySet); }

	/** The replicated per-character scratch pad. See FTraceAbilityNetState. */
	const FTraceAbilityNetState& GetNetState() const { return AbilityState; }

	/** SERVER ONLY. Write, then MarkNetStateDirty(). Asserts nothing — returns a scratch on clients. */
	FTraceAbilityNetState& GetMutableNetState();

	/** SERVER ONLY. Runs OnRep locally so a listen server behaves exactly like a remote client. */
	void MarkNetStateDirty();

	// =============================================================================================
	// Resolution — how anything anywhere finds a player's abilities
	// =============================================================================================

	/**
	 * The component for @p Actor, which may be an ATraceCharacter, an AController or an APlayerState.
	 * Null when that player has none (no PlayerState yet, or the subsystem has not attached it).
	 * Cheap: one GetPlayerState + one FindComponentByClass.
	 */
	static UTraceAbilityComponent* Get(const AActor* Actor);

	/** Get(), but null unless the player actually has a character. Sugar for the passive hooks. */
	static UTraceCharacterAbilitySet* GetAbilitySetFor(const AActor* Actor);

	/** The PlayerState this component hangs off. */
	APlayerState* GetOwningPlayerState() const;

	/** The player's current pawn, or null while dead / between pawns. */
	ATraceCharacter* GetOwningCharacter() const;

	/** The player's team, read off ATracePlayerState. */
	ETraceTeam GetTeam() const;

	/**
	 * True when this player is a bot.
	 *
	 * NOT a synonym for "characterless" any more. Spec v14 §3 made bots permanently Mannequins and
	 * several call sites still read that way; spec v15 §2 reverses it and bots hold characters like
	 * anybody else. What this flag still decides is WHEN and HOW one is assigned — a bot has no
	 * select screen, so it is filled by ATraceGameMode::PollCharacterSelect once its human team-mates
	 * have settled, rather than by a client RPC.
	 */
	bool IsBot() const;

	// =============================================================================================
	// Selection support — the character-select screen's server-side rules (spec §3)
	// =============================================================================================

	/**
	 * AUTHORITY ONLY. Is @p Candidate free for the player owning @p ForPlayerState?
	 *
	 * Per-TEAM uniqueness: an enemy holding the character does not block you, a teammate does.
	 * None is always available. Always true when characters are disabled — nobody is holding
	 * anything.
	 */
	static bool IsCharacterAvailableFor(const APlayerState* ForPlayerState, ETraceCharacterId Candidate);

	/** AUTHORITY ONLY. The player state on @p Team holding @p Candidate, or null. */
	static APlayerState* FindTeammateHolding(const UObject* WorldContextObject, ETraceTeam Team, ETraceCharacterId Candidate);

	/** AUTHORITY ONLY. A character nobody on @p Team holds, for the select screen's timeout. */
	static ETraceCharacterId PickFreeCharacterFor(const APlayerState* ForPlayerState);

	/**
	 * AUTHORITY ONLY. Like PickFreeCharacterFor, but a UNIFORM RANDOM one of the free characters.
	 *
	 * Spec v15 §2, verbatim: bots "all loading in with randomly chosen characters". Kept separate
	 * from PickFreeCharacterFor rather than replacing it, because the two answer different questions:
	 * the auto-assign a HUMAN gets on the select timeout is deliberately DETERMINISTIC (see
	 * ATraceGameMode::FindFreeCharacterForTeam — an assignment the player did not choose should at
	 * least be reproducible in a bug report), while a bot's fill is deliberately not, so a
	 * singleplayer match does not open with the same five characters every time.
	 *
	 * None when nothing is free, which is the same "the team has more players than the roster has
	 * characters" case PickFreeCharacterFor reports.
	 */
	static ETraceCharacterId PickRandomFreeCharacterFor(const APlayerState* ForPlayerState);

	/**
	 * AUTHORITY ONLY. Spec v15 §2's ORDERING RULE, as a predicate: has every HUMAN on @p Team
	 * finished choosing?
	 *
	 * Verbatim: "the computers should wait for any actual humans on its team to choose before all
	 * loading in with randomly chosen characters". A human is SETTLED when they hold a character —
	 * whether they picked it or the select timeout assigned it — and a human who cannot be served at
	 * all (their team already holds every character in the roster) is settled too, because otherwise
	 * one unserviceable player would keep every bot on their team a Mannequin forever.
	 *
	 * Note what is NOT consulted: whether the select screen is currently OPEN. That would let a bot
	 * jump the queue in the quarter second between a human joining and PollCharacterSelect opening
	 * their screen — "no character yet" is the fact that matters, and it is true across that gap.
	 *
	 * True for a team with no humans on it, which is the ordinary singleplayer case for the enemy
	 * side: there is nobody to wait for.
	 */
	static bool AreHumansOnTeamSettled(const UObject* WorldContextObject, ETraceTeam Team);

#if !UE_BUILD_SHIPPING
	/**
	 * THE FRAMEWORK'S HALF OF THE SPEC v15 §2 RED ARM. Dev only, cheat-gated, absent from shipping.
	 *
	 * False removes the two roster rules ServerSetCharacter enforces — per-team uniqueness and the
	 * bot ordering gate above — so ATraceGameMode's bot harness can be shown to FAIL.
	 *
	 * IT EXISTS BECAUSE THE RULE IS ENFORCED TWICE. The v14 red arm learned this the expensive way
	 * (see GTraceEnforceSelectRules in TraceGameMode.cpp): an arm that switched off only the game
	 * mode's copy came back 19/19 green, because this function was still refusing underneath it. A
	 * red arm that reports green is manufactured evidence, so the bot arm reaches both.
	 *
	 * Deliberately NOT touched by Trace.Characters.VerifyRed, whose documented result — "the loser
	 * does not end up holding the contested character stays GREEN, because the rule is enforced
	 * twice" — is still exactly true and is still worth knowing.
	 */
	static bool IsRosterEnforcementOn();
	static void SetRosterEnforcementOn(bool bEnforced);
#endif

	/**
	 * The master switch, answered from the WORLD so it is correct on clients too.
	 *
	 * False when UTraceSettings::bCharactersEnabled is off (spec §3: "Include a toggle in game
	 * settings to turn off all characters") OR when the match is MODE A (spec §2: mode A is frozen —
	 * "Do not implement abilities or characters into what was game mode a"; the [ASSUMPTION] is that
	 * loading mode A forces every player to the default characterless Mannequin, which is what this
	 * returning false does).
	 */
	static bool AreCharactersEnabled(const UObject* WorldContextObject);

	// =============================================================================================
	// Input entry points — called by ATracePlayerController (see the report's cross-file note)
	// =============================================================================================

	/** E pressed. Straight through to TryActivate(); exists so the controller has a named verb. */
	bool HandleActivatePressed() { return TryActivate(); }

	/** V pressed / released — Mace's suspend (spec §5). Forwarded to the character set. */
	bool HandleSecondaryPressed();
	void HandleSecondaryReleased();

	/** Jump pressed. Returns TRUE if a character consumed it and the normal jump must not run. */
	bool HandleJumpPressed();
	void HandleJumpReleased();

	// =============================================================================================
	// Movement / combat notifications — called by the slices that own each event
	// =============================================================================================

	void NotifyDashStarted(const FVector& DashDirection);
	void NotifyDashEnded(bool bReachedFullDistance);
	/** SERVER ONLY. Chut's bash. @p DashProgress is 0..1 through the dash. */
	void NotifyDashHitCharacter(ATraceCharacter* Other, float DashProgress);

	/**
	 * How far the per-frame dash sweep should look for victims, in uu. 0 means "do not sweep",
	 * which is the answer for every Mannequin, every bot and every character but Chut.
	 *
	 * Static and null-safe because the caller (UTraceCharacterMovementComponent, per frame, per
	 * pawn) must be able to ask without knowing whether an ability component exists at all.
	 */
	static float GetDashHitSweepRadiusFor(const AActor* Actor);

	void NotifyPawnSpawned();

	/**
	 * The pawn died. Runs the character's own OnPawnDied() and then SPEC v19 §4.2's central wipe.
	 *
	 * *** THIS IS THE ONE PLACE DEATH STOPS ABILITIES. *** "E.g chut should not have his ability
	 * active when he is dead. It should stop, and the cooldown timer should start." It is done here,
	 * once, rather than in each character, because the two characters it was already wrong for (Chut's
	 * Chud and Rocco's speed stack) were wrong by OMISSION — and an omission cannot be fixed by asking
	 * the next author to remember. See ApplyDeathStateWipe for exactly which state goes and which
	 * stays, and why a blanket reset would break the cooldown rule stated in the same paragraph.
	 */
	void NotifyPawnDied();
	/** SERVER ONLY. Rocco's headshot stack, Chud's knife-kill refresh. */
	void NotifyKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot);

	/** Aggregate speed multiplier from this player's passives. 1.0 with no character. */
	static float GetMoveSpeedMultiplierFor(const AActor* Actor);

	/** Aggregate magnet-radius multiplier. Mace is 1.30 of UTraceSettings::CoreCatchRadius. */
	static float GetMagnetRadiusMultiplierFor(const AActor* Actor);

	/**
	 * SPEC v18 §2. Multiplier the gun must apply to UTraceSettings::FireInterval for @p Actor.
	 *
	 * *** MULTIPLY THE INTERVAL BY THIS. DO NOT ALSO DIVIDE. *** It is already the reciprocal of the
	 * rate: Roxie's Modded ×1.65 comes back as 0.606 and Slimeball's stuck +30% as 0.769, so the
	 * shipped 0.3158 s interval (190 RPM, spec v24 §4) becomes 0.1914 s and 0.2429 s respectively —
	 * 314 and 247 RPM. See UTraceCharacterAbilitySet::GetFireIntervalScale for why the gun asks this
	 * instead of casting.
	 *
	 * *** THIS FUNCTION IS WHAT MAKES SPEC v24 §0 TRUE OF THE GUN. *** Because every character's
	 * answer is a SCALE and never an interval, moving UTraceSettings::FireInterval moves every
	 * character's real fire rate in the same proportion, with no per-character re-tune. The 150 -> 190
	 * RPM pass is the proof: nothing on either character changed, and Trace.FireRate.Measure timed
	 * Roxie from 238 to 309 RPM and a stuck Slimeball from 189 to 239 while the base went 148 to 186
	 * (measured at 60 Hz, where the frame boundary costs every arm the same ~2%). Anything that ever
	 * needs to return an absolute RPM belongs somewhere else, not here.
	 *
	 * Static and null-safe: the two callers are the local fire gate and the server's rate validation,
	 * and both run for Mannequins and bots that have no ability component at all. 1.0 in every one of
	 * those cases, so the shipped feel of every other character is byte-identical.
	 */
	static float GetFireIntervalScaleFor(const AActor* Actor);

	/**
	 * SPEC v18 §2. @p Actor's well-timed slide-jump planar multiplier, given the global one.
	 *
	 * Elle is the only character that changes it (+40% of the GAIN → 1.625 against everybody's
	 * 1.446875). Everybody else — and every pawn with no component — gets @p InWellTimedBonus back
	 * unchanged, which is spec v18 §4's "Elle changes only her own" made structural.
	 */
	static float GetSlideJumpWindowSpeedBonusFor(const AActor* Actor, float InWellTimedBonus);

#if !UE_BUILD_SHIPPING
	/**
	 * DEV ONLY, AUTHORITY ONLY. Forces the activated cooldown to @p Seconds from now.
	 *
	 * Exists for ONE caller: Trace.Ability.CooldownPersistenceTest. The spec's cooldown clauses
	 * cannot be checked by reading the code — "a player can spawn with an ability timer still
	 * counting down" is a claim about what survives a pawn being destroyed, and the only way to know
	 * is to put a timer on, kill the pawn, and look. This is how the timer gets put on before any
	 * character exists to start one.
	 */
	void DebugSetActivatedCooldown(float Seconds);
#endif

	// =============================================================================================
	// Replication
	// =============================================================================================

	UFUNCTION()
	void OnRep_CharacterId();

	UFUNCTION()
	void OnRep_AbilityState();

	/**
	 * Client -> server. The character-select screen's request path; ServerSetCharacter is the
	 * authoritative half and is what actually enforces uniqueness.
	 */
	UFUNCTION(Server, Reliable)
	void ServerRequestSetCharacter(ETraceCharacterId NewCharacter);

	/** Client -> server. The validated half of TryActivate(). */
	UFUNCTION(Server, Reliable)
	void ServerTryActivate();

	/** Server -> owning client. The prediction was wrong; snap the cooldown back. */
	UFUNCTION(Client, Reliable)
	void ClientActivateRejected(float AuthoritativeCooldownEndMatchTime);

	/**
	 * Client -> server. The authoritative half of the jump hook (Rocco's second jump, Oyster's jar
	 * jump), sent by ATracePlayerController::OnJumpStarted when the local hook consumed the press.
	 *
	 * IT IS NOT PREDICTION. Neither ability is saved-move state — both write
	 * UTraceCharacterMovementComponent::Velocity from outside FSavedMove_Trace — so the owning
	 * client's write alone is corrected away within a round trip. Running the identical hook on both
	 * ends is what makes the ability survive; the remaining seam is the one-RTT ordering difference,
	 * and it is a documented limitation rather than a claim.
	 *
	 * Re-runs the same HandleJumpPressed() the client ran, so every latch inside a character's hook
	 * (Rocco's bSecondJumpUsed, Oyster's broken jar) applies on the server too.
	 */
	UFUNCTION(Server, Reliable)
	void ServerHandleJumpPressed();

	/**
	 * Client -> server. The secondary (V) hold. Reliable because a dropped release would leave Mace
	 * suspended with gravity off.
	 *
	 * Lives on the COMPONENT rather than on UTraceAbilityInputRelay so that it exists for every
	 * character, including one whose ability set never attaches a relay.
	 */
	UFUNCTION(Server, Reliable)
	void ServerSetSecondaryHeld(bool bHeld);

protected:
	/**
	 * REPLICATED. The player's character. OnRep swaps the ability set in on clients.
	 * Written only by ServerSetCharacter.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_CharacterId)
	ETraceCharacterId CharacterId = ETraceCharacterId::None;

	/**
	 * REPLICATED. Absolute match-clock time (GetServerWorldTimeSeconds) at which E comes back.
	 * Zero means ready. See the cooldown contract at the top of this file — this is the only
	 * cooldown storage in the framework and half time is the only thing that resets it.
	 */
	UPROPERTY(Replicated)
	float ActivatedCooldownEndMatchTime = 0.f;

	/** REPLICATED. The per-character transient state. See FTraceAbilityNetState. */
	UPROPERTY(ReplicatedUsing = OnRep_AbilityState)
	FTraceAbilityNetState AbilityState;

	/** The live ability set. Constructed on every machine from CharacterId; never replicated. */
	UPROPERTY(Transient)
	TObjectPtr<UTraceCharacterAbilitySet> AbilitySet = nullptr;

private:
	/**
	 * SPEC v19 §4.2. AUTHORITY ONLY. Stops every ability EFFECT this player has running, and leaves
	 * every COOLDOWN alone. Called by NotifyPawnDied() and by nothing else.
	 *
	 * The .cpp carries the full field-by-field reasoning, including why AbilityState.Reset() is the
	 * wrong tool: two shipped characters keep a cooldown in AuxEndMatchTime, and wiping it would give
	 * a dead player a free ability — the exact thing the same spec paragraph forbids.
	 */
	void ApplyDeathStateWipe();

	/** Destroys the old set and builds the one CharacterId names. Runs on every machine. */
	void RebuildAbilitySet();

	/** GetOwner() != null && GetOwner()->HasAuthority(). The one authority test in this class. */
	bool HasAuthorityOwner() const;

	/** The match clock. 0 before the GameState exists. */
	float MatchTimeNow() const;

	/** Locally predicted cooldown end, so the owning client's HUD does not wait for the round trip. */
	float PredictedCooldownEndMatchTime = 0.f;

	/** Character the local ability set was built for, so OnRep can tell a real change from a resend. */
	ETraceCharacterId BuiltForCharacter = ETraceCharacterId::None;
};
