// Trace — X (spec v14 §6). Read TraceAbilitySetX.h first; it is the design document.

#include "Abilities/Characters/TraceAbilitySetX.h"

#include "Abilities/Characters/TraceAbilityWeaponHooks.h"
#include "Abilities/TraceAbilityComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"              // FTSTicker — the v16 §1 Sting-clip self-test
#include "Core/TraceCharacter.h"
#include "CoreGlobals.h"                    // GFrameCounter — the per-frame speed cache
#include "Engine/Engine.h"                  // GEngine->GetWorldContexts, for the self-test
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                    // TActorIterator
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "GameFramework/GameStateBase.h"    // the match clock the bees are placed on
#include "GameFramework/PlayerState.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceWeaponComponent.h"   // spec v16 §1: Sting replaces the CLIP now
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
	// STING. v14 §6: "Loads the 5 bees into his gun and they stop orbiting."
	// v16 §1: "X's Sting ability RELOADS THE CLIP WITH JUST THE 5 BEE BULLETS; after shooting those,
	// his next reload is normal."
	//
	// Runs on the server AND on the owning client (prediction). On a client MutableState() returns a
	// scratch object, so the client's swarm keeps orbiting for one round trip and then snaps — which
	// is the correct trade: mis-predicting a cosmetic swarm costs nothing, mis-predicting the LOADED
	// COUNT would let a client believe it had a sixth bee.
	if (HasAuthority())
	{
		const int32 BulletCount = TraceXBees::GetStingBulletCount();

		FTraceAbilityNetState& MutableNetState = MutableState();
		MutableNetState.Stacks = static_cast<uint8>(BulletCount);
		MutableNetState.Flags |= TraceAbilityFlags::AuxActive;
		MarkStateDirty();

		// *** SPEC v16 §1: STING IS A CLIP NOW, NOT A TAG ON THE NEXT FIVE SHOTS. ***
		//
		// This is the change: the gun's clip is REPLACED with five ability rounds, so twenty ordinary
		// rounds become five bee rounds and the count on the HUD goes DOWN. When those five are gone
		// the clip is empty and the gun auto-reloads to a normal thirty, which is the whole of "after
		// shooting those, his next reload is normal" — nothing here has to remember to undo anything.
		//
		// X DRIVES THE GUN, NOT THE OTHER WAY ROUND, and that direction is what keeps
		// UTraceWeaponComponent character-agnostic: it is told "load N ability rounds" and never
		// learns what a bee is. See Abilities/Characters/TraceAbilityWeaponHooks.h for the same
		// argument about the bullet-hit seam.
		if (ATraceCharacter* MyCharacter = GetCharacter())
		{
			if (UTraceWeaponComponent* MyWeapon = MyCharacter->Weapon)
			{
				MyWeapon->LoadAbilityClip(BulletCount);
			}
		}

		UE_LOG(LogTraceGame, Log, TEXT("[X] STING: the clip is now %d bee round(s) (cooldown %.0fs)."),
			BulletCount, GetActivatedCooldownSeconds());
	}

	UpdateSwarmActor();
	return true;
}

void UTraceAbilitySetX::SyncStingToClip()
{
	// *** SPEC v16 §1 MADE THE CLIP THE SOURCE OF TRUTH, AND THIS IS THE ONE-WAY MIRROR. ***
	//
	// Before v16, Stacks WAS the count of bee bullets and NotifyBulletHit spent one per landed mark.
	// Now the rounds leave the gun whether or not they hit anything, so the weapon's ability-round
	// count is the only honest answer to "how many bees are left" and Stacks is a display of it —
	// which is why this only ever pulls Stacks DOWN, never up. Two writers that could each raise the
	// count is the four-writer bug this codebase already carries once (ATracePlayerState::bIsCarrier)
	// and is not going to carry twice.
	//
	// It also closes the case the old accounting could not: five bee rounds fired and only three of
	// them connect. Stacks would have stopped at 2 with an empty clip, and the two ORDINARY rounds
	// after the reload would have carried marks. Clamping to the clip makes the swarm resume exactly
	// when the bee rounds are gone, hit or miss.
	if (!HasAuthority())
	{
		// Clients read Stacks by replication. A local write would be overwritten and would make a
		// client briefly believe in a bee count the server never granted.
		return;
	}

	const ATraceCharacter* MyCharacter = GetCharacter();
	const UTraceWeaponComponent* MyWeapon = (MyCharacter != nullptr) ? MyCharacter->Weapon : nullptr;
	if (MyWeapon == nullptr)
	{
		// No gun to mirror (a pawn mid-teardown, a fixture). Leave Stacks alone rather than zeroing
		// it: "I cannot see the clip" is not the same statement as "the clip is empty", and the
		// second would silently disarm Sting.
		return;
	}

	const int32 RoundsLeft = MyWeapon->GetAbilityRoundsInClip();
	FTraceAbilityNetState& MutableNetState = MutableState();
	const int32 Mirrored = FMath::Min(static_cast<int32>(MutableNetState.Stacks), RoundsLeft);
	if (Mirrored == static_cast<int32>(MutableNetState.Stacks))
	{
		return;   // already agrees; do not dirty the state every frame
	}

	MutableNetState.Stacks = static_cast<uint8>(FMath::Max(0, Mirrored));
	if (MutableNetState.Stacks == 0)
	{
		// "When all five are fired the bees resume orbiting."
		MutableNetState.Flags &= static_cast<uint8>(~TraceAbilityFlags::AuxActive);
	}
	MarkStateDirty();
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
	// victim may hold no character at all (mode A, the characters toggle, an unservable roster) and
	// would then have no ability set to hold it. This used to say "a characterless bot (spec §3)",
	// which spec v15 §2 made false; the reason is unchanged, the example was.
	return TargetHealth->ApplyVulnerable(TraceVulnerable::GetDurationSeconds(), Credited);
}

void UTraceAbilitySetX::NotifyBulletHit(ATraceCharacter* Victim, bool bHeadshot)
{
	if (!HasAuthority() || Victim == nullptr || !IsStingLoaded())
	{
		return;
	}

	// *** WAS THE ROUND THAT JUST LANDED ACTUALLY A BEE ROUND? ASK THE CLIP. ***
	//
	// Stacks is a mirror of the clip (SyncStingToClip) and the mirror is refreshed once per tick, so
	// on its own it can be up to one frame stale — and the frame that matters is the one where the
	// last bee round leaves and an ordinary clip arrives. The two-term test below is exact:
	//
	//   GetAbilityRoundsInClip() > 0     there are bee rounds still in the gun; this was one.
	//   WasLastRoundAbilityRound()       the clip is now empty of them BECAUSE this shot took the
	//                                    last one. Without this term the FIFTH bee would mark nobody,
	//                                    since ConsumeRound has already taken it out of the count by
	//                                    the time ServerFire reaches this hook.
	//
	// An ordinary round fired after the reload satisfies neither and is refused, which is the whole of
	// "after shooting those, his next reload is normal".
	if (const ATraceCharacter* MyCharacter = GetCharacter())
	{
		if (const UTraceWeaponComponent* MyWeapon = MyCharacter->Weapon)
		{
			if (MyWeapon->GetAbilityRoundsInClip() <= 0 && !MyWeapon->WasLastRoundAbilityRound())
			{
				SyncStingToClip();
				return;
			}
		}
	}

	if (!MarkVulnerable(Victim))
	{
		// The mark was refused (a Core carrier, a teammate). *** THE ROUND IS STILL GONE. *** Under
		// v14 §6 this function spent a bee per LANDED mark and the header's [ASSUMPTION] was that a
		// refused bullet cost X nothing; v16 §1 makes Sting a CLIP, and a round that has left the clip
		// has left it whether it found anybody or not. That forgiveness is gone by necessity rather
		// than by choice, and SyncStingToClip is what makes the displayed count say so.
		SyncStingToClip();
		return;
	}

	// The v14-era per-hit decrement, kept, and it is REDUNDANT-BUT-NOT-HARMFUL in the shipping path.
	// Worth the two lines of proof rather than a shrug:
	//
	//   Stacks falls once per LANDED mark; the clip's ability-round count falls once per FIRED round;
	//   landed <= fired, so Stacks is always >= the clip's count, so the FMath::Min inside
	//   SyncStingToClip below always picks the clip. The clip therefore has the final word on every
	//   real shot and this line cannot change any outcome.
	//
	// It is kept because the ability layer is also driven directly, without a gun, by the fixtures in
	// Abilities/Characters/TraceXVerify.cpp (TraceAbilityWeaponHooks::OnBulletHit called five times in
	// a row). There the clip never moves, and this is the only thing that counts the bees down.
	FTraceAbilityNetState& MutableNetState = MutableState();
	MutableNetState.Stacks = static_cast<uint8>(FMath::Max(0, static_cast<int32>(MutableNetState.Stacks) - 1));
	if (MutableNetState.Stacks == 0)
	{
		// "When all five are fired the bees resume orbiting."
		MutableNetState.Flags &= static_cast<uint8>(~TraceAbilityFlags::AuxActive);
	}
	MarkStateDirty();

	// ...and then let the clip correct it, which is what closes the missed-shot case: five rounds
	// fired with three hits leaves Stacks at 2 by the line above and at 0 by this one.
	SyncStingToClip();

	UE_LOG(LogTraceGame, Log, TEXT("[X] Sting bullet marked %s%s — %d bee round(s) left%s."),
		*GetNameSafe(Victim), bHeadshot ? TEXT(" (headshot)") : TEXT(""),
		GetLoadedBees(),
		(GetLoadedBees() == 0) ? TEXT(", the swarm resumes orbiting") : TEXT(""));

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
		// SPEC v16 §1. BEFORE the sweep, deliberately: SweepBeeContacts refuses to run while Sting is
		// loaded ("they stop orbiting"), so the frame the last bee round leaves the clip is the frame
		// the body sting must come back — not the frame after.
		SyncStingToClip();

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

// =================================================================================================
// Trace.X.StingClipTest — SPEC v16 §1's Sting clause, end to end
//
// Verbatim: "X's Sting ability reloads the clip with just the 5 bee bullets; after shooting those,
// his next reload is normal."
//
// WHY THIS IS A SEPARATE COMMAND FROM Trace.X.StingTest (Abilities/Characters/TraceXVerify.cpp).
// That one drives the ability layer DIRECTLY — TraceAbilityWeaponHooks::OnBulletHit called five
// times with no gun anywhere — which is the right way to test "five bullets apply the mark" and is
// completely blind to the thing v16 §1 changed, because no clip ever moves in it. This one is about
// the CLIP: that Sting replaces it (and the count therefore goes DOWN), that the bee rounds are
// spent by FIRING rather than by hitting, and that the reload after them is an ordinary thirty.
//
// It deliberately does NOT test the mark. The mark is TraceXVerify's, and duplicating it here would
// give two harnesses that both fail when one thing breaks and neither of which says which thing.
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace TraceXStingClipTest
{
	struct FChecklist
	{
		int32 Passed = 0;
		int32 Failed = 0;
		bool  bInvalid = false;
		FString InvalidReason;

		void Check(bool bCondition, const FString& Name, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[XSTINGCLIP] %s  %s  |  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *Name, *Detail);
		}

		void Invalidate(const FString& Reason) { bInvalid = true; InvalidReason = Reason; }

		void Report()
		{
			if (bInvalid)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[XSTINGCLIP] VERDICT: INVALID — %s (%d passed, %d failed)"),
					*InvalidReason, Passed, Failed);
			}
			else if (Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[XSTINGCLIP] VERDICT: PASS — %d checks, 0 failed."), Passed);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[XSTINGCLIP] VERDICT: *** FAIL *** — %d passed, %d FAILED."),
					Passed, Failed);
			}
		}
	};

	void SetArm(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

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

	/**
	 * Makes the first HUMAN player X and returns their set. A human, and it must be: handing a
	 * character to a BOT would fight ATraceGameMode's 4 Hz character fill for ownership of that
	 * player state, and the run would measure whichever won. Same reasoning TraceXVerify gives.
	 */
	UTraceAbilitySetX* MakePlayerIntoX(UWorld* World, FString& OutWhy)
	{
		if (World == nullptr)
		{
			OutWhy = TEXT("no world");
			return nullptr;
		}
		if (!UTraceAbilityComponent::AreCharactersEnabled(World))
		{
			OutWhy = TEXT("characters are DISABLED in this match (mode A, or the §3 toggle) — run this in mode B");
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = Cast<APlayerController>(It->Get());
			if (PC == nullptr || PC->GetPawn() == nullptr)
			{
				continue;
			}
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PC->GetPawn());
			if (Comp == nullptr || Comp->IsBot())
			{
				continue;
			}
			if (Comp->GetCharacterId() != ETraceCharacterId::X)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::X);
			}
			if (UTraceAbilitySetX* XSet = Comp->GetAbilitySetAs<UTraceAbilitySetX>())
			{
				return XSet;
			}
			OutWhy = TEXT("ServerSetCharacter(X) did not produce a UTraceAbilitySetX");
			return nullptr;
		}

		OutWhy = TEXT("no human player controller with a pawn");
		return nullptr;
	}

	struct FStingClipState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;
		int32 ClipBeforeSting = 0;
		bool bRedReproduced = false;
		bool bAllFiredRoundsWereBeeRounds = true;
	};

	void Run()
	{
		UWorld* World = FindAuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XSTINGCLIP] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FStingClipState> State = MakeShared<FStingClipState>();
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[XSTINGCLIP] ===== spec v16 §1: 'X's Sting ability reloads the clip with just the %d bee bullets; "
			     "after shooting those, his next reload is normal.' Ordinary clip %d, reload %.2fs. arm 0 = RED "
			     "(Trace.Ammo.Enabled 0): the bee rounds cannot be spent and the normal reload never happens. ====="),
			TraceXBees::GetStingBulletCount(), TraceAmmo::GetClipSize(), TraceAmmo::GetReloadSeconds());

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				SetArm(TEXT("Trace.Ammo.Enabled"), 1);
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetX* XSet = MakePlayerIntoX(TickWorld, Why);
			ATraceCharacter* XPawn = (XSet != nullptr) ? XSet->GetCharacter() : nullptr;
			UTraceWeaponComponent* Weapon = (XPawn != nullptr) ? XPawn->Weapon : nullptr;

			// Re-validated EVERY tick, not just at staging: an X who died, picked up the Core or drew
			// the knife between two steps would make everything below a true statement about the wrong
			// situation.
			if (XSet == nullptr || XPawn == nullptr || Weapon == nullptr
				|| !XPawn->IsAlive() || XPawn->IsCarrier() || Weapon->IsKnifeEquipped())
			{
				if (State->Step == 0 && NowReal <= State->Deadline)
				{
					return true;   // still staging
				}
				State->List.Invalidate(FString::Printf(
					TEXT("could not stage or hold X (%s): pawn=%s alive=%d carrying=%d knife=%d"),
					*Why, *GetNameSafe(XPawn),
					(XPawn != nullptr && XPawn->IsAlive()) ? 1 : 0,
					(XPawn != nullptr && XPawn->IsCarrier()) ? 1 : 0,
					(Weapon != nullptr && Weapon->IsKnifeEquipped()) ? 1 : 0));
				State->List.Report();
				SetArm(TEXT("Trace.Ammo.Enabled"), 1);
				return false;
			}

			const int32 BeeRounds = TraceXBees::GetStingBulletCount();

			// ---- step 0: THE RED ARM ---------------------------------------------------------
			if (State->Step == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[XSTINGCLIP] staged: X is %s."), *GetNameSafe(XPawn));

				SetArm(TEXT("Trace.Ammo.Enabled"), 0);
				XSet->ActivateAbility();
				for (int32 Index = 0; Index < BeeRounds + 2; ++Index)
				{
					Weapon->DebugConsumeRound();
				}
				const int32 RedClip = Weapon->GetClipAmmo();
				const bool bRedReloading = Weapon->IsReloading();
				State->bRedReproduced = (RedClip == BeeRounds) && !bRedReloading;

				State->List.Check(State->bRedReproduced,
					TEXT("RED ARM: with ammo disarmed the bee rounds cannot be spent and no reload follows"),
					FString::Printf(TEXT("clip %d after %d consumptions, reloading=%d — 'after shooting those' is "
					                     "unreachable, so the green arm below is measuring something"),
						RedClip, BeeRounds + 2, bRedReloading ? 1 : 0));

				SetArm(TEXT("Trace.Ammo.Enabled"), 1);
				State->Step = 1;
				return true;
			}

			// ---- step 1: STING REPLACES THE CLIP ---------------------------------------------
			if (State->Step == 1)
			{
				// Start from a PARTIAL ordinary clip, deliberately. Starting from a full 30 would let a
				// "Sting sets the clip to 5" implementation and a "Sting removes 25 rounds" one both
				// pass; from 25 they give 5 and 0.
				Weapon->LoadAbilityClip(0);                       // an ordinary-looking clip of 0...
				Weapon->RequestReload();                          // ...is illegal to leave, so refill it
				State->Step = 2;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.2;
				return true;
			}

			if (State->Step == 2)
			{
				for (int32 Index = 0; Index < 5; ++Index)
				{
					Weapon->DebugConsumeRound();
				}
				State->ClipBeforeSting = Weapon->GetClipAmmo();

				const bool bActivated = XSet->ActivateAbility();
				const int32 ClipAfter = Weapon->GetClipAmmo();
				const int32 AbilityAfter = Weapon->GetAbilityRoundsInClip();

				State->List.Check(bActivated && ClipAfter == BeeRounds && AbilityAfter == BeeRounds,
					TEXT("Sting REPLACES the clip with just the bee bullets"),
					FString::Printf(TEXT("clip %d -> %d (%d of them bee rounds) — not %d (unchanged) and not %d "
					                     "(added), so it replaced"),
						State->ClipBeforeSting, ClipAfter, AbilityAfter, State->ClipBeforeSting,
						State->ClipBeforeSting + BeeRounds));

				State->List.Check(ClipAfter < State->ClipBeforeSting,
					TEXT("...and the count therefore goes DOWN, which the HUD has to make obvious"),
					FString::Printf(TEXT("%d -> %d rounds"), State->ClipBeforeSting, ClipAfter));

				State->List.Check(XSet->GetLoadedBees() == BeeRounds && XSet->IsStingLoaded(),
					TEXT("the ability's replicated bee count mirrors the clip"),
					FString::Printf(TEXT("GetLoadedBees()=%d, stingLoaded=%d"),
						XSet->GetLoadedBees(), XSet->IsStingLoaded() ? 1 : 0));

				State->List.Check(!Weapon->IsReloading(),
					TEXT("Sting puts the gun up immediately — it does not cost a reload on top of the cast"),
					FString::Printf(TEXT("reloading=%d"), Weapon->IsReloading() ? 1 : 0));

				// R MUST NOT THROW THE BEES AWAY. [ASSUMPTION], spec v16 §1 — a 25 s ability lost to a
				// reflex press, with no undo and no feedback, is the worse of the two readings.
				const bool bManual = Weapon->RequestReload();
				State->List.Check(!bManual && !Weapon->IsReloading() && Weapon->GetAbilityRoundsInClip() == BeeRounds,
					TEXT("[ASSUMPTION] R cannot dump an ability-loaded clip"),
					FString::Printf(TEXT("RequestReload()=%d, still %d bee round(s)"),
						bManual ? 1 : 0, Weapon->GetAbilityRoundsInClip()));

				// ---- fire the five, and check each one was scored as a bee round ----
				for (int32 Index = 0; Index < BeeRounds; ++Index)
				{
					Weapon->DebugConsumeRound();
					if (!Weapon->WasLastRoundAbilityRound())
					{
						State->bAllFiredRoundsWereBeeRounds = false;
					}
				}

				State->List.Check(State->bAllFiredRoundsWereBeeRounds,
					TEXT("all five rounds out of the gun are scored as BEE rounds"),
					FString::Printf(TEXT("WasLastRoundAbilityRound() held for %d consecutive round(s) — this is "
					                     "what NotifyBulletHit gates the mark on, including the fifth and last"),
						BeeRounds));

				State->List.Check(Weapon->GetClipAmmo() == 0 && Weapon->GetAbilityRoundsInClip() == 0,
					TEXT("firing them empties the clip"),
					FString::Printf(TEXT("clip %d, bee rounds %d"),
						Weapon->GetClipAmmo(), Weapon->GetAbilityRoundsInClip()));

				State->Step = 3;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.25;
				return true;
			}

			// ---- step 3: "his next reload is NORMAL" -----------------------------------------
			State->List.Check(Weapon->GetClipAmmo() == TraceAmmo::GetClipSize()
				&& Weapon->GetAbilityRoundsInClip() == 0,
				TEXT("'after shooting those, his NEXT RELOAD IS NORMAL'"),
				FString::Printf(TEXT("clip %d/%d with %d bee round(s) — a full ordinary clip, unprompted"),
					Weapon->GetClipAmmo(), TraceAmmo::GetClipSize(), Weapon->GetAbilityRoundsInClip()));

			State->List.Check(XSet->GetLoadedBees() == 0 && !XSet->IsStingLoaded(),
				TEXT("'when all five are fired the bees resume orbiting' — even though none of them HIT"),
				FString::Printf(TEXT("GetLoadedBees()=%d, stingLoaded=%d. Under v14 §6 only a LANDED mark spent a "
				                     "bee, so five clean misses would have left the swarm in the gun forever"),
					XSet->GetLoadedBees(), XSet->IsStingLoaded() ? 1 : 0));

			Weapon->DebugConsumeRound();
			State->List.Check(!Weapon->WasLastRoundAbilityRound(),
				TEXT("the next round out of the gun is an ORDINARY round"),
				FString::Printf(TEXT("WasLastRoundAbilityRound()=%d — this is what stops the mark leaking past "
				                     "the reload"), Weapon->WasLastRoundAbilityRound() ? 1 : 0));

			if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — with Trace.Ammo.Enabled 0 the bee "
				                            "rounds were still spendable, so nothing above measures the clip"));
			}

			State->List.Report();
			SetArm(TEXT("Trace.Ammo.Enabled"), 1);
			return false;
		}));
	}

	FAutoConsoleCommand CmdStingClipTest(
		TEXT("Trace.X.StingClipTest"),
		TEXT("Dev only, SERVER. Spec v16 §1: Sting REPLACES the clip with 5 bee rounds, the rounds are spent by "
		     "FIRING rather than by hitting, and the reload after them is an ordinary 30. Red-arms itself with "
		     "Trace.Ammo.Enabled 0 first."),
		FConsoleCommandDelegate::CreateStatic(&Run));
}

#endif // !UE_BUILD_SHIPPING
