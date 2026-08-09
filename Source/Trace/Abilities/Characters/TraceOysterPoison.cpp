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
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

#if !UE_BUILD_SHIPPING
// Harness only — see "THE EVIDENCE" at the foot of this file.
#include "Components/CapsuleComponent.h"
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
#include "Abilities/Characters/TraceOysterJar.h"
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

	return Poison;
}

void UTraceOysterPoisonComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (GetOwner() != nullptr && GetOwner()->HasAuthority())
	{
		TickAuthority();
	}

	ApplySlowClamp();
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
	 * 0.30 + 0.45 = 0.75 at the instant of the burst, which the sweep above puts clearly short of the
	 * 1.00 that blows out — a flash, not a flashbang.
	 */
	constexpr float FlashBoost    = 0.45f;
	constexpr float FlashFraction = 0.12f;

	/**
	 * The cloud holds its brightness for most of the poison's life and only then fades out, rather
	 * than fading linearly from the first frame: §3's [ASSUMPTION] is that what you can see is what is
	 * still dangerous, and the poison is exactly as dangerous at 3 s as it was at 1 s.
	 */
	constexpr float TailStartFraction = 0.55f;

	/** Poison green. Not a team colour: a jar is dangerous to whoever is standing in it. */
	const FLinearColor CloudColor(0.12f, 0.85f, 0.25f, 1.f);

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

void ATraceOysterPoisonCloud::BeginPlay()
{
	Super::BeginPlay();

	// On the server ServerSpawnForBurst has already filled everything in. On a client the replicated
	// radius may not have landed yet, so the real build happens from Tick — BuildPuffsIfNeeded is
	// idempotent and costs one comparison until it can do the job.
	BuildPuffsIfNeeded();
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
	const FLinearColor Scaled = TraceOysterCloudTuning::CloudColor * LastAppliedIntensity;

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

#endif // !UE_BUILD_SHIPPING
