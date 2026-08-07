#include "Gameplay/TraceMelee.h"

#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"                   // FTSTicker — the staged carrier-immunity scenario
#include "Engine/Engine.h"                       // GEngine, FWorldContext (the console helpers)
#include "Engine/World.h"
#include "EngineUtils.h"                         // TActorIterator
#include "GameFramework/Character.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Math/NumericLimits.h"                  // TNumericLimits<double>
#include "Math/UnrealMathUtility.h"

#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"                  // IsCoreHolder — the carrier immunity
#include "Gameplay/TraceHealthComponent.h"       // the health the staged test actually reads
#include "Gameplay/TraceWeaponComponent.h"       // the state this namespace is a facade over
#include "Net/TraceLagCompensationComponent.h"   // ResolveHitscan — ONE resolver, shared with the gun
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// Console overrides. Every one defaults to a negative sentinel meaning "defer to the setting"; see
// the block comment in TraceMelee.h. None of these shares a name with a console COMMAND — that
// collision is fatal at module load in this engine version, and the commands below are all verbs
// (Trace.Knife.Swap / .Swing / .AngleTest / .DumpSettings) while these are all nouns.
// =================================================================================================

static TAutoConsoleVariable<float> CVarKnifeBackstabDamage(
	TEXT("Trace.Knife.BackstabDamage"), -1.f,
	TEXT("Override for the back-stab damage. Negative defers to UTraceMeleeSettings."), ECVF_Default);

static TAutoConsoleVariable<float> CVarKnifeFrontDamage(
	TEXT("Trace.Knife.FrontDamage"), -1.f,
	TEXT("Override for the front-hit damage. Negative defers to UTraceMeleeSettings."), ECVF_Default);

static TAutoConsoleVariable<float> CVarKnifeBackstabAngle(
	TEXT("Trace.Knife.BackstabAngle"), -1.f,
	TEXT("Override for the back-stab half-angle in degrees. Negative defers to UTraceMeleeSettings."), ECVF_Default);

static TAutoConsoleVariable<float> CVarKnifeCooldown(
	TEXT("Trace.Knife.Cooldown"), -1.f,
	TEXT("Override for the seconds between swings. Negative defers to UTraceMeleeSettings."), ECVF_Default);

static TAutoConsoleVariable<float> CVarKnifeSwapSeconds(
	TEXT("Trace.Knife.SwapSeconds"), -1.f,
	TEXT("Override for the pullout time, both directions. Negative defers to UTraceMeleeSettings."), ECVF_Default);

static TAutoConsoleVariable<float> CVarKnifeRange(
	TEXT("Trace.Knife.Range"), -1.f,
	TEXT("Override for the blade reach in uu. Negative defers to UTraceMeleeSettings."), ECVF_Default);

// NOTE there is deliberately no Trace.Knife.SpeedMultiplier. The knife's movement numbers live on
// UTraceSettings and are owned by UTraceCharacterMovementComponent (KnifeMoveSpeedMultiplier and
// the two air-cap multipliers, all live-editable there). A second override here would be a second
// definition of the same number.

static TAutoConsoleVariable<int32> CVarKnifeBotAuto(
	TEXT("Trace.Knife.BotAuto"), 1,
	TEXT("1: bots swap to the knife to close on a nearby target and swing when in reach. 0: bots stay on the gun."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarKnifeDebug(
	TEXT("Trace.Knife.Debug"), 0,
	TEXT("1: log every swap, every swing and every resolution (attacker, victim, approach angle, damage)."),
	ECVF_Default);

/**
 * A/B ARM FOR THE CARRIER-IMMUNITY RULE. Default 1 = the shipped rule; 0 removes it.
 *
 * THIS EXISTS SO THE RULE CAN BE PROVEN, NOT SO IT CAN BE TURNED OFF. [USER-CONFIRMED] "the knife
 * cannot hurt the Core carrier" is the single easiest rule in this pass to get wrong, because a
 * knife intuitively "should" bypass a gun shield — and it had been verified only by reading the
 * code, never by staging a back-stab against a live carrier and watching them survive.
 *
 * A test that has never gone RED is not evidence. With this at 0, Trace.Knife.CarrierImmunityTest
 * kills the carrier with one back-stab and the harness reports FAIL, which is what proves the
 * harness can see the bug at all. At 1 — the shipped value, and the only value any real run uses —
 * the same staged back-stab leaves the carrier at full health.
 *
 * Deliberately NOT a UTraceSettings knob and NOT in any .ini: this is not tuning, and a designer
 * must never find a slider that switches off a rule the user confirmed by hand.
 */
static TAutoConsoleVariable<int32> CVarKnifeCarrierImmune(
	TEXT("Trace.Knife.CarrierImmune"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): the knife cannot hurt the Core carrier. 0: removes the rule, so "
	     "Trace.Knife.CarrierImmunityTest can be shown FAILING on a broken build. Never ship 0."),
	ECVF_Cheat);

/**
 * Count of knife resolutions that landed on a Core carrier. See GetCarrierKnifeHitCount.
 *
 * File-static rather than a member of anything: ResolveSwing is a free function shared by the
 * server path, the client's cosmetic prediction and the tests, and the fact being counted is about
 * the RULE, not about any one pawn or component.
 */
static int32 GCarrierKnifeHits = 0;

int32 TraceMelee::GetCarrierKnifeHitCount()
{
	return GCarrierKnifeHits;
}

// =================================================================================================
// Settings object
// =================================================================================================

UTraceMeleeSettings::UTraceMeleeSettings()
{
	// Values live on the property declarations in the header, next to the reasoning for each — the
	// same arrangement UTraceDamageSettings uses, and for the same reason: a constructor that also
	// sets defaults gives the next reader two places to look and one of them will go stale.
}

const UTraceMeleeSettings& UTraceMeleeSettings::Get()
{
	// Same reasoning as UTraceSettings::Get(): the CDO is what the config system populates and what
	// the Project Settings panel edits in place, so reading it at the point of use is what makes the
	// knife live-tunable during PIE.
	const UTraceMeleeSettings* Settings = GetDefault<UTraceMeleeSettings>();
	check(Settings != nullptr);
	return *Settings;
}

FName UTraceMeleeSettings::GetCategoryName() const
{
	return FName(TEXT("Game"));
}

const TCHAR* LexToString(ETraceEquippedWeapon Weapon)
{
	return (Weapon == ETraceEquippedWeapon::Knife) ? TEXT("KNIFE") : TEXT("GUN");
}

const TCHAR* LexToString(ETraceMeleeRefusal Refusal)
{
	switch (Refusal)
	{
	case ETraceMeleeRefusal::None:                 return TEXT("allowed");
	case ETraceMeleeRefusal::NoPawn:               return TEXT("no pawn / no weapon component");
	case ETraceMeleeRefusal::Dead:                 return TEXT("dead");
	case ETraceMeleeRefusal::Carrying:             return TEXT("carrying the Core");
	case ETraceMeleeRefusal::Dashing:              return TEXT("mid-dash");
	case ETraceMeleeRefusal::Deploying:            return TEXT("weapon still coming up");
	case ETraceMeleeRefusal::OnCooldown:           return TEXT("swing cooldown");
	case ETraceMeleeRefusal::WrongWeapon:          return TEXT("wrong weapon equipped");
	case ETraceMeleeRefusal::NotLocallyControlled: return TEXT("not locally controlled");
	default:                                       return TEXT("?");
	}
}

// =================================================================================================
// Tunable accessors — the ONLY readers of UTraceMeleeSettings' properties.
// =================================================================================================

namespace
{
	/** Console override if it is non-negative, otherwise the setting; then the clamp, always. */
	float ResolveFloat(const TAutoConsoleVariable<float>& CVar, float SettingValue, float Min, float Max)
	{
		const float Override = CVar.GetValueOnAnyThread();
		return FMath::Clamp((Override >= 0.f) ? Override : SettingValue, Min, Max);
	}

	/** The shared clock — the same one the gun stamps shots with and the parry anchors windows to. */
	double SharedClockSeconds(const UWorld* World)
	{
		if (World == nullptr)
		{
			return 0.0;
		}
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return GameState->GetServerWorldTimeSeconds();
		}
		return World->GetTimeSeconds();
	}

	/** The weapon component that owns the knife state for @p Character, or null. Safe on null. */
	UTraceWeaponComponent* WeaponOf(const AActor* Character)
	{
		const ATraceCharacter* TraceChar = Cast<ATraceCharacter>(Character);
		if (TraceChar == nullptr)
		{
			return nullptr;
		}
		return const_cast<ATraceCharacter*>(TraceChar)->FindComponentByClass<UTraceWeaponComponent>();
	}
}

float TraceMelee::GetBackstabDamage()
{
	return ResolveFloat(CVarKnifeBackstabDamage, UTraceMeleeSettings::Get().BackstabDamage, 1.f, 500.f);
}

float TraceMelee::GetFrontDamage()
{
	return ResolveFloat(CVarKnifeFrontDamage, UTraceMeleeSettings::Get().FrontDamage, 1.f, 500.f);
}

float TraceMelee::GetBackstabHalfAngleDegrees()
{
	return ResolveFloat(CVarKnifeBackstabAngle, UTraceMeleeSettings::Get().BackstabHalfAngleDegrees, 5.f, 179.f);
}

float TraceMelee::GetSwingCooldownSeconds()
{
	return ResolveFloat(CVarKnifeCooldown, UTraceMeleeSettings::Get().SwingCooldownSeconds, 0.05f, 5.f);
}

float TraceMelee::GetSwapSeconds()
{
	return ResolveFloat(CVarKnifeSwapSeconds, UTraceMeleeSettings::Get().SwapSeconds, 0.f, 2.f);
}

float TraceMelee::GetSwingWindupSeconds()
{
	// Charged AGAINST the cooldown rather than added to it, so it can never exceed it and produce a
	// swing whose blade resolves after the next swing is already legal.
	const float Windup = FMath::Clamp(UTraceMeleeSettings::Get().SwingWindupSeconds, 0.f, 0.4f);
	return FMath::Min(Windup, GetSwingCooldownSeconds() * 0.5f);
}

float TraceMelee::GetSwingAnimSeconds()
{
	return FMath::Clamp(UTraceMeleeSettings::Get().SwingAnimSeconds, 0.05f, 1.f);
}

float TraceMelee::GetSwingRangeUU()
{
	return ResolveFloat(CVarKnifeRange, UTraceMeleeSettings::Get().SwingRangeUU, 20.f, 1000.f);
}

float TraceMelee::GetSwingArcDegrees()
{
	return FMath::Clamp(UTraceMeleeSettings::Get().SwingArcDegrees, 5.f, 270.f);
}

int32 TraceMelee::GetSwingSamples()
{
	// Forced ODD so one sample always runs dead ahead: an even count would leave the crosshair
	// itself between two rays, which is the one direction a player is certain they aimed at.
	const int32 Samples = FMath::Clamp(UTraceMeleeSettings::Get().SwingSamples, 1, 51);
	return (Samples % 2 == 0) ? (Samples + 1) : Samples;
}

float TraceMelee::GetBotEngageRangeUU()
{
	return FMath::Clamp(UTraceMeleeSettings::Get().BotEngageRangeUU, 0.f, 5000.f);
}

float TraceMelee::GetBotDisengageRangeUU()
{
	// Strictly outside the engage range, or the band collapses and the bot swap-thrashes.
	return FMath::Max(GetBotEngageRangeUU() + 50.f,
		FMath::Clamp(UTraceMeleeSettings::Get().BotDisengageRangeUU, 0.f, 6000.f));
}

float TraceMelee::GetBotSwingRangeFraction()
{
	return FMath::Clamp(UTraceMeleeSettings::Get().BotSwingRangeFraction, 0.1f, 1.f);
}

bool TraceMelee::IsBotAutoKnifeEnabled()
{
	return CVarKnifeBotAuto.GetValueOnAnyThread() != 0;
}

bool TraceMelee::IsDebugLoggingEnabled()
{
	return CVarKnifeDebug.GetValueOnAnyThread() != 0;
}

FName TraceMelee::GetBackstabKillCause()
{
	static const FName Cause(TEXT("Backstab"));
	return Cause;
}

FName TraceMelee::GetKnifeKillCause()
{
	static const FName Cause(TEXT("Knife"));
	return Cause;
}

// =================================================================================================
// THE DAMAGE MODEL — pure, world-free, and the thing Trace.Knife.AngleTest pins.
// =================================================================================================

bool TraceMelee::IsBackstab(const FVector& AttackerLocation, const FVector& VictimLocation,
	float VictimYawDegrees, double* OutAngleDegrees)
{
	// Planar throughout. A knife to the back of the head from above a ledge is still a back-stab,
	// and folding Z into the angle would make it depend on how tall the attacker was standing.
	FVector Approach = VictimLocation - AttackerLocation;
	Approach.Z = 0.0;

	const FVector VictimForward = FRotator(0.f, VictimYawDegrees, 0.f).Vector();

	if (Approach.IsNearlyZero())
	{
		// Planar-coincident: the attacker is standing inside the victim. There is no approach
		// direction to measure, so this resolves to FRONT — the safe side. Defaulting a one-hit
		// kill to "yes" on a degenerate input is how a 100-damage bug ships.
		if (OutAngleDegrees != nullptr)
		{
			*OutAngleDegrees = 180.0;
		}
		return false;
	}

	Approach.Normalize();

	// Approach points attacker -> victim. Standing BEHIND the victim means that direction agrees
	// with the victim's forward, so a small angle here is a back-stab. See the header.
	const double Dot = FMath::Clamp(FVector::DotProduct(Approach, VictimForward), -1.0, 1.0);
	const double AngleDegrees = FMath::RadiansToDegrees(FMath::Acos(Dot));

	if (OutAngleDegrees != nullptr)
	{
		*OutAngleDegrees = AngleDegrees;
	}

	return AngleDegrees <= static_cast<double>(GetBackstabHalfAngleDegrees());
}

float TraceMelee::DamageForApproach(bool bBackstab)
{
	return bBackstab ? GetBackstabDamage() : GetFrontDamage();
}

// =================================================================================================
// RESOLUTION — the swept arc.
// =================================================================================================

FVector TraceMelee::GetSwingAxis(const ATraceCharacter* Attacker, const FVector& AimDirection)
{
	const FVector Forward = AimDirection.GetSafeNormal();
	if (Forward.IsNearlyZero())
	{
		return FVector::UpVector;
	}

	// Right is built from world up; Up is then rebuilt from Right, so the basis stays orthonormal
	// even at a steep pitch. Straight up or straight down degenerates the cross product, and there
	// the attacker's own right vector is the honest answer — a slash aimed at the ceiling still has
	// a shoulder deciding which way it goes.
	FVector Right = FVector::CrossProduct(Forward, FVector::UpVector);
	if (Right.IsNearlyZero())
	{
		Right = (Attacker != nullptr) ? Attacker->GetActorRightVector() : FVector::RightVector;
	}
	Right.Normalize();

	const FVector Axis = FVector::CrossProduct(Right, Forward).GetSafeNormal();
	return Axis.IsNearlyZero() ? FVector::UpVector : Axis;
}

ATraceCharacter* TraceMelee::ResolveSwing(
	UWorld* World,
	ATraceCharacter* Attacker,
	const FVector& Origin,
	const FVector& AimDirection,
	float RewindToServerTime,
	FTraceMeleeHit& OutHit)
{
	OutHit = FTraceMeleeHit();
	OutHit.ImpactPoint = Origin;

	if (World == nullptr || Attacker == nullptr)
	{
		return nullptr;
	}

	const FVector Forward = AimDirection.GetSafeNormal();
	if (Forward.IsNearlyZero() || Origin.ContainsNaN())
	{
		return nullptr;
	}

	// The arc's plane. ONE derivation, shared with ATraceMeleeArc — see GetSwingAxis.
	const FVector SwingAxis = GetSwingAxis(Attacker, Forward);

	const int32 SampleCount = GetSwingSamples();
	const float HalfArc = GetSwingArcDegrees() * 0.5f;
	const float Range = GetSwingRangeUU();

	// --- Sweep ----------------------------------------------------------------------------------
	//
	// ONE RESOLVER, SHARED WITH THE GUN. Every sample goes through the same
	// UTraceLagCompensationComponent::ResolveHitscan the hitscan uses, so the knife inherits the
	// rewound pose history, static-geometry occlusion, the friendly-fire rule and the self/dead
	// exclusions as CODE rather than as intention. See the header.
	double BestDistance = TNumericLimits<double>::Max();

	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		const float Alpha = (SampleCount > 1)
			? (static_cast<float>(Index) / static_cast<float>(SampleCount - 1)) * 2.f - 1.f
			: 0.f;
		const FVector SampleDir = Forward.RotateAngleAxis(Alpha * HalfArc, SwingAxis);

		FVector Impact = Origin;
		ETraceHitZone Zone = ETraceHitZone::None;
		ATraceCharacter* Candidate = UTraceLagCompensationComponent::ResolveHitscan(
			World, Attacker, Origin, SampleDir, Range, RewindToServerTime, Impact, Zone);

		if (Candidate == nullptr)
		{
			continue;
		}

		// *** THE CARRIER IMMUNITY. [USER-CONFIRMED] — see the header before touching this. ***
		//
		// ResolveHitscan already skips a carrier whose shield is UP, so without these two lines the
		// knife would be immune-by-accident for most of the match and lethal for exactly the half
		// second that matters: the pass window, where ATraceCore::IsShieldSuppressedFor drops the
		// shield so bullets can land. The user's rule is not "the knife obeys the bullet rule", it
		// is "the knife cannot hurt the carrier" — the trace stays the only way to kill one, or a
		// back-stab makes dashing the trace pointless. The blade stops on the shield; the sample is
		// consumed rather than passing through to somebody standing behind them.
		//
		// The CVarKnifeCarrierImmune arm is a TEST HOOK and defaults to the rule being on; see its
		// declaration. It is read here, at the single place the rule is implemented, so that the
		// RED arm exercises exactly this branch and not a parallel copy of it.
		if (ATraceCore::IsCoreHolder(Candidate) && CVarKnifeCarrierImmune.GetValueOnAnyThread() != 0)
		{
			OutHit.bBlockedByCarrierShield = true;
			continue;
		}

		const double Distance = FVector::Dist(Origin, Impact);
		if (Distance >= BestDistance)
		{
			continue;
		}

		BestDistance = Distance;
		OutHit.Victim = Candidate;
		OutHit.ImpactPoint = Impact;
		OutHit.Zone = Zone;
		OutHit.SampleIndex = Index;
	}

	if (OutHit.Victim == nullptr)
	{
		return nullptr;
	}

	// THE ALARM. See TraceMelee::GetCarrierKnifeHitCount. Reaching here with a carrier as the victim
	// is only possible when the immunity branch above has been disarmed, which outside the test arm
	// means the rule has been deleted. Counted at the one line where the fact is known, because the
	// victim's health is a shared resource that ambient combat also writes to.
	if (ATraceCore::IsCoreHolder(OutHit.Victim))
	{
		++GCarrierKnifeHits;
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Knife] *** A KNIFE RESOLVED ONTO THE CORE CARRIER %s (hit #%d). The carrier is supposed to be "
			     "immune to melee — spec v10 s1, USER-CONFIRMED. Check Trace.Knife.CarrierImmune and ResolveSwing."),
			*GetNameSafe(OutHit.Victim), GCarrierKnifeHits);
	}

	// --- Back or front --------------------------------------------------------------------------
	//
	// Both positions are taken at the REWOUND instant so the verdict matches what the attacker saw:
	// the victim's capsule centre from the pose history the gun already keeps, and the victim's yaw
	// from the ring UTraceWeaponComponent keeps for exactly this (FTraceLagCompFrame has no
	// rotation — a bullet does not care which way you are looking, and this is the one thing that
	// does). A stale yaw would flip the verdict in the direction the attacker cannot see.
	FVector VictimLocation = OutHit.Victim->GetActorLocation();
	if (const UTraceLagCompensationComponent* VictimLagComp = OutHit.Victim->FindComponentByClass<UTraceLagCompensationComponent>())
	{
		FTraceLagCompFrame Frame;
		if (VictimLagComp->GetPoseAtTime(RewindToServerTime, Frame))
		{
			VictimLocation = Frame.CapsuleCenter;
		}
	}

	float VictimYaw = static_cast<float>(OutHit.Victim->GetActorRotation().Yaw);
	OutHit.bVictimYawRewound = UTraceWeaponComponent::GetFacingYawAtTime(OutHit.Victim, RewindToServerTime, VictimYaw);
	OutHit.VictimYawDegrees = VictimYaw;

	OutHit.bBackstab = IsBackstab(Origin, VictimLocation, VictimYaw, &OutHit.ApproachAngleDegrees);
	OutHit.Damage = DamageForApproach(OutHit.bBackstab);

	if (IsDebugLoggingEnabled())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Knife] %s -> %s | sample %d of %d | approach %.1fdeg (threshold %.0f) | %s | %.0f damage | victim yaw %.1f (%s) | %.0fuu"),
			*GetNameSafe(Attacker), *GetNameSafe(OutHit.Victim), OutHit.SampleIndex, SampleCount,
			OutHit.ApproachAngleDegrees, GetBackstabHalfAngleDegrees(),
			OutHit.bBackstab ? TEXT("BACKSTAB") : TEXT("front"), OutHit.Damage,
			OutHit.VictimYawDegrees, OutHit.bVictimYawRewound ? TEXT("rewound") : TEXT("live"),
			BestDistance);
	}

	return OutHit.Victim;
}

// =================================================================================================
// CROSS-SLICE QUERIES
// =================================================================================================

bool TraceMelee::IsKnifeEquipped(const AActor* Character)
{
	const UTraceWeaponComponent* Weapon = WeaponOf(Character);
	return (Weapon != nullptr) && Weapon->GetEquippedWeapon() == ETraceEquippedWeapon::Knife;
}

bool TraceMelee::ShouldUseKnifeMovementProfile(const AActor* Character)
{
	if (!IsKnifeEquipped(Character))
	{
		return false;
	}

	// THE CARRIER CLAUSE. RequestEquip already refuses a swap while carrying, so a carrier only
	// reaches this by picking the Core up with the knife already out — and at that moment the knife
	// is STOWED, not active. See the header: this stops 1.08 x 1.30 = 1.40x from making the Core
	// uncatchable and quietly retiring the one number the carrier's speed was ever tuned with.
	const ATraceCharacter* TraceChar = Cast<ATraceCharacter>(Character);
	return (TraceChar == nullptr) || !TraceChar->IsCarrier();
}

float TraceMelee::GetSwingCooldownRemaining(const AActor* Character)
{
	const UTraceWeaponComponent* Weapon = WeaponOf(Character);
	return (Weapon != nullptr) ? Weapon->GetSwingCooldownRemaining() : 0.f;
}

float TraceMelee::GetDeployRemaining(const AActor* Character)
{
	const UTraceWeaponComponent* Weapon = WeaponOf(Character);
	return (Weapon != nullptr) ? Weapon->GetDeployRemaining() : 0.f;
}

// =================================================================================================
// ENTRY POINTS
// =================================================================================================

bool TraceMelee::RequestSwapWeapon(ATraceCharacter* Pawn, ETraceMeleeRefusal* OutRefusal)
{
	UTraceWeaponComponent* Weapon = WeaponOf(Pawn);
	if (Weapon == nullptr)
	{
		if (OutRefusal != nullptr) { *OutRefusal = ETraceMeleeRefusal::NoPawn; }
		return false;
	}

	const ETraceEquippedWeapon Desired = (Weapon->GetEquippedWeapon() == ETraceEquippedWeapon::Gun)
		? ETraceEquippedWeapon::Knife
		: ETraceEquippedWeapon::Gun;

	return Weapon->RequestEquip(Desired, OutRefusal);
}

bool TraceMelee::RequestEquip(ATraceCharacter* Pawn, ETraceEquippedWeapon Desired, ETraceMeleeRefusal* OutRefusal)
{
	UTraceWeaponComponent* Weapon = WeaponOf(Pawn);
	if (Weapon == nullptr)
	{
		if (OutRefusal != nullptr) { *OutRefusal = ETraceMeleeRefusal::NoPawn; }
		return false;
	}
	return Weapon->RequestEquip(Desired, OutRefusal);
}

bool TraceMelee::RequestSwing(ATraceCharacter* Pawn, ETraceMeleeRefusal* OutRefusal)
{
	UTraceWeaponComponent* Weapon = WeaponOf(Pawn);
	if (Weapon == nullptr)
	{
		if (OutRefusal != nullptr) { *OutRefusal = ETraceMeleeRefusal::NoPawn; }
		return false;
	}
	return Weapon->StartSwing(OutRefusal);
}

bool TraceMelee::IsInSwingRange(const ATraceCharacter* Attacker, const AActor* Target)
{
	if (Attacker == nullptr || Target == nullptr)
	{
		return false;
	}

	// Centre to centre, slackened by both capsule radii, because the reach is measured to a SURFACE
	// and this predicate is measured between two origins.
	double Slack = 0.0;
	if (const UCapsuleComponent* AttackerCapsule = Attacker->GetCapsuleComponent())
	{
		Slack += AttackerCapsule->GetScaledCapsuleRadius();
	}
	if (const ACharacter* TargetCharacter = Cast<ACharacter>(Target))
	{
		if (const UCapsuleComponent* TargetCapsule = TargetCharacter->GetCapsuleComponent())
		{
			Slack += TargetCapsule->GetScaledCapsuleRadius();
		}
	}

	const double Reach = static_cast<double>(GetSwingRangeUU()) * GetBotSwingRangeFraction() + Slack;
	return FVector::DistSquared(Attacker->GetActorLocation(), Target->GetActorLocation()) <= Reach * Reach;
}

// =================================================================================================
// SELF-TEST — the back/front model, pinned without a world.
//
// It exists because the angle rule is the ONE part of this weapon that cannot be checked by looking:
// a screenshot of a knife hit does not say which side of a 90 degree boundary the attacker was on,
// and the whole feature is a 3.3x damage swing across that boundary. Every case below states the
// geometry in words so a failure names the rule it broke rather than a number.
// =================================================================================================

int32 TraceRunMeleeSelfTest()
{
	int32 Failures = 0;

	struct FCase
	{
		const TCHAR* What;
		FVector Attacker;
		FVector Victim;
		float VictimYaw;
		bool bExpectBackstab;
	};

	// The victim always stands at the origin. Yaw 0 faces +X, so an attacker at -X is DEAD BEHIND.
	const FCase Cases[] =
	{
		{ TEXT("dead behind (attacker at -X, victim faces +X)"),          FVector(-100, 0, 0),   FVector::ZeroVector, 0.f,    true  },
		{ TEXT("dead in front (attacker at +X, victim faces +X)"),        FVector(100, 0, 0),    FVector::ZeroVector, 0.f,    false },
		{ TEXT("directly to the victim's left (89.9deg from behind)"),    FVector(0, -100, 0),   FVector::ZeroVector, 0.f,    true  },
		{ TEXT("directly to the victim's right (89.9deg from behind)"),   FVector(0, 100, 0),    FVector::ZeroVector, 0.f,    true  },
		{ TEXT("45deg behind-left"),                                      FVector(-100, -100, 0), FVector::ZeroVector, 0.f,   true  },
		{ TEXT("45deg in front-right"),                                   FVector(100, 100, 0),  FVector::ZeroVector, 0.f,    false },
		{ TEXT("behind a victim who has turned 180deg (now in front)"),   FVector(-100, 0, 0),   FVector::ZeroVector, 180.f,  false },
		{ TEXT("in front of a victim who has turned 180deg (now behind)"),FVector(100, 0, 0),    FVector::ZeroVector, 180.f,  true  },
		{ TEXT("behind and 400uu above — Z must not change the verdict"), FVector(-100, 0, 400), FVector::ZeroVector, 0.f,    true  },
		{ TEXT("planar-coincident — degenerate, must resolve to FRONT"),  FVector(0, 0, 90),     FVector::ZeroVector, 0.f,    false },
		{ TEXT("offset victim, attacker behind them at yaw 90"),          FVector(500, 400, 0),  FVector(500, 500, 0), 90.f,  true  },
		{ TEXT("offset victim, attacker in front of them at yaw 90"),     FVector(500, 600, 0),  FVector(500, 500, 0), 90.f,  false },
	};

	const float Threshold = TraceMelee::GetBackstabHalfAngleDegrees();
	const float BackDamage = TraceMelee::GetBackstabDamage();
	const float FrontDamage = TraceMelee::GetFrontDamage();

	UE_LOG(LogTraceGame, Display, TEXT("========== TRACE KNIFE ANGLE TEST =========="));
	UE_LOG(LogTraceGame, Display,
		TEXT("[KnifeTest] model: back-stab when angle(approach, victim forward) <= %.1fdeg -> %.0f damage, else %.0f."),
		Threshold, BackDamage, FrontDamage);

	for (const FCase& Case : Cases)
	{
		double Angle = -1.0;
		const bool bBackstab = TraceMelee::IsBackstab(Case.Attacker, Case.Victim, Case.VictimYaw, &Angle);
		const float Damage = TraceMelee::DamageForApproach(bBackstab);
		const bool bPass = (bBackstab == Case.bExpectBackstab);

		if (!bPass)
		{
			++Failures;
		}

		// UE_LOG's verbosity is a compile-time token, so a failing case has to take its own branch
		// rather than selecting the verbosity with a ternary.
		const FString Line = FString::Printf(
			TEXT("[KnifeTest] %s  %-62s angle=%6.1fdeg -> %s (%.0f damage), expected %s"),
			bPass ? TEXT("PASS") : TEXT("FAIL"), Case.What, Angle,
			bBackstab ? TEXT("BACKSTAB") : TEXT("front   "), Damage,
			Case.bExpectBackstab ? TEXT("BACKSTAB") : TEXT("front"));

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("%s"), *Line);
		}
	}

	// The two numbers the user gave, checked as numbers rather than as intentions. A 100-damage
	// back-stab that does not kill, or a front hit that does, is the feature failing silently.
	const float MaxHealth = FMath::Max(1.f, UTraceSettings::Get().MaxHealth);
	if (BackDamage < MaxHealth)
	{
		++Failures;
		UE_LOG(LogTraceGame, Error,
			TEXT("[KnifeTest] FAIL  back-stab damage %.0f does not kill a full-health player (%.0f)."),
			BackDamage, MaxHealth);
	}
	else
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeTest] PASS  back-stab %.0f >= MaxHealth %.0f: one hit, one kill."), BackDamage, MaxHealth);
	}

	if (FrontDamage >= MaxHealth)
	{
		++Failures;
		UE_LOG(LogTraceGame, Error,
			TEXT("[KnifeTest] FAIL  front damage %.0f kills outright; the front hit must not be a one-shot."),
			FrontDamage);
	}
	else
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeTest] PASS  front %.0f < MaxHealth %.0f: %d front swings to kill, %.2fs apart."),
			FrontDamage, MaxHealth, FMath::CeilToInt(MaxHealth / FrontDamage), TraceMelee::GetSwingCooldownSeconds());
	}

	// Arc coverage: no body may sit between two adjacent rays anywhere in the sweep. This is
	// arithmetic, not opinion, and it is the difference between "the knife missed" and "the knife is
	// broken" in a bug report.
	const int32 Samples = TraceMelee::GetSwingSamples();
	const double ChordUU = (Samples > 1)
		? (2.0 * TraceMelee::GetSwingRangeUU()
			* FMath::Sin(FMath::DegreesToRadians(TraceMelee::GetSwingArcDegrees()) / (2.0 * (Samples - 1))))
		: 0.0;
	if (ChordUU > 34.5)
	{
		++Failures;
		UE_LOG(LogTraceGame, Error,
			TEXT("[KnifeTest] FAIL  arc sample spacing %.1fuu exceeds the 34uu capsule radius: a body can slip between rays."),
			ChordUU);
	}
	else
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeTest] PASS  %d samples over %.0fdeg at %.0fuu -> %.1fuu between adjacent rays (capsule radius is 34)."),
			Samples, TraceMelee::GetSwingArcDegrees(), TraceMelee::GetSwingRangeUU(), ChordUU);
	}

	if (Failures == 0)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[KnifeTest] 0 failures."));
	}
	else
	{
		UE_LOG(LogTraceGame, Error, TEXT("[KnifeTest] %d failure(s)."), Failures);
	}
	UE_LOG(LogTraceGame, Display, TEXT("==========================================="));

	return Failures;
}

// =================================================================================================
// Console surface. Verbs only — see the note at the top of this file about names colliding with the
// cvars above.
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace TraceMeleeConsole
{
	/** The pawn the local player is looking out of, in whichever game world is running. */
	ATraceCharacter* FindLocalTraceCharacter()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* TestWorld = Context.World();
			if (TestWorld == nullptr || !TestWorld->IsGameWorld())
			{
				continue;
			}
			if (APlayerController* LocalController = TestWorld->GetFirstPlayerController())
			{
				if (ATraceCharacter* LocalCharacter = Cast<ATraceCharacter>(LocalController->GetPawn()))
				{
					return LocalCharacter;
				}
			}
		}
		return nullptr;
	}

	FAutoConsoleCommand CmdSwap(
		TEXT("Trace.Knife.Swap"),
		TEXT("Dev only. Swap the local player between the gun and the knife, exactly as the bind does."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ATraceCharacter* Local = FindLocalTraceCharacter();
			ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::NoPawn;
			const bool bSwapped = TraceMelee::RequestSwapWeapon(Local, &Refusal);
			UE_LOG(LogTraceGame, Display, TEXT("[Knife] Trace.Knife.Swap on %s: %s (%s)"),
				*GetNameSafe(Local), bSwapped ? TEXT("swapping") : TEXT("REFUSED"), LexToString(Refusal));
		}));

	/**
	 * Trace.Knife.SwapIn <seconds> — the swap, but later.
	 *
	 * -ExecCmds runs at startup, and on a CLIENT that is well before the pawn has been replicated
	 * and possessed. A bare Trace.Knife.Swap in an unattended run therefore reports "REFUSED
	 * (NoPawn)" and the run silently measures the gun. This is the version an offscreen client can
	 * actually use; it retries until a pawn exists rather than firing once into an empty world.
	 */
	FAutoConsoleCommand CmdSwapIn(
		TEXT("Trace.Knife.SwapIn"),
		TEXT("Dev only. Trace.Knife.SwapIn <seconds> — swap once the delay has passed AND a local pawn exists."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const double Delay = (Args.Num() > 0) ? FMath::Max(0.0, FCString::Atod(*Args[0])) : 5.0;
			const double Deadline = FPlatformTime::Seconds() + Delay;
			// Retries for a further 60 s after the delay: a client that is still travelling has no
			// pawn yet, and giving up on the first attempt is what made the bare command useless.
			const double GiveUp = Deadline + 60.0;

			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Deadline, GiveUp](float) -> bool
			{
				const double Now = FPlatformTime::Seconds();
				if (Now < Deadline)
				{
					return true;
				}

				ATraceCharacter* Local = FindLocalTraceCharacter();
				ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::NoPawn;
				if (Local != nullptr && TraceMelee::RequestSwapWeapon(Local, &Refusal))
				{
					UE_LOG(LogTraceGame, Display, TEXT("[Knife] Trace.Knife.SwapIn fired on %s -> %s"),
						*GetNameSafe(Local),
						TraceMelee::IsKnifeEquipped(Local) ? TEXT("KNIFE") : TEXT("GUN"));
					return false;
				}

				if (Now > GiveUp)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[Knife] Trace.Knife.SwapIn gave up: pawn=%s, last refusal %s"),
						*GetNameSafe(Local), LexToString(Refusal));
					return false;
				}
				return true;
			}), 0.f);
		}));

	FAutoConsoleCommand CmdSwing(
		TEXT("Trace.Knife.Swing"),
		TEXT("Dev only. Swing the local player's knife, exactly as the fire bind does while it is equipped."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ATraceCharacter* Local = FindLocalTraceCharacter();
			ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::NoPawn;
			const bool bSwung = TraceMelee::RequestSwing(Local, &Refusal);
			UE_LOG(LogTraceGame, Display, TEXT("[Knife] Trace.Knife.Swing on %s: %s (%s)"),
				*GetNameSafe(Local), bSwung ? TEXT("swinging") : TEXT("REFUSED"), LexToString(Refusal));
		}));

	// =============================================================================================
	// Trace.Knife.CarrierImmunityTest — the [USER-CONFIRMED] rule, proven at runtime.
	//
	// WHY A STAGED SCENARIO AND NOT AN ASSERTION. "The knife cannot hurt the carrier" was verified
	// by reading ResolveSwing and by reasoning about it, which is exactly the kind of "verified"
	// this project has learned to treat as a hypothesis. Nobody had ever swung a knife into the
	// back of a living carrier and watched the health bar.
	//
	// TWO CASES, AND ONLY THE SECOND ONE IS INTERESTING:
	//
	//   CASE A  shield UP. The carrier is already skipped by ResolveHitscan, which every knife
	//           sample runs through. This case passes even on a build where the melee rule has been
	//           deleted, and it is here to show that — a test suite where every case always passes
	//           teaches nothing about which line is load-bearing.
	//
	//   CASE B  shield SUPPRESSED, mid-pass. ResolveHitscan lets the sample through on purpose (the
	//           §4 risk beat: mid-pass you are shootable). ONLY TraceMelee::ResolveSwing's own rule
	//           stops the blade here. This is the half-second that decides games, it is the case
	//           that goes RED under Trace.Knife.CarrierImmune 0, and it is the whole point.
	//
	// Both cases are BACK-STABS (100 damage, a one-shot kill on a full-health pawn), staged by
	// placing the attacker on the victim's forward axis — see TraceMelee::IsBackstab for why that
	// is the rear hemisphere.
	//
	// Driven off a ticker rather than a single call because the real path has real latencies in it:
	// a 0.2 s pullout, a swing wind-up before the blade goes live, and a 0.5 s cooldown between the
	// two swings. Short-circuiting any of them would test a path the game does not use.
	//
	// ===========================================================================================
	// AS MEASURED — READ THIS BEFORE BELIEVING THE VERDICT LINE
	//
	// This harness has NOT yet produced a red-then-green proof, and it says so itself rather than
	// reporting PASS. Across four staged runs:
	//
	//   GREEN arm, case B: FULLY STAGED and clean, repeatedly. shieldDownAtPress=1,
	//     heldToResolve=1, the victim held as the carrier across the whole flight of the blade,
	//     bladesOntoCarrier=0, health moved 0. That is a real runtime observation of the rule.
	//
	//   RED arm, case B: NEVER STAGED. With the rule disabled the blade did not land on ANYTHING —
	//     `Trace.Knife.Debug 1` logs every resolution that finds a victim, and across six staged
	//     swings there were ZERO resolution lines. So the red arm does not falsify anything; the
	//     sweep is missing for a reason not yet found (candidates: the per-tick teleport in
	//     PlaceBehind versus ServerSwing's MaxSwingOriginErrorUU plausibility check, or the
	//     lag-compensated pose history disagreeing with a pawn that is being teleported every
	//     frame).
	//
	//   A TRAP THIS HARNESS FELL INTO AND NOW GUARDS AGAINST: the first version judged on the
	//     victim's HEALTH and reported the rule broken on a 100-point drop that was an ordinary
	//     trace kill by a bot (cause 'Trail') landing inside the measurement window. Health has
	//     many writers. GetCarrierKnifeHitCount() has one, at the line where the rule is decided,
	//     and it is what the verdict now reads.
	//
	// So: the carrier's melee immunity is verified STATICALLY (ResolveSwing's branch is
	// unambiguous) and its OBSERVABLE behaviour is verified at runtime, but the discipline this
	// project runs on says an arm that never went red leaves the claim a hypothesis. Fix the red
	// arm before calling it proven.
	// ===========================================================================================
	//
	// BOTH ARMS RUN IN ONE INVOCATION, RED FIRST, and that is not a convenience. A harness that has
	// only ever been run against the fixed build is not evidence of anything — it is a function that
	// returns PASS. Running the arms in the other order, or leaving the red one to be requested by
	// hand, is how a "verified" claim rots into folklore. So the command cannot be run without its
	// own falsification: arm 0 deletes the rule and MUST report the carrier taking 100 damage.
	//
	// The RED arm kills the carrier, which is the whole point but also means the GREEN arm has no
	// victim for a moment. Phase -1 re-acquires participants and waits for the Core to find a new
	// holder rather than assuming one is standing there.
	// =============================================================================================
	struct FCarrierImmunityTest
	{
		TWeakObjectPtr<ATraceCharacter> Attacker;
		TWeakObjectPtr<ATraceCharacter> Victim;
		double StartTime = 0.0;
		double AcquireDeadline = 0.0;
		/** -1 acquiring participants, 0..3 the staged sequence. */
		int32 Phase = -1;
		/** 0 = RED (rule removed), 1 = GREEN (shipped rule). Runs 0 then 1. */
		int32 Arm = 0;
		float HealthBeforeA = 0.f;
		float HealthAfterA = 0.f;
		float HealthBeforeB = 0.f;
		float HealthAfterB = 0.f;
		bool bShieldUpAtA = false;
		bool bShieldDownAtB = false;
		/** Was the shield suppressed on EVERY tick from case B's press to its resolve? */
		bool bShieldHeldThroughB = true;
		bool bSwungA = false;
		bool bSwungB = false;
		/**
		 * THE ACTUAL VERDICT SIGNAL — TraceMelee::GetCarrierKnifeHitCount() sampled at the start of
		 * each case, so a case is judged on "did a blade resolve onto the carrier" rather than on a
		 * health delta. The first version of this test used health and was wrong twice in one run:
		 * it read a 100-point drop caused by an ordinary bot shooting the carrier and reported the
		 * immunity rule broken. Health has many writers; this counter has one.
		 */
		int32 CarrierHitsAtCaseA = 0;
		int32 CarrierHitsAtCaseB = 0;
		int32 CaseACarrierHits = 0;
		int32 CaseBCarrierHits = 0;
		/** Elapsed at each press, so the resolve windows follow the presses rather than the clock. */
		double SwingATime = 0.0;
		double SwingBTime = 0.0;
		/** Verdicts kept across arms so the final line can compare them. */
		bool bRedCaseBDamaged = false;
		bool bRedValid = false;
		/**
		 * Restarts left for the CURRENT arm.
		 *
		 * A staged scenario inside a live match is a race against the match. Twice in the first runs
		 * the arm was lost to ordinary play: once the victim was shot dead by a third party
		 * mid-sequence, and once the Core transferred to the ATTACKER when the carrier died — after
		 * which the attacker is refused with "carrying the Core", because carriers cannot swing.
		 * Both are the game behaving correctly, so the harness restarts rather than recording a
		 * false negative. Reporting "did not reproduce" when the setup fell apart is exactly the
		 * mistake a sibling harness made this pass, and it cost five good samples.
		 */
		int32 RetriesLeft = 6;
	};

	/**
	 * Puts the knife back in the attacker's hand if the bot AI has swapped it away, and reports
	 * whether it is out AND deployed.
	 *
	 * NEEDED because TickBotKnife is doing its job: it disengages to the gun when no non-carrier
	 * enemy is near, and the staged attacker is standing next to a CARRIER, whom the bot logic
	 * deliberately does not count as a knife target. The first staged run lost the interesting case
	 * to exactly this — "wrong weapon equipped" at the moment of the swing. That is correct game
	 * behaviour, so the test adapts rather than the game.
	 */
	bool EnsureKnifeReady(ATraceCharacter* Attacker)
	{
		if (Attacker == nullptr)
		{
			return false;
		}
		if (!TraceMelee::IsKnifeEquipped(Attacker))
		{
			TraceMelee::RequestEquip(Attacker, ETraceEquippedWeapon::Knife);
			return false;   // deploying — the caller waits rather than swinging into the pullout
		}
		return TraceMelee::GetDeployRemaining(Attacker) <= 0.f;
	}

	/** The living, locally-controlled enemy this test can swing with, or null. */
	ATraceCharacter* FindImmunityAttacker(UWorld* World, const ATraceCharacter* Victim)
	{
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Victim || !Candidate->IsAlive() || Candidate->IsCarrier())
			{
				continue;
			}
			// An ENEMY (friendly fire is excluded inside ResolveHitscan, so a teammate would produce
			// a pass for entirely the wrong reason) and locally controlled, which on a server means a
			// bot — StartSwing refuses a proxy, exactly as it refuses one for a human.
			if (Candidate->GetTeam() == Victim->GetTeam() || !Candidate->IsLocallyControlled())
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	/** Holds the attacker on the victim's forward axis, facing them. Re-applied every tick. */
	void PlaceBehind(ATraceCharacter* Attacker, const ATraceCharacter* Victim)
	{
		if (Attacker == nullptr || Victim == nullptr)
		{
			return;
		}

		const FRotator VictimRot(0.f, Victim->GetActorRotation().Yaw, 0.f);
		// 60% of the blade's reach: comfortably inside it, and far enough out that the two capsules
		// are not interpenetrating (which would make the approach vector degenerate and, by
		// IsBackstab's own degenerate rule, resolve to FRONT — the wrong case entirely).
		const double Distance = static_cast<double>(TraceMelee::GetSwingRangeUU()) * 0.6;
		const FVector Where = Victim->GetActorLocation() - VictimRot.Vector() * Distance;

		Attacker->SetActorLocationAndRotation(Where, VictimRot, false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* AttackerController = Attacker->GetController())
		{
			// The aim direction, not just the mesh: the swing origin and arc come from where the
			// pawn is LOOKING, and a bot's control rotation is independent of its actor rotation.
			AttackerController->SetControlRotation(VictimRot);
		}
	}

	FAutoConsoleCommand CmdCarrierImmunityTest(
		TEXT("Trace.Knife.CarrierImmunityTest"),
		TEXT("Dev only, server only. Runs BOTH arms, RED first: stages two back-stabs into a live Core "
		     "carrier (shield up, then mid-pass with the shield suppressed) with the immunity rule "
		     "removed, then again with it in place. Arm 0 must FAIL or the test proves nothing."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UWorld* World = nullptr;
			if (GEngine != nullptr)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					UWorld* TestWorld = Context.World();
					if (TestWorld != nullptr && TestWorld->IsGameWorld() && TestWorld->GetAuthGameMode() != nullptr)
					{
						World = TestWorld;
						break;
					}
				}
			}
			if (World == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[KNIFECARRIER] no authoritative game world — run this on the server."));
				return;
			}

			TSharedPtr<FCarrierImmunityTest> State = MakeShared<FCarrierImmunityTest>();
			State->Arm = 0;                       // RED first. See the block comment above.
			State->Phase = -1;
			State->StartTime = World->GetTimeSeconds();
			State->AcquireDeadline = State->StartTime + 60.0;

			// THE BOTS MUST STOP DECIDING FOR THEMSELVES FOR THE NEXT FEW SECONDS.
			//
			// TickBotKnife re-decides four times a second and disengages to the gun when no
			// NON-CARRIER enemy is within BotDisengageRangeUU. The staged attacker is standing 108 uu
			// behind a CARRIER — whom that logic deliberately does not count as a knife target — so
			// the bot puts the knife away between the setup and the swing and the case is lost to
			// "wrong weapon equipped". That happened on the first two staged runs. The bot is right
			// and the harness is the thing that has to yield, so the auto-knife is parked for the
			// duration and restored on every exit path below.
			CVarKnifeBotAuto->Set(0, ECVF_SetByConsole);

			UE_LOG(LogTraceGame, Display,
				TEXT("[KNIFECARRIER] ===== starting. Arm 0 = RED (rule REMOVED, must damage). Arm 1 = GREEN (shipped). "
				     "Bot auto-knife parked for the duration. ====="));

			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
			{
				UWorld* TickWorld = WeakWorld.Get();
				if (TickWorld == nullptr)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[KNIFECARRIER] ABORTED: the world went away mid-test."));
					return false;
				}

				// ---- Phase -1: acquire, and WAIT rather than assume ---------------------------
				//
				// Both at the very start and between the two arms, because the RED arm kills the
				// carrier by design and the Core needs a moment to find a new holder. Polling here
				// rather than trusting a fixed sleep is what stops this reporting SKIPPED on a build
				// where everything was working — the exact failure a sibling harness had this pass.
				if (State->Phase < 0)
				{
					ATraceCore* AcquireCore = ATraceCore::Get(TickWorld);
					ATraceCharacter* NewVictim = (AcquireCore != nullptr) ? AcquireCore->Carrier : nullptr;
					ATraceCharacter* NewAttacker = (NewVictim != nullptr && NewVictim->IsAlive())
						? FindImmunityAttacker(TickWorld, NewVictim) : nullptr;

					if (NewVictim == nullptr || NewAttacker == nullptr)
					{
						if (TickWorld->GetTimeSeconds() > State->AcquireDeadline)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[KNIFECARRIER] arm=%d SKIPPED: no living carrier + locally-controlled enemy within the acquire window."),
								State->Arm);
							return false;
						}
						return true;
					}

					State->Attacker = NewAttacker;
					State->Victim = NewVictim;
					State->StartTime = TickWorld->GetTimeSeconds();
					State->Phase = 0;

					// The arm is applied HERE, not by the caller, so the command carries its own
					// falsification and cannot be run in a way that skips the red half.
					CVarKnifeCarrierImmune->Set(State->Arm, ECVF_SetByConsole);

					// Knife out now, so the 0.2 s pullout has expired before the first swing.
					ETraceMeleeRefusal EquipRefusal = ETraceMeleeRefusal::None;
					const bool bEquipped = TraceMelee::RequestEquip(NewAttacker, ETraceEquippedWeapon::Knife, &EquipRefusal);

					UE_LOG(LogTraceGame, Display,
						TEXT("[KNIFECARRIER] --- arm=%d (%s) | attacker %s (team %d) | victim %s (team %d, health %.0f) | equip=%d (%s)"),
						State->Arm, (State->Arm == 0) ? TEXT("RED, rule removed") : TEXT("GREEN, shipped rule"),
						*GetNameSafe(NewAttacker), static_cast<int32>(NewAttacker->GetTeam()),
						*GetNameSafe(NewVictim), static_cast<int32>(NewVictim->GetTeam()),
						(NewVictim->Health != nullptr) ? NewVictim->Health->Health : -1.f,
						bEquipped ? 1 : 0, LexToString(EquipRefusal));
					return true;
				}

				ATraceCharacter* TickAttacker = State->Attacker.Get();
				ATraceCharacter* TickVictim = State->Victim.Get();
				if (TickAttacker == nullptr || TickVictim == nullptr)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[KNIFECARRIER] ABORTED: a participant went away mid-test."));
					CVarKnifeCarrierImmune->Set(1, ECVF_SetByConsole);
					CVarKnifeBotAuto->Set(1, ECVF_SetByConsole);   // the bots get their knife back
					return false;
				}

				const double Elapsed = TickWorld->GetTimeSeconds() - State->StartTime;

				// ---- HOLD THE SCENARIO TOGETHER, EVERY TICK, INCLUDING THROUGH THE RESOLVE -----
				//
				// Both halves below are PRECONDITIONS of the scenario, not part of what is being
				// measured, and both have to be true at the RESOLVE rather than at the press — those
				// are SwingWindupSeconds apart, and the rule is decided at the far end of that gap.
				//
				// ORDER MATTERS: pin the Core first, force the pass second. IsShieldSuppressedFor is
				// `bPassActive && Carrier == victim`, so re-opening a pass on a victim who is no
				// longer the holder does nothing at all.
				//
				// WHY BOTH ARE NEEDED, as measured rather than as guessed. Two runs of the red arm
				// failed here and each printed its own reason:
				//   1. Re-opening the window once, immediately before the press, gave
				//      "shieldDown=1 swing=1 bladesOntoCarrier=0" — the pass had already closed again
				//      by the time the blade landed, and ResolveHitscan's own carrier skip came back.
				//   2. Re-opening it every tick gave "shieldDownAtPress=1 heldToResolve=0" with the
				//      victim's health down exactly 100. That is the tell: the forced pass COMPLETED
				//      and handed the Core to its target, so the victim was back-stabbed as an
				//      ORDINARY PAWN. The knife worked perfectly; the carrier rule was simply never
				//      reached, because by then there was no carrier to be immune.
				// Pinning the Core back on every tick closes both, and the pass window is re-forced
				// on top of a holder who is guaranteed to still be the holder.
				if (State->Phase >= 0)
				{
					if (ATraceCore* PinCore = ATraceCore::Get(TickWorld))
					{
						// This is the same unconditional debug grant Trace.DebugTakeCore uses.
						if (PinCore->Carrier != TickVictim && TickVictim->IsAlive() && !TickAttacker->IsCarrier())
						{
							PinCore->TryPickup(TickVictim);
						}

						// Only from the press onwards: case A is the shield-UP case and forcing a
						// pass during it would destroy the very thing that case is named for.
						if (State->Phase >= 2)
						{
							PinCore->DebugForcePassWindow();
						}
					}

					if (State->Phase >= 2)
					{
						// Sampled continuously, not once. "The shield was down when I pressed" is not
						// the precondition — "the shield was down for the whole flight of the blade"
						// is, and only the second one makes case B a test of anything.
						State->bShieldHeldThroughB =
							State->bShieldHeldThroughB && ATraceCore::IsShieldSuppressedFor(TickVictim);
					}
				}

				// ---- Is the staged situation still the staged situation? ----------------------
				//
				// Checked EVERY tick, before any measurement is taken. See FCarrierImmunityTest::
				// RetriesLeft: the match does not pause for the harness, and a run that measures a
				// scenario which quietly stopped being the scenario reports nonsense with full
				// confidence. A restart is cheap; a false "did not reproduce" is expensive.
				//
				// NOT IN PHASE 3. Once case B's blade is in the air the collapse of the scenario is
				// the RESULT, not an interruption: in the RED arm the back-stab kills the carrier
				// outright, so a check that restarted on "the victim died" would throw away the very
				// reproduction it is there to capture.
				if (State->Phase < 3)
				{
					const ATraceCore* LiveCore = ATraceCore::Get(TickWorld);
					const bool bStillStaged =
						LiveCore != nullptr && LiveCore->Carrier == TickVictim &&
						TickVictim->IsAlive() && TickAttacker->IsAlive() && !TickAttacker->IsCarrier();

					if (!bStillStaged)
					{
						if (State->RetriesLeft-- <= 0)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[KNIFECARRIER] arm=%d ABANDONED: the staged situation kept collapsing (out of retries). "
								     "victimAlive=%d victimIsCarrier=%d attackerAlive=%d attackerIsCarrier=%d"),
								State->Arm, TickVictim->IsAlive() ? 1 : 0,
								(LiveCore != nullptr && LiveCore->Carrier == TickVictim) ? 1 : 0,
								TickAttacker->IsAlive() ? 1 : 0, TickAttacker->IsCarrier() ? 1 : 0);
							CVarKnifeCarrierImmune->Set(1, ECVF_SetByConsole);
							CVarKnifeBotAuto->Set(1, ECVF_SetByConsole);   // the bots get their knife back
							return false;
						}

						UE_LOG(LogTraceGame, Display,
							TEXT("[KNIFECARRIER] arm=%d restarting in phase %d — the match moved on "
							     "(victimAlive=%d victimStillCarrier=%d attackerAlive=%d attackerNowCarrying=%d). %d retries left."),
							State->Arm, State->Phase, TickVictim->IsAlive() ? 1 : 0,
							(LiveCore != nullptr && LiveCore->Carrier == TickVictim) ? 1 : 0,
							TickAttacker->IsAlive() ? 1 : 0, TickAttacker->IsCarrier() ? 1 : 0,
							State->RetriesLeft);

						State->Phase = -1;
						State->AcquireDeadline = TickWorld->GetTimeSeconds() + 60.0;
						State->bSwungA = State->bSwungB = false;
						State->bShieldUpAtA = State->bShieldDownAtB = false;
						State->bShieldHeldThroughB = true;
						State->CaseACarrierHits = State->CaseBCarrierHits = 0;
						return true;
					}
				}

				// Stall guard. Phases 0 and 2 wait on EnsureKnifeReady, which can in principle never
				// come true — the attacker could be permanently dashing, or the bot could be winning
				// a swap fight with the harness. A test that hangs silently is worse than one that
				// fails, so it says which phase it died in.
				if (Elapsed > 20.0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[KNIFECARRIER] arm=%d ABORTED: stalled in phase %d for %.0fs (knife=%d, deploy %.2fs)."),
						State->Arm, State->Phase, Elapsed,
						TraceMelee::IsKnifeEquipped(TickAttacker) ? 1 : 0,
						TraceMelee::GetDeployRemaining(TickAttacker));
					CVarKnifeCarrierImmune->Set(1, ECVF_SetByConsole);
					CVarKnifeBotAuto->Set(1, ECVF_SetByConsole);   // the bots get their knife back
					return false;
				}

				// Held in place on every tick, not once at the start: the victim is a live pawn and
				// walks away otherwise, which would turn a staged back-stab into a miss and report
				// "no damage" for entirely the wrong reason.
				PlaceBehind(TickAttacker, TickVictim);

				const float VictimHealth = (TickVictim->Health != nullptr) ? TickVictim->Health->Health : -1.f;

				switch (State->Phase)
				{
				case 0:
					// Pullout (0.2 s) plus a margin, and the knife actually in hand — the bot AI may
					// have taken it back off him in the meantime. Waits rather than swinging early.
					if (Elapsed >= 0.30 && EnsureKnifeReady(TickAttacker))
					{
						State->HealthBeforeA = VictimHealth;
						State->CarrierHitsAtCaseA = TraceMelee::GetCarrierKnifeHitCount();
						State->bShieldUpAtA = !ATraceCore::IsShieldSuppressedFor(TickVictim);
						ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
						State->bSwungA = TraceMelee::RequestSwing(TickAttacker, &Refusal);
						UE_LOG(LogTraceGame, Display,
							TEXT("[KNIFECARRIER] CASE A (shield up): swing=%d (%s), victim health before %.0f, carrier-hit counter %d"),
							State->bSwungA ? 1 : 0, LexToString(Refusal), State->HealthBeforeA, State->CarrierHitsAtCaseA);
						State->SwingATime = Elapsed;
						State->Phase = 1;
					}
					break;

				case 1:
					// Past the wind-up: the blade has resolved if it was ever going to.
					if (Elapsed >= State->SwingATime + 0.25)
					{
						State->HealthAfterA = VictimHealth;
						State->CaseACarrierHits = TraceMelee::GetCarrierKnifeHitCount() - State->CarrierHitsAtCaseA;
						State->Phase = 2;
					}
					break;

				case 2:
					// The 0.5 s swing cooldown from the first press, then the case that matters. The
					// pass window is only 0.5 s long, so it is opened HERE, immediately before the
					// swing, rather than earlier where it would have closed again by now.
					if (Elapsed >= State->SwingATime + 0.60 && EnsureKnifeReady(TickAttacker))
					{
						ATraceCore* TickCore = ATraceCore::Get(TickWorld);
						ATraceCharacter* PassTargetPawn = (TickCore != nullptr) ? TickCore->DebugForcePassWindow() : nullptr;
						State->bShieldDownAtB = ATraceCore::IsShieldSuppressedFor(TickVictim);
						State->HealthBeforeB = VictimHealth;
						State->CarrierHitsAtCaseB = TraceMelee::GetCarrierKnifeHitCount();

						ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
						State->bSwungB = TraceMelee::RequestSwing(TickAttacker, &Refusal);
						UE_LOG(LogTraceGame, Display,
							TEXT("[KNIFECARRIER] CASE B (shield SUPPRESSED, mid-pass to %s): shieldDown=%d swing=%d (%s), victim health before %.0f, carrier-hit counter %d"),
							*GetNameSafe(PassTargetPawn), State->bShieldDownAtB ? 1 : 0,
							State->bSwungB ? 1 : 0, LexToString(Refusal), State->HealthBeforeB,
							State->CarrierHitsAtCaseB);
						State->SwingBTime = Elapsed;
						State->Phase = 3;
					}
					break;

				default:
				{
					if (Elapsed < State->SwingBTime + 0.25)
					{
						break;
					}
					State->HealthAfterB = VictimHealth;
					State->CaseBCarrierHits = TraceMelee::GetCarrierKnifeHitCount() - State->CarrierHitsAtCaseB;

					// Health is reported but NOT judged on — see FCarrierImmunityTest. In a live
					// match the carrier is being shot at by everyone, and the first version of this
					// test read one of those bullets as a knife wound and declared the rule broken.
					const float DamageA = State->HealthBeforeA - State->HealthAfterA;
					const float DamageB = State->HealthBeforeB - State->HealthAfterB;

					// A case only counts if the swing actually happened AND the shield was in the
					// state the case is named for. Otherwise "the blade did not land" is not
					// evidence of the rule, it is evidence that nothing was tested — the same
					// mistake that made a sibling harness report SKIPPED on a healthy build.
					const bool bValidA = State->bSwungA && State->bShieldUpAtA;
					const bool bValidB = State->bSwungB && State->bShieldDownAtB && State->bShieldHeldThroughB;
					const bool bPassA = bValidA && State->CaseACarrierHits == 0;
					const bool bPassB = bValidB && State->CaseBCarrierHits == 0;

					UE_LOG(LogTraceGame, Display,
						TEXT("[KNIFECARRIER] arm=%d CASE A shieldUp : swung=%d bladesOntoCarrier=%d (staged=%d)  [health moved %.0f, not judged on]"),
						State->Arm, State->bSwungA ? 1 : 0, State->CaseACarrierHits, bValidA ? 1 : 0, DamageA);
					UE_LOG(LogTraceGame, Display,
						TEXT("[KNIFECARRIER] arm=%d CASE B shieldDOWN: swung=%d bladesOntoCarrier=%d (staged=%d, shieldDownAtPress=%d heldToResolve=%d)  [health moved %.0f]  <-- the case only ResolveSwing protects"),
						State->Arm, State->bSwungB ? 1 : 0, State->CaseBCarrierHits, bValidB ? 1 : 0,
						State->bShieldDownAtB ? 1 : 0, State->bShieldHeldThroughB ? 1 : 0, DamageB);

					if (State->Arm == 0)
					{
						// RED. The expectation is INVERTED: the rule is gone, so case B must resolve
						// a blade onto the carrier. If it does not, the staging is broken and the
						// green arm below would be meaningless — say so loudly rather than sail on.
						State->bRedValid = bValidB;
						State->bRedCaseBDamaged = bValidB && (State->CaseBCarrierHits > 0);

						UE_LOG(LogTraceGame, Display,
							TEXT("[KNIFECARRIER] RED ARM: %s"),
							State->bRedCaseBDamaged
								? TEXT("REPRODUCED — with the rule removed a blade landed on the carrier, so this test can see the bug")
								: TEXT("*** DID NOT REPRODUCE — the staging is broken and the green arm below proves NOTHING ***"));

						// Restore the shipped RULE and go round again on a fresh carrier. The bot
						// auto-knife deliberately stays parked — the test is not over, and handing
						// it back now would let the bots take the attacker's knife away during the
						// green arm, which is the failure this whole guard exists to prevent.
						CVarKnifeCarrierImmune->Set(1, ECVF_SetByConsole);
						State->Arm = 1;
						State->Phase = -1;
						State->AcquireDeadline = TickWorld->GetTimeSeconds() + 60.0;
						State->bSwungA = State->bSwungB = false;
						State->bShieldUpAtA = State->bShieldDownAtB = false;
						State->bShieldHeldThroughB = true;
						State->CaseACarrierHits = State->CaseBCarrierHits = 0;
						State->RetriesLeft = 6;   // a fresh budget: the arms are independent attempts
						return true;
					}

					// GREEN. Both cases must be staged AND land nothing on the carrier.
					UE_LOG(LogTraceGame, Display,
						TEXT("[KNIFECARRIER] GREEN ARM: A %s | B %s"),
						bValidA ? (bPassA ? TEXT("PASS") : TEXT("*** FAIL ***")) : TEXT("INVALID (not staged)"),
						bValidB ? (bPassB ? TEXT("PASS") : TEXT("*** FAIL ***")) : TEXT("INVALID (not staged)"));
					UE_LOG(LogTraceGame, Display,
						TEXT("[KNIFECARRIER] ===== RESULT: %s  (red reproduced=%d, lifetime blades onto a carrier=%d) ====="),
						(bPassA && bPassB && State->bRedCaseBDamaged)
							? TEXT("CARRIER IMMUNE TO THE KNIFE, PROVEN — red went red, green is clean")
							: TEXT("*** NOT PROVEN ***"),
						State->bRedCaseBDamaged ? 1 : 0, TraceMelee::GetCarrierKnifeHitCount());

					// Belt and braces: never leave the arm off, whatever happened above.
					CVarKnifeCarrierImmune->Set(1, ECVF_SetByConsole);
					CVarKnifeBotAuto->Set(1, ECVF_SetByConsole);   // the bots get their knife back
					return false;
				}
				}

				return true;
			}), 0.f);
		}));

	FAutoConsoleCommand CmdAngleTest(
		TEXT("Trace.Knife.AngleTest"),
		TEXT("Dev only. Run the back/front angle self-test. 0 failures means the model matches its documented contract."),
		FConsoleCommandDelegate::CreateStatic([]() { TraceRunMeleeSelfTest(); }));

	/**
	 * The knife's answer to "verify it from a running game, never by reading the header".
	 *
	 * Trace.DumpSettings walks UTraceSettings and cannot see UTraceMeleeSettings, so without this
	 * the only way to know what the knife is ACTUALLY running with would be to read the initialisers
	 * — which is exactly the practice the project rule forbids. It prints the resolved value AND
	 * whether a console override is currently displacing the setting, because an override that
	 * somebody forgot to clear is indistinguishable from a retuned default otherwise.
	 */
	FAutoConsoleCommand CmdDumpSettings(
		TEXT("Trace.Knife.DumpSettings"),
		TEXT("Dev only. Log the melee tuning values the knife resolves RIGHT NOW, overrides included."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			const UTraceMeleeSettings& Table = UTraceMeleeSettings::Get();
			UE_LOG(LogTraceGame, Display, TEXT("========== TRACE KNIFE SETTINGS =========="));
			UE_LOG(LogTraceGame, Display, TEXT("KNIFE damage      : back %.0f (table %.0f) | front %.0f (table %.0f) | back-stab half-angle %.1fdeg (table %.1f)"),
				TraceMelee::GetBackstabDamage(), Table.BackstabDamage,
				TraceMelee::GetFrontDamage(), Table.FrontDamage,
				TraceMelee::GetBackstabHalfAngleDegrees(), Table.BackstabHalfAngleDegrees);
			UE_LOG(LogTraceGame, Display, TEXT("KNIFE timing      : swing every %.3fs (table %.3f) | pullout %.3fs (table %.3f) | windup %.3fs | anim %.3fs"),
				TraceMelee::GetSwingCooldownSeconds(), Table.SwingCooldownSeconds,
				TraceMelee::GetSwapSeconds(), Table.SwapSeconds,
				TraceMelee::GetSwingWindupSeconds(), TraceMelee::GetSwingAnimSeconds());
			UE_LOG(LogTraceGame, Display, TEXT("KNIFE reach       : %.0fuu over %.0fdeg in %d samples"),
				TraceMelee::GetSwingRangeUU(), TraceMelee::GetSwingArcDegrees(), TraceMelee::GetSwingSamples());
			// The movement numbers are read back out of UTraceSettings, where the movement component
			// owns them — printed here anyway, because a designer asking "what does the knife do"
			// should get one answer from one command even though two slices implement it.
			const UTraceSettings& Move = UTraceSettings::Get();

			// THE ASYMPTOTE SCALE IS PART OF THE BASE, and leaving it out made this command lie.
			//
			// Spec v9 §8 slid both air caps up by AirStrafeAsymptoteScale (x1.10), so the numbers the
			// movement component actually starts from are 1045 and 1375, not the 950 and 1250 on this
			// settings page. Printing the unscaled pair reported the knife's air ceiling as 1188/1688
			// when the running game was using 1306/1856 — a tool whose entire purpose is "read it from
			// a running game rather than from a header" quietly reporting the header's arithmetic.
			const float Asymptote = FMath::Clamp(Move.AirStrafeAsymptoteScale, 0.5f, 2.f);
			const float BaseSoft = Move.AirStrafeSoftCapSpeed * Asymptote;
			const float BaseHard = Move.AirStrafeHardCapSpeed * Asymptote;

			UE_LOG(LogTraceGame, Display, TEXT("KNIFE movement    : ground x%.3f (%.0f -> %.0f uu/s) | air soft x%.3f (%.0f -> %.0f) | air hard x%.3f (%.0f -> %.0f) | maxAir x%.3f (%.0f -> %.0f)   [base caps include AirStrafeAsymptoteScale x%.2f; owned by UTraceCharacterMovementComponent]"),
				Move.KnifeMoveSpeedMultiplier, Move.WalkSpeed, Move.WalkSpeed * Move.KnifeMoveSpeedMultiplier,
				Move.KnifeAirStrafeSoftCapMultiplier, BaseSoft, BaseSoft * Move.KnifeAirStrafeSoftCapMultiplier,
				Move.KnifeAirStrafeHardCapMultiplier, BaseHard, BaseHard * Move.KnifeAirStrafeHardCapMultiplier,
				Move.KnifeAirStrafeHardCapMultiplier, Move.MaxAirSpeed, Move.MaxAirSpeed * Move.KnifeAirStrafeHardCapMultiplier,
				Asymptote);
			UE_LOG(LogTraceGame, Display, TEXT("KNIFE bots        : auto %d | engage <= %.0fuu, disengage > %.0fuu, swing inside %.0f%% of reach"),
				TraceMelee::IsBotAutoKnifeEnabled() ? 1 : 0,
				TraceMelee::GetBotEngageRangeUU(), TraceMelee::GetBotDisengageRangeUU(),
				100.f * TraceMelee::GetBotSwingRangeFraction());
			UE_LOG(LogTraceGame, Display, TEXT("KNIFE carrier     : IMMUNE, always. The trace is the only way to kill a carrier (spec v10 s1, USER-CONFIRMED)."));
			UE_LOG(LogTraceGame, Display, TEXT("=========================================="));
		}));
}

#endif // !UE_BUILD_SHIPPING
