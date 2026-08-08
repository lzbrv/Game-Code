// Trace — per-character health, damage and death.

#include "Gameplay/TraceHealthComponent.h"

#include "Net/UnrealNetwork.h"

#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"             // FTSTicker — the per-frame driver for the self-test
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

	OnRep_Health();
}

void UTraceHealthComponent::BroadcastDeath(AController* Instigator, FName Cause)
{
	if (bDeathBroadcast)
	{
		return;
	}
	bDeathBroadcast = 1;

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

#endif // !UE_BUILD_SHIPPING
