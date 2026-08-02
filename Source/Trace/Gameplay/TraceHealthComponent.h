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

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/ObjectMacros.h"

#include "TraceHealthComponent.generated.h"

class AActor;
class AController;

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
	 * Current health. Replicated COND_None (contract §8): the scoreboard needs alive/dead for
	 * every player, and one float per player is not worth a conditional.
	 * Initialised from UTraceSettings::MaxHealth in BeginPlay on the server.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Health, VisibleAnywhere, Category = "Trace|Health")
	float Health = 100.f;

	UPROPERTY(BlueprintAssignable, Category = "Trace|Health")
	FTraceOnDeath OnDeath;

	bool IsAlive() const;

	/** True while the owner is carrying the Core. Bullets cannot touch them; the trail still can. */
	bool IsInvulnerable() const;

	/** 0..1 against UTraceSettings::MaxHealth. Safe on clients. */
	float GetHealthPercent() const;

	/**
	 * Server only. Subtracts damage unless the owner is invulnerable, and fires OnDeath at zero.
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

	/** Single funnel for both death paths, so OnDeath can never fire twice for one life. */
	void BroadcastDeath(AController* Instigator, FName Cause);

	/**
	 * Latches once OnDeath has fired. Damage arriving in the same frame as a lethal hit (two
	 * bullets in flight, or a bullet landing on the same tick as a trail trip) must not produce a
	 * second death — the GameMode would count two deaths and schedule two respawns.
	 */
	uint8 bDeathBroadcast : 1;
};
