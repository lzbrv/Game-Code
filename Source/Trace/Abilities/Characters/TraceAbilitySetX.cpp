// Trace — X (spec v14 §6). Read TraceAbilitySetX.h first; it is the design document.

#include "Abilities/Characters/TraceAbilitySetX.h"

#include "Abilities/Characters/TraceAbilityWeaponHooks.h"
#include "Abilities/TraceAbilityComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/TraceCharacter.h"
#include "CoreGlobals.h"                    // GFrameCounter — the per-frame speed cache
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                    // TActorIterator
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"    // the match clock the bees are placed on
#include "GameFramework/PlayerState.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "UObject/ConstructorHelpers.h"

// =================================================================================================
// The orbit — one piece of arithmetic, two callers (the server's sting test and every machine's
// visual). See the header for why they may never be two pieces of arithmetic.
// =================================================================================================

namespace TraceXBees
{
	int32 GetBeeCount()
	{
		return FMath::Clamp(UTraceSettings::Get().XBeeCount, 1, 20);
	}

	float GetOrbitRadiusUU()
	{
		return FMath::Clamp(UTraceSettings::Get().XBeeOrbitRadiusUU, 20.f, 600.f);
	}

	float GetOrbitSpeedDegreesPerSecond()
	{
		return FMath::Clamp(UTraceSettings::Get().XBeeOrbitSpeedDegPerSecond, 0.f, 2000.f);
	}

	float GetBeeHitRadiusUU()
	{
		return FMath::Clamp(UTraceSettings::Get().XBeeHitRadiusUU, 5.f, 300.f);
	}

	int32 GetStingBulletCount()
	{
		return FMath::Clamp(UTraceSettings::Get().XStingBulletCount, 1, 20);
	}

	float GetSpeedBonusFraction()
	{
		return FMath::Clamp(UTraceSettings::Get().XVulnerableSpeedBonus, 0.f, 1.f);
	}

	FVector GetSwarmCentre(const ATraceCharacter* Character)
	{
		if (Character == nullptr)
		{
			return FVector::ZeroVector;
		}

		// Chest height, not the actor origin (which is at the feet). The bees have to sweep the
		// volume a body actually occupies, or "X's body is the delivery mechanism" would mean "X's
		// ankles are the delivery mechanism".
		FVector Centre = Character->GetActorLocation();
		if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
		{
			Centre.Z += Capsule->GetScaledCapsuleHalfHeight() * 0.35f;
		}
		return Centre;
	}

	FVector GetBeeLocation(const FVector& Centre, float TimeSeconds, int32 Index, int32 Count,
	                       float RadiusUU, float SpeedDegreesPerSecond)
	{
		const int32 SafeCount = FMath::Max(1, Count);
		const float Spacing = 360.f / static_cast<float>(SafeCount);

		// Evenly spaced around one ring, all turning together. The per-bee vertical offset is a
		// deterministic function of the index, so the ring is a shallow helix rather than a flat
		// disc — five bees in one plane read as a hoop, not as a swarm.
		const float AngleDegrees = SpeedDegreesPerSecond * TimeSeconds + Spacing * static_cast<float>(Index);
		const float AngleRadians = FMath::DegreesToRadians(AngleDegrees);

		const float Bob = FMath::Sin(AngleRadians * 2.f + static_cast<float>(Index) * 1.7f) * (RadiusUU * 0.28f);

		return Centre + FVector(FMath::Cos(AngleRadians) * RadiusUU,
		                        FMath::Sin(AngleRadians) * RadiusUU,
		                        Bob);
	}
}

// =================================================================================================
// UTraceAbilitySetX
// =================================================================================================

int32 UTraceAbilitySetX::GetLoadedBees() const
{
	return static_cast<int32>(State().Stacks);
}

bool UTraceAbilitySetX::IsStingLoaded() const
{
	return GetLoadedBees() > 0;
}

float UTraceAbilitySetX::GetActivatedCooldownSeconds() const
{
	// Spec §6: "25s cooldown" — X is the ONLY character in §6 whose activated cooldown is not 20.
	// Read from the settings at the point of use so a designer can retune it live.
	return FMath::Clamp(UTraceSettings::Get().XStingCooldownSeconds, 0.f, 180.f);
}

bool UTraceAbilitySetX::CanActivate(FText& OutReason) const
{
	if (IsStingLoaded())
	{
		OutReason = NSLOCTEXT("Trace", "XStingAlreadyLoaded", "BEES ALREADY LOADED");
		return false;
	}
	return true;
}

bool UTraceAbilitySetX::ActivateAbility()
{
	// STING. "Loads the 5 bees into his gun and they stop orbiting."
	//
	// Runs on the server AND on the owning client (prediction). On a client MutableState() returns a
	// scratch object, so the client's swarm keeps orbiting for one round trip and then snaps — which
	// is the correct trade: mis-predicting a cosmetic swarm costs nothing, mis-predicting the LOADED
	// COUNT would let a client believe it had a sixth bee.
	if (HasAuthority())
	{
		FTraceAbilityNetState& MutableNetState = MutableState();
		MutableNetState.Stacks = static_cast<uint8>(TraceXBees::GetStingBulletCount());
		MutableNetState.Flags |= TraceAbilityFlags::AuxActive;
		MarkStateDirty();

		UE_LOG(LogTraceGame, Log, TEXT("[X] STING: %d bees loaded into the gun (cooldown %.0fs)."),
			static_cast<int32>(MutableNetState.Stacks), GetActivatedCooldownSeconds());
	}

	UpdateSwarmActor();
	return true;
}

// -------------------------------------------------------------------------------------------------
// THE MARK — the only place in this file that touches anybody
// -------------------------------------------------------------------------------------------------

bool UTraceAbilitySetX::MarkVulnerable(ATraceCharacter* Target) const
{
	if (!HasAuthority() || Target == nullptr)
	{
		return false;
	}

	// *** THE CHOKE POINT. SPEC §4. RULE 1 OF THE FRAMEWORK. ***
	//
	// ETraceAbilityEffect::Control, not Damage: the mark takes no health, it is a debuff. Control is
	// the class the framework refuses on a carrier under the §4 [ASSUMPTION] — and §4 names X's
	// vulnerable by name as something that "must not become a damage path". It also handles dead
	// targets, self, and teammates while friendly fire is off, so nothing below has to.
	if (!CanAffect(Target, ETraceAbilityEffect::Control))
	{
		return false;
	}

	UTraceHealthComponent* TargetHealth = Target->Health;
	if (TargetHealth == nullptr)
	{
		return false;
	}

	AController* Credited = nullptr;
	if (const UTraceAbilityComponent* Comp = GetAbilityComponent())
	{
		if (const APlayerState* MyState = Comp->GetOwningPlayerState())
		{
			Credited = MyState->GetOwningController();
		}
	}

	// The victim's HEALTH COMPONENT owns the mark, not X and not the victim's ability set — the
	// victim may be a characterless bot (spec §3) and would have no ability set to hold it.
	return TargetHealth->ApplyVulnerable(TraceVulnerable::GetDurationSeconds(), Credited);
}

void UTraceAbilitySetX::NotifyBulletHit(ATraceCharacter* Victim, bool bHeadshot)
{
	if (!HasAuthority() || Victim == nullptr || !IsStingLoaded())
	{
		return;
	}

	if (!MarkVulnerable(Victim))
	{
		// The bee is NOT spent. See the header's [ASSUMPTION]: a bullet the choke point refused (a
		// Core carrier, a teammate) does not cost X one of his five.
		return;
	}

	FTraceAbilityNetState& MutableNetState = MutableState();
	MutableNetState.Stacks = static_cast<uint8>(FMath::Max(0, static_cast<int32>(MutableNetState.Stacks) - 1));
	if (MutableNetState.Stacks == 0)
	{
		// "When all five are fired the bees resume orbiting."
		MutableNetState.Flags &= static_cast<uint8>(~TraceAbilityFlags::AuxActive);
	}
	MarkStateDirty();

	UE_LOG(LogTraceGame, Log, TEXT("[X] Sting bullet marked %s%s — %d bee(s) left%s."),
		*GetNameSafe(Victim), bHeadshot ? TEXT(" (headshot)") : TEXT(""),
		static_cast<int32>(MutableNetState.Stacks),
		(MutableNetState.Stacks == 0) ? TEXT(", the swarm resumes orbiting") : TEXT(""));

	UpdateSwarmActor();
}

// -------------------------------------------------------------------------------------------------
// THE PASSIVE — the bees sting on contact
// -------------------------------------------------------------------------------------------------

void UTraceAbilitySetX::SweepBeeContacts()
{
	ATraceCharacter* MyCharacter = GetCharacter();
	UWorld* CurrentWorld = GetWorld();
	if (MyCharacter == nullptr || CurrentWorld == nullptr || !MyCharacter->IsAlive())
	{
		return;
	}

	// "They stop orbiting" — a loaded X is not also stinging with his body. Without this, Sting would
	// be strictly worse than the passive at close range, which is the opposite of an upgrade.
	if (IsStingLoaded())
	{
		return;
	}

	const float Now = MatchTimeNow();
	const int32 BeeCount = TraceXBees::GetBeeCount();
	const float OrbitRadius = TraceXBees::GetOrbitRadiusUU();
	const float OrbitSpeed = TraceXBees::GetOrbitSpeedDegreesPerSecond();
	const float HitRadius = TraceXBees::GetBeeHitRadiusUU();
	const FVector Centre = TraceXBees::GetSwarmCentre(MyCharacter);

	// Broad phase: nothing outside the swarm's own envelope plus the fattest capsule can be touched.
	const float BroadRadius = OrbitRadius + HitRadius + 200.f;
	const float BroadRadiusSq = BroadRadius * BroadRadius;

	// Refresh threshold. A target standing inside the swarm would otherwise be re-marked every single
	// tick — harmless (the deadline is a write, so it cannot stack) but it would bury the log and
	// move TraceVulnerable::GetMarkAppliedCount() at 60 Hz, which would make that counter useless as
	// evidence. Re-mark once the mark has decayed past 10%.
	const float Duration = TraceVulnerable::GetDurationSeconds();
	const float RefreshBelow = Duration * 0.9f;

	for (TActorIterator<ATraceCharacter> It(CurrentWorld); It; ++It)
	{
		ATraceCharacter* Other = *It;
		if (Other == nullptr || Other == MyCharacter || !Other->IsAlive())
		{
			continue;
		}

		if (FVector::DistSquared(Centre, Other->GetActorLocation()) > BroadRadiusSq)
		{
			continue;
		}

		// Ask the choke point BEFORE doing any narrow-phase work. It is the cheap test and it is the
		// one that matters: a carrier, a teammate or a dead player is not a candidate at all.
		if (!CanAffect(Other, ETraceAbilityEffect::Control))
		{
			continue;
		}

		if (Other->Health == nullptr)
		{
			continue;
		}
		if (Other->Health->GetVulnerableRemaining() > RefreshBelow)
		{
			continue;   // already freshly marked; nothing to add
		}

		// Narrow phase: the victim's capsule as a segment, each bee as a sphere. This is what makes
		// XBeeOrbitSpeedDegPerSecond a gameplay knob rather than a purely cosmetic one — it decides
		// which bee is where at the instant a body arrives.
		float CapsuleRadius = 34.f;
		float CapsuleHalfHeight = 88.f;
		if (const UCapsuleComponent* OtherCapsule = Other->GetCapsuleComponent())
		{
			CapsuleRadius = OtherCapsule->GetScaledCapsuleRadius();
			CapsuleHalfHeight = OtherCapsule->GetScaledCapsuleHalfHeight();
		}

		const FVector OtherCentre = Other->GetActorLocation();
		const FVector SegmentTop = OtherCentre + FVector(0.f, 0.f, FMath::Max(0.f, CapsuleHalfHeight - CapsuleRadius));
		const FVector SegmentBottom = OtherCentre - FVector(0.f, 0.f, FMath::Max(0.f, CapsuleHalfHeight - CapsuleRadius));
		const float TouchDistance = HitRadius + CapsuleRadius;
		const float TouchDistanceSq = TouchDistance * TouchDistance;

		for (int32 BeeIndex = 0; BeeIndex < BeeCount; ++BeeIndex)
		{
			const FVector BeeLocation = TraceXBees::GetBeeLocation(Centre, Now, BeeIndex, BeeCount, OrbitRadius, OrbitSpeed);
			const FVector Closest = FMath::ClosestPointOnSegment(BeeLocation, SegmentBottom, SegmentTop);

			if (FVector::DistSquared(BeeLocation, Closest) <= TouchDistanceSq)
			{
				if (MarkVulnerable(Other))
				{
					UE_LOG(LogTraceGame, Verbose, TEXT("[X] bee %d stung %s (%.1fuu)."),
						BeeIndex, *GetNameSafe(Other), FVector::Dist(BeeLocation, Closest));
				}
				break;   // one sting per target per tick; the mark does not stack
			}
		}
	}
}

// -------------------------------------------------------------------------------------------------
// THE MOVEMENT PASSIVE — "+10% speed while ANY enemy is vulnerable"
// -------------------------------------------------------------------------------------------------

bool UTraceAbilitySetX::IsAnyEnemyVulnerable() const
{
	const UWorld* CurrentWorld = GetWorld();
	if (CurrentWorld == nullptr)
	{
		return false;
	}

	ETraceTeam MyTeam = ETraceTeam::None;
	if (const UTraceAbilityComponent* Comp = GetAbilityComponent())
	{
		MyTeam = Comp->GetTeam();
	}

	const ATraceCharacter* MyCharacter = GetCharacter();

	for (TActorIterator<ATraceCharacter> It(const_cast<UWorld*>(CurrentWorld)); It; ++It)
	{
		const ATraceCharacter* Other = *It;
		if (Other == nullptr || Other == MyCharacter || !Other->IsAlive() || Other->Health == nullptr)
		{
			continue;
		}

		// "ANY ENEMY", so a marked teammate does not count. MyTeam == None (before the team has
		// replicated, and in a fixture) treats everyone as an enemy rather than nobody: the passive
		// erring ON for a frame is a 10% speed blip, erring OFF is a dead ability during warm-up.
		if (MyTeam != ETraceTeam::None && Other->GetTeam() == MyTeam)
		{
			continue;
		}

		if (Other->Health->IsVulnerable())
		{
			return true;
		}
	}

	return false;
}

float UTraceAbilitySetX::GetMoveSpeedMultiplier() const
{
	// Called from the movement tick on every machine, so it is cached for the frame. The underlying
	// query walks the pawn list; at ten pawns that is nothing, but "cheap and pure" is what the
	// framework's contract for this hook asks for and a frame cache is how it stays true if the
	// movement component ever asks more than once per frame.
	if (CachedSpeedFrame == GFrameCounter)
	{
		return CachedSpeedMultiplier;
	}

	CachedSpeedFrame = GFrameCounter;
	CachedSpeedMultiplier = IsAnyEnemyVulnerable() ? (1.f + TraceXBees::GetSpeedBonusFraction()) : 1.f;
	return CachedSpeedMultiplier;
}

// -------------------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------------------

void UTraceAbilitySetX::OnEquipped()
{
	UpdateSwarmActor();
}

void UTraceAbilitySetX::OnUnequipped()
{
	if (ATraceBeeSwarm* Existing = Swarm.Get())
	{
		Existing->Destroy();
	}
	Swarm = nullptr;
}

void UTraceAbilitySetX::OnPawnSpawned()
{
	UpdateSwarmActor();
}

void UTraceAbilitySetX::OnPawnDied()
{
	// The swarm dies with him. The COOLDOWN does not — spec §5 is explicit that it keeps counting
	// through death, and the framework owns that; nothing here touches it.
	if (ATraceBeeSwarm* Existing = Swarm.Get())
	{
		Existing->Destroy();
	}
	Swarm = nullptr;
}

void UTraceAbilitySetX::OnHalfTime()
{
	// The framework has already cleared the cooldown and Reset() the net state, so the bees are
	// unloaded by the time this runs. What is left is world state X put on OTHER players: any live
	// marks. Half time is a reset of the whole match state, and a player walking out of the tunnel
	// still taking +25% would be a 2 s mystery nobody could explain.
	if (HasAuthority())
	{
		if (UWorld* CurrentWorld = GetWorld())
		{
			for (TActorIterator<ATraceCharacter> It(CurrentWorld); It; ++It)
			{
				if (*It != nullptr && It->Health != nullptr)
				{
					It->Health->ClearVulnerable();
				}
			}
		}
	}

	UpdateSwarmActor();
}

void UTraceAbilitySetX::TickAbilities(float DeltaSeconds)
{
	if (HasAuthority())
	{
		SweepBeeContacts();
	}

	// Runs on every machine: the swarm has to appear on a simulated proxy too, and Stacks arriving by
	// replication is what tells a remote client that the bees went into the gun.
	UpdateSwarmActor();
}

// -------------------------------------------------------------------------------------------------
// The cosmetic swarm
// -------------------------------------------------------------------------------------------------

void UTraceAbilitySetX::UpdateSwarmActor()
{
	UWorld* CurrentWorld = GetWorld();
	if (CurrentWorld == nullptr || !CurrentWorld->IsGameWorld() || CurrentWorld->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ATraceCharacter* MyCharacter = GetCharacter();
	ATraceBeeSwarm* Existing = Swarm.Get();

	if (MyCharacter == nullptr || !MyCharacter->IsAlive())
	{
		if (Existing != nullptr)
		{
			Existing->Destroy();
			Swarm = nullptr;
		}
		return;
	}

	if (Existing == nullptr)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = MyCharacter;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transient;

		Existing = CurrentWorld->SpawnActor<ATraceBeeSwarm>(ATraceBeeSwarm::StaticClass(),
			MyCharacter->GetActorLocation(), FRotator::ZeroRotator, SpawnParams);
		if (Existing == nullptr)
		{
			return;
		}
		Existing->Host = MyCharacter;
		Swarm = Existing;
	}

	Existing->SetSwarmVisible(!IsStingLoaded());
}

// =================================================================================================
// ATraceBeeSwarm
// =================================================================================================

ATraceBeeSwarm::ATraceBeeSwarm()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = false;
	SetReplicateMovement(false);

	USceneComponent* SwarmRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SwarmRoot);
	SwarmRoot->SetMobility(EComponentMobility::Movable);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		BeeMesh = SphereFinder.Object;
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

void ATraceBeeSwarm::EnsureBeeComponents(int32 DesiredCount)
{
	if (Bees.Num() == DesiredCount)
	{
		return;
	}

	// Shrinking: destroy the surplus. Growing: build the difference. Both happen only when
	// UTraceSettings::XBeeCount is retuned live, which is exactly the live-editing this project
	// expects of every knob.
	while (Bees.Num() > DesiredCount)
	{
		if (UStaticMeshComponent* Surplus = Bees.Pop())
		{
			Surplus->DestroyComponent();
		}
		if (BeeMIDs.Num() > Bees.Num())
		{
			BeeMIDs.Pop();
		}
	}

	while (Bees.Num() < DesiredCount)
	{
		UStaticMeshComponent* Bee = NewObject<UStaticMeshComponent>(this);
		if (Bee == nullptr)
		{
			return;
		}
		Bee->SetupAttachment(GetRootComponent());
		Bee->RegisterComponent();

		// NO COLLISION. These orbit at chest height through the space bullets travel.
		Bee->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Bee->SetCollisionProfileName(TEXT("NoCollision"));
		Bee->SetGenerateOverlapEvents(false);
		Bee->SetCanEverAffectNavigation(false);
		Bee->SetCastShadow(false);
		Bee->bReceivesDecals = false;
		Bee->SetRelativeScale3D(FVector(0.16f));
		Bee->SetUsingAbsoluteLocation(true);   // placed in world space by Tick, one call per bee

		if (BeeMesh != nullptr)
		{
			Bee->SetStaticMesh(BeeMesh);
		}

		UMaterialInstanceDynamic* BeeMID = nullptr;
		if (BaseMaterial != nullptr)
		{
			BeeMID = Bee->CreateDynamicMaterialInstance(0, BaseMaterial);
			if (BeeMID != nullptr)
			{
				// Amber, the same family the ability HUD uses for X.
				const FLinearColor Amber(1.f, 0.72f, 0.12f, 1.f);
				BeeMID->SetVectorParameterValue(TEXT("Color"), Amber);
				BeeMID->SetVectorParameterValue(TEXT("BaseColor"), Amber);
			}
		}

		Bees.Add(Bee);
		BeeMIDs.Add(BeeMID);

		Bee->SetVisibility(bVisible, true);
	}
}

void ATraceBeeSwarm::SetSwarmVisible(bool bInVisible)
{
	if (bVisible == bInVisible)
	{
		return;
	}
	bVisible = bInVisible;

	for (UStaticMeshComponent* Bee : Bees)
	{
		if (Bee != nullptr)
		{
			Bee->SetVisibility(bVisible, true);
		}
	}
}

void ATraceBeeSwarm::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const ATraceCharacter* HostCharacter = Host.Get();
	if (HostCharacter == nullptr || !HostCharacter->IsAlive())
	{
		Destroy();
		return;
	}

	const int32 BeeCount = TraceXBees::GetBeeCount();
	EnsureBeeComponents(BeeCount);

	if (!bVisible)
	{
		return;   // in the gun; nothing to place
	}

	// THE MATCH CLOCK, not the local world clock. It is the one clock the server and every client
	// agree on, so the bee a player sees is the bee the server's contact test used.
	float Now = 0.f;
	if (const UWorld* CurrentWorld = GetWorld())
	{
		if (const AGameStateBase* StateBase = CurrentWorld->GetGameState())
		{
			Now = static_cast<float>(StateBase->GetServerWorldTimeSeconds());
		}
		else
		{
			Now = CurrentWorld->GetTimeSeconds();
		}
	}

	const FVector Centre = TraceXBees::GetSwarmCentre(HostCharacter);
	const float OrbitRadius = TraceXBees::GetOrbitRadiusUU();
	const float OrbitSpeed = TraceXBees::GetOrbitSpeedDegreesPerSecond();

	SetActorLocation(Centre);

	for (int32 BeeIndex = 0; BeeIndex < Bees.Num(); ++BeeIndex)
	{
		if (Bees[BeeIndex] == nullptr)
		{
			continue;
		}
		Bees[BeeIndex]->SetWorldLocation(
			TraceXBees::GetBeeLocation(Centre, Now, BeeIndex, BeeCount, OrbitRadius, OrbitSpeed));
	}
}

// =================================================================================================
// The weapon seam (see TraceAbilityWeaponHooks.h for why it is a seam and not a cast in the gun)
// =================================================================================================

namespace TraceAbilityWeaponHooks
{
	void OnBulletHit(ATraceCharacter* Shooter, ATraceCharacter* Victim, bool bHeadshot)
	{
		if (Shooter == nullptr || Victim == nullptr)
		{
			return;
		}

		if (UTraceAbilitySetX* XSet = Cast<UTraceAbilitySetX>(UTraceAbilityComponent::GetAbilitySetFor(Shooter)))
		{
			XSet->NotifyBulletHit(Victim, bHeadshot);
		}
	}
}
