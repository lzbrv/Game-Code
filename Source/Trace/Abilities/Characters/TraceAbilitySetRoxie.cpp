// Trace — Roxie. Read TraceAbilitySetRoxie.h first; it is the design document, and it states the two
// things about MODDED that belong in the playtest brief rather than in a footnote.

#include "Abilities/Characters/TraceAbilitySetRoxie.h"

#include "Containers/Ticker.h"               // the verification fixtures at the bottom
#include "Engine/Engine.h"                   // GEngine->GetWorldContexts, for the fixtures
#include "Engine/World.h"
#include "EngineUtils.h"                     // TActorIterator
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "Abilities/Characters/TraceRoxieRocket.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceWeaponComponent.h"    // v16 §1's ammo system: MODDED READS it, never writes it
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE TWO RED ARMS THIS FILE OWNS
//
// Both exist for the reason this project states everywhere: a harness that cannot be made to fail is
// not evidence. Each removes exactly one clause of spec v18 §2 so the matching Trace.Roxie.* command
// can be shown reporting the failure it is supposed to catch.
//
// (The rocket's wobble has a third, Trace.Roxie.RocketWobble, and it lives beside the amplitude
// accessor in TraceRoxieRocket.cpp so that all three path callers are disarmed by one switch.)
// =================================================================================================

static TAutoConsoleVariable<int32> CVarRoxieJumpPassive(
	TEXT("Trace.Roxie.JumpPassive"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): Roxie jumps RoxieJumpHeightBonus higher — spec v18 §2. 0: she "
	     "jumps exactly like everybody else, so Trace.Roxie.JumpTest can be shown FAILING. Never ship 0."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarRoxieSelfLaunch(
	TEXT("Trace.Roxie.SelfLaunch"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): firing the rocket launches Roxie backwards — spec v18 §2. 0: the "
	     "rocket fires and she does not move, so the launch can be shown missing. Never ship 0."),
	ECVF_Cheat);

// =================================================================================================
// THE SEAMS THE WEAPON READS  (spec v29 §2e and §2b)
//
// Two free functions, taking the shooting ACTOR, mirroring TraceSlimeball::GetFireIntervalScaleFor()
// so that UTraceWeaponComponent asks one question and does no casting of its own. Both are safe with
// a null actor, with an actor that has no ability component, and with every character who is not
// Roxie — they are a no-op for a Mannequin, for every bot and for Roxie herself with MODDED down.
//
// The header says where these BELONG (a virtual on UTraceCharacterAbilitySet with a static in front
// of it, exactly as the fire-rate scale has) and why they are here instead this pass. Two lines to
// migrate, and nothing else reads them.
// =================================================================================================

namespace TraceRoxie
{
	/** The Roxie set driving @p Shooter, or null. One place, so the two functions cannot diverge. */
	static const UTraceAbilitySetRoxie* FindRoxieSet(const AActor* Shooter)
	{
		UTraceCharacterAbilitySet* Set = UTraceAbilityComponent::GetAbilitySetFor(Shooter);
		return (Set != nullptr) ? Cast<UTraceAbilitySetRoxie>(Set) : nullptr;
	}

	float GetAddedRecoilScaleFor(const AActor* Shooter)
	{
		const UTraceAbilitySetRoxie* Roxie = FindRoxieSet(Shooter);
		return (Roxie != nullptr) ? Roxie->GetAddedRecoilScale() : 0.f;
	}

	bool IsFullAutoForcedFor(const AActor* Shooter)
	{
		const UTraceAbilitySetRoxie* Roxie = FindRoxieSet(Shooter);
		return (Roxie != nullptr) && Roxie->IsFullAutoForced();
	}
}

// =================================================================================================
// PASSIVE — "jumps 15% higher"
// =================================================================================================

float UTraceAbilitySetRoxie::GetJumpVelocityScale() const
{
	// *** THE SQUARE ROOT. *** Apex height is v^2 / 2g, so a +15% APEX costs sqrt(1.15) = 1.0724 on the
	// launch velocity, not 1.15. See the header for the +25%-distance/+65.8%-actual mistake this
	// project already shipped once on Chut's bash.
	const float HeightBonus = FMath::Clamp(UTraceSettings::Get().RoxieJumpHeightBonus, 0.f, 2.f);
	return FMath::Sqrt(1.f + HeightBonus);
}

float UTraceAbilitySetRoxie::GetBaseJumpZVelocity() const
{
	const UTraceCharacterMovementComponent* Move = GetMovement();
	if (Move == nullptr)
	{
		return 0.f;
	}

	// The CDO of the pawn's OWN movement class, so a subclass with a different authored jump is
	// scaled from its own number rather than from a literal. Taking the base from the live component
	// instead would compound: every re-assert would scale the already-scaled value.
	if (const UCharacterMovementComponent* MoveDefaults = GetDefault<UCharacterMovementComponent>(Move->GetClass()))
	{
		return MoveDefaults->JumpZVelocity;
	}
	return Move->JumpZVelocity;
}

float UTraceAbilitySetRoxie::GetAppliedJumpZVelocity() const
{
	const UTraceCharacterMovementComponent* Move = GetMovement();
	return (Move != nullptr) ? Move->JumpZVelocity : 0.f;
}

void UTraceAbilitySetRoxie::ApplyJumpProfile()
{
	UTraceCharacterMovementComponent* Move = GetMovement();
	if (Move == nullptr)
	{
		return;   // dead, or between pawns. The next pawn gets it on the tick after it arrives.
	}

	const float BaseJumpZ = GetBaseJumpZVelocity();
	if (BaseJumpZ <= 0.f)
	{
		return;
	}

	const bool bPassiveArmed = (CVarRoxieJumpPassive.GetValueOnAnyThread() != 0);
	const float DesiredJumpZ = bPassiveArmed ? (BaseJumpZ * GetJumpVelocityScale()) : BaseJumpZ;

	// IDEMPOTENT AND RE-ASSERTED, never toggled on an edge. Same contract (and the same reason)
	// UTraceWeaponComponent::RefreshMovementProfile gives: the value must be right on the server AND on
	// the owning client that predicts its own moves, and OnPawnSpawned only runs on the server. Writing
	// on a change only means the tick costs one float compare.
	if (!FMath::IsNearlyEqual(Move->JumpZVelocity, DesiredJumpZ, 0.01f))
	{
		Move->JumpZVelocity = DesiredJumpZ;
	}
}

void UTraceAbilitySetRoxie::RestoreJumpProfile()
{
	UTraceCharacterMovementComponent* Move = GetMovement();
	const float BaseJumpZ = GetBaseJumpZVelocity();
	if (Move != nullptr && BaseJumpZ > 0.f && !FMath::IsNearlyEqual(Move->JumpZVelocity, BaseJumpZ, 0.01f))
	{
		Move->JumpZVelocity = BaseJumpZ;
	}
}

// =================================================================================================
// MOVEMENT — the rocket, on V
// =================================================================================================

float UTraceAbilitySetRoxie::GetRocketCooldownRemaining() const
{
	// THE LATER OF THE TWO, so the predicted mirror can only ever be stricter than the replicated
	// truth. See the comment on PredictedRocketReadyMatchTime for the one case it exists for.
	const float ReadyAt = FMath::Max(State().AuxEndMatchTime, PredictedRocketReadyMatchTime);
	return FMath::Max(0.f, ReadyAt - MatchTimeNow());
}

bool UTraceAbilitySetRoxie::IsRocketReady() const
{
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		return false;
	}
	return GetRocketCooldownRemaining() <= 0.f;
}

ATraceRoxieRocket* UTraceAbilitySetRoxie::GetLiveRocket() const
{
	return LiveRocket.Get();
}

bool UTraceAbilitySetRoxie::ShouldDriveMovement() const
{
	// A simulated proxy's velocity is replicated; writing it there would fight the interpolation and
	// show up as somebody else's Roxie stuttering. Same rule Mace's suspend uses.
	return HasAuthority() || IsLocallyControlled();
}

bool UTraceAbilitySetRoxie::OnSecondaryPressed()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		return false;
	}

	// *** THERE IS DELIBERATELY NO CARRIER CHECK ON THIS LINE. [ASSUMPTION], FLAGGED IN THE REPORT. ***
	//
	// A Roxie CARRYING THE CORE may still fire the rocket. §2 says nothing either way, and the
	// precedent in this codebase is that carrying does not switch an ability off: spec v14 §6 has
	// Oyster dropping a jar "at the start of every dash, INCLUDING while carrying the Core", and those
	// jars deal 30 damage. The rocket is also primarily a MOVEMENT ability, and taking a character's
	// mobility away for the whole time they hold the objective is a far bigger change than letting them
	// keep one shot. To reverse it, add `|| MyPawn->IsCarrier()` to the guard above; there is no knob
	// for it because this slice may not add one to UTraceSettings.
	//
	// The carrier the rocket might HIT is a separate question and is not an assumption at all — that is
	// the founding invariant, and it is enforced in ApplyRocketDamageTo.
	if (!IsRocketReady())
	{
		return false;   // the 35 s. Its own cooldown, on the match clock; see IsRocketReady.
	}

	const float Now = MatchTimeNow();
	const float ReadyAt = Now + TraceRoxieRocket::GetCooldownSeconds();

	// Written on BOTH machines, from the same shared clock, so the owning client's own V greys out on
	// the press instead of a round trip later.
	PredictedRocketReadyMatchTime = ReadyAt;

	if (HasAuthority())
	{
		FTraceAbilityNetState& Writable = MutableState();
		Writable.AuxEndMatchTime = ReadyAt;
		Writable.Flags |= TraceRoxieFlags::RocketInFlight;
		MarkStateDirty();

		// THE SEED IS ROLLED ON THE SERVER, ONCE, AND REPLICATED. Rolling it on each machine would give
		// every machine a different arc, which is the "the drawn path is not the lethal path" defect
		// this whole feature is arranged to avoid.
		SpawnRocket(FMath::FRand());

		UE_LOG(LogTraceGame, Log,
			TEXT("[Roxie] ROCKET away: %.0f uu/s, %.0f damage flat, wobble %.0f uu @ %.1f Hz, self-launch "
			     "%.0f uu/s backwards (+%.0f%% up). Next V in %.0fs."),
			TraceRoxieRocket::GetSpeedUU(), TraceRoxieRocket::GetDamage(),
			TraceRoxieRocket::GetWobbleAmplitudeUU(), TraceRoxieRocket::GetWobbleFrequencyHz(),
			TraceRoxieRocket::GetSelfLaunchImpulse(), TraceRoxieRocket::GetSelfLaunchUpBias() * 100.f,
			TraceRoxieRocket::GetCooldownSeconds());
	}

	ApplySelfLaunch();
	return true;
}

ATraceRoxieRocket* UTraceAbilitySetRoxie::SpawnRocket(float WobbleSeedTurns)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UWorld* RoxieWorld = GetWorld();
	if (!HasAuthority() || MyPawn == nullptr || RoxieWorld == nullptr)
	{
		return nullptr;
	}

	const FVector Origin = MyPawn->GetMuzzleLocation();
	const FVector AimDirection = MyPawn->GetAimDirection().GetSafeNormal();
	if (AimDirection.IsNearlyZero())
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = MyPawn;
	SpawnParams.Instigator = MyPawn;
	// AlwaysSpawn: the rocket has no collision of its own, so there is nothing for the spawn handler to
	// resolve, and a refused spawn would silently eat a 35 s ability.
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ATraceRoxieRocket* Rocket = RoxieWorld->SpawnActor<ATraceRoxieRocket>(
		ATraceRoxieRocket::StaticClass(), Origin, AimDirection.Rotation(), SpawnParams);
	if (Rocket == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Roxie] rocket failed to spawn — the ability fizzled."));
		return nullptr;
	}

	Rocket->InitialiseFlight(this, Origin, AimDirection, MatchTimeNow(), WobbleSeedTurns);
	LiveRocket = Rocket;
	return Rocket;
}

void UTraceAbilitySetRoxie::ApplySelfLaunch()
{
	if (CVarRoxieSelfLaunch.GetValueOnAnyThread() == 0)
	{
		return;   // RED ARM: the rocket still fires, she simply does not move.
	}
	if (!ShouldDriveMovement())
	{
		return;
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* Move = GetMovement();
	if (MyPawn == nullptr || Move == nullptr)
	{
		return;
	}

	const FVector AimDirection = MyPawn->GetAimDirection().GetSafeNormal();
	if (AimDirection.IsNearlyZero())
	{
		return;
	}

	const float Impulse = TraceRoxieRocket::GetSelfLaunchImpulse();
	const FVector Launch = (-AimDirection) * Impulse
	                     + FVector::UpVector * (Impulse * TraceRoxieRocket::GetSelfLaunchUpBias());

	if (Move->IsMovingOnGround() && Launch.Z > 0.f)
	{
		// Without this she is shoved along the floor and friction eats it in half a second, which is
		// the opposite of "fast and FAR". Same line Oyster's jar jump needs, for the same reason.
		Move->SetMovementMode(MOVE_Falling);
	}

	FVector NewVelocity = Move->Velocity;

	// THE PLANAR HALF IS A REPLACEMENT, NOT AN ADDITION — "launches her BACKWARDS" has to beat whatever
	// she was already doing, and adding would let a forward sprint cancel most of a 35 s ability.
	//
	// ...EXCEPT WHEN THE SHOT IS ESSENTIALLY VERTICAL, which is the rocket jump: aiming at the floor
	// makes -Aim point almost straight up, so the launch's planar part is near zero and replacing with
	// it would DELETE her run. A Roxie who rocket-jumps out of a sprint must land further away than one
	// who did it standing still. Below a tenth of the impulse the planar velocity is left alone.
	const FVector LaunchPlanar(Launch.X, Launch.Y, 0.f);
	if (LaunchPlanar.Size() >= Impulse * 0.1f)
	{
		NewVelocity.X = Launch.X;
		NewVelocity.Y = Launch.Y;
	}

	// A FLOOR on the vertical, not an assignment: a Roxie already rising must not be SLOWED by firing.
	// Same shape and the same argument as Rocco's second jump.
	NewVelocity.Z = FMath::Max(NewVelocity.Z, Launch.Z);

	Move->Velocity = NewVelocity;
}

float UTraceAbilitySetRoxie::ApplyRocketDamageTo(ATraceCharacter* Victim)
{
	if (!HasAuthority() || Victim == nullptr)
	{
		return 0.f;
	}

	// *** THE CHOKE POINT. SPEC §4. THE FOUNDING INVARIANT. ***
	//
	// DealDamage forwards to UTraceAbilityComponent::ApplyAbilityDamage, whose FIRST act is
	// CanAffectTargetDetailed(Target, Damage) — the one function in this project that answers "may an
	// ability touch this player". It returns 0 for a Core carrier unconditionally, with no knob and no
	// exception, and also for a team-mate while friendly fire is off and for the dead.
	//
	// There is NO carrier test in this file and none in ATraceRoxieRocket, deliberately: spec §4 asks
	// for one choke point rather than fifteen call sites each remembering, and a flat 100 that ignores
	// hit zones is the single worst thing to have a second opinion about.
	//
	// bHeadshot AND bMelee ARE BOTH FALSE, ALWAYS. §2: "100 damage on impact, ANYWHERE ON THE BODY — no
	// headshot/body distinction." Nothing in this feature ever resolves a hit zone, so there is no
	// place a zone could leak in.
	const float Dealt = DealDamage(Victim, TraceRoxieRocket::GetDamage(), TraceRoxieRocket::GetKillCause(),
		/*bMelee*/ false, /*bHeadshot*/ false);

	if (Dealt <= 0.f)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Roxie] rocket refused on %s (carrier, team-mate or dead) — it flies ON rather than "
			     "detonating, so an immune body cannot become a rocket shield."),
			*GetNameSafe(Victim));
	}

	return Dealt;
}

ATraceRoxieRocket* UTraceAbilitySetRoxie::DebugFireRocket(float WobbleSeedTurns, bool bAlsoSelfLaunch)
{
	if (!HasAuthority())
	{
		return nullptr;
	}

	ATraceRoxieRocket* Rocket = SpawnRocket(WobbleSeedTurns);
	if (bAlsoSelfLaunch)
	{
		ApplySelfLaunch();
	}
	return Rocket;
}

void UTraceAbilitySetRoxie::DebugClearRocketCooldown()
{
	PredictedRocketReadyMatchTime = 0.f;
	if (HasAuthority())
	{
		MutableState().AuxEndMatchTime = 0.f;
		MarkStateDirty();
	}
}

// =================================================================================================
// ACTIVATED — MODDED
// =================================================================================================

float UTraceAbilitySetRoxie::GetActivatedCooldownSeconds() const
{
	// Read at the point of use, never cached — the settings page is live-editable during PIE and the
	// HUD's cooldown ring asks this every frame it draws. It is also the number the select card prints
	// and the number Trace.VerifyCharacterData section D compares against, so the two cannot drift.
	return FMath::Clamp(UTraceSettings::Get().RoxieModdedCooldownSeconds, 0.f, 180.f);
}

bool UTraceAbilitySetRoxie::IsModdedActive() const
{
	return (State().Flags & TraceRoxieFlags::ModdedActive) != 0;
}

float UTraceAbilitySetRoxie::GetModdedRemainingSeconds() const
{
	if (!IsModdedActive())
	{
		return 0.f;
	}
	return FMath::Max(0.f, State().EffectEndMatchTime - MatchTimeNow());
}

float UTraceAbilitySetRoxie::GetFireIntervalScale() const
{
	if (!IsModdedActive())
	{
		return 1.f;
	}

	// *** THE INVERSION. *** §2 asks for "fire rate x1.65"; FireInterval is a PERIOD, so the interval is
	// DIVIDED by the rate. Returning the reciprocal here means the call site multiplies, which is the
	// operation that cannot be got backwards by accident. Multiplying an interval by 1.65 would make
	// her fire SLOWER while every label said faster — an inversion that reads in a playtest as "the
	// ability does nothing" rather than as a bug.
	const float RateMultiplier = FMath::Clamp(UTraceSettings::Get().RoxieModdedFireRateMultiplier, 0.1f, 5.f);
	return 1.f / RateMultiplier;
}

bool UTraceAbilitySetRoxie::IsFullAutoForced() const
{
	return IsModdedActive() && UTraceSettings::Get().bRoxieModdedFullAuto;
}

float UTraceAbilitySetRoxie::GetAddedRecoilScale() const
{
	// SPEC v29 §2e — "Roxie's modded should add recoil now."
	//
	// A MULTIPLE OF UTraceSettings::RecoilPitchPerShot, never a number of degrees: the weapon
	// multiplies the base kick by it, so retuning the base retunes her trade in proportion. This is
	// the same rule spec v24 §0 imposed on GetFireIntervalScale() above, applied to the second thing
	// MODDED now modifies. Clamped for the same reason every other seam in this file is: a mistyped
	// ini must degrade to a hard gun, never to a view that leaves the screen.
	if (!IsModdedActive())
	{
		return 0.f;
	}
	return FMath::Clamp(UTraceSettings::Get().RoxieModdedRecoilScale, 0.f, 8.f);
}

bool UTraceAbilitySetRoxie::CanActivate(FText& OutReason) const
{
	if (IsModdedActive())
	{
		OutReason = NSLOCTEXT("Trace", "RoxieModdedAlreadyIn", "MODDED CLIP ALREADY IN");
		return false;
	}
	return true;
}

bool UTraceAbilitySetRoxie::ActivateAbility()
{
	// Runs on the server AND on the owning client (prediction). On a client MutableState() returns a
	// scratch object, so the client's flag arrives by replication one round trip later — which is the
	// right trade here: nothing MODDED does is instantaneous enough for a client to notice the delay,
	// and mis-predicting the DEADLINE would let a client believe in fire rate it had not been granted.
	if (HasAuthority())
	{
		const float Now = MatchTimeNow();
		const float Duration = FMath::Clamp(UTraceSettings::Get().RoxieModdedDurationSeconds, 0.1f, 60.f);

		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags |= TraceRoxieFlags::ModdedActive;
		Writable.EffectEndMatchTime = Now + Duration;
		MarkStateDirty();

		// THE "ONE CLIP" HALF, and it READS the v16 ammo system rather than writing to it. See the
		// header for why MODDED does not do what X's Sting does: an ability-loaded clip locks out the
		// manual reload (a deliberate v16 §1 rule), which costs X five rounds of patience and would
		// cost Roxie up to twenty-nine every time the 5 s expired with most of a clip left.
		ModdedClipTracked = -1;
		if (const ATraceCharacter* MyPawn = GetCharacter())
		{
			if (const UTraceWeaponComponent* MyWeapon = MyPawn->Weapon)
			{
				ModdedClipTracked = MyWeapon->GetClipAmmo();
			}
		}

		UE_LOG(LogTraceGame, Log,
			TEXT("[Roxie] MODDED: fire interval x%.3f (rate x%.2f), full auto %d, for %.1fs or until the "
			     "clip of %d is gone (whichever first). Cooldown %.0fs."),
			GetFireIntervalScale(), UTraceSettings::Get().RoxieModdedFireRateMultiplier,
			IsFullAutoForced() ? 1 : 0, Duration, ModdedClipTracked, GetActivatedCooldownSeconds());
	}

	return true;
}

void UTraceAbilitySetRoxie::TickModded()
{
	if (!HasAuthority() || !IsModdedActive())
	{
		return;
	}

	// "OR 5 SECONDS" — the timer half. Absolute match-clock time, like every timer in the framework.
	if (MatchTimeNow() >= State().EffectEndMatchTime)
	{
		EndModded(TEXT("its 5 s ran out"));
		return;
	}

	// "ONE CLIP" — §2's [ASSUMPTION], and bRoxieModdedEndsOnReload is its switch. OFF leaves the timer
	// as the only end condition, which is the stronger reading and is one tick box away.
	if (!UTraceSettings::Get().bRoxieModdedEndsOnReload)
	{
		return;
	}

	const ATraceCharacter* MyPawn = GetCharacter();
	const UTraceWeaponComponent* MyWeapon = (MyPawn != nullptr) ? MyPawn->Weapon : nullptr;
	if (MyWeapon == nullptr)
	{
		// No gun to watch (mid-teardown, a fixture). "I cannot see the clip" is not "the clip is gone",
		// so the timer keeps the end condition rather than this guessing.
		return;
	}

	if (MyWeapon->IsReloading())
	{
		EndModded(TEXT("a reload started, and that is a new clip"));
		return;
	}

	const int32 ClipNow = MyWeapon->GetClipAmmo();

	if (ModdedClipTracked >= 0 && ClipNow > ModdedClipTracked)
	{
		// A RISING count is the only unambiguous signal that the clip was REPLACED — a falling one is
		// just her firing. This catches the refill that lands between two ability ticks, which
		// IsReloading() above can miss at 20 Hz.
		EndModded(TEXT("the clip was refilled"));
		return;
	}

	if (ModdedClipTracked > 0 && ClipNow <= 0)
	{
		EndModded(TEXT("the clip ran dry — that is 'one clip'"));
		return;
	}

	ModdedClipTracked = ClipNow;
}

void UTraceAbilitySetRoxie::EndModded(const TCHAR* Why)
{
	if (!HasAuthority() || !IsModdedActive())
	{
		return;
	}

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags &= static_cast<uint8>(~TraceRoxieFlags::ModdedActive);
	Writable.EffectEndMatchTime = 0.f;
	MarkStateDirty();

	ModdedClipTracked = -1;

	UE_LOG(LogTraceGame, Log, TEXT("[Roxie] MODDED ended: %s. The gun is back to its ordinary rate."), Why);
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetRoxie::OnEquipped()
{
	// Runs on EVERY machine (the component builds the set from the replicated CharacterId), which is
	// what makes the jump profile land on the owning client as well as on the server. OnPawnSpawned is
	// server-only, so this — plus the per-tick re-assert — is the whole of the client's story.
	ApplyJumpProfile();
}

void UTraceAbilitySetRoxie::OnUnequipped()
{
	RestoreJumpProfile();

	if (ATraceRoxieRocket* Rocket = LiveRocket.Get())
	{
		// Her rocket goes with her character. Left flying, it would be an ability effect whose owner no
		// longer exists — the orphaned-world-effect case spec §4 names.
		Rocket->Destroy();
	}
	LiveRocket = nullptr;

	PredictedRocketReadyMatchTime = 0.f;
	ModdedClipTracked = -1;
}

void UTraceAbilitySetRoxie::OnPawnSpawned()
{
	// The pawn is new, so its JumpZVelocity is the authored default again. Server-side only; the
	// owning client reaches the same state through the per-tick re-assert.
	ApplyJumpProfile();
}

void UTraceAbilitySetRoxie::OnPawnDied()
{
	// MODDED dies with her: it is a property of a gun she no longer has, and a player who respawned
	// mid-buff with three seconds of it left would have no way to know why their rate changed back.
	// THE COOLDOWNS DO NOT — spec §5 is explicit that they keep counting through death, and neither
	// the framework's E deadline nor AuxEndMatchTime is touched here.
	EndModded(TEXT("she died"));

	// A rocket already in the air is deliberately LEFT FLYING. Its damage still routes through her
	// ability component (which lives on the PlayerState and survives the pawn), so the choke point is
	// still asked; and a rocket that vanished at the moment its shooter was killed would make trading
	// with Roxie feel like the game rescinding a shot that had already left the tube.
}

void UTraceAbilitySetRoxie::OnHalfTime()
{
	// The framework has already cleared the E cooldown and Reset() the net state — which zeroes
	// AuxEndMatchTime, so V is ready too. What is left is the local mirror and the world actor.
	PredictedRocketReadyMatchTime = 0.f;
	ModdedClipTracked = -1;

	if (ATraceRoxieRocket* Rocket = LiveRocket.Get())
	{
		Rocket->Destroy();
	}
	LiveRocket = nullptr;
}

void UTraceAbilitySetRoxie::TickAbilities(float DeltaSeconds)
{
	// EVERY MACHINE, 20 Hz. The re-assert is what gets the passive onto an owning client (OnPawnSpawned
	// is server-only) and onto a pawn whose movement component was not ready when OnEquipped ran.
	ApplyJumpProfile();

	if (!HasAuthority())
	{
		return;
	}

	TickModded();

	// Keep the replicated "a rocket is in the air" bit honest for the HUD and for proxies. Nothing
	// gameplay-critical reads it — the rocket actor is itself replicated — so it is a display fact.
	const bool bRocketAlive = LiveRocket.IsValid();
	const bool bFlagSet = (State().Flags & TraceRoxieFlags::RocketInFlight) != 0;
	if (bRocketAlive != bFlagSet)
	{
		FTraceAbilityNetState& Writable = MutableState();
		if (bRocketAlive)
		{
			Writable.Flags |= TraceRoxieFlags::RocketInFlight;
		}
		else
		{
			Writable.Flags &= static_cast<uint8>(~TraceRoxieFlags::RocketInFlight);
		}
		MarkStateDirty();
	}
}

// =================================================================================================
// THE EVIDENCE
//
// Four console commands. Each one arms its own falsification FIRST — there is no way to run any of
// them that skips the red half — because a harness that has never failed is not a harness.
//
//   Trace.Roxie.RocketCarrierTest  *** THE FOUNDING INVARIANT. *** Fires the rocket's OWN damage call
//                                  at a live enemy Core carrier with the rule removed (which MUST
//                                  reach them, or the fixture never got near the carrier and its
//                                  green means nothing), then again with the shipped rule — which
//                                  must deal exactly zero while the SAME call KILLS a non-carrier
//                                  enemy in the same fixture, on the same frame.
//   Trace.Roxie.RocketFlightTest   "It WOBBLES in flight, deliberately inaccurate". Pure arithmetic
//                                  on the shipped path function; needs no world. Red arm:
//                                  Trace.Roxie.RocketWobble 0, which must produce a dead-straight line.
//   Trace.Roxie.JumpTest           "Jumps 15% higher" — measured as a HEIGHT ratio, which is the only
//                                  way the sqrt-vs-linear mistake is visible. Red arm:
//                                  Trace.Roxie.JumpPassive 0.
//   Trace.Roxie.ModdedTest         The 25 s, the 5 s, "one clip", and an HONEST measurement of the
//                                  x1.65 that reports it as NOT WIRED rather than passing.
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace TraceRoxieVerify
{
	/** Named after its file, not anonymous: an unnamed namespace collides under the unity build. */

	struct FChecklist
	{
		int32 Passed = 0;
		int32 Failed = 0;
		bool  bInvalid = false;
		FString InvalidReason;
		const TCHAR* Tag = TEXT("ROXIE");

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
	 * Makes the first HUMAN player Roxie and returns her set.
	 *
	 * A HUMAN, and it must be: handing a character to a BOT would fight ATraceGameMode's 4 Hz character
	 * fill for ownership of that player state, and the run would measure whichever won. Same reasoning
	 * TraceXVerify and the Sting-clip fixture both give.
	 */
	UTraceAbilitySetRoxie* MakePlayerIntoRoxie(UWorld* WorldPtr, FString& OutWhy)
	{
		if (WorldPtr == nullptr)
		{
			OutWhy = TEXT("no world");
			return nullptr;
		}
		if (!UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
		{
			OutWhy = TEXT("characters are DISABLED in this match (mode A, or the §3 toggle) — run this in mode B");
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
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
			if (Comp->GetCharacterId() != ETraceCharacterId::Roxie)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::Roxie);
			}
			if (UTraceAbilitySetRoxie* RoxieSet = Comp->GetAbilitySetAs<UTraceAbilitySetRoxie>())
			{
				return RoxieSet;
			}
			OutWhy = TEXT("ServerSetCharacter(Roxie) did not take — most likely a LIVING TEAM-MATE already "
			              "holds Roxie (per-team uniqueness). Run this early, before the bots have filled.");
			return nullptr;
		}

		OutWhy = TEXT("no human player controller with a pawn");
		return nullptr;
	}

	/** A living ENEMY of @p Roxie's pawn, optionally excluding one actor. */
	ATraceCharacter* FindLivingEnemy(UWorld* WorldPtr, const UTraceAbilityComponent* RoxieComp,
	                                 const ATraceCharacter* Exclude)
	{
		if (WorldPtr == nullptr || RoxieComp == nullptr)
		{
			return nullptr;
		}
		const ETraceTeam RoxieTeam = RoxieComp->GetTeam();
		const ATraceCharacter* RoxiePawn = RoxieComp->GetOwningCharacter();

		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == RoxiePawn || Candidate == Exclude || !Candidate->IsAlive())
			{
				continue;
			}
			// A genuine ENEMY. A team-mate would be refused by the SameTeam clause, which would make a
			// zero on the carrier strike prove nothing about the carrier rule.
			if (RoxieTeam == ETraceTeam::None || Candidate->GetTeam() == RoxieTeam
				|| Candidate->GetTeam() == ETraceTeam::None)
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	// =============================================================================================
	// Trace.Roxie.RocketCarrierTest — THE FOUNDING INVARIANT
	// =============================================================================================

	struct FCarrierTestState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;

		TWeakObjectPtr<ATraceCharacter> Carrier;
		TWeakObjectPtr<ATraceCharacter> Control;

		float RedArmCarrierDamage = -1.f;
		float GreenArmCarrierDamage = -1.f;
		float GreenArmControlDamage = -1.f;
		float CarrierHealthBefore = -1.f;
		float ControlHealthBefore = -1.f;
		float CarrierHealthAfter = -1.f;
		bool  bControlDied = false;
		bool  bRedReproduced = false;
	};

	void RunCarrierTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ROXIECARRIER] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FCarrierTestState> State = MakeShared<FCarrierTestState>();
		State->List.Tag = TEXT("ROXIECARRIER");
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROXIECARRIER] ===== spec v18 §2 + §4: Roxie's rocket deals %.0f 'anywhere on the body', and "
			     "NO ABILITY MAY DAMAGE A CORE CARRIER. arm 0 = RED (Trace.Ability.CarrierImmune 0): the "
			     "rocket MUST reach the carrier, or nothing below is measuring the rule. arm 1 = GREEN "
			     "(shipped): zero on the carrier, and the SAME call must KILL a non-carrier enemy. ====="),
			TraceRoxieRocket::GetDamage());

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				SetArm(TEXT("Trace.Ability.CarrierImmune"), 1);
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetRoxie* RoxieSet = MakePlayerIntoRoxie(TickWorld, Why);
			UTraceAbilityComponent* RoxieComp = (RoxieSet != nullptr) ? RoxieSet->GetAbilityComponent() : nullptr;
			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);

			if (RoxieSet == nullptr || RoxieComp == nullptr || CoreActor == nullptr)
			{
				if (State->Step == 0 && NowReal <= State->Deadline)
				{
					return true;   // still staging
				}
				State->List.Invalidate(FString::Printf(TEXT("could not stage Roxie (%s) or find the Core"), *Why));
				State->List.Report();
				SetArm(TEXT("Trace.Ability.CarrierImmune"), 1);
				return false;
			}

			// ---- step 0: stage a live ENEMY carrier and a live ENEMY control target ---------------
			if (State->Step == 0)
			{
				ATraceCharacter* NewCarrier = FindLivingEnemy(TickWorld, RoxieComp, nullptr);
				ATraceCharacter* NewControl = FindLivingEnemy(TickWorld, RoxieComp, NewCarrier);

				if (NewCarrier == nullptr || NewControl == nullptr)
				{
					if (NowReal <= State->Deadline)
					{
						return true;
					}
					State->List.Invalidate(TEXT("need TWO living enemies of Roxie — one to carry the Core and "
					                            "one to prove the fixture reaches health at all"));
					State->List.Report();
					SetArm(TEXT("Trace.Ability.CarrierImmune"), 1);
					return false;
				}

				// THE CARRIER MUST BE AN ENEMY. If it were a team-mate, the SameTeam clause would refuse
				// the strike and a zero would prove nothing about the carrier rule.
				CoreActor->TryPickup(NewCarrier);

				State->Carrier = NewCarrier;
				State->Control = NewControl;
				State->Step = 1;
				State->NextStepRealTime = NowReal + 0.25;

				UE_LOG(LogTraceGame, Display,
					TEXT("[ROXIECARRIER] staged: Roxie %s (team %d) | CARRIER %s (team %d) | CONTROL %s (team %d)"),
					*GetNameSafe(RoxieSet->GetCharacter()), static_cast<int32>(RoxieComp->GetTeam()),
					*GetNameSafe(NewCarrier), static_cast<int32>(NewCarrier->GetTeam()),
					*GetNameSafe(NewControl), static_cast<int32>(NewControl->GetTeam()));
				return true;
			}

			ATraceCharacter* Carrier = State->Carrier.Get();
			ATraceCharacter* Control = State->Control.Get();
			if (Carrier == nullptr || Control == nullptr)
			{
				State->List.Invalidate(TEXT("a participant went away mid-test"));
				State->List.Report();
				SetArm(TEXT("Trace.Ability.CarrierImmune"), 1);
				return false;
			}

			// The Core must STAY on the carrier: bots pass and score, and a victim who stopped carrying
			// between staging and the strike would turn the case under test into an ordinary damage test
			// that passes for entirely the wrong reason.
			if (CoreActor->Carrier != Carrier)
			{
				CoreActor->TryPickup(Carrier);
			}

			// ---- step 1: THE RED ARM ---------------------------------------------------------------
			if (State->Step == 1)
			{
				SetArm(TEXT("Trace.Ability.CarrierImmune"), 0);

				State->RedArmCarrierDamage = RoxieSet->ApplyRocketDamageTo(Carrier);
				State->bRedReproduced = (State->RedArmCarrierDamage > 0.f);

				State->List.Check(State->bRedReproduced,
					TEXT("RED ARM: with the carrier rule REMOVED the rocket reaches the carrier"),
					FString::Printf(TEXT("the rocket's own damage call returned %.1f on a LIVE ENEMY CARRIER — "
					                     "so the fixture genuinely reaches a carrier and the green arm below "
					                     "measures the rule rather than a broken setup"),
						State->RedArmCarrierDamage));

				// Heal both back so the green arm starts from a comparable state and the red arm's
				// damage cannot be mistaken for the green arm's.
				if (Carrier->Health != nullptr) { Carrier->Health->ResetHealth(); }
				if (Control->Health != nullptr) { Control->Health->ResetHealth(); }

				SetArm(TEXT("Trace.Ability.CarrierImmune"), 1);
				State->Step = 2;
				State->NextStepRealTime = NowReal + 0.25;
				return true;
			}

			// ---- step 2: THE GREEN ARM — both strikes, same call, same frame -----------------------
			if (State->Step == 2)
			{
				State->CarrierHealthBefore = (Carrier->Health != nullptr) ? Carrier->Health->Health : -1.f;
				State->ControlHealthBefore = (Control->Health != nullptr) ? Control->Health->Health : -1.f;

				State->GreenArmCarrierDamage = RoxieSet->ApplyRocketDamageTo(Carrier);
				State->GreenArmControlDamage = RoxieSet->ApplyRocketDamageTo(Control);

				State->Step = 3;
				State->NextStepRealTime = NowReal + 0.20;
				return true;
			}

			// ---- step 3: measure -------------------------------------------------------------------
			State->CarrierHealthAfter = (Carrier->Health != nullptr) ? Carrier->Health->Health : -1.f;
			State->bControlDied = !Control->IsAlive();
			const float ControlHealthAfter = (Control->Health != nullptr) ? Control->Health->Health : -1.f;

			State->List.Check(State->GreenArmControlDamage > 0.f
				&& (State->ControlHealthBefore - ControlHealthAfter) > 0.01f,
				TEXT("THE FIXTURE PROVES ITSELF: the identical call KILLS a non-carrier enemy"),
				FString::Printf(TEXT("returned %.1f, health %.1f -> %.1f, alive=%d. If this were zero, the "
				                     "carrier's zero below would prove nothing at all"),
					State->GreenArmControlDamage, State->ControlHealthBefore, ControlHealthAfter,
					Control->IsAlive() ? 1 : 0));

			State->List.Check(State->bControlDied,
				TEXT("...and the rocket's damage is a whole health bar, so a normal enemy DIES to one"),
				FString::Printf(TEXT("control target alive=%d after %.1f damage against %.0f of health"),
					Control->IsAlive() ? 1 : 0, State->GreenArmControlDamage, State->ControlHealthBefore));

			State->List.Check(FMath::IsNearlyZero(State->GreenArmCarrierDamage),
				TEXT("*** THE FOUNDING INVARIANT: the rocket deals EXACTLY ZERO to the Core carrier ***"),
				FString::Printf(TEXT("the rocket's own damage call returned %.4f on the carrier (health %.1f -> "
				                     "%.1f) — and it returned %.1f on the SAME carrier one step ago with the "
				                     "rule removed"),
					State->GreenArmCarrierDamage, State->CarrierHealthBefore, State->CarrierHealthAfter,
					State->RedArmCarrierDamage));

			State->List.Check(Carrier->IsAlive(),
				TEXT("...and the carrier is still standing"),
				FString::Printf(TEXT("carrier alive=%d, still holding=%d"),
					Carrier->IsAlive() ? 1 : 0, (CoreActor->Carrier == Carrier) ? 1 : 0));

			State->List.Check(TraceAbility::GetCarrierAbilityDamageHitCount() == 0
				|| State->RedArmCarrierDamage > 0.f,
				TEXT("the framework's carrier-damage ALARM tells the same story"),
				FString::Printf(TEXT("alarm count %d (a non-zero count is expected here and only here: the RED "
				                     "arm deliberately damaged a carrier)"),
					TraceAbility::GetCarrierAbilityDamageHitCount()));

			if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — with Trace.Ability.CarrierImmune 0 "
				                            "the rocket was STILL refused, so the zero above may be any of a "
				                            "dozen things and is not evidence of the carrier rule"));
			}

			State->List.Report();
			SetArm(TEXT("Trace.Ability.CarrierImmune"), 1);
			return false;
		}));
	}

	FAutoConsoleCommand CmdRocketCarrierTest(
		TEXT("Trace.Roxie.RocketCarrierTest"),
		TEXT("Dev only, SERVER. Spec v18 §2 + §4: Roxie's 100-damage rocket must KILL a normal enemy and do "
		     "NOTHING to a Core carrier, in the same fixture, through the same call. Red-arms itself with "
		     "Trace.Ability.CarrierImmune 0 first."),
		FConsoleCommandDelegate::CreateStatic(&RunCarrierTest));

	// =============================================================================================
	// Trace.Roxie.RocketLiveFireTest — the whole V press, end to end, in a live match
	//
	// The carrier test above drives ApplyRocketDamageTo() directly, which is the right way to test the
	// choke point and is completely blind to everything BETWEEN the key and the damage: whether the
	// press is refused, whether an actor spawns, whether it flies, whether the sweep finds a body, and
	// whether Roxie is actually thrown backwards. This one presses V.
	//
	// IT DISARMS THE WOBBLE ON PURPOSE (Trace.Roxie.RocketWobble 0). A deliberately inaccurate rocket
	// makes a deterministic hit test impossible, and a flaky harness is worse than no harness — the
	// wobble is Trace.Roxie.RocketFlightTest's, measured there in uu. It also PINS the target onto the
	// rocket's own launch line every tick of the flight, because the target is a bot and a bot that
	// walks 180 uu during a 0.23 s flight would turn "the sweep is broken" and "the bot moved" into the
	// same red.
	// =============================================================================================

	struct FLiveFireState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		double FlightDeadline = 0.0;
		FChecklist List;

		TWeakObjectPtr<ATraceCharacter> Target;
		FVector PinLocation = FVector::ZeroVector;

		float RedBackwardsSpeed = -1.f;
		float GreenBackwardsSpeed = -1.f;
		float GreenUpSpeed = -1.f;
		float CooldownAfterFire = -1.f;
		float TargetHealthBefore = -1.f;
		float TargetHealthAfter = -1.f;
		bool  bRedRocketSpawned = false;
		bool  bGreenRocketSpawned = false;
		bool  bSecondPressRefused = false;
		bool  bTargetDied = false;
	};

	/** Distance down the launch line the target is pinned. Short, so the flight is a quarter second. */
	constexpr float LiveFireTargetRangeUU = 600.f;

	void RestoreLiveFireArms()
	{
		SetArm(TEXT("Trace.Roxie.RocketWobble"), 1);
		SetArm(TEXT("Trace.Roxie.SelfLaunch"), 1);
	}

	/** Points Roxie at @p At and returns the aim direction she will actually fire along. */
	FVector AimRoxieAt(ATraceCharacter* RoxiePawn, const FVector& At)
	{
		if (RoxiePawn == nullptr)
		{
			return FVector::ForwardVector;
		}
		if (AController* RoxieController = RoxiePawn->GetController())
		{
			RoxieController->SetControlRotation((At - RoxiePawn->GetPawnViewLocation()).Rotation());
		}
		return RoxiePawn->GetAimDirection().GetSafeNormal();
	}

	void RunLiveFireTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ROXIELIVE] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FLiveFireState> State = MakeShared<FLiveFireState>();
		State->List.Tag = TEXT("ROXIELIVE");
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROXIELIVE] ===== spec v18 §2, the whole V press: a rocket leaves the muzzle, flies, and "
			     "deals %.0f on impact, while Roxie is thrown backwards at %.0f uu/s (+%.0f%% up) on a %.0fs "
			     "cooldown. arm 0 = RED (Trace.Roxie.SelfLaunch 0): the rocket still fires and she does NOT "
			     "move. The wobble is disarmed here so the hit is deterministic; it is measured in "
			     "Trace.Roxie.RocketFlightTest. ====="),
			TraceRoxieRocket::GetDamage(), TraceRoxieRocket::GetSelfLaunchImpulse(),
			TraceRoxieRocket::GetSelfLaunchUpBias() * 100.f, TraceRoxieRocket::GetCooldownSeconds());

		SetArm(TEXT("Trace.Roxie.RocketWobble"), 0);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreLiveFireArms();
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetRoxie* RoxieSet = MakePlayerIntoRoxie(TickWorld, Why);
			UTraceAbilityComponent* RoxieComp = (RoxieSet != nullptr) ? RoxieSet->GetAbilityComponent() : nullptr;
			ATraceCharacter* RoxiePawn = (RoxieSet != nullptr) ? RoxieSet->GetCharacter() : nullptr;
			UTraceCharacterMovementComponent* RoxieMove = (RoxieSet != nullptr) ? RoxieSet->GetMovement() : nullptr;

			if (RoxieSet == nullptr || RoxieComp == nullptr || RoxiePawn == nullptr || RoxieMove == nullptr
				|| !RoxiePawn->IsAlive())
			{
				if (State->Step == 0 && NowReal <= State->Deadline)
				{
					return true;
				}
				State->List.Invalidate(FString::Printf(TEXT("could not stage or hold Roxie: %s"), *Why));
				State->List.Report();
				RestoreLiveFireArms();
				return false;
			}

			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);

			// ---- step 0: find a living enemy who is NOT the carrier --------------------------------
			if (State->Step == 0)
			{
				ATraceCharacter* Candidate = FindLivingEnemy(TickWorld, RoxieComp, nullptr);
				if (Candidate != nullptr && CoreActor != nullptr && CoreActor->Carrier == Candidate)
				{
					// A carrier target would be flown THROUGH by design, which is a different test.
					Candidate = FindLivingEnemy(TickWorld, RoxieComp, Candidate);
				}
				if (Candidate == nullptr)
				{
					if (NowReal <= State->Deadline)
					{
						return true;
					}
					State->List.Invalidate(TEXT("no living NON-CARRIER enemy of Roxie to shoot at"));
					State->List.Report();
					RestoreLiveFireArms();
					return false;
				}

				State->Target = Candidate;
				State->Step = 1;
				UE_LOG(LogTraceGame, Display, TEXT("[ROXIELIVE] staged: Roxie %s, target %s."),
					*GetNameSafe(RoxiePawn), *GetNameSafe(Candidate));
				return true;
			}

			ATraceCharacter* Target = State->Target.Get();
			if (Target == nullptr)
			{
				State->List.Invalidate(TEXT("the target went away mid-test"));
				State->List.Report();
				RestoreLiveFireArms();
				return false;
			}

			// ---- step 1: THE RED ARM — the rocket fires, she does not move -------------------------
			if (State->Step == 1)
			{
				SetArm(TEXT("Trace.Roxie.SelfLaunch"), 0);
				RoxieSet->DebugClearRocketCooldown();

				const FVector AimAt = RoxiePawn->GetPawnViewLocation()
				                    + RoxiePawn->GetActorForwardVector() * LiveFireTargetRangeUU;
				const FVector AimDirection = AimRoxieAt(RoxiePawn, AimAt);

				const FVector VelocityBefore = RoxieMove->Velocity;
				const bool bFired = RoxieSet->OnSecondaryPressed();
				const FVector VelocityAfter = RoxieMove->Velocity;

				State->bRedRocketSpawned = (RoxieSet->GetLiveRocket() != nullptr);
				State->RedBackwardsSpeed = static_cast<float>(
					FVector::DotProduct(VelocityAfter - VelocityBefore, -AimDirection));

				State->List.Check(bFired && State->bRedRocketSpawned,
					TEXT("V fires: the press is accepted and a rocket actor exists"),
					FString::Printf(TEXT("OnSecondaryPressed()=%d, rocket=%s"),
						bFired ? 1 : 0, *GetNameSafe(RoxieSet->GetLiveRocket())));

				State->List.Check(FMath::Abs(State->RedBackwardsSpeed) < 50.f,
					TEXT("RED ARM: Trace.Roxie.SelfLaunch 0 fires the rocket and leaves her standing"),
					FString::Printf(TEXT("%.1f uu/s of backwards velocity against the %.0f the shipped arm must "
					                     "produce — so the green arm below is measuring the launch"),
						State->RedBackwardsSpeed, TraceRoxieRocket::GetSelfLaunchImpulse()));

				if (ATraceRoxieRocket* RedRocket = RoxieSet->GetLiveRocket())
				{
					RedRocket->Destroy();
				}
				SetArm(TEXT("Trace.Roxie.SelfLaunch"), 1);
				RoxieSet->DebugClearRocketCooldown();

				State->Step = 2;
				State->NextStepRealTime = NowReal + 0.20;
				return true;
			}

			// ---- step 2: THE GREEN ARM — fire for real, and pin the target on the launch line -------
			if (State->Step == 2)
			{
				const FVector AimAt = RoxiePawn->GetPawnViewLocation()
				                    + RoxiePawn->GetActorForwardVector() * LiveFireTargetRangeUU;
				const FVector AimDirection = AimRoxieAt(RoxiePawn, AimAt);

				if (Target->Health != nullptr)
				{
					Target->Health->ResetHealth();
				}
				State->TargetHealthBefore = (Target->Health != nullptr) ? Target->Health->Health : -1.f;

				const FVector VelocityBefore = RoxieMove->Velocity;
				const bool bFired = RoxieSet->OnSecondaryPressed();
				const FVector VelocityAfter = RoxieMove->Velocity;
				const FVector VelocityDelta = VelocityAfter - VelocityBefore;

				State->bGreenRocketSpawned = (RoxieSet->GetLiveRocket() != nullptr);
				State->GreenBackwardsSpeed = static_cast<float>(FVector::DotProduct(VelocityDelta, -AimDirection));
				State->GreenUpSpeed = static_cast<float>(VelocityDelta.Z);
				State->CooldownAfterFire = RoxieSet->GetRocketCooldownRemaining();
				State->bSecondPressRefused = !RoxieSet->OnSecondaryPressed();

				ATraceRoxieRocket* Rocket = RoxieSet->GetLiveRocket();
				if (!bFired || Rocket == nullptr)
				{
					State->List.Invalidate(TEXT("the shipped arm did not produce a rocket at all"));
					State->List.Report();
					RestoreLiveFireArms();
					return false;
				}

				// PIN THE TARGET ONTO THE ROCKET'S OWN LAUNCH LINE. The actor origin of an ACharacter is
				// its capsule centre, so putting it on the line puts the capsule squarely on the path.
				State->PinLocation = Rocket->GetLaunchOrigin()
				                   + Rocket->GetLaunchDirection() * LiveFireTargetRangeUU;
				Target->SetActorLocation(State->PinLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);

				State->Step = 3;
				State->FlightDeadline = NowReal + 2.0;
				return true;
			}

			// ---- step 3: hold the target on the line until the rocket resolves ---------------------
			if (State->Step == 3)
			{
				ATraceRoxieRocket* Rocket = RoxieSet->GetLiveRocket();
				if (Rocket != nullptr && NowReal < State->FlightDeadline)
				{
					Target->SetActorLocation(State->PinLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
					return true;
				}
				State->Step = 4;
				State->NextStepRealTime = NowReal + 0.15;
				return true;
			}

			// ---- step 4: measure -------------------------------------------------------------------
			State->TargetHealthAfter = (Target->Health != nullptr) ? Target->Health->Health : -1.f;
			State->bTargetDied = !Target->IsAlive();
			const float HealthLost = State->TargetHealthBefore - State->TargetHealthAfter;

			const float Impulse = TraceRoxieRocket::GetSelfLaunchImpulse();

			State->List.Check(State->GreenBackwardsSpeed > Impulse * 0.8f,
				TEXT("*** 'LAUNCHES HER BACKWARDS, FAST AND FAR' — she is thrown opposite her aim ***"),
				FString::Printf(TEXT("%.0f uu/s backwards against a knob of %.0f (the dash is 3300 and a wall "
				                     "jump throws 360, so this is firmly a movement ability)"),
					State->GreenBackwardsSpeed, Impulse));

			State->List.Check(State->GreenUpSpeed > Impulse * TraceRoxieRocket::GetSelfLaunchUpBias() * 0.5f,
				TEXT("...and upward as well, which is what turns 'fast' into 'far'"),
				FString::Printf(TEXT("%.0f uu/s of lift against %.0f asked for (%.0f%% of the impulse); without "
				                     "air time a horizontal shove is eaten by ground friction in half a second"),
					State->GreenUpSpeed, Impulse * TraceRoxieRocket::GetSelfLaunchUpBias(),
					TraceRoxieRocket::GetSelfLaunchUpBias() * 100.f));

			State->List.Check(HealthLost >= TraceRoxieRocket::GetDamage() - 0.5f || State->bTargetDied,
				TEXT("the rocket FLEW to the target and dealt its damage on impact"),
				FString::Printf(TEXT("over %.0f uu: target health %.1f -> %.1f (lost %.1f against the %.0f "
				                     "knob), alive=%d — this is the whole path: press, spawn, flight, server "
				                     "sweep, choke point, health"),
					LiveFireTargetRangeUU, State->TargetHealthBefore, State->TargetHealthAfter, HealthLost,
					TraceRoxieRocket::GetDamage(), Target->IsAlive() ? 1 : 0));

			State->List.Check(FMath::IsNearlyEqual(State->CooldownAfterFire,
				TraceRoxieRocket::GetCooldownSeconds(), 0.25f),
				TEXT("'35 S COOLDOWN' — and it is V's own, not MODDED's 25 s on E"),
				FString::Printf(TEXT("%.2fs remaining right after the shot against a knob of %.1fs; MODDED's is "
				                     "%.1fs and is untouched"),
					State->CooldownAfterFire, TraceRoxieRocket::GetCooldownSeconds(),
					RoxieSet->GetActivatedCooldownSeconds()));

			State->List.Check(State->bSecondPressRefused,
				TEXT("...and a second press inside the cooldown is refused rather than double-firing"),
				FString::Printf(TEXT("the immediate second OnSecondaryPressed() returned %d"),
					State->bSecondPressRefused ? 0 : 1));

			State->List.Report();
			RestoreLiveFireArms();
			return false;
		}));
	}

	FAutoConsoleCommand CmdRocketLiveFireTest(
		TEXT("Trace.Roxie.RocketLiveFireTest"),
		TEXT("Dev only, SERVER. Spec v18 §2: presses V for real — the rocket spawns, flies, sweeps and damages "
		     "on impact, and Roxie is thrown backwards on a 35 s cooldown. Red-armed with "
		     "Trace.Roxie.SelfLaunch 0."),
		FConsoleCommandDelegate::CreateStatic(&RunLiveFireTest));

	// =============================================================================================
	// Trace.Roxie.RocketFlightTest — "it WOBBLES in flight, deliberately inaccurate and hard to aim"
	//
	// Pure arithmetic on the SHIPPED path function, so it needs no world, no pawn and no match — which
	// is the point: "hard to aim" becomes a lateral deviation measured in uu that a designer can retune
	// against, rather than an adjective nobody can check.
	// =============================================================================================

	void RunFlightTest()
	{
		FChecklist List;
		List.Tag = TEXT("ROXIEFLIGHT");

		const FVector Origin(0.f, 0.f, 200.f);
		const FVector Direction = FVector::ForwardVector;
		const float Speed = TraceRoxieRocket::GetSpeedUU();
		const float Frequency = TraceRoxieRocket::GetWobbleFrequencyHz();
		const float Lifetime = TraceRoxieRocket::GetLifetimeSeconds();
		const float Seed = 0.37f;
		const int32 Samples = 240;

		auto MeasurePeakDeviation = [&](float Amplitude) -> float
		{
			float Peak = 0.f;
			for (int32 Index = 0; Index <= Samples; ++Index)
			{
				const float SampleTime = Lifetime * static_cast<float>(Index) / static_cast<float>(Samples);
				const FVector At = TraceRoxieRocket::GetPositionAtTime(Origin, Direction, SampleTime,
					Speed, Amplitude, Frequency, Seed);
				const FVector OnAimLine = Origin + Direction * (Speed * SampleTime);
				Peak = FMath::Max(Peak, static_cast<float>(FVector::Dist(At, OnAimLine)));
			}
			return Peak;
		};

		// ---- THE RED ARM: amplitude 0 is a straight, easily-aimed 100-damage rocket ----------------
		SetArm(TEXT("Trace.Roxie.RocketWobble"), 0);
		const float RedAmplitude = TraceRoxieRocket::GetWobbleAmplitudeUU();
		const float RedPeak = MeasurePeakDeviation(RedAmplitude);
		SetArm(TEXT("Trace.Roxie.RocketWobble"), 1);

		const float GreenAmplitude = TraceRoxieRocket::GetWobbleAmplitudeUU();
		const float GreenPeak = MeasurePeakDeviation(GreenAmplitude);

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROXIEFLIGHT] ===== spec v18 §2: 'it WOBBLES in flight, deliberately inaccurate and hard to "
			     "aim'. Speed %.0f uu/s, lifetime %.2fs (range %.0f uu), amplitude %.0f uu @ %.1f Hz. ====="),
			Speed, Lifetime, Speed * Lifetime, GreenAmplitude, Frequency);

		List.Check(RedAmplitude <= KINDA_SMALL_NUMBER && RedPeak < 1.f,
			TEXT("RED ARM: Trace.Roxie.RocketWobble 0 makes it fly DEAD STRAIGHT"),
			FString::Printf(TEXT("amplitude %.1f uu, peak deviation from the aim line %.3f uu over the whole "
			                     "flight — i.e. a straight, easily-aimed rocket, which is the build the green "
			                     "arm below has to differ from"),
				RedAmplitude, RedPeak));

		List.Check(GreenPeak > 20.f,
			TEXT("SHIPPED: the rocket leaves the aim line by a distance a player can miss with"),
			FString::Printf(TEXT("peak deviation %.1f uu (%.1fx the %.0f uu knob; the second, incommensurate "
			                     "term is why it exceeds the amplitude). At %.0f uu of range that is the whole "
			                     "of 'hard to aim'"),
				GreenPeak, (GreenAmplitude > 0.f) ? GreenPeak / GreenAmplitude : 0.f, GreenAmplitude,
				Speed * Lifetime));

		// IT MUST LEAVE THE MUZZLE ON THE CROSSHAIR. A rocket that spawns already displaced reads as the
		// gun being misaligned rather than as the rocket being wild — a different and much worse bug.
		const FVector AtZero = TraceRoxieRocket::GetPositionAtTime(Origin, Direction, 0.f, Speed,
			GreenAmplitude, Frequency, Seed);
		List.Check(AtZero.Equals(Origin, 0.01f),
			TEXT("it leaves the muzzle exactly ON the crosshair and wanders off it"),
			FString::Printf(TEXT("position at t=0 is (%s) against a muzzle at (%s)"),
				*AtZero.ToCompactString(), *Origin.ToCompactString()));

		// THE SEED MUST MATTER, or every rocket traces the identical sine and a player simply learns to
		// lead it — "hard to aim" would be true for one week.
		const float MidTime = Lifetime * 0.5f;
		const FVector SeedA = TraceRoxieRocket::GetPositionAtTime(Origin, Direction, MidTime, Speed,
			GreenAmplitude, Frequency, 0.10f);
		const FVector SeedB = TraceRoxieRocket::GetPositionAtTime(Origin, Direction, MidTime, Speed,
			GreenAmplitude, Frequency, 0.60f);
		List.Check(FVector::Dist(SeedA, SeedB) > 20.f,
			TEXT("two rockets with different seeds are in DIFFERENT places at the same instant"),
			FString::Printf(TEXT("%.1f uu apart at t=%.2fs — this is what stops the arc being learnable, and "
			                     "the seed is rolled per shot on the server"),
				FVector::Dist(SeedA, SeedB), MidTime));

		// DETERMINISM: the same arguments must give the same answer, or the server's hit test and a
		// client's visual are two different rockets.
		const FVector RepeatA = TraceRoxieRocket::GetPositionAtTime(Origin, Direction, MidTime, Speed,
			GreenAmplitude, Frequency, 0.10f);
		List.Check(RepeatA.Equals(SeedA, 0.001f),
			TEXT("the path is DETERMINISTIC — the server's hit test and every client's visual agree"),
			TEXT("the same arguments produced the same point twice; nothing in the path reads a local clock, "
			     "a random stream or a frame time"));

		// ---- DEMO 17 item 3: "IT NOW HAS TRAVEL TIME, SO IT CAN MISS, BE DODGED, AND BE WATCHED" ----
		//
		// *** THE SPEC'S [DIAGNOSED] "it was built as an instant trace" DOES NOT MATCH THIS BUILD. ***
		// The rocket is already a spawned, replicated ATraceRoxieRocket that walks GetPositionAtTime()
		// every frame and resolves hits with a swept segment; nothing about it is hitscan, and this
		// section exists to say so with numbers rather than with a claim. What Demo 17 asked for that was
		// genuinely missing is the SIZE, which is now derived from the hit radius (see ApplyVisualSize).
		//
		// "Dodgeable" is made a measurement rather than an adjective: at a realistic engagement range,
		// how long is the rocket in the air, and can a player running at the shipped ground speed clear
		// its lethal cross-section in that time? If they can, it can be dodged; if they cannot, it is a
		// hitscan with a delay.
		{
			const float EngagementRangeUU = 2000.f;                  // a long lane on this arena
			const float TimeOfFlight = EngagementRangeUU / FMath::Max(1.f, Speed);
			const float LethalHalfWidth = TraceRoxieRocket::GetHitRadiusUU() + 34.f;   // + a pawn capsule
			const float DodgeSpeed = FMath::Max(1.f, UTraceSettings::Get().WalkSpeed);
			const float SidestepUU = DodgeSpeed * TimeOfFlight;

			List.Check(TimeOfFlight > 0.25f,
				TEXT("DEMO 17: the rocket has REAL TRAVEL TIME — it is a projectile, not a trace"),
				FString::Printf(TEXT("%.2fs in the air over %.0f uu at %.0f uu/s. A hitscan would be 0.00s; "
				                     "this is a quarter of a second of flight a player can watch and react to"),
					TimeOfFlight, EngagementRangeUU, Speed));

			List.Check(SidestepUU > LethalHalfWidth,
				TEXT("DEMO 17: ...so it CAN BE DODGED — a running player clears the lethal width in the time "
				     "it takes to arrive"),
				FString::Printf(TEXT("%.0f uu of sidestep at %.0f uu/s against a %.0f uu lethal half-width "
				                     "(rocket %.0f + capsule 34)"),
					SidestepUU, DodgeSpeed, LethalHalfWidth, TraceRoxieRocket::GetHitRadiusUU()));

			// AND THE DRAWN SIZE, WHICH IS THE PART OF ITEM 3 THAT WAS ACTUALLY MISSING. The body is
			// sized off the hit radius, so "as wide as the thing that kills you" is a property of the
			// code rather than of two numbers somebody keeps in step by hand.
			const float DrawnDiameter = TraceRoxieRocket::GetHitRadiusUU() * TraceRoxieRocket::GetVisualScale() * 2.f;
			List.Check(DrawnDiameter >= TraceRoxieRocket::GetHitRadiusUU() * 2.f * 0.9f,
				TEXT("DEMO 17: the MODEL IS AS BIG AS THE HIT — 'make the model bigger, so it is easy to see'"),
				FString::Printf(TEXT("drawn %.0f uu across against a %.0f uu hit radius (x%.2f). It was a fixed "
				                     "13 uu before Demo 17, i.e. narrower than its own hit test"),
					DrawnDiameter, TraceRoxieRocket::GetHitRadiusUU(), TraceRoxieRocket::GetVisualScale()));
		}

		List.Report();
	}

	FAutoConsoleCommand CmdRocketFlightTest(
		TEXT("Trace.Roxie.RocketFlightTest"),
		TEXT("Dev only. Spec v18 §2: measures the rocket's wobble as a lateral deviation in uu against its "
		     "own aim line, red-armed with Trace.Roxie.RocketWobble 0 (which must fly straight). Pure "
		     "arithmetic — needs no world."),
		FConsoleCommandDelegate::CreateStatic(&RunFlightTest));

	// =============================================================================================
	// Trace.Roxie.JumpTest — "jumps 15% higher"
	// =============================================================================================

	struct FJumpTestState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;
		float BaseJumpZ = 0.f;
		float RedJumpZ = -1.f;
		float GreenJumpZ = -1.f;
	};

	void RunJumpTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ROXIEJUMP] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FJumpTestState> State = MakeShared<FJumpTestState>();
		State->List.Tag = TEXT("ROXIEJUMP");
		State->Deadline = FPlatformTime::Seconds() + 40.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROXIEJUMP] ===== spec v18 §2: 'jumps 15%% higher'. Measured as a HEIGHT ratio (v^2/2g), "
			     "because a velocity ratio cannot tell the correct sqrt(1.15)=1.0724 from the naive 1.15 — "
			     "which would buy +32.25%% height. arm 0 = RED (Trace.Roxie.JumpPassive 0). ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				SetArm(TEXT("Trace.Roxie.JumpPassive"), 1);
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetRoxie* RoxieSet = MakePlayerIntoRoxie(TickWorld, Why);
			if (RoxieSet == nullptr || RoxieSet->GetCharacter() == nullptr)
			{
				if (State->Step == 0 && NowReal <= State->Deadline)
				{
					return true;
				}
				State->List.Invalidate(FString::Printf(TEXT("could not stage Roxie: %s"), *Why));
				State->List.Report();
				SetArm(TEXT("Trace.Roxie.JumpPassive"), 1);
				return false;
			}

			// The profile is pushed from TickAbilities at 20 Hz, so every arm waits a beat rather than
			// reading a value that has not been asserted yet.
			if (State->Step == 0)
			{
				State->BaseJumpZ = RoxieSet->GetBaseJumpZVelocity();
				SetArm(TEXT("Trace.Roxie.JumpPassive"), 0);
				State->Step = 1;
				State->NextStepRealTime = NowReal + 0.30;
				return true;
			}

			if (State->Step == 1)
			{
				State->RedJumpZ = RoxieSet->GetAppliedJumpZVelocity();
				SetArm(TEXT("Trace.Roxie.JumpPassive"), 1);
				State->Step = 2;
				State->NextStepRealTime = NowReal + 0.30;
				return true;
			}

			State->GreenJumpZ = RoxieSet->GetAppliedJumpZVelocity();

			const float Bonus = FMath::Clamp(UTraceSettings::Get().RoxieJumpHeightBonus, 0.f, 2.f);
			const float VelocityRatio = (State->BaseJumpZ > 0.f) ? (State->GreenJumpZ / State->BaseJumpZ) : 0.f;
			const float HeightRatio = VelocityRatio * VelocityRatio;   // apex goes as v^2 / 2g
			const float NaiveHeightRatio = (1.f + Bonus) * (1.f + Bonus);

			State->List.Check(State->BaseJumpZ > 0.f
				&& FMath::IsNearlyEqual(State->RedJumpZ, State->BaseJumpZ, 0.5f),
				TEXT("RED ARM: Trace.Roxie.JumpPassive 0 leaves her jumping exactly like everybody else"),
				FString::Printf(TEXT("JumpZVelocity %.1f against the authored %.1f — so the green arm below is "
				                     "measuring the passive and not the engine default"),
					State->RedJumpZ, State->BaseJumpZ));

			State->List.Check(HeightRatio > 1.f + Bonus * 0.9f && HeightRatio < 1.f + Bonus * 1.1f,
				TEXT("SHIPPED: she jumps the asked-for fraction higher — measured as HEIGHT"),
				FString::Printf(TEXT("JumpZVelocity %.1f -> %.1f (velocity x%.4f), so apex x%.4f against the "
				                     "%.0f%% asked for"),
					State->BaseJumpZ, State->GreenJumpZ, VelocityRatio, HeightRatio, Bonus * 100.f));

			State->List.Check(HeightRatio < NaiveHeightRatio - 0.05f,
				TEXT("*** THE SQUARE ROOT: the naive x(1+bonus) on the VELOCITY is not what shipped ***"),
				FString::Printf(TEXT("velocity x%.4f = sqrt(1+%.2f); had it been x%.2f the apex would be x%.4f "
				                     "(+%.1f%%) instead of x%.4f. This is the Chut-bash mistake (spec v16 §0) "
				                     "not being made twice"),
					VelocityRatio, Bonus, 1.f + Bonus, NaiveHeightRatio, (NaiveHeightRatio - 1.f) * 100.f,
					HeightRatio));

			State->List.Report();
			SetArm(TEXT("Trace.Roxie.JumpPassive"), 1);
			return false;
		}));
	}

	FAutoConsoleCommand CmdJumpTest(
		TEXT("Trace.Roxie.JumpTest"),
		TEXT("Dev only, SERVER. Spec v18 §2: Roxie's passive, measured as an APEX HEIGHT ratio so the "
		     "sqrt-vs-linear mistake is visible. Red-armed with Trace.Roxie.JumpPassive 0."),
		FConsoleCommandDelegate::CreateStatic(&RunJumpTest));

	// =============================================================================================
	// Trace.Roxie.ModdedTest — the 25 s, the 5 s, "one clip", and the x1.65 that is NOT WIRED
	//
	// THIS COMMAND IS EXPECTED TO REPORT ONE FAILURE TODAY AND THAT IS THE POINT. Everything MODDED
	// owns on this side is proven; the fire-rate clause needs two lines in
	// Gameplay/TraceWeaponComponent.cpp, which this slice does not own, so the harness MEASURES the
	// gun's actual cadence and says so with numbers rather than passing on a promise.
	// =============================================================================================

	struct FModdedTestState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		double WindowStartReal = 0.0;
		FChecklist List;

		int32 ClipAtWindowStart = 0;
		int32 BaselineShots = 0;
		int32 ModdedShots = 0;
		float ModdedDeadlineAtCast = -1.f;
		float ModdedRemainingWhenClipEnded = -1.f;
		bool  bActiveDuringWindow = false;
		bool  bEndedByReload = false;
	};

	/** How long each fire window is held. Long enough for the two rates to be several rounds apart. */
	constexpr double ModdedFireWindowSeconds = 1.6;

	void RunModdedTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ROXIEMODDED] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FModdedTestState> State = MakeShared<FModdedTestState>();
		State->List.Tag = TEXT("ROXIEMODDED");
		State->Deadline = FPlatformTime::Seconds() + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROXIEMODDED] ===== spec v18 §2: 'loads a modded clip: the gun becomes full auto and fire "
			     "rate x%.2f. Lasts one clip OR %.0f seconds, whichever comes first.' Cooldown %.0fs. The "
			     "baseline window is held WITHOUT Modded and the second WITH it, on the same clip size and "
			     "the same wall clock — that pair IS the red/green arm for the rate. ====="),
			UTraceSettings::Get().RoxieModdedFireRateMultiplier,
			UTraceSettings::Get().RoxieModdedDurationSeconds,
			UTraceSettings::Get().RoxieModdedCooldownSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetRoxie* RoxieSet = MakePlayerIntoRoxie(TickWorld, Why);
			ATraceCharacter* RoxiePawn = (RoxieSet != nullptr) ? RoxieSet->GetCharacter() : nullptr;
			UTraceWeaponComponent* RoxieWeapon = (RoxiePawn != nullptr) ? RoxiePawn->Weapon : nullptr;

			// Re-validated EVERY step, not just at staging: a Roxie who died, picked up the Core or drew
			// the knife between two steps would make everything below a true statement about the wrong
			// situation.
			if (RoxieSet == nullptr || RoxiePawn == nullptr || RoxieWeapon == nullptr
				|| !RoxiePawn->IsAlive() || RoxiePawn->IsCarrier() || RoxieWeapon->IsKnifeEquipped())
			{
				if (State->Step == 0 && NowReal <= State->Deadline)
				{
					return true;
				}
				State->List.Invalidate(FString::Printf(
					TEXT("could not stage or hold Roxie (%s): pawn=%s alive=%d carrying=%d knife=%d"),
					*Why, *GetNameSafe(RoxiePawn),
					(RoxiePawn != nullptr && RoxiePawn->IsAlive()) ? 1 : 0,
					(RoxiePawn != nullptr && RoxiePawn->IsCarrier()) ? 1 : 0,
					(RoxieWeapon != nullptr && RoxieWeapon->IsKnifeEquipped()) ? 1 : 0));
				State->List.Report();
				if (RoxieWeapon != nullptr) { RoxieWeapon->StopFire(); }
				return false;
			}

			// ---- step 0: the cooldown, then start a BASELINE fire window ---------------------------
			if (State->Step == 0)
			{
				State->List.Check(FMath::IsNearlyEqual(RoxieSet->GetActivatedCooldownSeconds(),
					UTraceSettings::Get().RoxieModdedCooldownSeconds, 0.01f),
					TEXT("MODDED's cooldown is the 25 s the select card prints"),
					FString::Printf(TEXT("GetActivatedCooldownSeconds()=%.1f against the knob's %.1f — this is "
					                     "the pair Trace.VerifyCharacterData section D compares"),
						RoxieSet->GetActivatedCooldownSeconds(),
						UTraceSettings::Get().RoxieModdedCooldownSeconds));

				State->List.Check(FMath::IsNearlyEqual(RoxieSet->GetFireIntervalScale(), 1.f, 0.001f)
					&& !RoxieSet->IsModdedActive(),
					TEXT("with MODDED down the gun asks for its ordinary interval"),
					FString::Printf(TEXT("interval scale x%.4f, modded=%d"),
						RoxieSet->GetFireIntervalScale(), RoxieSet->IsModdedActive() ? 1 : 0));

				State->ClipAtWindowStart = RoxieWeapon->GetClipAmmo();
				State->WindowStartReal = NowReal;
				RoxieWeapon->StartFire();

				State->Step = 1;
				State->NextStepRealTime = NowReal + ModdedFireWindowSeconds;
				return true;
			}

			// ---- step 1: close the baseline window, reload, cast MODDED ----------------------------
			if (State->Step == 1)
			{
				RoxieWeapon->StopFire();
				State->BaselineShots = State->ClipAtWindowStart - RoxieWeapon->GetClipAmmo();

				RoxieWeapon->RequestReload();
				State->Step = 2;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.35;
				return true;
			}

			if (State->Step == 2)
			{
				const bool bActivated = RoxieSet->ActivateAbility();
				State->ModdedDeadlineAtCast = RoxieSet->GetModdedRemainingSeconds();

				State->List.Check(bActivated && RoxieSet->IsModdedActive(),
					TEXT("MODDED goes up on the cast"),
					FString::Printf(TEXT("ActivateAbility()=%d, active=%d, %.2fs on the clock"),
						bActivated ? 1 : 0, RoxieSet->IsModdedActive() ? 1 : 0, State->ModdedDeadlineAtCast));

				State->List.Check(FMath::IsNearlyEqual(State->ModdedDeadlineAtCast,
					UTraceSettings::Get().RoxieModdedDurationSeconds, 0.15f),
					TEXT("'OR 5 SECONDS' — the deadline is set to the knob, on the match clock"),
					FString::Printf(TEXT("%.2fs remaining at the cast against a knob of %.2fs"),
						State->ModdedDeadlineAtCast, UTraceSettings::Get().RoxieModdedDurationSeconds));

				const float Scale = RoxieSet->GetFireIntervalScale();
				const float Rate = FMath::Max(0.1f, UTraceSettings::Get().RoxieModdedFireRateMultiplier);
				State->List.Check(Scale < 1.f && FMath::IsNearlyEqual(Scale, 1.f / Rate, 0.001f),
					TEXT("the ability asks for a SHORTER interval, i.e. it DIVIDES rather than multiplies"),
					FString::Printf(TEXT("interval scale x%.4f = 1/%.2f, so %.3fs becomes %.3fs (%.0f RPM -> "
					                     "%.0f RPM). Multiplying would have made her fire SLOWER"),
						Scale, Rate, UTraceSettings::Get().FireInterval,
						UTraceSettings::Get().FireInterval * Scale,
						60.f / FMath::Max(0.01f, UTraceSettings::Get().FireInterval),
						60.f / FMath::Max(0.01f, UTraceSettings::Get().FireInterval * Scale)));

				State->ClipAtWindowStart = RoxieWeapon->GetClipAmmo();
				State->WindowStartReal = NowReal;
				RoxieWeapon->StartFire();

				State->Step = 3;
				State->NextStepRealTime = NowReal + ModdedFireWindowSeconds;
				return true;
			}

			// ---- step 3: close the MODDED window and compare the two cadences ----------------------
			if (State->Step == 3)
			{
				RoxieWeapon->StopFire();
				State->ModdedShots = State->ClipAtWindowStart - RoxieWeapon->GetClipAmmo();
				State->bActiveDuringWindow = RoxieSet->IsModdedActive();

				const float Rate = FMath::Max(0.1f, UTraceSettings::Get().RoxieModdedFireRateMultiplier);
				const float ExpectedShots = static_cast<float>(State->BaselineShots) * Rate;
				const bool bRateWired = (State->BaselineShots > 0)
					&& (static_cast<float>(State->ModdedShots) >= static_cast<float>(State->BaselineShots) * (1.f + (Rate - 1.f) * 0.5f));

				if (State->BaselineShots <= 0)
				{
					State->List.Invalidate(FString::Printf(
						TEXT("the BASELINE window fired nothing (%d rounds in %.2fs) — the gun never fired at "
						     "all, so the modded window measures nothing"),
						State->BaselineShots, ModdedFireWindowSeconds));
				}

				State->List.Check(bRateWired,
					TEXT("*** 'FIRE RATE x1.65' — the gun actually fires faster while MODDED is up ***"),
					FString::Printf(TEXT("%d rounds in %.2fs without MODDED, %d WITH it (expected ~%.1f). "
					                     "IF THIS FAILS, THE ABILITY SIDE IS FINE AND THE SEAM HAS BEEN BROKEN: "
					                     "UTraceWeaponComponent::CanFire() and ServerFire's rate validation must "
					                     "BOTH multiply UTraceSettings::FireInterval by "
					                     "UTraceAbilityComponent::GetFireIntervalScaleFor(), which dispatches to "
					                     "UTraceAbilitySetRoxie::GetFireIntervalScale() (currently x%.4f). Scaling "
					                     "only the local gate makes the server reject the extra shots as "
					                     "rate-limited, which reads as the gun eating bullets."),
						State->BaselineShots, ModdedFireWindowSeconds, State->ModdedShots, ExpectedShots,
						RoxieSet->GetFireIntervalScale()));

				State->List.Check(State->bActiveDuringWindow,
					TEXT("MODDED was still up for the whole measured window"),
					FString::Printf(TEXT("active=%d with %.2fs left — the window (%.2fs) is inside the %.1fs "
					                     "duration by construction"),
						State->bActiveDuringWindow ? 1 : 0, RoxieSet->GetModdedRemainingSeconds(),
						ModdedFireWindowSeconds, UTraceSettings::Get().RoxieModdedDurationSeconds));

				State->Step = 4;
				State->NextStepRealTime = NowReal + 0.15;
				return true;
			}

			// ---- step 4: "ONE CLIP" — a reload ends it, and it ends EARLY -------------------------
			if (State->Step == 4)
			{
				State->ModdedRemainingWhenClipEnded = RoxieSet->GetModdedRemainingSeconds();

				State->List.Check(RoxieSet->IsModdedActive(),
					TEXT("control: MODDED is STILL up before the reload, so the reload is what ends it"),
					FString::Printf(TEXT("active=%d, %.2fs of the %.1fs still to run"),
						RoxieSet->IsModdedActive() ? 1 : 0, State->ModdedRemainingWhenClipEnded,
						UTraceSettings::Get().RoxieModdedDurationSeconds));

				RoxieWeapon->RequestReload();
				State->Step = 5;
				State->NextStepRealTime = NowReal + 0.30;
				return true;
			}

			State->bEndedByReload = !RoxieSet->IsModdedActive();

			const bool bEndsOnReloadArmed = UTraceSettings::Get().bRoxieModdedEndsOnReload;
			State->List.Check(bEndsOnReloadArmed ? State->bEndedByReload : !State->bEndedByReload,
				bEndsOnReloadArmed
					? TEXT("'ONE CLIP' — reloading ends MODDED, and it ended BEFORE its 5 s")
					: TEXT("bRoxieModdedEndsOnReload is OFF, so only the 5 s ends it and the reload did not"),
				FString::Printf(TEXT("active after the reload=%d, and there were still %.2fs on the timer when "
				                     "the clip went — which is what makes 'whichever comes FIRST' a measured "
				                     "fact rather than a claim"),
					RoxieSet->IsModdedActive() ? 1 : 0, State->ModdedRemainingWhenClipEnded));

			State->List.Check(FMath::IsNearlyEqual(RoxieSet->GetFireIntervalScale(), 1.f, 0.001f),
				TEXT("...and the gun goes back to its ordinary interval the moment MODDED ends"),
				FString::Printf(TEXT("interval scale x%.4f"), RoxieSet->GetFireIntervalScale()));

			State->List.Report();
			RoxieWeapon->StopFire();
			return false;
		}));
	}

	FAutoConsoleCommand CmdModdedTest(
		TEXT("Trace.Roxie.ModdedTest"),
		TEXT("Dev only, SERVER. Spec v18 §2: MODDED's 25 s cooldown, its 5 s deadline, the 'one clip' end "
		     "condition, and a MEASURED comparison of the gun's cadence with and without it. Expected to "
		     "report the x1.65 clause as NOT WIRED until Gameplay/TraceWeaponComponent.cpp calls "
		     "GetFireIntervalScale()."),
		FConsoleCommandDelegate::CreateStatic(&RunModdedTest));
}

#endif // !UE_BUILD_SHIPPING
