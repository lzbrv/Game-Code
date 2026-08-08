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

	UPROPERTY(BlueprintAssignable, Category = "Trace|Health")
	FTraceOnDeath OnDeath;

	bool IsAlive() const;

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
