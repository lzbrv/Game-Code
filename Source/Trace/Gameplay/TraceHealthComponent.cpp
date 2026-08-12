// Trace — per-character health, damage and death.

#include "Gameplay/TraceHealthComponent.h"

#include "Net/UnrealNetwork.h"

#include "Abilities/TraceAbilityTypes.h"   // spec v19 §3: TraceAbilityTraits — Lily's 60 health

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"             // FTSTicker — the per-frame driver for the self-test
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "CoreGlobals.h"                   // GFrameCounter — the same-frame guarantee is counted in frames
#include "Core/TraceCharacter.h"
#include "Engine/World.h"
#include "EngineUtils.h"                   // TActorIterator
#include "Gameplay/TraceCore.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"   // GetServerWorldTimeSeconds — the one clock both ends share
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"              // FPlatformTime::Seconds — the v16 §4 harnesses pace on real time
#include "Math/UnrealMathUtility.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// Console overrides for regeneration (spec v13 §1).
//
// The two tuning knobs use the project's negative-sentinel convention ("negative defers to the
// setting"), the same one UTraceMeleeSettings' overrides use. The enable arm is an int because it
// is a switch and because it is the arm the self-test uses to prove it can go RED.
//
// NONE of these shares a name with a console COMMAND — that collision is fatal at module load in
// this engine version. The commands below are all verbs or compounds (Trace.Health.RegenTest,
// .DumpSettings, .Hurt, .Watch); these three are nouns, and "Trace.Health.Regen" is deliberately
// NOT also a command name.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarHealthRegen(
	TEXT("Trace.Health.Regen"), 1,
	TEXT("1 (shipped): health regenerates after Trace.Health.RegenDelay seconds without damage. "
	     "0: removes the mechanic, so Trace.Health.RegenTest can be shown FAILING on a build without it."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarHealthRegenDelay(
	TEXT("Trace.Health.RegenDelay"), -1.f,
	TEXT("Override for the seconds of no damage before regeneration begins. Negative defers to UTraceHealthSettings."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarHealthRegenRate(
	TEXT("Trace.Health.RegenRate"), -1.f,
	TEXT("Override for the regeneration rate in HP per second. Negative defers to UTraceHealthSettings."),
	ECVF_Default);

/**
 * How many times a heal has been rescinded by damage arriving on the same frame.
 *
 * File-static rather than a member: the fact being counted is about the RULE ("taking damage stops
 * this process immediately"), not about any one pawn, and the self-test reads it as a global.
 *
 * Expected to stay at 0 in practice — the TG_PostPhysics tick group already orders the ramp after
 * every damage source in the project. A non-zero count is not a bug, it is the second mechanism
 * doing its job, and it is worth knowing about because it means some damage source has moved into
 * a later tick group than it used to be in.
 */
static int32 GRegenSameFrameRescinds = 0;

// =================================================================================================
// VULNERABLE — spec v14 §6 (X's passive), guarded by spec v14 §4.
//
// Three arms, three separate things they falsify. None of them shares a name with a console COMMAND;
// the commands that read them live in Abilities/Characters/TraceXVerify.cpp and are all verbs
// (Trace.X.VulnerableTest, Trace.X.CarrierTest), never these nouns.
// =================================================================================================

/** MASTER ARM. 0 removes the amplification entirely, so "+25% is real" can be shown failing. */
static TAutoConsoleVariable<int32> CVarVulnerable(
	TEXT("Trace.X.Vulnerable"), 1,
	TEXT("1 (shipped): a vulnerable target takes +XVulnerableDamageBonus damage from every source. "
	     "0: the RED arm — the mark is still applied and still drawn, but multiplies nothing, so "
	     "Trace.X.VulnerableTest can be shown measuring 40 instead of 50."),
	ECVF_Default);

/**
 * THE ORDERING ARM, and the only observable form the ordering claim can take.
 *
 * Spec brief, verbatim: "make sure the multiplier is applied AFTER the carrier check, never before
 * it, or a 0 becomes a 0 by luck rather than by rule."
 *
 * ApplyDamage()'s carrier check is a `return`, not a scale — so moving the multiply above it does
 * not change the damage a carrier takes (it is zero either way) and a red arm that only watched
 * health would be uninformative, which is exactly the failure mode this project has been bitten by.
 * What DOES change is whether the amplifier ran on a carrier's damage at all, and that is counted:
 * GCarrierAmplified increments only from AmplifyForVulnerable(), only for a Core holder. On the red
 * arm (0) the counter moves; on the shipped arm (1) it cannot, because the function is never reached
 * with a carrier as the owner.
 */
static TAutoConsoleVariable<int32> CVarVulnerableApplyOrder(
	TEXT("Trace.X.VulnerableApplyOrder"), 1,
	TEXT("1 (shipped): the vulnerable multiplier is evaluated AFTER ApplyDamage's carrier early-out. "
	     "0: the RED arm — evaluated BEFORE it, which makes the amplifier run on a Core carrier's "
	     "damage and moves TraceVulnerable::GetCarrierAmplifiedCount()."),
	ECVF_Default);

/** THE CARRIER ARM. 0 removes both carrier locks so Trace.X.CarrierTest can reproduce the failure. */
static TAutoConsoleVariable<int32> CVarVulnerableCarrierImmune(
	TEXT("Trace.X.VulnerableCarrierImmune"), 1,
	TEXT("1 (shipped): a Core carrier can neither be marked vulnerable nor have the multiplier "
	     "applied to them. 0: the RED arm for spec v14 §4 — both locks removed."),
	ECVF_Default);

/**
 * THE STACKING ARM (spec v16 §4). 0 pins every mark at one stack.
 *
 * A separate arm from CVarVulnerable, and the separation is what makes it useful: with the master
 * arm at 0 a marked target takes 40 and an unmarked target takes 40, which says nothing about
 * stacking. With THIS arm at 0 the mark still lands, still amplifies, and still resets its timer —
 * the ONLY thing that changes is that the third hit measures x1.25 instead of x1.35. That is a
 * single-variable A/B on the sentence the spec actually added.
 */
static TAutoConsoleVariable<int32> CVarVulnerableStacking(
	TEXT("Trace.X.VulnerableStacking"), 1,
	TEXT("1 (shipped, spec v16 §4): vulnerable stacks — +25% for the first hit and +5% for each one "
	     "after, all of them expiring together. 0: the RED arm — the mark still lands and still "
	     "amplifies, but the count is pinned at 1, so Trace.X.VulnerableStackTest measures x1.25 "
	     "where it expects x1.35."),
	ECVF_Default);

/**
 * The four counters. File-static for the same reason GRegenSameFrameRescinds is: the facts being
 * counted are about the RULES, not about any one pawn.
 *
 * The first two must be zero for the life of a correct process. The second two are LIVENESS — they
 * are what lets a harness distinguish "the carrier rule held" from "the fixture never fired".
 */
static int32 GVulnerableCarrierMarked   = 0;
static int32 GVulnerableCarrierAmplified = 0;
static int32 GVulnerableAmplifiedHits    = 0;
static int32 GVulnerableMarksApplied     = 0;

namespace TraceVulnerable
{
	int32 GetCarrierMarkedCount()    { return GVulnerableCarrierMarked; }
	int32 GetCarrierAmplifiedCount() { return GVulnerableCarrierAmplified; }
	int32 GetAmplifiedHitCount()     { return GVulnerableAmplifiedHits; }
	int32 GetMarkAppliedCount()      { return GVulnerableMarksApplied; }

	void ResetCounters()
	{
		GVulnerableCarrierMarked = 0;
		GVulnerableCarrierAmplified = 0;
		GVulnerableAmplifiedHits = 0;
		GVulnerableMarksApplied = 0;
	}

	float GetDamageMultiplier()
	{
		// Derived from the knob, never hardcoded: spec §6 says +25%, UTraceSettings::
		// XVulnerableDamageBonus says 0.25, and Config/DefaultGame.ini wins over both.
		//
		// STILL THE ONE-STACK ANSWER after spec v16 §4 — see the header. Callers that predate
		// stacking mean "the multiplier of a mark", and that is what they still get.
		return 1.f + FMath::Clamp(UTraceSettings::Get().XVulnerableDamageBonus, 0.f, 3.f);
	}

	int32 GetMaxStacks()
	{
		return FMath::Clamp(UTraceSettings::Get().XVulnerableMaxStacks, 1, 50);
	}

	float GetStackBonus()
	{
		return FMath::Clamp(UTraceSettings::Get().XVulnerableStackBonus, 0.f, 1.f);
	}

	float GetMultiplierForStacks(int32 Stacks)
	{
		if (Stacks <= 0)
		{
			return 1.f;
		}

		// The cap is applied HERE as well as at the point the count is written, and that is not
		// belt-and-braces for its own sake: a stack count can also arrive by REPLICATION from a
		// server whose XVulnerableMaxStacks differs from this machine's (a mid-session .ini edit, a
		// dev-console change on one end). Clamping the arithmetic means the worst such a mismatch can
		// do is disagree by a multiplier, never produce an unbounded one.
		const int32 Effective = FMath::Clamp(Stacks, 1, GetMaxStacks());

		// SPEC v16 §4, verbatim: "The first stack still causes 25% extra damage, but each additional
		// stack only adds 5%." So the first stack is the whole of GetDamageMultiplier() and the
		// (N - 1) after it are worth GetStackBonus() each.
		return GetDamageMultiplier() + static_cast<float>(Effective - 1) * GetStackBonus();
	}

	float GetDurationSeconds()
	{
		return FMath::Clamp(UTraceSettings::Get().XVulnerableDurationSeconds, 0.01f, 30.f);
	}

	bool IsEnabled()          { return CVarVulnerable.GetValueOnAnyThread() != 0; }
	bool IsStackingEnabled()  { return CVarVulnerableStacking.GetValueOnAnyThread() != 0; }
	bool IsApplyOrderShipped(){ return CVarVulnerableApplyOrder.GetValueOnAnyThread() != 0; }
	bool IsCarrierImmune()    { return CVarVulnerableCarrierImmune.GetValueOnAnyThread() != 0; }
}

// =================================================================================================
// Settings object
// =================================================================================================

UTraceHealthSettings::UTraceHealthSettings()
{
	// Values live on the property declarations in the header, next to the reasoning for each — the
	// same arrangement UTraceMeleeSettings uses, and for the same reason: a constructor that also
	// sets defaults gives the next reader two places to look and one of them will go stale.
}

const UTraceHealthSettings& UTraceHealthSettings::Get()
{
	const UTraceHealthSettings* Settings = GetDefault<UTraceHealthSettings>();
	check(Settings != nullptr);
	return *Settings;
}

FName UTraceHealthSettings::GetCategoryName() const
{
	return FName(TEXT("Game"));
}

namespace TraceHealthRegen
{
	bool IsEnabled()
	{
		// BOTH have to agree. The setting is the designer's answer and the CVar is the test arm; an
		// arm that could switch the mechanic back ON against a designer's "off" would make the
		// self-test's red run meaningless.
		return UTraceHealthSettings::Get().bRegenEnabled && (CVarHealthRegen.GetValueOnAnyThread() != 0);
	}

	float GetDelaySeconds()
	{
		const float Override = CVarHealthRegenDelay.GetValueOnAnyThread();
		const float Resolved = (Override >= 0.f) ? Override : UTraceHealthSettings::Get().RegenDelaySeconds;
		return FMath::Clamp(Resolved, 0.f, 60.f);
	}

	float GetRatePerSecond()
	{
		const float Override = CVarHealthRegenRate.GetValueOnAnyThread();
		const float Resolved = (Override > 0.f) ? Override : UTraceHealthSettings::Get().RegenRatePerSecond;
		return FMath::Clamp(Resolved, 0.1f, 1000.f);
	}

	int32 GetSameFrameRescindCount()
	{
		return GRegenSameFrameRescinds;
	}
}

UTraceHealthComponent::UTraceHealthComponent()
{
	// TICKS NOW, and only on the server — see the note on TickComponent(). It used to say "nothing
	// to tick: health only changes in response to events", which stopped being true the moment
	// health acquired a ramp. bCanEverTick has to be set here (the tick function is registered from
	// the constructor's value); bStartWithTickEnabled stays false so a CLIENT's copy never ticks at
	// all, and BeginPlay turns it on where there is authority.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	SetIsReplicatedByDefault(true);

	bDeathBroadcast = 0;
}

void UTraceHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// Settings are read at runtime rather than cached in the constructor: UTraceSettings is a
	// config CDO and designers change MaxHealth live.
	if (HasAuthority())
	{
		Health = GetMaxHealth();
		bDeathBroadcast = 0;

		// Stamp the clock at spawn rather than leaving the -1000 sentinel. At full health nothing
		// reads it, but a pawn that takes a hit inside the first frames of its life then has a
		// regen delay measured from a real instant instead of from the beginning of time.
		LastDamageServerTime = ServerTimeNow();
		TotalRegenerated = 0.f;
		LastRegenFrame = 0;
		LastRegenApplied = 0.f;

		SetComponentTickEnabled(true);
	}
}

void UTraceHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UTraceHealthComponent, Health);

	// COND_None, like Health and for the same reason: any client's HUD may need to reason about any
	// pawn's regeneration state (a spectator, and the scoreboard's alive/dead already pays this
	// cost), and this is one float that changes only when somebody is hit.
	DOREPLIFETIME(UTraceHealthComponent, LastDamageServerTime);

	// COND_None as well. The mark has to be visible on every machine — the marked player needs to
	// know they are marked, X needs to see the payoff, and everyone else needs to know which enemy is
	// worth shooting first. One float that changes only when a bee connects.
	DOREPLIFETIME(UTraceHealthComponent, VulnerableUntilServerTime);

	// Spec v16 §4: the HUD draws the stack count, so it travels with the deadline. One BYTE, and it
	// changes on exactly the same events the deadline does, so this adds no new replication cadence
	// at all — the two are written together and go out in the same bunch.
	DOREPLIFETIME(UTraceHealthComponent, VulnerableStacks);
}

float UTraceHealthComponent::ServerTimeNow() const
{
	const UWorld* CurrentWorld = GetWorld();
	if (CurrentWorld == nullptr)
	{
		return 0.f;
	}

	if (const AGameStateBase* StateBase = CurrentWorld->GetGameState())
	{
		// Double on 5.3+; the clock only needs float precision over a match, and every other
		// consumer of it in this project (the kill feed, the respawn countdown) narrows here too.
		return static_cast<float>(StateBase->GetServerWorldTimeSeconds());
	}

	return CurrentWorld->GetTimeSeconds();
}

bool UTraceHealthComponent::HasAuthority() const
{
	const AActor* Owner = GetOwner();
	return Owner != nullptr && Owner->HasAuthority();
}

float UTraceHealthComponent::GetMaxHealth() const
{
	// SPEC v19 §3 — LILY'S 60 HEALTH, AS AN ABSOLUTE AND NOT AS A FRACTION.
	//
	// Zero means "this character has no opinion, use the shared number", which is every character but
	// Lily. It is an absolute because the spec states one: a fraction of a retuned MaxHealth would
	// silently stop being 60 the first time somebody moved the global, and "Lily has 60 health" would
	// quietly become a lie in a file nobody thought to re-read.
	//
	// Read live and on every call, exactly like the shared number below it, so that the value the
	// health bar divides by, the value a heal clamps to and the value the server enforces cannot
	// disagree — and so a character SWITCH in the practice range takes effect immediately.
	const float Override = TraceAbilityTraits::GetMaxHealthOverride(GetOwner());
	if (Override > 0.f)
	{
		return FMath::Max(1.f, Override);
	}

	return FMath::Max(1.f, UTraceSettings::Get().MaxHealth);
}

bool UTraceHealthComponent::IsAlive() const
{
	return Health > 0.f;
}

bool UTraceHealthComponent::IsInvulnerable() const
{
	// Invulnerability is a property of *holding the Core*, not of this component, so it is derived
	// rather than stored — there is no way for the two to fall out of sync.
	if (const ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner()))
	{
		// Spec §4, THE RISK BEAT: the shield drops for the whole pass window. Both halves of the
		// beat (this, and the trace hardening) read the SAME replicated bool on ATraceCore, so they
		// cannot drift apart, and a cancel restores both in one statement.
		//
		// BOTH TERMS NOW ASK THE CORE. This used to be `OwningCharacter->IsCarrier() && ...`, mixing
		// the pawn's replicated presentation mirror with the Core's authoritative pointer inside one
		// expression — two independently replicated facts deciding one damage rule. IsCoreHolder()
		// reads Core->Carrier, the same object the second term reads, so a frame in which the mirror
		// and the Core disagree can no longer make a player briefly invulnerable or briefly shootable.
		return ATraceCore::IsCoreHolder(OwningCharacter)
			&& !ATraceCore::IsShieldSuppressedFor(OwningCharacter);
	}
	return false;
}

float UTraceHealthComponent::GetHealthPercent() const
{
	return FMath::Clamp(Health / GetMaxHealth(), 0.f, 1.f);
}

// -------------------------------------------------------------------------------------------
// VULNERABLE (spec v14 §6, guarded by spec v14 §4)
// -------------------------------------------------------------------------------------------

bool UTraceHealthComponent::IsVulnerable() const
{
	return VulnerableUntilServerTime > ServerTimeNow();
}

float UTraceHealthComponent::GetVulnerableRemaining() const
{
	return FMath::Max(0.f, VulnerableUntilServerTime - ServerTimeNow());
}

int32 UTraceHealthComponent::GetVulnerableStacks() const
{
	// *** "WHENEVER THE TIMER RUNS OUT, ALL STACKS DISAPPEAR" (spec v16 §4), IMPLEMENTED. ***
	//
	// The count is gated on the deadline rather than being zeroed by anything, so every stack stops
	// counting on the same frame — there is no drain, no per-stack timer and nothing to tick. It also
	// means an expired VulnerableStacks left sitting in the field is harmless: it is unreadable
	// through this accessor, and the next application overwrites it (see ApplyVulnerable).
	if (!IsVulnerable())
	{
		return 0;
	}

	// A carrier can never hold a stack. ApplyVulnerable refuses them, so this is a second, cheap
	// statement of the same rule for anything that reads the count rather than the multiplier — the
	// HUD included, which must not draw a stack badge over a carrier.
	if (TraceVulnerable::IsCarrierImmune())
	{
		if (const ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner()))
		{
			if (ATraceCore::IsCoreHolder(OwningCharacter) || OwningCharacter->IsCarrier())
			{
				return 0;
			}
		}
	}

	return FMath::Clamp(static_cast<int32>(VulnerableStacks), 0, TraceVulnerable::GetMaxStacks());
}

float UTraceHealthComponent::GetVulnerableDamageMultiplier() const
{
	if (!TraceVulnerable::IsEnabled() || !IsVulnerable())
	{
		return 1.f;
	}

	// *** SPEC §4, LOCK 2. The amplifier never touches a Core holder's damage. ***
	//
	// IsCoreHolder(), NOT IsInvulnerable(). The two differ inside the mode-A hover-pass window, where
	// IsInvulnerable() is deliberately false so that a passing carrier is shootable (spec §4's risk
	// beat). A bullet in that window is not an ability and is allowed to land — but amplifying it
	// with an ability's mark would be an ability contributing damage to a carrier, which is the
	// sentence §4 forbids. So the strictest of the two predicates is the one used here, and the cost
	// is nil: mode A has no characters at all, so nothing in mode A can ever be marked.
	if (TraceVulnerable::IsCarrierImmune())
	{
		if (const ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner()))
		{
			if (ATraceCore::IsCoreHolder(OwningCharacter) || OwningCharacter->IsCarrier())
			{
				return 1.f;
			}
		}
	}

	// SPEC v16 §4. The stack count decides the number now; one stack still resolves to exactly
	// TraceVulnerable::GetDamageMultiplier(), so nothing about the pre-v16 single-mark case moved.
	//
	// The floor of 1 matters for one real case: a mark that landed while stacking was DISARMED, or
	// one replicated from a machine that wrote 0 stacks alongside a live deadline. IsVulnerable() is
	// true there but the count is 0, and multiplying by GetMultiplierForStacks(0) = 1.0 is the right
	// answer — never a silent x0 that would delete the damage entirely.
	const int32 Stacks = FMath::Max(1, GetVulnerableStacks());
	return TraceVulnerable::GetMultiplierForStacks(Stacks);
}

float UTraceHealthComponent::AmplifyForVulnerable(float Amount) const
{
	const float Multiplier = GetVulnerableDamageMultiplier();
	if (Multiplier <= 1.f)
	{
		return Amount;
	}

	// THE ALARM. Reaching here with a carrier as the owner means either the carrier lock in
	// GetVulnerableDamageMultiplier() was disarmed (the red arm) or the ordering regressed. Logged as
	// an Error, counted, and read by Trace.X.CarrierTest.
	if (const ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner()))
	{
		if (ATraceCore::IsCoreHolder(OwningCharacter) || OwningCharacter->IsCarrier())
		{
			++GVulnerableCarrierAmplified;
			UE_LOG(LogTraceGame, Error,
				TEXT("[Vulnerable] *** THE +%.0f%% VULNERABLE MULTIPLIER WAS EVALUATED ON DAMAGE AIMED AT THE CORE "
				     "CARRIER %s (hit #%d). Spec v14 §4: no ability may contribute damage to a carrier. Check "
				     "Trace.X.VulnerableCarrierImmune and Trace.X.VulnerableApplyOrder."),
				(Multiplier - 1.f) * 100.f, *GetNameSafe(GetOwner()), GVulnerableCarrierAmplified);
		}
	}

	++GVulnerableAmplifiedHits;
	return Amount * Multiplier;
}

bool UTraceHealthComponent::ApplyVulnerable(float DurationSeconds, AController* Source)
{
	if (!HasAuthority())
	{
		// Clients get the mark by replication. A local write would be overwritten and would make a
		// client briefly believe in a mark the server never granted.
		return false;
	}

	// NOTE: Trace.X.Vulnerable is NOT consulted here, deliberately. That arm disarms the AMPLIFIER,
	// not the mark, so that the red run and the green run differ in exactly one thing — the
	// multiplication — and are otherwise the same marked target taking the same call. An arm that
	// also suppressed the mark would be comparing "marked, unamplified" against "not marked at all",
	// which tests nothing about the multiplier.
	if (!IsAlive() || DurationSeconds <= 0.f || !FMath::IsFinite(DurationSeconds))
	{
		return false;
	}

	// *** SPEC §4, LOCK 1 (the second half of it). ***
	//
	// The caller is expected to have asked UTraceAbilityComponent::CanAffectTarget(Target, Control)
	// and been refused already — UTraceAbilitySetX does. This is the belt to that pair of braces, and
	// it exists because "the mark must not become a damage path" has to survive a future caller who
	// reaches for ApplyVulnerable() directly and forgets the choke point.
	if (TraceVulnerable::IsCarrierImmune())
	{
		if (const ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner()))
		{
			if (ATraceCore::IsCoreHolder(OwningCharacter) || OwningCharacter->IsCarrier())
			{
				UE_LOG(LogTraceGame, Verbose,
					TEXT("[Vulnerable] mark on %s refused: they are the Core carrier (spec v14 §4)."),
					*GetNameSafe(GetOwner()));
				return false;
			}
		}
	}
	else if (const ATraceCharacter* RedArmCharacter = Cast<ATraceCharacter>(GetOwner()))
	{
		// The red arm reached a carrier. Counted so the harness can prove the arm actually disarmed
		// something rather than reporting a green that never had a rule to break.
		if (ATraceCore::IsCoreHolder(RedArmCharacter) || RedArmCharacter->IsCarrier())
		{
			++GVulnerableCarrierMarked;
			UE_LOG(LogTraceGame, Error,
				TEXT("[Vulnerable] *** THE CORE CARRIER %s WAS MARKED VULNERABLE (mark #%d). Spec v14 §4. This is "
				     "only reachable with Trace.X.VulnerableCarrierImmune 0."),
				*GetNameSafe(GetOwner()), GVulnerableCarrierMarked);
		}
	}

	// *** THE STACK COUNT (spec v16 §4), COMPUTED BEFORE THE DEADLINE IS REWRITTEN. ***
	//
	// The order is load-bearing and it is the whole of "whenever the timer runs out, all stacks
	// disappear": GetVulnerableStacks() is read while the OLD deadline is still in the field, so a
	// mark whose timer had already run out reads 0 and this application starts again at one stack. If
	// the write below happened first, an expired mark would read as live and a target could
	// accumulate stacks across arbitrarily long gaps — the exact bug the sentence forbids.
	//
	// It is also what makes the count belong to the DEADLINE rather than to the player.
	const int32 PreviousStacks = GetVulnerableStacks();
	const int32 NewStacks = TraceVulnerable::IsStackingEnabled()
		? FMath::Clamp(PreviousStacks + 1, 1, TraceVulnerable::GetMaxStacks())
		: 1;   // the RED arm: the mark lands and amplifies, but never stacks

	// THE DEADLINE IS WRITTEN, NEVER ACCUMULATED. Spec §6: "a new application RESETS the timer."
	// A plain write both resets a running mark and starts a new one, and can never accumulate past
	// DurationSeconds — which is what makes ONE timer enough for N stacks.
	VulnerableUntilServerTime = ServerTimeNow() + DurationSeconds;
	VulnerableStacks = static_cast<uint8>(NewStacks);
	VulnerableSource = Source;
	++GVulnerableMarksApplied;

	// Replication callbacks never fire on the authority; call it directly so a listen server draws
	// the marker on the same frame a remote client does.
	OnRep_Vulnerable();

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Vulnerable] %s marked for %.2fs by %s — stack %d of %d, x%.2f damage."),
		*GetNameSafe(GetOwner()), DurationSeconds, *GetNameSafe(Source),
		NewStacks, TraceVulnerable::GetMaxStacks(), TraceVulnerable::GetMultiplierForStacks(NewStacks));
	return true;
}

void UTraceHealthComponent::ClearVulnerable()
{
	if (!HasAuthority())
	{
		return;
	}

	VulnerableUntilServerTime = -1000.f;
	// Zeroed as well as expired. The deadline alone would already make GetVulnerableStacks() report
	// 0, but leaving a stale count in a REPLICATED field means every client's HUD holds a number it
	// is only not drawing because of a second field — and the next mark would overwrite it anyway.
	// Clearing both keeps the wire state and the game state saying the same thing.
	VulnerableStacks = 0;
	VulnerableSource = nullptr;
	OnRep_Vulnerable();
}

void UTraceHealthComponent::OnRep_Vulnerable()
{
	UpdateVulnerableMarker();
}

// -------------------------------------------------------------------------------------------
// Regeneration (spec v13 §1)
// -------------------------------------------------------------------------------------------

float UTraceHealthComponent::GetSecondsUntilRegen() const
{
	// Negative is "not applicable", and each of these three is a different reason the HUD should
	// draw nothing rather than a countdown to something that will not happen.
	if (!TraceHealthRegen::IsEnabled() || !IsAlive())
	{
		return -1.f;
	}

	if (Health >= GetMaxHealth() - UE_KINDA_SMALL_NUMBER)
	{
		return -1.f;
	}

	const float Elapsed = ServerTimeNow() - LastDamageServerTime;
	return FMath::Max(0.f, TraceHealthRegen::GetDelaySeconds() - Elapsed);
}

bool UTraceHealthComponent::IsRegenerating() const
{
	// One source, three states, so the bar and the countdown can never disagree about which of them
	// should be on screen: < 0 is "not applicable", > 0 is "counting down", exactly 0 is "running".
	const float Remaining = GetSecondsUntilRegen();
	return FMath::IsNearlyZero(Remaining) && (Remaining >= 0.f);
}

void UTraceHealthComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Belt and braces against the tick ever being enabled somewhere it should not be. A client that
	// advanced its own health would win the argument for exactly one frame and then be snapped back
	// by the next replication, which is the classic "the bar jitters" bug.
	if (!HasAuthority() || DeltaTime <= 0.f)
	{
		return;
	}

	if (!TraceHealthRegen::IsEnabled() || !IsAlive())
	{
		return;
	}

	const float MaxHealth = GetMaxHealth();
	if (Health >= MaxHealth)
	{
		return;
	}

	// THE WHOLE RULE, in one comparison against a stamped instant. Deliberately not a countdown
	// float decremented per tick and not a bool set by a timer: both of those are second copies of
	// "when was I last hit" that can be reset from one place and read from another. ApplyDamage
	// writes the instant; everything else derives.
	if ((ServerTimeNow() - LastDamageServerTime) < TraceHealthRegen::GetDelaySeconds())
	{
		return;
	}

	const float Before = Health;
	Health = FMath::Clamp(Health + TraceHealthRegen::GetRatePerSecond() * DeltaTime, 0.f, MaxHealth);

	const float Applied = Health - Before;
	if (Applied <= 0.f)
	{
		return;
	}

	TotalRegenerated += Applied;
	LastRegenFrame = GFrameCounter;
	LastRegenApplied = Applied;

	// NO OnRep_Health() CALL HERE, and that is deliberate rather than an omission. OnRep_Health's
	// entire job is the alive/dead presentation of the pawn, and regeneration cannot change whether
	// a pawn is alive: it only ever runs on something that already has health above zero, and it
	// only ever adds. Calling it would push a redundant SetDeadPresentation(false) sixty times a
	// second on every hurt pawn in the match. The HUD polls GetHealthPercent() per frame and so
	// needs nothing pushed to it; a remote client's OnRep still fires normally off the replicated
	// float, exactly as it does for damage.
}

void UTraceHealthComponent::ApplyDamage(float Amount, AController* Instigator, FName Cause)
{
	if (!HasAuthority() || !IsAlive())
	{
		return;
	}

	// Network-sourced numbers reach this via UTraceWeaponComponent; validate instead of check().
	if (!FMath::IsFinite(Amount) || Amount <= 0.f)
	{
		return;
	}

	// *** THE RED ARM FOR THE ORDERING CLAIM, AND THE ONLY THING ABOVE THE CARRIER CHECK. ***
	//
	// Shipped is Trace.X.VulnerableApplyOrder 1, so this branch is dead in a shipped process and the
	// amplification happens below, after IsInvulnerable() has returned. Setting the arm to 0 moves the
	// multiply to the wrong side of the carrier rule, which is what makes "applied AFTER the carrier
	// check" a claim a harness can falsify instead of a claim about code somebody has read.
	if (!TraceVulnerable::IsApplyOrderShipped())
	{
		Amount = AmplifyForVulnerable(Amount);
	}

	if (IsInvulnerable())
	{
		// The carrier is bullet-proof. Dropping the hit here (rather than at the weapon) means
		// every future damage source inherits the rule for free.
		//
		// NOTE FOR SPEC v13 §1: this returns BEFORE the regen clock is stamped, and that is the rule
		// rather than an accident of ordering. "Nine seconds without taking damage" — a bullet that
		// bounced off the shield is damage taken by nobody, so a carrier under continuous
		// ineffective fire still heals from the trace hit or the fall that actually hurt them.
		UE_LOG(LogTraceGame, Verbose, TEXT("[%s] Damage %.1f ignored: carrier is invulnerable"),
			*GetNameSafe(GetOwner()), Amount);
		return;
	}

	// *** "TAKING DAMAGE STOPS THIS PROCESS IMMEDIATELY" — THE SAME-FRAME HALF. ***
	//
	// If the ramp already added health on THIS frame, take it back before the damage lands, so the
	// hit cannot leave a frame of healing behind it under any tick ordering.
	//
	// MEASURED, AND IT FIRES. No gameplay damage source should ever reach it — bullets, the knife,
	// the trail and fall damage all resolve inside the world tick, ahead of the ramp's
	// TG_PostPhysics slot — but Trace.Health.RegenTest's interrupting hit is applied from the CORE
	// TICKER, which this engine runs after the world tick, so its hit genuinely lands on a frame the
	// ramp has already healed. That run reports "same-frame rescinds 1" and a delta of exactly
	// 0.000000 across the frame boundary, which is the whole guarantee demonstrated rather than
	// asserted. Expect a non-zero rescind count in Trace.Health.DumpSettings after a self-test and
	// zero after ordinary play; a non-zero count from ordinary play means a damage source has moved
	// into a later tick group and this branch is now the only thing holding the rule up.
	//
	// The clamped amount is rescinded, not the requested one — see LastRegenApplied — so a ramp that
	// ran into the health ceiling does not turn into invented damage.
	if (LastRegenFrame == GFrameCounter && LastRegenApplied > 0.f)
	{
		Health = FMath::Max(0.f, Health - LastRegenApplied);
		TotalRegenerated -= LastRegenApplied;
		++GRegenSameFrameRescinds;

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[%s] Regen rescinded: %.4f HP healed earlier on frame %llu, damage landed on the same frame"),
			*GetNameSafe(GetOwner()), LastRegenApplied, static_cast<uint64>(GFrameCounter));

		LastRegenApplied = 0.f;
	}

	// *** THE ONE PLACE THE VULNERABLE MULTIPLIER IS APPLIED (spec v14 §6, X's passive). ***
	//
	// "+25% damage FROM ALL SOURCES" — bullets, headshots, the knife, Oyster's poison, Pickler and
	// every future ability reach this function and nothing else has to remember. See the block
	// comment at the top of the header for why the mark lives on this component and not on
	// UTraceCharacterAbilitySet::GetIncomingDamageMultiplier().
	//
	// *** POSITION IS LOAD-BEARING: STRICTLY AFTER THE IsInvulnerable() EARLY-OUT ABOVE. *** A Core
	// carrier's damage never reaches this line, so a carrier's zero is produced by the carrier rule
	// and not by an amplifier that happened to multiply zero. Trace.X.VulnerableApplyOrder 0 is the
	// red arm that moves it above and makes the difference observable.
	//
	// The arm MOVES the multiplication, it does not add a second one — hence the guard. Without it
	// the red arm would amplify twice (1.5625x) and would be testing a defect nobody proposed.
	if (TraceVulnerable::IsApplyOrderShipped())
	{
		Amount = AmplifyForVulnerable(Amount);
	}

	// And the clock restarts here, for every hit that actually lands — before the health write, so
	// that even a lethal hit leaves a coherent record behind it.
	LastDamageServerTime = ServerTimeNow();

	Health = FMath::Clamp(Health - Amount, 0.f, GetMaxHealth());

	// Replication callbacks never fire on the authority; call it directly so a listen server's
	// local HUD/effects behave exactly like a remote client's.
	OnRep_Health();

	if (Health <= 0.f)
	{
		BroadcastDeath(Instigator, Cause);
	}
}

void UTraceHealthComponent::Kill(AController* Instigator, FName Cause)
{
	if (!HasAuthority() || !IsAlive())
	{
		return;
	}

	// Deliberately no IsInvulnerable() check: the trail is the counterplay to the carrier, and
	// this is the door it comes through.
	Health = 0.f;
	OnRep_Health();

	BroadcastDeath(Instigator, Cause);
}

void UTraceHealthComponent::ResetHealth()
{
	if (!HasAuthority())
	{
		// Clients get the reset through replication; a local write would only be overwritten.
		return;
	}

	Health = GetMaxHealth();
	bDeathBroadcast = 0;

	// A respawned player starts their regen clock now. At full health nothing reads it, but the
	// alternative — leaving the instant of the death that ended the last life — would let somebody
	// who is hit two seconds after respawning start healing seven seconds early.
	LastDamageServerTime = ServerTimeNow();
	LastRegenApplied = 0.f;

	// A new life is not born marked. ResetHealth() is the respawn/half-time path, and X's mark is a
	// 2 s tactical state, not something to inherit across a death. This is NOT the cooldown rule
	// being violated — spec §5's "cooldowns keep running through death" is about the ACTIVATED
	// ability's cooldown, which lives on the PlayerState and is untouched by anything here.
	ClearVulnerable();

	OnRep_Health();
}

void UTraceHealthComponent::BroadcastDeath(AController* Instigator, FName Cause)
{
	if (bDeathBroadcast)
	{
		return;
	}
	bDeathBroadcast = 1;

	// SPEC v19 §4.2, the health slice's one line of it: "Death wipes active abilities and statuses."
	//
	// X's vulnerable mark is a STATUS and it lives here rather than on the ability set (see the block
	// at the top of the header for why), so this is the only place that can end it at the moment of
	// death. ResetHealth() already cleared it on the way BACK, which covers the respawned pawn; what
	// that could not cover is the corpse, which keeps its replicated mark — and therefore its marker
	// on every client's screen — for the whole respawn delay. A dead player is not "vulnerable"; they
	// are dead.
	//
	// NOT A COOLDOWN, so the rule restated in the same paragraph is untouched: the mark is a 2 s
	// tactical state on a body, and the ability that applies it keeps counting down on X's own
	// PlayerState exactly as before.
	ClearVulnerable();

	UE_LOG(LogTraceGame, Log, TEXT("[%s] died (cause '%s', killer '%s')"),
		*GetNameSafe(GetOwner()), *Cause.ToString(), *GetNameSafe(Instigator));

	OnDeath.Broadcast(GetOwner(), Instigator, Cause);
}

void UTraceHealthComponent::OnRep_Health()
{
	// The HUD polls GetHealthPercent() every frame, so there is nothing to push there. What this
	// hook *is* responsible for is the alive/dead presentation of the pawn.
	//
	// Driving it from replicated health rather than from a death multicast is deliberate: this fires
	// on the server (called by hand after every write), on every connected client, and on a client
	// that joins after the death — because a replicated value that differs from the class default
	// runs its OnRep on initial receive. A multicast would miss that last case and leave a late
	// joiner colliding with a corpse the server has already made intangible.
	if (ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner()))
	{
		OwningCharacter->SetDeadPresentation(!IsAlive());
	}
}

// =================================================================================================
// The vulnerable marker — cosmetic only, local to each machine
// =================================================================================================

void UTraceHealthComponent::UpdateVulnerableMarker()
{
	UWorld* CurrentWorld = GetWorld();
	if (CurrentWorld == nullptr || !CurrentWorld->IsGameWorld())
	{
		return;
	}

	// A dedicated server draws nothing; a listen server draws for its own player like any client.
	if (CurrentWorld->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// Only ATraceCharacters get a marker. The regen fixture can be marked by a harness and there is
	// nothing above its head to hang a ring on.
	ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner());
	if (OwningCharacter == nullptr)
	{
		return;
	}

	const bool bWantMarker = IsVulnerable();
	AActor* Existing = VulnerableMarker.Get();

	if (!bWantMarker)
	{
		// Leave the destroy to the marker's own tick, which also handles the deadline simply passing
		// (an expiry is not a replication event and would never reach an OnRep).
		return;
	}

	if (Existing != nullptr)
	{
		return;   // already up; the deadline moved and the marker re-reads it every tick
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = OwningCharacter;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	ATraceVulnerableMarker* Marker = CurrentWorld->SpawnActor<ATraceVulnerableMarker>(
		ATraceVulnerableMarker::StaticClass(), OwningCharacter->GetActorLocation(), FRotator::ZeroRotator, SpawnParams);
	if (Marker != nullptr)
	{
		Marker->Watching = this;
		VulnerableMarker = Marker;
	}
}

ATraceVulnerableMarker::ATraceVulnerableMarker()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// LOCAL AND COSMETIC. Never replicated: every machine derives it from the replicated deadline,
	// which is one float instead of one actor per marked player per client.
	bReplicates = false;
	SetReplicateMovement(false);

	USceneComponent* MarkerRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(MarkerRoot);
	MarkerRoot->SetMobility(EComponentMobility::Movable);

	Ring = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Ring"));
	Ring->SetupAttachment(MarkerRoot);
	// NO COLLISION. It floats where a head is and must never eat a bullet meant for one.
	Ring->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ring->SetCollisionProfileName(TEXT("NoCollision"));
	Ring->SetGenerateOverlapEvents(false);
	Ring->SetCanEverAffectNavigation(false);
	Ring->SetCastShadow(false);
	Ring->bReceivesDecals = false;
	Ring->SetRelativeScale3D(FVector(0.55f, 0.55f, 0.06f));

	// Constructor-time finders, the same policy TraceCore uses: engine assets referenced this way
	// cook into a packaged build, a runtime LoadObject would resolve to null once cooked.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		Ring->SetStaticMesh(CylinderFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		BaseMaterial = NeonFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BaseMaterial == nullptr && BasicFinder.Succeeded())
	{
		BaseMaterial = BasicFinder.Object;
	}
}

void ATraceVulnerableMarker::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const UTraceHealthComponent* Watched = Watching.Get();
	if (Watched == nullptr || !Watched->IsVulnerable() || !Watched->IsAlive())
	{
		// THE EXPIRY PATH. A deadline passing is not a replication event, so nothing will ever call
		// an OnRep to tear this down — the marker retires itself.
		Destroy();
		return;
	}

	const AActor* MarkedActor = Watched->GetOwner();
	const UWorld* MarkerWorld = GetWorld();
	if (MarkedActor == nullptr || MarkerWorld == nullptr)
	{
		Destroy();
		return;
	}

	// Follow the head, spin, and pulse faster as the 2 s runs out so the mark reads as a timer.
	const float Remaining = Watched->GetVulnerableRemaining();
	const float Total = FMath::Max(0.01f, TraceVulnerable::GetDurationSeconds());
	const float Fraction = FMath::Clamp(Remaining / Total, 0.f, 1.f);
	const float LocalNow = static_cast<float>(MarkerWorld->GetTimeSeconds());

	// NOT named "Location": AActor has had a member by that name in this engine's lineage, and MSVC
	// errors on a local shadowing an enclosing class's member where clang says nothing. That exact
	// mistake has broken the Windows build in this project three times.
	FVector MarkerLocation = MarkedActor->GetActorLocation();
	MarkerLocation.Z += 118.f + 6.f * FMath::Sin(LocalNow * 6.f);
	SetActorLocation(MarkerLocation);
	SetActorRotation(FRotator(0.f, LocalNow * 220.f, 0.f));

	if (Ring != nullptr)
	{
		if (RingMID == nullptr && BaseMaterial != nullptr)
		{
			RingMID = Ring->CreateDynamicMaterialInstance(0, BaseMaterial);
		}
		if (RingMID != nullptr)
		{
			// Amber -> red as it expires. Both parameter names are set because the neon material and
			// the engine basic material do not agree on one; the absent one is a no-op.
			const FLinearColor Colour = FMath::Lerp(FLinearColor(1.f, 0.15f, 0.05f, 1.f),
			                                        FLinearColor(1.f, 0.75f, 0.05f, 1.f), Fraction);
			RingMID->SetVectorParameterValue(TEXT("Color"), Colour);
			RingMID->SetVectorParameterValue(TEXT("BaseColor"), Colour);
		}
	}
}

// =================================================================================================
// Regeneration test fixture
// =================================================================================================

ATraceHealthRegenFixture::ATraceHealthRegenFixture()
{
	PrimaryActorTick.bCanEverTick = false;

	// A root has to exist for SetActorLocation to place it, and an actor with no root is also an
	// actor with no way to be positioned out of the arena — which is the fixture's entire premise.
	USceneComponent* FixtureRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(FixtureRoot);

	bReplicates = true;
	SetReplicateMovement(false);

	// ALWAYS RELEVANT, and it has to be: the fixture sits 20000 uu above the field precisely so no
	// player can reach it, which is also exactly the condition under which normal distance-based
	// relevancy would stop replicating it and leave the joined client watching a frozen number.
	bAlwaysRelevant = true;

	Health = CreateDefaultSubobject<UTraceHealthComponent>(TEXT("Health"));
}

// =================================================================================================
// Dev console: tuning read-back, the regeneration self-test, and the two-process observation aids.
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace
{
	/** The one fixture in the world, spawned on demand and reused across commands. */
	ATraceHealthRegenFixture* FindOrSpawnRegenFixture(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}

		for (TActorIterator<ATraceHealthRegenFixture> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				return *It;
			}
		}

		if (World->GetNetMode() == NM_Client)
		{
			// A client cannot conjure a server-authoritative actor, and silently spawning a local one
			// would give the watcher a number that looks right and means nothing.
			return nullptr;
		}

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;

		// Well above the arena. Out of every weapon's way, out of every bot's target list, and out
		// of the trace's — so the only thing that ever damages it is the test.
		return World->SpawnActor<ATraceHealthRegenFixture>(
			ATraceHealthRegenFixture::StaticClass(), FVector(0.f, 0.f, 20000.f), FRotator::ZeroRotator, Params);
	}

	/** Every alive player pawn's health component on this machine. Used by Trace.Health.Hurt. */
	void CollectPlayerHealth(UWorld* World, TArray<UTraceHealthComponent*>& Out)
	{
		if (World == nullptr)
		{
			return;
		}

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Character = *It;
			if (IsValid(Character) && Character->Health != nullptr && Character->Health->IsAlive())
			{
				Out.Add(Character->Health);
			}
		}
	}

	/**
	 * The self-test's state, held by shared pointer so the per-frame ticker owns it and it dies with
	 * the ticker rather than living on as a file static between runs.
	 */
	struct FRegenTestState
	{
		enum class EPhase : uint8
		{
			WaitForFirstGain,
			MeasureRate,
			Interrupt,
			ConfirmStopped,
			WaitForResume,
			Done
		};

		TWeakObjectPtr<ATraceHealthRegenFixture> Fixture;
		EPhase Phase = EPhase::WaitForFirstGain;

		float ConfiguredDelay = 0.f;
		float ConfiguredRate = 0.f;

		// Phase 1 — delay from damage to the first health gained.
		float DamageTime = 0.f;
		float HealthAtDamage = 0.f;
		float FirstGainTime = -1.f;
		float FirstGainHealth = 0.f;
		float MeasuredDelay = -1.f;

		// Phase 2 — the rate, measured over a window that starts after the first gain.
		float RateWindowStartTime = -1.f;
		float RateWindowStartHealth = 0.f;
		float MeasuredRate = -1.f;
		static constexpr float RateWindowSeconds = 2.f;

		// Phase 3/4 — the interruption, and the same-frame proof.
		uint64 InterruptFrame = 0;
		float InterruptTime = 0.f;
		float HealthRightAfterInterrupt = 0.f;
		float HealthOnNextFrame = -1.f;
		uint64 NextFrameSeen = 0;
		float MaxHealthDuringHold = -1.f;
		int32 RescindsAtInterrupt = 0;
		static constexpr float HoldSeconds = 1.5f;

		// Phase 5 — that the clock RESET rather than paused.
		float SecondGainTime = -1.f;
		float MeasuredResumeDelay = -1.f;

		TArray<FString> Failures;
	};

	void LogRegenTestResult(const TSharedRef<FRegenTestState>& State)
	{
		const float DelayError = (State->MeasuredDelay >= 0.f)
			? FMath::Abs(State->MeasuredDelay - State->ConfiguredDelay) : -1.f;
		const float RateError = (State->MeasuredRate >= 0.f)
			? FMath::Abs(State->MeasuredRate - State->ConfiguredRate) : -1.f;
		const float ResumeError = (State->MeasuredResumeDelay >= 0.f)
			? FMath::Abs(State->MeasuredResumeDelay - State->ConfiguredDelay) : -1.f;

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE HEALTH REGEN SELF-TEST =========="));
		UE_LOG(LogTraceGame, Display, TEXT("[RegenTest] configured   : enabled=%d delay=%.3fs rate=%.3f HP/s"),
			TraceHealthRegen::IsEnabled() ? 1 : 0, State->ConfiguredDelay, State->ConfiguredRate);
		UE_LOG(LogTraceGame, Display, TEXT("[RegenTest] 1 first heal  : %.3fs after damage (want %.3f, err %.3f)"),
			State->MeasuredDelay, State->ConfiguredDelay, DelayError);
		UE_LOG(LogTraceGame, Display, TEXT("[RegenTest] 2 rate        : %.3f HP/s over %.1fs (want %.3f, err %.3f)"),
			State->MeasuredRate, FRegenTestState::RateWindowSeconds, State->ConfiguredRate, RateError);
		UE_LOG(LogTraceGame, Display,
			TEXT("[RegenTest] 3 same frame  : hit on frame %llu left health %.4f; frame %llu read %.4f (delta %.6f); ")
			TEXT("peak over the next %.1fs %.4f; same-frame rescinds %d"),
			State->InterruptFrame, State->HealthRightAfterInterrupt,
			State->NextFrameSeen, State->HealthOnNextFrame,
			State->HealthOnNextFrame - State->HealthRightAfterInterrupt,
			FRegenTestState::HoldSeconds, State->MaxHealthDuringHold,
			TraceHealthRegen::GetSameFrameRescindCount() - State->RescindsAtInterrupt);
		UE_LOG(LogTraceGame, Display, TEXT("[RegenTest] 4 clock reset : regen resumed %.3fs after the interrupting hit (want %.3f, err %.3f)"),
			State->MeasuredResumeDelay, State->ConfiguredDelay, ResumeError);

		for (const FString& Failure : State->Failures)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[RegenTest] FAIL: %s"), *Failure);
		}

		UE_LOG(LogTraceGame, Display, TEXT("[RegenTest] RESULT: %s (%d failure(s))"),
			State->Failures.Num() == 0 ? TEXT("PASS") : TEXT("FAIL"), State->Failures.Num());
		UE_LOG(LogTraceGame, Display, TEXT("================================================="));
	}
}

/**
 * Trace.Health.RegenTest — the measurement this feature's report is made of.
 *
 * *** IT MUST BE ABLE TO GO RED, AND HERE IS HOW TO MAKE IT. ***
 * Run it once with `Trace.Health.Regen 0` and once with `Trace.Health.Regen 1`. At 0 phase 1 times
 * out and the command reports FAIL with "never regenerated"; at 1 it reports the four numbers below.
 * A harness that has only ever been seen passing is not evidence, and this project has been bitten
 * by exactly that twice.
 *
 * Four things measured, in one uninterrupted run against one fixture:
 *   1. seconds from the damage that landed to the FIRST health gained;
 *   2. the rate, over a two-second window taken after the ramp is running;
 *   3. that a hit mid-regen leaves NO healing behind it on its own frame — the damage is applied
 *      from the core ticker, which runs before the world tick, so the component's TG_PostPhysics
 *      regen tick still gets its chance on that same frame and must decline it. The check is made
 *      on the FOLLOWING frame, which is the first moment frame N is finished and readable;
 *   4. that the interrupting hit RESET the clock rather than pausing it — the second wait must be
 *      another full delay, not the remainder of the first.
 */
static void TraceRunRegenSelfTest(UWorld* World)
{
	if (World == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[RegenTest] No world."));
		return;
	}

	if (World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[RegenTest] Refusing to run on a CLIENT: health is server-authoritative, so every number ")
			TEXT("this would print is a replication artefact. Run it on the listen server and watch the client ")
			TEXT("with Trace.Health.Watch."));
		return;
	}

	ATraceHealthRegenFixture* Fixture = FindOrSpawnRegenFixture(World);
	if (Fixture == nullptr || Fixture->Health == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[RegenTest] Could not spawn the fixture."));
		return;
	}

	UTraceHealthComponent* FixtureHealth = Fixture->Health;
	FixtureHealth->ResetHealth();

	TSharedRef<FRegenTestState> State = MakeShared<FRegenTestState>();
	State->Fixture = Fixture;
	State->ConfiguredDelay = TraceHealthRegen::GetDelaySeconds();
	State->ConfiguredRate = TraceHealthRegen::GetRatePerSecond();

	// Half the bar, so there is room to climb for the whole measurement without hitting the ceiling:
	// 50 HP at 10 HP/s is five seconds of headroom against a two-second rate window.
	const float OpeningDamage = FMath::Max(1.f, FixtureHealth->Health * 0.5f);
	FixtureHealth->ApplyDamage(OpeningDamage, nullptr, FName(TEXT("RegenTest")));

	State->DamageTime = World->GetTimeSeconds();
	State->HealthAtDamage = FixtureHealth->Health;

	UE_LOG(LogTraceGame, Display,
		TEXT("[RegenTest] Armed. Fixture at %.1f HP after %.1f damage; expecting the first heal in %.2fs, then %.1f HP/s. ")
		TEXT("Total run ~%.0fs."),
		State->HealthAtDamage, OpeningDamage, State->ConfiguredDelay, State->ConfiguredRate,
		2.f * State->ConfiguredDelay + FRegenTestState::RateWindowSeconds + FRegenTestState::HoldSeconds + 4.f);

	TWeakObjectPtr<UWorld> WeakWorld(World);

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[State, WeakWorld](float /*Delta*/) -> bool
	{
		UWorld* TickWorld = WeakWorld.Get();
		ATraceHealthRegenFixture* TickFixture = State->Fixture.Get();
		if (TickWorld == nullptr || TickFixture == nullptr || TickFixture->Health == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[RegenTest] Fixture or world went away mid-run; result is inconclusive."));
			return false;
		}

		UTraceHealthComponent* TickHealth = TickFixture->Health;
		const float Now = TickWorld->GetTimeSeconds();
		const float H = TickHealth->Health;

		switch (State->Phase)
		{
		case FRegenTestState::EPhase::WaitForFirstGain:
		{
			if (H > State->HealthAtDamage + 1e-4f)
			{
				State->FirstGainTime = Now;
				State->FirstGainHealth = H;
				State->MeasuredDelay = Now - State->DamageTime;
				State->RateWindowStartTime = Now;
				State->RateWindowStartHealth = H;
				State->Phase = FRegenTestState::EPhase::MeasureRate;
			}
			else if ((Now - State->DamageTime) > (State->ConfiguredDelay + 5.f))
			{
				State->Failures.Add(FString::Printf(
					TEXT("never regenerated: still %.2f HP %.1fs after taking damage (delay is %.2fs)"),
					H, Now - State->DamageTime, State->ConfiguredDelay));
				State->Phase = FRegenTestState::EPhase::Done;
				LogRegenTestResult(State);
				return false;
			}
			break;
		}

		case FRegenTestState::EPhase::MeasureRate:
		{
			if ((Now - State->RateWindowStartTime) >= FRegenTestState::RateWindowSeconds)
			{
				const float Elapsed = Now - State->RateWindowStartTime;
				State->MeasuredRate = (H - State->RateWindowStartHealth) / FMath::Max(1e-4f, Elapsed);
				State->Phase = FRegenTestState::EPhase::Interrupt;
			}
			break;
		}

		case FRegenTestState::EPhase::Interrupt:
		{
			// The hit lands here, from the core ticker, i.e. BEFORE this frame's world tick — so the
			// component's regen tick still runs on this very frame and has every opportunity to add
			// the frame of healing this test exists to rule out.
			State->RescindsAtInterrupt = TraceHealthRegen::GetSameFrameRescindCount();
			State->InterruptFrame = GFrameCounter;
			State->InterruptTime = Now;
			TickHealth->ApplyDamage(5.f, nullptr, FName(TEXT("RegenTestInterrupt")));
			State->HealthRightAfterInterrupt = TickHealth->Health;
			State->MaxHealthDuringHold = TickHealth->Health;

			UE_LOG(LogTraceGame, Display,
				TEXT("[RegenTest] Interrupt: 5 damage on frame %llu, health %.4f -> %.4f. Watching for %.1fs."),
				State->InterruptFrame, H, State->HealthRightAfterInterrupt, FRegenTestState::HoldSeconds);

			State->Phase = FRegenTestState::EPhase::ConfirmStopped;
			break;
		}

		case FRegenTestState::EPhase::ConfirmStopped:
		{
			// The first tick after the interrupt is the first moment the interrupted frame is
			// complete. Whatever the regen tick did on that frame is now visible, and it must have
			// done nothing at all.
			if (State->HealthOnNextFrame < 0.f)
			{
				State->HealthOnNextFrame = H;
				State->NextFrameSeen = GFrameCounter;

				if (H > State->HealthRightAfterInterrupt + 1e-6f)
				{
					State->Failures.Add(FString::Printf(
						TEXT("SAME-FRAME LEAK: health was %.6f right after the hit on frame %llu and %.6f when ")
						TEXT("frame %llu came round — the hit left %.6f HP of healing behind it"),
						State->HealthRightAfterInterrupt, State->InterruptFrame, H, State->NextFrameSeen,
						H - State->HealthRightAfterInterrupt));
				}
			}

			State->MaxHealthDuringHold = FMath::Max(State->MaxHealthDuringHold, H);

			if ((Now - State->InterruptTime) >= FRegenTestState::HoldSeconds)
			{
				if (State->MaxHealthDuringHold > State->HealthRightAfterInterrupt + 1e-6f)
				{
					State->Failures.Add(FString::Printf(
						TEXT("regen did not stop: health peaked at %.4f during the %.1fs after the hit, ")
						TEXT("against %.4f at the moment of the hit"),
						State->MaxHealthDuringHold, FRegenTestState::HoldSeconds, State->HealthRightAfterInterrupt));
				}
				State->Phase = FRegenTestState::EPhase::WaitForResume;
			}
			break;
		}

		case FRegenTestState::EPhase::WaitForResume:
		{
			if (H > State->HealthRightAfterInterrupt + 1e-4f)
			{
				State->SecondGainTime = Now;
				State->MeasuredResumeDelay = Now - State->InterruptTime;

				if (State->MeasuredResumeDelay < State->ConfiguredDelay * 0.9f)
				{
					State->Failures.Add(FString::Printf(
						TEXT("the hit PAUSED the clock instead of RESETTING it: regen resumed %.3fs after the hit, ")
						TEXT("against a configured delay of %.3fs"),
						State->MeasuredResumeDelay, State->ConfiguredDelay));
				}

				State->Phase = FRegenTestState::EPhase::Done;
				LogRegenTestResult(State);
				return false;
			}

			if ((Now - State->InterruptTime) > (State->ConfiguredDelay + 5.f))
			{
				State->Failures.Add(FString::Printf(
					TEXT("regen never resumed: still %.2f HP %.1fs after the interrupting hit"),
					H, Now - State->InterruptTime));
				State->Phase = FRegenTestState::EPhase::Done;
				LogRegenTestResult(State);
				return false;
			}
			break;
		}

		default:
			return false;
		}

		return true;
	}), 0.f);
}

static FAutoConsoleCommandWithWorld CmdHealthRegenTest(
	TEXT("Trace.Health.RegenTest"),
	TEXT("Dev only, SERVER. Measure the regen delay, the rate, that a mid-regen hit stops it on the same frame, "
	     "and that the hit resets the clock. Run it with Trace.Health.Regen 0 first to see it go RED."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&TraceRunRegenSelfTest));

/**
 * Trace.Health.DumpSettings — the answer to "verify it from a running game, never by reading the
 * header".
 *
 * Trace.DumpSettings and Trace.VerifyKnobs BOTH reach this page now (by /Script path, the same way
 * they already reached UTraceMeleeSettings), so this command is no longer the only thing that can
 * see these three knobs. It is still the more useful one: it prints the RESOLVED value beside the
 * table value and the console override, which is what tells you whether a number came from the ini
 * or from somebody's CVar.
 */
static FAutoConsoleCommandWithWorld CmdHealthDumpSettings(
	TEXT("Trace.Health.DumpSettings"),
	TEXT("Dev only. Log the health/regeneration values this process resolves RIGHT NOW, overrides included."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		const UTraceHealthSettings& Table = UTraceHealthSettings::Get();
		const float Max = FMath::Max(1.f, UTraceSettings::Get().MaxHealth);
		const float Rate = TraceHealthRegen::GetRatePerSecond();

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE HEALTH SETTINGS =========="));
		UE_LOG(LogTraceGame, Display, TEXT("HEALTH  max        : %.1f (UTraceSettings::MaxHealth)"), Max);
		UE_LOG(LogTraceGame, Display, TEXT("REGEN   enabled    : %d (setting %d, CVar Trace.Health.Regen %d)"),
			TraceHealthRegen::IsEnabled() ? 1 : 0, Table.bRegenEnabled ? 1 : 0, CVarHealthRegen.GetValueOnAnyThread());
		UE_LOG(LogTraceGame, Display, TEXT("REGEN   delay      : %.3fs (table %.3f, override %.3f)"),
			TraceHealthRegen::GetDelaySeconds(), Table.RegenDelaySeconds, CVarHealthRegenDelay.GetValueOnAnyThread());
		UE_LOG(LogTraceGame, Display, TEXT("REGEN   rate       : %.3f HP/s (table %.3f, override %.3f) => 0 -> %.0f in %.2fs"),
			Rate, Table.RegenRatePerSecond, CVarHealthRegenRate.GetValueOnAnyThread(), Max, Max / Rate);
		UE_LOG(LogTraceGame, Display, TEXT("REGEN   rescinds   : %d heal(s) taken back by same-frame damage since launch"),
			TraceHealthRegen::GetSameFrameRescindCount());
		UE_LOG(LogTraceGame, Display,
			TEXT("VULN    (v14 §6 X): enabled=%d multiplier=x%.3f duration=%.2fs | applyOrder=%s carrierImmune=%d"),
			TraceVulnerable::IsEnabled() ? 1 : 0, TraceVulnerable::GetDamageMultiplier(),
			TraceVulnerable::GetDurationSeconds(),
			TraceVulnerable::IsApplyOrderShipped() ? TEXT("SHIPPED (after carrier check)") : TEXT("*** RED (before) ***"),
			TraceVulnerable::IsCarrierImmune() ? 1 : 0);
		UE_LOG(LogTraceGame, Display,
			TEXT("VULN    stacking   : enabled=%d, +%.0f%% per extra stack, cap %d => x%.3f / x%.3f / x%.3f ... x%.3f (v16 §4)"),
			TraceVulnerable::IsStackingEnabled() ? 1 : 0, TraceVulnerable::GetStackBonus() * 100.f,
			TraceVulnerable::GetMaxStacks(),
			TraceVulnerable::GetMultiplierForStacks(1), TraceVulnerable::GetMultiplierForStacks(2),
			TraceVulnerable::GetMultiplierForStacks(3),
			TraceVulnerable::GetMultiplierForStacks(TraceVulnerable::GetMaxStacks()));
		UE_LOG(LogTraceGame, Display,
			TEXT("VULN    alarms     : carrierMarked=%d carrierAmplified=%d (both MUST be 0) | marks=%d amplifiedHits=%d"),
			TraceVulnerable::GetCarrierMarkedCount(), TraceVulnerable::GetCarrierAmplifiedCount(),
			TraceVulnerable::GetMarkAppliedCount(), TraceVulnerable::GetAmplifiedHitCount());
		UE_LOG(LogTraceGame, Display, TEXT("NET     mode       : %d (0 standalone, 1 dedicated, 2 listen, 3 client)"),
			World != nullptr ? static_cast<int32>(World->GetNetMode()) : -1);
		UE_LOG(LogTraceGame, Display, TEXT("=========================================="));
	}));

/**
 * Trace.Health.Hurt <amount> [delaySeconds] — SERVER. Damage the regen fixture and every alive
 * player pawn, optionally after a delay.
 *
 * The delay argument is what makes the two-process client test a single -TraceExec line: the host
 * runs the self-test immediately and schedules the hit the joined client will watch heal back.
 */
static FAutoConsoleCommandWithWorldAndArgs CmdHealthHurt(
	TEXT("Trace.Health.Hurt"),
	TEXT("Dev only, SERVER. Trace.Health.Hurt <amount> [delaySeconds]: damage the regen fixture and every alive pawn."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr || World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[Health.Hurt] Server only."));
			return;
		}

		const float Amount = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 40.f;
		const float Delay = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : 0.f;

		TWeakObjectPtr<UWorld> WeakWorld(World);
		auto Strike = [WeakWorld, Amount]()
		{
			UWorld* StrikeWorld = WeakWorld.Get();
			if (StrikeWorld == nullptr)
			{
				return;
			}

			TArray<UTraceHealthComponent*> Targets;
			if (ATraceHealthRegenFixture* Fixture = FindOrSpawnRegenFixture(StrikeWorld))
			{
				if (Fixture->Health != nullptr)
				{
					Targets.Add(Fixture->Health);
				}
			}
			CollectPlayerHealth(StrikeWorld, Targets);

			for (UTraceHealthComponent* Target : Targets)
			{
				Target->ApplyDamage(Amount, nullptr, FName(TEXT("DevHurt")));
			}

			UE_LOG(LogTraceGame, Display, TEXT("[Health.Hurt] Applied %.1f to %d target(s) at t=%.2f."),
				Amount, Targets.Num(), StrikeWorld->GetTimeSeconds());
		};

		if (Delay <= 0.f)
		{
			Strike();
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[Health.Hurt] %.1f damage scheduled in %.1fs."), Amount, Delay);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Strike](float) -> bool
		{
			Strike();
			return false;
		}), Delay);
	}));

/**
 * Trace.Health.Watch [seconds] [interval] — runs anywhere, and is meant for the CLIENT.
 *
 * Logs the replicated health of the regen fixture and of this machine's own pawn on a fixed
 * cadence, with the regen state each one derives locally. On a joined client under
 * NetEmulation.PktLag this is the proof that the bar moves on a client at all: the numbers in its
 * log are the ones its HUD is drawing, arriving purely by replication with no local prediction.
 */
static FAutoConsoleCommandWithWorldAndArgs CmdHealthWatch(
	TEXT("Trace.Health.Watch"),
	TEXT("Dev only. Trace.Health.Watch [seconds] [interval]: log replicated health of the regen fixture and the local pawn."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}

		const float Seconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 1.f, 300.f) : 30.f;
		const float Interval = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.05f, 5.f) : 0.25f;

		TWeakObjectPtr<UWorld> WeakWorld(World);
		const float EndTime = World->GetTimeSeconds() + Seconds;

		UE_LOG(LogTraceGame, Display, TEXT("[Health.Watch] Watching for %.1fs every %.2fs (netmode %d)."),
			Seconds, Interval, static_cast<int32>(World->GetNetMode()));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakWorld, EndTime](float) -> bool
		{
			UWorld* WatchWorld = WeakWorld.Get();
			if (WatchWorld == nullptr)
			{
				return false;
			}

			float FixtureHP = -1.f;
			float FixtureUntil = -1.f;
			int32 bFixtureRegen = 0;
			for (TActorIterator<ATraceHealthRegenFixture> It(WatchWorld); It; ++It)
			{
				if (IsValid(*It) && It->Health != nullptr)
				{
					FixtureHP = It->Health->Health;
					FixtureUntil = It->Health->GetSecondsUntilRegen();
					bFixtureRegen = It->Health->IsRegenerating() ? 1 : 0;
					break;
				}
			}

			float PawnHP = -1.f;
			float PawnUntil = -1.f;
			int32 bPawnRegen = 0;
			if (const APlayerController* PC = WatchWorld->GetFirstPlayerController())
			{
				if (const ATraceCharacter* Character = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					if (Character->Health != nullptr)
					{
						PawnHP = Character->Health->Health;
						PawnUntil = Character->Health->GetSecondsUntilRegen();
						bPawnRegen = Character->Health->IsRegenerating() ? 1 : 0;
					}
				}
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[Health.Watch] t=%.2f | fixture %.2f HP regen=%d in %.2fs | pawn %.2f HP regen=%d in %.2fs"),
				WatchWorld->GetTimeSeconds(), FixtureHP, bFixtureRegen, FixtureUntil, PawnHP, bPawnRegen, PawnUntil);

			return WatchWorld->GetTimeSeconds() < EndTime;
		}), Interval);
	}));

// =================================================================================================
// SPEC v16 §4 — VULNERABLE STACKS, AND THE CARRIER STILL BEING IMMUNE TO ALL OF IT
//
//   Trace.X.VulnerableStackTest   the arithmetic (x1.25 / x1.30 / x1.35), the cap, the single
//                                 refreshed timer, and "all stacks disappear together". RED ARM
//                                 FIRST: with Trace.X.VulnerableStacking 0 the same three marks
//                                 measure x1.25, so a green run is a measurement of the stacking
//                                 code rather than of the multiplier that predates it.
//
//   Trace.X.StackCarrierTest      *** THE §4 INVARIANT, RE-PROVEN WITH STACKING IN PLAY. *** Three
//                                 marks aimed at the Core carrier. RED ARM (Trace.X.
//                                 VulnerableCarrierImmune 0) must show the carrier reaching three
//                                 stacks and x1.35 and moving both alarms; the shipped arm must
//                                 show 0 stacks, x1.000 and neither alarm moving. Plus a CONTROL on
//                                 a NON-carrier taking the identical three marks and really losing
//                                 the amplified health — without it the run cannot tell "the carrier
//                                 rule held" from "the fixture never fired", which is the failure
//                                 this project has been bitten by more than once.
//
// WHY THE STACK TEST USES THE REGEN FIXTURE AND THE CARRIER TEST USES REAL PAWNS. The arithmetic
// needs a health component nothing else in the match can touch — a bot landing a body shot between
// two measurements would be indistinguishable from a broken multiplier — and the fixture sits
// 20000 uu above the field for exactly that reason. The carrier rule cannot be tested on it at all:
// the locks are `Cast<ATraceCharacter>` on the owner, so a fixture would pass every one of them
// vacuously, having never been a character in the first place.
// =================================================================================================

namespace TraceVulnerableStackTest
{
	/** 40 is the body shot, and 40 x 1.35 = 54 — three numbers nobody can confuse with each other. */
	constexpr float StackTestDamage = 40.f;

	const FName StackTestCause(TEXT("VulnStackVerify"));

	void SetArm(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** Every arm this file's two commands touch, back to shipped. Called on EVERY exit path. */
	void RestoreArms()
	{
		SetArm(TEXT("Trace.X.Vulnerable"), 1);
		SetArm(TEXT("Trace.X.VulnerableStacking"), 1);
		SetArm(TEXT("Trace.X.VulnerableApplyOrder"), 1);
		SetArm(TEXT("Trace.X.VulnerableCarrierImmune"), 1);
		SetArm(TEXT("Trace.Ability.CarrierImmune"), 1);
	}

	/**
	 * Applies @p Amount and returns what was actually taken off.
	 *
	 * Read-apply-read with NOTHING in between — no tick, no await — for the reason TraceXVerify's
	 * header gives: a bot shooting the subject between two reads would look exactly like a broken
	 * multiplier. ApplyDamage is synchronous, so this is a measurement rather than an anecdote.
	 */
	float MeasureDamage(UTraceHealthComponent* Target, float Amount)
	{
		if (Target == nullptr)
		{
			return -1.f;
		}
		const float Before = Target->Health;
		Target->ApplyDamage(Amount, nullptr, StackTestCause);
		return Before - Target->Health;
	}

	/** The same checklist shape TraceXVerify uses, so a FAIL is impossible to skim past. */
	struct FChecklist
	{
		const TCHAR* Tag = TEXT("VULNSTACK");
		int32 Passed = 0;
		int32 Failed = 0;
		bool  bInvalid = false;
		FString InvalidReason;

		void Check(bool bCondition, const FString& Name, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[%s] %s  %s  |  %s"),
				Tag, bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *Name, *Detail);
		}

		void Invalidate(const FString& Reason)
		{
			bInvalid = true;
			InvalidReason = Reason;
		}

		void Report()
		{
			if (bInvalid)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: INVALID — %s (%d passed, %d failed)"),
					Tag, *InvalidReason, Passed, Failed);
			}
			else if (Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[%s] VERDICT: PASS — %d checks, 0 failed."), Tag, Passed);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d passed, %d FAILED."),
					Tag, Passed, Failed);
			}
		}
	};

	/** Marks @p Target @p Count times through the shipping entry point. Returns the resulting stacks. */
	int32 MarkTimes(UTraceHealthComponent* Target, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Target->ApplyVulnerable(TraceVulnerable::GetDurationSeconds(), nullptr);
		}
		return Target->GetVulnerableStacks();
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.X.VulnerableStackTest
	// ---------------------------------------------------------------------------------------------

	struct FStackState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		FChecklist List;
		TWeakObjectPtr<ATraceHealthRegenFixture> Fixture;

		/** The red arm's measurement, kept so the verdict can invalidate a run that never went red. */
		float RedThreeMarkDelta = -1.f;
		bool  bRedReproduced = false;
	};

	void RunStackTest(UWorld* World)
	{
		if (World == nullptr || World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[VULNSTACK] Server only — the mark is applied on the authority and arrives on a client "
				     "by replication, so a client run would measure nothing."));
			return;
		}

		ATraceHealthRegenFixture* Fixture = FindOrSpawnRegenFixture(World);
		if (Fixture == nullptr || Fixture->Health == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[VULNSTACK] could not obtain the health fixture."));
			return;
		}

		TSharedPtr<FStackState> State = MakeShared<FStackState>();
		State->Fixture = Fixture;

		UE_LOG(LogTraceGame, Display,
			TEXT("[VULNSTACK] ===== spec v16 §4: 'vulnerable stacks with each hit. The first stack still causes 25%% "
			     "extra damage, but each additional stack only adds 5%%. Whenever the timer runs out, all stacks "
			     "disappear.' Resolved: cap %d, x%.3f / x%.3f / x%.3f, duration %.2fs. arm 0 = RED "
			     "(Trace.X.VulnerableStacking 0) must measure %.2f for three marks; arm 1 = shipped must measure "
			     "%.2f. ====="),
			TraceVulnerable::GetMaxStacks(),
			TraceVulnerable::GetMultiplierForStacks(1), TraceVulnerable::GetMultiplierForStacks(2),
			TraceVulnerable::GetMultiplierForStacks(3), TraceVulnerable::GetDurationSeconds(),
			StackTestDamage * TraceVulnerable::GetMultiplierForStacks(1),
			StackTestDamage * TraceVulnerable::GetMultiplierForStacks(3));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float) -> bool
		{
			const double NowReal = FPlatformTime::Seconds();
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			ATraceHealthRegenFixture* Subject = State->Fixture.Get();
			if (Subject == nullptr || Subject->Health == nullptr)
			{
				State->List.Invalidate(TEXT("the fixture went away mid-test"));
				State->List.Report();
				RestoreArms();
				return false;
			}
			UTraceHealthComponent* Health = Subject->Health;

			// ---- step 0: THE RED ARM. Stacking off; the mark still lands and still amplifies. ----
			if (State->Step == 0)
			{
				SetArm(TEXT("Trace.X.VulnerableStacking"), 0);

				Health->ResetHealth();                     // ResetHealth clears the mark, so mark AFTER
				const int32 RedStacks = MarkTimes(Health, 3);
				const float RedMultiplier = Health->GetVulnerableDamageMultiplier();
				State->RedThreeMarkDelta = MeasureDamage(Health, StackTestDamage);

				const float OneStackExpected = StackTestDamage * TraceVulnerable::GetMultiplierForStacks(1);
				State->bRedReproduced = (RedStacks == 1)
					&& FMath::IsNearlyEqual(State->RedThreeMarkDelta, OneStackExpected, 0.01f);

				State->List.Check(RedStacks == 1,
					TEXT("RED ARM: with stacking disarmed, three marks are still ONE stack"),
					FString::Printf(TEXT("stacks=%d multiplier=x%.3f — this is what a build without §4 does"),
						RedStacks, RedMultiplier));

				State->List.Check(FMath::IsNearlyEqual(State->RedThreeMarkDelta, OneStackExpected, 0.01f),
					TEXT("RED ARM: three marks measure the UNSTACKED number"),
					FString::Printf(TEXT("took %.2f, expected %.2f (the stacked answer would be %.2f)"),
						State->RedThreeMarkDelta, OneStackExpected,
						StackTestDamage * TraceVulnerable::GetMultiplierForStacks(3)));

				SetArm(TEXT("Trace.X.VulnerableStacking"), 1);
				State->Step = 1;
				return true;
			}

			// ---- step 1: THE GREEN ARM. The identical calls, one arm different. ----
			if (State->Step == 1)
			{
				// One mark. Must be EXACTLY what the pre-v16 build did — §4 changed what happens on the
				// SECOND hit, and a first hit that moved would be a regression dressed as a feature.
				Health->ResetHealth();
				const int32 OneStack = MarkTimes(Health, 1);
				const float OneDelta = MeasureDamage(Health, StackTestDamage);
				const float OneExpected = StackTestDamage * TraceVulnerable::GetMultiplierForStacks(1);

				State->List.Check(OneStack == 1 && FMath::IsNearlyEqual(OneDelta, OneExpected, 0.01f),
					TEXT("1 stack: 'the first stack still causes 25% extra damage'"),
					FString::Printf(TEXT("stacks=%d, took %.2f, expected %.2f (x%.3f)"),
						OneStack, OneDelta, OneExpected, TraceVulnerable::GetMultiplierForStacks(1)));

				// Two.
				Health->ResetHealth();
				const int32 TwoStacks = MarkTimes(Health, 2);
				const float TwoDelta = MeasureDamage(Health, StackTestDamage);
				const float TwoExpected = StackTestDamage * TraceVulnerable::GetMultiplierForStacks(2);

				State->List.Check(TwoStacks == 2 && FMath::IsNearlyEqual(TwoDelta, TwoExpected, 0.01f),
					TEXT("2 stacks: 'each additional stack only adds 5%'"),
					FString::Printf(TEXT("stacks=%d, took %.2f, expected %.2f (x%.3f) — NOT %.2f, which is what "
					                     "a second FULL 25%% would give"),
						TwoStacks, TwoDelta, TwoExpected, TraceVulnerable::GetMultiplierForStacks(2),
						StackTestDamage * (TraceVulnerable::GetDamageMultiplier() * 2.f - 1.f)));

				// Three.
				Health->ResetHealth();
				const int32 ThreeStacks = MarkTimes(Health, 3);
				const float ThreeDelta = MeasureDamage(Health, StackTestDamage);
				const float ThreeExpected = StackTestDamage * TraceVulnerable::GetMultiplierForStacks(3);

				State->List.Check(ThreeStacks == 3 && FMath::IsNearlyEqual(ThreeDelta, ThreeExpected, 0.01f),
					TEXT("3 stacks: +35%"),
					FString::Printf(TEXT("stacks=%d, took %.2f, expected %.2f (x%.3f)"),
						ThreeStacks, ThreeDelta, ThreeExpected, TraceVulnerable::GetMultiplierForStacks(3)));

				// ---- THE UNMARKED CONTROL. Proves the numbers above were the MARK's doing. ----
				Health->ResetHealth();
				const float PlainDelta = MeasureDamage(Health, StackTestDamage);
				State->List.Check(FMath::IsNearlyEqual(PlainDelta, StackTestDamage, 0.01f),
					TEXT("CONTROL: an unmarked target takes exactly the asked damage"),
					FString::Printf(TEXT("took %.2f, expected %.1f"), PlainDelta, StackTestDamage));

				// ---- THE CAP. Ask for far more than the ceiling and check both the count and the
				//      number, because a cap on one and not the other is a real and silent bug. ----
				const int32 Cap = TraceVulnerable::GetMaxStacks();
				Health->ResetHealth();
				const int32 CappedStacks = MarkTimes(Health, Cap + 7);
				const float CappedDelta = MeasureDamage(Health, StackTestDamage);
				const float CappedExpected = StackTestDamage * TraceVulnerable::GetMultiplierForStacks(Cap);

				State->List.Check(CappedStacks == Cap && FMath::IsNearlyEqual(CappedDelta, CappedExpected, 0.01f),
					TEXT("the stack count is CAPPED (v16 §4 [ASSUMPTION]: cap is a knob)"),
					FString::Printf(TEXT("%d applications -> %d stacks (cap %d), took %.2f, expected %.2f (x%.3f)"),
						Cap + 7, CappedStacks, Cap, CappedDelta, CappedExpected,
						TraceVulnerable::GetMultiplierForStacks(Cap)));

				// Stamp two stacks and let ~1s of the duration burn off; step 2 checks the refresh.
				Health->ResetHealth();
				MarkTimes(Health, 2);
				State->Step = 2;
				State->NextStepRealTime = NowReal + 1.0;
				return true;
			}

			// ---- step 2: ONE TIMER, REFRESHED BY EVERY HIT, SHARED BY EVERY STACK ----
			if (State->Step == 2)
			{
				const float Duration = TraceVulnerable::GetDurationSeconds();
				const float BeforeRefresh = Health->GetVulnerableRemaining();
				const int32 BeforeStacks = Health->GetVulnerableStacks();

				Health->ApplyVulnerable(Duration, nullptr);

				const float AfterRefresh = Health->GetVulnerableRemaining();
				const int32 AfterStacks = Health->GetVulnerableStacks();

				State->List.Check(BeforeStacks == 2 && BeforeRefresh > 0.f && BeforeRefresh < Duration * 0.85f,
					TEXT("the shared timer decays with the stacks still on"),
					FString::Printf(TEXT("%d stacks, %.2fs left of %.2fs after ~1s of real time"),
						BeforeStacks, BeforeRefresh, Duration));

				State->List.Check(AfterStacks == 3 && FMath::IsNearlyEqual(AfterRefresh, Duration, 0.15f),
					TEXT("each hit adds a stack AND refreshes the ONE timer"),
					FString::Printf(TEXT("%d -> %d stacks, %.2fs -> %.2fs (and NOT %.2fs, which is what a "
					                     "per-stack timer or an extension would give)"),
						BeforeStacks, AfterStacks, BeforeRefresh, AfterRefresh, BeforeRefresh + Duration));

				State->Step = 3;
				State->NextStepRealTime = NowReal + static_cast<double>(Duration) + 0.5;
				return true;
			}

			// ---- step 3: "WHENEVER THE TIMER RUNS OUT, ALL STACKS DISAPPEAR" ----
			{
				const int32 ExpiredStacks = Health->GetVulnerableStacks();
				const float ExpiredMultiplier = Health->GetVulnerableDamageMultiplier();
				const bool bStillMarked = Health->IsVulnerable();

				Health->ResetHealth();
				const float ExpiredDelta = MeasureDamage(Health, StackTestDamage);

				State->List.Check(!bStillMarked && ExpiredStacks == 0
					&& FMath::IsNearlyEqual(ExpiredMultiplier, 1.f, 0.001f),
					TEXT("ALL THREE stacks vanish together when the timer runs out"),
					FString::Printf(TEXT("marked=%d stacks=%d multiplier=x%.3f — not 2, not 1, zero, and all in "
					                     "the same instant"),
						bStillMarked ? 1 : 0, ExpiredStacks, ExpiredMultiplier));

				State->List.Check(FMath::IsNearlyEqual(ExpiredDelta, StackTestDamage, 0.01f),
					TEXT("an expired target is back to unamplified damage"),
					FString::Printf(TEXT("took %.2f, expected %.1f"), ExpiredDelta, StackTestDamage));

				// ---- the verdict, and the red arm gating it ----
				if (!State->bRedReproduced)
				{
					State->List.Invalidate(FString::Printf(
						TEXT("the RED arm did not reproduce (three marks measured %.2f with stacking disarmed, "
						     "expected %.2f) — with nothing falsified, the green arm's numbers are uninformative"),
						State->RedThreeMarkDelta,
						StackTestDamage * TraceVulnerable::GetMultiplierForStacks(1)));
				}

				Health->ResetHealth();
				State->List.Report();
				RestoreArms();
				return false;
			}
		}));
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.X.StackCarrierTest — spec v16 §4's carrier clause, red-armed
	// ---------------------------------------------------------------------------------------------

	struct FCarrierStackState
	{
		int32 Arm = 0;              // 0 = RED (carrier locks removed), 1 = GREEN (shipped)
		double Deadline = 0.0;
		FChecklist List;
		TWeakObjectPtr<ATraceCharacter> CarrierPawn;
		TWeakObjectPtr<ATraceCharacter> ControlPawn;

		int32 RedStacks = -1;
		float RedMultiplier = -1.f;
		bool  bRedAlarmMoved = false;

		int32 GreenStacks = -1;
		float GreenMultiplier = -1.f;
		bool  bGreenAlarmsQuiet = false;

		int32 ControlStacks = -1;
		float ControlDelta = -1.f;
		bool  bControlWorked = false;
	};

	/** A living, non-carrier ATraceCharacter that is not @p Exclude. The control subject. */
	ATraceCharacter* FindLiveNonCarrier(UWorld* World, const ATraceCharacter* Exclude)
	{
		const ATraceCore* CoreActor = ATraceCore::Get(World);
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Exclude || !Candidate->IsAlive()
				|| Candidate->Health == nullptr)
			{
				continue;
			}
			if (Candidate->IsCarrier() || (CoreActor != nullptr && CoreActor->Carrier == Candidate))
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	void RunCarrierStackTest(UWorld* World)
	{
		if (World == nullptr || World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[VULNSTACKCARRIER] Server only."));
			return;
		}

		TSharedPtr<FCarrierStackState> State = MakeShared<FCarrierStackState>();
		State->List.Tag = TEXT("VULNSTACKCARRIER");
		State->Deadline = FPlatformTime::Seconds() + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[VULNSTACKCARRIER] ===== spec v14 §4 re-proven under spec v16 §4's STACKING: a Core carrier can "
			     "neither collect stacks nor have any multiplier evaluated on their damage. arm 0 removes the "
			     "carrier locks and MUST reproduce (carrier reaches %d stacks at x%.3f and both alarms move); "
			     "arm 1 is shipped and must show 0 stacks, x1.000 and silent alarms. ====="),
			3, TraceVulnerable::GetMultiplierForStacks(3));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreArms();
				return false;
			}

			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);
			ATraceCharacter* CarrierPawn = (CoreActor != nullptr) ? CoreActor->Carrier : nullptr;

			// ---- staging ----------------------------------------------------------------------
			if (!State->CarrierPawn.IsValid() || !State->ControlPawn.IsValid())
			{
				if (CarrierPawn == nullptr && CoreActor != nullptr)
				{
					// No holder right now (the Core is loose, or in flight). Volunteer somebody, the
					// same way Trace.X.CarrierTest does — a fixture that can only run when a bot
					// happens to be carrying is a fixture that mostly does not run.
					if (ATraceCharacter* Volunteer = FindLiveNonCarrier(TickWorld, nullptr))
					{
						CoreActor->GrantTo(Volunteer, ETraceCoreGrantReason::Debug);
						CarrierPawn = CoreActor->Carrier;
					}
				}

				ATraceCharacter* ControlPawn = FindLiveNonCarrier(TickWorld, CarrierPawn);

				if (CarrierPawn == nullptr || CarrierPawn->Health == nullptr
					|| ControlPawn == nullptr || ControlPawn->Health == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(FString::Printf(
							TEXT("could not stage: carrier=%s control=%s — this test needs a live match with at "
							     "least two pawns"),
							*GetNameSafe(CarrierPawn), *GetNameSafe(ControlPawn)));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}

				State->CarrierPawn = CarrierPawn;
				State->ControlPawn = ControlPawn;
				UE_LOG(LogTraceGame, Display, TEXT("[VULNSTACKCARRIER] staged: CARRIER %s | CONTROL %s"),
					*GetNameSafe(CarrierPawn), *GetNameSafe(ControlPawn));
				return true;
			}

			ATraceCharacter* Carrier = State->CarrierPawn.Get();
			ATraceCharacter* Control = State->ControlPawn.Get();
			if (Carrier == nullptr || Control == nullptr
				|| Carrier->Health == nullptr || Control->Health == nullptr)
			{
				State->List.Invalidate(TEXT("a participant went away mid-test"));
				State->List.Report();
				RestoreArms();
				return false;
			}

			// *** THE LIVE-CARRIER GATE, HELD EVERY TICK. *** Bots pass and score, and a subject who
			// stopped carrying between staging and the strike would turn the case under test into an
			// ordinary damage test that passes for entirely the wrong reason. Held for the CONTROL too:
			// a control that picked the Core up would be immune for the same reason the subject is.
			if (CoreActor != nullptr && CoreActor->Carrier != Carrier)
			{
				CoreActor->TryPickup(Carrier);
			}
			const bool bStaged = (CoreActor != nullptr) && (CoreActor->Carrier == Carrier)
				&& Carrier->IsAlive() && Control->IsAlive()
				&& (CoreActor->Carrier != Control) && !Control->IsCarrier();
			if (!bStaged)
			{
				if (NowReal > State->Deadline)
				{
					State->List.Invalidate(FString::Printf(
						TEXT("could not hold the staging (carrier alive=%d holds=%d | control alive=%d carrying=%d)"),
						Carrier->IsAlive() ? 1 : 0,
						(CoreActor != nullptr && CoreActor->Carrier == Carrier) ? 1 : 0,
						Control->IsAlive() ? 1 : 0, Control->IsCarrier() ? 1 : 0));
					State->List.Report();
					RestoreArms();
					return false;
				}
				return true;
			}

			// ---- arm 0: RED. Carrier locks removed, ordering moved above the shield. ----
			if (State->Arm == 0)
			{
				SetArm(TEXT("Trace.Ability.CarrierImmune"), 0);
				SetArm(TEXT("Trace.X.VulnerableCarrierImmune"), 0);
				SetArm(TEXT("Trace.X.VulnerableApplyOrder"), 0);

				const int32 AmplifiedBefore = TraceVulnerable::GetCarrierAmplifiedCount();
				const int32 MarkedBefore = TraceVulnerable::GetCarrierMarkedCount();

				Carrier->Health->ResetHealth();          // also clears any mark
				State->RedStacks = MarkTimes(Carrier->Health, 3);
				State->RedMultiplier = Carrier->Health->GetVulnerableDamageMultiplier();
				Carrier->Health->ApplyDamage(StackTestDamage, nullptr, StackTestCause);

				State->bRedAlarmMoved = (TraceVulnerable::GetCarrierMarkedCount() > MarkedBefore)
					&& (TraceVulnerable::GetCarrierAmplifiedCount() > AmplifiedBefore);

				UE_LOG(LogTraceGame, Display,
					TEXT("[VULNSTACKCARRIER] arm=0 (RED): carrier reached %d stacks at x%.3f | carrierMarked +%d "
					     "carrierAmplified +%d"),
					State->RedStacks, State->RedMultiplier,
					TraceVulnerable::GetCarrierMarkedCount() - MarkedBefore,
					TraceVulnerable::GetCarrierAmplifiedCount() - AmplifiedBefore);

				Carrier->Health->ClearVulnerable();
				Carrier->Health->ResetHealth();
				State->Arm = 1;
				return true;
			}

			// ---- arm 1: GREEN. The shipped build, identical calls. ----
			RestoreArms();

			const int32 AmplifiedBefore = TraceVulnerable::GetCarrierAmplifiedCount();
			const int32 MarkedBefore = TraceVulnerable::GetCarrierMarkedCount();

			Carrier->Health->ResetHealth();
			State->GreenStacks = MarkTimes(Carrier->Health, 3);
			State->GreenMultiplier = Carrier->Health->GetVulnerableDamageMultiplier();
			Carrier->Health->ApplyDamage(StackTestDamage, nullptr, StackTestCause);
			State->bGreenAlarmsQuiet = (TraceVulnerable::GetCarrierMarkedCount() == MarkedBefore)
				&& (TraceVulnerable::GetCarrierAmplifiedCount() == AmplifiedBefore);

			// ---- THE CONTROL. The identical three marks on a NON-carrier must stack and must really
			//      take amplified health off, or this run has proved something about itself only. ----
			Control->Health->ResetHealth();
			State->ControlStacks = MarkTimes(Control->Health, 3);
			State->ControlDelta = MeasureDamage(Control->Health, StackTestDamage);
			const float ControlExpected = StackTestDamage * TraceVulnerable::GetMultiplierForStacks(3);
			State->bControlWorked = (State->ControlStacks == 3)
				&& FMath::IsNearlyEqual(State->ControlDelta, ControlExpected, 0.01f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[VULNSTACKCARRIER] arm=1 (GREEN): carrier %d stacks at x%.3f, alarms quiet=%d | CONTROL %d "
				     "stacks took %.2f (expected %.2f) -> fixture %s"),
				State->GreenStacks, State->GreenMultiplier, State->bGreenAlarmsQuiet ? 1 : 0,
				State->ControlStacks, State->ControlDelta, ControlExpected,
				State->bControlWorked ? TEXT("REACHES HEALTH") : TEXT("*** DEAD — measures nothing ***"));

			// ---- verdict ----------------------------------------------------------------------
			if (!State->bControlWorked)
			{
				State->List.Invalidate(TEXT("the CONTROL's three marks on a NON-carrier did not produce three "
				                            "stacks of amplified damage — the fixture cannot reach health, so "
				                            "nothing it says about the carrier means anything"));
			}
			else if (State->RedStacks < 3 || !State->bRedAlarmMoved)
			{
				State->List.Invalidate(FString::Printf(
					TEXT("the RED arm did not reproduce (carrier reached %d stacks, alarms moved=%d) — with "
					     "nothing falsified, the green arm's clean run is uninformative"),
					State->RedStacks, State->bRedAlarmMoved ? 1 : 0));
			}

			State->List.Check(State->RedStacks == 3 && State->RedMultiplier > 1.f,
				TEXT("RED: with the carrier locks removed a carrier DOES collect stacks"),
				FString::Printf(TEXT("%d stacks at x%.3f — this is the failure the shipped build prevents"),
					State->RedStacks, State->RedMultiplier));
			State->List.Check(State->bRedAlarmMoved,
				TEXT("RED: both carrier alarms move"),
				TEXT("carrierMarked and carrierAmplified — the ordering claim made observable, since the "
				     "carrier shield is a `return` and leaves no damage number behind"));
			State->List.Check(State->GreenStacks == 0,
				TEXT("GREEN: three marks leave the carrier on ZERO stacks"),
				FString::Printf(TEXT("stacks=%d — ApplyVulnerable refuses a carrier, so there is nothing to "
				                     "count up from"), State->GreenStacks));
			State->List.Check(FMath::IsNearlyEqual(State->GreenMultiplier, 1.f, 0.001f),
				TEXT("GREEN: GetVulnerableDamageMultiplier is exactly 1 for a carrier, stacking or not"),
				FString::Printf(TEXT("x%.3f"), State->GreenMultiplier));
			State->List.Check(State->bGreenAlarmsQuiet,
				TEXT("GREEN: neither carrier alarm moves across the whole arm"),
				FString::Printf(TEXT("carrierMarked +%d carrierAmplified +%d (both must be +0; running totals "
				                     "%d and %d, which include the red arm's)"),
					TraceVulnerable::GetCarrierMarkedCount() - MarkedBefore,
					TraceVulnerable::GetCarrierAmplifiedCount() - AmplifiedBefore,
					TraceVulnerable::GetCarrierMarkedCount(), TraceVulnerable::GetCarrierAmplifiedCount()));
			State->List.Check(State->bControlWorked,
				TEXT("CONTROL: the identical three marks DO stack on a non-carrier"),
				FString::Printf(TEXT("%d stacks, %.2f damage, expected %.2f"),
					State->ControlStacks, State->ControlDelta, ControlExpected));

			Carrier->Health->ClearVulnerable();
			Control->Health->ClearVulnerable();
			Control->Health->ResetHealth();
			State->List.Report();
			RestoreArms();
			return false;
		}));
	}
}

static FAutoConsoleCommandWithWorld CmdVulnerableStackTest(
	TEXT("Trace.X.VulnerableStackTest"),
	TEXT("Dev only, SERVER. Spec v16 §4: prove vulnerable stacks (+25% then +5% each), that the cap holds, that "
	     "every hit refreshes the ONE timer, and that all stacks expire together. Red-arms itself with "
	     "Trace.X.VulnerableStacking 0 first and reports INVALID if that arm did not reproduce."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&TraceVulnerableStackTest::RunStackTest));

static FAutoConsoleCommandWithWorld CmdVulnerableStackCarrierTest(
	TEXT("Trace.X.StackCarrierTest"),
	TEXT("Dev only, SERVER. Spec v14 §4 re-proven with v16 §4 stacking in play: three marks at the Core carrier "
	     "collect no stacks and evaluate no multiplier. Red-arms the carrier locks first and carries a live "
	     "non-carrier CONTROL, so a clean run cannot be a fixture that never fired."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&TraceVulnerableStackTest::RunCarrierStackTest));

#endif // !UE_BUILD_SHIPPING
