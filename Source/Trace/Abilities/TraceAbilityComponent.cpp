// Trace — the ability component and THE CARRIER CHOKE POINT (spec v14 §4 / §5).
//
// Read the header before this file. The two things worth knowing here:
//
//   * CanAffectTargetDetailed() is the choke point. Everything else in the framework that touches a
//     target goes through it, and it is the only place the carrier rule is written down.
//   * Every cooldown value in this file is an absolute match-clock timestamp. There is no duration
//     stored anywhere and no per-life timer. See the cooldown contract in the header.

#include "Abilities/TraceAbilityComponent.h"

#include "Containers/Ticker.h"           // FTSTicker — Trace.Ability.DeathWipeTest
#include "Engine/Engine.h"                // GEngine — the harness's world lookup
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"

#include "Abilities/TraceCharacterAbilitySet.h"
#include "Abilities/TraceAbilityWorldSubsystem.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"   // Demo 17 item 7: RefundDashCharge on a kill
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"          // ATraceCore::IsCoreHolder — the SAME predicate the knife uses
#include "Gameplay/TraceHealthComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE RED ARM.
//
// The knife's carrier immunity is proven by Trace.Knife.CarrierImmunityTest, and the only reason
// that harness is evidence is that Trace.Knife.CarrierImmune 0 can make it go RED. A harness that
// has never failed is not a harness. This is the ability-side twin of that CVar, and it exists for
// exactly the same reason: Trace.Ability.CarrierChokeTest must be able to fail on a broken build.
//
// It arms the SAME rule the knife's does — the predicate underneath both is ATraceCore::IsCoreHolder
// — rather than adding a second, independently-breakable copy of "who is the carrier". That is what
// spec §4's "extend that pattern, do not duplicate it" asks for.
//
// 1 (shipped): no ability may damage the Core carrier.
// 0:           the rule is removed so the harness can be shown FAILING. NEVER SHIP 0.
// =================================================================================================
static TAutoConsoleVariable<int32> CVarAbilityCarrierImmune(
	TEXT("Trace.Ability.CarrierImmune"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): no ability may damage or control the Core carrier — spec v14 §4. "
	     "0: removes the rule so Trace.Ability.CarrierChokeTest can be shown FAILING on a broken "
	     "build. Never ship 0."),
	ECVF_Cheat);

/**
 * THE ALARM. Counts times an ability's damage actually landed on a Core carrier.
 *
 * The twin of TraceMelee::GetCarrierKnifeHitCount. Must be zero for the whole life of the process on
 * a correct build; the harness reads it and every increment logs an Error naming the ability.
 */
static int32 GCarrierAbilityDamageHits = 0;

// =================================================================================================
// THE INTEGRATION RED ARM. See the namespace comment in the header for what it disarms and why.
// =================================================================================================
static TAutoConsoleVariable<int32> CVarAbilityIntegration(
	TEXT("Trace.Ability.Integration"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): the cross-file ability hooks are live — the movement speed "
	     "passives, the dash notifications, Mace's magnet bonus, the kill / spawn notifications and "
	     "the E / V / jump key routing. 0: removes ALL of them at once, which reproduces the "
	     "pre-integration build so Trace.Integration.Verify can be shown FAILING. Never ship 0."),
	ECVF_Cheat);

namespace TraceAbilityIntegration
{
	bool IsEnabled()
	{
		return CVarAbilityIntegration.GetValueOnAnyThread() != 0;
	}

	FCounters& Counters()
	{
		// Function-local static: one set per process, alive before any world exists (a bot can dash
		// during the very first tick) and never torn down between PIE sessions, which is what lets a
		// harness reset it explicitly and trust the delta.
		static FCounters Instance;
		return Instance;
	}

	void ResetCounters()
	{
		Counters() = FCounters();
	}
}

namespace TraceAbility
{
	TRACE_API int32 GetCarrierAbilityDamageHitCount() { return GCarrierAbilityDamageHits; }
	TRACE_API void  ResetCarrierAbilityDamageHitCount() { GCarrierAbilityDamageHits = 0; }
}

#if !UE_BUILD_SHIPPING
// =================================================================================================
// THE SPEC v15 §2 RED ARM, framework half. See UTraceAbilityComponent::IsRosterEnforcementOn.
//
// A plain int rather than a TAutoConsoleVariable::GetValueOnAnyThread() read, because the bot
// harness flips it around one scoped run with TGuardValue and reads it back on the same thread in
// the same call — the console-variable sink is asynchronous on some paths and this must not be.
// FAutoConsoleVariableRef still publishes it under a name so a manual session can reach it.
// =================================================================================================
static int32 GTraceEnforceRosterRules = 1;

static FAutoConsoleVariableRef CVarTraceEnforceRosterRules(
	TEXT("Trace.Characters.EnforceRosterRules"),
	GTraceEnforceRosterRules,
	TEXT("1 (default): UTraceAbilityComponent::ServerSetCharacter enforces per-team uniqueness AND "
	     "spec v15 s2's rule that a bot waits for every human on its team. 0 removes both so "
	     "Trace.Characters.BotVerifyRed can be shown to FAIL. Dev only; absent from shipping."),
	ECVF_Cheat);
#endif

// A scratch instance handed out by GetMutableNetState() on a machine with no authority, so a
// character file that forgets a HasAuthority() check writes into a bin rather than into replicated
// state that the next OnRep silently reverts. Deliberately shared and deliberately never read.
static FTraceAbilityNetState GNonAuthorityScratchState;

// =================================================================================================
// Construction / lifetime
// =================================================================================================

UTraceAbilityComponent::UTraceAbilityComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// TICKS ON THE PLAYERSTATE, WHICH IS ALIVE WHILE THE PLAYER IS DEAD. That is the whole point:
	// spec §5's "cooldowns should continue to countdown while a player is dead" is only free because
	// nothing here is attached to a pawn that gets destroyed.
	//
	// 20 Hz, not every frame. Nothing in this component's own tick is a per-frame fact — the
	// cooldown is a timestamp compared on read, not a counter — and the per-character work is
	// forwarded to TickAbilities().
	//
	// THIS RATE IS THE ONE EVERY CHARACTER GETS, and there is deliberately no supported way for a
	// character to raise it. An earlier version of this comment said characters "may raise" it, which
	// invited exactly the wrong fix: a character reaching into PrimaryComponentTick from its own
	// equip would change the rate for the whole component, including the cooldown bookkeeping, and
	// the next character to be equipped on that PlayerState would inherit it. If a future ability
	// genuinely needs frame-accurate integration, add an explicit opt-in here (SetAbilityTickRate)
	// so the component owns the decision and can put it back. Mace's pull and V-suspend write
	// Velocity at this rate today and were measured to be fine at it — see TickAbilities' doc in
	// TraceCharacterAbilitySet.h for the numbers.
	PrimaryComponentTick.TickInterval = 0.05f;

	SetIsReplicatedByDefault(true);
}

void UTraceAbilityComponent::BeginPlay()
{
	Super::BeginPlay();

	// On a client the character may already have replicated before BeginPlay ran, in which case
	// OnRep fired against a component that had no world yet. Build unconditionally here; the
	// BuiltForCharacter guard makes it idempotent.
	RebuildAbilitySet();
}

void UTraceAbilityComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AbilitySet != nullptr)
	{
		AbilitySet->OnUnequipped();
		AbilitySet = nullptr;
		BuiltForCharacter = ETraceCharacterId::None;
	}

	Super::EndPlay(EndPlayReason);
}

void UTraceAbilityComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// All three to EVERYONE, not COND_OwnerOnly. The HUD draws enemy character identities, the
	// kill feed names them, and the choke point's caller may be any machine — a cooldown that only
	// the owner can see would make a spectator's HUD and a listen server's HUD disagree.
	DOREPLIFETIME(UTraceAbilityComponent, CharacterId);
	DOREPLIFETIME(UTraceAbilityComponent, ActivatedCooldownEndMatchTime);
	DOREPLIFETIME(UTraceAbilityComponent, AbilityState);
}

void UTraceAbilityComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                           FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (AbilitySet != nullptr)
	{
		AbilitySet->TickAbilities(DeltaTime);
	}
}

// =================================================================================================
// Identity and selection
// =================================================================================================

void UTraceAbilityComponent::ServerSetCharacter(ETraceCharacterId NewCharacter)
{
	if (!HasAuthorityOwner())
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Ability] ServerSetCharacter(%s) called without authority on %s — ignored. Clients "
			     "must go through ServerRequestSetCharacter."),
			TraceCharacterIdToString(NewCharacter), *GetNameSafe(GetOwningPlayerState()));
		return;
	}

	// Clearing is infallible and always allowed: it is the way back to the default Mannequin, it is
	// what mode A forces (§2), what the disable toggle produces (§3) and what the select screen's
	// timeout falls back to. A refusal here would strand a player on a character the rules just
	// outlawed.
	if (NewCharacter == ETraceCharacterId::None)
	{
		if (CharacterId != ETraceCharacterId::None)
		{
			UE_LOG(LogTraceGame, Log, TEXT("[Ability] %s: %s -> None (default Mannequin)."),
				*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(CharacterId));
		}
		CharacterId = ETraceCharacterId::None;
		AbilityState.Reset();
		MarkNetStateDirty();
		OnRep_CharacterId();
		return;
	}

	if (!AreCharactersEnabled(this))
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("[Ability] %s asked for %s but characters are OFF here (mode A is frozen — spec §2 — "
			     "or the 'disable all characters' toggle is set). Staying on the default Mannequin."),
			*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(NewCharacter));
		return;
	}

	if (static_cast<int32>(NewCharacter) >= static_cast<int32>(ETraceCharacterId::Count))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Ability] %s asked for character id %d, which does not exist."),
			*GetNameSafe(GetOwningPlayerState()), static_cast<int32>(NewCharacter));
		return;
	}

	if (CharacterId == NewCharacter)
	{
		return;   // idempotent; a re-request is not a re-equip and must not wipe transient state
	}

	// THE TWO ROSTER RULES BELOW ARE THE ONES THE §2 RED ARM REMOVES. See IsRosterEnforcementOn().
	bool bEnforceRoster = true;
#if !UE_BUILD_SHIPPING
	bEnforceRoster = IsRosterEnforcementOn();
#endif

	// ---- SPEC v15 §2's ORDERING RULE ------------------------------------------------------------
	//
	// Verbatim: "the computers should wait for any actual humans on its team to choose before all
	// loading in with randomly chosen characters."
	//
	// ENFORCED HERE rather than only where bots are filled, and that is the whole reason it is
	// trustworthy: TWO independent things assign a bot a character (ATraceGameMode::
	// PollCharacterSelect at 4 Hz, and ATraceBotController::UpdateAutoCharacter's own 0.5 Hz poll in
	// the AI slice), and a rule implemented in one of them is a rule the other can walk straight
	// past. Putting it on the single function BOTH must go through makes the ordering true by
	// construction instead of by two files agreeing.
	//
	// Note that this replaced spec v14 §3's outright "bots stay characterless" refusal, which used to
	// live on exactly these lines.
	if (bEnforceRoster && IsBot() && !AreHumansOnTeamSettled(this, GetTeam()))
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("[Ability] %s (bot) asked for %s — REFUSED, a human on its team has not settled yet "
			     "(spec v15 §2: bots pick last). The select timeout is what guarantees this ends."),
			*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(NewCharacter));
		return;
	}

	// PER-TEAM UNIQUENESS. Spec v14 §3: "Do not allow players to select a character who has already
	// been chosen by a player on their team", and spec v15 §2 extends it verbatim to bots: "no two
	// should be able to pick the same characters". FIRST REQUEST WINS — this runs on the server, so
	// two teammates pressing the same key in the same frame are two sequential calls here and the
	// second one finds the first already recorded. The loser keeps what they had and the select
	// screen re-queries IsCharacterAvailableFor to redraw.
	//
	// There is deliberately no "unless one of them is a bot" branch: FindTeammateHolding walks every
	// player state on the team, and a bot is a player state.
	if (bEnforceRoster)
	{
		if (const APlayerState* Holder = FindTeammateHolding(this, GetTeam(), NewCharacter))
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[Ability] %s asked for %s — REFUSED, teammate %s already holds it (per-team uniqueness, spec §3)."),
				*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(NewCharacter), *GetNameSafe(Holder));
			return;
		}
	}

	const ETraceCharacterId Previous = CharacterId;
	CharacterId = NewCharacter;

	// The transient state belonged to the character that just left, so it goes. THE COOLDOWN DOES
	// NOT: swapping character mid-match must not be a way to buy a free E, and spec §5 gives exactly
	// one automatic reset — half time.
	AbilityState.Reset();
	MarkNetStateDirty();

	// Server-side OnRep, so a listen server's own machine takes the identical path a remote client
	// takes. Every state change in this file does this; it is the reason there is no separate
	// "listen server" branch anywhere.
	OnRep_CharacterId();

	UE_LOG(LogTraceGame, Display, TEXT("[Ability] %s: %s -> %s (cooldown untouched: %.2fs remaining)."),
		*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(Previous),
		TraceCharacterIdToString(CharacterId), GetActivatedCooldownRemaining());
}

void UTraceAbilityComponent::ServerRequestSetCharacter_Implementation(ETraceCharacterId NewCharacter)
{
	ServerSetCharacter(NewCharacter);
}

void UTraceAbilityComponent::OnRep_CharacterId()
{
	RebuildAbilitySet();
}

void UTraceAbilityComponent::OnRep_AbilityState()
{
	// Nothing generic to do — characters read GetNetState() at the point of use. The UFUNCTION
	// exists so that a character which needs a client-side edge (a sound when Chud comes up) has a
	// place to hang it: override TickAbilities and compare, or ask for a hook here.
}

void UTraceAbilityComponent::RebuildAbilitySet()
{
	if (BuiltForCharacter == CharacterId && (AbilitySet != nullptr || CharacterId == ETraceCharacterId::None))
	{
		return;
	}

	if (AbilitySet != nullptr)
	{
		AbilitySet->OnUnequipped();
		AbilitySet = nullptr;
	}

	BuiltForCharacter = CharacterId;

	if (CharacterId == ETraceCharacterId::None)
	{
		return;   // the default characterless Mannequin. Every hook must be a no-op here.
	}

	UClass* SetClass = UTraceCharacterAbilitySet::FindClassFor(CharacterId);
	if (SetClass == nullptr)
	{
		// NOT an error yet: this is exactly the state the framework ships in before the five
		// character agents land their files. The character is recorded and replicated, the HUD can
		// name it, the cooldown runs — there is simply no behaviour behind it.
		UE_LOG(LogTraceGame, Log,
			TEXT("[Ability] %s is %s, but no UTraceCharacterAbilitySet subclass claims that id yet — "
			     "the player is a Mannequin with a name. Add Abilities/TraceAbilitySet%s.h/.cpp."),
			*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(CharacterId),
			TraceCharacterIdToString(CharacterId));
		return;
	}

	AbilitySet = NewObject<UTraceCharacterAbilitySet>(this, SetClass);
	AbilitySet->Initialize(this);
	AbilitySet->OnInitialized();
	AbilitySet->OnEquipped();
}

// =================================================================================================
// The activated (E) ability
// =================================================================================================

bool UTraceAbilityComponent::HasAuthorityOwner() const
{
	// One helper rather than nine copies of `GetOwner() != nullptr && GetOwner()->HasAuthority()`.
	// The null check is not defensive noise: this component is created at runtime and attached to a
	// PlayerState, and a PlayerState is destroyed on logout while its components can still be
	// reached for a frame from a queued RPC or a ticker-driven harness.
	const AActor* MyOwner = GetOwner();
	return (MyOwner != nullptr) && MyOwner->HasAuthority();
}

float UTraceAbilityComponent::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

float UTraceAbilityComponent::GetActivatedCooldownRemaining() const
{
	// The owning client's PREDICTED deadline counts too, and it is deliberately the max of the two:
	// a prediction that fired locally must grey the HUD button immediately, and the replicated value
	// arriving a round trip later must not let it flash back to ready in between.
	const float Deadline = FMath::Max(ActivatedCooldownEndMatchTime, PredictedCooldownEndMatchTime);
	const float FrameworkRemaining = FMath::Max(0.f, Deadline - MatchTimeNow());

	// ...and a THIRD, for the one character whose own CanActivate() refuses for longer than the
	// framework's timer runs. See UTraceCharacterAbilitySet::GetCharacterOwnedCooldownRemaining():
	// Elle's fluffed Snap cast leaves the framework at zero while she refuses for up to 31 s, and a
	// ring that reads READY on a button that does nothing is the worst of the available lies. Max, so
	// a character can only ever be more conservative than the framework, never less.
	if (AbilitySet != nullptr)
	{
		return FMath::Max(FrameworkRemaining, FMath::Max(0.f, AbilitySet->GetCharacterOwnedCooldownRemaining()));
	}

	return FrameworkRemaining;
}

bool UTraceAbilityComponent::TryActivate()
{
	// ---- every refusal, centralised, so no character has to remember any of them ----------------
	if (CharacterId == ETraceCharacterId::None || AbilitySet == nullptr)
	{
		return false;
	}

	if (!AreCharactersEnabled(this))
	{
		return false;
	}

	ATraceCharacter* MyPawn = GetOwningCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		return false;
	}

	const UWorld* WorldPtr = GetWorld();
	const ATraceGameState* TraceGS = (WorldPtr != nullptr) ? WorldPtr->GetGameState<ATraceGameState>() : nullptr;
	if (TraceGS != nullptr && TraceGS->IsHalfTimeBreak())
	{
		return false;   // the interval is a dead phase; nothing fires in it
	}

	if (GetActivatedCooldownRemaining() > 0.f)
	{
		return false;
	}

	FText Reason;
	if (!AbilitySet->CanActivate(Reason))
	{
		return false;
	}

	// ---- fire ------------------------------------------------------------------------------------
	const bool bFired = AbilitySet->ActivateAbility();
	if (!bFired)
	{
		// A deliberate fizzle. The character chose not to be charged for it.
		return false;
	}

	const float Cooldown = FMath::Max(0.f, AbilitySet->GetActivatedCooldownSeconds());
	const float Deadline = MatchTimeNow() + Cooldown;

	if (HasAuthorityOwner())
	{
		ActivatedCooldownEndMatchTime = Deadline;
		PredictedCooldownEndMatchTime = Deadline;   // keeps a listen server's own HUD in step

		UE_LOG(LogTraceGame, Verbose, TEXT("[Ability] %s activated %s; ready again at match time %.2f (%.1fs)."),
			*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(CharacterId), Deadline, Cooldown);
	}
	else
	{
		// PREDICTED. The local half already ran inside ActivateAbility(); grey the button now and let
		// the server's replicated deadline overwrite it a round trip later.
		PredictedCooldownEndMatchTime = Deadline;
		ServerTryActivate();
	}

	return true;
}

void UTraceAbilityComponent::ServerTryActivate_Implementation()
{
	const float BeforeDeadline = ActivatedCooldownEndMatchTime;

	if (!TryActivate())
	{
		// The client predicted an activation the server refuses. Tell it the truth so the HUD does
		// not sit greyed out for a cooldown that never started.
		ClientActivateRejected(BeforeDeadline);
	}
}

void UTraceAbilityComponent::ClientActivateRejected_Implementation(float AuthoritativeCooldownEndMatchTime)
{
	PredictedCooldownEndMatchTime = AuthoritativeCooldownEndMatchTime;
}

void UTraceAbilityComponent::ServerHandleJumpPressed_Implementation()
{
	// Every refusal is inside the character's own hook (Rocco declines on the ground and after his
	// one use; Oyster declines when he is not stood on a jar), so there is nothing to re-validate
	// here — and re-validating would mean two copies of a rule that must agree.
	HandleJumpPressed();
}

void UTraceAbilityComponent::ServerSetSecondaryHeld_Implementation(bool bHeld)
{
	if (bHeld)
	{
		HandleSecondaryPressed();
	}
	else
	{
		HandleSecondaryReleased();
	}
}

void UTraceAbilityComponent::OnHalfTime()
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	// SPEC §5's ONE AUTOMATIC RESET. Nothing else in the framework writes these two lines.
	ActivatedCooldownEndMatchTime = 0.f;
	PredictedCooldownEndMatchTime = 0.f;
	AbilityState.Reset();
	MarkNetStateDirty();

	if (AbilitySet != nullptr)
	{
		AbilitySet->OnHalfTime();
	}

	UE_LOG(LogTraceGame, Log, TEXT("[Ability] HALF TIME reset: %s (%s) — cooldown and transient state cleared."),
		*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(CharacterId));
}

// =================================================================================================
// *** THE CARRIER CHOKE POINT — SPEC §4 ***
// =================================================================================================

bool UTraceAbilityComponent::IsCarrier(const ATraceCharacter* Target)
{
	if (Target == nullptr)
	{
		return false;
	}

	// TWO INDEPENDENT SOURCES, EITHER ONE SUFFICIENT, ON PURPOSE.
	//
	// ATraceCore::IsCoreHolder is the predicate TraceMelee::ResolveSwing's proven rule uses, so this
	// is literally the knife's rule extended rather than a second implementation of "who has the
	// Core". ATraceCharacter::bIsCarrier is the pawn's own replicated mirror, which is correct on a
	// simulated proxy in the frame before the Core actor's Carrier pointer has resolved.
	//
	// OR, not AND. A one-frame disagreement between them must resolve to "yes, carrier" — the cost
	// of a false positive is one ability that does nothing, the cost of a false negative is a dead
	// carrier and the game's founding invariant broken.
	return Target->IsCarrier() || ATraceCore::IsCoreHolder(Target);
}

bool UTraceAbilityComponent::MayAbilityAffectCarrier(const ATraceCharacter* Target, ETraceAbilityEffect Effect)
{
	if (!IsCarrier(Target))
	{
		return true;   // not a carrier; this rule has nothing to say
	}

	// Beneficial always passes, and that is a rule from §6 rather than a convenience: "Any character,
	// either team, may enter the ripple's start… The Core carrier can use it."
	if (Effect == ETraceAbilityEffect::Beneficial)
	{
		return true;
	}

	// THE RED ARM. See CVarAbilityCarrierImmune. 0 removes the rule so the harness can go red.
	if (CVarAbilityCarrierImmune.GetValueOnAnyThread() == 0)
	{
		return true;
	}

	if (Effect == ETraceAbilityEffect::Damage)
	{
		// *** THE FOUNDING INVARIANT. No knob, no exception, no ability. ***
		// "NO abilities damage carriers, carriers can still only be killed when an enemy dashes
		// through their trace."
		return false;
	}

	// Control — the [ASSUMPTION] half, and the only part of this rule a designer may reverse.
	return !UTraceSettings::Get().bCarrierImmuneToAbilityControl;
}

ETraceAbilityBlockReason UTraceAbilityComponent::CanAffectTargetDetailed(const ATraceCharacter* Target,
                                                                        ETraceAbilityEffect Effect) const
{
	if (Target == nullptr)
	{
		return ETraceAbilityBlockReason::NoTarget;
	}

	// THE CARRIER RULE IS TESTED FIRST, BEFORE ANYTHING THAT COULD SHORT-CIRCUIT IT. Ordering is a
	// safety property here, not a style choice: if a later clause ever gains an early `return
	// Allowed`, the invariant must already have had its say.
	if (!MayAbilityAffectCarrier(Target, Effect))
	{
		return (Effect == ETraceAbilityEffect::Damage)
			? ETraceAbilityBlockReason::CarrierDamageImmune
			: ETraceAbilityBlockReason::CarrierControlImmune;
	}

	if (!Target->IsAlive())
	{
		return ETraceAbilityBlockReason::Dead;
	}

	const ATraceCharacter* MyPawn = GetOwningCharacter();
	if (MyPawn != nullptr && MyPawn == Target)
	{
		// Self. Beneficial on yourself is fine (a self-buff is not routed through here anyway);
		// damaging or controlling yourself with your own ability is not a thing any of §6 wants.
		return (Effect == ETraceAbilityEffect::Beneficial)
			? ETraceAbilityBlockReason::Allowed
			: ETraceAbilityBlockReason::Self;
	}

	if (Effect != ETraceAbilityEffect::Beneficial)
	{
		const ETraceTeam MyTeam = GetTeam();
		const ETraceTeam TargetTeam = Target->GetTeam();
		if (MyTeam != ETraceTeam::None && MyTeam == TargetTeam && !UTraceSettings::Get().bFriendlyFire)
		{
			return ETraceAbilityBlockReason::SameTeam;
		}
	}

	return ETraceAbilityBlockReason::Allowed;
}

bool UTraceAbilityComponent::CanAffectTarget(const ATraceCharacter* Target, ETraceAbilityEffect Effect) const
{
	return CanAffectTargetDetailed(Target, Effect) == ETraceAbilityBlockReason::Allowed;
}

bool UTraceAbilityComponent::CanAffect(const AActor* InstigatorActor, const ATraceCharacter* Target,
                                       ETraceAbilityEffect Effect)
{
	// THE CARRIER RULE HOLDS EVEN WITH NO INSTIGATOR AT ALL. A jar whose owner logged out, a
	// projectile whose shooter's PlayerState has gone, a bee outliving X — none of them may become
	// the one path that damages a carrier. This is the whole reason the static form exists.
	if (!MayAbilityAffectCarrier(Target, Effect))
	{
		return false;
	}

	if (const UTraceAbilityComponent* Comp = Get(InstigatorActor))
	{
		return Comp->CanAffectTarget(Target, Effect);
	}

	// No component: apply the parts that do not need one.
	return (Target != nullptr) && Target->IsAlive();
}

// =================================================================================================
// Damage, routed through the choke point and both characters' passives
// =================================================================================================

float UTraceAbilityComponent::ModifyDamageThroughPassives(float Damage, const FTraceAbilityDamageContext& Context)
{
	float Result = Damage;

	if (const UTraceCharacterAbilitySet* InstigatorSet = GetAbilitySetFor(Context.Instigator))
	{
		Result = InstigatorSet->ModifyOutgoingDamage(Result, Context);
	}

	if (const UTraceCharacterAbilitySet* TargetSet = GetAbilitySetFor(Context.Target))
	{
		// REDUCTION FIRST, THEN AMPLIFICATION, and the order is defined rather than incidental:
		// Chut's Chud (−30%) and X's vulnerable (+25%) will meet, and 100 must resolve the same way
		// on every machine. 100 -> 70 -> 87.5, not 100 -> 125 -> 87.5 (which happens to agree here,
		// but stops agreeing the moment either becomes non-multiplicative).
		Result = TargetSet->ModifyIncomingDamage(Result, Context);
		Result *= FMath::Max(0.f, TargetSet->GetIncomingDamageMultiplier());
	}

	return FMath::Max(0.f, Result);
}

float UTraceAbilityComponent::ApplyAbilityDamage(ATraceCharacter* Target, float Amount, FName Cause,
                                                 bool bMelee, bool bHeadshot) const
{
	if (!HasAuthorityOwner())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Ability] ApplyAbilityDamage(%s) off the server — ignored."), *Cause.ToString());
		return 0.f;
	}

	// *** THE CHOKE POINT. Every ability's damage passes exactly here. ***
	const ETraceAbilityBlockReason Reason = CanAffectTargetDetailed(Target, ETraceAbilityEffect::Damage);
	if (Reason != ETraceAbilityBlockReason::Allowed)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Ability] %s's '%s' refused on %s: %s"),
			TraceCharacterIdToString(CharacterId), *Cause.ToString(), *GetNameSafe(Target),
			TraceAbilityBlockReasonToString(Reason));
		return 0.f;
	}

	// THE ALARM. Reaching here with a carrier as the victim means the choke point above let one
	// through, which is the single worst regression this pass can produce. It is logged as an Error
	// naming the ability, and Trace.Ability.CarrierChokeTest reads the counter.
	if (IsCarrier(Target))
	{
		++GCarrierAbilityDamageHits;
		UE_LOG(LogTraceGame, Error,
			TEXT("[Ability] *** ABILITY DAMAGE '%s' RESOLVED ONTO THE CORE CARRIER %s (hit #%d). Spec v14 §4: "
			     "NO ability may damage a carrier. Check Trace.Ability.CarrierImmune and CanAffectTargetDetailed."),
			*Cause.ToString(), *GetNameSafe(Target), GCarrierAbilityDamageHits);
	}

	FTraceAbilityDamageContext Context;
	Context.Instigator   = GetOwningCharacter();
	Context.Target       = Target;
	Context.Cause        = Cause;
	Context.bHeadshot    = bHeadshot;
	Context.bMelee       = bMelee;
	Context.bFromAbility = true;

	const float Final = ModifyDamageThroughPassives(Amount, Context);
	if (Final <= 0.f)
	{
		return 0.f;
	}

	UTraceHealthComponent* TargetHealth = Target->Health;
	if (TargetHealth == nullptr)
	{
		return 0.f;
	}

	AController* MyController = nullptr;
	if (const APlayerState* MyState = GetOwningPlayerState())
	{
		MyController = MyState->GetOwningController();
	}

	TargetHealth->ApplyDamage(Final, MyController, Cause);
	return Final;
}

// =================================================================================================
// Resolution
// =================================================================================================

UTraceAbilityComponent* UTraceAbilityComponent::Get(const AActor* Actor)
{
	if (Actor == nullptr)
	{
		return nullptr;
	}

	// Already a PlayerState — the component's real home.
	if (const APlayerState* AsState = Cast<APlayerState>(Actor))
	{
		return AsState->FindComponentByClass<UTraceAbilityComponent>();
	}

	if (const APawn* AsPawn = Cast<APawn>(Actor))
	{
		// GetPlayerState() is null for a frame or two after a respawn on a client, and for a corpse
		// on the server. A component that vanishes for two frames would look exactly like "this
		// player has no character", so fall through to the controller as a second route.
		if (const APlayerState* PawnState = AsPawn->GetPlayerState())
		{
			return PawnState->FindComponentByClass<UTraceAbilityComponent>();
		}
		if (const AController* PawnController = AsPawn->GetController())
		{
			if (const APlayerState* ControllerState = PawnController->PlayerState)
			{
				return ControllerState->FindComponentByClass<UTraceAbilityComponent>();
			}
		}
		return nullptr;
	}

	if (const AController* AsController = Cast<AController>(Actor))
	{
		if (const APlayerState* ControllerState = AsController->PlayerState)
		{
			return ControllerState->FindComponentByClass<UTraceAbilityComponent>();
		}
	}

	return nullptr;
}

UTraceCharacterAbilitySet* UTraceAbilityComponent::GetAbilitySetFor(const AActor* Actor)
{
	const UTraceAbilityComponent* Comp = Get(Actor);
	return (Comp != nullptr) ? Comp->AbilitySet : nullptr;
}

APlayerState* UTraceAbilityComponent::GetOwningPlayerState() const
{
	return Cast<APlayerState>(GetOwner());
}

ATraceCharacter* UTraceAbilityComponent::GetOwningCharacter() const
{
	const APlayerState* MyState = GetOwningPlayerState();
	return (MyState != nullptr) ? Cast<ATraceCharacter>(MyState->GetPawn()) : nullptr;
}

ETraceTeam UTraceAbilityComponent::GetTeam() const
{
	if (const ATracePlayerState* MyState = Cast<ATracePlayerState>(GetOwner()))
	{
		return MyState->Team;
	}
	return ETraceTeam::None;
}

bool UTraceAbilityComponent::IsBot() const
{
	const APlayerState* MyState = GetOwningPlayerState();
	return (MyState != nullptr) && MyState->IsABot();
}

FTraceAbilityNetState& UTraceAbilityComponent::GetMutableNetState()
{
	if (!HasAuthorityOwner())
	{
		GNonAuthorityScratchState.Reset();
		return GNonAuthorityScratchState;
	}
	return AbilityState;
}

void UTraceAbilityComponent::MarkNetStateDirty()
{
	if (HasAuthorityOwner())
	{
		// Listen server parity: run the OnRep by hand so this machine takes the same path a remote
		// client does. Every replicated write in this file does it.
		OnRep_AbilityState();
	}
}

// =================================================================================================
// Selection support (spec §3's server-side rules; the screen itself is another slice)
// =================================================================================================

bool UTraceAbilityComponent::AreCharactersEnabled(const UObject* WorldContextObject)
{
	if (!UTraceSettings::Get().bCharactersEnabled)
	{
		return false;
	}

	// SPEC §2 — MODE A IS FROZEN. "Do not implement abilities or characters into what was game mode
	// a (endzones)." Answered from the replicated GameState, so it is the same answer on the server
	// and on every client, and GetScoringModeFor falls back to mode A when there is no GameState yet
	// — which is the safe direction: too early means "no characters", never "characters in mode A".
	return ATraceGameState::GetScoringModeFor(WorldContextObject) == ETraceScoringMode::ThrownCoreAndGoals;
}

APlayerState* UTraceAbilityComponent::FindTeammateHolding(const UObject* WorldContextObject,
                                                          ETraceTeam Team, ETraceCharacterId Candidate)
{
	if (Candidate == ETraceCharacterId::None || Team == ETraceTeam::None)
	{
		return nullptr;
	}

	// GetWorld() rather than GEngine->GetWorldFromContextObject, for the reason
	// ATraceGameState::GetScoringModeFor gives: every caller here is an actor or a component, and
	// UObject::GetWorld() is correct for both without dragging Engine.h in.
	const UWorld* WorldPtr = (WorldContextObject != nullptr) ? WorldContextObject->GetWorld() : nullptr;
	const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	if (BaseState == nullptr)
	{
		return nullptr;
	}

	for (APlayerState* Entry : BaseState->PlayerArray)
	{
		const ATracePlayerState* AsTraceState = Cast<ATracePlayerState>(Entry);
		if (AsTraceState == nullptr || AsTraceState->Team != Team)
		{
			continue;
		}

		const UTraceAbilityComponent* Comp = Entry->FindComponentByClass<UTraceAbilityComponent>();
		if (Comp != nullptr && Comp->GetCharacterId() == Candidate)
		{
			return Entry;
		}
	}

	return nullptr;
}

bool UTraceAbilityComponent::IsCharacterAvailableFor(const APlayerState* ForPlayerState, ETraceCharacterId Candidate)
{
	if (Candidate == ETraceCharacterId::None)
	{
		return true;
	}

	if (ForPlayerState == nullptr || !AreCharactersEnabled(ForPlayerState))
	{
		// Characters are off — nobody is holding anything, so nothing is unavailable. The select
		// screen should not be up at all in this state; answering "available" keeps it harmless if
		// it is.
		return true;
	}

	const ATracePlayerState* AsTraceState = Cast<ATracePlayerState>(ForPlayerState);
	const ETraceTeam Team = (AsTraceState != nullptr) ? AsTraceState->Team : ETraceTeam::None;

	const APlayerState* Holder = FindTeammateHolding(ForPlayerState, Team, Candidate);
	return (Holder == nullptr) || (Holder == ForPlayerState);
}

ETraceCharacterId UTraceAbilityComponent::PickFreeCharacterFor(const APlayerState* ForPlayerState)
{
	for (int32 Index = 1; Index < static_cast<int32>(ETraceCharacterId::Count); ++Index)
	{
		const ETraceCharacterId Candidate = static_cast<ETraceCharacterId>(Index);
		if (IsCharacterAvailableFor(ForPlayerState, Candidate))
		{
			return Candidate;
		}
	}
	return ETraceCharacterId::None;
}

ETraceCharacterId UTraceAbilityComponent::PickRandomFreeCharacterFor(const APlayerState* ForPlayerState)
{
	// Collect first, then roll ONCE. The obvious alternative — roll a number 1..5 and retry until it
	// lands on a free one — has no bound on its worst case and, on a team holding four of the five,
	// spends most of its rolls being wrong. This is one pass and one FMath::RandHelper.
	TArray<ETraceCharacterId, TInlineAllocator<TraceCharacterCount>> Free;

	for (int32 Index = 1; Index < static_cast<int32>(ETraceCharacterId::Count); ++Index)
	{
		const ETraceCharacterId Candidate = static_cast<ETraceCharacterId>(Index);
		if (IsCharacterAvailableFor(ForPlayerState, Candidate))
		{
			Free.Add(Candidate);
		}
	}

	if (Free.Num() == 0)
	{
		return ETraceCharacterId::None;
	}

	// FMath::RandHelper, not rand(): it is the engine's own stream, so a run started with -FixedSeed
	// reproduces the same fill — which is the difference between "the bots picked oddly" being a bug
	// report and being a shrug.
	return Free[FMath::RandHelper(Free.Num())];
}

bool UTraceAbilityComponent::AreHumansOnTeamSettled(const UObject* WorldContextObject, ETraceTeam Team)
{
	if (Team == ETraceTeam::None)
	{
		// No team means no team-mates, so there is nobody this player could be made to wait for. The
		// bot fill has its own "wait for a team" check; answering true here keeps this predicate about
		// humans and only about humans.
		return true;
	}

	const UWorld* WorldPtr = (WorldContextObject != nullptr) ? WorldContextObject->GetWorld() : nullptr;
	const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	if (BaseState == nullptr)
	{
		// No roster to read. FALSE — "not settled" — is the safe direction: it delays a bot's pick by
		// one poll rather than letting the whole team fill before the humans exist.
		return false;
	}

	for (APlayerState* Entry : BaseState->PlayerArray)
	{
		const ATracePlayerState* AsTraceState = Cast<ATracePlayerState>(Entry);
		if (AsTraceState == nullptr || AsTraceState->Team != Team || AsTraceState->IsABot())
		{
			continue;
		}

		const UTraceAbilityComponent* Comp = Entry->FindComponentByClass<UTraceAbilityComponent>();
		if (Comp != nullptr && Comp->GetCharacterId() != ETraceCharacterId::None)
		{
			continue;   // chose it, or the select timeout assigned it. Either way they are done.
		}

		// THE UNSERVICEABLE HUMAN. If the roster has nothing left for them, waiting cannot help and
		// would keep every bot on this team a Mannequin for the rest of the match. Reachable only
		// when a team has more players than the roster has characters, which is also the one case
		// ATraceGameMode::FindFreeCharacterForTeam already warns about.
		if (PickFreeCharacterFor(Entry) == ETraceCharacterId::None)
		{
			continue;
		}

		return false;
	}

	return true;
}

#if !UE_BUILD_SHIPPING
bool UTraceAbilityComponent::IsRosterEnforcementOn()
{
	return GTraceEnforceRosterRules != 0;
}

void UTraceAbilityComponent::SetRosterEnforcementOn(bool bEnforced)
{
	GTraceEnforceRosterRules = bEnforced ? 1 : 0;
}
#endif

// =================================================================================================
// Input and notification forwarding
// =================================================================================================

bool UTraceAbilityComponent::HandleSecondaryPressed()
{
	return (AbilitySet != nullptr) && AbilitySet->OnSecondaryPressed();
}

void UTraceAbilityComponent::HandleSecondaryReleased()
{
	if (AbilitySet != nullptr)
	{
		AbilitySet->OnSecondaryReleased();
	}
}

bool UTraceAbilityComponent::HandleJumpPressed()
{
	return (AbilitySet != nullptr) && AbilitySet->OnJumpPressed();
}

void UTraceAbilityComponent::HandleJumpReleased()
{
	if (AbilitySet != nullptr)
	{
		AbilitySet->OnJumpReleased();
	}
}

void UTraceAbilityComponent::NotifyDashStarted(const FVector& DashDirection)
{
	if (AbilitySet != nullptr)
	{
		AbilitySet->OnDashStarted(DashDirection);
	}
}

void UTraceAbilityComponent::NotifyDashEnded(bool bReachedFullDistance)
{
	if (AbilitySet != nullptr)
	{
		AbilitySet->OnDashEnded(bReachedFullDistance);
	}
}

void UTraceAbilityComponent::NotifyDashHitCharacter(ATraceCharacter* Other, float DashProgress)
{
	if (AbilitySet != nullptr && HasAuthorityOwner())
	{
		AbilitySet->OnDashHitCharacter(Other, DashProgress);
	}
}

float UTraceAbilityComponent::GetDashHitSweepRadiusFor(const AActor* Actor)
{
	// The whole point of this being static and null-safe: the movement component asks it once a
	// frame for every dashing pawn in the world, most of which are Mannequins and bots with no
	// component at all. 0 is "run no sweep", and it is the answer in every one of those cases.
	const UTraceAbilityComponent* Comp = Get(Actor);
	if (Comp == nullptr || Comp->AbilitySet == nullptr)
	{
		return 0.f;
	}
	return FMath::Max(0.f, Comp->AbilitySet->GetDashHitSweepRadius());
}

void UTraceAbilityComponent::NotifyPawnSpawned()
{
	if (AbilitySet != nullptr)
	{
		// Deliberately does NOT touch the cooldown. Spec §5: "a player can spawn with an ability
		// timer still counting down."
		AbilitySet->OnPawnSpawned();
	}
}

void UTraceAbilityComponent::NotifyPawnDied()
{
	// THE CHARACTER FIRST. Its OnPawnDied() is where world actors it owns are torn down and where its
	// LOCAL mirrors are cleared (Mace's spike, Oyster's jars, X's swarm, Roxie's Modded). Several of
	// those characters re-publish their flags into AbilityState every tick from those mirrors, so a
	// framework wipe that ran first would be overwritten one frame later by a stale mirror.
	if (AbilitySet != nullptr)
	{
		AbilitySet->OnPawnDied();
	}

	// ...THEN THE FRAMEWORK. See ApplyDeathStateWipe: this is spec v19 §4.2's ONE central place.
	ApplyDeathStateWipe();
}

// =================================================================================================
// *** SPEC v19 §4.2 — THE DEATH WIPE. THE ONE PLACE. ***
//
// Verbatim: "E.g chut should not have his ability active when he is dead. It should stop, and the
// cooldown timer should start." And, restated in the same breath: "cooldown timers should still not
// reset when players die, and should just continue ticking down."
//
// So the sentence has two halves that pull in opposite directions, and the whole difficulty of this
// function is that they share one struct. EFFECTS STOP. COOLDOWNS DO NOT.
//
// [DIAGNOSED] CHUT IS EXACTLY RIGHT AND IT IS NOT A CHUT BUG. UTraceAbilitySetChut::OnPawnDied()
// reads, in full, `ResetDashTracking();` — it never clears Chud. Chud lives in the replicated
// AbilityState, AbilityState lives on the component, and the component lives on the PLAYERSTATE,
// which by design survives the pawn (see the cooldown contract at the top of the header). So Chud
// ran on happily through Chut's corpse and into his next life until its ten seconds expired. Rocco's
// headshot speed stack had the same shape. Both are single characters forgetting a line, which is
// precisely why the spec says to fix it centrally: the next character to be written will forget it
// too.
//
// WHICH FIELDS, AND WHY NOT ALL OF THEM. A blanket AbilityState.Reset() was the obvious move and is
// WRONG, and the code says so rather than the reader having to find out:
//
//   Flags bits 0 and 1   CLEARED. TraceAbilityTypes.h names them EffectActive and MovementActive,
//                        and every character's private alias sits on the same two bits (Mace's
//                        Suspending/Pulling, Roxie's ModdedActive, Slimeball's Stuck). They mean
//                        "something of mine is running on my body right now", which is the exact
//                        thing a corpse must not have.
//   EffectEndMatchTime   CLEARED. Its documented job is the deadline of that running effect —
//                        Chud's 10 s, Mace's 1.25 s suspend, Modded's window.
//   Flags bit 2 (Aux)    KEPT. It means "a thing I put in the WORLD is still there" — Rocco's
//                        Ripple, Elle's gates, Slimeball's wall, Oyster's jars, a rocket in flight.
//                        Each character already documents its own [ASSUMPTION] about whether that
//                        outlives its author, and those decisions are theirs, not this function's.
//   Flags bit 3          KEPT, for the same reason: Elle's Charged is a placed pair, not a buff.
//   AuxEndMatchTime      *** KEPT, AND THIS IS THE HALF THAT MATTERS. *** It is a COOLDOWN for at
//                        least two shipped characters — Elle stores Snap's ready time in it and
//                        Roxie stores the rocket's — so clearing it would hand a dead player a free
//                        ability, which is the precise thing the user restated in the same
//                        paragraph. This is why the blanket Reset() is not used.
//   Stacks               KEPT. It is X's Sting clip mirror and Rocco's stack count; Rocco's stack is
//                        already dead because its EffectActive bit is gone, and zeroing X's would
//                        edit ammunition rather than a status.
//   ActivatedCooldown*   NEVER TOUCHED. OnHalfTime() remains the only automatic reset in the whole
//                        framework, exactly as the header's contract promises.
//
// The cooldown "starting" that the user asks for needs no code: TryActivate() sets the deadline at
// ACTIVATION, so Chut's cooldown has been running since he pressed E and simply keeps running.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarAbilityDeathWipe(
	TEXT("Trace.Ability.DeathWipe"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): death stops every ability effect and status a player has "
	     "running, while their cooldowns keep ticking — spec v19 §4.2. 0: removes the wipe so "
	     "Trace.Ability.DeathWipeTest can be shown FAILING (Chut keeps Chud through his own corpse). "
	     "Never ship 0."),
	ECVF_Cheat);

/** Times the central wipe actually stopped something. Read by Trace.Ability.DeathWipeTest. */
static int32 GAbilityDeathWipes = 0;

// Named after this file, and static, because the module builds in unity blobs: an anonymous
// namespace or an externally-linked helper called IsEnabled() would be one merge away from
// colliding with somebody else's.
namespace TraceAbilityDeathWipe
{
	static int32 GetCount() { return GAbilityDeathWipes; }
	static bool  IsEnabled() { return CVarAbilityDeathWipe.GetValueOnAnyThread() != 0; }
}

void UTraceAbilityComponent::ApplyDeathStateWipe()
{
	if (!HasAuthorityOwner() || !TraceAbilityDeathWipe::IsEnabled())
	{
		return;
	}

	// The two bits that mean "an effect of mine is running on my body". See the block above.
	// The cast is not decoration: `uint8 | uint8` promotes to int, and MSVC calls the implicit
	// narrowing back to uint8 a conversion warning that some configurations treat as an error.
	constexpr uint8 RunningBits = static_cast<uint8>(
		TraceAbilityFlags::EffectActive | TraceAbilityFlags::MovementActive);

	const bool bHadSomethingRunning =
		((AbilityState.Flags & RunningBits) != 0) || (AbilityState.EffectEndMatchTime != 0.f);

	if (!bHadSomethingRunning)
	{
		return;   // The overwhelmingly common case: nothing was up. No replication, no log.
	}

	const uint8 FlagsBefore = AbilityState.Flags;
	const float EffectEndBefore = AbilityState.EffectEndMatchTime;

	AbilityState.Flags &= static_cast<uint8>(~RunningBits);
	AbilityState.EffectEndMatchTime = 0.f;
	MarkNetStateDirty();

	++GAbilityDeathWipes;

	UE_LOG(LogTraceGame, Log,
		TEXT("[Ability] spec v19 §4.2 DEATH WIPE: %s (%s) died with an effect running (flags 0x%02x -> ")
		TEXT("0x%02x, effect had %.2fs left) - it is STOPPED. E cooldown untouched and still %.2fs from ")
		TEXT("ready; the second timer (AuxEndMatchTime %.2f) is a cooldown for some characters and is ")
		TEXT("untouched too."),
		*GetNameSafe(GetOwningPlayerState()), TraceCharacterIdToString(CharacterId),
		FlagsBefore, AbilityState.Flags, FMath::Max(0.f, EffectEndBefore - MatchTimeNow()),
		GetActivatedCooldownRemaining(), AbilityState.AuxEndMatchTime);
}

void UTraceAbilityComponent::NotifyKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot)
{
	if (AbilitySet != nullptr && HasAuthorityOwner())
	{
		AbilitySet->OnKill(Victim, Cause, bHeadshot);
	}

	// =============================================================================================
	// DEMO 17 ITEM 7 — "a toggle to test dash cooldown refreshing on every kill".
	//
	// HERE, and not in any character file, because the ask is for the experiment to apply to the
	// GAME: one call site in the framework's kill notification gives all ten characters the same
	// rule, and no ability set has to know the toggle exists.
	//
	// DEFAULT OFF. This is a knob to TRY the idea with, not the idea shipped — with it off this block
	// is one bool read and the game plays exactly as it did.
	//
	// THE TWO REFUSALS ARE THE WHOLE CARE HERE, both copied from the parry's identical grant rather
	// than reasoned about again:
	//   - SERVER ONLY. RefundDashCharge refuses off the authority in any case and mirrors itself onto
	//     the owning client, so the meter moves on the same frame the kill lands.
	//   - NEVER A SELF-KILL. Spec v19 §4.1 has just made "walk out of the arena" an ordinary death
	//     credited to nobody, and NotifyCharacterDied reads a null killer as a self-kill; without this
	//     guard a player could top their dash pool up by repeatedly stepping off the map.
	// A full pool is not an error: RefundDashCharge returns false harmlessly and nothing is logged.
	// =============================================================================================
	if (UTraceSettings::Get().bRefreshDashChargeOnKill
		&& HasAuthorityOwner()
		&& Victim != nullptr
		&& Victim != GetOwningCharacter())
	{
		if (ATraceCharacter* Killer = GetOwningCharacter())
		{
			if (UTraceCharacterMovementComponent* KillerMovement = Killer->GetTraceMovement())
			{
				KillerMovement->RefundDashCharge();
			}
		}
	}
}

float UTraceAbilityComponent::GetMoveSpeedMultiplierFor(const AActor* Actor)
{
	const UTraceCharacterAbilitySet* Set = GetAbilitySetFor(Actor);
	return (Set != nullptr) ? FMath::Max(0.01f, Set->GetMoveSpeedMultiplier()) : 1.f;
}

float UTraceAbilityComponent::GetMagnetRadiusMultiplierFor(const AActor* Actor)
{
	const UTraceCharacterAbilitySet* Set = GetAbilitySetFor(Actor);
	return (Set != nullptr) ? FMath::Max(0.01f, Set->GetMagnetRadiusMultiplier()) : 1.f;
}

float UTraceAbilityComponent::GetFireIntervalScaleFor(const AActor* Actor)
{
	const UTraceCharacterAbilitySet* Set = GetAbilitySetFor(Actor);
	if (Set == nullptr)
	{
		return 1.f;
	}

	// Floored well above zero, and that floor is the safety rail rather than a tuning value: this
	// number divides the gun's minimum interval, so a character (or a mistyped ini) answering 0 would
	// hand that player an unbounded fire rate. 0.05 is a 20x rate cap — twenty times anything spec §2
	// asks for, so it can never bind on a legitimate value. Ceilinged at 10 for the same reason in the
	// other direction: a gun that will not fire for four seconds reads as broken, not as a nerf.
	return FMath::Clamp(Set->GetFireIntervalScale(), 0.05f, 10.f);
}

float UTraceAbilityComponent::GetSlideJumpWindowSpeedBonusFor(const AActor* Actor, float InWellTimedBonus)
{
	const UTraceCharacterAbilitySet* Set = GetAbilitySetFor(Actor);
	if (Set == nullptr)
	{
		return InWellTimedBonus;
	}

	// Never below the global number the movement component just computed. The rule there is that a
	// well-timed hop must never be worth LESS than a mistimed one; a character passive is not allowed
	// to smuggle in an exception to it, however its own knob is tuned.
	return FMath::Max(InWellTimedBonus, Set->ModifySlideJumpWindowSpeedBonus(InWellTimedBonus));
}

#if !UE_BUILD_SHIPPING
void UTraceAbilityComponent::DebugSetActivatedCooldown(float Seconds)
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	ActivatedCooldownEndMatchTime = MatchTimeNow() + FMath::Max(0.f, Seconds);
	PredictedCooldownEndMatchTime = ActivatedCooldownEndMatchTime;
}
#endif

// =================================================================================================
// SPEC v19 §4.2 — THE REPRODUCTION
//
// Verbatim: "E.g chut should not have his ability active when he is dead. It should stop, and the
// cooldown timer should start." Plus the standing rule restated in the same paragraph: "cooldown
// timers should still not reset when players die, and should just continue ticking down."
//
// So it uses the user's own example, end to end and through the shipping path: give somebody Chut,
// press E for real (TryActivate, which is what a key press calls), confirm Chud is UP, kill them,
// and then assert BOTH halves at once — Chud is down, and the cooldown is still counting.
//
// WHY IT CAN GO RED, WHICH IS THE PART THAT MATTERS. Trace.Ability.DeathWipe 0 removes the central
// wipe and nothing else, and the harness then reports FAIL with the seconds of Chud still on the
// corpse — which is the build the project shipped before this pass, because
// UTraceAbilitySetChut::OnPawnDied() never cleared it. Both arms measure the same two numbers.
//
// The two clauses pull against each other on purpose. "Everything stops" is trivially satisfiable by
// wiping the whole state, which would clear the cooldowns too; "cooldowns keep running" is trivially
// satisfiable by wiping nothing. Only a run that reports both can tell those apart.
// =================================================================================================

#if !UE_BUILD_SHIPPING

// Named after this file rather than anonymous: the module builds in unity blobs and a bare
// `namespace {}` here would be one merge away from colliding with another slice's helper.
namespace TraceAbilityDeathWipeVerify
{
	/**
	 * ARM AND WAIT, rather than run-or-refuse, and that is the testing policy rather than politeness:
	 * this harness has to be startable from -ExecCmds, which fires on the very first frame — before
	 * there is a GameState, before the match is InProgress, and before anybody holds a pawn. A command
	 * that gave up there would report INVALID every time it was run the only way it is allowed to be.
	 *
	 * It still gives up eventually, and loudly. "Could not run" is a different result from "passed".
	 */
	constexpr float ArmTimeoutSeconds = 45.f;

	/** Seconds after the kill before the two clauses are read, so the death has cleared the funnel. */
	constexpr float SettleSeconds = 0.25f;

	static UWorld* PlayingWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr && Context.World()->GetAuthGameMode() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	struct FRun
	{
		TWeakObjectPtr<UTraceAbilityComponent> Abilities;
		FString VictimName;
		float EffectRemainingAtDeath = 0.f;
		float CooldownAtDeath = 0.f;
		float SinceKill = 0.f;
		float SinceArmed = 0.f;
		bool  bKilled = false;
		bool  bGaveUp = false;
	};

	/**
	 * Sets a living player to Chut, presses E for real, and kills them.
	 * @return false while the match is not yet in a state where that is possible.
	 */
	static bool TryStartRun(FRun& Run)
	{
		UWorld* const World = PlayingWorld();
		if (World == nullptr || !UTraceAbilityComponent::AreCharactersEnabled(World))
		{
			return false;
		}

		const ATraceGameState* const TraceGS = World->GetGameState<ATraceGameState>();
		if (TraceGS == nullptr
			|| TraceGS->TraceMatchState != ETraceMatchState::InProgress
			|| TraceGS->IsHalfTimeBreak())
		{
			return false;
		}

		// A living pawn that is NOT the carrier: a carrier's own rules are proven elsewhere and would
		// only add a second possible reason for whatever this run reports.
		UTraceAbilityComponent* Abilities = nullptr;
		ATraceCharacter* Victim = nullptr;

		for (APlayerState* PS : TraceGS->PlayerArray)
		{
			UTraceAbilityComponent* const Candidate = UTraceAbilityComponent::Get(PS);
			ATraceCharacter* const Pawn = (Candidate != nullptr) ? Candidate->GetOwningCharacter() : nullptr;

			if (Candidate == nullptr || Pawn == nullptr || !Pawn->IsAlive() || Pawn->IsCarrier())
			{
				continue;
			}

			// Per-TEAM uniqueness (spec §3): whoever is picked has to be ALLOWED to hold Chut.
			if (Candidate->GetCharacterId() == ETraceCharacterId::Chut
				|| UTraceAbilityComponent::IsCharacterAvailableFor(PS, ETraceCharacterId::Chut))
			{
				Abilities = Candidate;
				Victim = Pawn;
				break;
			}
		}

		if (Abilities == nullptr || Victim == nullptr)
		{
			return false;
		}

		Abilities->ServerSetCharacter(ETraceCharacterId::Chut);
		if (Abilities->GetCharacterId() != ETraceCharacterId::Chut)
		{
			return false;   // The roster refused; try again next frame with somebody else.
		}

		// THE SHIPPING PATH. TryActivate() is literally what the E key calls, so this exercises the
		// same activation, the same cooldown write and the same net state a player produces.
		if (!Abilities->TryActivate())
		{
			return false;
		}

		const FTraceAbilityNetState& State = Abilities->GetNetState();
		if ((State.Flags & TraceAbilityFlags::EffectActive) == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[DeathWipeTest] INVALID: E fired on Chut but no effect flag came up, so there is "
				     "nothing to watch stop. NOT a pass."));
			Run.bGaveUp = true;
			return true;   // Stop arming; the run is over and it did not pass.
		}

		const float MatchNow = static_cast<float>(TraceGS->GetServerWorldTimeSeconds());

		Run.Abilities = Abilities;
		Run.VictimName = GetNameSafe(Victim);
		Run.EffectRemainingAtDeath = FMath::Max(0.f, State.EffectEndMatchTime - MatchNow);
		Run.CooldownAtDeath = Abilities->GetActivatedCooldownRemaining();

		UE_LOG(LogTraceGame, Display,
			TEXT("[DeathWipeTest] ===== %s is Chut with CHUD UP (%.2fs left) and an E cooldown of %.2fs ")
			TEXT("already running. Killing them now. | rule = %s ====="),
			*Run.VictimName, Run.EffectRemainingAtDeath, Run.CooldownAtDeath,
			TraceAbilityDeathWipe::IsEnabled()
				? TEXT("v19 §4.2 ON") : TEXT("OFF - THE RED ARM, ARMED"));

		UTraceHealthComponent* const Health = Victim->FindComponentByClass<UTraceHealthComponent>();
		if (Health == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[DeathWipeTest] INVALID: the victim has no health component to kill. NOT a pass."));
			Run.bGaveUp = true;
			return true;
		}

		Health->Kill(nullptr, FName(TEXT("DeathWipeTest")));
		Run.bKilled = true;
		return true;
	}

	/** Reads the two clauses and prints the verdict. @return false when the run is finished. */
	static bool ReportRun(FRun& Run)
	{
		UTraceAbilityComponent* const Abilities = Run.Abilities.Get();
		const UWorld* const World = PlayingWorld();
		const AGameStateBase* const Clock = (World != nullptr) ? World->GetGameState() : nullptr;

		if (Abilities == nullptr || Clock == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[DeathWipeTest] INVALID: the world or the player went away mid-run. NOT a pass."));
			return false;
		}

		const float MatchNow = static_cast<float>(Clock->GetServerWorldTimeSeconds());
		const FTraceAbilityNetState& State = Abilities->GetNetState();

		const float EffectLeft = FMath::Max(0.f, State.EffectEndMatchTime - MatchNow);
		const bool bStillRunning =
			((State.Flags & TraceAbilityFlags::EffectActive) != 0) && (EffectLeft > 0.f);
		const float CooldownNow = Abilities->GetActivatedCooldownRemaining();

		// THE TWO CLAUSES, ASSERTED TOGETHER, because neither means anything alone: "everything stops"
		// is trivially satisfied by wiping the whole state (which would clear the cooldowns the user
		// explicitly said must survive), and "cooldowns keep running" is trivially satisfied by wiping
		// nothing at all. Only a run that reports both can tell a fix from either failure.
		const bool bEffectStopped = !bStillRunning;
		const bool bCooldownKeptTicking = (CooldownNow > 0.f) && (CooldownNow < Run.CooldownAtDeath);
		const bool bPass = bEffectStopped && bCooldownKeptTicking;

		const FString Detail = FString::Printf(
			TEXT("%s died with %.2fs of Chud left. AFTER the death: Chud still active = %d (must be 0 - ")
			TEXT("\"chut should not have his ability active when he is dead\"), %.2fs of it still on the ")
			TEXT("clock | E cooldown %.2fs -> %.2fs (must be > 0 and FALLING - \"cooldown timers should ")
			TEXT("still not reset when players die\") | central wipes this session %d | rule = %s"),
			*Run.VictimName, Run.EffectRemainingAtDeath, bStillRunning ? 1 : 0, EffectLeft,
			Run.CooldownAtDeath, CooldownNow, TraceAbilityDeathWipe::GetCount(),
			TraceAbilityDeathWipe::IsEnabled()
				? TEXT("v19 §4.2 ON") : TEXT("OFF - THE RED ARM, ARMED"));

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[DeathWipeTest] PASS: %s"), *Detail);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[DeathWipeTest] FAIL: %s"), *Detail);
		}
		return false;
	}

	FAutoConsoleCommand CmdDeathWipeVerify(
		TEXT("Trace.Ability.DeathWipeTest"),
		TEXT("SPEC v19 §4.2. Gives a living player Chut, presses E for real, confirms Chud is UP, kills "
		     "them, and then asserts BOTH halves of the rule at once: Chud stopped, and the E cooldown "
		     "kept ticking down. Waits for the match to go live, so it is safe from -ExecCmds. Run it "
		     "again with Trace.Ability.DeathWipe 0, which is the arm that must go RED."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			TSharedRef<FRun> Run = MakeShared<FRun>();

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Run](float DeltaTime) -> bool
				{
					if (Run->bGaveUp)
					{
						return false;
					}

					if (!Run->bKilled)
					{
						Run->SinceArmed += DeltaTime;

						if (TryStartRun(*Run))
						{
							return !Run->bGaveUp;
						}

						if (Run->SinceArmed >= ArmTimeoutSeconds)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[DeathWipeTest] INVALID: gave up after %.0fs waiting for a live match with ")
								TEXT("somebody alive who could be given Chut and press E. NOT a pass - it could ")
								TEXT("not run."),
								Run->SinceArmed);
							return false;
						}
						return true;
					}

					Run->SinceKill += DeltaTime;
					if (Run->SinceKill < SettleSeconds)
					{
						return true;
					}

					return ReportRun(*Run);
				}),
				0.f);
		}));
}

#endif // !UE_BUILD_SHIPPING
