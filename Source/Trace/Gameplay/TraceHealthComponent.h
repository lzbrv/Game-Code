// Trace — per-character health, damage and death.
//
// Two entry points, and the difference between them is a game rule, not an implementation detail:
//
//   ApplyDamage() respects invulnerability. The Core carrier is invulnerable to bullets
//                 (contract §3), so every hitscan hit on a carrier is silently dropped here.
//   Kill()        ignores invulnerability entirely. This is the *only* way a carrier dies, and
//                 it is what UTraceTrailComponent calls when an enemy dashes through the trail.
//
// Health is server-authoritative: both mutators early-out without authority, and the replicated
// value is what clients read. The server calls OnRep_Health() by hand after every write so a
// listen server's own HUD sees the same callback a remote client does.
//
// REGENERATION (spec v13 §1) lives here too — "after 9 seconds without taking damage, characters'
// health slowly begins to regenerate. Taking damage stops this process immediately." Read the block
// comment on UTraceHealthSettings and on TickComponent() before touching either half; the
// "immediately" in that sentence is a same-FRAME guarantee and it is enforced in two independent
// ways rather than one.
//
// ===================================================================================================
// VULNERABLE (spec v14 §6, X's passive; NOW STACKING — spec v16 §4) — AND WHY IT LIVES HERE
// ===================================================================================================
//
// Verbatim (v14 §6): "An enemy hit by a bee becomes VULNERABLE for 2s, taking +25% damage FROM ALL
// SOURCES."
// Verbatim (v16 §4): "Change X's vulnerable to stack with each hit. The first stack still causes 25%
// extra damage, but each additional stack only adds 5%. Whenever the timer runs out, all stacks
// disappear."
//
// *** ONE DEADLINE, N STACKS, AND THAT SHAPE IS THE WHOLE OF v16 §4. *** There is exactly one timer
// (VulnerableUntilServerTime) and one count (VulnerableStacks). Every hit adds a stack AND rewrites
// the single deadline, so "whenever the timer runs out, all stacks disappear" needs no expiry tick,
// no per-stack timers and no cleanup pass: GetVulnerableStacks() reports 0 the instant the deadline
// passes, because it asks IsVulnerable() first. A design with one timer per stack would have made
// that sentence false — stacks would have drained one at a time — which is why it is not that.
//
// v14 §6 said "does not stack; a new application RESETS the timer". v16 §4 supersedes the first half
// and keeps the second: a new application still resets the timer, and now also adds one stack.
//
// "From all sources" is the whole requirement, so the mark is stored on the thing every source
// already funnels through: this component. Bullets (UTraceWeaponComponent::ServerFire), the knife
// (ServerSwing), ability damage (UTraceAbilityComponent::ApplyAbilityDamage), Oyster's poison ticks
// and Pickler's area damage ALL end at UTraceHealthComponent::ApplyDamage. One multiplication there
// is the whole feature; five call sites each remembering to multiply is how one of them forgets.
//
// *** IT IS NOT ON UTraceCharacterAbilitySet::GetIncomingDamageMultiplier(), AND THAT IS FORCED. ***
// That hook is only consulted when the TARGET has an ability set — i.e. when the target has picked a
// character. A player may legitimately have none: mode A freezes everybody to the Mannequin, so does
// the characters toggle, and so does a full team roster that could not serve them. A mark that only
// worked on characters would quietly stop working in mode A. (This paragraph used to say "bots are
// characterless by spec §3"; spec v15 §2 reversed that rule — bots hold characters now — but the
// conclusion is unchanged.) The victim of a mark is not necessarily a character; the victim of a mark
// is always a health component. UTraceAbilitySetX therefore deliberately does NOT override that hook,
// which is also what keeps the amplification from being applied twice on the ability damage path
// (UTraceAbilityComponent::ModifyDamageThroughPassives runs the hook, then calls ApplyDamage here).
//
// *** SPEC §4: THE MARK MUST NEVER BECOME A PATH TO DAMAGING A CORE CARRIER. ***
// Two independent locks, either one sufficient:
//   1. Applying the mark is an ETraceAbilityEffect::Control effect and goes through the ability
//      framework's choke point, which refuses a carrier. ApplyVulnerable() ALSO refuses a carrier by
//      itself, so a future caller that forgets the choke point still cannot mark one.
//   2. GetVulnerableDamageMultiplier() returns 1.0 for a Core holder, unconditionally — including
//      inside the mode-A hover-pass window where the carrier is deliberately shootable. The
//      amplification is evaluated STRICTLY AFTER ApplyDamage()'s IsInvulnerable() early-out, so a
//      carrier's zero is produced by the carrier rule and not by an amplifier that multiplied zero.
// TraceVulnerable::GetCarrierAmplifiedCount() and GetCarrierMarkedCount() are the alarms for those
// two locks; both are zero for the life of a correct process, and Trace.X.CarrierTest reads them.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/DeveloperSettings.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceHealthComponent.generated.h"

class AController;
class UTraceHealthComponent;

/**
 * Health regeneration's numbers, as config.
 *
 * A SEPARATE settings page rather than more properties on UTraceSettings, for the same file
 * ownership reason UTraceMeleeSettings and UTraceDamageSettings give: UTraceSettings belongs to
 * another slice. Nothing outside this header reads these properties directly — the clamped
 * accessors in namespace TraceHealthRegen below are the only readers, and they are also where the
 * console overrides are folded in.
 *
 * *** THE .INI WINS. READ THE VALUES FROM A RUNNING GAME, NOT FROM HERE. ***
 *
 * `Config/DefaultGame.ini` carries a live `[/Script/Trace.TraceHealthSettings]` block that sets all
 * three properties below, so every initialiser here is a default-of-last-resort the shipped game
 * never actually uses. They agree as of this pass; that is a fact about today, not an invariant.
 * `Trace.Health.DumpSettings` prints what the running process resolved, overrides included.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Trace Health (Regeneration)"))
class TRACE_API UTraceHealthSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UTraceHealthSettings();

	/** CDO, already populated by the config system. Cheap enough to call per tick. */
	static const UTraceHealthSettings& Get();

	virtual FName GetCategoryName() const override;

	/**
	 * Master switch for the whole mechanic. Shipped ON.
	 *
	 * It exists so the regen self-test can be shown going RED on a build without the feature —
	 * `Trace.Health.Regen 0` is the same arm as the console override and both funnel through
	 * TraceHealthRegen::IsEnabled().
	 */
	UPROPERTY(config, EditAnywhere, Category = "Regeneration", meta = (DisplayName = "Enable Health Regeneration"))
	bool bRegenEnabled = true;

	/**
	 * "After 9 seconds without taking damage" — seconds from the LAST DAMAGE TAKEN to the first
	 * health gained. Verbatim from the spec, so treat 9 as the design and not as a starting point.
	 *
	 * Measured from damage that actually landed. A bullet dropped by carrier invulnerability is not
	 * damage taken and deliberately does not restart this clock (see ApplyDamage): a carrier being
	 * shot at ineffectually all the way down the field would otherwise never heal, which is the
	 * opposite of the rule the spec's [ASSUMPTION] settles on.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Regeneration", meta = (DisplayName = "Regen Delay (s)", ClampMin = "0.0", ClampMax = "60.0", UIMin = "0.0", UIMax = "20.0"))
	float RegenDelaySeconds = 9.f;

	/**
	 * HP per second once regeneration is running. 10 HP/s is 0 -> 100 in ten seconds, the spec's
	 * [ASSUMPTION] for "slowly".
	 *
	 * Worth stating in the units of the guns that oppose it: a body shot is 40 (three to kill), so
	 * this buys back one body shot every four seconds of not being shot. The 9 s delay is what makes
	 * that a reward for disengaging rather than a reward for winning a trade.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Regeneration", meta = (DisplayName = "Regen Rate (HP/s)", ClampMin = "0.1", ClampMax = "1000.0", UIMin = "1.0", UIMax = "50.0"))
	float RegenRatePerSecond = 10.f;
};

/**
 * The resolved regeneration numbers: the setting, with any console override applied and clamped.
 *
 * Every reader — the component, the HUD, the self-test — goes through these three functions, so
 * there is exactly one answer to "what is the delay right now" on any given machine.
 *
 * NOTE FOR THE HUD: the console overrides are per-process, so a delay changed on the server alone
 * would make a client's countdown text disagree with the server's clock. That is a dev-knob caveat
 * only; the shipped path reads the same .ini on every machine.
 */
namespace TraceHealthRegen
{
	TRACE_API bool  IsEnabled();
	TRACE_API float GetDelaySeconds();
	TRACE_API float GetRatePerSecond();

	/** How many times damage has rescinded a heal applied earlier in the SAME frame. See ApplyDamage. */
	TRACE_API int32 GetSameFrameRescindCount();
}

/**
 * THE ALARMS FOR X's VULNERABLE MARK (spec v14 §6, guarded by spec v14 §4).
 *
 * Twins of TraceAbility::GetCarrierAbilityDamageHitCount and TraceMelee::GetCarrierKnifeHitCount:
 * counters that are zero for the whole life of a correct process, that log an Error on every
 * increment, and that a harness reads so its verdict is a measurement rather than an assertion about
 * code somebody has already read.
 *
 *   GetCarrierMarkedCount()     a Core carrier was handed the vulnerable mark. Must never happen.
 *   GetCarrierAmplifiedCount()  the +25% multiplier was evaluated on damage aimed at a Core carrier.
 *                               Must never happen — that is the ORDERING claim, and it is the one
 *                               thing about "the multiplier is applied AFTER the carrier check" that
 *                               is observable at all, because the carrier check is a `return` and
 *                               therefore leaves no damage number behind to inspect.
 *
 * The two liveness counters are the other half of the evidence: a harness that only ever reads zeros
 * cannot tell "the rule held" from "nothing happened".
 *
 *   GetAmplifiedHitCount()      how many hits the multiplier has actually amplified, ever.
 *   GetMarkAppliedCount()       how many marks have actually landed, ever.
 */
namespace TraceVulnerable
{
	TRACE_API int32 GetCarrierMarkedCount();
	TRACE_API int32 GetCarrierAmplifiedCount();
	TRACE_API int32 GetAmplifiedHitCount();
	TRACE_API int32 GetMarkAppliedCount();
	TRACE_API void  ResetCounters();

	/**
	 * The resolved multiplier for ONE stack: 1 + UTraceSettings::XVulnerableDamageBonus (x1.25).
	 *
	 * DELIBERATELY STILL THE ONE-STACK ANSWER after spec v16 §4 made the mark stack. It is what every
	 * caller written before v16 meant by "the vulnerable multiplier", and silently turning it into
	 * "the multiplier for however many stacks happen to be on somebody" would have changed the
	 * meaning of code that never asked about stacks. GetMultiplierForStacks() below is the stacking
	 * form, and UTraceHealthComponent::GetVulnerableDamageMultiplier() is what damage actually uses.
	 */
	TRACE_API float GetDamageMultiplier();

	/**
	 * Spec v16 §4's arithmetic, as a pure function: 1 + Bonus + (Stacks - 1) * StackBonus, clamped to
	 * the cap, and exactly 1 for @p Stacks <= 0.
	 *
	 * So 1 -> x1.25, 2 -> x1.30, 3 -> x1.35, and 5 (the shipped cap) -> x1.45. Pure and public so a
	 * harness can state the expected number without re-deriving it from two knobs, which is how the
	 * test and the implementation end up agreeing on a shared mistake.
	 */
	TRACE_API float GetMultiplierForStacks(int32 Stacks);

	/** The resolved stack ceiling: UTraceSettings::XVulnerableMaxStacks, clamped. Shipped 5. */
	TRACE_API int32 GetMaxStacks();

	/** What each stack after the first adds: UTraceSettings::XVulnerableStackBonus. Shipped 0.05. */
	TRACE_API float GetStackBonus();

	/** The resolved mark duration in seconds: UTraceSettings::XVulnerableDurationSeconds. */
	TRACE_API float GetDurationSeconds();

	/** True when the whole mechanic is armed (Trace.X.Vulnerable). 0 is the RED arm. */
	TRACE_API bool  IsEnabled();

	/**
	 * True when STACKING is armed (Trace.X.VulnerableStacking). 0 is the RED arm for spec v16 §4:
	 * the mark still lands and still amplifies, but every application pins the count at one, so a
	 * harness can watch three hits measure x1.25 instead of x1.35.
	 */
	TRACE_API bool  IsStackingEnabled();

	/** True when the amplifier is evaluated in the SHIPPED position, after the carrier early-out. */
	TRACE_API bool  IsApplyOrderShipped();

	/** True when the carrier locks are armed (Trace.X.VulnerableCarrierImmune). 0 is the RED arm. */
	TRACE_API bool  IsCarrierImmune();
}

/**
 * Broadcast once, on the server, the moment health reaches zero.
 * ATraceCharacter binds this to route into HandleDeath(); anything else may listen too.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FTraceOnDeath, AActor*, Victim, AController*, Killer, FName, Cause);

UCLASS(ClassGroup = (Trace), meta = (BlueprintSpawnableComponent))
class TRACE_API UTraceHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTraceHealthComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * SERVER ONLY, and TG_PostPhysics on purpose. Runs the regeneration ramp (spec v13 §1).
	 *
	 * The tick is enabled in BeginPlay only where HasAuthority() is true, so a client never advances
	 * its own health — the bar moves on a client because the replicated float moves, which is the
	 * whole point of doing it here rather than predicting it.
	 *
	 * *** WHY TG_PostPhysics: THIS IS HALF OF THE "STOPS IMMEDIATELY" GUARANTEE. ***
	 * Every damage source in this project resolves during input/movement/actor tick, i.e. in
	 * TG_PrePhysics or TG_DuringPhysics. Ticking the regen after them means that within any single
	 * frame the damage is already recorded before the ramp looks at the clock, so a hit landing on
	 * frame N produces exactly zero healing on frame N. The other half of the guarantee is the
	 * same-frame rescind in ApplyDamage(), which holds even if some future damage source moves into
	 * a later tick group. Two mechanisms, because "no frame of healing behind the hit" is the one
	 * thing about this feature that is checkable and therefore the one thing worth over-building.
	 */
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Current health. Replicated COND_None (contract §8): the scoreboard needs alive/dead for
	 * every player, and one float per player is not worth a conditional.
	 * Initialised from UTraceSettings::MaxHealth in BeginPlay on the server.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Health, VisibleAnywhere, Category = "Trace|Health")
	float Health = 100.f;

	/**
	 * Server world time (AGameStateBase::GetServerWorldTimeSeconds) of the last damage that actually
	 * LANDED on this owner. Replicated, and that is what lets a client answer the regen question for
	 * itself instead of being told.
	 *
	 * THE REPLICATED CLOCK, NOT GetWorld()->GetTimeSeconds(). A client's world time is its own; the
	 * game state's server time is the one clock every machine agrees on, and it is the same clock the
	 * kill feed and the respawn countdown already use. Comparing a server-stamped instant against a
	 * client-local clock would put the client's "REGEN IN 4.2" out by however long that client has
	 * been connected, which is a lie that gets worse the longer you play.
	 *
	 * Cheap to replicate: one float that changes only when somebody is hit, not once per tick. The
	 * REGENERATING state itself is deliberately NOT replicated — it is derived from this plus Health
	 * plus the settings, so there is no second fact that can disagree with the first.
	 */
	UPROPERTY(Replicated, VisibleAnywhere, Category = "Trace|Health")
	float LastDamageServerTime = -1000.f;

	/**
	 * REPLICATED. Absolute server time (the same clock as LastDamageServerTime) at which X's
	 * vulnerable mark expires. -1000 = never marked. Spec v14 §6: "a new application RESETS the
	 * timer" — a reset is a plain write of a later deadline, which is exactly why this is a deadline
	 * and not a countdown.
	 *
	 * *** THERE IS ONE OF THESE FOR ANY NUMBER OF STACKS (spec v16 §4). *** See the block comment at
	 * the top of this file: the single deadline is what makes "whenever the timer runs out, all
	 * stacks disappear" true without an expiry pass.
	 *
	 * Replicated so every machine can draw the mark and so a client's own damage numbers and the
	 * server's agree about who was marked when. One float that changes only when a bee connects.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Vulnerable, VisibleAnywhere, Category = "Trace|Health")
	float VulnerableUntilServerTime = -1000.f;

	/**
	 * REPLICATED. How many vulnerable stacks have been applied under the CURRENT deadline (spec
	 * v16 §4). Capped at TraceVulnerable::GetMaxStacks().
	 *
	 * *** NEVER READ THIS DIRECTLY — CALL GetVulnerableStacks(). *** This raw field is not zeroed
	 * when the deadline passes (nothing ticks to zero it, deliberately), so between expiry and the
	 * next application it still holds the count of a mark that is no longer live. The accessor asks
	 * IsVulnerable() first and is the only honest reading of "how many stacks are on this player".
	 * It is a UPROPERTY here rather than private state for the same reason the deadline is: the HUD
	 * on every machine draws the number.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Vulnerable, VisibleAnywhere, Category = "Trace|Health")
	uint8 VulnerableStacks = 0;

	UPROPERTY(BlueprintAssignable, Category = "Trace|Health")
	FTraceOnDeath OnDeath;

	bool IsAlive() const;

	// =============================================================================================
	// VULNERABLE (spec v14 §6). See the block comment at the top of this file.
	// =============================================================================================

	/**
	 * SERVER ONLY. Marks this owner vulnerable for @p DurationSeconds. Returns true if it landed.
	 *
	 * REFUSES, and that is the point of it returning a bool:
	 *   - off the server, or with the mechanic disarmed (Trace.X.Vulnerable 0);
	 *   - a dead owner;
	 *   - *** A CORE CARRIER. Spec §4. *** This is the second of the two locks; the first is that the
	 *     caller asked UTraceAbilityComponent::CanAffectTarget(Target, Control) and was refused. Both
	 *     exist because "the mark must not become a damage path" has to survive a future caller who
	 *     forgets the choke point.
	 *
	 * THE DEADLINE IS WRITTEN, NEVER ACCUMULATED, so a new application resets the duration and never
	 * extends it past DurationSeconds.
	 *
	 * STACKS (spec v16 §4). Every application that LANDS also adds one stack, up to
	 * TraceVulnerable::GetMaxStacks(). An application that lands on a target whose previous mark has
	 * already EXPIRED starts again at one stack — the count belongs to the deadline, not to the
	 * player. A refused application (carrier, dead, disarmed) adds nothing, which is what keeps the
	 * carrier locks below from being bypassed by counting.
	 *
	 * @param Source  the controller credited with the mark. May be null; kept server-side only.
	 */
	bool ApplyVulnerable(float DurationSeconds, AController* Source);

	/** SERVER ONLY. Removes the mark AND its stacks. Called on respawn and at half time. */
	void ClearVulnerable();

	/** True while the mark is live. Correct on clients — the deadline is replicated. */
	bool IsVulnerable() const;

	/** Seconds of mark left, 0 when not marked. Safe on clients. */
	float GetVulnerableRemaining() const;

	/**
	 * *** THE STACK COUNT, AND THE ONE THE HUD MUST DRAW (spec v16 §4). *** 0 when not marked.
	 *
	 * Correct on clients: both the deadline and the count are replicated, and this derives the
	 * answer from both rather than trusting the count on its own. "Whenever the timer runs out, all
	 * stacks disappear" is implemented HERE, by asking IsVulnerable() first — which is why the
	 * number falls to zero all at once and never drains.
	 *
	 * Returns 0 for a Core carrier too, because a carrier can never be marked at all (spec §4).
	 */
	int32 GetVulnerableStacks() const;

	/**
	 * THE ONE MULTIPLIER, and the reason this feature is not five call sites.
	 *
	 * Spec v16 §4's arithmetic for the live stack count: 1 + 0.25 + (N - 1) * 0.05, i.e. x1.25 at one
	 * stack, x1.30 at two, x1.35 at three, x1.45 at the shipped cap of five. Exactly 1 when unmarked.
	 * *** ALWAYS 1 FOR A CORE HOLDER *** — unconditionally, including inside the hover-pass window
	 * where a carrier is deliberately shootable, so the amplifier can never contribute a single point
	 * of damage to a carrier under any circumstance, at any stack count.
	 */
	float GetVulnerableDamageMultiplier() const;

	UFUNCTION()
	void OnRep_Vulnerable();

	/**
	 * True when health is climbing RIGHT NOW. Safe and correct on clients — see LastDamageServerTime.
	 *
	 * Requires alive, below maximum, the mechanic enabled, and the delay elapsed. "Below maximum" is
	 * part of the answer rather than an optimisation: a HUD that says REGENERATING at 100/100 is
	 * telling the player something they cannot act on.
	 */
	bool IsRegenerating() const;

	/**
	 * Seconds until regeneration starts, for the HUD's countdown. Safe on clients.
	 *
	 * 0 while it is already running. Negative means "not applicable" — dead, at full health, or the
	 * mechanic is off — so the HUD can drop the row entirely rather than print a countdown to
	 * nothing.
	 */
	float GetSecondsUntilRegen() const;

	/** Total health this component has regenerated since BeginPlay. Server-side; used by the self-test. */
	float GetTotalRegenerated() const { return TotalRegenerated; }

	/** True while the owner is carrying the Core. Bullets cannot touch them; the trail still can. */
	bool IsInvulnerable() const;

	/** 0..1 against UTraceSettings::MaxHealth. Safe on clients. */
	float GetHealthPercent() const;

	/**
	 * Server only. Subtracts damage unless the owner is invulnerable, and fires OnDeath at zero.
	 * Also the ONE place the regeneration clock is restarted, and the place the same-frame rescind
	 * lives — both because they must happen for damage that LANDED and only for damage that landed.
	 *
	 * @param Amount     Damage to apply; non-positive and non-finite amounts are ignored.
	 * @param Instigator Controller credited with the damage. May be null (world damage).
	 * @param Cause      "Bullet" / "Trail" / "Fell".
	 */
	void ApplyDamage(float Amount, AController* Instigator, FName Cause);

	/**
	 * Server only. Kills outright, *ignoring* invulnerability — this is how trail deaths land on
	 * an otherwise bullet-proof carrier. Never route bullets through here.
	 */
	void Kill(AController* Instigator, FName Cause);

	/** Server only. Restores full health and re-arms the death broadcast (used on respawn/reset). */
	void ResetHealth();

	UFUNCTION()
	void OnRep_Health();

private:
	float GetMaxHealth() const;

	/** True only on the machine that owns this component's actor authority-wise. */
	bool HasAuthority() const;

	/**
	 * The replicated clock this component stamps and compares against, in seconds.
	 *
	 * Falls back to the local world clock when there is no game state yet (the first frames of a
	 * map load, and every headless fixture spawned before one exists). On the server the two are the
	 * same number, so the fallback only ever costs a client a fraction of a second of countdown text
	 * before the game state arrives.
	 */
	float ServerTimeNow() const;

	/** Total health regenerated over this component's lifetime. Server-side bookkeeping for the tests. */
	float TotalRegenerated = 0.f;

	/**
	 * The frame on which the regen ramp last added health, and how much it added AFTER clamping.
	 *
	 * These two exist for one job: ApplyDamage() takes the heal back if it landed on the same frame
	 * as the hit. Storing the CLAMPED amount matters — a ramp that ran into the 100 ceiling applied
	 * less than Rate * DeltaTime, and rescinding the requested amount instead of the applied one
	 * would quietly invent damage.
	 */
	uint64 LastRegenFrame = 0;
	float  LastRegenApplied = 0.f;

	/** Single funnel for both death paths, so OnDeath can never fire twice for one life. */
	void BroadcastDeath(AController* Instigator, FName Cause);

	/**
	 * Applies GetVulnerableDamageMultiplier() to @p Amount and keeps the alarms.
	 *
	 * Separated from ApplyDamage() so that the RED ARM (Trace.X.VulnerableApplyOrder 0) can call it
	 * from the wrong side of the carrier early-out and the harness can see the difference. On the
	 * shipped arm it is called from exactly one place, immediately after IsInvulnerable() has had
	 * its say.
	 */
	float AmplifyForVulnerable(float Amount) const;

	/** The controller credited with the live mark. Server-side only; never replicated. */
	TWeakObjectPtr<AController> VulnerableSource;

	/** The purely cosmetic marker actor, spawned locally on every non-dedicated machine. */
	TWeakObjectPtr<AActor> VulnerableMarker;

	/** Spawns / destroys VulnerableMarker to match IsVulnerable(). Cosmetic, never authoritative. */
	void UpdateVulnerableMarker();

	/**
	 * Latches once OnDeath has fired. Damage arriving in the same frame as a lethal hit (two
	 * bullets in flight, or a bullet landing on the same tick as a trail trip) must not produce a
	 * second death — the GameMode would count two deaths and schedule two respawns.
	 */
	uint8 bDeathBroadcast : 1;
};

/**
 * TEST FIXTURE for `Trace.Health.RegenTest`. A bare replicated actor whose only content is one
 * UTraceHealthComponent. Never spawned by gameplay; the dev console command spawns exactly one.
 *
 * *** WHY A FIXTURE AND NOT THE LOCAL PAWN, which is the obvious thing to measure. ***
 * A headless run fills both teams with bots, and a bot that shoots the pawn under test restarts the
 * very clock the test is timing — so a run measuring "9 s to first heal" on a live pawn would report
 * whatever number the bots' aim happened to produce, and a failing build and a busy firefight would
 * be indistinguishable in the log. The fixture is spawned far above the field, is not a
 * ATraceCharacter (so it is never a carrier, never shootable, never a bot's target) and takes damage
 * only from the test itself. That makes the timing numbers in the report measurements rather than
 * anecdotes.
 *
 * It REPLICATES, and that is the second half of its job: a joined client watches this actor's health
 * climb to prove the bar moves on a client, again without a bot deciding when the clock restarts.
 * The component is created in the constructor rather than added at runtime so it is a class-default
 * subobject on both machines, which is the arrangement of replicated component this engine handles
 * without any subobject-list bookkeeping.
 */
UCLASS(NotPlaceable, Transient)
class TRACE_API ATraceHealthRegenFixture : public AActor
{
	GENERATED_BODY()

public:
	ATraceHealthRegenFixture();

	UPROPERTY(VisibleAnywhere, Category = "Trace|Health")
	TObjectPtr<UTraceHealthComponent> Health;
};

/**
 * The purely cosmetic "this player is VULNERABLE" marker (spec v14 §6).
 *
 * A small emissive ring above a marked player's head. It exists so that the mark is a thing a player
 * can SEE — a +25% amplifier nobody can perceive is a balance change disguised as a mechanic — and so
 * that a screenshot can show the feature working rather than a log line claiming it.
 *
 * *** LOCAL, NEVER REPLICATED, NEVER AUTHORITATIVE. *** Each machine spawns its own from the
 * replicated VulnerableUntilServerTime, exactly the way the Core's beacon is driven. It has no
 * collision, is not a ATraceCharacter and can never eat a bullet, and it lives in this header rather
 * than in the Abilities folder so that the health component — which is where the mark lives — does
 * not acquire a dependency on any one character's files.
 *
 * It is declared here alongside ATraceHealthRegenFixture for the same reason that one is: it is a
 * helper actor with exactly one owner and no independent existence.
 */
UCLASS(NotPlaceable, Transient)
class TRACE_API ATraceVulnerableMarker : public AActor
{
	GENERATED_BODY()

public:
	ATraceVulnerableMarker();

	virtual void Tick(float DeltaSeconds) override;

	/** The health component whose mark this is drawing. Cleared -> the marker destroys itself. */
	TWeakObjectPtr<UTraceHealthComponent> Watching;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Health")
	TObjectPtr<class UStaticMeshComponent> Ring;

private:
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> RingMID;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> BaseMaterial;
};
