// Trace — X (spec v14 §6). Read TraceAbilitySetX.h first; it is the design document.

#include "Abilities/Characters/TraceAbilitySetX.h"

#include "Abilities/Characters/TraceAbilityWeaponHooks.h"
#include "Abilities/TraceAbilityComponent.h"

#include "Audio/TraceAudio.h"               // FX_AUDIO_PLAN §2.7: XStingLoad
#include "Audio/TraceSoundEvents.h"
#include "Camera/CameraActor.h"             // the client probe's local observer
#include "Misc/Paths.h"
#include "UnrealClient.h"                   // FScreenshotRequest — the client probe photographs
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"   // §2.7's five instanced swarm pieces
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
#include "Gameplay/TraceFxBurst.h"          // §2.7's BeeSting spark, at the contact
#include "Gameplay/TraceFxShapes.h"          // the five swarm pieces are built through the library
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceWeaponComponent.h"   // spec v16 §1: Sting replaces the CLIP now
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "UObject/ConstructorHelpers.h"

// *** THE RED ARM FOR §2.7's STING-LOAD CONVERGE. ***
//
// 0 reproduces the shipped-before-this-wave behaviour exactly: the swarm hides on the same frame the
// bees are loaded, with no flight. That is the build Trace.X.BeeFxTest has to be shown FAILING on,
// because "the bees fly to the gun" is otherwise a claim about an animation nobody measured.
//
// 1 (shipped): they lerp into the muzzle over TraceXBeeFx::ConvergeSeconds and then hide.
static TAutoConsoleVariable<int32> CVarXBeeConverge(
	TEXT("Trace.X.BeeConverge"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): loading Sting flies the bees into the gun over 0.3 s before hiding "
	     "them — FX plan §2.7. 0: they vanish instantly, the pre-§2.7 behaviour, so Trace.X.BeeFxTest can "
	     "be shown FAILING. Never ship 0."),
	ECVF_Cheat);

// =================================================================================================
// FX_AUDIO_PLAN §2.7 — THE BEE POLISH'S NUMBERS. Named, not anonymous: this module builds as a unity
// blob and Scripts/check-jumbo-build-collisions.py gates on exactly that.
// =================================================================================================
namespace TraceXBeeFx
{
	/**
	 * BeeRounds amber, linear (1.00, 0.78, 0.10) — FX_AUDIO_PLAN §2's palette, and the SAME triple
	 * UTraceWeaponComponent::GetTracerTintOverride returns for a loaded bee clip. That agreement is
	 * the point: the bees, their trails and the round they turn into are one colour, so a player who
	 * has seen the swarm recognises the tracer coming at them.
	 *
	 * The swarm used to be (1.00, 0.72, 0.12), a near-miss of the same amber invented separately.
	 */
	const FLinearColor BeeAmber(1.00f, 0.78f, 0.10f, 1.f);

	/** The shipped bee size: 0.16 of a 100 uu engine sphere = 16 uu across, as §2.7 records it. */
	constexpr float CoreScale = 0.16f;

	/**
	 * How hard the bee cores glow. Emissive, so this rides the Glow scalar and is bounded by the
	 * bible's 4.2 transient ceiling — 2.6 is the trail-glow family §2.4 names for Mace's rope core,
	 * which is the same job: a small hot element that has to stay legible across the arena.
	 */
	constexpr float CoreGlow = 2.6f;

	/**
	 * *** DEMO 29 ITEM 5 — "REVERT X'S BEES TO THE OLD MODEL." ***
	 *
	 * false (shipped) is the model the swarm had before the overhaul: N plain spheres, 16 uu across,
	 * in M_TraceNeon at LegacyBeeAmber, and NOTHING ELSE around them. The two pieces §2.7 added on
	 * top of that model — the additive halo sleeve and the three-segment tangent trail — are not
	 * built at all, so they cost nothing and draw nothing.
	 *
	 * true restores §2.7's five-piece swarm exactly as it shipped in the overhaul. Everything it
	 * needs is still in this file: the halo and trail construction, their placement, their MIDs and
	 * their measurements are all still here, still compiled and still measured by Trace.X.BeeFxTest,
	 * which reads this gate and checks whichever model is actually on screen.
	 *
	 * WHAT IS *NOT* GATED, deliberately: the orbit arithmetic, the contact test and §2.7's 0.3 s
	 * sting-load converge. None of those is "the model" — the orbit is where the bees sting from and
	 * the converge is an animation of the same spheres — and the owner asked for the model.
	 */
	constexpr bool bBeePolish = false;

	/**
	 * THE OLD BEES' AMBER, restored with the old model: linear (1.00, 0.72, 0.12), set on both
	 * "Color" and "BaseColor" because M_TraceNeon and the engine's BasicShapeMaterial name it
	 * differently. BeeAmber above is the overhaul's slightly different amber and is what the halo,
	 * the trail and the bee-round tracer use; with the gate off, none of those three is drawn, so
	 * there is nothing for this to disagree with.
	 */
	const FLinearColor LegacyBeeAmber(1.00f, 0.72f, 0.12f, 1.f);

	/** §2.7: "additive halo sleeve x1.8 scale ... I 0.3". */
	constexpr float HaloScaleMultiple = 1.8f;
	constexpr float HaloIntensity = 0.3f;

	/**
	 * §2.7: "one trailing cylinder (l 60 uu, r 3 uu)". THE RADIUS IS DRAWN AT 4 uu, not 3.
	 *
	 * Sub-8 uu emissive dissolves into dashes under TSR (bible §3.4), which is the floor
	 * ATraceFxBurst::MinEmissiveRadiusUU enforces on every spark in the game for the same reason.
	 * Written as a Max against that constant rather than as the number 4, so the day the floor moves
	 * the trail moves with it.
	 */
	constexpr float TrailLengthUU = 60.f;
	const float TrailRadiusUU = FMath::Max(3.f, ATraceFxBurst::MinEmissiveRadiusUU);

	/**
	 * §2.7: "I fading 0.4 -> 0 over the trail", as three stacked segments. 0.40 / 0.24 / 0.10 is a
	 * geometric-ish fall rather than a linear one because additive brightness reads perceptually
	 * closer to logarithmic — an even 0.40 / 0.27 / 0.13 photographs as a trail with a visible step
	 * at the far end instead of one that vanishes.
	 */
	inline float TrailIntensityAt(int32 SegmentIndex)
	{
		static const float Intensities[3] = { 0.40f, 0.24f, 0.10f };
		return Intensities[FMath::Clamp(SegmentIndex, 0, 2)];
	}

	/**
	 * How far back on the orbit the TANGENT is sampled, in seconds of match clock.
	 *
	 * Sampled rather than differentiated: one extra call to the same GetBeeLocation() the whole
	 * feature is built on, which stays correct if the orbit's shape is ever changed, where a
	 * hand-written derivative would silently keep pointing at the old curve.
	 */
	constexpr float TangentSampleSeconds = 0.02f;

	/** §2.7: "bees lerp to the gun over 0.3 s, then hide". */
	constexpr float ConvergeSeconds = 0.3f;
}

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

		// FX_AUDIO_PLAN §2.7: XStingLoad, World-side, once, from the authority. TraceAudio::PlayAt is
		// the ordinary table-driven multicast — the bees going into the gun is a fact about a player
		// everybody nearby is about to be shot by, and the sound is what tells them before the first
		// amber tracer does.
		if (const ATraceCharacter* SoundPawn = GetCharacter())
		{
			TraceAudio::PlayAt(this, TraceSoundEvents::XStingLoad, SoundPawn->GetActorLocation());
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

					// --- FX_AUDIO_PLAN §2.7's STING CONTACT SPARK ------------------------------
					//
					// At the CONTACT POINT on the victim's capsule axis, facing back at the bee, so
					// the four sparks spray off the body rather than through it. The burst carries
					// XSting with it (PlayReplicatedLocal, from its own BeginPlay on every machine),
					// which is why there is no TraceAudio call on this line.
					//
					// *** ONLY THE BODY STING SPARKS, AND THE STING BULLET DELIBERATELY DOES NOT. ***
					// NotifyBulletHit marks through this same MarkVulnerable(), so it would have been
					// one line to spark there too. It must not: a bee ROUND already has a full
					// presentation — an amber tracer down its whole flight (§2.7's tint override) and
					// the shooter's hitmarker — and adding a flash ON THE VICTIM'S BODY is precisely
					// the on-body impact sphere the deleted-sphere ruling removed from this game
					// (TraceTracer.h:300-310). The passive sting has no other presentation at all,
					// which is exactly why it gets one.
					//
					// It cannot spam: the RefreshBelow gate above refuses to re-mark a target until
					// its mark has decayed past 10%, so this is at most one burst per target per
					// 1.8 s even for somebody standing inside the swarm.
					ATraceFxBurst::Burst(CurrentWorld, ETraceFxBurstType::BeeSting, Closest,
						(BeeLocation - Closest).GetSafeNormal(1.e-4f, FVector::UpVector));
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
	// still taking the mark's extra damage (+35% since spec v24 §9) would be a 2 s mystery nobody
	// could explain.
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

	// THE 20 Hz BELT to the §1.2 router edge below. Animating (bAnimate = true) because if the edge
	// was missed — a machine that built its swarm the frame after the state arrived — this is the
	// only thing that will start the converge, and a converge that plays 50 ms late is better than
	// bees that vanish. SetSwarmLoaded is idempotent, so calling it forty times a second with the
	// same answer neither restarts the animation nor stacks anything.
	Existing->SetSwarmLoaded(IsStingLoaded(), /*bAnimate*/ true);
}

// =================================================================================================
// FX_AUDIO_PLAN §1.2's router — X's one use of it is §2.7's sting-load converge
// =================================================================================================

void UTraceAbilitySetX::OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New)
{
	const bool bWasLoaded = (Old.Flags & TraceAbilityFlags::AuxActive) != 0;
	const bool bIsLoaded = (New.Flags & TraceAbilityFlags::AuxActive) != 0;
	if (bWasLoaded == bIsLoaded)
	{
		// Stacks moves once per bee round fired and is in the same struct. It changes NOTHING the
		// swarm draws — a loaded swarm is hidden whether four bees are left or one — so reacting to
		// it would be four no-op calls per Sting.
		return;
	}

	if (ATraceBeeSwarm* Existing = Swarm.Get())
	{
		// ANIMATED: this is the real edge, on the frame the state actually changed, which is the
		// only place §2.7's 0.3 s flight can start without being late.
		Existing->SetSwarmLoaded(bIsLoaded, /*bAnimate*/ true);
	}
}

void UTraceAbilitySetX::SyncClientFx(const FTraceAbilityNetState& Current)
{
	// FIRST SIGHT: a client that joined, swapped character or respawned into a state where Sting is
	// ALREADY loaded. SNAPPED, not animated — the bees flew into the gun before this machine was
	// watching, and replaying that flight now would be showing a past event as a present one.
	//
	// The swarm may not exist yet on this machine (the set is built before the pawn is resolved);
	// UpdateSwarmActor's poll covers that case one tick later and snaps to the same answer, because
	// by then Phase is still Orbiting and bAnimate has nothing to animate away from.
	if (ATraceBeeSwarm* Existing = Swarm.Get())
	{
		Existing->SetSwarmLoaded((Current.Flags & TraceAbilityFlags::AuxActive) != 0, /*bAnimate*/ false);
	}
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

	// Constructor-time finders, the policy TraceCore states: this is what leaves a COOK REFERENCE,
	// where a runtime LoadObject of the same path resolves to null in a packaged build.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		BeeMesh = SphereFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		TrailMesh = CylinderFinder.Object;
	}

	// *** THE OLD BEE MATERIAL IS FOUND HERE AGAIN — DEMO 29 ITEM 5. *** These two finders are the
	// pair the swarm carried before the overhaul: M_TraceNeon, with the engine's BasicShapeMaterial
	// as the fallback, giving every bee one opaque MID with its colour set. The overhaul removed them
	// because §2.7's halo and trail are ADDITIVE by specification and needed UTraceFxShapes'
	// blend ladder — which the halo and the trail still use. The cores use these again while
	// TraceXBeeFx::bBeePolish is false, because "the old model" is this material as much as it is
	// this sphere.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		LegacyBeeMaterial = NeonFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (LegacyBeeMaterial == nullptr && BasicFinder.Succeeded())
	{
		LegacyBeeMaterial = BasicFinder.Object;
	}
}

void ATraceBeeSwarm::EnsureBeeInstances(int32 DesiredCount)
{
	USceneComponent* const SwarmRoot = GetRootComponent();
	if (SwarmRoot == nullptr || DesiredCount <= 0)
	{
		return;
	}

	// ---- build the pieces, once (five with the polish gate on, one with it off) -----------------
	if (Cores == nullptr && BeeMesh != nullptr)
	{
		Cores = NewObject<UInstancedStaticMeshComponent>(this, TEXT("BeeCores"));
		if (Cores != nullptr)
		{
			Cores->SetupAttachment(SwarmRoot);
			Cores->SetStaticMesh(BeeMesh);
			UTraceFxShapes::ConfigureFxComponent(Cores);
			Cores->RegisterComponent();

			if (TraceXBeeFx::bBeePolish)
			{
				// EMISSIVE, not additive, and that is §2.7's look: the bees are the small, thin, hot
				// element of the effect and ATraceTracer's rule is that thin hot pieces are emissive
				// (they can push a hue past a lit background, which additive — clamped at 1.0 and
				// able only to ADD — cannot). The halo around them is the big soft volume, and it is
				// additive for the mirror-image reason.
				CoreMID = UTraceFxShapes::MakeGlowMID(Cores, 0, ETraceFxBlend::Emissive, CoreBlend);
				UTraceFxShapes::SetGlow(CoreMID, CoreBlend, TraceXBeeFx::BeeAmber, TraceXBeeFx::CoreGlow);
			}
			else
			{
				// THE OLD MODEL (Demo 29 item 5): one opaque MID on M_TraceNeon with the colour set
				// and nothing else — no Glow scalar, no blend ladder. Reported as Emissive rather
				// than None because that is what M_TraceNeon IS and because CoreBlend is what
				// SetPiecesVisible and DebugDescribe read; a bee that reported None would be hidden.
				// If the material is missing entirely the blend stays None and the ladder ends where
				// it always did — at "no effect", never at a grey primitive orbiting a player's head.
				if (LegacyBeeMaterial != nullptr)
				{
					CoreMID = Cores->CreateDynamicMaterialInstance(0, LegacyBeeMaterial);
					if (CoreMID != nullptr)
					{
						CoreMID->SetVectorParameterValue(TEXT("Color"), TraceXBeeFx::LegacyBeeAmber);
						CoreMID->SetVectorParameterValue(TEXT("BaseColor"), TraceXBeeFx::LegacyBeeAmber);
						CoreBlend = ETraceFxBlend::Emissive;
					}
				}
			}

			if (CoreBlend == ETraceFxBlend::None)
			{
				Cores->SetVisibility(false, true);
			}
		}
	}

	// *** DEMO 29 ITEM 5: THE HALO AND THE TRAIL ARE NOT BUILT AT ALL WHILE THE GATE IS OFF. ***
	// Not built rather than built-and-hidden: an unbuilt component costs no instances, no MID, no
	// render state and no per-frame transform write, and PlaceBees/SetPiecesVisible/DebugDescribe all
	// already handle a null piece because a missing material could always produce one.
	if (TraceXBeeFx::bBeePolish && Halos == nullptr && BeeMesh != nullptr)
	{
		Halos = NewObject<UInstancedStaticMeshComponent>(this, TEXT("BeeHalos"));
		if (Halos != nullptr)
		{
			Halos->SetupAttachment(SwarmRoot);
			Halos->SetStaticMesh(BeeMesh);
			UTraceFxShapes::ConfigureFxComponent(Halos);
			Halos->RegisterComponent();

			HaloMID = UTraceFxShapes::MakeGlowMID(Halos, 0, ETraceFxBlend::Additive, HaloBlend);
			UTraceFxShapes::SetGlow(HaloMID, HaloBlend, TraceXBeeFx::BeeAmber, TraceXBeeFx::HaloIntensity);
			if (HaloBlend == ETraceFxBlend::None)
			{
				Halos->SetVisibility(false, true);
			}
		}
	}

	if (TraceXBeeFx::bBeePolish && TrailParts[0] == nullptr && TrailMesh != nullptr)
	{
		for (int32 Part = 0; Part < UE_ARRAY_COUNT(TrailParts); ++Part)
		{
			UInstancedStaticMeshComponent* Segment = NewObject<UInstancedStaticMeshComponent>(this,
				*FString::Printf(TEXT("BeeTrail%d"), Part));
			if (Segment == nullptr)
			{
				continue;
			}

			Segment->SetupAttachment(SwarmRoot);
			Segment->SetStaticMesh(TrailMesh);
			UTraceFxShapes::ConfigureFxComponent(Segment);
			Segment->RegisterComponent();

			ETraceFxBlend Achieved = ETraceFxBlend::None;
			TrailMIDs[Part] = UTraceFxShapes::MakeGlowMID(Segment, 0, ETraceFxBlend::Additive, Achieved);
			if (Part == 0)
			{
				TrailBlend = Achieved;
			}

			// THE FADE IS THE THREE INTENSITIES, and this is the line that makes it real. §2.7 asks
			// for "I fading 0.4 -> 0 over the trail"; one component would have one MID and therefore
			// one brightness, which is a streak. Three give 0.40 / 0.24 / 0.10 from the bee outwards.
			UTraceFxShapes::SetGlow(TrailMIDs[Part], Achieved, TraceXBeeFx::BeeAmber,
				TraceXBeeFx::TrailIntensityAt(Part));

			if (Achieved == ETraceFxBlend::None)
			{
				Segment->SetVisibility(false, true);
			}

			TrailParts[Part] = Segment;
		}
	}

	// ---- match the instance COUNT to the live knob ----------------------------------------------
	//
	// Rebuilt rather than adjusted: XBeeCount is a live settings knob and changing it is rare (a
	// designer retuning during PIE), so clearing and re-adding N instances at that moment is simpler
	// than reconciling two lists, and the transforms are all written the same frame anyway.
	const int32 WantCores = DesiredCount;
	const int32 WantTrail = DesiredCount;

	if (Cores != nullptr && Cores->GetInstanceCount() != WantCores)
	{
		Cores->ClearInstances();
		for (int32 Index = 0; Index < WantCores; ++Index)
		{
			Cores->AddInstance(FTransform::Identity, /*bWorldSpace*/ true);
		}
	}
	if (Halos != nullptr && Halos->GetInstanceCount() != WantCores)
	{
		Halos->ClearInstances();
		for (int32 Index = 0; Index < WantCores; ++Index)
		{
			Halos->AddInstance(FTransform::Identity, /*bWorldSpace*/ true);
		}
	}
	for (int32 Part = 0; Part < UE_ARRAY_COUNT(TrailParts); ++Part)
	{
		UInstancedStaticMeshComponent* Segment = TrailParts[Part];
		if (Segment != nullptr && Segment->GetInstanceCount() != WantTrail)
		{
			Segment->ClearInstances();
			for (int32 Index = 0; Index < WantTrail; ++Index)
			{
				Segment->AddInstance(FTransform::Identity, /*bWorldSpace*/ true);
			}
		}
	}
}

void ATraceBeeSwarm::SetPiecesVisible(bool bInVisible)
{
	// A piece whose blend is None stays hidden whatever anybody asks for: the degradation ladder
	// ends at "no effect", never at "a grey primitive orbiting a player's head".
	if (Cores != nullptr)
	{
		Cores->SetVisibility(bInVisible && CoreBlend != ETraceFxBlend::None, true);
	}
	if (Halos != nullptr)
	{
		Halos->SetVisibility(bInVisible && HaloBlend != ETraceFxBlend::None, true);
	}
	for (UInstancedStaticMeshComponent* Segment : TrailParts)
	{
		if (Segment != nullptr)
		{
			Segment->SetVisibility(bInVisible && TrailBlend != ETraceFxBlend::None, true);
		}
	}
}

void ATraceBeeSwarm::SetSwarmLoaded(bool bLoaded, bool bAnimate)
{
	if (!bLoaded)
	{
		// "When all five are fired the bees resume orbiting." There is no animation on the way BACK
		// and §2.7 does not ask for one: the bees reappearing on their orbit is the signal that X's
		// body is dangerous again, and a 0.3 s fly-out would delay that signal by 0.3 s.
		Phase = EPhase::Orbiting;
		SetPiecesVisible(true);
		return;
	}

	if (Phase != EPhase::Orbiting)
	{
		// ALREADY CONVERGING OR ALREADY LOADED. This is what makes the 20 Hz poll safe to call
		// beside the router edge: without it, every poll would restart the converge at alpha 0 and
		// the bees would sit at their orbit radius for as long as Sting was loaded.
		return;
	}

	// Either the caller asked for a snap (first sight), or the RED ARM is on and this build is the
	// one where the swarm simply disappeared. ONE branch, so the two cannot drift — and the arm is
	// compiled out of Shipping entirely, so the shipped answer is `!bAnimate` and nothing else.
	bool bSnapWithoutFlying = !bAnimate;
#if !UE_BUILD_SHIPPING
	bSnapWithoutFlying = bSnapWithoutFlying || (CVarXBeeConverge.GetValueOnAnyThread() == 0);
#endif
	if (bSnapWithoutFlying)
	{
		Phase = EPhase::Loaded;
		SetPiecesVisible(false);
		return;
	}

	Phase = EPhase::Converging;
	ConvergeStartWorldTime = (GetWorld() != nullptr) ? GetWorld()->GetTimeSeconds() : 0.f;
}

void ATraceBeeSwarm::PlaceBees(const FVector& Centre, float MatchTimeSeconds, int32 BeeCount,
                               float ConvergeAlpha)
{
	const float OrbitRadius = TraceXBees::GetOrbitRadiusUU();
	const float OrbitSpeed = TraceXBees::GetOrbitSpeedDegreesPerSecond();
	const float CoreScale = TraceXBeeFx::CoreScale;
	const float BeeRadiusUU = UTraceFxShapes::BasicShapeExtentUU * CoreScale * 0.5f;

	// WHERE THE BEES ARE CONVERGING TO. Asked of the pawn rather than guessed: GetMuzzleLocation()
	// is the point every shot in this game leaves from, so the bees end their flight exactly where
	// the bee ROUNDS will come out. A null host has already been handled by the caller.
	const ATraceCharacter* HostCharacter = Host.Get();
	const FVector Muzzle = (HostCharacter != nullptr) ? HostCharacter->GetMuzzleLocation() : Centre;

	for (int32 BeeIndex = 0; BeeIndex < BeeCount; ++BeeIndex)
	{
		// *** THE ORBIT IS STILL THE ONE FUNCTION THE SERVER'S CONTACT TEST CALLS. *** Nothing in
		// §2.7's polish is allowed to move a bee off the position UTraceAbilitySetX::SweepBeeContacts
		// computes for the same index at the same match time — the drawn volume IS the lethal one.
		// The converge below is the single exception and it is safe, because a converging swarm is a
		// LOADED swarm and SweepBeeContacts refuses to run at all while Sting is loaded.
		const FVector OrbitPosition = TraceXBees::GetBeeLocation(Centre, MatchTimeSeconds, BeeIndex,
			BeeCount, OrbitRadius, OrbitSpeed);
		const FVector BeePosition = FMath::Lerp(OrbitPosition, Muzzle, ConvergeAlpha);

		// The instantaneous orbit TANGENT, sampled a hair back on the same function rather than
		// derived — one extra evaluation, no calculus, and it stays correct if the orbit's shape ever
		// changes. During the converge it becomes the flight direction for free, because the sample
		// is lerped by the same alpha.
		const FVector PreviousOrbit = TraceXBees::GetBeeLocation(Centre,
			MatchTimeSeconds - TraceXBeeFx::TangentSampleSeconds, BeeIndex, BeeCount, OrbitRadius, OrbitSpeed);
		const FVector PreviousPosition = FMath::Lerp(PreviousOrbit, Muzzle, ConvergeAlpha);

		FVector Backwards = (PreviousPosition - BeePosition).GetSafeNormal();
		if (Backwards.IsNearlyZero())
		{
			Backwards = -FVector::ForwardVector;
		}

		if (Cores != nullptr && Cores->GetInstanceCount() > BeeIndex)
		{
			Cores->UpdateInstanceTransform(BeeIndex,
				FTransform(FQuat::Identity, BeePosition, FVector(CoreScale)),
				/*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
		}

		if (Halos != nullptr && Halos->GetInstanceCount() > BeeIndex)
		{
			// §2.7: "additive halo sleeve x1.8 scale". Concentric with the core, so the bee reads as
			// a hot dot inside a soft glow rather than as two objects.
			Halos->UpdateInstanceTransform(BeeIndex,
				FTransform(FQuat::Identity, BeePosition, FVector(CoreScale * TraceXBeeFx::HaloScaleMultiple)),
				/*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
		}

		// ---- the trail: three segments laid back along the tangent -------------------------------
		const float SegmentLength = TraceXBeeFx::TrailLengthUU / static_cast<float>(UE_ARRAY_COUNT(TrailParts));
		for (int32 Part = 0; Part < UE_ARRAY_COUNT(TrailParts); ++Part)
		{
			UInstancedStaticMeshComponent* Segment = TrailParts[Part];
			if (Segment == nullptr || Segment->GetInstanceCount() <= BeeIndex)
			{
				continue;
			}

			// The segment starts one bee radius behind the sphere so the trail emerges from the bee
			// rather than through it, and each one picks up where the last left off.
			const float NearDistance = BeeRadiusUU + SegmentLength * static_cast<float>(Part);
			const FVector From = BeePosition + Backwards * NearDistance;
			const FVector To = From + Backwards * SegmentLength;
			const FVector Middle = (From + To) * 0.5f;

			Segment->UpdateInstanceTransform(BeeIndex,
				FTransform(FRotationMatrix::MakeFromZ(Backwards).ToQuat(), Middle,
					FVector(UTraceFxShapes::ShapeScaleForRadiusUU(TraceXBeeFx::TrailRadiusUU),
					        UTraceFxShapes::ShapeScaleForRadiusUU(TraceXBeeFx::TrailRadiusUU),
					        UTraceFxShapes::ShapeScaleForLengthUU(SegmentLength))),
				/*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
		}
	}

	// ONE render-state flush per component per frame instead of one per instance. Five components at
	// five instances each is twenty-five transforms a frame on a local cosmetic actor; marking the
	// state dirty inside the loop would be twenty-five flushes for the same picture.
	if (Cores != nullptr)  { Cores->MarkRenderStateDirty(); }
	if (Halos != nullptr)  { Halos->MarkRenderStateDirty(); }
	for (UInstancedStaticMeshComponent* Segment : TrailParts)
	{
		if (Segment != nullptr) { Segment->MarkRenderStateDirty(); }
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
	EnsureBeeInstances(BeeCount);

	// ---- §2.7's converge: 0.3 s from the orbit into the muzzle, then hide -----------------------
	float ConvergeAlpha = 0.f;
	if (Phase == EPhase::Converging)
	{
		const UWorld* SwarmWorld = GetWorld();
		const float Elapsed = (SwarmWorld != nullptr)
			? (SwarmWorld->GetTimeSeconds() - ConvergeStartWorldTime)
			: TraceXBeeFx::ConvergeSeconds;

		ConvergeAlpha = FMath::Clamp(Elapsed / TraceXBeeFx::ConvergeSeconds, 0.f, 1.f);
		if (ConvergeAlpha >= 1.f)
		{
			// ARRIVED. The bees are in the gun; this is where the shipped "hide instantly" behaviour
			// finally happens, 0.3 s after the state change instead of on the same frame as it.
			Phase = EPhase::Loaded;
			SetPiecesVisible(false);
			return;
		}
	}
	else if (Phase == EPhase::Loaded)
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
	SetActorLocation(Centre);

	PlaceBees(Centre, Now, BeeCount, ConvergeAlpha);
}

FString ATraceBeeSwarm::DebugDescribe() const
{
	// READ BACK OFF THE LIVE COMPONENTS. Nothing below re-derives a number from TraceXBeeFx.
	const int32 CoreCount = (Cores != nullptr) ? Cores->GetInstanceCount() : 0;
	const int32 HaloCount = (Halos != nullptr) ? Halos->GetInstanceCount() : 0;

	int32 Pieces = 0;
	int32 Visible = 0;
	auto Count = [&Pieces, &Visible](const UInstancedStaticMeshComponent* Piece)
	{
		if (Piece != nullptr)
		{
			++Pieces;
			Visible += Piece->IsVisible() ? 1 : 0;
		}
	};
	Count(Cores);
	Count(Halos);
	for (const UInstancedStaticMeshComponent* Segment : TrailParts)
	{
		Count(Segment);
	}

	// The halo's scale AS A MULTIPLE OF THE CORE'S, both taken off instance 0's transform — which is
	// the only honest way to check "x1.8 scale", since either one alone could be wrong.
	float HaloMultiple = 0.f;
	FTransform CoreXf;
	FTransform HaloXf;
	if (Cores != nullptr && CoreCount > 0 && Cores->GetInstanceTransform(0, CoreXf, /*bWorldSpace*/ true)
		&& Halos != nullptr && HaloCount > 0 && Halos->GetInstanceTransform(0, HaloXf, /*bWorldSpace*/ true))
	{
		const float CoreScaleRead = CoreXf.GetScale3D().X;
		HaloMultiple = (CoreScaleRead > KINDA_SMALL_NUMBER) ? (HaloXf.GetScale3D().X / CoreScaleRead) : 0.f;
	}

	// The trail's radius and ONE segment's length, recovered from the instance scale through the
	// same conversion that wrote it. Multiplied by the segment count for the trail's true length.
	float TrailRadiusRead = 0.f;
	float TrailLengthRead = 0.f;
	FTransform TrailXf;
	if (TrailParts[0] != nullptr && TrailParts[0]->GetInstanceCount() > 0
		&& TrailParts[0]->GetInstanceTransform(0, TrailXf, /*bWorldSpace*/ true))
	{
		TrailRadiusRead = UTraceFxShapes::RadiusUUFromShapeScale(TrailXf.GetScale3D().X);
		TrailLengthRead = UTraceFxShapes::LengthUUFromShapeScale(TrailXf.GetScale3D().Z)
			* static_cast<float>(UE_ARRAY_COUNT(TrailParts));
	}

	return FString::Printf(
		TEXT("model=%s | pieces=%d visible=%d | cores=%d (%s) halos=%d (%s) x%.2f scale | trail 3 x (%s) "
		     "r=%.1fuu total len=%.0fuu | phase=%s"),
		TraceXBeeFx::bBeePolish ? TEXT("POLISH(2.7)") : TEXT("OLD(demo29-5)"),
		Pieces, Visible,
		CoreCount, UTraceFxShapes::BlendName(CoreBlend),
		HaloCount, UTraceFxShapes::BlendName(HaloBlend), HaloMultiple,
		UTraceFxShapes::BlendName(TrailBlend), TrailRadiusRead, TrailLengthRead,
		(Phase == EPhase::Orbiting) ? TEXT("ORBITING")
			: ((Phase == EPhase::Converging) ? TEXT("CONVERGING") : TEXT("LOADED/HIDDEN")));
}

bool ATraceBeeSwarm::DebugGetBeeWorldLocation(int32 BeeIndex, FVector& OutLocation) const
{
	FTransform Instance;
	if (Cores == nullptr || BeeIndex < 0 || Cores->GetInstanceCount() <= BeeIndex
		|| !Cores->GetInstanceTransform(BeeIndex, Instance, /*bWorldSpace*/ true))
	{
		return false;
	}
	OutLocation = Instance.GetLocation();
	return true;
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

		/**
		 * *** THE TAG IS A FIELD AND NOT A LITERAL, AND ONE RUN OF THIS FILE'S HARNESSES IS WHY. ***
		 *
		 * Trace.X.BeeFxTest reuses this checklist. With [XSTINGCLIP] baked into the format string,
		 * the first run of the two printed BeeFxTest's fourteen lines under the clip test's name and
		 * ended with two verdicts that both claimed to be XSTINGCLIP's — unreadable, and worse,
		 * unattributable. Same shape as TraceRoxieVerify::FChecklist::Tag, for the same reason.
		 */
		const TCHAR* Tag = TEXT("XSTINGCLIP");

		void Check(bool bCondition, const FString& Name, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[%s] %s  %s  |  %s"),
				Tag, bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *Name, *Detail);
		}

		void Invalidate(const FString& Reason) { bInvalid = true; InvalidReason = Reason; }

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

				// --- FX_AUDIO_PLAN §2.7's TRACER TINT, MEASURED WHERE IT MATTERS: BEFORE ---------
				//
				// The seam is UTraceWeaponComponent::GetTracerTintOverride (W4-SHOTS wired it and the
				// tracer already consumes it). This tranche does not re-implement it; it PROVES the
				// half of §2.7 that belongs to X — "amber ONLY while the bee clip is loaded" — at the
				// three moments that can be wrong, using the ordinary clip staged above as the arm.
				FLinearColor TintBefore = FLinearColor::White;
				const bool bTintedBefore = Weapon->GetTracerTintOverride(TintBefore);
				State->List.Check(!bTintedBefore,
					TEXT("§2.7 tint: an ORDINARY clip gets NO override — the tracer stays weapon-neutral"),
					FString::Printf(TEXT("GetTracerTintOverride()=%d on a clip of %d ordinary rounds"),
						bTintedBefore ? 1 : 0, State->ClipBeforeSting));

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

				// --- §2.7's tint, DURING: amber, and the exact BeeRounds amber ------------------
				FLinearColor TintLoaded = FLinearColor::White;
				const bool bTintedLoaded = Weapon->GetTracerTintOverride(TintLoaded);
				const bool bAmber = bTintedLoaded
					&& TintLoaded.Equals(TraceXBeeFx::BeeAmber, 0.01f);
				State->List.Check(bAmber,
					TEXT("§2.7 tint: a BEE clip overrides the tracer to BeeRounds amber"),
					FString::Printf(TEXT("override=%d, tint=(%.2f,%.2f,%.2f) against the swarm's own "
					                     "(%.2f,%.2f,%.2f) — one amber, so the bees, their trails and the "
					                     "round they become are the same colour"),
						bTintedLoaded ? 1 : 0, TintLoaded.R, TintLoaded.G, TintLoaded.B,
						TraceXBeeFx::BeeAmber.R, TraceXBeeFx::BeeAmber.G, TraceXBeeFx::BeeAmber.B));

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

				// --- §2.7's tint, AFTER: the amber goes with the last bee round -----------------
				FLinearColor TintSpent = FLinearColor::White;
				const bool bTintedSpent = Weapon->GetTracerTintOverride(TintSpent);
				State->List.Check(!bTintedSpent,
					TEXT("§2.7 tint: the override goes the instant the bee rounds are gone"),
					FString::Printf(TEXT("GetTracerTintOverride()=%d with %d bee round(s) left — 'ONLY while "
					                     "loaded' is the half of the rule a stuck override would break, and "
					                     "the tracer would stay amber for the rest of the match"),
						bTintedSpent ? 1 : 0, Weapon->GetAbilityRoundsInClip()));

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

// =================================================================================================
// Trace.X.MarkParade / Trace.X.MarkWatch — FX_AUDIO_PLAN §2.7's MARK TELL, ON TWO REAL MACHINES
//
// *** WHY THIS NEEDS TWO PROCESSES AND CANNOT BE PROVEN IN ONE. ***
//
// The whole claim of §2.7's marked-enemy tell is "visible to EVERYONE". The mechanism is
// UTraceHealthComponent::VulnerableUntilServerTime replicating COND_None and OnRep_Vulnerable
// building the marker on whatever machine received it. On a listen server the authority calls
// OnRep_Vulnerable BY HAND, so a standalone run exercises a code path that is guaranteed to work and
// says nothing at all about the one that was in doubt — the same trap Trace.Elle.SnapPressTest's own
// header records falling into.
//
// So the work is split. The SERVER half decides and marks; the CLIENT half decides nothing, drives
// nothing, and reports only what it can SEE — how many markers exist on this machine, what their
// pieces measure, what colour their materials actually hold, and how fast the mark is turning. Then
// it photographs one.
//
// FIXED FILENAMES (W5KitsE_Mark_*.png): take the capture lock.
// =================================================================================================

namespace TraceXMarkProbe
{
	/**
	 * The one place this probe marks anybody. Prefers X's own shipped call; says so when it cannot.
	 *
	 * @param XSet  the staged X, or null. When it is non-null and @p Victim is one of its ENEMIES,
	 *              this is the full shipped path: the §4 choke point, then ApplyVulnerable.
	 */
	bool MarkOne(UTraceAbilitySetX* XSet, ATraceCharacter* Victim, bool& bOutThroughX)
	{
		bOutThroughX = false;
		if (Victim == nullptr || Victim->Health == nullptr)
		{
			return false;
		}

		if (XSet != nullptr && XSet->MarkVulnerable(Victim))
		{
			bOutThroughX = true;
			return true;
		}

		// FALLBACK, and it is NOT a copy of the mark: ApplyVulnerable is the identical function
		// MarkVulnerable calls one line later, and it is the function whose OnRep drives the tell.
		// What this arm skips is the CHOKE POINT, which is not what §2.7 is about and which
		// Trace.X.CarrierTest red-arms on its own.
		return Victim->Health->ApplyVulnerable(TraceVulnerable::GetDurationSeconds(), nullptr);
	}

	void RunMarkParade(UWorld* WorldPtr, float Seconds)
	{
		if (WorldPtr == nullptr || WorldPtr->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[MARKPARADE] needs the AUTHORITY — run it on the server."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[MARKPARADE] marking for %.0fs on netmode=%d. Mark duration %.2fs, so a re-application "
			     "every %.2fs keeps a live mark on screen for a client watching from anywhere in the window."),
			Seconds, static_cast<int32>(WorldPtr->GetNetMode()),
			TraceVulnerable::GetDurationSeconds(), TraceVulnerable::GetDurationSeconds() * 0.6f);

		struct FParadeState
		{
			double EndReal = 0.0;
			double NextMarkReal = 0.0;
			int32 Applied = 0;
			int32 ThroughX = 0;
		};

		TSharedRef<FParadeState> P = MakeShared<FParadeState>();
		P->EndReal = FPlatformTime::Seconds() + FMath::Clamp(Seconds, 1.f, 240.f);
		TWeakObjectPtr<UWorld> WeakWorld(WorldPtr);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([P, WeakWorld](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}

			const double NowReal = FPlatformTime::Seconds();
			if (NowReal >= P->NextMarkReal)
			{
				P->NextMarkReal = NowReal + static_cast<double>(TraceVulnerable::GetDurationSeconds() * 0.6f);

				// *** THE HUMAN PLAYER IS MADE X, AND THE VICTIMS ARE HIS ENEMIES. join1 AND join2
				// ARE BOTH WHY. *** join1 assumed one of six bots would happen to be X and reported
				// "no X in this match" a hundred times. join2 staged one but still fell back, because
				// the first two living non-carriers the iterator found were on X's OWN TEAM and
				// CanAffect correctly refused them — a true refusal that looked like a missing X.
				// Choosing enemies is what makes the shipped path the normal answer.
				FString Why;
				UTraceAbilitySetX* XSet = TraceXStingClipTest::MakePlayerIntoX(TickWorld, Why);
				const ATraceCharacter* XPawn = (XSet != nullptr) ? XSet->GetCharacter() : nullptr;

				// TWO victims, so a client whose camera lands on one of them has a second chance, and
				// so the frame shows the tell is per-victim rather than one actor in the level.
				int32 MarkedThisRound = 0;
				for (TActorIterator<ATraceCharacter> It(TickWorld); It && MarkedThisRound < 2; ++It)
				{
					ATraceCharacter* Victim = *It;
					if (Victim == nullptr || !Victim->IsAlive() || Victim->IsCarrier())
					{
						continue;   // a carrier is refused by the rule, not by this loop's preference
					}
					if (XPawn != nullptr && (Victim == XPawn || Victim->GetTeam() == XPawn->GetTeam()))
					{
						continue;   // his own side: CanAffect would refuse, and rightly
					}

					bool bThroughX = false;
					if (MarkOne(XSet, Victim, bThroughX))
					{
						++MarkedThisRound;
						++P->Applied;
						P->ThroughX += bThroughX ? 1 : 0;
						UE_LOG(LogTraceGame, Display,
							TEXT("[MARKPARADE] marked %s at (%s) — through %s. Stacks now %d, %.2fs left."),
							*GetNameSafe(Victim), *Victim->GetActorLocation().ToCompactString(),
							bThroughX ? TEXT("UTraceAbilitySetX::MarkVulnerable (the full shipped path)")
							          : TEXT("UTraceHealthComponent::ApplyVulnerable (no X in this match)"),
							Victim->Health->GetVulnerableStacks(), Victim->Health->GetVulnerableRemaining());
					}
				}
			}

			if (NowReal < P->EndReal)
			{
				return true;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[MARKPARADE] VERDICT: %d mark(s) applied, %d of them through X's own MarkVulnerable."),
				P->Applied, P->ThroughX);
			return false;
		}), 0.f);
	}

	void RunMarkWatch(UWorld* WorldPtr, float Seconds)
	{
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[MARKWATCH] no world."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[MARKWATCH] watching for %.0fs on netmode=%d (0 standalone, 2 listen, 3 CLIENT). This "
			     "machine decides nothing: every number below is read back off the components that are on "
			     "screen here."),
			Seconds, static_cast<int32>(WorldPtr->GetNetMode()));

		struct FWatchState
		{
			double EndReal = 0.0;
			int32 MostMarkers = 0;
			int32 MostPieces = 0;
			int32 MostVisiblePieces = 0;
			bool  bSeen = false;
			bool  bViewed = false;
			int32 Shots = 0;
			double NextShotReal = 0.0;
			FString FirstSightLine;
			// *** THE SPIN IS ACCUMULATED PER TICK, AND join1's -10.5 deg/s IS WHY. ***
			//
			// FRotator::Yaw is normalised to [-180, 180]. At 90 deg/s a marker turns 648 degrees over
			// a 7 s sample, i.e. it wraps nearly twice, and first-minus-last across the window is
			// meaningless however many unwraps are bolted onto it — join1 reported -10.5 deg/s for a
			// marker that was visibly turning the right way. Summing FindDeltaAngleDegrees between
			// CONSECUTIVE frames cannot wrap: at 60 Hz each step is about 1.5 degrees.
			float YawPrevious = 0.f;
			double YawPreviousAt = 0.0;
			float YawAccumulated = 0.f;
			double YawSeconds = 0.0;
			bool bYawSampled = false;
			FObjectKey YawSubject;
			int32 BeeStingBursts = 0;
			TWeakObjectPtr<ACameraActor> Observer;
		};

		TSharedRef<FWatchState> W = MakeShared<FWatchState>();
		W->EndReal = FPlatformTime::Seconds() + FMath::Clamp(Seconds, 1.f, 240.f);
		TWeakObjectPtr<UWorld> WeakWorld(WorldPtr);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([W, WeakWorld](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}

			int32 Markers = 0;
			ATraceVulnerableMarker* First = nullptr;
			for (TActorIterator<ATraceVulnerableMarker> It(TickWorld); It; ++It)
			{
				if (!IsValid(*It))
				{
					continue;
				}
				++Markers;
				if (First == nullptr)
				{
					First = *It;
				}
			}
			W->MostMarkers = FMath::Max(W->MostMarkers, Markers);

			int32 Bursts = 0;
			for (TActorIterator<ATraceFxBurst> It(TickWorld); It; ++It)
			{
				if (IsValid(*It) && It->GetBurstType() == ETraceFxBurstType::BeeSting)
				{
					++Bursts;
				}
			}
			W->BeeStingBursts = FMath::Max(W->BeeStingBursts, Bursts);

			if (First != nullptr)
			{
				// ---- measure the pieces off the LIVE actor ----------------------------------
				TArray<UStaticMeshComponent*> Pieces;
				First->GetComponents<UStaticMeshComponent>(Pieces);

				int32 VisiblePieces = 0;
				FString PieceLines;
				for (const UStaticMeshComponent* Piece : Pieces)
				{
					if (Piece == nullptr)
					{
						continue;
					}
					VisiblePieces += Piece->IsVisible() ? 1 : 0;

					// The COLOUR THE MATERIAL ACTUALLY HOLDS. On the additive rungs SetGlow folds
					// hue x intensity x opacity into "Color", so this one read proves the hue is
					// rose AND that the piece is not sitting at the engine's default white.
					FLinearColor Colour = FLinearColor::White;
					bool bHaveColour = false;
					if (const UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Piece->GetMaterial(0)))
					{
						bHaveColour = MID->GetVectorParameterValue(FMaterialParameterInfo(TEXT("Color")), Colour);
					}

					const FVector Scale = Piece->GetComponentScale();
					PieceLines += FString::Printf(
						TEXT("\n[MARKWATCH]   %-22s visible=%d  scale=(%.3f,%.3f,%.3f)  "
						     "planeW=%.1fuu / ringR=%.1fuu  Color=%s(%.3f,%.3f,%.3f)"),
						*Piece->GetName(), Piece->IsVisible() ? 1 : 0, Scale.X, Scale.Y, Scale.Z,
						Scale.X * UTraceFxShapes::BasicShapeExtentUU,
						UTraceFxShapes::RadiusUUFromShapeScale(Scale.X),
						bHaveColour ? TEXT("") : TEXT("<absent>"), Colour.R, Colour.G, Colour.B);
				}
				W->MostPieces = FMath::Max(W->MostPieces, Pieces.Num());
				W->MostVisiblePieces = FMath::Max(W->MostVisiblePieces, VisiblePieces);

				// ---- the spin rate, per marker, reset when the subject changes ----------------
				const FObjectKey Subject(First);
				const float Yaw = First->GetActorRotation().Yaw;
				const double NowReal = FPlatformTime::Seconds();
				if (!W->bYawSampled || Subject != W->YawSubject)
				{
					// A NEW SUBJECT RESTARTS THE SAMPLE. A rate taken across two different markers is
					// arithmetic about two unrelated actors — the same trap Trace.Elle.GateWatch
					// records for its own ring.
					W->bYawSampled = true;
					W->YawSubject = Subject;
				}
				else
				{
					W->YawAccumulated += FMath::Abs(FMath::FindDeltaAngleDegrees(W->YawPrevious, Yaw));
					W->YawSeconds += (NowReal - W->YawPreviousAt);
				}
				W->YawPrevious = Yaw;
				W->YawPreviousAt = NowReal;

				if (!W->bSeen)
				{
					W->bSeen = true;
					W->FirstSightLine = FString::Printf(
						TEXT("%d marker(s), %d piece(s) of which %d visible, on %s at (%s).%s"),
						Markers, Pieces.Num(), VisiblePieces, *GetNameSafe(First->GetOwner()),
						*First->GetActorLocation().ToCompactString(), *PieceLines);
					UE_LOG(LogTraceGame, Display, TEXT("[MARKWATCH] *** FIRST FRAME WITH A MARK *** %s"),
						*W->FirstSightLine);
				}

				// ---- keep this machine's own camera ON the marked player ----------------------
				//
				// *** IT FOLLOWS, AND join1's four empty frames ARE WHY. *** The first version placed
				// one observer at the first marker's position and never moved it. Bots run: frame 1
				// caught the tell and frames 2 to 5 photographed an empty stretch of arena the marked
				// pawn had left. The observer is now re-aimed every tick, so every frame in the set is
				// a frame of the thing under test.
				//
				// Close and slightly above: the tell is an 18 uu diamond and a 44 uu ring, and a
				// 700 uu observer photographs a pair of dots. ~300 uu is about a duel's range.
				const FVector Mid = First->GetActorLocation();
				const FVector At = Mid + FVector(-260.f, 90.f, 130.f);

				if (!W->bViewed)
				{
					if (APlayerController* PC = TickWorld->GetFirstPlayerController())
					{
						FActorSpawnParameters Params;
						Params.ObjectFlags |= RF_Transient;
						if (ACameraActor* Observer = TickWorld->SpawnActor<ACameraActor>(
							ACameraActor::StaticClass(), At, (Mid - At).Rotation(), Params))
						{
							// LOCAL VIEW ONLY: no pawn is moved, so nothing the server owns is touched
							// and no correction can fight it. Same trick Trace.Elle.GateWatch uses.
							PC->SetViewTargetWithBlend(Observer, 0.f);
							W->Observer = Observer;
							W->bViewed = true;
							W->NextShotReal = FPlatformTime::Seconds() + 0.3;
						}
					}
				}
				else if (ACameraActor* Observer = W->Observer.Get())
				{
					Observer->SetActorLocationAndRotation(At, (Mid - At).Rotation());
				}
			}

			if (W->bViewed && W->Shots < 5 && Markers > 0 && FPlatformTime::Seconds() >= W->NextShotReal)
			{
				const FString Path = FPaths::ConvertRelativePathToFull(
					FPaths::ProjectSavedDir() / TEXT("Screenshots")
					/ FString::Printf(TEXT("W5KitsE_Mark_%02d.png"), W->Shots + 1));
				FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
				++W->Shots;
				W->NextShotReal = FPlatformTime::Seconds() + ((W->Shots < 2) ? 0.7 : 3.0);
				UE_LOG(LogTraceGame, Display,
					TEXT("[MARKWATCH] Screenshot requested: %s  (markers=%d, BeeSting bursts live=%d)"),
					*Path, Markers, Bursts);
			}

			if (FPlatformTime::Seconds() < W->EndReal)
			{
				return true;
			}

			const double Window = FMath::Max(0.01, W->YawSeconds);
			const float Rate = static_cast<float>(W->YawAccumulated / Window);

			UE_LOG(LogTraceGame, Display,
				TEXT("[MARKWATCH] VERDICT on this machine (netmode=%d): markers seen=%d | most pieces=%d, "
				     "visible=%d | spin %.1f deg/s over %.1fs | live BeeSting bursts caught=%d | %d frame(s). "
				     "First sight: %s"),
				static_cast<int32>(TickWorld->GetNetMode()), W->MostMarkers, W->MostPieces,
				W->MostVisiblePieces, Rate, Window, W->BeeStingBursts, W->Shots,
				W->bSeen ? *W->FirstSightLine : TEXT("*** NO MARK WAS EVER VISIBLE ON THIS MACHINE ***"));
			return false;
		}), 0.f);
	}

	FAutoConsoleCommandWithWorld CmdMarkParade(
		TEXT("Trace.X.MarkParade"),
		TEXT("Dev only, SERVER. FX plan §2.7: keeps two living non-carriers marked VULNERABLE for 60 s so a "
		     "second process can be asked whether it can see the mark. Prefers X's own MarkVulnerable and "
		     "says in the log when it had to fall back to ApplyVulnerable."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			RunMarkParade(World, 60.f);
		}));

	FAutoConsoleCommandWithWorld CmdMarkWatch(
		TEXT("Trace.X.MarkWatch"),
		TEXT("Dev only, runs ANYWHERE and is meant for a CLIENT. Watches 45 s and reports what THIS machine "
		     "can see of §2.7's vulnerable mark — marker count, each piece's visibility, measured size and the "
		     "colour its material actually holds, the spin rate, and any live BeeSting bursts — then "
		     "photographs one. Writes FIXED filenames; take the capture lock."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			RunMarkWatch(World, 45.f);
		}));
}

// =================================================================================================
// Trace.X.BeeFxTest — X's bee swarm and its sting-load converge, MEASURED
//
// Everything it asserts is read back off the live components (ATraceBeeSwarm::DebugDescribe and
// DebugGetBeeWorldLocation), never recomputed from TraceXBeeFx — a verifier that re-derives its own
// expectations is checking its arithmetic, not the picture.
//
// *** IT CHECKS WHICHEVER MODEL IS BUILT. *** Demo 29 item 5 put the OLD bee model back
// (TraceXBeeFx::bBeePolish false) — N plain amber spheres and nothing around them — so step 1 runs
// the old model's checklist and says so. With the gate on it runs §2.7's five-piece checklist
// unchanged. The converge half below is common to both, because the converge is not the model.
//
// The converge half is the one that needed a red arm: "the bees fly to the gun" is an ANIMATION, and
// an animation is exactly the kind of claim that passes by accident. Trace.X.BeeConverge 0 restores
// the instant hide, and the run below refuses to call itself green unless that arm was shown failing
// first.
// =================================================================================================

namespace TraceXBeeFxTest
{
	void Run()
	{
		UWorld* WorldPtr = TraceXStingClipTest::FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XBEEFX] no authoritative game world — run this on the server."));
			return;
		}

		struct FState
		{
			int32 Step = 0;
			double NextStepReal = 0.0;
			double Deadline = 0.0;
			TraceXStingClipTest::FChecklist List;
			bool bRedReproduced = false;
			float DistanceAtStart = 0.f;
			float DistanceMid = 0.f;
		};

		TSharedPtr<FState> State = MakeShared<FState>();
		State->List.Tag = TEXT("XBEEFX");
		State->Deadline = FPlatformTime::Seconds() + 45.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[XBEEFX] ===== model=%s (Demo 29 item 5: bBeePolish=%d). The §2.7 numbers below are what\n")
			TEXT("[XBEEFX]       the halo and trail WOULD be built to; with the gate off they are not built.\n"),
			TraceXBeeFx::bBeePolish ? TEXT("POLISH(2.7)") : TEXT("OLD"), TraceXBeeFx::bBeePolish ? 1 : 0);
		UE_LOG(LogTraceGame, Display,
			TEXT("[XBEEFX] ===== FX plan §2.7: %d bees, halo x%.2f at I %.2f, trail %.0f uu at I %.2f/%.2f/%.2f, "
			     "converge %.2fs. arm 0 = RED (Trace.X.BeeConverge 0): the swarm vanishes with no flight. ====="),
			TraceXBees::GetBeeCount(), TraceXBeeFx::HaloScaleMultiple, TraceXBeeFx::HaloIntensity,
			TraceXBeeFx::TrailLengthUU, TraceXBeeFx::TrailIntensityAt(0), TraceXBeeFx::TrailIntensityAt(1),
			TraceXBeeFx::TrailIntensityAt(2), TraceXBeeFx::ConvergeSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				TraceXStingClipTest::SetArm(TEXT("Trace.X.BeeConverge"), 1);
				return false;
			}
			if (NowReal < State->NextStepReal)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetX* XSet = TraceXStingClipTest::MakePlayerIntoX(TickWorld, Why);
			ATraceCharacter* XPawn = (XSet != nullptr) ? XSet->GetCharacter() : nullptr;

			// The swarm is local and is built by UpdateSwarmActor on the next ability tick, so this
			// finds it the way any other machine would: by looking in the world.
			ATraceBeeSwarm* SwarmActor = nullptr;
			for (TActorIterator<ATraceBeeSwarm> It(TickWorld); It; ++It)
			{
				if (IsValid(*It) && It->Host.Get() == XPawn)
				{
					SwarmActor = *It;
					break;
				}
			}

			if (XSet == nullptr || XPawn == nullptr || SwarmActor == nullptr || !XPawn->IsAlive())
			{
				if (State->Step == 0 && NowReal <= State->Deadline)
				{
					return true;   // still staging: the set, the pawn and the swarm arrive on three ticks
				}
				State->List.Invalidate(FString::Printf(
					TEXT("could not stage X and a swarm (%s): pawn=%s swarm=%s"),
					*Why, *GetNameSafe(XPawn), *GetNameSafe(SwarmActor)));
				State->List.Report();
				TraceXStingClipTest::SetArm(TEXT("Trace.X.BeeConverge"), 1);
				return false;
			}

			// ---- step 0: the RED arm — no flight at all --------------------------------------
			if (State->Step == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[XBEEFX] staged: %s | %s"),
					*GetNameSafe(XPawn), *SwarmActor->DebugDescribe());

				// *** RESET THE PHASE FIRST, AND THE FIRST RUN OF THIS COMMAND IS WHY. ***
				//
				// SetSwarmLoaded is idempotent by design: it refuses to restart a converge that is
				// already running. Run beside Trace.X.StingClipTest, which loads the clip itself,
				// this arrived with the swarm already CONVERGING, the red arm's call was correctly a
				// no-op, and the run reported "the red arm did not reproduce" — a true statement
				// about a harness that had not staged its own subject. Owning the phase here makes
				// the command independent of whatever else has touched this X.
				SwarmActor->SetSwarmLoaded(false, /*bAnimate*/ false);

				TraceXStingClipTest::SetArm(TEXT("Trace.X.BeeConverge"), 0);
				SwarmActor->SetSwarmLoaded(true, /*bAnimate*/ true);
				State->bRedReproduced = !SwarmActor->IsConverging() && SwarmActor->IsLoadedAndHidden();

				State->List.Check(State->bRedReproduced,
					TEXT("RED ARM: with the converge disarmed the swarm vanishes on the same frame"),
					FString::Printf(TEXT("converging=%d, loaded/hidden=%d — this IS the pre-§2.7 build, so the "
					                     "green run below is measuring an animation that can be absent"),
						SwarmActor->IsConverging() ? 1 : 0, SwarmActor->IsLoadedAndHidden() ? 1 : 0));

				TraceXStingClipTest::SetArm(TEXT("Trace.X.BeeConverge"), 1);
				SwarmActor->SetSwarmLoaded(false, /*bAnimate*/ false);   // back to orbiting
				State->Step = 1;
				State->NextStepReal = NowReal + 0.2;
				return true;
			}

			// ---- step 1: the pieces, measured off the live components ------------------------
			if (State->Step == 1)
			{
				const FString Described = SwarmActor->DebugDescribe();
				UE_LOG(LogTraceGame, Display, TEXT("[XBEEFX] live swarm: %s"), *Described);

				// *** THIS BLOCK CHECKS WHICHEVER MODEL IS ACTUALLY ON SCREEN — DEMO 29 ITEM 5. ***
				// Asserting §2.7's five pieces on a build that deliberately does not build them would
				// be a harness reporting a fault it was told to expect, which is worse than no
				// harness. So the gate picks the checklist, and the checklist SAYS which one it ran.
				if (TraceXBeeFx::bBeePolish)
				{
					State->List.Check(Described.Contains(TEXT("pieces=5 visible=5")),
						TEXT("§2.7: five instanced pieces — cores, halos and three trail segments — all visible"),
						FString::Printf(TEXT("%s. Five per-bee meshes became five INSTANCED pieces, so the halo and "
						                     "the trail cost no net components"), *Described));

					State->List.Check(Described.Contains(TEXT("x1.80 scale")),
						TEXT("§2.7: the halo sleeve is x1.8 the core's scale, read off instance 0 of BOTH"),
						*Described);

					State->List.Check(!Described.Contains(TEXT("(None)")),
						TEXT("no piece degraded to None — nothing is hidden and nothing is drawing grey"),
						*Described);
				}
				else
				{
					State->List.Check(Described.Contains(TEXT("model=OLD(demo29-5)")),
						TEXT("Demo 29 item 5: the swarm reports the OLD bee model, not §2.7's polish"),
						*Described);

					State->List.Check(Described.Contains(TEXT("pieces=1 visible=1")),
						TEXT("Demo 29 item 5: ONE piece — the bee cores. No halo component, no trail components"),
						FString::Printf(TEXT("%s. The halo and the three trail segments are not built at all "
						                     "while TraceXBeeFx::bBeePolish is false"), *Described));

					State->List.Check(Described.Contains(TEXT("halos=0")) && Described.Contains(TEXT("len=0uu")),
						TEXT("Demo 29 item 5: no halo instances and no trail length — nothing is drawn around a bee"),
						*Described);

					State->List.Check(!Described.Contains(TEXT("cores=0")),
						TEXT("the bees themselves are still there and still instanced"),
						*Described);
				}

				State->DistanceAtStart = 0.f;
				FVector BeeZero = FVector::ZeroVector;
				if (SwarmActor->DebugGetBeeWorldLocation(0, BeeZero))
				{
					State->DistanceAtStart = static_cast<float>(
						FVector::Dist(BeeZero, XPawn->GetMuzzleLocation()));
				}

				State->List.Check(State->DistanceAtStart > TraceXBees::GetOrbitRadiusUU() * 0.4f,
					TEXT("before Sting the bees are OUT on the orbit, not already at the gun"),
					FString::Printf(TEXT("bee 0 is %.0f uu from the muzzle, orbit radius %.0f uu"),
						State->DistanceAtStart, TraceXBees::GetOrbitRadiusUU()));

				// THROUGH THE ABILITY, not by poking the swarm: this is the state edge a player makes.
				XSet->ActivateAbility();
				SwarmActor->SetSwarmLoaded(true, /*bAnimate*/ true);

				State->List.Check(SwarmActor->IsConverging(),
					TEXT("§2.7: activating Sting starts the FLIGHT rather than hiding the swarm"),
					FString::Printf(TEXT("converging=%d, hidden=%d"),
						SwarmActor->IsConverging() ? 1 : 0, SwarmActor->IsLoadedAndHidden() ? 1 : 0));

				State->Step = 2;
				State->NextStepReal = NowReal + static_cast<double>(TraceXBeeFx::ConvergeSeconds) * 0.5;
				return true;
			}

			// ---- step 2: half way — closer to the gun, still drawn ---------------------------
			if (State->Step == 2)
			{
				FVector BeeZero = FVector::ZeroVector;
				State->DistanceMid = SwarmActor->DebugGetBeeWorldLocation(0, BeeZero)
					? static_cast<float>(FVector::Dist(BeeZero, XPawn->GetMuzzleLocation()))
					: State->DistanceAtStart;

				State->List.Check(State->DistanceMid < State->DistanceAtStart * 0.9f,
					TEXT("half way through the converge the bees are measurably CLOSER to the muzzle"),
					FString::Printf(TEXT("bee 0: %.0f uu -> %.0f uu in %.2fs"),
						State->DistanceAtStart, State->DistanceMid, TraceXBeeFx::ConvergeSeconds * 0.5f));

				State->List.Check(!SwarmActor->IsLoadedAndHidden(),
					TEXT("...and they are still DRAWN while they fly, which is the whole of §2.7's change"),
					FString::Printf(TEXT("hidden=%d"), SwarmActor->IsLoadedAndHidden() ? 1 : 0));

				State->Step = 3;
				State->NextStepReal = NowReal + static_cast<double>(TraceXBeeFx::ConvergeSeconds) * 0.9;
				return true;
			}

			// ---- step 3: arrived --------------------------------------------------------------
			State->List.Check(SwarmActor->IsLoadedAndHidden() && !SwarmActor->IsConverging(),
				TEXT("§2.7: after the converge they are in the gun and drawn nowhere"),
				FString::Printf(TEXT("%s"), *SwarmActor->DebugDescribe()));

			if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — with Trace.X.BeeConverge 0 the "
				                            "swarm still animated, so nothing above measures the converge"));
			}

			State->List.Report();
			TraceXStingClipTest::SetArm(TEXT("Trace.X.BeeConverge"), 1);
			return false;
		}));
	}

	FAutoConsoleCommand CmdBeeFxTest(
		TEXT("Trace.X.BeeFxTest"),
		TEXT("Dev only, SERVER. FX plan §2.7: the swarm's five instanced pieces and their achieved blends, the "
		     "halo's x1.8 scale and the trail's measured radius and length — all read back off the live "
		     "components — then the 0.3 s sting-load converge, measured as bee 0's distance to the muzzle. "
		     "Red-arms itself with Trace.X.BeeConverge 0 first."),
		FConsoleCommandDelegate::CreateStatic(&Run));
}

#endif // !UE_BUILD_SHIPPING
