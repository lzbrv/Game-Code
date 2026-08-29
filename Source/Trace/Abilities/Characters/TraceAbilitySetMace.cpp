// Trace — MACE. See the header for the doc's wording and for why velocity is written by hand.

#include "Abilities/Characters/TraceAbilitySetMace.h"

#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "Containers/Ticker.h"
#include "Camera/CameraActor.h"                // the parade's observer — see PlaceObserver
#include "Engine/Engine.h"
#include "EngineUtils.h"                      // TActorIterator — the FxTest parade counts live bursts
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"                     // FScreenshotRequest — the parade aims its own frames

#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"     // ETraceCharacterId::Mace — the id, not the colour
#include "Abilities/Characters/TraceAbilityInputRelay.h"
#include "Abilities/Characters/TraceMaceSpike.h"
#include "Audio/TraceAudio.h"                 // FX §2.4's three sounds: throw (World), embed (burst), pull (loop)
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterRoster.h"       // THE accent. See MaceAccent() below.
#include "Core/TracePlayerController.h"
#include "Gameplay/TraceFxBurst.h"            // TraceFxLoopBudget — the §1.4 attach choke point
#include "Gameplay/TraceFxShapes.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// FX_AUDIO_PLAN §2.4 — the two WHILE-ACTIVE tells. Numbers here, mechanism at the bottom of the file.
// =================================================================================================

namespace TraceAbilitySetMaceFxFile
{
	/**
	 * Mace's accent — the SAME hue the spike, the rope, the sleeve and the embed burst wear.
	 * One hue per kit (bible §6.2), and the halo and the pull streams are part of that kit.
	 *
	 * *** READ FROM THE ROSTER, NOT COPIED. THIS IS A BUG FIX, NOT A REFACTOR. *** This line was
	 * `const FLinearColor MaceViolet(0.65f, 0.55f, 1.00f, 1.f)` — ART_BIBLE §2.3's old #D3C4FF —
	 * carrying the sentence above as a claim. When the ten accents were re-spaced away from the two
	 * team hues, Mace moved to #DFC4FE, TraceMaceSpike.cpp was moved onto the roster with him, and
	 * THIS copy was not. The sentence above became false: for one wave his suspend halo and his pull
	 * streams drew 13.2 degrees of hue away from his own spike, his own rope and his own body.
	 *
	 * It is the drawn-vs-lethal rule wearing a different hat — never a second literal for a number
	 * something else owns. TraceCharacterRoster is the owner: it is the table the ten
	 * UTraceCharacterDefinition assets are generated from and the table the body materials are
	 * stamped from, so "equals the roster row" is literally "equals the body". Falls back to white
	 * on a roster that did not resolve, which is loud rather than subtly wrong.
	 *
	 * Cheap enough to call per attach: Find() is a bounds check and an index into a static table,
	 * and both call sites run once per ability state edge, not per frame.
	 */
	FLinearColor MaceAccent()
	{
		if (const TraceCharacterRoster::FTraceCharacterEntry* Row =
			TraceCharacterRoster::Find(static_cast<uint8>(ETraceCharacterId::Mace)))
		{
			return FLinearColor(Row->Accent.R, Row->Accent.G, Row->Accent.B, 1.f);
		}
		return FLinearColor::White;
	}

	// --- the suspend halo -------------------------------------------------------------------------
	/** FX §2.4: "halo ring under feet (cylinder r 40 uu, h 3 uu), additive violet I 0.3". */
	constexpr float HaloRadiusUU = 40.f;
	constexpr float HaloHeightUU = 3.f;
	constexpr float HaloIntensity = 0.3f;

	/** How far under the capsule centre it sits: a standing pawn's feet are ~88 uu down. */
	constexpr float HaloFeetOffsetUU = -86.f;

	/** FX §2.4: "bobbing +/-4 uu @ 1.2 Hz (motion, not brightness — suspend is not a lethal telegraph)". */
	constexpr float HaloBobAmplitudeUU = 4.f;
	constexpr float HaloBobHz = 1.2f;

	// --- the pull slip-stream ---------------------------------------------------------------------
	/** FX §2.4: "2 trailing cylinders behind the pawn (l 140 uu, r 4 uu), additive violet I 0.4". */
	constexpr float StreamLengthUU = 140.f;
	constexpr float StreamRadiusUU = 4.f;
	constexpr float StreamIntensity = 0.4f;

	/** How far behind the capsule centre each piece is centred, and how far apart the two sit. */
	constexpr float StreamTrailOffsetUU = 60.f;
	constexpr float StreamLateralOffsetUU = 18.f;

	/** Fade the pull loop out over this rather than cutting it: a loop that stops dead reads as a bug. */
	constexpr float PullLoopFadeOutSeconds = 0.25f;
}

/**
 * THE RED ARM FOR DEMO 17 item 6 — the 3 s hidden cooldown on V.
 *
 * 1 (shipped): releasing V (or letting the suspend expire) refuses the next one for
 *              MaceSuspendCooldownSeconds.
 * 0:           the pre-Demo-17 behaviour, where she could re-suspend on the very next frame she was
 *              airborne. Trace.Mace.SuspendCooldownTest must go red under it. Never ship 0.
 *
 * A cvar rather than "set the knob to 0", because the knob is what the test is measuring and a harness
 * that rewrote its own subject would be proving that it can write a number.
 */
static TAutoConsoleVariable<int32> CVarMaceSuspendCooldown(
	TEXT("Trace.Mace.SuspendCooldown"),
	1,
	TEXT("Dev/red arm. 1 (default) = Demo 17's hidden cooldown on V, timed from the release or the "
	     "expiry. 0 = she may suspend again immediately, which is the behaviour before Demo 17. "
	     "Never ship 0."),
	ECVF_Cheat);

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetMace::OnEquipped()
{
	bSuspendHeld = false;
	bSuspending = false;
	SuspendEndMatchTime = 0.f;
	SuspendReadyMatchTime = 0.f;
	bPulling = false;
	bPullQueued = false;
	SpikeEmbedEndMatchTime = 0.f;
	ClearSpike();

	if (HasAuthority())
	{
		// The V key has nowhere to come from until ETraceInputAction gains the action; the relay is
		// the interim path and it is attached the moment somebody actually becomes Mace.
		if (UTraceAbilityComponent* Comp = GetAbilityComponent())
		{
			UTraceAbilityInputRelay::EnsureOn(Comp->GetOwningPlayerState());
		}
		PublishState();
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Mace] Equipped. magnet x%.2f | suspend %.2fs @ %.0f uu/s | spike %.0f uu @ %.0f uu/s, wall test "
		     "|n.Z|<=%.2f, aim forgiveness %.0f uu, embed %.1fs, pull %.0f uu/s | cooldown %.0fs [ASSUMPTION: §6 unspecified]"),
		GetMagnetRadiusMultiplier(),
		UTraceSettings::Get().MaceSuspendMaxSeconds, UTraceSettings::Get().MaceSuspendLateralSpeedCap,
		UTraceSettings::Get().MaceSpikeRangeUU, UTraceSettings::Get().MaceSpikeTravelSpeed,
		UTraceSettings::Get().MaceSpikeMaxSurfaceNormalZ, UTraceSettings::Get().MaceSpikeTraceRadiusUU,
		UTraceSettings::Get().MaceSpikeEmbedSeconds,
		GetPullSpeed(), GetActivatedCooldownSeconds());
}

void UTraceAbilitySetMace::OnUnequipped()
{
	StopSuspend(TEXT("unequipped"));
	StopPull(TEXT("unequipped"), /*bRemoveSpike*/ true);
	ClearSpike();

	// The pawn SURVIVES a character change, so attached FX would survive it too — onto a pawn that is
	// no longer Mace. Cosmetics first, exactly as UTraceAbilitySetElle::OnUnequipped does it.
	DetachAllKitFx();
}

void UTraceAbilitySetMace::OnPawnSpawned()
{
	// A fresh pawn carries nothing. Forget the old one rather than trying to detach from it — it is
	// being destroyed on this same path — and then re-attach whatever the live state says is on, which
	// is the respawn half of the §1.2 sync rule.
	SuspendHalo = nullptr;
	SuspendHaloMID = nullptr;
	PullStreamA = nullptr;
	PullStreamB = nullptr;
	PullStreamAMID = nullptr;
	PullStreamBMID = nullptr;
	PullLoopAudio = nullptr;
	FxPawn = nullptr;

	ApplyKitFx(State());
}

void UTraceAbilitySetMace::OnPawnDied()
{
	// Cooldowns are NOT touched — spec §5 is explicit that death does not reset them. Only the world
	// state a dead pawn cannot own goes away.
	StopSuspend(TEXT("died"));
	StopPull(TEXT("died"), /*bRemoveSpike*/ true);
	ClearSpike();

	// §8.9: no FX component survives its pawn. On a machine that is not the server this hook is the
	// ONLY notice — the flags are wiped by the death state wipe, but a wipe that arrives as "all bits
	// zero" is still an edge this kit would rather not depend on for a corpse.
	DetachAllKitFx();
}

void UTraceAbilitySetMace::OnHalfTime()
{
	// The framework has already zeroed the cooldown and Reset() the net state. All that is left is
	// the actor, which the framework knows nothing about.
	StopSuspend(TEXT("half time"));

	// ...AND THE V COOLDOWN, WHICH STOPSUSPEND HAS JUST STAMPED. Spec §5 is one line and it is
	// absolute: "They should all reset at halftime." Demo 17's 3 s hidden cooldown is a cooldown like
	// any other, and StopSuspend above sets it by design — so half time has to take it back off again,
	// or Mace would come out of the interval with a V she cannot use and no way to know why. This is
	// the ONLY place that clears it other than a fresh equip.
	SuspendReadyMatchTime = 0.f;
	StopPull(TEXT("half time"), /*bRemoveSpike*/ true);
	ClearSpike();
	DetachAllKitFx();
}

bool UTraceAbilitySetMace::ShouldDriveMovement() const
{
	// A simulated proxy's velocity is replicated; writing it there would fight the interpolation and
	// would show up as somebody else's Mace stuttering.
	return HasAuthority() || IsLocallyControlled();
}

// =================================================================================================
// PASSIVE — the magnet
// =================================================================================================

float UTraceAbilitySetMace::GetMagnetRadiusMultiplier() const
{
	// DERIVED, per §6's own instruction. The 450 lives in UTraceSettings::CoreCatchRadius and the
	// 0.30 lives in MaceMagnetRadiusBonus; 585 is written down nowhere.
	return 1.f + FMath::Max(0.f, UTraceSettings::Get().MaceMagnetRadiusBonus);
}

// =================================================================================================
// MOVEMENT — hold V to suspend
// =================================================================================================

bool UTraceAbilitySetMace::OnSecondaryPressed()
{
	bSuspendHeld = true;

	const ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		return false;
	}

	// "hold V IN THE AIR to suspend". On the ground it is simply not the ability.
	if (!MoveComp->IsFalling())
	{
		return false;
	}

	if (bSuspending || bPulling)
	{
		return false;
	}

	if (MatchTimeNow() < SuspendReadyMatchTime)
	{
		return false;
	}

	StartSuspend();
	return true;
}

void UTraceAbilitySetMace::OnSecondaryReleased()
{
	bSuspendHeld = false;

	// "Releasing V cancels IMMEDIATELY and gravity resumes." Not at the end of the frame, not on the
	// next physics step: the flag is cleared here and ApplySuspend simply stops writing Velocity.Z.
	if (bSuspending)
	{
		StopSuspend(TEXT("V released"));
	}
}

void UTraceAbilitySetMace::StartSuspend()
{
	bSuspending = true;
	SuspendEndMatchTime = MatchTimeNow() + FMath::Max(0.05f, UTraceSettings::Get().MaceSuspendMaxSeconds);

	if (HasAuthority())
	{
		PublishState();
	}
}

void UTraceAbilitySetMace::StopSuspend(const TCHAR* Why)
{
	if (!bSuspending)
	{
		return;
	}

	bSuspending = false;
	SuspendEndMatchTime = 0.f;

	// *** DEMO 17 item 6: THE 3 s HIDDEN COOLDOWN IS STAMPED HERE, WHICH IS THE WHOLE POINT. ***
	// "Time it from when she releases V or the suspend expires, not from when it started." This
	// function is the single exit from a suspend — release, the 1.25 s cap, landing, a pull, death —
	// so every one of those starts the clock and nothing has to remember to.
	const float Cooldown = (CVarMaceSuspendCooldown.GetValueOnAnyThread() != 0)
		? FMath::Max(0.f, UTraceSettings::Get().MaceSuspendCooldownSeconds)
		: 0.f;
	SuspendReadyMatchTime = MatchTimeNow() + Cooldown;

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Mace] Suspend ended (%s). V is refused for the next %.2fs — hidden, nothing draws it."),
			Why, Cooldown);
	}
}

void UTraceAbilitySetMace::ApplySuspend(float DeltaSeconds)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		StopSuspend(TEXT("no pawn"));
		return;
	}
	if (bPulling)
	{
		StopSuspend(TEXT("pull started"));
		return;
	}
	if (!MoveComp->IsFalling())
	{
		StopSuspend(TEXT("landed"));
		return;
	}
	if (MatchTimeNow() >= SuspendEndMatchTime)
	{
		StopSuspend(TEXT("1.25 s window elapsed"));
		return;
	}
	if (!bSuspendHeld)
	{
		StopSuspend(TEXT("no longer held"));
		return;
	}

	// "gravity does not affect her" — the whole of it. Velocity.Z is re-zeroed every frame rather
	// than once, because PhysFalling integrates gravity on every sub-step in between.
	FVector Vel = MoveComp->Velocity;
	Vel.Z = 0.f;

	// "she may move laterally capped at 550 uu/s". A CLAMP, not a scale: below the cap her ordinary
	// air strafe is untouched, which is what "may move laterally" reads as.
	const float Cap = FMath::Max(0.f, UTraceSettings::Get().MaceSuspendLateralSpeedCap);
	const FVector Planar(Vel.X, Vel.Y, 0.f);
	const float PlanarSpeed = Planar.Size();
	if (PlanarSpeed > Cap && PlanarSpeed > KINDA_SMALL_NUMBER)
	{
		const FVector Capped = Planar * (Cap / PlanarSpeed);
		Vel.X = Capped.X;
		Vel.Y = Capped.Y;
	}

	MoveComp->Velocity = Vel;
}

float UTraceAbilitySetMace::GetSuspendRemaining() const
{
	return bSuspending ? FMath::Max(0.f, SuspendEndMatchTime - MatchTimeNow()) : 0.f;
}

float UTraceAbilitySetMace::GetSuspendCooldownRemaining() const
{
	// DEMO 17 item 6. See the header: this is for the harness and the log, and deliberately for
	// nothing in UI/. Zero while she is actually suspending — the cooldown has not started yet,
	// because it is stamped when the suspend ENDS.
	return bSuspending ? 0.f : FMath::Max(0.f, SuspendReadyMatchTime - MatchTimeNow());
}

// =================================================================================================
// ACTIVATED — Spike
// =================================================================================================

float UTraceAbilitySetMace::GetActivatedCooldownSeconds() const
{
	// [ASSUMPTION] §6 leaves Mace's cooldown unspecified and the spec suggests 20 s "to match the
	// others". Shipped as a knob so a playtest can move it without a rebuild.
	return FMath::Max(0.f, UTraceSettings::Get().MaceSpikeCooldownSeconds);
}

bool UTraceAbilitySetMace::CanActivate(FText& OutReason) const
{
	if (bPulling)
	{
		OutReason = NSLOCTEXT("Trace", "MaceAlreadyPulling", "PULLING");
		return false;
	}
	return true;
}

bool UTraceAbilitySetMace::IsSpikeableSurface(const FHitResult& Hit, FString& OutWhy) const
{
	// "On hitting a WALL it embeds."
	//
	// THIS USED TO READ WallJumpMaxNormalZ, on the reasoning that the project should have exactly one
	// definition of a wall. Trace.Mace.SpikeConsistency measured that reasoning wrong: the wall jump's
	// 0.40 is a surface 66 degrees off horizontal, so every surface between the 45-degree walkable
	// limit and 66 degrees was refused — geometry she can neither stand on, walk up, NOR spike. Plates
	// at |normal.Z| 0.50 and 0.64 scored 0/14.
	//
	// MaceSpikeMaxSurfaceNormalZ defaults to the walkable floor limit instead, so the rule the spike
	// applies is "if she cannot walk on it, she can stick a spike in it". The wall jump keeps its own
	// number, which is a movement-feel decision that has been tuned against and has no reason to move
	// because the spike moved.
	const float MaxNormalZ = FMath::Clamp(UTraceSettings::Get().MaceSpikeMaxSurfaceNormalZ, 0.f, 1.f);
	if (FMath::Abs(Hit.ImpactNormal.Z) > MaxNormalZ)
	{
		OutWhy = FString::Printf(TEXT("hit a surface with |normal.Z| %.2f > %.2f — a floor or a ramp, not a wall"),
			FMath::Abs(Hit.ImpactNormal.Z), MaxNormalZ);
		return false;
	}
	return true;
}

bool UTraceAbilitySetMace::ResolveSpikeAnchor(FVector& OutAnchor, FVector& OutNormal, FString& OutWhy) const
{
	OutAnchor = FVector::ZeroVector;
	OutNormal = FVector::ZeroVector;

	const ATraceCharacter* MyPawn = GetCharacter();
	UWorld* WorldPtr = GetWorld();
	if (MyPawn == nullptr || WorldPtr == nullptr)
	{
		OutWhy = TEXT("no pawn");
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Range = FMath::Max(100.f, Settings.MaceSpikeRangeUU);
	const FVector Start = MyPawn->GetMuzzleLocation();
	const FVector End = Start + MyPawn->GetAimDirection() * Range;

	// BY OBJECT TYPE, not by channel. A channel trace on ECC_WorldStatic is also blocked by every
	// pawn capsule (the "Pawn" profile blocks the WorldStatic channel), so a team-mate standing in
	// front of Mace would have eaten the throw and reported "no wall". Asking for WorldStatic
	// OBJECTS asks the question the doc asks: is there a wall there.
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MaceSpikeThrow), /*bTraceComplex*/ false);
	QueryParams.AddIgnoredActor(MyPawn);

	// ---- STAGE 1: THE EXACT AIM RAY. Tried first, and it wins whenever it lands. ------------------
	//
	// It has to stay first, and it has to stay a line: the crosshair is exact in first person
	// (ATraceCharacter::GetAimDirection is arithmetically the view forward), so the wall the player
	// is looking AT must beat any wall the forgiveness below would settle for.
	FString WhyExactRayFailed;
	FHitResult Hit;
	if (WorldPtr->LineTraceSingleByObjectType(Hit, Start, End, ObjectParams, QueryParams))
	{
		if (IsSpikeableSurface(Hit, WhyExactRayFailed))
		{
			OutAnchor = Hit.ImpactPoint;
			OutNormal = Hit.ImpactNormal;
			OutWhy = TEXT("wall");
			return true;
		}
	}
	else
	{
		WhyExactRayFailed = FString::Printf(TEXT("nothing within %.0f uu"), Range);
	}

	// ---- STAGE 2: THE FORGIVENESS SWEEP. Only reached when the exact ray found nothing to stick to.
	//
	// An infinitely thin ray misses a 40 uu pillar at 1200 uu on one degree of aim error and a 16 uu
	// railing on half a degree — while the crosshair is still visibly on them. MEASURED at 5/7 and
	// 3/7 by Trace.Mace.SpikeConsistency, and the player has no way to tell that silence apart from
	// a bug. A sphere the width of the spike head is the smallest thing that fixes it.
	//
	// bStartPenetrating is refused rather than used: a sweep that begins inside geometry reports the
	// depenetration direction, not a surface, and anchoring to that would put the spike inside a wall
	// at a normal nobody asked about. Stage 1 is the path for anything that close.
	const float SweepRadius = FMath::Clamp(Settings.MaceSpikeTraceRadiusUU, 0.f, 60.f);
	if (SweepRadius > KINDA_SMALL_NUMBER)
	{
		FHitResult SweepHit;
		if (WorldPtr->SweepSingleByObjectType(SweepHit, Start, End, FQuat::Identity, ObjectParams,
				FCollisionShape::MakeSphere(SweepRadius), QueryParams)
			&& !SweepHit.bStartPenetrating)
		{
			FString WhySweepFailed;
			if (IsSpikeableSurface(SweepHit, WhySweepFailed))
			{
				OutAnchor = SweepHit.ImpactPoint;
				OutNormal = SweepHit.ImpactNormal;
				OutWhy = TEXT("wall (aim forgiveness sweep)");
				return true;
			}
		}
	}

	// The exact ray's complaint is the one reported: it is the one that describes what the player was
	// actually pointing at, and the harness classifies fizzles by reading it.
	OutWhy = WhyExactRayFailed;
	return false;
}

bool UTraceAbilitySetMace::ActivateAbility()
{
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr)
	{
		return false;
	}

	FVector Anchor = FVector::ZeroVector;
	FVector Normal = FVector::ZeroVector;
	FString Why;

	// THE SWEEP RUNS ON BOTH MACHINES. The client needs the same answer the server will reach or its
	// predicted cooldown is wrong for a whole round trip; arena geometry is present on both, so the
	// sweep is the cheapest way to agree. Only the server spawns anything.
	if (!ResolveSpikeAnchor(Anchor, Normal, Why))
	{
		// A FREE FIZZLE. The framework charges the cooldown only on a true return, and the header of
		// UTraceCharacterAbilitySet names "Mace's spike hitting nothing" as the example. Throwing at
		// the sky must not cost 20 s.
		UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Spike fizzled: %s. No cooldown charged."), *Why);
		return false;
	}

	if (HasAuthority())
	{
		// A second throw replaces the first rather than orphaning it. Reachable after a half-time
		// cooldown reset while a spike is still in the wall.
		ClearSpike();

		UWorld* WorldPtr = GetWorld();
		if (WorldPtr == nullptr)
		{
			return false;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Owner = const_cast<ATraceCharacter*>(MyPawn);

		ATraceMaceSpike* NewSpike = WorldPtr->SpawnActor<ATraceMaceSpike>(
			ATraceMaceSpike::StaticClass(), MyPawn->GetMuzzleLocation(), Normal.Rotation(), SpawnParams);

		if (NewSpike == nullptr)
		{
			return false;
		}

		const UTraceSettings& Settings = UTraceSettings::Get();
		// The backstop is generous on purpose: it is not the ability's timer, it is the "the owner
		// vanished" catch. The embed window is enforced in TickAbilities.
		NewSpike->InitialiseFlight(this, Anchor, FMath::Max(1.f, Settings.MaceSpikeTravelSpeed),
			/*BackstopLifetimeSeconds*/ Settings.MaceSpikeEmbedSeconds + 10.f);

		Spike = NewSpike;
		SpikeEmbedEndMatchTime = 0.f;    // starts when the flight ends, not when it is thrown
		PublishState();

		// FX §5.1: MaceSpikeThrow is a WORLD event fired at the cast ACCEPT — i.e. here, after the
		// sweep found a wall and the actor exists, never at the press. A fizzle (throwing at the sky)
		// costs no cooldown and must make no noise either, or the sound teaches the wrong lesson about
		// what worked. One play, on the authority, multicast to everybody: the spike's own replication
		// carries the visual, and this carries the sound.
		TraceAudio::PlayAt(this, TraceSoundEvents::MaceSpikeThrow, MyPawn->GetMuzzleLocation());

		UE_LOG(LogTraceGame, Log, TEXT("[Mace] Spike thrown at %s (%.0f uu away); embeds for %.1fs on arrival."),
			*Anchor.ToCompactString(), FVector::Dist(MyPawn->GetActorLocation(), Anchor),
			UTraceSettings::Get().MaceSpikeEmbedSeconds);
	}

	return true;
}

ATraceMaceSpike* UTraceAbilitySetMace::DebugThrowSpikeAt(const FVector& Anchor, float TravelSpeedOverride)
{
	if (!HasAuthority())
	{
		return nullptr;
	}
	UWorld* WorldPtr = GetWorld();
	ATraceCharacter* MyPawn = GetCharacter();
	if (WorldPtr == nullptr || MyPawn == nullptr)
	{
		return nullptr;
	}

	ClearSpike();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = MyPawn;

	ATraceMaceSpike* NewSpike = WorldPtr->SpawnActor<ATraceMaceSpike>(
		ATraceMaceSpike::StaticClass(), MyPawn->GetMuzzleLocation(), FRotator::ZeroRotator, SpawnParams);
	if (NewSpike == nullptr)
	{
		return nullptr;
	}

	NewSpike->InitialiseFlight(this, Anchor, TravelSpeedOverride,
		UTraceSettings::Get().MaceSpikeEmbedSeconds + 10.f);

	Spike = NewSpike;
	SpikeEmbedEndMatchTime = 0.f;

	if (NewSpike->IsEmbedded())
	{
		NotifySpikeEmbedded(NewSpike);
	}
	else
	{
		PublishState();
	}
	return NewSpike;
}

void UTraceAbilitySetMace::NotifySpikeEmbedded(ATraceMaceSpike* WhichSpike)
{
	if (!HasAuthority() || WhichSpike == nullptr || WhichSpike != Spike.Get())
	{
		return;
	}

	// "On hitting a wall it embeds for 2 s." The window starts HERE, on arrival, not at the throw —
	// otherwise a long throw would spend most of its 2 s in the air.
	SpikeEmbedEndMatchTime = MatchTimeNow() + FMath::Max(0.1f, UTraceSettings::Get().MaceSpikeEmbedSeconds);
	PublishState();

	UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Spike embedded; pullable for %.1fs."),
		UTraceSettings::Get().MaceSpikeEmbedSeconds);

	// A press made while it was still travelling fires HERE, on the frame it lands. See bPullQueued.
	if (bPullQueued)
	{
		bPullQueued = false;
		StartPull();
	}
}

// =================================================================================================
// WHOSE SPIKE IS IT — the two accessors below exist because only the SERVER has the pointer
// =================================================================================================
//
// ActivateAbility() spawns the actor inside `if (HasAuthority())`, so `Spike` is set on the server
// and NEVER on a remote client — the client's copy of the actor exists (it replicates) but nothing
// hands the ability set a pointer to it. Everything that asked `Spike.Get()` was therefore answering
// "no spike" on every machine that is not the server, which is why the reactivation could not be
// pressed on a client at all: IsSpikeReadyToPull() was false, the relay fell through to a fresh
// TryActivate(), and the cooldown the throw had just started refused it.
//
// The replicated scratch pad already carries both facts for the HUD — the flags and AuxLocation —
// so these read the pointer where it exists and the replicated state everywhere else.

bool UTraceAbilitySetMace::HasSpikeEmbedded() const
{
	if (const ATraceMaceSpike* MySpike = Spike.Get())
	{
		return MySpike->IsEmbedded();
	}
	return !HasAuthority() && (State().Flags & TraceMaceFlags::SpikeEmbedded) != 0;
}

bool UTraceAbilitySetMace::HasSpikeInFlight() const
{
	if (const ATraceMaceSpike* MySpike = Spike.Get())
	{
		return !MySpike->IsEmbedded();
	}
	return !HasAuthority() && (State().Flags & TraceMaceFlags::SpikeInFlight) != 0;
}

FVector UTraceAbilitySetMace::GetSpikeAnchorLocation() const
{
	if (const ATraceMaceSpike* MySpike = Spike.Get())
	{
		return MySpike->GetAnchorLocation();
	}
	return State().AuxLocation;
}

bool UTraceAbilitySetMace::IsSpikeReadyToPull() const
{
	if (bPulling)
	{
		return false;
	}
	// IN FLIGHT COUNTS. See RequestSpikePull: the press cannot pull yet, but it must not be handed
	// back to the framework as a fresh throw either, because TryActivate() will refuse it against the
	// cooldown this same spike started and the input is then simply gone.
	if (!HasSpikeEmbedded() && !HasSpikeInFlight())
	{
		return false;
	}

	const ATraceCharacter* MyPawn = GetCharacter();
	return MyPawn != nullptr && MyPawn->IsAlive();
}

float UTraceAbilitySetMace::GetPullSpeed() const
{
	// §6: "pulls her toward it AT THE MOMENTUM CEILING (the air-strafe hard cap)". DERIVED — the
	// ceiling is AirStrafeHardCapSpeed x AirStrafeAsymptoteScale, which is exactly how
	// UTraceCharacterMovementComponent::GetAirStrafeHardCapSpeed() composes it, and those two knobs
	// stay the only place the number lives.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Ceiling = FMath::Max(1.f, Settings.AirStrafeHardCapSpeed)
		* FMath::Clamp(Settings.AirStrafeAsymptoteScale, 0.5f, 2.f);
	return Ceiling * FMath::Max(0.1f, Settings.MaceSpikePullSpeedMultiplier);
}

void UTraceAbilitySetMace::RequestSpikePull()
{
	if (!IsSpikeReadyToPull())
	{
		return;
	}

	// THE PRESS IS REMEMBERED RATHER THAN DROPPED. A player who throws to reach height and
	// immediately double-taps is pressing during the flight every single time, and the flight is a
	// third of a second at the v15 travel speed. Before this the press went nowhere at all: nothing
	// may pull on a spike that has not landed, so RequestSpikePull returned and TryActivate had
	// already been ruled out by the cooldown.
	//
	// It is a remembered press, NOT a timed window: it is cleared by the pull starting, by the spike
	// going away, and by a second throw (ClearSpike), so it cannot outlive the throw that armed it.
	if (!HasSpikeEmbedded())
	{
		bPullQueued = true;
		return;
	}

	StartPull();
}

void UTraceAbilitySetMace::StartPull()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (!HasSpikeEmbedded() || MyPawn == nullptr)
	{
		return;
	}

	StopSuspend(TEXT("pull started"));

	bPulling = true;
	bPullQueued = false;
	PullAnchor = GetSpikeAnchorLocation();
	PullLastLocation = MyPawn->GetActorLocation();
	bPullSkipInputTestThisTick = true;

	if (UTraceCharacterMovementComponent* MoveComp = GetMovement())
	{
		// The pull must be able to lift her off the floor. Without this a ground-level pull is
		// integrated by PhysWalking, which discards Z and drags her along the floor instead.
		if (MoveComp->IsMovingOnGround())
		{
			MoveComp->SetMovementMode(MOVE_Falling);
		}
	}

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Log, TEXT("[Mace] Pull started toward %s at %.0f uu/s (the momentum ceiling)."),
			*PullAnchor.ToCompactString(), GetPullSpeed());
	}
}

void UTraceAbilitySetMace::StopPull(const TCHAR* Why, bool bRemoveSpike)
{
	if (!bPulling)
	{
		if (bRemoveSpike)
		{
			ClearSpike();
		}
		return;
	}

	bPulling = false;
	bPullSkipInputTestThisTick = false;
	bPullQueued = false;

	if (bRemoveSpike)
	{
		ClearSpike();
	}

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Log, TEXT("[Mace] Pull ended (%s)%s."), Why,
			bRemoveSpike ? TEXT(", spike removed") : TEXT(""));
	}
}

void UTraceAbilitySetMace::ApplyPull(float DeltaSeconds)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();

	// HasSpikeEmbedded() rather than Spike.Get(): on the owning client the pointer is always null (see
	// the accessors above), so asking for it here cancelled the predicted pull on its very first tick
	// and the client saw a pull that never moved her.
	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr || !HasSpikeEmbedded())
	{
		StopPull(TEXT("pawn or spike gone"), /*bRemoveSpike*/ true);
		return;
	}

	// THE FIRST TICK OF A PULL TESTS NEITHER OF THE TWO CANCEL CONDITIONS, and both exclusions are
	// load-bearing rather than tidy:
	//
	//   the INPUT test, because §6's "any movement input cancels" taken literally would cancel on the
	//   very frame a player still holding W presses the key (see the header on bPullSkipInputTestThisTick);
	//
	//   the BLOCKED test, because PullLastLocation was seeded at StartPull() in this same frame, so
	//   the pawn has by definition not moved yet and every pull would cancel itself as "bounced off
	//   geometry" one frame after it began. MEASURED: this is exactly what the first build did, and
	//   Trace.Mace.Verify caught it as a pull that reached 0 uu/s.
	const bool bFirstPullTick = bPullSkipInputTestThisTick;
	bPullSkipInputTestThisTick = false;

	// GetCurrentAcceleration() is the wish vector after the controller has applied it, and it is
	// valid on the server for a remote client too (ServerMove carries it), so both ends cancel on the
	// same press.
	if (!bFirstPullTick && !MoveComp->GetCurrentAcceleration().IsNearlyZero(1.f))
	{
		StopPull(TEXT("movement input"), /*bRemoveSpike*/ true);
		return;
	}

	const FVector Here = MyPawn->GetActorLocation();
	const FVector ToAnchor = PullAnchor - Here;
	const float Distance = ToAnchor.Size();

	const float ArriveRadius = FMath::Max(20.f, UTraceSettings::Get().MaceSpikeArriveRadiusUU);
	if (Distance <= ArriveRadius)
	{
		StopPull(TEXT("arrived"), /*bRemoveSpike*/ true);
		return;
	}

	// "She obeys normal physics while pulled — BOUNCING OFF A WALL CANCELS IT."
	//
	// Measured rather than predicted: the pull writes a known velocity, so the displacement it should
	// have produced last frame is known too. If the pawn covered far less than that, something solid
	// was in the way — which is precisely "bounced off a wall", and it also covers being wedged on a
	// ledge or a corner. Done AFTER the first tick (PullLastLocation is seeded at StartPull).
	const float ExpectedStep = GetPullSpeed() * DeltaSeconds;
	if (!bFirstPullTick && ExpectedStep > KINDA_SMALL_NUMBER && !PullLastLocation.IsZero())
	{
		const float ActualStep = FVector::Dist(Here, PullLastLocation);
		if (ActualStep < ExpectedStep * 0.35f)
		{
			StopPull(TEXT("blocked — bounced off geometry"), /*bRemoveSpike*/ true);
			return;
		}
	}
	PullLastLocation = Here;

	// The pull IS her velocity for the frame. Nothing here touches IsDashing(), so
	// AreWeaponActionsBlocked() stays false and §6's "she can shoot while pulled, and be shot" is
	// true for free — the weapon gate is a pure function of the dash clock.
	MoveComp->Velocity = (ToAnchor / Distance) * GetPullSpeed();
}

void UTraceAbilitySetMace::ClearSpike()
{
	if (ATraceMaceSpike* MySpike = Spike.Get())
	{
		if (HasAuthority())
		{
			MySpike->Destroy();
		}
	}
	Spike.Reset();
	SpikeEmbedEndMatchTime = 0.f;
	// A remembered reactivation belongs to ONE throw. ActivateAbility clears the old spike before
	// spawning the new one, so this is also what stops a press aimed at a spike she has replaced.
	bPullQueued = false;

	if (HasAuthority())
	{
		PublishState();
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void UTraceAbilitySetMace::TickAbilities(float DeltaSeconds)
{
	// The whole character is mode-B only. AreCharactersEnabled() is false in mode A and false with
	// the §3 toggle off, and the world subsystem will already be forcing her back to the Mannequin;
	// this stops her writing velocity in the frames before that lands.
	if (!UTraceAbilityComponent::AreCharactersEnabled(this))
	{
		if (bSuspending || bPulling)
		{
			StopSuspend(TEXT("characters disabled"));
			StopPull(TEXT("characters disabled"), /*bRemoveSpike*/ true);
		}
		DetachAllKitFx();
		return;
	}

	// EVERY MACHINE, AND BEFORE THE MOVEMENT EARLY-OUT BELOW. The two returns further down are about
	// who may WRITE velocity; presentation is for everybody watching, which is the whole point of the
	// router. 20 Hz is the component's tick rate (see the TickAbilities contract) — the bob and the
	// stream aim are both continuous functions of time, so they sample cleanly at it.
	TickKitFx(DeltaSeconds);

	if (HasAuthority())
	{
		// The 2 s embed window. Not enforced while she is mid-pull: the window is how long she has to
		// USE the spike, and cutting a pull that has already started at the two-second mark would be
		// a different rule from the one §6 states.
		const ATraceMaceSpike* MySpike = Spike.Get();
		if (MySpike != nullptr && MySpike->IsEmbedded() && !bPulling
			&& SpikeEmbedEndMatchTime > 0.f && MatchTimeNow() >= SpikeEmbedEndMatchTime)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Spike embed window expired unused."));
			ClearSpike();
		}
		else if (MySpike == nullptr && (State().Flags & (TraceMaceFlags::SpikeEmbedded | TraceMaceFlags::SpikeInFlight)) != 0)
		{
			PublishState();   // the actor went away by some route that did not publish
		}
		else if (MySpike != nullptr && MySpike->IsEmbedded()
			&& (State().Flags & TraceMaceFlags::SpikeEmbedded) == 0)
		{
			PublishState();   // the flight ended on a frame NotifySpikeEmbedded could not reach us
		}
	}

	if (!ShouldDriveMovement())
	{
		return;
	}

	// THE REMEMBERED REACTIVATION. NotifySpikeEmbedded fires this on the server the instant the spike
	// lands; this is the path for the owning client, where "it landed" arrives as a replicated flag
	// rather than as a call. Also the place a remembered press is forgotten when the spike it was
	// meant for stopped existing — a queued press must never survive its own throw.
	if (bPullQueued && !bPulling)
	{
		if (HasSpikeEmbedded())
		{
			bPullQueued = false;
			StartPull();
		}
		else if (!HasSpikeInFlight())
		{
			bPullQueued = false;
		}
	}

	if (bPulling)
	{
		ApplyPull(DeltaSeconds);
		return;   // the pull owns velocity outright; a suspend on the same frame would fight it
	}

	if (bSuspending)
	{
		ApplySuspend(DeltaSeconds);
	}
}

// =================================================================================================
// Replicated mirror — HUD, proxies, and the harness reading a client
// =================================================================================================

void UTraceAbilitySetMace::PublishState()
{
	if (!HasAuthority())
	{
		return;
	}

	const ATraceMaceSpike* MySpike = Spike.Get();

	FTraceAbilityNetState& NetState = MutableState();
	NetState.Flags = 0;
	if (bSuspending)                                 { NetState.Flags |= TraceMaceFlags::Suspending; }
	if (bPulling)                                    { NetState.Flags |= TraceMaceFlags::Pulling; }
	if (MySpike != nullptr && MySpike->IsEmbedded()) { NetState.Flags |= TraceMaceFlags::SpikeEmbedded; }
	if (MySpike != nullptr && !MySpike->IsEmbedded()){ NetState.Flags |= TraceMaceFlags::SpikeInFlight; }

	NetState.EffectEndMatchTime = SuspendEndMatchTime;
	NetState.AuxEndMatchTime = SpikeEmbedEndMatchTime;
	NetState.AuxLocation = (MySpike != nullptr) ? MySpike->GetAnchorLocation() : FVector::ZeroVector;

	MarkStateDirty();
}

// =================================================================================================
// FX §2.4 — THE ROUTER HALF. Runs on EVERY machine, off the replicated flags.
// =================================================================================================
//
// WHY THE FLAGS AND NOT THE LOCAL BOOLEANS. bSuspending and bPulling are written by StartSuspend()
// and StartPull(), which run on the server and on the owning client and nowhere else — an opponent's
// machine has neither, and that is precisely the class of bug F10 is about (the ability is invisible
// to everybody except the person using it). TraceMaceFlags::Suspending / ::Pulling are the same two
// facts, published by PublishState() and replicated to every machine, so hanging the presentation on
// them is the difference between a tell and a private one.
//
// The mapping is the file's own, not TraceAbilityFlags': FX §2.4 says "router edge on MovementActive
// (suspend flag)", and in this kit MovementActive is the PULL bit — Suspending is 1 << 0. Reading
// the plan's generic name rather than this character's would have put the halo on the wrong ability.

ATraceCharacter* UTraceAbilitySetMace::ResolveFxPawn() const
{
	// GetCharacter() is the same lookup, but going through it here would hide the §1.2 rule this
	// function exists to keep: the pawn is asked for FRESH every time, so a respawn between two ticks
	// is detected rather than leaving components parented to a corpse.
	const UTraceAbilityComponent* Comp = GetAbilityComponent();
	const APlayerState* OwningState = (Comp != nullptr) ? Comp->GetOwningPlayerState() : nullptr;
	return (OwningState != nullptr) ? Cast<ATraceCharacter>(OwningState->GetPawn()) : nullptr;
}

void UTraceAbilitySetMace::SetSuspendFxAttached(bool bAttached)
{
	ATraceCharacter* Pawn = ResolveFxPawn();

	if (!bAttached || Pawn == nullptr)
	{
		if (SuspendHalo != nullptr)
		{
			TraceFxLoopBudget::DetachLoopPrimitive(FxPawn.Get(), SuspendHalo);
			SuspendHalo = nullptr;
			SuspendHaloMID = nullptr;
		}
		return;
	}

	if (SuspendHalo != nullptr)
	{
		return;   // already up. SyncClientFx can run twice for one live state and must not stack.
	}

	USceneComponent* AttachTo = Pawn->GetRootComponent();
	if (AttachTo == nullptr)
	{
		return;
	}

	SuspendFxSeconds = 0.f;
	SuspendHalo = TraceFxLoopBudget::AttachLoopPrimitive(Pawn, AttachTo, UTraceFxShapes::GetCylinder(),
		TEXT("MaceSuspendHalo"), TraceAbilitySetMaceFxFile::MaceAccent(),
		TraceAbilitySetMaceFxFile::HaloIntensity,
		FVector(0.f, 0.f, TraceAbilitySetMaceFxFile::HaloFeetOffsetUU),
		TraceAbilitySetMaceFxFile::HaloRadiusUU, SuspendHaloMID);

	if (SuspendHalo != nullptr)
	{
		// AttachLoopPrimitive gives every piece a UNIFORM scale from its radius, which is right for a
		// blob and wrong for a disc. §2.4 asks for h 3 uu, so the Z is overwritten here — the helper
		// owns the budget and the material, the caller owns the shape.
		const float XY = UTraceFxShapes::ShapeScaleForRadiusUU(TraceAbilitySetMaceFxFile::HaloRadiusUU);
		SuspendHalo->SetRelativeScale3D(FVector(XY, XY,
			UTraceFxShapes::ShapeScaleForLengthUU(TraceAbilitySetMaceFxFile::HaloHeightUU)));

		FxPawn = Pawn;
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Mace] suspend halo up on %s (r %.0f uu, additive I %.2f, bobbing +/-%.0f uu @ %.1f Hz)."),
			*GetNameSafe(Pawn), TraceAbilitySetMaceFxFile::HaloRadiusUU,
			TraceAbilitySetMaceFxFile::HaloIntensity, TraceAbilitySetMaceFxFile::HaloBobAmplitudeUU,
			TraceAbilitySetMaceFxFile::HaloBobHz);
	}
}

void UTraceAbilitySetMace::SetPullFxAttached(bool bAttached)
{
	ATraceCharacter* Pawn = ResolveFxPawn();

	if (!bAttached || Pawn == nullptr)
	{
		if (PullStreamA != nullptr || PullStreamB != nullptr)
		{
			TraceFxLoopBudget::DetachLoopPrimitive(FxPawn.Get(), PullStreamA);
			TraceFxLoopBudget::DetachLoopPrimitive(FxPawn.Get(), PullStreamB);
			PullStreamA = nullptr;
			PullStreamB = nullptr;
			PullStreamAMID = nullptr;
			PullStreamBMID = nullptr;
		}

		if (PullLoopAudio != nullptr)
		{
			// FADED, NOT STOPPED. §1.6.4: the caller owns this component and bAutoDestroy is off, so a
			// loop that is never faded is a loop that plays until the pawn dies.
			PullLoopAudio->FadeOut(TraceAbilitySetMaceFxFile::PullLoopFadeOutSeconds, 0.f);
			PullLoopAudio = nullptr;
		}
		return;
	}

	USceneComponent* AttachTo = Pawn->GetRootComponent();
	if (AttachTo == nullptr)
	{
		return;
	}

	if (PullStreamA == nullptr && PullStreamB == nullptr)
	{
		const float XY = UTraceFxShapes::ShapeScaleForRadiusUU(TraceAbilitySetMaceFxFile::StreamRadiusUU);
		const float Z = UTraceFxShapes::ShapeScaleForLengthUU(TraceAbilitySetMaceFxFile::StreamLengthUU);

		auto MakeStream = [&](const TCHAR* NameHint, float Lateral,
		                      TObjectPtr<UMaterialInstanceDynamic>& OutMID) -> UStaticMeshComponent*
		{
			UStaticMeshComponent* Piece = TraceFxLoopBudget::AttachLoopPrimitive(Pawn, AttachTo,
				UTraceFxShapes::GetCylinder(), NameHint, TraceAbilitySetMaceFxFile::MaceAccent(),
				TraceAbilitySetMaceFxFile::StreamIntensity,
				FVector(-TraceAbilitySetMaceFxFile::StreamTrailOffsetUU, Lateral, 0.f),
				TraceAbilitySetMaceFxFile::StreamRadiusUU, OutMID);
			if (Piece != nullptr)
			{
				Piece->SetRelativeScale3D(FVector(XY, XY, Z));
			}
			return Piece;
		};

		PullStreamA = MakeStream(TEXT("MacePullStreamA"),
			-TraceAbilitySetMaceFxFile::StreamLateralOffsetUU, PullStreamAMID);
		PullStreamB = MakeStream(TEXT("MacePullStreamB"),
			+TraceAbilitySetMaceFxFile::StreamLateralOffsetUU, PullStreamBMID);

		if (PullStreamA != nullptr || PullStreamB != nullptr)
		{
			FxPawn = Pawn;
		}
	}

	if (PullLoopAudio == nullptr)
	{
		// LOCAL TO THIS MACHINE, and started on every one of them by this same router edge — which is
		// exactly what makes MacePullLoop a Client-side event in the table (§5.1). A World play here
		// would multicast a second copy on top of the one every machine has already started.
		PullLoopAudio = TraceAudio::StartLoopOn(AttachTo, TraceSoundEvents::MacePullLoop);
	}
}

void UTraceAbilitySetMace::DetachAllKitFx()
{
	SetSuspendFxAttached(false);
	SetPullFxAttached(false);
	FxPawn = nullptr;
}

void UTraceAbilitySetMace::ApplyKitFx(const FTraceAbilityNetState& Which)
{
	ATraceCharacter* Pawn = ResolveFxPawn();

	// RULE 1 OF THE ROUTER CONTRACT: a null pawn — or a DIFFERENT pawn from the one the pieces are
	// parented to — means detach everything. A respawn between two edges is otherwise a set of
	// components attached to a pawn nobody will ever ask about again.
	if (Pawn == nullptr || (FxPawn.IsValid() && FxPawn.Get() != Pawn))
	{
		DetachAllKitFx();
		if (Pawn == nullptr)
		{
			return;
		}
	}

	SetSuspendFxAttached((Which.Flags & TraceMaceFlags::Suspending) != 0);
	SetPullFxAttached((Which.Flags & TraceMaceFlags::Pulling) != 0);
}

void UTraceAbilitySetMace::OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New)
{
	// Only presentation, and only when the two bits this kit draws actually moved. Everything else on
	// the scratch pad (the embed deadline, the anchor) changes far more often and drives no FX.
	const uint8 Watched = TraceMaceFlags::Suspending | TraceMaceFlags::Pulling;
	if ((Old.Flags & Watched) == (New.Flags & Watched))
	{
		return;
	}

	ApplyKitFx(New);
}

void UTraceAbilitySetMace::SyncClientFx(const FTraceAbilityNetState& Current)
{
	// JOIN IN PROGRESS. A client that connects while Mace is already reeling in never saw the edge
	// that started the slip-stream, and would watch her fly across the arena with nothing on her.
	// Idempotent by construction: each Set*Attached returns early when its pieces are already up.
	ApplyKitFx(Current);
}

void UTraceAbilitySetMace::TickKitFx(float DeltaSeconds)
{
	if (SuspendHalo == nullptr && PullStreamA == nullptr && PullStreamB == nullptr)
	{
		return;
	}

	ATraceCharacter* Pawn = ResolveFxPawn();
	if (Pawn == nullptr || (FxPawn.IsValid() && FxPawn.Get() != Pawn))
	{
		DetachAllKitFx();
		return;
	}

	if (SuspendHalo != nullptr)
	{
		// MOTION, NOT BRIGHTNESS. Bible §3.3 forbids a brightness pulse on anything a player
		// range-finds against, and §1.4 allows a bob — so the halo moves and its intensity never does.
		SuspendFxSeconds += DeltaSeconds;
		const float Bob = TraceAbilitySetMaceFxFile::HaloBobAmplitudeUU
			* FMath::Sin(2.f * PI * TraceAbilitySetMaceFxFile::HaloBobHz * SuspendFxSeconds);
		SuspendHalo->SetRelativeLocation(TraceFxLoopBudget::ClampToFootprint(
			FVector(0.f, 0.f, TraceAbilitySetMaceFxFile::HaloFeetOffsetUU + Bob)));
	}

	if (PullStreamA != nullptr || PullStreamB != nullptr)
	{
		USceneComponent* Parent = Pawn->GetRootComponent();
		if (Parent == nullptr)
		{
			return;
		}

		// THE STREAM POINTS WHERE SHE HAS BEEN, which during a pull is the rope she is climbing. Taken
		// from live velocity rather than from PullAnchor because velocity is a fact every machine has
		// (it is replicated movement) while PullAnchor is server-and-owner only — the same reason the
		// flags drive the attach.
		FVector WorldBack = -Pawn->GetVelocity().GetSafeNormal();
		if (WorldBack.IsNearlyZero())
		{
			WorldBack = -Pawn->GetActorForwardVector();
		}

		const FVector LocalBack = Parent->GetComponentTransform().InverseTransformVectorNoScale(WorldBack).GetSafeNormal();
		const FRotator Aim = FRotationMatrix::MakeFromZ(LocalBack).Rotator();

		auto AimStream = [&](UStaticMeshComponent* Piece, float Lateral)
		{
			if (Piece == nullptr)
			{
				return;
			}
			const FVector Lateral3D = FVector::CrossProduct(LocalBack, FVector::UpVector).GetSafeNormal() * Lateral;
			Piece->SetRelativeRotation(Aim);
			Piece->SetRelativeLocation(TraceFxLoopBudget::ClampToFootprint(
				LocalBack * TraceAbilitySetMaceFxFile::StreamTrailOffsetUU + Lateral3D));
		};

		AimStream(PullStreamA, -TraceAbilitySetMaceFxFile::StreamLateralOffsetUU);
		AimStream(PullStreamB, +TraceAbilitySetMaceFxFile::StreamLateralOffsetUU);
	}
}

#if !UE_BUILD_SHIPPING

FString UTraceAbilitySetMace::DebugDescribeKitFx() const
{
	const ATraceCharacter* Pawn = ResolveFxPawn();

	auto DescribePiece = [](const UStaticMeshComponent* Piece) -> FString
	{
		if (Piece == nullptr)
		{
			return TEXT("-");
		}
		const FVector Scale = Piece->GetComponentScale();
		return FString::Printf(TEXT("r%.0f/l%.0fuu%s"),
			UTraceFxShapes::RadiusUUFromShapeScale(static_cast<float>(Scale.X)),
			UTraceFxShapes::LengthUUFromShapeScale(static_cast<float>(Scale.Z)),
			Piece->IsVisible() ? TEXT("") : TEXT(" HIDDEN"));
	};

	const bool bLoopPlaying = (PullLoopAudio != nullptr) && PullLoopAudio->IsPlaying();

	return FString::Printf(
		TEXT("pawn=%s | flags susp=%d pull=%d | halo %s (z %.0fuu) | streamA %s streamB %s | "
		     "MacePullLoop=%s | attached %d/%d of the §1.4 budget"),
		*GetNameSafe(Pawn),
		(State().Flags & TraceMaceFlags::Suspending) != 0 ? 1 : 0,
		(State().Flags & TraceMaceFlags::Pulling) != 0 ? 1 : 0,
		*DescribePiece(SuspendHalo),
		(SuspendHalo != nullptr) ? SuspendHalo->GetRelativeLocation().Z : 0.0,
		*DescribePiece(PullStreamA), *DescribePiece(PullStreamB),
		bLoopPlaying ? TEXT("PLAYING") : ((PullLoopAudio != nullptr) ? TEXT("component but silent") : TEXT("none")),
		DebugAttachedFxCount(), TraceFxLoopBudget::MaxPrimitivesPerPawn);
}

double UTraceAbilitySetMace::GetSuspendHaloLocalZ() const
{
	return (SuspendHalo != nullptr) ? SuspendHalo->GetRelativeLocation().Z : 0.0;
}

int32 UTraceAbilitySetMace::DebugAttachedFxCount() const
{
	int32 Count = 0;
	Count += (SuspendHalo != nullptr) ? 1 : 0;
	Count += (PullStreamA != nullptr) ? 1 : 0;
	Count += (PullStreamB != nullptr) ? 1 : 0;
	return Count;
}

bool UTraceAbilitySetMace::DebugPieceHue(const TCHAR* Piece, FLinearColor& OutHue) const
{
	const UMaterialInstanceDynamic* MID = nullptr;
	float Intensity = 1.f;

	if (FCString::Stricmp(Piece, TEXT("halo")) == 0)
	{
		MID = SuspendHaloMID;
		Intensity = TraceAbilitySetMaceFxFile::HaloIntensity;
	}
	else if (FCString::Stricmp(Piece, TEXT("streamA")) == 0)
	{
		MID = PullStreamAMID;
		Intensity = TraceAbilitySetMaceFxFile::StreamIntensity;
	}
	else if (FCString::Stricmp(Piece, TEXT("streamB")) == 0)
	{
		MID = PullStreamBMID;
		Intensity = TraceAbilitySetMaceFxFile::StreamIntensity;
	}

	if (MID == nullptr || Intensity <= UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	// READ BACK, not recomputed. Asking TraceAbilitySetMaceFxFile::MaceAccent() again here would be a
	// fixture checking its own arithmetic — the whole point is to interrogate the object the renderer
	// is actually handed. GetVectorParameterValue answers with the value SetGlow pushed.
	FLinearColor Stored = FLinearColor::White;
	if (!const_cast<UMaterialInstanceDynamic*>(MID)->GetVectorParameterValue(
			FMaterialParameterInfo(TEXT("Color")), Stored))
	{
		return false;
	}

	// Both pieces are ADDITIVE: SetGlow folds the intensity INTO the colour for that blend, because
	// for additive geometry brightness and colour are the same quantity. Dividing it back out is what
	// makes the result comparable with a roster row rather than with a roster row times 0.3.
	OutHue = FLinearColor(Stored.R / Intensity, Stored.G / Intensity, Stored.B / Intensity, 1.f);
	return true;
}

// =================================================================================================
// Trace.Mace.SuspendCooldownTest — DEMO 17 item 6, two arms, RED first
//
// Verbatim: "Add a 3 second cooldown to Mace's V. Time it from when she releases V or the suspend
// expires, not from when it started. Do not show it on the HUD."
//
// THREE CLAIMS, AND THE MIDDLE ONE IS THE ONLY INTERESTING ONE:
//
//   1. after a suspend, V is refused for a while             — trivially true of any cooldown;
//   2. *** THE WAIT IS MEASURED FROM THE RELEASE. *** She is held for a full second, and then V is
//      probed at RELEASE + 2.5 s, which is PRESS + 3.5 s. A cooldown timed from the press would
//      already be over at that instant and would accept the probe; the shipped one refuses it. That
//      single probe is the whole difference between the two readings of the sentence;
//   3. and it comes back — 3.4 s after the release V is accepted again.
//
// HIDDEN IS NOT ASSERTED HERE BECAUSE IT IS NOT A RUNTIME FACT: nothing in UI/ reads
// GetSuspendCooldownRemaining() (the HUD's Mace branch draws a chip while IsSuspending() and nothing
// otherwise), and this character does not override GetCharacterOwnedCooldownRemaining(), so the E ring
// cannot learn about it either. Both are properties of the code, checkable by reading two files, and a
// harness that "proved" them would be proving that it can grep.
//
// THE PRESSES ARE THE REAL V KEY, injected through the real input pipeline, because Mace's V is the
// one input in this game with its own bind and its own hold semantics.
//
// RED ARM: Trace.Mace.SuspendCooldown 0 restores the pre-Demo-17 behaviour, where she could re-suspend
// on the very next airborne frame. Probe 1 must be ACCEPTED under it.
// =================================================================================================

namespace TraceAbilitySetMaceFile
{
	struct FSuspendCooldownState
	{
		int32 Arm = 0;              // 0 = RED (cooldown disarmed), 1 = GREEN (shipped)
		int32 Step = 0;
		double StepStartReal = 0.0;
		double Deadline = 0.0;

		double ReleaseRealTime = 0.0;
		float HeldSeconds = 1.f;

		bool bSuspendedOnFirstHold = false;
		double FirstSuspendSeenReal = 0.0;
		double FirstSuspendLastSeenReal = 0.0;
		int32 LiftAttempts = 0;

		/** The shared lift leg (step 5) presses V for this long and then goes to this step. */
		float PendingHoldSeconds = 0.15f;
		int32 AfterPressStep = 1;

		/**
		 * Whether the lift leg must wait for V to be OFF cooldown before pressing.
		 *
		 * True for the arm's opening hold and false for the three probes, whose entire purpose is to
		 * press while the cooldown is running. It has to be checked AT THE PRESS rather than at staging:
		 * the previous arm's last suspend is still live when this one starts, and a live suspend reports
		 * zero cooldown by definition — so a staging-time check passed and the press then landed 0.2 s
		 * later against a freshly stamped 3 s. Measured, twice.
		 */
		bool bRequireReadyBeforePress = true;

		/**
		 * Real time the LAST injected hold lets go of V. The lift leg refuses to press before then.
		 *
		 * *** THIS IS THE BUG THE FIRST TWO RUNS OF THIS TEST REPORTED AS A PRODUCT FAILURE. ***
		 * Trace.SimInput schedules its key-up on a timer, so two holds can overlap: the previous probe's
		 * 0.15 s release landed 0.04 s INTO the next arm's 1 s hold, ended that suspend, and stamped the
		 * cooldown a whole second before the release this test was measuring from. Every "timed from the
		 * release" number after it was then measured against the wrong instant. The product was right in
		 * both runs; the fixture was pressing a key it had not finished pressing.
		 */
		double KeyFreeAfterReal = 0.0;
		bool bRedAcceptedImmediately = false;
		bool bGreenRefusedImmediately = false;
		bool bGreenRefusedPastPressWindow = false;
		bool bGreenAcceptedAfterCooldown = false;
		float ObservedCooldownAtRelease = -1.f;

		int32 Passed = 0;
		int32 Failed = 0;
		bool bInvalid = false;
		FString InvalidReason;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[MACEV]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	UWorld* FindAuthoritativeWorldForMaceV()
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

	UTraceAbilitySetMace* MakeLocalPlayerIntoMace(UWorld* WorldPtr)
	{
		const APlayerController* LocalPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		APlayerState* LocalState = (LocalPC != nullptr) ? LocalPC->PlayerState : nullptr;
		UTraceAbilityComponent* Comp = (LocalState != nullptr)
			? LocalState->FindComponentByClass<UTraceAbilityComponent>() : nullptr;
		if (Comp == nullptr)
		{
			return nullptr;
		}
		if (Comp->GetCharacterId() != ETraceCharacterId::Mace)
		{
			Comp->ServerSetCharacter(ETraceCharacterId::Mace);
		}
		return Comp->GetAbilitySetAs<UTraceAbilitySetMace>();
	}

	/**
	 * Puts her in the air, because "hold V IN THE AIR to suspend" is the ability's own precondition —
	 * and RETURNS WHETHER SHE ACTUALLY IS, which is the half the first version of this fixture left
	 * out. A press delivered while she is back on the deck is refused by the ability for a reason that
	 * has nothing to do with Demo 17, and the run then reports a cooldown failure that is not one.
	 *
	 * The lift is capped by the headroom above her: teleporting a capsule into a ceiling with bNoCheck
	 * leaves it embedded, the floor test finds the surface it is inside, and she is "walking" again on
	 * the very next update.
	 */
	bool LiftIntoTheAir(ATraceCharacter* Pawn)
	{
		if (Pawn == nullptr)
		{
			return false;
		}

		UTraceCharacterMovementComponent* MoveComp = Pawn->GetTraceMovement();
		UWorld* WorldPtr = Pawn->GetWorld();
		float Lift = 500.f;

		if (WorldPtr != nullptr)
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceMaceVLift), /*bTraceComplex*/ false);
			Params.AddIgnoredActor(Pawn);

			FHitResult Ceiling;
			const FVector From = Pawn->GetActorLocation();
			if (WorldPtr->LineTraceSingleByChannel(Ceiling, From, From + FVector(0.f, 0.f, 700.f),
				ECC_WorldStatic, Params))
			{
				Lift = FMath::Max(0.f, static_cast<float>(Ceiling.Distance) - 120.f);
			}
		}

		Pawn->TeleportTo(Pawn->GetActorLocation() + FVector(0.f, 0.f, Lift), Pawn->GetActorRotation(),
			/*bIsATest*/ false, /*bNoCheck*/ true);
		if (MoveComp != nullptr)
		{
			MoveComp->SetMovementMode(MOVE_Falling);
			MoveComp->Velocity = FVector::ZeroVector;
			return MoveComp->IsFalling();
		}
		return false;
	}

	void PressSecondaryKey(UWorld* WorldPtr, float HoldSeconds)
	{
		if (APlayerController* LocalPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr)
		{
			LocalPC->ConsoleCommand(FString::Printf(TEXT("Trace.SimInput V %.2f"), HoldSeconds),
				/*bWriteToLog=*/false);
		}
	}

	void RunSuspendCooldownTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorldForMaceV();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[MACEV] no authoritative game world — run this on the server."));
			return;
		}
		if (WorldPtr->IsPaused())
		{
			if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
			{
				FirstPC->SetPause(false);
			}
		}

		TSharedPtr<FSuspendCooldownState> State = MakeShared<FSuspendCooldownState>();
		State->Deadline = FPlatformTime::Seconds() + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[MACEV] ===== DEMO 17 item 6: a %.1fs HIDDEN cooldown on V, timed from the RELEASE and not "
			     "from the press. She is held for %.1fs, then V is probed at release+0.4s, at release+2.5s "
			     "(= press+3.5s, which a press-timed cooldown would have finished) and at release+3.4s. Arm 0 "
			     "is RED (Trace.Mace.SuspendCooldown 0). ====="),
			UTraceSettings::Get().MaceSuspendCooldownSeconds, State->HeldSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				if (IConsoleVariable* Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mace.SuspendCooldown")))
				{
					Arm->Set(1, ECVF_SetByConsole);
				}
				return false;
			}

			const double NowReal = FPlatformTime::Seconds();

			UTraceAbilitySetMace* Mace = MakeLocalPlayerIntoMace(TickWorld);
			ATraceCharacter* MyPawn = (Mace != nullptr) ? Mace->GetCharacter() : nullptr;
			const ATracePlayerController* LocalTracePC =
				Cast<ATracePlayerController>(TickWorld->GetFirstPlayerController());
			const bool bInputLive = (LocalTracePC != nullptr) && !LocalTracePC->IsGameInputSuppressed()
				&& !TickWorld->IsPaused();

			if (Mace == nullptr || MyPawn == nullptr || !MyPawn->IsAlive() || !bInputLive)
			{
				if (NowReal > State->Deadline)
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[MACEV] VERDICT: INVALID — could not stage (mace=%d livingPawn=%d inputLive=%d). Run "
						     "it EARLY in a match, with characters ON, before a bot team-mate claims Mace."),
						(Mace != nullptr) ? 1 : 0, (MyPawn != nullptr && MyPawn->IsAlive()) ? 1 : 0,
						bInputLive ? 1 : 0);
					return false;
				}
				return true;
			}

			IConsoleVariable* Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mace.SuspendCooldown"));

			// ---- Step 0: arm, START FROM A READY V, and get her genuinely airborne ------------
			if (State->Step == 0)
			{
				if (Arm != nullptr)
				{
					Arm->Set(State->Arm == 0 ? 0 : 1, ECVF_SetByConsole);
				}

				// EACH ARM MUST BEGIN WITH V ACTUALLY READY — enforced in the lift leg AT THE PRESS
				// (bRequireReadyBeforePress) rather than here, because the previous arm's last suspend is
				// usually still live at this instant and a live suspend reports zero cooldown.

				// Per-arm observations, reset per arm — otherwise the second arm inherits the first's
				// "she suspended for 0.99s" and a hold that never happened reads as a hold that did.
				State->bSuspendedOnFirstHold = false;
				State->FirstSuspendSeenReal = 0.0;
				State->FirstSuspendLastSeenReal = 0.0;

				State->LiftAttempts = 0;
				State->PendingHoldSeconds = State->HeldSeconds;
				State->AfterPressStep = 1;
				State->bRequireReadyBeforePress = true;
				State->Step = 5;      // the shared "lift, wait, then press" leg
				State->StepStartReal = NowReal;
				return true;
			}

			// ---- Step 5: THE SHARED LIFT. Press only once she is really falling ---------------
			//
			// Every probe in this test goes through here. The first version pressed on the same tick as
			// the teleport and measured whatever the ability said about a pawn that was sometimes still
			// on the deck — which produced a red that was the fixture's, not the game's.
			if (State->Step == 5)
			{
				const UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement();
				const bool bAirborne = (MoveComp != nullptr) && MoveComp->IsFalling();

				if (!bAirborne)
				{
					if (State->LiftAttempts > 40)
					{
						State->bInvalid = true;
						State->InvalidReason = TEXT("could not get Mace airborne for the probe — every lift "
						                            "put her back on a surface, so 'hold V IN THE AIR' was "
						                            "never satisfied and nothing below would be about the "
						                            "cooldown");
						State->Step = 9;
						return true;
					}
					++State->LiftAttempts;
					LiftIntoTheAir(MyPawn);
					return true;
				}

				// ONE HOLD AT A TIME. See KeyFreeAfterReal.
				if (NowReal < State->KeyFreeAfterReal)
				{
					return true;
				}

				// ...AND THE ARM'S OPENING HOLD MUST START FROM A READY V. See bRequireReadyBeforePress.
				if (State->bRequireReadyBeforePress && Mace->GetSuspendCooldownRemaining() > 0.f)
				{
					if (NowReal > State->Deadline)
					{
						State->bInvalid = true;
						State->InvalidReason = TEXT("V never came off cooldown before the staging deadline");
						State->Step = 9;
					}
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[MACEV] arm=%d press V for %.2fs (lifts=%d, airborne=%d, cooldown showing %.2fs, "
					     "suspending=%d, alive=%d)"),
					State->Arm, State->PendingHoldSeconds, State->LiftAttempts, bAirborne ? 1 : 0,
					Mace->GetSuspendCooldownRemaining(), Mace->IsSuspending() ? 1 : 0,
					MyPawn->IsAlive() ? 1 : 0);

				PressSecondaryKey(TickWorld, State->PendingHoldSeconds);
				State->KeyFreeAfterReal = NowReal + static_cast<double>(State->PendingHoldSeconds) + 0.20;
				State->Step = State->AfterPressStep;
				State->StepStartReal = NowReal;
				return true;
			}

			// ---- Step 1: confirm she actually suspended, then wait out the hold ---------------
			if (State->Step == 1)
			{
				if (Mace->IsSuspending())
				{
					if (!State->bSuspendedOnFirstHold)
					{
						State->FirstSuspendSeenReal = NowReal;
					}
					State->bSuspendedOnFirstHold = true;
					State->FirstSuspendLastSeenReal = NowReal;
				}

				// The hold plus a beat for the release to be delivered and processed.
				if ((NowReal - State->StepStartReal) < static_cast<double>(State->HeldSeconds) + 0.15)
				{
					return true;
				}

				// THE HOLD HAS TO HAVE BEEN A REAL HOLD. A suspend that collapsed two frames in (she
				// landed, a pull started) stamps the cooldown at the COLLAPSE, and every "timed from the
				// release" measurement below would then be measuring something else — which is exactly
				// how the first run of this test produced a red that belonged to the fixture. Half the
				// requested hold is the bar.
				const double SuspendHeldFor = State->bSuspendedOnFirstHold
					? (State->FirstSuspendLastSeenReal - State->FirstSuspendSeenReal) : 0.0;
				if (!State->bSuspendedOnFirstHold || SuspendHeldFor < (State->HeldSeconds * 0.5f))
				{
					State->bInvalid = true;
					State->InvalidReason = FString::Printf(
						TEXT("the first hold did not last: she suspended for %.2fs of a %.2fs hold, so the "
						     "cooldown was stamped by whatever cut it short (landing, a pull) and not by the "
						     "release. Nothing below would be about Demo 17"),
						SuspendHeldFor, State->HeldSeconds);
					State->Step = 9;
					return true;
				}

				State->ReleaseRealTime = NowReal;
				if (State->Arm == 1)
				{
					State->ObservedCooldownAtRelease = Mace->GetSuspendCooldownRemaining();
				}

				// The fixture's own vital signs, printed every run: a suspend that did not last, or a pawn
				// that is back on the floor, is the difference between a red that belongs to the game and
				// one that belongs to this file. Two runs were lost to not printing them.
				UE_LOG(LogTraceGame, Display,
					TEXT("[MACEV] arm=%d hold done: suspended for %.2fs of %.2fs, airborne now=%d, cooldown "
					     "showing %.2fs"),
					State->Arm, SuspendHeldFor, State->HeldSeconds,
					(MyPawn->GetTraceMovement() != nullptr && MyPawn->GetTraceMovement()->IsFalling()) ? 1 : 0,
					Mace->GetSuspendCooldownRemaining());
				State->Step = 2;
				return true;
			}

			// ---- Step 2: PROBE 1, 0.4 s after the release -------------------------------------
			if (State->Step == 2)
			{
				if ((NowReal - State->ReleaseRealTime) < 0.40)
				{
					return true;
				}
				State->LiftAttempts = 0;
				State->PendingHoldSeconds = 0.15f;
				State->AfterPressStep = 3;
				State->bRequireReadyBeforePress = false;
				State->Step = 5;
				return true;
			}

			if (State->Step == 3)
			{
				if ((NowReal - State->StepStartReal) < 0.10)
				{
					return true;
				}
				if (State->Arm == 0) { State->bRedAcceptedImmediately   =  Mace->IsSuspending(); }
				else                 { State->bGreenRefusedImmediately  = !Mace->IsSuspending(); }
				State->Step = 4;
				return true;
			}

			// ---- Step 4: PROBE 2 at release + 2.5 s = press + 3.5 s ---------------------------
			//
			// THE PROBE THAT DECIDES BETWEEN THE TWO READINGS. A 3 s cooldown timed from the PRESS is
			// over by now; one timed from the RELEASE is not.
			if (State->Step == 4)
			{
				if ((NowReal - State->ReleaseRealTime) < 2.5)
				{
					return true;
				}
				State->LiftAttempts = 0;
				State->PendingHoldSeconds = 0.15f;
				State->AfterPressStep = 6;
				State->bRequireReadyBeforePress = false;
				State->Step = 5;
				return true;
			}

			if (State->Step == 6)
			{
				if ((NowReal - State->StepStartReal) < 0.10)
				{
					return true;
				}
				if (State->Arm == 1)
				{
					State->bGreenRefusedPastPressWindow = !Mace->IsSuspending();
				}
				State->Step = 7;
				return true;
			}

			// ---- Step 7: PROBE 3 at release + 3.4 s — it must come back -----------------------
			if (State->Step == 7)
			{
				if ((NowReal - State->ReleaseRealTime) < 3.4)
				{
					return true;
				}
				State->LiftAttempts = 0;
				State->PendingHoldSeconds = 0.15f;
				State->AfterPressStep = 8;
				State->bRequireReadyBeforePress = false;
				State->Step = 5;
				return true;
			}

			if (State->Step == 8)
			{
				if ((NowReal - State->StepStartReal) < 0.10)
				{
					return true;
				}
				if (State->Arm == 1)
				{
					State->bGreenAcceptedAfterCooldown = Mace->IsSuspending();
				}

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Step = 0;
					State->Deadline = NowReal + 90.0;
					return true;
				}
				State->Step = 9;
				return true;
			}

			// ---- Step 9: verdict ---------------------------------------------------------------
			if (Arm != nullptr)
			{
				Arm->Set(1, ECVF_SetByConsole);
			}

			const float Knob = UTraceSettings::Get().MaceSuspendCooldownSeconds;

			if (State->bInvalid)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[MACEV] VERDICT: INVALID — %s"), *State->InvalidReason);
				return false;
			}

			// THE RED ARM IS JUDGED FIRST: with nothing falsified, a clean green means only that the code
			// did not crash.
			if (!State->bRedAcceptedImmediately)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[MACEV] VERDICT: INVALID — the RED arm did NOT reproduce: with the cooldown disarmed V "
					     "was still refused straight after a release, so something other than Demo 17's cooldown "
					     "is refusing it and the green arm proves nothing."));
				return false;
			}

			State->Check(State->bRedAcceptedImmediately,
				TEXT("RED (Trace.Mace.SuspendCooldown 0): V is accepted again IMMEDIATELY after a release — the "
				     "behaviour before Demo 17"));
			State->Check(State->bGreenRefusedImmediately,
				FString::Printf(TEXT("SHIPPED: V is REFUSED 0.4s after the release (%.1fs cooldown, and %.2fs of "
				                     "it was already showing at the release itself)"),
					Knob, State->ObservedCooldownAtRelease));
			State->Check(State->bGreenRefusedPastPressWindow,
				FString::Printf(TEXT("*** SHIPPED: still REFUSED at release + 2.5s, which is press + %.1fs — so "
				                     "the %.1fs is timed from the RELEASE, not from the activation ***"),
					State->HeldSeconds + 2.5f, Knob));
			State->Check(State->bGreenAcceptedAfterCooldown,
				FString::Printf(TEXT("...and it comes BACK: V is accepted again %.1fs after the release"), 3.4f));

			UE_LOG(LogTraceGame, Display,
				TEXT("[MACEV] The cooldown is HIDDEN by construction: nothing in UI/ reads "
				     "GetSuspendCooldownRemaining(), the HUD's Mace branch draws a chip only while she is "
				     "actually suspending, and Mace does not override GetCharacterOwnedCooldownRemaining(), so "
				     "the E ring never sees it either."));
			UE_LOG(LogTraceGame, Display, TEXT("[MACEV] ===== %d passed, %d failed ====="),
				State->Passed, State->Failed);
			if (State->Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[MACEV] VERDICT: PASS"));
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[MACEV] VERDICT: *** FAIL *** (%d)"), State->Failed);
			}
			return false;
		}));
	}

	FAutoConsoleCommand CmdMaceSuspendCooldownTest(
		TEXT("Trace.Mace.SuspendCooldownTest"),
		TEXT("DEMO 17 item 6, two arms RED first: the hidden cooldown on V, and specifically that it is timed "
		     "from the RELEASE rather than the press (probed at press + 3.5s, which a press-timed cooldown "
		     "would already have finished). Drives the local pawn — do not batch it with another driving test."),
		FConsoleCommandDelegate::CreateStatic(&RunSuspendCooldownTest));
}

// =================================================================================================
// Trace.Mace.FxTest — FX §2.4's FIVE ELEMENTS, STAGED ONE AT A TIME AND MEASURED OFF LIVE OBJECTS
//
// ===================================================================================================
// WHY A STAGED PARADE AND NOT AN ASSERTION SUITE
// ===================================================================================================
//
// Every element of §2.4 is a thing a player SEES, and four of the five only exist for a fraction of a
// second at a place the camera is not pointing: the embed burst is 0.22 s at a wall, the slip-stream
// lives for the length of a pull, the halo only exists while she is hovering. A test that spawned
// them and asked "did the pointer become non-null" would be measuring its own call, and a screenshot
// taken at "whatever frame that turned out to be" is not evidence of anything.
//
// So this stages each element, holds it, reads the LIVE components back (radii off their scales,
// audio off IsPlaying(), bursts off a world iterator) and requests a frame while it is genuinely on
// screen — the same shape as Trace.Fx.BurstTest's parade, for the same reason.
//
// IT ALSO POINTS THE CAMERA. Three of the five elements are on Mace's own body, and a first-person
// camera cannot see the body it is inside; without an observer every third-person frame is a
// photograph of a wall. The observer is a bare transient actor used as a view target — a purely local
// view change, so no pawn is moved and nothing the server owns is touched.
//
// FIXED FILENAMES (W4KitsB_Mace_*.png), so this run takes the release capture lock.
// =================================================================================================

namespace TraceAbilitySetMaceFile
{
	struct FMaceFxState
	{
		int32 Stage = 0;
		double StageStartReal = 0.0;
		int32 Passed = 0;
		int32 Failed = 0;
		int32 Shots = 0;

		TWeakObjectPtr<UTraceAbilitySetMace> Mace;
		TWeakObjectPtr<ACameraActor> Observer;

		FVector Anchor = FVector::ZeroVector;
		bool bFoundWall = false;
		bool bSpikeSeenEmbedded = false;
		int32 EmbedBursts = 0;
		int32 PullPieces = 0;
		bool bPullLoopPlaying = false;
		int32 HaloPieces = 0;
		int32 SpikePiecesDrawn = 0;
		bool  bStreamsSeen = false;
		bool  bHaloSeen = false;
		float HaloZ1 = 0.f;
		float HaloZ2 = 0.f;
		int32 HaloSamples = 0;
		float HaloZMin = 0.f;
		float HaloZMax = 0.f;

		// ---- THE HUE, READ OFF THE LIVE MIDs -------------------------------------------------
		//
		// Added because this parade passed nine checks on a kit whose halo and both slip-streams were
		// wearing LAST PALETTE'S violet: it measured radii, bob, budget and audio, and never once
		// asked what colour anything was. See UTraceAbilitySetMace::DebugPieceHue.
		bool bHaloHueRead = false;
		bool bStreamHueRead = false;
		FLinearColor HaloHue = FLinearColor::White;
		FLinearColor StreamAHue = FLinearColor::White;
		FLinearColor StreamBHue = FLinearColor::White;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; UE_LOG(LogTraceGame, Display, TEXT("[MACEFX]   ok   %s"), *What); }
			else            { ++Failed; UE_LOG(LogTraceGame, Error,   TEXT("[MACEFX]   FAIL %s"), *What); }
		}

		void Advance(int32 Next) { Stage = Next; StageStartReal = FPlatformTime::Seconds(); }
		double In() const { return FPlatformTime::Seconds() - StageStartReal; }
	};

	/**
	 * A camera to watch her from, because a first-person view cannot see the body it is inside.
	 *
	 * *** ACameraActor AND NOT A BARE AActor, AND THE DIFFERENCE COST A WHOLE CAPTURE RUN. ***
	 * AActor has NO ROOT COMPONENT, so SpawnActor's transform has nothing to write to: the actor sits
	 * at the world origin for ever and GetActorLocation() reports (0,0,0). Made the view target, it
	 * produced two perfectly convincing frames of the arena seen from its own centre at floor level —
	 * a picture of somewhere nobody was, with the right HUD on it. ACameraActor has a root and a
	 * camera component, so it goes where it is put.
	 */
	ACameraActor* PlaceObserver(UWorld* WorldPtr, const FVector& At, const FVector& LookAt)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		ACameraActor* Observer = WorldPtr->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), At,
			(LookAt - At).Rotation(), Params);
		if (Observer != nullptr)
		{
			if (APlayerController* PC = WorldPtr->GetFirstPlayerController())
			{
				PC->SetViewTargetWithBlend(Observer, 0.f);
			}
		}
		return Observer;
	}

	/** Keeps the observer over her shoulder while she moves — a pull crosses the arena in a second. */
	void FollowWithObserver(AActor* Observer, const ATraceCharacter* Pawn, float BehindUU, float UpUU)
	{
		if (Observer == nullptr || Pawn == nullptr)
		{
			return;
		}
		const FVector Focus = Pawn->GetActorLocation();
		const FVector At = Focus - Pawn->GetActorForwardVector() * BehindUU
			+ FVector::CrossProduct(Pawn->GetActorForwardVector(), FVector::UpVector) * 90.f
			+ FVector(0.f, 0.f, UpUU);
		Observer->SetActorLocation(At);
		Observer->SetActorRotation((Focus - At).Rotation());
	}

	void ReleaseObserver(UWorld* WorldPtr, AActor* Observer)
	{
		if (APlayerController* PC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr)
		{
			PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
		}
		if (Observer != nullptr)
		{
			Observer->Destroy();
		}
	}

	void ShootFrame(FMaceFxState& State, const TCHAR* Tag)
	{
		++State.Shots;
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots")
			/ FString::Printf(TEXT("W4KitsB_Mace_%02d_%s.png"), State.Shots, Tag));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[MACEFX] Screenshot requested: %s"), *Path);
	}

	/** Counts live bursts of one type. The burst actor's own replication is what put them there. */
	int32 CountBursts(UWorld* WorldPtr, ETraceFxBurstType Type)
	{
		int32 Count = 0;
		for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
		{
			if (IsValid(*It) && It->GetBurstType() == Type)
			{
				++Count;
			}
		}
		return Count;
	}

	/**
	 * Turns the pawn on the spot until the SHIPPED resolver finds a wall, and returns where.
	 *
	 * The shipped resolver, deliberately: a fixture with its own line trace would happily find a
	 * surface Mace's own rules refuse, and then report the ability broken for obeying them.
	 */
	bool AimAtAWall(UTraceAbilitySetMace* Mace, ATraceCharacter* Pawn, UWorld* WorldPtr, FVector& OutAnchor,
	                float PreferredDistanceUU = 1500.f)
	{
		APlayerController* PC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		if (Mace == nullptr || Pawn == nullptr || PC == nullptr)
		{
			return false;
		}

		// *** THE WALL NEAREST A CHOSEN DISTANCE, NOT THE FIRST AND NOT THE NEAREST. ***
		//
		// Both of the obvious rules were tried and both produced a useless run. The FIRST yaw that
		// answers is usually the length of the arena away: a 5,748 uu rope photographs as a line
		// converging on a vanishing point — technically the effect, visually nothing. The NEAREST is
		// worse in the other direction: at 297 uu the pull was over in under a tenth of a second, so
		// the slip-stream and MacePullLoop were both gone before the parade could look at them and the
		// run reported two failures about effects that had worked perfectly.
		//
		// A middling wall is the one that shows both: long enough that the rope has length and the pull
		// has duration, short enough that the spike is a thing in frame rather than a pixel.
		FVector Normal = FVector::ZeroVector;
		FString Why;
		FVector BestAnchor = FVector::ZeroVector;
		float BestDistance = 0.f;
		float BestScore = TNumericLimits<float>::Max();
		float BestYaw = 0.f;
		int32 Found = 0;

		for (int32 Step = 0; Step < 24; ++Step)
		{
			const FRotator Aim(0.f, static_cast<float>(Step) * 15.f, 0.f);
			PC->SetControlRotation(Aim);
			Pawn->SetActorRotation(FRotator(0.f, Aim.Yaw, 0.f));

			FVector Anchor = FVector::ZeroVector;
			if (Mace->ResolveSpikeAnchor(Anchor, Normal, Why))
			{
				++Found;
				const float Distance = FVector::Dist(Pawn->GetActorLocation(), Anchor);
				const float Score = FMath::Abs(Distance - PreferredDistanceUU);
				if (Score < BestScore)
				{
					BestScore = Score;
					BestDistance = Distance;
					BestAnchor = Anchor;
					BestYaw = Aim.Yaw;
				}
			}
		}

		if (Found == 0)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[MACEFX] no spikeable wall from here: %s"), *Why);
			return false;
		}

		PC->SetControlRotation(FRotator(0.f, BestYaw, 0.f));
		Pawn->SetActorRotation(FRotator(0.f, BestYaw, 0.f));
		OutAnchor = BestAnchor;
		UE_LOG(LogTraceGame, Display,
			TEXT("[MACEFX] %d of 24 yaws found a wall; the one closest to the %.0f uu the parade wants is "
			     "yaw %.0f deg at (%s), %.0f uu away."),
			Found, PreferredDistanceUU, BestYaw, *OutAnchor.ToCompactString(), BestDistance);
		return true;
	}

	void RunMaceFxTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorldForMaceV();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[MACEFX] no authoritative game world."));
			return;
		}

		UTraceAbilitySetMace* Mace = MakeLocalPlayerIntoMace(WorldPtr);
		if (Mace == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[MACEFX] the local player has no ability component."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[MACEFX] ===== FX §2.4 parade: spike material, rope core+sleeve, embed burst, suspend "
			     "halo, pull slip-stream + MacePullLoop ====="));

		TSharedRef<FMaceFxState> State = MakeShared<FMaceFxState>();
		State->Mace = Mace;
		State->StageStartReal = FPlatformTime::Seconds();
		TWeakObjectPtr<UWorld> WeakWorld(WorldPtr);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			UTraceAbilitySetMace* Kit = State->Mace.Get();
			if (TickWorld == nullptr || Kit == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[MACEFX] the world or the kit went away mid-run."));
				return false;
			}
			ATraceCharacter* Pawn = Kit->GetCharacter();
			if (Pawn == nullptr)
			{
				return true;   // between respawns; the stage clock is real time and will time out
			}

			switch (State->Stage)
			{
			case 0:
				// Let ServerSetCharacter build the set and the pawn settle.
				if (State->In() > 1.0)
				{
					State->bFoundWall = AimAtAWall(Kit, Pawn, TickWorld, State->Anchor);
					State->Advance(1);
				}
				break;

			case 1:
				// THE REAL CAST, through the real E key: the MaceSpikeThrow sound and the cooldown are
				// both on that path, and a DebugThrowSpikeAt here would have proven the dressing while
				// quietly skipping the sound this tranche also owns.
				if (State->bFoundWall)
				{
					if (APlayerController* PC = TickWorld->GetFirstPlayerController())
					{
						PC->ConsoleCommand(TEXT("Trace.SimInput E 0.05"), /*bWriteToLog=*/false);
					}
				}
				State->Advance(2);
				break;

			case 2:
			{
				const ATraceMaceSpike* Spike = Kit->GetSpike();
				if (Spike != nullptr && Spike->IsEmbedded())
				{
					State->bSpikeSeenEmbedded = true;
					State->SpikePiecesDrawn = Spike->GetDrawnPieceCount();
					State->EmbedBursts = CountBursts(TickWorld, ETraceFxBurstType::SpikeEmbed);
					UE_LOG(LogTraceGame, Display, TEXT("[MACEFX] spike: %s"), *Spike->DebugDescribe());
					ShootFrame(*State, TEXT("SpikeRope"));
					State->Advance(3);
				}
				else if (State->In() > 4.0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[MACEFX] no spike embedded within 4 s (found a wall = %d)."),
						State->bFoundWall ? 1 : 0);
					State->Advance(3);
				}
				break;
			}

			case 3:
				// ONE FRAME OF THE EMBED BURST'S TAIL, at ~0.5 s — well past its 0.22 s animation and
				// well inside ATraceFxBurst's 1.2 s lifespan. It is here to DOCUMENT a cross-tranche
				// defect rather than to prove this kit's work: a burst holds alpha = 1 for the rest of
				// its life, and an opaque emissive piece written to intensity 0 renders BLACK. See the
				// report; the fix belongs to whoever owns TraceFxBurst.cpp, not to this file.
				if (State->In() > 0.45 && State->Shots < 2)
				{
					ShootFrame(*State, TEXT("SpikeEmbedTail"));
				}
				// The pull soon after: the embed window is MaceSpikeEmbedSeconds and a parade that
				// dawdled would be testing the expiry instead.
				if (State->In() > 0.60)
				{
					State->Observer = PlaceObserver(TickWorld,
						Pawn->GetActorLocation() - Pawn->GetActorForwardVector() * 300.f + FVector(0.f, 0.f, 90.f),
						Pawn->GetActorLocation());
					Kit->RequestSpikePull();
					State->Advance(4);
				}
				break;

			case 4:
			{
				// THE OBSERVER FOLLOWS. A pull crosses hundreds of uu in the third of a second between
				// asking for a frame and getting one; a fixed camera photographs where she WAS.
				FollowWithObserver(State->Observer.Get(), Pawn, 300.f, 90.f);

				// POLLED, for the same reason the halo is: a pull at the momentum ceiling is over in a
				// fraction of a second, and a fixture that sampled at a fixed delay measured an ability
				// that had already finished and called the result a defect.
				const FString Line = Kit->DebugDescribeKitFx();
				if (Line.Contains(TEXT("streamA r4/l140uu")) && Line.Contains(TEXT("streamB r4/l140uu")))
				{
					if (!State->bStreamsSeen)
					{
						State->bStreamsSeen = true;
						State->PullPieces = Kit->DebugAttachedFxCount();
						State->bStreamHueRead =
							Kit->DebugPieceHue(TEXT("streamA"), State->StreamAHue)
							&& Kit->DebugPieceHue(TEXT("streamB"), State->StreamBHue);
						UE_LOG(LogTraceGame, Display, TEXT("[MACEFX] pull: %s"), *Line);
						UE_LOG(LogTraceGame, Display,
							TEXT("[MACEFX] pull hue (read off the MIDs): A (%.3f,%.3f,%.3f) B (%.3f,%.3f,%.3f)"),
							State->StreamAHue.R, State->StreamAHue.G, State->StreamAHue.B,
							State->StreamBHue.R, State->StreamBHue.G, State->StreamBHue.B);
						ShootFrame(*State, TEXT("PullStream"));
					}
					State->bPullLoopPlaying = State->bPullLoopPlaying
						|| Line.Contains(TEXT("MacePullLoop=PLAYING"));
				}

				if (State->bStreamsSeen && !Kit->IsPulling())
				{
					State->Advance(5);
				}
				else if (State->In() > 3.0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[MACEFX] the pull produced no slip-stream inside 3 s. Last: %s"), *Line);
					State->Advance(5);
				}
				break;
			}

			case 5:
				// THE SUSPEND IS STAGED ONLY ONCE THE PULL IS OVER, and that is not a style point: the
				// pull owns velocity outright and StartPull() stops a suspend, so a V pressed mid-pull
				// is refused. The first version of this parade did exactly that, read the two SLIP-STREAM
				// pieces still attached as "the halo is up", and printed a green line about an effect
				// that had never been attached — a fixture passing on somebody else's evidence.
				FollowWithObserver(State->Observer.Get(), Pawn, 300.f, 90.f);
				if (!Kit->IsPulling() && State->In() > 0.6)
				{
					LiftIntoTheAir(Pawn);
					PressSecondaryKey(TickWorld, 2.5f);
					State->Advance(6);
				}
				else if (State->In() > 4.0)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[MACEFX] the pull never ended; staging the suspend anyway."));
					LiftIntoTheAir(Pawn);
					PressSecondaryKey(TickWorld, 2.5f);
					State->Advance(6);
				}
				break;

			case 6:
			{
				FollowWithObserver(State->Observer.Get(), Pawn, 260.f, 40.f);

				// POLLED for the HALO ITSELF, by name, so the pieces that answer cannot be somebody
				// else's. r40 is §2.4's radius read back off the live component's scale.
				const FString Line = Kit->DebugDescribeKitFx();
				if (Line.Contains(TEXT("halo r40")))
				{
					if (!State->bHaloSeen)
					{
						State->bHaloSeen = true;
						State->HaloPieces = Kit->DebugAttachedFxCount();
						State->bHaloHueRead = Kit->DebugPieceHue(TEXT("halo"), State->HaloHue);
						UE_LOG(LogTraceGame, Display, TEXT("[MACEFX] suspend: %s"), *Line);
						UE_LOG(LogTraceGame, Display,
							TEXT("[MACEFX] halo hue (read off the MID): (%.3f,%.3f,%.3f)"),
							State->HaloHue.R, State->HaloHue.G, State->HaloHue.B);
						ShootFrame(*State, TEXT("SuspendHalo"));
					}

					// THE BOB IS MOTION, so it is measured as a SPREAD over many samples rather than as
					// two readings that could both land on the same phase of a 1.2 Hz sine.
					const float Z = static_cast<float>(Kit->GetSuspendHaloLocalZ());
					if (State->HaloSamples == 0) { State->HaloZMin = Z; State->HaloZMax = Z; State->HaloZ1 = Z; }
					State->HaloZMin = FMath::Min(State->HaloZMin, Z);
					State->HaloZMax = FMath::Max(State->HaloZMax, Z);
					State->HaloZ2 = Z;
					++State->HaloSamples;
				}

				if (State->In() > 1.6)
				{
					State->Advance(7);
				}
				break;
			}

			case 7:
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[MACEFX] suspend halo: %d sample(s), local z %.1f .. %.1f uu (spread %.1f uu against "
					     "the +/-4 uu §2.4 asks for), last %s"),
					State->HaloSamples, State->HaloZMin, State->HaloZMax,
					State->HaloZMax - State->HaloZMin, *Kit->DebugDescribeKitFx());

				ReleaseObserver(TickWorld, State->Observer.Get());

				UE_LOG(LogTraceGame, Display, TEXT("[MACEFX] ===== verdict ====="));
				State->Check(State->bFoundWall, TEXT("the SHIPPED resolver found a spikeable wall to throw at"));
				State->Check(State->bSpikeSeenEmbedded, TEXT("the spike flew and reported itself EMBEDDED"));
				State->Check(State->SpikePiecesDrawn == 3,
					FString::Printf(TEXT("all THREE dressed pieces are drawn on this machine — cone, rope core, "
					                     "rope sleeve (drawn=%d of 3)"), State->SpikePiecesDrawn));
				State->Check(State->EmbedBursts >= 1,
					FString::Printf(TEXT("an ATraceFxBurst(SpikeEmbed) was standing at the anchor (%d)"),
						State->EmbedBursts));
				State->Check(State->bStreamsSeen,
					FString::Printf(TEXT("the pull attached BOTH slip-stream cylinders at §2.4's r 4 uu / l 140 uu "
					                     "(%d attached piece(s))"), State->PullPieces));
				State->Check(State->bPullLoopPlaying,
					TEXT("MacePullLoop is a PLAYING attached audio component during the pull"));
				State->Check(State->bHaloSeen,
					FString::Printf(TEXT("the SUSPEND attached its own halo at §2.4's r 40 uu — asked for by name, "
					                     "not inferred from a piece count (%d attached)"), State->HaloPieces));
				State->Check(State->HaloSamples > 3 && (State->HaloZMax - State->HaloZMin) > 1.0f,
					FString::Printf(TEXT("*** and it BOBS: %d samples spanning %.1f uu of travel, motion rather "
					                     "than a brightness pulse (bible §3.3) ***"),
						State->HaloSamples, State->HaloZMax - State->HaloZMin));
				// ---- THE HUE, ASSERTED AGAINST THE ROSTER ROW ------------------------------------
				//
				// *** THE CHECK THAT WAS MISSING, AND ITS ABSENCE IS WHY A STALE VIOLET LIVED HERE FOR
				// A WHOLE WAVE. *** Everything above measures SHAPE, MOTION, COUNT and AUDIO, all of
				// which were correct while the halo and both slip-streams drew in the pre-re-space
				// violet — 13.2 degrees from the one on her body, which a frame cannot adjudicate
				// because a wrong violet still looks like violet.
				//
				// The instrument is the shipped path in both directions: the LEFT side is read back off
				// the live MID the renderer was handed (DebugPieceHue), and the RIGHT side is
				// TraceCharacterRoster's row — the same table the body material is stamped from. So
				// "these are equal" is literally "her halo and her body are the same colour".
				{
					FLinearColor Accent = FLinearColor::White;
					if (const TraceCharacterRoster::FTraceCharacterEntry* Row =
						TraceCharacterRoster::Find(static_cast<uint8>(ETraceCharacterId::Mace)))
					{
						Accent = FLinearColor(Row->Accent.R, Row->Accent.G, Row->Accent.B, 1.f);
					}

					auto SameHue = [&Accent](const FLinearColor& C)
					{
						// 1e-3 rather than 1e-4: the additive intensity is divided back out of the
						// stored colour, so one float division of round-off is expected and 0.001 is
						// still two orders of magnitude tighter than the 13.2-degree bug.
						return FMath::IsNearlyEqual(C.R, Accent.R, 1e-3f)
							&& FMath::IsNearlyEqual(C.G, Accent.G, 1e-3f)
							&& FMath::IsNearlyEqual(C.B, Accent.B, 1e-3f);
					};

					UE_LOG(LogTraceGame, Display,
						TEXT("[MACEFX] Mace's LIVE roster accent: (%.3f, %.3f, %.3f)"),
						Accent.R, Accent.G, Accent.B);

					State->Check(State->bHaloHueRead && SameHue(State->HaloHue),
						FString::Printf(TEXT("*** the SUSPEND HALO's hue IS Mace's live roster accent — "
						                     "read off the MID (%.3f,%.3f,%.3f) vs roster (%.3f,%.3f,%.3f) ***"),
							State->HaloHue.R, State->HaloHue.G, State->HaloHue.B,
							Accent.R, Accent.G, Accent.B));
					State->Check(State->bStreamHueRead && SameHue(State->StreamAHue) && SameHue(State->StreamBHue),
						FString::Printf(TEXT("*** BOTH PULL SLIP-STREAMS' hue IS that same accent — "
						                     "A (%.3f,%.3f,%.3f) B (%.3f,%.3f,%.3f) ***"),
							State->StreamAHue.R, State->StreamAHue.G, State->StreamAHue.B,
							State->StreamBHue.R, State->StreamBHue.G, State->StreamBHue.B));
					State->Check(State->bHaloHueRead && State->bStreamHueRead
							&& SameHue(State->HaloHue) && SameHue(State->StreamAHue),
						TEXT("*** and therefore the halo, the streams, the spike cone, the rope and the "
						     "embed burst are ONE hue — bible §6.2, one hue per kit ***"));
				}

				State->Check(FMath::Max3(State->PullPieces, State->HaloPieces, Kit->DebugAttachedFxCount())
						<= TraceFxLoopBudget::MaxPrimitivesPerPawn,
					FString::Printf(TEXT("never over the §1.4 budget: the most this kit ever had attached at once "
					                     "was %d of %d"),
						FMath::Max3(State->PullPieces, State->HaloPieces, Kit->DebugAttachedFxCount()),
						TraceFxLoopBudget::MaxPrimitivesPerPawn));

				UE_LOG(LogTraceGame, Display, TEXT("[MACEFX] ===== %d passed, %d failed, %d frame(s) ====="),
					State->Passed, State->Failed, State->Shots);
				if (State->Failed == 0)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[MACEFX] VERDICT: PASS"));
				}
				else
				{
					UE_LOG(LogTraceGame, Error, TEXT("[MACEFX] VERDICT: *** FAIL *** (%d)"), State->Failed);
				}
				return false;
			}

			default:
				return false;
			}

			return true;
		}), 0.05f);
	}

// =================================================================================================
// Trace.Mace.RopeLadder — THE HUE HEADROOM, MEASURED OFF FRAMES INSTEAD OF ARGUED ABOUT
//
// FX §2.4 asks for the rope core at Glow 2.6 and the spike cone at 3.0. Mace's violet is
// (0.74, 0.55, 0.99) — read live from the roster, see MaceAccent() at the top of this file — so its
// brightest channel is 0.99 and 2.6x it is 2.57: every channel is asked to clip and the tonemapper
// hands back white. That is what ATraceElleGate measured for ITS purple (3.5 -> pink-white, 1.0 ->
// purple) and what ATraceFxBurst's four-rung ladder measured again for every accent in the game.
//
// The headroom conclusion is stated as a MULTIPLE of the brightest channel precisely so that it
// survives an accent re-space: the pre-W6 violet was (0.65, 0.55, 1.00), brightest 1.00, and the
// live one is brightest 0.99 — a 1% shift in where the clip starts, not a new experiment.
//
// Rather than inherit either answer, this shoots Mace's OWN ladder: four spikes at four headroom
// values, thrown at the same anchor from the same place, each photographed side-on so the rope fills
// the frame. The value that ships is whichever of those four frames actually looks violet.
//
// It uses DebugThrowSpikeAt rather than the E key on purpose — the ladder is about the DRESSING, and
// four real casts would spend four cooldowns and four MaceSpikeThrow plays for nothing.
// =================================================================================================

	void RunMaceRopeLadder()
	{
		UWorld* WorldPtr = FindAuthoritativeWorldForMaceV();
		UTraceAbilitySetMace* Mace = (WorldPtr != nullptr) ? MakeLocalPlayerIntoMace(WorldPtr) : nullptr;
		if (Mace == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[MACELADDER] no authoritative world or no ability component."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[MACELADDER] ===== four headroom rungs, one frame each, same anchor and same camera ====="));

		TSharedRef<FMaceFxState> State = MakeShared<FMaceFxState>();
		State->Mace = Mace;
		State->StageStartReal = FPlatformTime::Seconds();
		TWeakObjectPtr<UWorld> WeakWorld(WorldPtr);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld](float) -> bool
		{
			static const float Rungs[] = { 0.50f, 0.80f, 1.40f, 2.60f };
			constexpr int32 RungCount = UE_ARRAY_COUNT(Rungs);

			UWorld* TickWorld = WeakWorld.Get();
			UTraceAbilitySetMace* Kit = State->Mace.Get();
			ATraceCharacter* Pawn = (Kit != nullptr) ? Kit->GetCharacter() : nullptr;
			if (TickWorld == nullptr || Kit == nullptr || Pawn == nullptr)
			{
				return true;
			}

			// Stage 0: find the wall once, and put the camera BESIDE the rope rather than behind it —
			// a rope photographed end-on is a dot.
			if (State->Stage == 0)
			{
				if (State->In() < 1.0)
				{
					return true;
				}
				State->bFoundWall = AimAtAWall(Kit, Pawn, TickWorld, State->Anchor);
				if (!State->bFoundWall)
				{
					UE_LOG(LogTraceGame, Error, TEXT("[MACELADDER] no wall to throw at."));
					return false;
				}

				const FVector From = Pawn->GetActorLocation();
				const FVector Mid = 0.5f * (From + State->Anchor);
				const FVector Along = (State->Anchor - From).GetSafeNormal();
				const FVector Side = FVector::CrossProduct(Along, FVector::UpVector).GetSafeNormal();
				const float Back = FMath::Clamp(FVector::Dist(From, State->Anchor) * 0.55f, 260.f, 900.f);
				State->Observer = PlaceObserver(TickWorld, Mid + Side * Back + FVector(0.f, 0.f, 120.f), Mid);
				State->Advance(1);
				return true;
			}

			const int32 Rung = (State->Stage - 1) / 2;
			if (Rung >= RungCount)
			{
				ReleaseObserver(TickWorld, State->Observer.Get());
				UE_LOG(LogTraceGame, Display,
					TEXT("[MACELADDER] ===== %d rung(s) photographed. Judge the FRAMES: the rung that still "
					     "reads VIOLET at the highest brightness is the one to ship in "
					     "TraceMaceSpikeFile::EmissiveHueHeadroom. ====="), RungCount);
				return false;
			}

			const bool bThrowPhase = ((State->Stage - 1) % 2) == 0;
			if (bThrowPhase)
			{
				// The headroom is LATCHED when a spike builds, so it has to be set BEFORE the throw —
				// and each rung needs its own spike for the same reason.
				if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mace.VioletHeadroom")))
				{
					CVar->Set(Rungs[Rung], ECVF_SetByConsole);
				}
				Kit->DebugThrowSpikeAt(State->Anchor, UTraceSettings::Get().MaceSpikeTravelSpeed);
				State->Advance(State->Stage + 1);
				return true;
			}

			if (State->In() < 0.45)
			{
				return true;
			}

			const ATraceMaceSpike* Spike = Kit->GetSpike();
			UE_LOG(LogTraceGame, Display, TEXT("[MACELADDER] headroom %.2f -> %s"),
				Rungs[Rung], Spike != nullptr ? *Spike->DebugDescribe() : TEXT("no spike"));
			++State->Shots;
			const FString Path = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir() / TEXT("Screenshots")
				/ FString::Printf(TEXT("W4KitsB_Ladder_%02d_headroom%03d.png"),
					State->Shots, FMath::RoundToInt(Rungs[Rung] * 100.f)));
			FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
			UE_LOG(LogTraceGame, Display, TEXT("[MACELADDER] Screenshot requested: %s"), *Path);

			State->Advance(State->Stage + 1);
			return true;
		}), 0.05f);
	}

	FAutoConsoleCommand CmdMaceRopeLadder(
		TEXT("Trace.Mace.RopeLadder"),
		TEXT("Four spikes at four Trace.Mace.VioletHeadroom values, same anchor, same side-on camera, one "
		     "frame each — so the rope's hue is chosen from photographs rather than from the recipe's "
		     "number. Drives the local pawn and writes FIXED filenames; take the capture lock."),
		FConsoleCommandDelegate::CreateStatic(&RunMaceRopeLadder));

	FAutoConsoleCommand CmdMaceFxTest(
		TEXT("Trace.Mace.FxTest"),
		TEXT("FX §2.4, staged and photographed: throws a real spike at a real wall, reads the spike's three "
		     "dressed pieces back off the live components, counts the SpikeEmbed burst, then pulls and "
		     "suspends and measures the attached slip-stream, halo and MacePullLoop. Drives the local pawn "
		     "and writes FIXED filenames — take the capture lock, and do not batch it with another driving test."),
		FConsoleCommandDelegate::CreateStatic(&RunMaceFxTest));
}

#endif   // !UE_BUILD_SHIPPING
