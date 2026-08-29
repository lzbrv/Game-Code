// Trace — Oyster's poison. See the header for why the choke point is re-asked every tick.

#include "Abilities/Characters/TraceOysterPoison.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceOysterJar.h"   // TraceOysterJar::IsLegacyE — spec v26 §6's red arm
#include "Abilities/Characters/TraceAbilitySetElle.h"   // IsCloakVisualApplied — §1.2's cloak rule
#include "Audio/TraceAudio.h"
#include "Audio/TraceSoundEvents.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceFxBurst.h"                 // ATraceFxBurst + TraceFxLoopBudget (§1.3, §1.4)
#include "Gameplay/TraceFxShapes.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

#if !UE_BUILD_SHIPPING
// Harness only — see "THE EVIDENCE" at the foot of this file. (CapsuleComponent.h moved up: the §2.6
// drip reads the victim's capsule to find chest and feet, so it is needed in every configuration.)
#include "Containers/Ticker.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"                 // FScreenshotRequest

#include "Camera/CameraActor.h"
#include "Abilities/Characters/TraceAbilitySetOyster.h"
// TraceOysterJar.h is now included unconditionally above — spec v26 §6's red arm needs it in every
// configuration, not only in the harness ones.
#endif

// =================================================================================================
// The alarms
// =================================================================================================

namespace TraceOyster
{
	namespace
	{
		FEffectTally GCarrierTally;
		FEffectTally GOtherTally;
	}

	FEffectTally& CarrierTally() { return GCarrierTally; }
	FEffectTally& OtherTally()   { return GOtherTally; }

	void ResetTallies()
	{
		GCarrierTally.Reset();
		GOtherTally.Reset();
	}

	void RecordEffect(const ATraceCharacter* Target, const TCHAR* VectorName, int32 FEffectTally::* Field)
	{
		if (Field == nullptr)
		{
			return;
		}

		if (UTraceAbilityComponent::IsCarrier(Target))
		{
			++(GCarrierTally.*Field);

			// LOUD, and naming the vector. Reaching here means one of Oyster's four ways to touch a
			// player got past spec §4's rule; the whole point of counting per-vector is that the log
			// says WHICH one.
			UE_LOG(LogTraceGame, Error,
				TEXT("[Oyster] *** '%s' RESOLVED ONTO THE CORE CARRIER %s. Spec v14 §4: no ability may damage or "
				     "control a carrier. Check Trace.Ability.CarrierImmune and CanAffectTargetDetailed. ***"),
				VectorName, *GetNameSafe(Target));
		}
		else
		{
			++(GOtherTally.*Field);
		}
	}
}

// =================================================================================================
// SPEC v26 §6a — "CHANGE OYSTER'S E COOLDOWN TO RESET EVERYTIME HE POISONS SOMEONE"
// =================================================================================================
//
// Named after the file rather than anonymous: two anonymous namespaces merged into one unity
// translation unit is MSVC C2084 on Windows only, and Scripts/check-jumbo-build-collisions.py gates
// on the name being here.
namespace TraceOysterPoisonFile
{
	/**
	 * SPEC v28 §4's RED ARM — the v26 "someone must be on an enemy TEAM" test, restorable live.
	 *
	 * 0 (shipped): "someone" is anybody this poison landed on who is not Oyster himself and not his
	 *              team-mate. A practice-range dummy (ETraceTeam::None) therefore refunds the E, and
	 *              so does any other teamless pawn a mode might add.
	 * 1:           the v26 test — both sides must have a team and they must differ. On the practice
	 *              range this refuses EVERY target in the mode, which is the reported bug, and it is
	 *              here so Trace.Oyster.EPressRepro can show it failing and then passing in one run.
	 *
	 * NEVER SHIP 1.
	 */
	static TAutoConsoleVariable<int32> CVarOysterLegacyRefundTeamTest(
		TEXT("Trace.Oyster.LegacyRefundTeamTest"),
		0,
		TEXT("TEST ARM ONLY. 0 (shipped, spec v28 §4): poisoning anybody who is not Oyster and not his\n")
		TEXT("   team-mate resets his E — including a teamless pawn such as a practice-range dummy.\n")
		TEXT("1: the v26 test, which required the victim to be on an opposing TEAM and therefore never\n")
		TEXT("   refunded anything in the practice range. The reported bug. Never ship 1."),
		ECVF_Cheat);

	static bool IsLegacyRefundTeamTest()
	{
		return CVarOysterLegacyRefundTeamTest.GetValueOnAnyThread() != 0;
	}

	/**
	 * The refund, applied at THE ONE PLACE A POISON BEGINS.
	 *
	 * WHY IT LIVES IN ApplyTo AND NOT IN THE JAR. §6a says "everytime he poisons someone", and
	 * ApplyTo is the only function in the project that can make that true of every route at once:
	 * the dash jar's burst, the Pickler jar's detonation, and the harness all arrive here. Putting it
	 * in ATraceOysterJar::Burst() would have covered two of the three and read as if it did cover all
	 * of them.
	 *
	 * "EVERYTIME", INCLUDING A REFRESH. ApplyTo is called once per victim per burst whether or not
	 * that victim was already poisoned, and the refund is unconditional here for the same reason —
	 * re-poisoning somebody who is already green is still poisoning them. What it is NOT is once per
	 * poison TICK: a tick is the poison working, not Oyster poisoning anybody, and paying per tick
	 * would mean E was never on cooldown at all while anything was alive and poisoned.
	 *
	 * "SOMEONE" IS AN ENEMY. Read narrowly on purpose. The choke point already refuses self and
	 * refuses team-mates while friendly fire is off, but friendly fire is a knob, and a build with it
	 * on must not turn "dash past your own team" into an infinite E. The team test is here rather
	 * than left to the choke point because it is a DIFFERENT question from "may I poison them".
	 *
	 * AND IT IS OYSTER'S COOLDOWN, checked rather than assumed: the source component is asked what
	 * character it is running. A poison that outlives its Oyster — he died, left, or swapped
	 * character — must not hand whoever holds that component a free ability of a different name.
	 */
	/**
	 * *** SPEC v28 §4 — EVERY REFUSAL NAMES ITSELF, OUT LOUD, AT Log. ***
	 *
	 * v26 shipped this function silent. When the owner reported the refund still not working there
	 * was therefore no way to tell the three possible answers apart from a log — absent (never
	 * called), wrong path (called and refused by one of the four guards), or fired and overwritten
	 * (called, accepted, and the cooldown re-armed afterwards) — and those three have completely
	 * different fixes. One line per poison is not spam: a poison is a discrete event, and the line
	 * only exists on the authority.
	 *
	 * The line prints the cooldown BEFORE and AFTER the call, which is what separates "fired" from
	 * "fired and something else undid it": if before is 20.0 and after is 0.0 and the player still
	 * sees a running ring, the fault is downstream of here and not in this file.
	 */
	static void RefundPicklerForPoisoning(const ATraceCharacter* Victim, UTraceAbilityComponent* SourceComp)
	{
		const auto Refuse = [Victim, SourceComp](const TCHAR* Why)
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[Oyster] §6a REFUND REFUSED (%s): victim=%s source=%s"),
				Why, *GetNameSafe(Victim), *GetNameSafe(SourceComp));
		};

		if (Victim == nullptr || SourceComp == nullptr)
		{
			Refuse(TEXT("no victim, or the poison has no source component"));
			return;
		}

		// The pre-v26 arm: no refund at all. See TraceOysterJar::IsLegacyE.
		if (TraceOysterJar::IsLegacyE())
		{
			Refuse(TEXT("Trace.Oyster.LegacyE is 1 — the pre-v26 arm is armed"));
			return;
		}

		if (SourceComp->GetCharacterId() != ETraceCharacterId::Oyster)
		{
			Refuse(TEXT("the source component is not running Oyster"));
			return;
		}

		// =========================================================================================
		// *** SPEC v28 §4 — THIS TEST IS THE BUG. "Oyster's E is not resetting when he poisons
		//     someone", reported delivered in Demo 23 and still true from the player's side. ***
		//
		// v26 shipped:
		//
		//     if (SourceTeam == None || VictimTeam == None || VictimTeam == SourceTeam) return;
		//
		// The middle clause is the one that fails, and it fails on the ONE fixture a player actually
		// uses to check an ability. The practice range's five dummies are deliberately
		// ETraceTeam::None (see TracePracticeActors.h, "WHY THE DUMMIES ARE ON ETraceTeam::None"),
		// and the ability choke point only ever refuses a MATCHING team — so a dummy is a perfectly
		// legal poison victim: it turns green, it takes the 3-per-half-second, it is slowed 30%.
		// Every visible sign says "I poisoned someone". And then this line threw the refund away
		// because the someone had no team, so the E ring kept counting down. The rule was never
		// absent and never on the wrong path: it fired, reached here, and refused.
		//
		// It also explains why it was reported delivered and why Trace.Oyster.ETest is green: that
		// harness poisons a BOT on the opposing team, which is the one victim shape the old test
		// accepts. A harness that only ever staged the passing case could not see this.
		//
		// THE RULE NOW, and it keeps the whole of v26's reasoning: "someone" is ANYBODY THIS POISON
		// ACTUALLY LANDED ON WHO IS NOT ME AND NOT MY TEAM-MATE. Written as two refusals rather than
		// as one positive test, because the thing being guarded against is specific and stays
		// guarded:
		//   * SELF — poisoning yourself is not poisoning someone;
		//   * A TEAM-MATE — this is v26's friendly-fire guard, unchanged and still the reason the
		//     test exists at all: with bFriendlyFire on, "dash past your own team" must not become an
		//     infinite E.
		// Everything else — an enemy, a practice dummy, any teamless pawn a future mode adds — is
		// somebody Oyster poisoned, because ApplyTo only reaches this line for a victim the choke
		// point already allowed him to poison.
		//
		// Trace.Oyster.LegacyRefundTeamTest 1 restores the v26 test in the same binary, which is what
		// Trace.Oyster.EPressRepro's red arm runs.
		// =========================================================================================
		if (Victim == SourceComp->GetOwningCharacter())
		{
			Refuse(TEXT("the victim is the source's own pawn"));
			return;
		}

		const ETraceTeam SourceTeam = SourceComp->GetTeam();
		const ETraceTeam VictimTeam = Victim->GetTeam();

		if (SourceTeam != ETraceTeam::None && VictimTeam == SourceTeam)
		{
			Refuse(TEXT("the victim is a team-mate (friendly fire is on) — v26's guard, unchanged"));
			return;
		}

		if (TraceOysterPoisonFile::IsLegacyRefundTeamTest()
			&& (SourceTeam == ETraceTeam::None || VictimTeam == ETraceTeam::None))
		{
			Refuse(TEXT("Trace.Oyster.LegacyRefundTeamTest is 1 — the v26 test refuses a teamless victim, "
			            "which is every practice-range dummy. THIS IS THE REPORTED BUG."));
			return;
		}

		const float Before = SourceComp->GetActivatedCooldownRemaining();
		SourceComp->ServerResetActivatedCooldown(TEXT("Oyster poisoned an enemy — spec v26 §6a"));
		const float After = SourceComp->GetActivatedCooldownRemaining();

		UE_LOG(LogTraceGame, Log,
			TEXT("[Oyster] §6a REFUND APPLIED: %s poisoned %s; E cooldown %.1fs -> %.1fs."),
			*GetNameSafe(SourceComp->GetOwner()), *GetNameSafe(Victim), Before, After);
	}
}

// =================================================================================================
// The component
// =================================================================================================

UTraceOysterPoisonComponent::UTraceOysterPoisonComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// AFTER physics: the clamp is a correction to the velocity the movement step just produced, and
	// clamping before it would be overwritten by the same step it is trying to limit.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	// SetIsReplicatedByDefault, NOT SetIsReplicated — same reason as UTraceAbilityInputRelay's
	// constructor: SetIsReplicated during CDO construction trips a handled ensure at engine init on
	// every run. The runtime call in EnsureOn below is the legitimate case and is unchanged.
	SetIsReplicatedByDefault(true);
}

void UTraceOysterPoisonComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UTraceOysterPoisonComponent, EndMatchTime);
	DOREPLIFETIME(UTraceOysterPoisonComponent, bSlowActive);
}

ATraceCharacter* UTraceOysterPoisonComponent::GetVictim() const
{
	return Cast<ATraceCharacter>(GetOwner());
}

float UTraceOysterPoisonComponent::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

UTraceOysterPoisonComponent* UTraceOysterPoisonComponent::Find(const ATraceCharacter* Target)
{
	return (Target != nullptr) ? Target->FindComponentByClass<UTraceOysterPoisonComponent>() : nullptr;
}

UTraceOysterPoisonComponent* UTraceOysterPoisonComponent::ApplyTo(ATraceCharacter* Target,
                                                                  UTraceAbilityComponent* SourceComp)
{
	if (Target == nullptr || !Target->HasAuthority() || !Target->IsAlive())
	{
		return nullptr;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Now = (Target->GetWorld() != nullptr && Target->GetWorld()->GetGameState() != nullptr)
		? static_cast<float>(Target->GetWorld()->GetGameState()->GetServerWorldTimeSeconds())
		: 0.f;

	UTraceOysterPoisonComponent* Poison = Target->FindComponentByClass<UTraceOysterPoisonComponent>();
	const bool bFresh = (Poison == nullptr);

	if (bFresh)
	{
		Poison = NewObject<UTraceOysterPoisonComponent>(Target, UTraceOysterPoisonComponent::StaticClass(),
			TEXT("TraceOysterPoison"));

		// SetIsReplicated BEFORE RegisterComponent, for the reason the ability framework's own
		// attachment documents: the flag is sampled at registration.
		Poison->SetIsReplicated(true);
		Poison->RegisterComponent();
	}

	// REFRESH, NOT STACK. §6 gives one duration and one damage number and says nothing about two
	// jars; a second application resetting the clock is the same rule X's vulnerable states
	// explicitly ("a new application resets the timer") and is the conservative reading.
	Poison->EndMatchTime = Now + FMath::Max(0.25f, Settings.OysterPoisonDurationSeconds);
	Poison->SourceComponent = SourceComp;

	if (bFresh)
	{
		// The first tick lands one interval in, so 4 s at 0.5 s is 8 ticks of 3 = 24 damage, which is
		// what "3 damage every 0.5 s for 4 s" reads as.
		Poison->NextTickMatchTime = Now + FMath::Max(0.05f, Settings.OysterPoisonTickIntervalSeconds);
		Poison->DamageDealtSoFar = 0.f;
	}

	TraceOyster::RecordEffect(Target, TEXT("poison applied"), &TraceOyster::FEffectTally::PoisonApplications);

	// SPEC v26 §6a. LAST, and only on the success path: everything above can still bail out, and a
	// refund for a poison that was never applied would be a free E for missing.
	TraceOysterPoisonFile::RefundPicklerForPoisoning(Target, SourceComp);

	return Poison;
}

void UTraceOysterPoisonComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (GetOwner() != nullptr && GetOwner()->HasAuthority())
	{
		TickAuthority();
		if (!IsValid(this))
		{
			return;   // TickAuthority destroys the component when the poison is over
		}
	}

	ApplySlowClamp();

	// EVERY MACHINE, and after the authority half so a component that just expired does not spend a
	// frame drawing drips for a poison that is over. See the header block on UpdateDripFx.
	DripElapsed += DeltaTime;
	UpdateDripFx();
}

// =================================================================================================
// FX_AUDIO_PLAN §2.6 — THE VICTIM DRIP
//
// "3 spheres r 6 uu, additive Poisoned I 0.4, spawning at CHEST and falling to FEET over 0.4 s,
//  staggered, while poisoned."
//
// Three pieces, one phase each, 1/3 of the cycle apart, so at any instant the victim has a sphere
// near the chest, one mid-fall and one at the ankles — a continuous drip rather than three balls
// that blink together. The fall is the ONLY animation: intensity is constant at 0.4 apart from a
// short fade-in at the top and a fade-out at the bottom, because a piece that popped into existence
// at the chest and vanished at the feet would strobe at 2.5 Hz.
//
// WHY THE FALL IS ALLOWED AND A BRIGHTNESS PULSE WOULD NOT BE: FX_AUDIO_PLAN §1.4 permits while-active
// loops to "bob/rotate (motion)" and forbids a brightness pulse only where the state is a LETHAL
// TELEGRAPH. Poison-on-a-victim is a status readout, not a kill volume — the kill volume is the cloud
// back at the jar, and that one holds a constant brightness for exactly this reason.
// =================================================================================================

namespace TraceOysterDripTuning
{
	/** §2.6: three of them. */
	constexpr int32 NumDrips = 3;

	/** §2.6: r 6 uu. 12 uu across, comfortably over ART_BIBLE §3.4's 8 uu floor. */
	constexpr float DripRadiusUU = 6.f;

	/** §2.6: additive I 0.4. TraceFxLoopBudget::AttachLoopPrimitive clamps to §1.4's 0.5 regardless. */
	constexpr float DripIntensity = 0.4f;

	/** §2.6: chest to feet in 0.4 s, per drip. Three drips, staggered, so one lands every 0.133 s. */
	constexpr float FallSeconds = 0.4f;

	/** Where a drip is born, as a fraction of the capsule half-height above the pawn origin. */
	constexpr float ChestFraction = 0.45f;

	/**
	 * The top and bottom of the fall are cross-faded over this fraction of it.
	 *
	 * Not a taste call: the alternative is a piece that appears and disappears, and a hard on/off at
	 * 2.5 Hz is a brightness pulse in everything but name. ART_BIBLE §6.4's "never a pop-out" applied
	 * to the smallest effect in the plan.
	 */
	constexpr float EdgeFadeFraction = 0.2f;
}

int32 UTraceOysterPoisonComponent::GetDripPieceCount() const
{
	int32 Count = 0;
	for (const UStaticMeshComponent* Piece : DripPieces)
	{
		if (Piece != nullptr && IsValid(Piece))
		{
			++Count;
		}
	}
	return Count;
}

void UTraceOysterPoisonComponent::DetachDripFx()
{
	if (DripPieces.Num() == 0)
	{
		return;
	}

	// Through the budget helper, not through DestroyComponent, because the helper owns the per-pawn
	// registration as well as the component. FX_AUDIO_PLAN §8.9: "no FX component survives its pawn",
	// and Trace.Fx.LoopBudget is what reports a kit that forgot.
	APawn* Pawn = Cast<APawn>(GetOwner());
	for (UStaticMeshComponent* Piece : DripPieces)
	{
		TraceFxLoopBudget::DetachLoopPrimitive(Pawn, Piece);
	}

	DripPieces.Reset();
	DripMIDs.Reset();
}

void UTraceOysterPoisonComponent::UpdateDripFx()
{
	// A dedicated server cooks no shaders and has nobody to show them to. It still runs every line of
	// the poison's rules above; this is the only part of the component it skips.
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr || !Victim->IsAlive())
	{
		DetachDripFx();
		return;
	}

	// *** ELLE'S CLOAK. FX_AUDIO_PLAN §1.2's last rule, applied to a debuff nobody thought about. ***
	//
	// The rule is written for a kit's own attached FX; a VICTIM's status FX is the case that actually
	// bites, because the victim did not choose to be lit up. A cloaked Elle who walks through a poison
	// cloud would otherwise be outlined by three glowing green spheres — her ability defeated by
	// somebody else's passive, on every machine, with no way for her to know. The pieces are plainly
	// HIDDEN rather than detached, so they resume the instant she decloaks with no rebuild and no
	// budget churn.
	bool bHiddenByCloak = false;
	if (const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Victim))
	{
		if (const UTraceAbilitySetElle* Elle = Comp->GetAbilitySetAs<UTraceAbilitySetElle>())
		{
			bHiddenByCloak = Elle->IsCloakVisualApplied();
		}
	}

	const UCapsuleComponent* Capsule = Victim->GetCapsuleComponent();
	if (Capsule == nullptr)
	{
		return;
	}
	const float HalfHeightUU = Capsule->GetScaledCapsuleHalfHeight();

	// ---- build once ------------------------------------------------------------------------------
	if (DripPieces.Num() == 0)
	{
		UStaticMesh* Sphere = UTraceFxShapes::GetSphere();
		if (Sphere == nullptr)
		{
			return;   // no mesh, no drips. The poison is unaffected; it always was.
		}

		DripPieces.SetNum(TraceOysterDripTuning::NumDrips);
		DripMIDs.SetNum(TraceOysterDripTuning::NumDrips);

		for (int32 Index = 0; Index < TraceOysterDripTuning::NumDrips; ++Index)
		{
			// THE §1.4 CHOKE POINT, not a hand-rolled NewObject. It enforces additive-only, the 0.5
			// intensity ceiling and the capsule footprint, and it REFUSES the fifth primitive on a pawn
			// — which is the case that matters here, because a victim can be poisoned AND slimed at the
			// same time and the SLOWED ring is the fourth. A refusal is survivable: fewer drips.
			DripPieces[Index] = TraceFxLoopBudget::AttachLoopPrimitive(
				Victim, Victim->GetRootComponent(), Sphere,
				*FString::Printf(TEXT("OysterDrip%d"), Index),
				ATraceOysterPoisonCloud::GetPoisonedHue(), TraceOysterDripTuning::DripIntensity,
				FVector::ZeroVector, TraceOysterDripTuning::DripRadiusUU, DripMIDs[Index]);
		}

		// Compacted so GetDripPieceCount, the loop below and the harness all agree on what exists. A
		// null slot is a refusal the budget already logged; it is not an error here.
		for (int32 Index = DripPieces.Num() - 1; Index >= 0; --Index)
		{
			if (DripPieces[Index] == nullptr)
			{
				DripPieces.RemoveAt(Index);
				DripMIDs.RemoveAt(Index);
			}
		}

		if (DripPieces.Num() == 0)
		{
			DripMIDs.Reset();
			return;
		}

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Oyster] Poison drips attached to %s: %d of %d piece(s), r %.0f uu, additive I %.2f."),
			*GetNameSafe(Victim), DripPieces.Num(), TraceOysterDripTuning::NumDrips,
			TraceOysterDripTuning::DripRadiusUU, TraceOysterDripTuning::DripIntensity);
	}

	// ---- animate ---------------------------------------------------------------------------------
	const float ChestZ = TraceOysterDripTuning::ChestFraction * HalfHeightUU;
	const float FeetZ  = -HalfHeightUU + TraceOysterDripTuning::DripRadiusUU;
	const int32 Count  = DripPieces.Num();

	for (int32 Index = 0; Index < Count; ++Index)
	{
		UStaticMeshComponent* Piece = DripPieces[Index];
		if (Piece == nullptr)
		{
			continue;
		}

		Piece->SetVisibility(!bHiddenByCloak);
		if (bHiddenByCloak)
		{
			continue;   // hidden, but still ticking its phase, so decloaking does not restart the fall
		}

		// The stagger. Phase is per-piece and constant, so three drips are evenly spread down the fall
		// however many of them the budget actually granted.
		const float Phase = FMath::Frac(
			DripElapsed / TraceOysterDripTuning::FallSeconds + static_cast<float>(Index) / static_cast<float>(Count));

		// A little side-to-side so three drips are not one column. Derived from the index, not random:
		// a client and the server draw the identical drip without either of them sending anything.
		const float Yaw = 2.f * PI * (static_cast<float>(Index) / static_cast<float>(Count));
		const float Lateral = 0.55f * TraceOysterDripTuning::DripRadiusUU;

		Piece->SetRelativeLocation(FVector(
			Lateral * FMath::Cos(Yaw),
			Lateral * FMath::Sin(Yaw),
			FMath::Lerp(ChestZ, FeetZ, Phase)));

		// Cross-faded at both ends — see EdgeFadeFraction. Never above DripIntensity, which is itself
		// below §1.4's ceiling; this only ever writes intensity DOWN, which the budget helper permits.
		const float Edge = TraceOysterDripTuning::EdgeFadeFraction;
		const float Fade = FMath::Min(FMath::Min(Phase, 1.f - Phase) / Edge, 1.f);

		if (UMaterialInstanceDynamic* MID = DripMIDs.IsValidIndex(Index) ? DripMIDs[Index].Get() : nullptr)
		{
			UTraceFxShapes::SetGlow(MID, ETraceFxBlend::Additive,
				ATraceOysterPoisonCloud::GetPoisonedHue(),
				TraceOysterDripTuning::DripIntensity * Fade);
		}
	}
}

void UTraceOysterPoisonComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DetachDripFx();
	Super::EndPlay(EndPlayReason);
}

void UTraceOysterPoisonComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// BOTH paths, because they are different paths: TickAuthority calls DestroyComponent() (which
	// reaches OnComponentDestroyed and, for a registered component, EndPlay too), while a client loses
	// this component to replication and a level teardown loses it to EndPlay. DetachDripFx is
	// idempotent, so whichever arrives second does nothing.
	DetachDripFx();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UTraceOysterPoisonComponent::TickAuthority()
{
	ATraceCharacter* Victim = GetVictim();
	const float Now = MatchTimeNow();

	if (Victim == nullptr || !Victim->IsAlive() || Now >= EndMatchTime)
	{
		bSlowActive = false;
		DestroyComponent();
		return;
	}

	UTraceAbilityComponent* SourceComp = SourceComponent.Get();
	AActor* SourceActor = (SourceComp != nullptr) ? SourceComp->GetOwner() : nullptr;

	// *** THE CHOKE POINT, RE-ASKED EVERY TICK. See the header. ***
	//
	// The static form is used rather than the instance form so that a poison whose Oyster has died,
	// left or changed character still has the carrier rule applied to it — the static form applies
	// MayAbilityAffectCarrier even with a null instigator, which is exactly the "orphaned effect"
	// case it was written for.
	const bool bControlAllowed = UTraceAbilityComponent::CanAffect(SourceActor, Victim, ETraceAbilityEffect::Control);
	const bool bDamageAllowed  = UTraceAbilityComponent::CanAffect(SourceActor, Victim, ETraceAbilityEffect::Damage);

	bSlowActive = bControlAllowed;
	if (bSlowActive)
	{
		TraceOyster::RecordEffect(Victim, TEXT("poison slow"), &TraceOyster::FEffectTally::SlowFrames);
	}

	const float Interval = FMath::Max(0.05f, UTraceSettings::Get().OysterPoisonTickIntervalSeconds);
	while (Now >= NextTickMatchTime)
	{
		NextTickMatchTime += Interval;

		if (!bDamageAllowed || SourceComp == nullptr)
		{
			continue;   // the tick is SKIPPED, not deferred: a carrier does not bank up poison damage
		}

		// Through ApplyAbilityDamage, which is the framework's one damage path and asks the choke
		// point again itself. Two independent refusals for the same rule is the intent, not waste.
		const float Dealt = SourceComp->ApplyAbilityDamage(
			Victim, FMath::Max(0.f, UTraceSettings::Get().OysterPoisonDamagePerTick), TEXT("OysterPoison"));

		if (Dealt > 0.f)
		{
			DamageDealtSoFar += Dealt;
			TraceOyster::RecordEffect(Victim, TEXT("poison tick"), &TraceOyster::FEffectTally::PoisonTicks);
		}
	}
}

float UTraceOysterPoisonComponent::GetSpeedMultiplier() const
{
	if (!bSlowActive)
	{
		return 1.f;
	}

	// Clamped at 0.95 for the same reason the clamp below was: a knob of 1.0 would be a total stop,
	// which is not a slow — it is a stun, and §6 does not ask for one.
	const float Fraction = FMath::Clamp(UTraceSettings::Get().OysterPoisonSlowFraction, 0.f, 0.95f);
	return 1.f - Fraction;
}

void UTraceOysterPoisonComponent::ApplySlowClamp()
{
	if (!bSlowActive)
	{
		return;
	}

	ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr || !Victim->IsAlive())
	{
		return;
	}

	// Only the machines that actually simulate this pawn. A simulated proxy's velocity is replicated;
	// clamping it there would fight the interpolation and would slow somebody else's view of a player
	// the server never slowed.
	if (!Victim->HasAuthority() && !Victim->IsLocallyControlled())
	{
		return;
	}

	UTraceCharacterMovementComponent* MoveComp = Victim->GetTraceMovement();
	if (MoveComp == nullptr)
	{
		return;
	}

	// [ASSUMPTION], and a load-bearing one. §6 says "-30% speed for 4 s" and nothing more.
	//
	// NOT DURING A DASH: a dash is velocity on rails for its window and GetMaxSpeed() returns
	// DashSpeed while it runs, so clamping there would make the poison a dash nerf of a completely
	// different magnitude than 30%.
	//
	// GROUND ONLY: the air ceilings in this project are the momentum model (soft cap, hard cap,
	// falloff), and clamping planar air speed to 70% of the WALK speed would not be a slow, it would
	// delete air momentum entirely — a far bigger effect than the doc describes. So the poison is a
	// ground-speed debuff. Both halves are flagged in the report as reversible tuning decisions.
	if (MoveComp->IsFalling() || Victim->IsDashing())
	{
		return;
	}

	// THE FRACTION IS NOT RE-APPLIED HERE. As of the v14 integration,
	// UTraceCharacterMovementComponent::GetMaxSpeed() already multiplies by GetSpeedMultiplier()
	// (through TraceAbilityDebuff::GetMoveSpeedMultiplier), so GetMaxSpeed() IS the slowed ceiling.
	// Multiplying by (1 - fraction) a second time here would compound 0.70 into 0.49 and quietly
	// turn a -30% slow into a -51% one. This clamp's job is now only to stop the acceleration model
	// overshooting a ceiling it already knows about.
	const float Limit = FMath::Max(1.f, MoveComp->GetMaxSpeed());

	FVector Vel = MoveComp->Velocity;
	const FVector Planar(Vel.X, Vel.Y, 0.f);
	const float PlanarSpeed = Planar.Size();
	if (PlanarSpeed > Limit && PlanarSpeed > KINDA_SMALL_NUMBER)
	{
		const FVector Capped = Planar * (Limit / PlanarSpeed);
		Vel.X = Capped.X;
		Vel.Y = Capped.Y;
		MoveComp->Velocity = Vel;
	}
}

// =================================================================================================
// THE CLOUD (spec v16 §3). See the class comment in the header for the shape, the material and the
// replication model; what follows is the arithmetic.
// =================================================================================================

namespace TraceOysterCloudTuning
{
	/**
	 * The silhouette is EXACTLY the burst radius, and this is where that is enforced:
	 *
	 *     ShellDistanceFraction + ShellPuffRadiusFraction == 1.0
	 *
	 * so the outermost surface of the outermost puff sits at 1.0 × R and not a unit further. Change
	 * one of these and change the other, or the cloud stops being "the radius of the explosion".
	 */
	constexpr float ShellDistanceFraction   = 0.60f;
	constexpr float ShellPuffRadiusFraction = 0.40f;
	static_assert(ShellDistanceFraction + ShellPuffRadiusFraction > 0.999f
		&& ShellDistanceFraction + ShellPuffRadiusFraction < 1.001f,
		"Spec v16 §3: the cloud must reach exactly the burst radius, no more and no less.");

	/** Puffs on the shell. Twelve closes the ball (each covers a ~42° cap) while staying visibly lumpy. */
	constexpr int32 NumShellPuffs = 12;

	/** One more in the middle, so the cloud is densest where the jar was rather than a hollow bubble. */
	constexpr float CorePuffRadiusFraction = 0.45f;

	constexpr int32 NumPuffs = NumShellPuffs + 1;

	/** π(3−√5). The golden angle: it is what makes NumShellPuffs points sit EVENLY on a sphere. */
	constexpr float GoldenAngleRadians = 2.39996323f;

	/**
	 * Brightness added per additive layer, and there are a handful of layers along any one ray.
	 *
	 * *** THIS NUMBER WAS MEASURED, NOT REASONED ABOUT, AND THE DIFFERENCE MATTERED. *** It was 0.05
	 * first — a value picked as "faint" in the abstract — and it photographed as literally nothing:
	 * this arena is built out of emissive neon at Glow 3.5 with a bloom pass to match, so an addition
	 * of 0.05 is below the noise of its own reflections. Trace.Oyster.CloudSweep captured 0.05, 0.30,
	 * 1.00 and 3.00 in one run against the real arena:
	 *
	 *     0.05   invisible. Not subtle — absent.
	 *     0.30   a green dome you can read the neon lines and a cover block straight through. THIS.
	 *     1.00   the middle blows out to white. It would hide a player standing in it, which is the
	 *            one thing §3 says it must not do.
	 *
	 * So 0.30 is not a taste call, it is the top of the range that still satisfies "must not obscure
	 * a fight". Re-run the sweep before changing it.
	 */
	constexpr float BaseIntensity = 0.30f;

	/**
	 * The break itself: a short brighter beat so a jar going off is noticed, then it settles back.
	 *
	 * *** CUT FROM 0.45 TO 0.20 BY FX_AUDIO_PLAN §2.6, AND THE CEILING IS THE POINT. *** §2.6's row
	 * for this cloud reads "KEEP the shipped honest cloud ... Enforce hue = Poisoned (0.35,0.95,0.20)
	 * exactly, ADDITIVE I <= 0.5", so the peak of this curve — not its floor — is what the rule
	 * governs. 0.30 + 0.20 = 0.50 exactly, and PeakIntensity below asserts it at compile time.
	 *
	 * The old 0.45 peaked at 0.75. That was not a bug against the sweep quoted above (0.75 sits well
	 * short of the 1.00 that blows out), it was a bug against the BUDGET: 0.5 is the ceiling the whole
	 * plan holds every while-active additive effect to, and a cloud that a poisoned player is standing
	 * INSIDE for four seconds is exactly the case the ceiling is for. Re-run Trace.Oyster.CloudSweep
	 * before moving either number; the sweep is what says 0.30 is readable at all.
	 */
	constexpr float FlashBoost    = 0.20f;
	constexpr float FlashFraction = 0.12f;

	/** The brightest this cloud can ever be, and FX_AUDIO_PLAN §2.6's stated ceiling for it. */
	constexpr float PeakIntensity = BaseIntensity + FlashBoost;
	static_assert(PeakIntensity <= 0.5f + UE_KINDA_SMALL_NUMBER,
		"FX_AUDIO_PLAN §2.6: the poison cloud is an additive effect and its intensity must never exceed 0.5.");

	/**
	 * The cloud holds its brightness for most of the poison's life and only then fades out, rather
	 * than fading linearly from the first frame: §3's [ASSUMPTION] is that what you can see is what is
	 * still dangerous, and the poison is exactly as dangerous at 3 s as it was at 1 s.
	 */
	constexpr float TailStartFraction = 0.55f;

	/**
	 * Poison green. Not a team colour: a jar is dangerous to whoever is standing in it.
	 *
	 * *** RE-SAMPLED TO THE BIBLE'S SEMANTIC WHEEL BY FX_AUDIO_PLAN §2.6. *** It was (0.12, 0.85, 0.25)
	 * — a green invented here — and §2.6 says "Enforce hue = Poisoned (0.35, 0.95, 0.20) EXACTLY". The
	 * difference is not decorative: ART_BIBLE §6.2's "one hue per effect" is a statement about the
	 * whole game, and poison has four pieces of geometry across three files (this cloud, the jar-break
	 * pop, the victim's drips and the Pickler pull link). Four near-misses of the same green read as
	 * four unrelated effects. So the constant moved to ATraceOysterPoisonCloud::GetPoisonedHue() and
	 * every one of the four calls it.
	 */
	const FLinearColor& CloudColor()
	{
		return ATraceOysterPoisonCloud::GetPoisonedHue();
	}

	/** /Engine/BasicShapes primitives are 100 uu across; the real half-width is read off the mesh. */
	constexpr float ExpectedMeshHalfWidthUU = 50.f;

	TAutoConsoleVariable<float> CVarCloudIntensity(
		TEXT("Trace.Oyster.CloudIntensity"),
		1.f,
		TEXT("Multiplier on Oyster's poison cloud brightness, on THIS machine only. 0 hides it.\n")
		TEXT("Cosmetic: it cannot move where the poison lands, only how strongly the cloud says so."),
		ECVF_Default);
}

ATraceOysterPoisonCloud::ATraceOysterPoisonCloud()
{
	// Ticks on every machine: the server also needs the deadline, and every machine drives its own
	// fade off the same replicated match-clock pair, so nothing about the fade goes on the wire.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// §3: "Replicated: every client must see it, not just the server." A server-spawned replicated
	// actor rather than a multicast, because the jar is destroyed in the same call that bursts it.
	bReplicates = true;
	SetReplicateMovement(false);   // it never moves; replicating a static transform is pure cost
	bAlwaysRelevant = true;        // small, short-lived, and a culled poison marker is a lie
	// Nothing changes after the initial bunch — which carries all three properties — so this only
	// governs the updates that follow it, and there are none. 10 Hz matches ATraceRippleActor.
	SetNetUpdateFrequency(10.f);
	SetCanBeDamaged(false);

	// §3: "It is cosmetic — no gameplay collision." Belt and braces at the actor level as well as on
	// every component, so a future component added here cannot quietly bring collision with it.
	SetActorEnableCollision(false);

	CloudRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CloudRoot"));
	SetRootComponent(CloudRoot);
	CloudRoot->SetMobility(EComponentMobility::Movable);

	// Constructor-time finders, not runtime loads, so the cooker follows them — the idiom every other
	// effect in this project uses (ATraceTracer, ATraceMeleeArc, ATraceRippleActor).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> AdditiveFinder(TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"));

	if (SphereFinder.Succeeded())
	{
		PuffMesh = SphereFinder.Object;
	}
	if (AdditiveFinder.Succeeded())
	{
		AdditiveMaterial = AdditiveFinder.Object;
	}

	// Default subobjects rather than runtime NewObject/RegisterComponent: the count is fixed, so a
	// cloud costs a template copy. Same reasoning as ATraceMeleeArc's fan.
	Puffs.Reserve(TraceOysterCloudTuning::NumPuffs);
	for (int32 Index = 0; Index < TraceOysterCloudTuning::NumPuffs; ++Index)
	{
		const FName PuffName(*FString::Printf(TEXT("Puff%d"), Index));
		UStaticMeshComponent* Puff = CreateDefaultSubobject<UStaticMeshComponent>(PuffName);
		Puff->SetupAttachment(CloudRoot);
		Puff->SetMobility(EComponentMobility::Movable);

		Puff->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Puff->SetCollisionProfileName(TEXT("NoCollision"));
		Puff->SetGenerateOverlapEvents(false);
		Puff->SetCanEverAffectNavigation(false);
		Puff->SetCastShadow(false);
		Puff->bReceivesDecals = false;
		Puff->bUseAsOccluder = false;

		if (PuffMesh != nullptr)
		{
			Puff->SetStaticMesh(PuffMesh);
		}

		// HIDDEN UNTIL BUILT, and that is not tidiness. On a client CloudRadius arrives some frames
		// after the actor does, and an unscaled sphere is a 100 uu ball: without this a client would
		// see thirteen of them stacked at the jar for a frame or two before the real cloud appeared.
		Puff->SetVisibility(false);

		Puffs.Add(Puff);
	}

	PuffMIDs.SetNum(TraceOysterCloudTuning::NumPuffs);
}

void ATraceOysterPoisonCloud::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceOysterPoisonCloud, CloudRadius);
	DOREPLIFETIME(ATraceOysterPoisonCloud, BurstMatchTime);
	DOREPLIFETIME(ATraceOysterPoisonCloud, ExpireMatchTime);
}

const FLinearColor& ATraceOysterPoisonCloud::GetPoisonedHue()
{
	// ART_BIBLE §2.5, semantic `Poisoned`. See the header for why this is a function and not four
	// copies of three floats.
	static const FLinearColor Poisoned(0.35f, 0.95f, 0.20f, 1.f);
	return Poisoned;
}

void ATraceOysterPoisonCloud::BeginPlay()
{
	Super::BeginPlay();

	// On the server ServerSpawnForBurst has already filled everything in. On a client the replicated
	// radius may not have landed yet, so the real build happens from Tick — BuildPuffsIfNeeded is
	// idempotent and costs one comparison until it can do the job.
	BuildPuffsIfNeeded();

	// =============================================================================================
	// FX_AUDIO_PLAN §2.6 audio row — `OysterJarBreak`, "ONE SOUND, THREE TRIGGERS, ALL VIA THE CLOUD"
	// =============================================================================================
	//
	// §2.6: "OysterJarBreak — cloud-local (PlayReplicatedLocal in cloud BeginPlay) — covers dash-jar
	// break, jar-jump AND Pickler detonation (one sound, three triggers, all via the cloud)". That is
	// not a shortcut, it is the only site that is true for all three: ATraceOysterJar::ServerBreakNow
	// DESTROYS the jar in the same call that bursts it, so a multicast from the jar would be an RPC on
	// an actor that is already gone — the failure mode that works on a listen server and quietly does
	// not on a client, which is the identical argument the header gives for the cloud being a
	// replicated actor rather than a multicast in the first place.
	//
	// PlayReplicatedLocal and NOT Play: this BeginPlay runs on EVERY machine because the cloud
	// replicates, so the actor's own replication IS the multicast. An RPC on top would play the break
	// twice on every client and once more on the host — FX_AUDIO_PLAN §8.7's double-audio failure.
	// `OysterJarBreak` is declared CLIENT-side in Audio/TraceSoundEvents.cpp for exactly this reason,
	// which is what makes a stray TraceAudio::Play() on it impossible to get wrong.
	//
	// UNCONDITIONAL, like the cloud itself: a jar that broke in an empty corridor still broke.
	TraceAudio::PlayReplicatedLocal(this, TraceSoundEvents::OysterJarBreak, GetActorLocation());
}

float ATraceOysterPoisonCloud::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

ATraceOysterPoisonCloud* ATraceOysterPoisonCloud::ServerSpawnForBurst(UWorld* WorldPtr, const FVector& Origin,
                                                                      float RadiusUU, float DurationSeconds)
{
	if (WorldPtr == nullptr || RadiusUU <= 0.f || Origin.ContainsNaN())
	{
		return nullptr;
	}

	// A client has no business creating one: it would be a local-only cloud that nobody else can see,
	// which is the exact failure mode §3 asks to be avoided, dressed up as a success.
	if (WorldPtr->GetNetMode() == NM_Client)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	ATraceOysterPoisonCloud* Cloud = WorldPtr->SpawnActor<ATraceOysterPoisonCloud>(
		ATraceOysterPoisonCloud::StaticClass(), FTransform(Origin), SpawnParams);
	if (Cloud == nullptr)
	{
		return nullptr;
	}

	const float Duration = FMath::Max(0.25f, DurationSeconds);

	Cloud->CloudRadius     = RadiusUU;
	Cloud->BurstMatchTime  = Cloud->MatchTimeNow();
	Cloud->ExpireMatchTime = Cloud->BurstMatchTime + Duration;

	// A backstop, not the mechanism. The deadline in Tick is what normally kills it, but that reads
	// the match clock, and a cloud left behind by a frozen or reset clock would otherwise be permanent
	// scenery in the middle of the arena.
	Cloud->SetLifeSpan(Duration + 1.f);

	// The server has every number already, so there is no reason to make it wait a frame like a client.
	Cloud->BuildPuffsIfNeeded();

	// =============================================================================================
	// FX_AUDIO_PLAN §2.6 — the jar-break POP, the fast beat the four-second cloud cannot carry
	// =============================================================================================
	//
	// §2.6: "Jar-break pop | `JarPop` burst type: 6 tiny spheres (r 8 uu) scatter 60 uu outward + fade
	// 0.25 s, Poisoned hue | cloud BeginPlay (all machines — the cloud is the replicated fact; no
	// extra actor)".
	//
	// DEVIATION, NAMED: it is fired from HERE (the server, one line after the cloud is spawned) rather
	// than from the cloud's BeginPlay on every machine, and it therefore IS "an extra actor" — one
	// ATraceFxBurst per break, alive 1.2 s. The reason is that ATraceFxBurst::Burst is authority-only
	// by contract (W3-FXBURST built it that way so a burst is one replicated fact rather than nine
	// independent local guesses), and calling it from a BeginPlay that runs on every machine would
	// mean the server spawning one burst and then each client spawning a second, local, unreplicated
	// copy on top of the replicated one. The §2.6 parenthetical is arguing against an RPC, and this
	// costs none: the burst actor's own replication is the multicast, exactly as the cloud's is.
	//
	// The pop is what makes a break READ. The cloud is deliberately slow (it holds for 55% of the
	// poison's life before it fades) because it is announcing a volume that stays dangerous; the pop
	// is the 0.25 s event that says a jar just went off, and one cannot do the other's job.
	//
	// UpVector, because §2.6 gives the pop no surface to spray off — the JarPop recipe scatters on a
	// sphere and uses the direction only to orient the scatter's poles.
	ATraceFxBurst::Burst(WorldPtr, ETraceFxBurstType::JarPop, Origin, FVector::UpVector);

	return Cloud;
}

void ATraceOysterPoisonCloud::BuildPuffsIfNeeded()
{
	if (bPuffsBuilt || CloudRadius <= 0.f)
	{
		return;   // on a client, until the replicated radius arrives
	}

	// Set FIRST, so a failure below is reported once rather than every frame — and so it leaves the
	// cloud with zero built puffs, which is what the harness measures. A silent retry loop would hide
	// exactly the kind of fault this is here to surface.
	bPuffsBuilt = true;

	// Read off the MESH rather than assuming 100 uu, and use the box half-width rather than the bounds
	// sphere radius: for a sphere mesh the half-width is unambiguously its radius, whereas the bounds
	// sphere is computed differently in different engine paths. Getting this wrong would scale every
	// puff by 1/√3 while every number in the log still said the cloud was the right size.
	const float MeshHalfWidthUU = (PuffMesh != nullptr)
		? static_cast<float>(PuffMesh->GetBounds().BoxExtent.X)
		: 0.f;

	if (PuffMesh == nullptr || MeshHalfWidthUU <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[Oyster] Poison cloud: /Engine/BasicShapes/Sphere did not resolve (half-width %.2f). No cloud will "
			     "be drawn; the poison itself is unaffected."), MeshHalfWidthUU);
		return;
	}

	if (!FMath::IsNearlyEqual(MeshHalfWidthUU, TraceOysterCloudTuning::ExpectedMeshHalfWidthUU, 0.5f))
	{
		// Not fatal — every size below is derived from the measured value, so the cloud is still the
		// right size. Said out loud because "the basic shapes are 100 uu across" is an assumption this
		// whole codebase leans on, and this is the one place that would notice it changing.
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Oyster] Poison cloud: /Engine/BasicShapes/Sphere is %.1f uu half-width, not the %.1f this project "
			     "assumes everywhere. Sizes are derived from the measured value, so the cloud is unaffected."),
			MeshHalfWidthUU, TraceOysterCloudTuning::ExpectedMeshHalfWidthUU);
	}

	// ADDITIVE OR NOTHING — see the header. An opaque green ball the size of the burst radius dropped
	// into a firefight is a far worse outcome than no cloud, so the puffs stay hidden instead.
	UMaterialInterface* Parent = AdditiveMaterial.Get();
	bPuffsVisible = (Parent != nullptr);
	if (!bPuffsVisible)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Oyster] Poison cloud: /Engine/EngineMaterials/EmissiveMeshMaterial is missing, so the cloud is "
			     "HIDDEN rather than drawn opaque (spec v16 §3: it must not obscure a fight)."));
		return;
	}

	// The additive engine material multiplies its colour by a grid TEXTURE, which is invisible under
	// ATraceTracer's bloom-bright sheath but would draw a wireframe globe across something this dim.
	// The parameters are enumerated rather than named: the name is engine content and not ours to
	// assume, and an override for a parameter that does not exist would be a silent no-op.
	UTexture* WhiteTexture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	TArray<FMaterialParameterInfo> TextureParameters;
	TArray<FGuid> TextureParameterIds;
	Parent->GetAllTextureParameterInfo(TextureParameters, TextureParameterIds);

	for (int32 Index = 0; Index < Puffs.Num(); ++Index)
	{
		UStaticMeshComponent* Puff = Puffs[Index];
		if (Puff == nullptr)
		{
			continue;
		}

		// Puff 0 is the dense middle; the rest are the shell that carries the silhouette.
		FVector Offset = FVector::ZeroVector;
		float PuffRadiusUU = TraceOysterCloudTuning::CorePuffRadiusFraction * CloudRadius;

		if (Index > 0)
		{
			// A Fibonacci sphere: evenly spread directions with no clustering at the poles, which a
			// naive lat/long loop would give and which would read as a stack of rings.
			const int32 ShellIndex = Index - 1;
			const float Zed = 1.f - 2.f * (static_cast<float>(ShellIndex) + 0.5f)
				/ static_cast<float>(TraceOysterCloudTuning::NumShellPuffs);
			const float RingRadius = FMath::Sqrt(FMath::Max(0.f, 1.f - Zed * Zed));
			const float Theta = TraceOysterCloudTuning::GoldenAngleRadians * static_cast<float>(ShellIndex);

			Offset = FVector(RingRadius * FMath::Cos(Theta), RingRadius * FMath::Sin(Theta), Zed)
				* (TraceOysterCloudTuning::ShellDistanceFraction * CloudRadius);
			PuffRadiusUU = TraceOysterCloudTuning::ShellPuffRadiusFraction * CloudRadius;
		}

		Puff->SetRelativeLocation(Offset);
		Puff->SetRelativeScale3D(FVector(PuffRadiusUU / MeshHalfWidthUU));

		PuffMIDs[Index] = Puff->CreateDynamicMaterialInstance(0, Parent);
		if (PuffMIDs[Index] != nullptr && WhiteTexture != nullptr)
		{
			for (const FMaterialParameterInfo& TextureParameter : TextureParameters)
			{
				PuffMIDs[Index]->SetTextureParameterValueByInfo(TextureParameter, WhiteTexture);
			}
		}

		Puff->SetVisibility(true);
	}

	// Frame one must already look right — a jar breaks and the cloud is simply there. The dial is
	// honoured here too, or a machine that has hidden the cloud would still get one bright frame.
	ApplyIntensity((TraceOysterCloudTuning::BaseIntensity + TraceOysterCloudTuning::FlashBoost)
		* FMath::Max(0.f, TraceOysterCloudTuning::CVarCloudIntensity.GetValueOnGameThread()));

	UE_LOG(LogTraceGame, Log,
		TEXT("[Oyster] Poison cloud built at %s: radius %.0f uu (the burst's own), %d puffs, measured extent %.1f uu, "
		     "mesh half-width %.1f uu, material %s, netMode %d, authority %d."),
		*GetActorLocation().ToCompactString(), CloudRadius, GetBuiltPuffCount(), MeasureBuiltExtentUU(),
		MeshHalfWidthUU, *DescribePuffMaterial(), static_cast<int32>(GetNetMode()), HasAuthority() ? 1 : 0);
}

void ATraceOysterPoisonCloud::ApplyIntensity(float Intensity)
{
	LastAppliedIntensity = FMath::Max(0.f, Intensity);

	// EmissiveMeshMaterial has no Glow scalar, so the intensity rides in the colour itself — exactly
	// how ATraceTracer::SetMIDColor drives this same material for its sheath (bGlowScalar = false).
	const FLinearColor Scaled = TraceOysterCloudTuning::CloudColor() * LastAppliedIntensity;

	for (UMaterialInstanceDynamic* MID : PuffMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetVectorParameterValue(TEXT("Color"), Scaled);
		}
	}
}

void ATraceOysterPoisonCloud::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	BuildPuffsIfNeeded();

	const float Now = MatchTimeNow();
	const float Life = FMath::Max(KINDA_SMALL_NUMBER, ExpireMatchTime - BurstMatchTime);
	const float Alpha = FMath::Clamp((Now - BurstMatchTime) / Life, 0.f, 1.f);

	// Two beats, and the SIZE is in neither of them. The cloud is the burst radius from its first
	// frame to its last; only the brightness moves. An expanding cloud would look better and would
	// under-report the danger for as long as it was still growing.
	const float Flash = 1.f - FMath::Min(Alpha / TraceOysterCloudTuning::FlashFraction, 1.f);
	const float Tail = (Alpha <= TraceOysterCloudTuning::TailStartFraction)
		? 1.f
		: 1.f - (Alpha - TraceOysterCloudTuning::TailStartFraction)
			/ (1.f - TraceOysterCloudTuning::TailStartFraction);

	const float Dial = FMath::Max(0.f, TraceOysterCloudTuning::CVarCloudIntensity.GetValueOnGameThread());

	ApplyIntensity((TraceOysterCloudTuning::BaseIntensity
		+ TraceOysterCloudTuning::FlashBoost * Flash * Flash) * Tail * Dial);

	// The server owns the deadline; the clients lose the actor when it goes. The fade reaches zero at
	// exactly this moment, so there is nothing to pop.
	if (HasAuthority() && ExpireMatchTime > 0.f && Now >= ExpireMatchTime)
	{
		Destroy();
	}
}

int32 ATraceOysterPoisonCloud::GetBuiltPuffCount() const
{
	int32 Count = 0;
	for (const UStaticMeshComponent* Puff : Puffs)
	{
		// Visible AND holding a mesh. A hidden puff is one this machine decided not to draw, and a
		// mesh-less one draws nothing — neither is part of the cloud a player can see.
		if (Puff != nullptr && Puff->GetStaticMesh() != nullptr && Puff->IsVisible())
		{
			++Count;
		}
	}
	return Count;
}

float ATraceOysterPoisonCloud::MeasureBuiltExtentUU() const
{
	const FVector Centre = GetActorLocation();
	float Extent = 0.f;

	for (const UStaticMeshComponent* Puff : Puffs)
	{
		if (Puff == nullptr || Puff->GetStaticMesh() == nullptr || !Puff->IsVisible())
		{
			continue;
		}

		// The RENDERER'S numbers, not this class's intentions: a puff whose mesh or scale went wrong
		// has bounds to match, and drags this measurement down with it.
		const FBoxSphereBounds PuffBounds = Puff->Bounds;
		const float Reach = static_cast<float>(FVector::Dist(Centre, PuffBounds.Origin))
			+ static_cast<float>(PuffBounds.BoxExtent.GetMax());
		Extent = FMath::Max(Extent, Reach);
	}

	return Extent;
}

FString ATraceOysterPoisonCloud::DescribePuffMaterial() const
{
	for (const UMaterialInstanceDynamic* MID : PuffMIDs)
	{
		if (MID != nullptr && MID->Parent != nullptr)
		{
			return GetNameSafe(MID->Parent);
		}
	}
	return TEXT("<none>");
}

bool ATraceOysterPoisonCloud::HasAnyCollisionEnabled() const
{
	for (UActorComponent* Component : GetComponents())
	{
		const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
		if (Primitive != nullptr && Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
		{
			return true;
		}
	}
	return false;
}

// =================================================================================================
// THE EVIDENCE — spec v16 §3
//
// TWO COMMANDS, AND THEY ARE DELIBERATELY SPLIT ALONG THE SERVER/CLIENT LINE, because §3 asks for
// two different things and one of them cannot be seen from the server at all:
//
//   Trace.Oyster.CloudTest [repeatSeconds]
//       SERVER. Breaks a real jar through the shipping path (DebugSpawnJarAt -> ServerBreakNow ->
//       Burst) and then judges the cloud that came out of it: it exists, it is the radius the DAMAGE
//       uses, its BUILT GEOMETRY really is that big, its material resolved, nothing on it collides,
//       and it lives exactly as long as the poison. Then it aims the observer at it, draws a debug
//       wire sphere at the knob's radius READ FRESH, and photographs the two together — which is the
//       picture §3 asks for: the cloud next to the poison's actual radius.
//
//       THE RED ARM IS THE FIRST ASSERTION. Every cloud already in the world is destroyed and the
//       count re-taken; it must read ZERO before the break and ONE after. A detector that cannot
//       report absence cannot report presence either, and this project has shipped that mistake
//       before.
//
//       [repeatSeconds] keeps breaking a jar every two seconds afterwards, which is the window a
//       connected client needs — see below.
//
//   Trace.Oyster.CloudReport [seconds]
//       ANY MACHINE, and the point of it is to be run on a CLIENT. It polls for a cloud and reports
//       what THIS process can see: net mode, local role, and above all authority — which must be 0 on
//       a client, because a cloud a client built for itself would prove nothing about replication.
//       It also measures the client's OWN geometry, so "the actor replicated" and "the client can see
//       a cloud" are two separate claims with two separate numbers. If the window closes with nothing
//       found it logs an Error and says so; that is the same command going red.
//
// TIMING IS REAL TIME (FPlatformTime::Seconds), for the reason TraceMaceOysterVerify gives: the
// character-select screen pauses the world, and a harness waiting on world time waits forever.
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace TraceOysterCloudVerify
{
	/** The world with a game mode, i.e. the server's. Null on a client, which is the intended answer. */
	UWorld* FindAuthoritativeWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetAuthGameMode() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** Any game world in this process — the client's included. */
	UWorld* FindAnyGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	const TCHAR* DescribeNetMode(const UWorld* WorldPtr)
	{
		switch ((WorldPtr != nullptr) ? WorldPtr->GetNetMode() : NM_Standalone)
		{
			case NM_Client:          return TEXT("client");
			case NM_ListenServer:    return TEXT("listen-server");
			case NM_DedicatedServer: return TEXT("dedicated-server");
			default:                 return TEXT("standalone");
		}
	}

	/** The select screen pauses the world, and a paused world makes every number below a frozen zero. */
	void UnpauseAndReport(UWorld* WorldPtr, const TCHAR* WhichTest)
	{
		if (WorldPtr == nullptr || !WorldPtr->IsPaused())
		{
			return;
		}
		if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
		{
			FirstPC->SetPause(false);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] The world was PAUSED (the character-select screen does that). Unpaused."), WhichTest);
		}
	}

	UTraceAbilityComponent* FindHumanAbilityComponent(UWorld* WorldPtr)
	{
		const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (BaseState == nullptr)
		{
			return nullptr;
		}
		for (APlayerState* Entry : BaseState->PlayerArray)
		{
			if (Entry == nullptr || Entry->IsABot())
			{
				continue;
			}
			if (UTraceAbilityComponent* Comp = Entry->FindComponentByClass<UTraceAbilityComponent>())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	int32 CountClouds(UWorld* WorldPtr, ATraceOysterPoisonCloud** OutFirst = nullptr)
	{
		int32 Count = 0;
		for (TActorIterator<ATraceOysterPoisonCloud> It(WorldPtr); It; ++It)
		{
			if (*It == nullptr || !IsValid(*It))
			{
				continue;
			}
			if (OutFirst != nullptr && *OutFirst == nullptr)
			{
				*OutFirst = *It;
			}
			++Count;
		}
		return Count;
	}

	/**
	 * A screenshot on THIS process, at a path namespaced by machine and pid so two processes (and two
	 * agents) cannot overwrite each other's evidence. Returns the absolute path it asked for.
	 */
	FString RequestShot(UWorld* WorldPtr, const TCHAR* WhichMachine)
	{
		const FString FileName = FString::Printf(TEXT("OysterCloud_%s_pid%d.png"),
			WhichMachine, FPlatformProcess::GetCurrentProcessId());
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);

		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Path));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);

		UE_LOG(LogTraceGame, Display, TEXT("[OysterCloud] Screenshot requested (%s): %s"), WhichMachine, *Path);
		(void)WorldPtr;
		return Path;
	}

	void ConfirmShot(const FString& Path)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.FileExists(*Path))
		{
			UE_LOG(LogTraceGame, Display, TEXT("[OysterCloud] Screenshot written (%lld bytes): %s"),
				PlatformFile.FileSize(*Path), *Path);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[OysterCloud] No screenshot appeared at: %s"), *Path);
		}
	}

	/**
	 * One line per claim, so a failure names itself rather than sinking into a summary.
	 *
	 * Two calls rather than a conditional verbosity: UE_LOG pastes its second argument into
	 * ELogVerbosity::<token>, so the level has to be a literal.
	 */
	bool Judge(const TCHAR* WhichTest, const TCHAR* Claim, bool bPassed, const FString& Detail)
	{
		if (bPassed)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[%s] PASS  %s — %s"), WhichTest, Claim, *Detail);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] *** FAIL ***  %s — %s"), WhichTest, Claim, *Detail);
		}
		return bPassed;
	}

	// =============================================================================================
	// Trace.Oyster.CloudTest — the server arm, and the picture
	// =============================================================================================

	struct FCloudTestRun
	{
		int32 AttemptsLeft = 40;
		int32 Step = 0;
		double NextRealTime = 0.0;

		float RepeatSeconds = 0.f;
		double RepeatUntilRealTime = 0.0;

		FVector BurstOrigin = FVector::ZeroVector;
		FString ShotPath;
		int32 Failures = 0;

		TWeakObjectPtr<ATraceOysterPoisonCloud> Cloud;
	};

	bool TickCloudTest(TSharedPtr<FCloudTestRun> Run);

	void ScheduleCloudTest(TSharedPtr<FCloudTestRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + static_cast<double>(DelaySeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickCloudTest(Run);
			}), 0.f);
	}

	/**
	 * Somewhere in front of the pawn with a clear line to it, so the picture is of a cloud and not of
	 * the inside of a wall. Falls back to straight ahead, which is still a usable frame with a note
	 * in the log rather than a silent bad capture.
	 */
	FVector PickOpenBurstOrigin(UWorld* WorldPtr, ATraceCharacter* Pawn, float DistanceUU, bool& bOutClear)
	{
		const FVector Eye = Pawn->GetActorLocation();
		float FeetZ = Eye.Z;
		if (const UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent())
		{
			FeetZ -= Capsule->GetScaledCapsuleHalfHeight();
		}

		const FVector Facing = Pawn->GetActorForwardVector().GetSafeNormal2D();
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceOysterCloudShot), /*bTraceComplex*/ false, Pawn);

		for (int32 Step = 0; Step < 8; ++Step)
		{
			const float YawOffset = 45.f * static_cast<float>(Step);
			const FVector Direction = Facing.RotateAngleAxis(YawOffset, FVector::UpVector);
			FVector Candidate = Eye + Direction * DistanceUU;
			Candidate.Z = FeetZ + 90.f;   // a little off the floor, so the dome is not half buried

			FHitResult Hit;
			if (!WorldPtr->LineTraceSingleByChannel(Hit, Eye, Candidate, ECC_Visibility, QueryParams))
			{
				if (AController* PawnController = Pawn->GetController())
				{
					PawnController->SetControlRotation((Candidate - Eye).Rotation());
				}
				bOutClear = true;
				return Candidate;
			}
		}

		bOutClear = false;
		FVector Fallback = Eye + Facing * DistanceUU;
		Fallback.Z = FeetZ + 90.f;
		if (AController* PawnController = Pawn->GetController())
		{
			PawnController->SetControlRotation((Fallback - Eye).Rotation());
		}
		return Fallback;
	}

	/** Places a jar at @p Where and breaks it through the shipping path. Returns the cloud, if any. */
	ATraceOysterPoisonCloud* BurstAJarAt(UWorld* WorldPtr, UTraceAbilitySetOyster* OysterSet, const FVector& Where)
	{
		ATraceOysterJar* JarActor = OysterSet->DebugSpawnJarAt(Where, /*bPickler*/ false);
		if (JarActor == nullptr)
		{
			return nullptr;
		}

		// THE SHIPPING BREAK PATH. Not a direct call to the cloud: what is under test is that a jar
		// BREAKING produces one, which is the whole of §3's first sentence.
		JarActor->ServerBreakNow(TEXT("harness: spec v16 §3 cloud"));

		ATraceOysterPoisonCloud* Found = nullptr;
		CountClouds(WorldPtr, &Found);
		return Found;
	}

	bool TickCloudTest(TSharedPtr<FCloudTestRun> Run)
	{
		static const TCHAR* Test = TEXT("OysterCloud");

		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] No authoritative world. This command is SERVER ONLY; on a client run "
				     "Trace.Oyster.CloudReport instead."), Test);
			return false;
		}
		UnpauseAndReport(WorldPtr, Test);

		// ---- step 0: the red arm, the break, and every assertion -------------------------------------
		if (Run->Step == 0)
		{
			UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
			ATraceCharacter* Pawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (Comp == nullptr || Pawn == nullptr)
			{
				if (Run->AttemptsLeft-- > 0)
				{
					ScheduleCloudTest(Run, 1.0f);
					return false;
				}
				UE_LOG(LogTraceGame, Error, TEXT("[%s] No human pawn inside the budget."), Test);
				return false;
			}

			Comp->ServerSetCharacter(ETraceCharacterId::Oyster);
			UTraceAbilitySetOyster* OysterSet = Comp->GetAbilitySetAs<UTraceAbilitySetOyster>();
			if (OysterSet == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] The human is not holding Oyster's ability set."), Test);
				return false;
			}

			// *** THE RED ARM. *** Clear the field, then prove the detector reads ZERO. Everything
			// after this is worthless if this line cannot fail.
			//
			// Gathered before destroying rather than destroyed inside the iteration: TActorIterator
			// walks the level's live actor array and Destroy() takes actors out of it.
			TArray<ATraceOysterPoisonCloud*> Stale;
			for (TActorIterator<ATraceOysterPoisonCloud> It(WorldPtr); It; ++It)
			{
				Stale.Add(*It);
			}
			for (ATraceOysterPoisonCloud* Old : Stale)
			{
				if (IsValid(Old))
				{
					Old->Destroy();
				}
			}
			const int32 BeforeCount = CountClouds(WorldPtr);
			Run->Failures += Judge(Test, TEXT("RED ARM: no cloud before a jar breaks"), BeforeCount == 0,
				FString::Printf(TEXT("clouds in the world = %d (want 0)"), BeforeCount)) ? 0 : 1;

			bool bClearShot = false;
			Run->BurstOrigin = PickOpenBurstOrigin(WorldPtr, Pawn, 1150.f, bClearShot);
			if (!bClearShot)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[%s] No clear line to any of the eight candidate spots; the capture may be occluded. "
					     "The measurements below are unaffected."), Test);
			}

			ATraceOysterPoisonCloud* Cloud = BurstAJarAt(WorldPtr, OysterSet, Run->BurstOrigin);
			Run->Cloud = Cloud;

			const int32 AfterCount = CountClouds(WorldPtr);
			Run->Failures += Judge(Test, TEXT("a breaking jar leaves exactly one cloud"), AfterCount == 1,
				FString::Printf(TEXT("clouds in the world = %d (want 1)"), AfterCount)) ? 0 : 1;

			if (Cloud == nullptr)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[%s] *** NO CLOUD. A jar broke through the shipping path and nothing was spawned. "
					     "%d claim(s) failed so far; the rest could not even be asked. ***"), Test, Run->Failures);
				return false;
			}

			// ---- the size claim, which is the whole of §3's second sentence --------------------------
			//
			// Read the knob FRESH here rather than trusting the number the cloud was handed: that is the
			// only way this can catch the failure §3 warns about, where the two drift apart.
			const UTraceSettings& Settings = UTraceSettings::Get();
			const float KnobRadius = Settings.OysterPoisonRadiusUU;
			const float KnobDuration = Settings.OysterPoisonDurationSeconds;

			Run->Failures += Judge(Test, TEXT("the cloud's radius IS the poison's radius knob"),
				FMath::IsNearlyEqual(Cloud->GetCloudRadiusUU(), KnobRadius, 0.01f),
				FString::Printf(TEXT("cloud %.2f uu vs OysterPoisonRadiusUU %.2f uu"),
					Cloud->GetCloudRadiusUU(), KnobRadius)) ? 0 : 1;

			const int32 PuffCount = Cloud->GetBuiltPuffCount();
			Run->Failures += Judge(Test, TEXT("the geometry was actually built"), PuffCount > 0,
				FString::Printf(TEXT("%d puff(s) built, material %s"), PuffCount, *Cloud->DescribePuffMaterial())) ? 0 : 1;

			// Measured off the registered components' own bounds — the renderer's opinion, not ours.
			const float Extent = Cloud->MeasureBuiltExtentUU();
			Run->Failures += Judge(Test, TEXT("the BUILT geometry reaches that radius and no further"),
				FMath::IsNearlyEqual(Extent, KnobRadius, 2.0f),
				FString::Printf(TEXT("measured extent %.1f uu vs %.1f uu (tolerance 2 uu)"), Extent, KnobRadius)) ? 0 : 1;

			Run->Failures += Judge(Test, TEXT("it is centred on the jar that broke"),
				FVector::Dist(Cloud->GetActorLocation(), Run->BurstOrigin) <= 1.f,
				FString::Printf(TEXT("cloud at %s, jar at %s"),
					*Cloud->GetActorLocation().ToCompactString(), *Run->BurstOrigin.ToCompactString())) ? 0 : 1;

			Run->Failures += Judge(Test, TEXT("it is on the ADDITIVE material, so it cannot occlude"),
				Cloud->DescribePuffMaterial().Contains(TEXT("EmissiveMeshMaterial")),
				FString::Printf(TEXT("material = %s"), *Cloud->DescribePuffMaterial())) ? 0 : 1;

			Run->Failures += Judge(Test, TEXT("nothing on it collides (§3: cosmetic only)"),
				!Cloud->HasAnyCollisionEnabled(), TEXT("every primitive is NoCollision")) ? 0 : 1;

			Run->Failures += Judge(Test, TEXT("it is replicated"), Cloud->GetIsReplicated(),
				FString::Printf(TEXT("bReplicates=%d alwaysRelevant=%d"),
					Cloud->GetIsReplicated() ? 1 : 0, Cloud->bAlwaysRelevant ? 1 : 0)) ? 0 : 1;

			const float Life = Cloud->GetExpireMatchTime() - Cloud->GetBurstMatchTime();
			Run->Failures += Judge(Test, TEXT("it lives exactly as long as the poison it announces"),
				FMath::IsNearlyEqual(Life, KnobDuration, 0.02f),
				FString::Printf(TEXT("cloud %.2f s vs OysterPoisonDurationSeconds %.2f s"), Life, KnobDuration)) ? 0 : 1;

			// ---- the picture ------------------------------------------------------------------------
			//
			// A RED WIRE SPHERE AT THE KNOB'S RADIUS, drawn from the setting and not from the cloud, so
			// the capture compares the cloud against the poison rather than against itself. If they ever
			// disagree the picture shows it without anybody having to read a log.
			DrawDebugSphere(WorldPtr, Run->BurstOrigin, KnobRadius, 32, FColor::Red,
				/*bPersistent*/ false, /*LifeTime*/ 8.f, /*DepthPriority*/ 0, /*Thickness*/ 3.f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] Jar broken at %s. Cloud radius %.0f uu, extent %.1f uu, %d puffs, material %s. A red wire "
				     "sphere of OysterPoisonRadiusUU (%.0f uu) is drawn around it. Capture in 0.75s."),
				Test, *Run->BurstOrigin.ToCompactString(), Cloud->GetCloudRadiusUU(), Extent, PuffCount,
				*Cloud->DescribePuffMaterial(), KnobRadius);

			Run->Step = 1;
			ScheduleCloudTest(Run, 0.75f);
			return false;
		}

		// ---- step 1: photograph it -------------------------------------------------------------------
		if (Run->Step == 1)
		{
			ATraceOysterPoisonCloud* Cloud = Run->Cloud.Get();
			Run->Failures += Judge(Test, TEXT("it is still lit when the shutter opens"),
				Cloud != nullptr && Cloud->GetLastAppliedIntensity() > 0.f,
				FString::Printf(TEXT("intensity = %.4f"), (Cloud != nullptr) ? Cloud->GetLastAppliedIntensity() : 0.f)) ? 0 : 1;

			Run->ShotPath = RequestShot(WorldPtr, TEXT("server"));
			Run->Step = 2;
			ScheduleCloudTest(Run, 2.5f);
			return false;
		}

		// ---- step 2: the verdict ---------------------------------------------------------------------
		if (Run->Step == 2)
		{
			ConfirmShot(Run->ShotPath);

			if (Run->Failures == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[%s] ===== spec v16 §3: PASS. ====="), Test);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] ===== spec v16 §3: FAIL (%d failed claim(s)). ====="),
					Test, Run->Failures);
			}

			if (Run->RepeatSeconds <= 0.f)
			{
				return false;
			}

			Run->RepeatUntilRealTime = FPlatformTime::Seconds() + static_cast<double>(Run->RepeatSeconds);
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] Now breaking a jar every 2 s for %.0f s, so a connected CLIENT running "
				     "Trace.Oyster.CloudReport has a window to catch one."), Test, Run->RepeatSeconds);

			Run->Step = 3;
			ScheduleCloudTest(Run, 0.5f);
			return false;
		}

		// ---- step 3: keep bursting for the client's benefit -------------------------------------------
		if (FPlatformTime::Seconds() >= Run->RepeatUntilRealTime)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[%s] Repeat window closed."), Test);
			return false;
		}

		UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
		UTraceAbilitySetOyster* OysterSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;
		if (OysterSet != nullptr)
		{
			BurstAJarAt(WorldPtr, OysterSet, Run->BurstOrigin);
		}
		ScheduleCloudTest(Run, 2.0f);
		return false;
	}

	FAutoConsoleCommand CmdCloudTest(
		TEXT("Trace.Oyster.CloudTest"),
		TEXT("Dev only, SERVER only. Spec v16 §3. Breaks a real jar through the shipping path and judges the cloud "
		     "it leaves: red arm first (zero clouds before the break), then the radius against OysterPoisonRadiusUU, "
		     "the BUILT geometry's own extent, the additive material, no collision, and the 4 s life. Then draws a "
		     "wire sphere at the knob's radius and photographs the two together. An optional argument keeps bursting "
		     "for that many seconds so a connected client can catch one."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			TSharedPtr<FCloudTestRun> Run = MakeShared<FCloudTestRun>();
			if (Args.Num() > 0)
			{
				Run->RepeatSeconds = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 300.f);
			}
			ScheduleCloudTest(Run, 0.f);
		}));

	// =============================================================================================
	// Trace.Oyster.CloudSweep — how bright is "semi transparent" in THIS arena?
	//
	// The first version of this cloud was tuned by arithmetic — a small additive value per layer,
	// reasoned about in isolation — and it photographed as nothing at all. This arena is built out of
	// emissive neon at Glow 3.5 and a bloom pass to match, so "faint" against a grey box scene is
	// invisible here. The only way to pick the number is to look at it, and the only way to look at it
	// without burning a three-minute launch per guess is to capture several in one run.
	//
	// Same reason ATraceTracer ships Trace.TestBeam, and it stays for the same reason: the next person
	// to re-tune the cloud needs it too.
	// =============================================================================================

	struct FCloudSweepRun
	{
		TArray<float> Dials;
		int32 Index = 0;
		int32 Step = 0;
		int32 AttemptsLeft = 40;
		double NextRealTime = 0.0;
		FVector BurstOrigin = FVector::ZeroVector;
		FString ShotPath;
	};

	bool TickCloudSweep(TSharedPtr<FCloudSweepRun> Run);

	void ScheduleCloudSweep(TSharedPtr<FCloudSweepRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + static_cast<double>(DelaySeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickCloudSweep(Run);
			}), 0.f);
	}

	bool TickCloudSweep(TSharedPtr<FCloudSweepRun> Run)
	{
		static const TCHAR* Sweep = TEXT("OysterCloudSweep");

		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] No authoritative world. Server only."), Sweep);
			return false;
		}
		UnpauseAndReport(WorldPtr, Sweep);

		UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
		ATraceCharacter* Pawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
		if (Comp == nullptr || Pawn == nullptr)
		{
			if (Run->AttemptsLeft-- > 0)
			{
				ScheduleCloudSweep(Run, 1.0f);
				return false;
			}
			UE_LOG(LogTraceGame, Error, TEXT("[%s] No human pawn inside the budget."), Sweep);
			return false;
		}

		if (!Run->Dials.IsValidIndex(Run->Index))
		{
			TraceOysterCloudTuning::CVarCloudIntensity->Set(1.f, ECVF_SetByConsole);
			UE_LOG(LogTraceGame, Display, TEXT("[%s] Done; dial restored to 1."), Sweep);
			return false;
		}

		// --- fire one -------------------------------------------------------------------------------
		if (Run->Step == 0)
		{
			Comp->ServerSetCharacter(ETraceCharacterId::Oyster);
			UTraceAbilitySetOyster* OysterSet = Comp->GetAbilitySetAs<UTraceAbilitySetOyster>();
			if (OysterSet == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] The human is not holding Oyster's ability set."), Sweep);
				return false;
			}

			if (Run->Index == 0)
			{
				bool bClearShot = false;
				Run->BurstOrigin = PickOpenBurstOrigin(WorldPtr, Pawn, 1150.f, bClearShot);
			}

			// One cloud in frame at a time, or two dials would be photographed on top of each other.
			TArray<ATraceOysterPoisonCloud*> Stale;
			for (TActorIterator<ATraceOysterPoisonCloud> It(WorldPtr); It; ++It)
			{
				Stale.Add(*It);
			}
			for (ATraceOysterPoisonCloud* Old : Stale)
			{
				if (IsValid(Old))
				{
					Old->Destroy();
				}
			}

			TraceOysterCloudTuning::CVarCloudIntensity->Set(Run->Dials[Run->Index], ECVF_SetByConsole);
			BurstAJarAt(WorldPtr, OysterSet, Run->BurstOrigin);

			UE_LOG(LogTraceGame, Display, TEXT("[%s] dial %.2f — capture in 0.9s."), Sweep, Run->Dials[Run->Index]);
			Run->Step = 1;
			ScheduleCloudSweep(Run, 0.9f);
			return false;
		}

		// --- photograph it --------------------------------------------------------------------------
		if (Run->Step == 1)
		{
			ATraceOysterPoisonCloud* Cloud = nullptr;
			CountClouds(WorldPtr, &Cloud);

			const FString MachineTag = FString::Printf(TEXT("sweep_dial%03d"),
				FMath::RoundToInt(Run->Dials[Run->Index] * 10.f));
			Run->ShotPath = RequestShot(WorldPtr, *MachineTag);

			UE_LOG(LogTraceGame, Display, TEXT("[%s] dial %.2f, per-layer intensity %.4f."),
				Sweep, Run->Dials[Run->Index], (Cloud != nullptr) ? Cloud->GetLastAppliedIntensity() : -1.f);

			Run->Step = 2;
			ScheduleCloudSweep(Run, 2.0f);
			return false;
		}

		ConfirmShot(Run->ShotPath);
		++Run->Index;
		Run->Step = 0;
		ScheduleCloudSweep(Run, 0.5f);
		return false;
	}

	FAutoConsoleCommand CmdCloudSweep(
		TEXT("Trace.Oyster.CloudSweep"),
		TEXT("Dev only, SERVER only. Breaks a jar once per given Trace.Oyster.CloudIntensity dial and photographs "
		     "each, so the cloud's brightness can be judged against the real arena rather than argued about. "
		     "Defaults to 1 4 12 30. Restores the dial to 1 when it finishes."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			TSharedPtr<FCloudSweepRun> Run = MakeShared<FCloudSweepRun>();
			for (const FString& Arg : Args)
			{
				Run->Dials.Add(FMath::Clamp(FCString::Atof(*Arg), 0.f, 200.f));
			}
			if (Run->Dials.Num() == 0)
			{
				Run->Dials = { 1.f, 4.f, 12.f, 30.f };
			}
			ScheduleCloudSweep(Run, 0.f);
		}));

	// =============================================================================================
	// Trace.Oyster.CloudReport — what THIS machine can see. Meant for a client.
	// =============================================================================================

	struct FCloudReportRun
	{
		double DeadlineRealTime = 0.0;
		double NextRealTime = 0.0;
		int32 Polls = 0;
		FString ShotPath;
		bool bFramed = false;
		bool bShotTaken = false;
		TWeakObjectPtr<ACameraActor> ShotCamera;
	};

	/** 0 at the burst, 1 at the deadline, from the cloud's own two replicated match-clock times. */
	float CloudAgeFraction(const UWorld* WorldPtr, const ATraceOysterPoisonCloud* Cloud)
	{
		const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (ClockState == nullptr || Cloud == nullptr)
		{
			return 1.f;
		}
		const float Now = static_cast<float>(ClockState->GetServerWorldTimeSeconds());
		const float Life = FMath::Max(KINDA_SMALL_NUMBER, Cloud->GetExpireMatchTime() - Cloud->GetBurstMatchTime());
		return FMath::Clamp((Now - Cloud->GetBurstMatchTime()) / Life, 0.f, 1.f);
	}

	/**
	 * Frames @p Cloud for the capture from a throwaway camera, rather than by moving the pawn.
	 *
	 * A client MUST NOT teleport its own pawn to get a photograph: that is a prediction the server
	 * would correct, so the picture would be of a place the client is being dragged out of. A view
	 * target is purely local and the server never hears about it. The camera stands back along the
	 * line from the cloud to the local pawn, which is the one direction already known to be open —
	 * the player is standing in it.
	 */
	ACameraActor* FrameCloudForCapture(UWorld* WorldPtr, ATraceOysterPoisonCloud* Cloud)
	{
		APlayerController* PC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		if (PC == nullptr || Cloud == nullptr)
		{
			return nullptr;
		}

		const FVector Centre = Cloud->GetActorLocation();
		FVector Away = FVector(1.f, 0.f, 0.f);
		if (const APawn* LocalPawn = PC->GetPawn())
		{
			const FVector Towards = (LocalPawn->GetActorLocation() - Centre).GetSafeNormal2D();
			if (!Towards.IsNearlyZero())
			{
				Away = Towards;
			}
		}

		const FVector Eye = Centre + Away * (Cloud->GetCloudRadiusUU() * 3.f) + FVector(0.f, 0.f, 180.f);

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transient;

		ACameraActor* Camera = WorldPtr->SpawnActor<ACameraActor>(
			ACameraActor::StaticClass(), FTransform((Centre - Eye).Rotation(), Eye), SpawnParams);
		if (Camera != nullptr)
		{
			PC->SetViewTargetWithBlend(Camera, 0.f);
		}
		return Camera;
	}

	bool TickCloudReport(TSharedPtr<FCloudReportRun> Run);

	void ScheduleCloudReport(TSharedPtr<FCloudReportRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + static_cast<double>(DelaySeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickCloudReport(Run);
			}), 0.f);
	}

	bool TickCloudReport(TSharedPtr<FCloudReportRun> Run)
	{
		static const TCHAR* Report = TEXT("OysterCloudReport");

		UWorld* WorldPtr = FindAnyGameWorld();

		// The confirm pass, after a capture. Hand the view back and take the camera away with it.
		if (Run->bShotTaken)
		{
			ConfirmShot(Run->ShotPath);
			if (APlayerController* PC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr)
			{
				PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
			}
			if (ACameraActor* Camera = Run->ShotCamera.Get())
			{
				Camera->Destroy();
			}
			return false;
		}

		// The capture itself, one beat after the camera moved. Checked BEFORE the search below, or the
		// freshness gate would get a second vote on a cloud that has already been chosen and framed.
		if (Run->bFramed)
		{
			Run->ShotPath = RequestShot(WorldPtr,
				(WorldPtr != nullptr && WorldPtr->GetNetMode() == NM_Client) ? TEXT("client") : TEXT("local"));
			Run->bShotTaken = true;
			ScheduleCloudReport(Run, 2.5f);
			return false;
		}

		ATraceOysterPoisonCloud* Cloud = nullptr;
		const int32 Count = CountClouds(WorldPtr, &Cloud);
		++Run->Polls;

		// Hold out for a FRESH one while there is still time to be choosy. A cloud caught in its last
		// half second is a dim smear and photographs as a disappointment rather than as the effect —
		// but near the deadline, a dim cloud beats no evidence, and the log says which one it is.
		const float Age = CloudAgeFraction(WorldPtr, Cloud);
		const bool bNearDeadline = (FPlatformTime::Seconds() > Run->DeadlineRealTime - 5.0);
		if (Cloud != nullptr && Age > 0.35f && !bNearDeadline)
		{
			ScheduleCloudReport(Run, 0.15f);
			return false;
		}

		if (Cloud != nullptr)
		{
			// *** THE CLIENT PROOF. authority=0 is the line that matters: a cloud this process built
			// for itself would say 1 and would prove nothing about §3's "every client must see it". ***
			//
			// The local pawn is reported alongside it because the two claims are different. A client
			// still sitting on the character-select screen HAS the cloud — every number below is real
			// — but its viewport is showing the select overlay, so the capture that goes with this line
			// would be a picture of a menu. See the note on the command itself.
			const APlayerController* LocalPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
			const APawn* LocalPawn = (LocalPC != nullptr) ? LocalPC->GetPawn() : nullptr;

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] *** CLOUD SEEN on a %s: authority=%d localRole=%d clouds=%d radius=%.0f uu extent=%.1f uu "
				     "puffs=%d material=%s intensity=%.4f at %s (found after %d poll(s)). Local pawn: %s. ***"),
				Report, DescribeNetMode(WorldPtr), Cloud->HasAuthority() ? 1 : 0,
				static_cast<int32>(Cloud->GetLocalRole()), Count, Cloud->GetCloudRadiusUU(),
				Cloud->MeasureBuiltExtentUU(), Cloud->GetBuiltPuffCount(), *Cloud->DescribePuffMaterial(),
				Cloud->GetLastAppliedIntensity(), *Cloud->GetActorLocation().ToCompactString(), Run->Polls,
				*GetNameSafe(LocalPawn));

			UE_LOG(LogTraceGame, Display, TEXT("[%s] Age %.0f%% of its life when it was framed."), Report, Age * 100.f);

			// Frame it from a throwaway camera — see FrameCloudForCapture for why not the pawn.
			Run->ShotCamera = FrameCloudForCapture(WorldPtr, Cloud);
			Run->bFramed = true;
			ScheduleCloudReport(Run, 0.3f);   // a view target change needs a frame to become the view
			return false;
		}

		if (FPlatformTime::Seconds() >= Run->DeadlineRealTime)
		{
			// RED, and loudly. "Nothing happened" is a result, not a silence.
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] *** NO CLOUD ON THIS MACHINE (%s) after %d poll(s). Spec v16 §3 asks for one on every "
				     "client, not just the server. ***"), Report, DescribeNetMode(WorldPtr), Run->Polls);
			return false;
		}

		ScheduleCloudReport(Run, 0.25f);
		return false;
	}

	FAutoConsoleCommand CmdCloudReport(
		TEXT("Trace.Oyster.CloudReport"),
		TEXT("Dev only, ANY machine — meant for a CLIENT. Spec v16 §3. Polls for up to N seconds (default 30) for a "
		     "replicated poison cloud and reports what this process can see, including authority (0 on a client) and "
		     "the geometry this machine built for itself, then photographs it. Logs an Error if none arrives.\n"
		     "Run it once the local view is IN the match: a client still on the character-select screen has the "
		     "cloud and reports it correctly, but its screenshot is a picture of the select overlay. The 'Local "
		     "pawn' field in the report line says which of the two you got."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			TSharedPtr<FCloudReportRun> Run = MakeShared<FCloudReportRun>();
			const float Window = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 1.f, 300.f) : 30.f;
			Run->DeadlineRealTime = FPlatformTime::Seconds() + static_cast<double>(Window);
			ScheduleCloudReport(Run, 0.f);
		}));
}

// =================================================================================================
// FX_AUDIO_PLAN §2.6 — THE PARADE
//
// Trace.Oyster.FxParade
//     SERVER. Stages every §2.6 element that has a picture and photographs each one from a camera
//     that is actually pointing at it:
//
//       1  a DASH jar and a PICKLER jar 90 uu apart, so the collar — the only thing that tells them
//          apart, by ART_BIBLE §6.3's explicit ruling — can be judged side by side.
//       2  the Pickler jar BROKEN through the shipping path: the cloud at the Poisoned hue, the
//          JarPop burst, and the OysterJarBreak sound, all from one ServerBreakNow.
//       3  a poisoned VICTIM, framed from outside, so the three falling drips can be judged. There is
//          no other way to see them: they hang on somebody else's pawn and the local view is a rifle.
//
// It exists for the reason W3-FXBURST's parade did: headless, a screenshot lands one or two frames
// after it is asked for, and an FX nobody has photographed is an FX nobody has verified. Each step
// PRINTS what it measured off the live actors as well as shooting it, so the log and the frame are
// two independent claims about the same thing.
//
// The measurements that make DRAWN == LETHAL a fact rather than a hope live in Trace.Oyster.CloudTest,
// not here — that command already reads the knob fresh, compares it against the cloud's replicated
// radius AND against the renderer's own bounds, and draws a red wire sphere at the knob's radius in
// the photograph. This parade is about hue, shape and the things CloudTest does not dress.
// =================================================================================================

namespace TraceOysterFxParade
{
	using namespace TraceOysterCloudVerify;

	struct FParadeRun
	{
		int32 Step = 0;
		int32 AttemptsLeft = 40;
		double NextRealTime = 0.0;

		FVector Origin = FVector::ZeroVector;
		TWeakObjectPtr<ATraceOysterJar> DashJar;
		TWeakObjectPtr<ATraceOysterJar> PicklerJar;
		TWeakObjectPtr<ATraceCharacter> DripVictim;
		TWeakObjectPtr<ACameraActor> ShotCamera;
		FString ShotPath;

		/** Play counts sampled BEFORE a step, so the step's own audio is a delta and not a total. */
		int32 AudioBaselinePickler = 0;
		int32 AudioBaselineBreak = 0;

		/** Recorded at spawn, because a Pickler jar detonates 0.29 s later and cannot be asked twice. */
		bool bPicklerHadCollarAtSpawn = false;
		bool bDashWasPlainAtSpawn = false;
	};

	/**
	 * How many times @p Event has reached the ENGINE on this machine.
	 *
	 * The subsystem's per-event map is bumped after the side gate, the settings gate, the device test
	 * and the resolve, so a delta across a step is the difference between "I called Play" and "a sound
	 * played" — which is the distinction W3-FXBURST's first capture run discovered the hard way.
	 */
	int32 AudioPlays(UWorld* WorldPtr, FName Event)
	{
		const UTraceAudioSubsystem* Audio = (WorldPtr != nullptr)
			? WorldPtr->GetSubsystem<UTraceAudioSubsystem>() : nullptr;
		if (Audio == nullptr)
		{
			return 0;
		}
		const int32* Found = Audio->GetPlaysByEvent().Find(Event);
		return (Found != nullptr) ? *Found : 0;
	}

	bool TickParade(TSharedPtr<FParadeRun> Run);

	void Schedule(TSharedPtr<FParadeRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + static_cast<double>(DelaySeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickParade(Run);
			}), 0.f);
	}

	/** A local view target looking at @p Focus from @p Distance uu, slightly above it. */
	ACameraActor* LookAt(UWorld* WorldPtr, TWeakObjectPtr<ACameraActor>& CameraSlot, APlayerController* PC,
		const FVector& Focus, float DistanceUU, float HeightUU)
	{
		if (WorldPtr == nullptr || PC == nullptr)
		{
			return nullptr;
		}

		// Back along the line to the local pawn: the one direction already known to be open, because
		// the player is standing in it. Same argument as FrameCloudForCapture's.
		FVector Away(1.f, 0.f, 0.f);
		if (const APawn* LocalPawn = PC->GetPawn())
		{
			const FVector Towards = (LocalPawn->GetActorLocation() - Focus).GetSafeNormal2D();
			if (!Towards.IsNearlyZero())
			{
				Away = Towards;
			}
		}

		// AND IT HAS TO BE A DIRECTION WITH A CLEAR LINE. "Back along the line to the local pawn" is
		// usually open — the player is standing in it — but a pawn on the far side of a pillar puts the
		// camera inside it, and the first drip frame came back half-occluded by a goal-mouth wall. Eight
		// candidates at 45 degrees, first clear one wins, and the fallback is the original direction
		// with a note rather than a silent bad capture. Same shape as PickOpenBurstOrigin's search.
		FVector Eye = Focus + Away * DistanceUU + FVector(0.f, 0.f, HeightUU);
		{
			FCollisionQueryParams CameraParams(SCENE_QUERY_STAT(TraceOysterFxCamera), /*bTraceComplex*/ false);
			if (const APawn* LocalPawn = PC->GetPawn())
			{
				CameraParams.AddIgnoredActor(LocalPawn);
			}

			for (int32 Step = 0; Step < 8; ++Step)
			{
				const FVector Direction = Away.RotateAngleAxis(45.f * static_cast<float>(Step), FVector::UpVector);
				const FVector Candidate = Focus + Direction * DistanceUU + FVector(0.f, 0.f, HeightUU);

				FHitResult CameraHit;
				if (!WorldPtr->LineTraceSingleByChannel(CameraHit, Candidate, Focus, ECC_Visibility, CameraParams))
				{
					Eye = Candidate;
					break;
				}
			}
		}

		if (ACameraActor* Old = CameraSlot.Get())
		{
			Old->Destroy();
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transient;

		ACameraActor* Camera = WorldPtr->SpawnActor<ACameraActor>(
			ACameraActor::StaticClass(), FTransform((Focus - Eye).Rotation(), Eye), SpawnParams);
		if (Camera != nullptr)
		{
			PC->SetViewTargetWithBlend(Camera, 0.f);
			CameraSlot = Camera;
		}
		return Camera;
	}

	/** The parade's own shorthand for the call above. */
	ACameraActor* LookAt(UWorld* WorldPtr, TSharedPtr<FParadeRun> Run, const FVector& Focus,
		float DistanceUU, float HeightUU)
	{
		return LookAt(WorldPtr, Run->ShotCamera,
			(WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr,
			Focus, DistanceUU, HeightUU);
	}

	FString Shoot(UWorld* WorldPtr, const TCHAR* Label)
	{
		const FString FileName = FString::Printf(TEXT("OysterFx_%s_pid%d.png"),
			Label, FPlatformProcess::GetCurrentProcessId());
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);

		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Path));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[OysterFx] Screenshot requested: %s"), *Path);
		(void)WorldPtr;
		return Path;
	}

	/** Destroys every cloud in the world. A frame of a NEW cloud must not contain an old one. */
	void ClearClouds(UWorld* WorldPtr)
	{
		// Gathered first: Destroy() mutates the level's actor array, which TActorIterator is walking.
		TArray<ATraceOysterPoisonCloud*> Stale;
		for (TActorIterator<ATraceOysterPoisonCloud> It(WorldPtr); It; ++It)
		{
			Stale.Add(*It);
		}
		for (ATraceOysterPoisonCloud* Old : Stale)
		{
			if (IsValid(Old))
			{
				Old->Destroy();
			}
		}
	}

	/**
	 * Stops a bot where it stands, so a camera aimed at it 0.8 s ago is still aimed at it.
	 *
	 * The first run of this parade photographed an empty stretch of arena floor: bots run at ~600 uu/s
	 * and the victim had simply left. Freezing the SUBJECT rather than chasing it with the camera is
	 * the same call Trace.Slimeball.Verify makes when it puts the wall ON the victim — what is being
	 * photographed is an effect, not a bot's pathing.
	 */
	void FreezeVictim(ATraceCharacter* Victim)
	{
		if (Victim == nullptr)
		{
			return;
		}
		if (UTraceCharacterMovementComponent* MoveComp = Victim->GetTraceMovement())
		{
			MoveComp->StopMovementImmediately();
			MoveComp->DisableMovement();
		}
	}

	void ThawVictim(ATraceCharacter* Victim)
	{
		if (Victim == nullptr)
		{
			return;
		}
		if (UTraceCharacterMovementComponent* MoveComp = Victim->GetTraceMovement())
		{
			MoveComp->SetMovementMode(MOVE_Walking);
		}
	}

	/**
	 * A living ENEMY of @p NotThis, falling back to anybody else.
	 *
	 * The enemy test is not cosmetic. The §4 choke point refuses Control on a team-mate, so a pull
	 * staged on a friendly bot produces no launch and therefore NO PULL LINK — which is exactly what
	 * this parade's second run reported ("0 GenericRing link(s)") and it was the harness picking the
	 * wrong bot, not the effect failing. The fallback is kept so a 1v0 world still photographs drips.
	 */
	ATraceCharacter* FindVictim(UWorld* WorldPtr, const ATraceCharacter* NotThis)
	{
		const ETraceTeam MyTeam = (NotThis != nullptr) ? NotThis->GetTeam() : ETraceTeam::None;
		ATraceCharacter* Fallback = nullptr;

		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == NotThis || !Candidate->IsAlive())
			{
				continue;
			}
			// AND NOT ONE THIS MACHINE IS NOT DRAWING. The first drip frame came back as a single green
			// sphere hanging in mid-air over an empty floor: the effect was correct, on the right pawn,
			// and the pawn's body was simply not rendered (a corpse still hidden, or a cloak). A
			// photograph of a status effect needs the BODY it is a status of.
			if (Candidate->IsHidden() || (Candidate->GetMesh() != nullptr && !Candidate->GetMesh()->IsVisible()))
			{
				continue;
			}

			// AND NOT THE CARRIER. Spec §4 is the founding invariant — no ability Control reaches a
			// player holding the Core — so a fixture that stages one on the carrier is photographing
			// the rule working, labelled as the effect failing. That is precisely what this parade's
			// first run did: SLOWED came back active=0 and the frame shows the Core in the victim's
			// hands. Excluded here rather than "handled" at the shot, because there is nothing to fix.
			if (UTraceAbilityComponent::IsCarrier(Candidate))
			{
				continue;
			}

			const ETraceTeam TheirTeam = Candidate->GetTeam();
			if (MyTeam != ETraceTeam::None && TheirTeam != ETraceTeam::None && TheirTeam != MyTeam)
			{
				return Candidate;
			}
			if (Fallback == nullptr)
			{
				Fallback = Candidate;
			}
		}
		return Fallback;
	}

	bool TickParade(TSharedPtr<FParadeRun> Run)
	{
		static const TCHAR* Test = TEXT("OysterFx");

		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] No authoritative world. SERVER ONLY."), Test);
			return false;
		}
		UnpauseAndReport(WorldPtr, Test);

		UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
		ATraceCharacter* Pawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
		if (Comp == nullptr || Pawn == nullptr)
		{
			if (Run->AttemptsLeft-- > 0)
			{
				Schedule(Run, 1.0f);
				return false;
			}
			UE_LOG(LogTraceGame, Error, TEXT("[%s] No human pawn inside the budget."), Test);
			return false;
		}

		switch (Run->Step)
		{
		// ---- 0: two jars, side by side --------------------------------------------------------
		case 0:
		{
			Comp->ServerSetCharacter(ETraceCharacterId::Oyster);
			UTraceAbilitySetOyster* OysterSet = Comp->GetAbilitySetAs<UTraceAbilitySetOyster>();
			if (OysterSet == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] The human is not holding Oyster's ability set."), Test);
				return false;
			}

			// A cloud left over from a bot's own jar would put the camera INSIDE a 380 uu additive dome
			// and turn this frame into a green wash — which is exactly what the first run produced.
			ClearClouds(WorldPtr);

			bool bClear = false;
			Run->Origin = PickOpenBurstOrigin(WorldPtr, Pawn, 520.f, bClear);
			// Off the floor by the jar's own half-height so neither jar is buried in it.
			Run->Origin.Z += 20.f;

			const FVector Across = FVector::CrossProduct(
				(Run->Origin - Pawn->GetActorLocation()).GetSafeNormal2D(), FVector::UpVector) * 45.f;

			ATraceOysterJar* Dash    = OysterSet->DebugSpawnJarAt(Run->Origin - Across, /*bPickler*/ false);
			ATraceOysterJar* Pickler = OysterSet->DebugSpawnJarAt(Run->Origin + Across, /*bPickler*/ true);
			Run->DashJar    = Dash;
			Run->PicklerJar = Pickler;

			// *** ASKED NOW, NOT AT THE SHUTTER. *** A Pickler jar that is placed on the ground lands
			// immediately, and spec v26 §6b then gives it a fuse of PullRadius/PullSpeed = 0.29 s — so
			// by the time a screenshot has been requested, taken and confirmed, the jar this assertion
			// is about has blown itself up. The first run of this parade reported "*** no collar ***"
			// for exactly that reason and it was the HARNESS that was wrong, not the collar. The
			// picture below is therefore taken inside the fuse; the claim is recorded here.
			Run->bDashWasPlainAtSpawn      = (Dash != nullptr) && !Dash->HasCollarBuilt();
			Run->bPicklerHadCollarAtSpawn  = (Pickler != nullptr) && Pickler->HasCollarBuilt();

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] JARS at spawn: dash %s (%s), pickler %s (%s), body radius %.1f uu. "
				     "ART_BIBLE §6.3 'distinguish by SHAPE' rule: %s."),
				Test,
				Run->bDashWasPlainAtSpawn ? TEXT("PLAIN") : TEXT("*** has a collar ***"),
				(Dash != nullptr) ? *Dash->DescribeLook() : TEXT("<no jar>"),
				Run->bPicklerHadCollarAtSpawn ? TEXT("COLLARED") : TEXT("*** no collar ***"),
				(Pickler != nullptr) ? *Pickler->DescribeLook() : TEXT("<no jar>"),
				(Dash != nullptr) ? Dash->MeasureJarRadiusUU() : 0.f,
				(Run->bDashWasPlainAtSpawn && Run->bPicklerHadCollarAtSpawn) ? TEXT("PASS") : TEXT("*** FAIL ***"));

			LookAt(WorldPtr, Run, Run->Origin, 300.f, 45.f);
			Run->Step = 1;
			Schedule(Run, 0.07f);   // inside the Pickler jar's 0.29 s fuse
			return false;
		}

		// ---- 1: photograph them ------------------------------------------------------------------
		case 1:
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] shutter: pickler still alive = %d (its §6b fuse is %.2f s)."),
				Test, Run->PicklerJar.IsValid() ? 1 : 0, ATraceOysterJar::GetPicklerDetonateDelaySeconds());

			Run->ShotPath = Shoot(WorldPtr, TEXT("jars"));
			Run->Step = 2;
			Schedule(Run, 2.0f);
			return false;
		}

		// ---- 2: break a FRESH dash jar -----------------------------------------------------------
		case 2:
		{
			ConfirmShot(Run->ShotPath);

			UTraceAbilitySetOyster* OysterSet = Comp->GetAbilitySetAs<UTraceAbilitySetOyster>();
			if (OysterSet == nullptr)
			{
				return false;
			}

			// Clear the field so the cloud in the next frame is THIS break's and not a survivor of the
			// Pickler jar's own detonation two seconds ago.
			ClearClouds(WorldPtr);

			Run->AudioBaselineBreak = AudioPlays(WorldPtr, TraceSoundEvents::OysterJarBreak);

			if (ATraceOysterJar* Fresh = OysterSet->DebugSpawnJarAt(Run->Origin, /*bPickler*/ false))
			{
				// THE SHIPPING BREAK PATH. Burst() spawns the cloud; the cloud's BeginPlay plays
				// OysterJarBreak locally on every machine; ServerSpawnForBurst fires the JarPop.
				Fresh->ServerBreakNow(TEXT("harness: FX_AUDIO_PLAN §2.6 parade"));
			}

			LookAt(WorldPtr, Run, Run->Origin,
				FMath::Max(300.f, UTraceSettings::Get().OysterPoisonRadiusUU * 2.6f), 160.f);
			Run->Step = 3;
			Schedule(Run, 0.20f);   // inside JarPop's 0.25 s animation
			return false;
		}

		// ---- 3: photograph the cloud + pop --------------------------------------------------------
		case 3:
		{
			int32 Bursts = 0;
			for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
			{
				if (*It != nullptr && (*It)->GetBurstType() == ETraceFxBurstType::JarPop)
				{
					++Bursts;
					UE_LOG(LogTraceGame, Display,
						TEXT("[%s] JarPop burst live: %s, radius %.0f uu, %d/%d primitive(s) visible."),
						Test, *(*It)->DescribeBlends(), (*It)->GetResolvedRadiusUU(),
						(*It)->GetVisiblePrimitiveCount(), (*It)->GetPrimitiveCount());
				}
			}

			ATraceOysterPoisonCloud* Cloud = nullptr;
			CountClouds(WorldPtr, &Cloud);

			const int32 BreakPlays = AudioPlays(WorldPtr, TraceSoundEvents::OysterJarBreak)
				- Run->AudioBaselineBreak;

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] BREAK: %d JarPop burst(s); cloud %s, intensity %.3f (§2.6 ceiling 0.5); "
				     "OysterJarBreak reached the engine %d time(s) — §8.7 wants exactly 1 per break."),
				Test, Bursts,
				(Cloud != nullptr) ? *FString::Printf(TEXT("r %.0f uu, %d puffs, extent %.1f uu"),
					Cloud->GetCloudRadiusUU(), Cloud->GetBuiltPuffCount(), Cloud->MeasureBuiltExtentUU())
					: TEXT("*** MISSING ***"),
				(Cloud != nullptr) ? Cloud->GetLastAppliedIntensity() : 0.f,
				BreakPlays);

			Run->ShotPath = Shoot(WorldPtr, TEXT("break"));
			Run->Step = 4;
			Schedule(Run, 2.0f);
			return false;
		}

		// ---- 4: a real LOB, landed on a victim: the sound and the pull links -----------------------
		case 4:
		{
			ConfirmShot(Run->ShotPath);

			ATraceCharacter* Victim = FindVictim(WorldPtr, Pawn);
			if (Victim == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[%s] No second pawn, so the PULL LINK and the DRIPS cannot be staged. "
					     "Run this in a match with ?bots=N."), Test);
				Run->Step = 8;
				Schedule(Run, 0.f);
				return false;
			}
			Run->DripVictim = Victim;
			ClearClouds(WorldPtr);

			Run->AudioBaselinePickler = AudioPlays(WorldPtr, TraceSoundEvents::OysterPickler);

			// A REAL LOB, through the shipping constructor: Initialise() with a non-zero velocity is
			// what makes a jar a thrown one, and it is the line that plays OysterPickler.
			//
			// RELEASED ESSENTIALLY ON TOP OF THE VICTIM (40 uu up, 30 uu back), not "a short way" from
			// them. The 160 uu offset the first version used made the pull a measurement of the bot's
			// pathing as much as of the rule: a bot that took two steps between being chosen and the
			// jar landing was outside a radius the frame could not show, and "0 pull links" then means
			// nothing at all. The block reason is printed below so a refusal names itself.
			const FVector Toward = (Victim->GetActorLocation() - Pawn->GetActorLocation()).GetSafeNormal2D();
			const FVector Release = Victim->GetActorLocation() - Toward * 30.f + FVector(0.f, 0.f, 40.f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] pull victim %s: team %d, carrier %d, %.0f uu from the release point; the choke "
				     "point says Control is '%s' (pull radius %.0f uu)."),
				Test, *GetNameSafe(Victim), static_cast<int32>(Victim->GetTeam()),
				UTraceAbilityComponent::IsCarrier(Victim) ? 1 : 0,
				static_cast<float>(FVector::Dist(Victim->GetActorLocation(), Release)),
				TraceAbilityBlockReasonToString(
					Comp->CanAffectTargetDetailed(Victim, ETraceAbilityEffect::Control)),
				UTraceSettings::Get().OysterPicklerPullRadiusUU);

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ATraceOysterJar* Lobbed = WorldPtr->SpawnActor<ATraceOysterJar>(
				ATraceOysterJar::StaticClass(), FTransform(Release), SpawnParams);
			if (Lobbed != nullptr)
			{
				Lobbed->Initialise(Comp, Pawn->GetTeam(), /*bPickler*/ true, Toward * 400.f + FVector(0.f, 0.f, 120.f));
				// Landed by hand so the impact happens at a known place in a known frame rather than
				// wherever a 400 uu/s toss ends up. Land() is the same call the flight makes.
				Lobbed->ServerForceLandNow();
			}
			Run->PicklerJar = Lobbed;

			// FAR ENOUGH BACK TO BE OUTSIDE THE POISON RADIUS. The first pull frame was a flat green
			// wash: the jar detonates 0.29 s after it lands (spec v26 §6b) and a camera 300 uu from the
			// victim is INSIDE the 380 uu cloud that leaves behind. Backed off past the radius, and the
			// shutter moved inside the fuse.
			LookAt(WorldPtr, Run,
				Victim->GetActorLocation(),
				FMath::Max(650.f, UTraceSettings::Get().OysterPoisonRadiusUU * 1.8f), 170.f);
			Run->Step = 5;
			Schedule(Run, 0.08f);
			return false;
		}

		// ---- 5: photograph the pull links ---------------------------------------------------------
		case 5:
		{
			int32 Rings = 0;
			for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
			{
				if (*It != nullptr && (*It)->GetBurstType() == ETraceFxBurstType::GenericRing)
				{
					++Rings;
				}
			}

			const int32 PicklerPlays = AudioPlays(WorldPtr, TraceSoundEvents::OysterPickler)
				- Run->AudioBaselinePickler;

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] PULL: %d GenericRing link(s) live (one per victim the choke point allowed); "
				     "OysterPickler reached the engine %d time(s) on the lob."),
				Test, Rings, PicklerPlays);

			// CLEARED IMMEDIATELY BEFORE THE SHUTTER, not two steps ago. This match has bot Oysters in
			// it dropping dash jars of their own, and a 380 uu additive dome anywhere near the camera
			// turns the frame into a flat green wash — which is what the first two attempts produced,
			// even from 684 uu back. Clouds are actors; the pull LINKS are ATraceFxBurst actors and are
			// untouched by this.
			ClearClouds(WorldPtr);

			// Re-aimed at where the victim IS, not where they were when the jar landed — they have just
			// been launched at 1300 uu/s, which is the whole point of the effect being photographed.
			if (const ATraceCharacter* Victim = Run->DripVictim.Get())
			{
				LookAt(WorldPtr, Run, Victim->GetActorLocation(), 320.f, 110.f);
			}

			Run->ShotPath = Shoot(WorldPtr, TEXT("pull"));
			Run->Step = 6;
			Schedule(Run, 2.0f);
			return false;
		}

		// ---- 6: poison the victim and frame the drips ----------------------------------------------
		case 6:
		{
			ConfirmShot(Run->ShotPath);

			ATraceCharacter* Victim = Run->DripVictim.Get();
			if (Victim == nullptr || !Victim->IsAlive())
			{
				Victim = FindVictim(WorldPtr, Pawn);
				Run->DripVictim = Victim;
			}
			if (Victim == nullptr)
			{
				Run->Step = 8;
				Schedule(Run, 0.f);
				return false;
			}

			FreezeVictim(Victim);
			// The drips are 6 uu spheres; a 380 uu cloud in the same frame would be the only thing in it.
			ClearClouds(WorldPtr);
			UTraceOysterPoisonComponent::ApplyTo(Victim, Comp);

			LookAt(WorldPtr, Run, Victim->GetActorLocation(), 230.f, 30.f);
			Run->Step = 7;
			Schedule(Run, 0.6f);
			return false;
		}

		// ---- 7: photograph the drips ----------------------------------------------------------------
		case 7:
		{
			const ATraceCharacter* Victim = Run->DripVictim.Get();
			const UTraceOysterPoisonComponent* Poison = UTraceOysterPoisonComponent::Find(Victim);

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] DRIPS on %s: %d piece(s) drawn on this machine (§2.6 asks for 3; fewer means the "
				     "§1.4 loop budget refused some, which it logs). Slow in force: %d, multiplier %.3f."),
				Test, *GetNameSafe(Victim),
				(Poison != nullptr) ? Poison->GetDripPieceCount() : -1,
				(Poison != nullptr && Poison->IsSlowActive()) ? 1 : 0,
				(Poison != nullptr) ? Poison->GetSpeedMultiplier() : 1.f);

			// Same reason as the pull shot: the drips are 6 uu spheres and a bot's cloud drifting into
			// frame is the only thing anybody would see.
			ClearClouds(WorldPtr);

			// Re-aimed one last time: FreezeVictim stops the pawn, but a frozen pawn that was mid-air
			// still finishes its fall, so the camera is pointed at the body that is actually there.
			if (Victim != nullptr)
			{
				LookAt(WorldPtr, Run, Victim->GetActorLocation(), 230.f, 30.f);
			}

			Run->ShotPath = Shoot(WorldPtr, TEXT("drips"));
			Run->Step = 8;
			Schedule(Run, 2.0f);
			return false;
		}

		// ---- 8: hand the view back ------------------------------------------------------------------
		default:
		{
			ConfirmShot(Run->ShotPath);

			ThawVictim(Run->DripVictim.Get());

			if (APlayerController* PC = WorldPtr->GetFirstPlayerController())
			{
				PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
			}
			if (ACameraActor* Camera = Run->ShotCamera.Get())
			{
				Camera->Destroy();
			}

			UE_LOG(LogTraceGame, Display, TEXT("[%s] ===== parade complete. ====="), Test);
			return false;
		}
		}
	}

	// =============================================================================================
	// Trace.Oyster.JarLadder — the measurement behind ATraceOysterJar's shipped Glow
	//
	// FX_AUDIO_PLAN §2.6 asks for Glow 1.4. The first parade frame said that is a WHITE cylinder: the
	// pixels came back at the correct hue but saturation 0.094, because M_TraceNeon's emissive is
	// Color x Glow and the accent's brightest channels clip long before 1.4.
	//
	// So four jars are staged in a row, each BUILT at a different Glow — the value is latched per jar
	// at build for exactly this reason — and photographed in ONE frame, on ONE surface, under ONE
	// exposure. Reading four rungs off four separate frames would be comparing four tone-mappings.
	// The pixels are then read back out of the PNG by hand (see the report), which is the only way to
	// turn "it looks white" into a number somebody can disagree with.
	//
	// *** TWO THINGS THIS BLOCK USED TO SAY THAT ARE NO LONGER TRUE, AND WHY THE RUNGS ARE STILL RIGHT.
	// *** It said "Oyster's two brightest channels (0.85, 0.95)" and it called 0.74 the shipped value.
	// Oyster's accent was re-spaced from cyan #95EDF9 to deep sea green #6FE5A2 (brightest channel
	// 0.95 -> 0.78), and TraceOysterJar's glow is no longer a literal at all: it is DERIVED as
	// 0.70 / brightest, i.e. 0.897 today, so that the product the tonemapper sees stays on
	// ATraceFxBurst's measured hue-headroom cap whatever colour Oyster is wearing. See the long block
	// on TraceOysterJar::JarGlowShipped().
	//
	// The four rungs below are deliberately left as ABSOLUTE glows. What this ladder measures is the
	// rate at which saturation is spent as the brightest channel is pushed, which is a property of the
	// tonemapper and not of a hue; and 0.74 / 1.00 now STRADDLE the derived 0.897 instead of one of
	// them landing on it, which is what a ladder is for. Re-shoot it if the HEADROOM is retuned — not
	// because an accent moved.
	// =============================================================================================

	struct FLadderRun
	{
		int32 Step = 0;
		int32 AttemptsLeft = 40;
		double NextRealTime = 0.0;
		FString ShotPath;
		TWeakObjectPtr<ACameraActor> ShotCamera;
	};

	/** The rungs, brightest first, left to right in the frame. */
	static const float LadderGlows[4] = { 1.40f, 1.00f, 0.74f, 0.50f };

	bool TickLadder(TSharedPtr<FLadderRun> Run);

	void ScheduleLadder(TSharedPtr<FLadderRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + static_cast<double>(DelaySeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickLadder(Run);
			}), 0.f);
	}

	void SetJarGlowCVar(float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Oyster.JarGlow")))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	bool TickLadder(TSharedPtr<FLadderRun> Run)
	{
		static const TCHAR* Test = TEXT("OysterJarLadder");

		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] No authoritative world. SERVER ONLY."), Test);
			return false;
		}
		UnpauseAndReport(WorldPtr, Test);

		UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
		ATraceCharacter* Pawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
		if (Comp == nullptr || Pawn == nullptr)
		{
			if (Run->AttemptsLeft-- > 0)
			{
				ScheduleLadder(Run, 1.0f);
				return false;
			}
			UE_LOG(LogTraceGame, Error, TEXT("[%s] No human pawn inside the budget."), Test);
			return false;
		}

		if (Run->Step == 0)
		{
			Comp->ServerSetCharacter(ETraceCharacterId::Oyster);
			UTraceAbilitySetOyster* OysterSet = Comp->GetAbilitySetAs<UTraceAbilitySetOyster>();
			if (OysterSet == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] The human is not holding Oyster's ability set."), Test);
				return false;
			}

			ClearClouds(WorldPtr);

			bool bClear = false;
			FVector Centre = PickOpenBurstOrigin(WorldPtr, Pawn, 560.f, bClear);
			Centre.Z += 20.f;

			const FVector Across = FVector::CrossProduct(
				(Centre - Pawn->GetActorLocation()).GetSafeNormal2D(), FVector::UpVector);

			// DASH jars, not Pickler ones: a Pickler jar detonates 0.29 s after it is placed and this
			// frame is taken later than that. The body's Glow is what is being measured and both kinds
			// wear the same one.
			//
			// SPAWNED DIRECTLY, NOT THROUGH DebugSpawnJarAt, and that is the fix for the ladder's own
			// first run: the kit's spawn path enforces spec v14 §6's "max 3; a fourth despawns the
			// oldest", so the BRIGHTEST rung had been deleted before the shutter opened and the frame
			// showed three jars presented as four. The cap is a rule about Oyster's jars in a match; a
			// row of jars standing still to be photographed is not that, and going round it here keeps
			// the rule intact everywhere it means something. Initialise() with a zero velocity is the
			// same call the dash jar makes.
			FActorSpawnParameters LadderSpawn;
			LadderSpawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			LadderSpawn.ObjectFlags |= RF_Transient;

			for (int32 Index = 0; Index < 4; ++Index)
			{
				SetJarGlowCVar(LadderGlows[Index]);
				const float Offset = (static_cast<float>(Index) - 1.5f) * 95.f;

				ATraceOysterJar* Jar = WorldPtr->SpawnActor<ATraceOysterJar>(
					ATraceOysterJar::StaticClass(), FTransform(Centre + Across * Offset), LadderSpawn);
				if (Jar != nullptr)
				{
					Jar->Initialise(Comp, Pawn->GetTeam(), /*bPickler*/ false, FVector::ZeroVector);
					UE_LOG(LogTraceGame, Display,
						TEXT("[%s] rung %d at offset %+.0f uu: requested %.2f, built %.2f (%s)."),
						Test, Index, Offset, LadderGlows[Index], Jar->GetBuiltGlow(), *Jar->DescribeLook());
				}
			}
			SetJarGlowCVar(0.f);   // back to the shipped constant before anything else spawns a jar

			LookAt(WorldPtr, Run->ShotCamera, WorldPtr->GetFirstPlayerController(), Centre, 330.f, 40.f);
			Run->Step = 1;
			ScheduleLadder(Run, 0.5f);
			return false;
		}

		if (Run->Step == 1)
		{
			// The OFFSETS are printed with the rungs above, not just the values: Across is
			// Cross(toward, Up), which puts a NEGATIVE offset on the right of frame, and the ladder's
			// first read got its left and its right the wrong way round because nothing said so.
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] four rungs by offset (-142 uu is RIGHTMOST in frame): %.2f@-142 %.2f@-47 "
				     "%.2f@+47 %.2f@+142."),
				Test, LadderGlows[0], LadderGlows[1], LadderGlows[2], LadderGlows[3]);
			Run->ShotPath = Shoot(WorldPtr, TEXT("jarladder"));
			Run->Step = 2;
			ScheduleLadder(Run, 2.5f);
			return false;
		}

		ConfirmShot(Run->ShotPath);
		if (APlayerController* PC = WorldPtr->GetFirstPlayerController())
		{
			PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
		}
		if (ACameraActor* Camera = Run->ShotCamera.Get())
		{
			Camera->Destroy();
		}
		UE_LOG(LogTraceGame, Display, TEXT("[%s] ===== ladder complete. ====="), Test);
		return false;
	}

	FAutoConsoleCommand CmdOysterJarLadder(
		TEXT("Trace.Oyster.JarLadder"),
		TEXT("Dev only, SERVER. Stages four Oyster jars built at Glow 1.40 / 1.00 / 0.74 / 0.50 in one "
		     "row and photographs them in ONE frame, so the shipped value can be read off pixels rather "
		     "than argued about. Restores Trace.Oyster.JarGlow to 0 before it returns."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ScheduleLadder(MakeShared<FLadderRun>(), 0.f);
		}));

	FAutoConsoleCommand CmdOysterFxParade(
		TEXT("Trace.Oyster.FxParade"),
		TEXT("Dev only, SERVER. FX_AUDIO_PLAN §2.6. Stages Oyster's FX and photographs each one: a dash "
		     "jar beside a Pickler jar (the collar is the only thing that distinguishes them), the break "
		     "(cloud + JarPop + OysterJarBreak), and a poisoned victim's drips seen from outside. Prints "
		     "what it measured off the live actors as well as shooting it. Trace.Oyster.CloudTest is the "
		     "command that proves drawn == lethal; this one judges hue and shape."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			Schedule(MakeShared<FParadeRun>(), 0.f);
		}));
}

#endif // !UE_BUILD_SHIPPING
