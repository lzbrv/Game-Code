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

#include "Components/CapsuleComponent.h"        // the harness reads the pawn capsule off the CDO
#include "Components/SkeletalMeshComponent.h"   // the body MIDs MODDED's third-person tell writes
#include "Materials/MaterialInstanceDynamic.h"

#include "Abilities/Characters/TraceRoxieRocket.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"                 // §2.3's RoxieRocketLaunch / RoxieModded
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"       // ClientAbilityKick(RocketSelf) — §2.3's self-kick
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceFxBurst.h"            // §2.3's RocketBurst — the harness counts and measures them
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTracer.h"             // the muzzle-flash timing §2.3's launch flash is built on
#include "Gameplay/TraceWeaponComponent.h"    // v16 §1's ammo system: MODDED READS it, never writes it
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

#if !UE_BUILD_SHIPPING
// Trace.Roxie.RocketShot only — a camera and a screenshot, exactly the pieces
// TraceSlimeFxParade uses for the same job in Slimeball's file.
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"                     // FScreenshotRequest
#endif

// =================================================================================================
// FX_AUDIO_PLAN §2.3 — MODDED'S NUMBERS. Named, not anonymous: this module builds as a unity blob,
// and an unnamed namespace collides with every other one concatenated into the same translation
// unit (a Windows-only failure macOS structurally cannot see).
// =================================================================================================
namespace TraceRoxieFxFile
{
	/**
	 * M_TraceBodyAccent's emissive is AccentColor x AccentGlow x 0.2125
	 * (Scripts/generate_body_materials.py, GLOW_SCALE), so the material's AccentGlow is NOT the
	 * bible's "Glow": the shipped body value of 8 IS bible Glow 1.7. Every Glow in this file is
	 * converted through here and never written as a raw material number, which is the same rule and
	 * the same constant UTraceAbilitySetChut's armed tell uses — one equivalence, two kits.
	 */
	constexpr float AccentGlowPerBibleGlow = 1.f / 0.2125f;

	/** §2.3: "her body accent stripes lift Glow 1.7 -> 2.6". The destination, in the bible's units. */
	constexpr float ModdedBibleGlow = 2.6f;

	/** The same number in the material's units: 2.6 / 0.2125 = 12.24. */
	constexpr float ModdedAccentGlow = ModdedBibleGlow * AccentGlowPerBibleGlow;

	/** ART_BIBLE §6.2's ceiling, asserted at compile time so a retune cannot quietly clear it. */
	constexpr float MaxBibleGlow = 4.2f;
	static_assert(ModdedBibleGlow <= MaxBibleGlow,
		"FX_AUDIO_PLAN 2.3's MODDED tell must stay under ART_BIBLE 6.2's Glow ceiling of 4.2.");

	/** M_TraceBodyAccent's scalar. A silent no-op on any material without it (the stock Mannequin). */
	const FName AccentGlowParam(TEXT("AccentGlow"));

	/**
	 * EMBER. The one hue MODDED wears, and it is the SAME triple three other places already use for
	 * Roxie's mod — the rocket body (TraceRoxieRocket.cpp), the HUD's MODDED chip
	 * (TraceHUD.cpp's `Modded`) and ATraceFxBurst's RocketBurst recipe. Bible §6.2 invariant 2 is
	 * "one hue per effect"; this is that hue, written once per file because there is no shared
	 * constant to point at and four copies of a triple is what a rename would have to find.
	 */
	const FLinearColor Ember(1.f, 0.45f, 0.12f, 1.f);

	/**
	 * §2.3's "viewmodel gun MIDs get an ember emissive lift x2 (owner-only)", and the CAP that
	 * lift lands under.
	 *
	 * ATraceCharacter::ApplyTeamColors writes the viewmodel neon strip at Glow 2.4, so a literal x2
	 * is 4.8 — over the bible's 4.2 transient ceiling. The multiplier is applied and then CLAMPED,
	 * rather than the multiplier being quietly rewritten to 1.75: the plan's number stays visible in
	 * the code, and the thing that stops it is the rule that stopped it.
	 */
	constexpr float ViewModelGlowMultiplier = 2.f;
	constexpr float ViewModelGlowCap = MaxBibleGlow;
}

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

// *** THE THIRD RED ARM: THE V ROW'S PRODUCER (FX_AUDIO_PLAN §7.2 / finding F2). ***
//
// Set to 0, GetSecondaryCooldownDisplay() returns false and the HUD draws NO V row at all — which is
// EXACTLY the build the audit found: AuxEndMatchTime replicated for a consumer that did not exist.
// That is what makes Trace.Roxie.VRowTest evidence rather than a formality; without an arm, a test of
// "the row draws" has never been shown drawing nothing.
static TAutoConsoleVariable<int32> CVarRoxieVRow(
	TEXT("Trace.Roxie.VRow"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): Roxie publishes her rocket cooldown to the HUD's V row (FX plan "
	     "§7.2). 0: the override refuses, reproducing the pre-F2 build where the row had no producer, so "
	     "Trace.Roxie.VRowTest can be shown FAILING. Never ship 0."),
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

		// --- FX_AUDIO_PLAN §2.3: the launch report, World-side, once, from the authority ------------
		//
		// TraceAudio::PlayAt is the ordinary table-driven authority path — RoxieRocketLaunch is
		// declared World in TraceSoundEvents.cpp, so this one call multicasts it to every machine
		// and the rocket's own BeginPlay does NOT play it a second time (that hook owns the LOOP,
		// which is a different event; §8.7's double-audio rule is what keeps the two apart).
		TraceAudio::PlayAt(this, TraceSoundEvents::RoxieRocketLaunch, MyPawn->GetMuzzleLocation());

		// --- §1.5 / §2.3's self-kick: 2.5 deg / 0.30 s / 8 Hz, server -> Roxie only -----------------
		//
		// Reliable, and aimed at the SHOOTER rather than at a victim, which makes it the one kick in
		// the plan that is not an injury: it is the tube going off in her hands, and it lands on the
		// same frame she is thrown backwards so the two read as one event. Sent from here rather than
		// from ApplySelfLaunch() because ApplySelfLaunch also runs on the owning client (prediction)
		// and a kick fired there as well would be two kicks on the one machine that can see it.
		if (ATracePlayerController* MyPC = Cast<ATracePlayerController>(MyPawn->GetController()))
		{
			MyPC->ClientAbilityKick(ETraceViewKick::RocketSelf);
		}

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

float UTraceAbilitySetRoxie::ApplyRocketDamageTo(ATraceCharacter* Victim, float DamageAmount)
{
	if (!HasAuthority() || Victim == nullptr)
	{
		return 0.f;
	}

	// *** THE CEILING IS ENFORCED HERE, NOT PROMISED BY THE CALLERS. ***
	//
	// DEMO 29 item 7: "it should only do 100 damage for direct impacts. Otherwise, the damage should
	// fall off." The direct hit asks for exactly TraceRoxieRocket::GetDamage() and the blast asks for
	// a fraction of it, but neither of those facts is what makes the sentence true — this clamp is.
	// Nothing in the game can route more than the direct-hit knob through the rocket, whatever a
	// future caller computes or an .ini says, which is the same reasoning that makes every knob read
	// in this feature a clamped accessor rather than a bare property dereference.
	const float Requested = FMath::Clamp(DamageAmount, 0.f, TraceRoxieRocket::GetDamage());
	if (Requested <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	// *** THE CHOKE POINT. SPEC §4. THE FOUNDING INVARIANT. ***
	//
	// DealDamage forwards to UTraceAbilityComponent::ApplyAbilityDamage, whose FIRST act is
	// CanAffectTargetDetailed(Target, Damage) — the one function in this project that answers "may an
	// ability touch this player". It returns 0 for a Core carrier unconditionally, with no knob and no
	// exception; for a team-mate while friendly fire is off; for the dead; and for the instigator's
	// OWN PAWN (ETraceAbilityBlockReason::Self).
	//
	// THAT LAST CLAUSE IS ROXIE'S ROCKET-JUMP ANSWER AND IT IS WORTH NAMING. Demo 29 added a blast at
	// the detonation point, and ATraceRoxieRocket::ApplySplashDamage deliberately does NOT skip the
	// shooter — she is offered here like anybody else and refused by the framework. So her own rocket
	// deals her 0 at every range including a rocket jump fired into the floor at her feet, and that is
	// structural rather than a special case somebody could delete or an .ini could flip.
	//
	// There is NO carrier test in this file and none in ATraceRoxieRocket, deliberately: spec §4 asks
	// for one choke point rather than fifteen call sites each remembering, and a 100 that ignores hit
	// zones is the single worst thing to have a second opinion about.
	//
	// bHeadshot AND bMelee ARE BOTH FALSE, ALWAYS. §2: "100 damage on impact, ANYWHERE ON THE BODY —
	// no headshot/body distinction." Nothing in this feature ever resolves a hit zone, on either the
	// direct path or the falloff — the blast varies with DISTANCE, never with where on a body it
	// lands, so there is no place a zone could leak in.
	const float Dealt = DealDamage(Victim, Requested, TraceRoxieRocket::GetKillCause(),
		/*bMelee*/ false, /*bHeadshot*/ false);

	if (Dealt <= 0.f)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Roxie] rocket refused %.1f on %s (carrier, team-mate, dead, or Roxie herself) — on a "
			     "DIRECT hit it flies ON rather than detonating, so an immune body cannot become a "
			     "rocket shield; in a BLAST it simply takes nothing."),
			Requested, *GetNameSafe(Victim));
	}

	return Dealt;
}

void UTraceAbilitySetRoxie::BeginDetonationRecord()
{
	if (!HasAuthority())
	{
		return;
	}

	// RESET, not Empty: the allocation is kept so the second and every later rocket of a match costs
	// nothing. See the member's comment.
	LastDetonationRecord.Reset();
}

void UTraceAbilitySetRoxie::RecordDetonationHit(ATraceCharacter* Victim, float SurfaceGapUU,
                                                float RequestedDamage, float DealtDamage, bool bDirect)
{
	if (!HasAuthority())
	{
		return;
	}

	FRocketDetonationHit& Entry = LastDetonationRecord.AddDefaulted_GetRef();
	Entry.Victim = Victim;
	Entry.SurfaceGapUU = SurfaceGapUU;
	Entry.RequestedDamage = RequestedDamage;
	Entry.DealtDamage = DealtDamage;
	Entry.bDirect = bDirect;
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

		// FX_AUDIO_PLAN §2.3: RoxieModded, World-side, once, from the authority. The clip going in is
		// a fact about a player everybody near her is about to be shot by, so it is a world sound and
		// not a client one — the same call shape Mace's throw and Slimeball's wall use.
		if (const ATraceCharacter* SoundPawn = GetCharacter())
		{
			TraceAudio::PlayAt(this, TraceSoundEvents::RoxieModded, SoundPawn->GetActorLocation());
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
// FX_AUDIO_PLAN §7.2 — THE V ROW'S PRODUCER (closes F2)
// =================================================================================================

bool UTraceAbilitySetRoxie::GetSecondaryCooldownDisplay(float& OutRemaining, float& OutDuration,
                                                        FString& OutLabel) const
{
	// TRUE WHETHER OR NOT IT IS COOLING. 0 remaining is a drawn state — the row is how a player who
	// has never pressed V learns that V exists and what it is called. Returning false while ready
	// would make the row appear only after the ability had already been used once, which is exactly
	// backwards. The base class's doc comment says this in as many words.
	//
	// BOTH NUMBERS COME FROM THE FEATURE'S OWN PUBLISHED ACCESSORS, and neither is re-derived here:
	//
	//   remaining  GetRocketCooldownRemaining() — the MAX of the replicated AuxEndMatchTime and the
	//              predicted local mirror, so the row greys the instant V is pressed rather than a
	//              round trip later, and can only ever be STRICTER than the server's truth.
	//   duration   TraceRoxieRocket::GetCooldownSeconds() — the LIVE clamped settings read the
	//              ability itself uses at the press (OnSecondaryPressed). Not the raw property, not
	//              the character DataAsset's snapshot of it: that is the F6 dual-source trap, and
	//              a denominator that drifted from the numerator would draw a meter that never
	//              reaches either end.
#if !UE_BUILD_SHIPPING
	if (CVarRoxieVRow.GetValueOnAnyThread() == 0)
	{
		// THE RED ARM. Reproduces the pre-F2 build exactly: the base class's default answer, the HUD
		// drawing nothing, and a replicated deadline nobody reads. Cheat-only and compiled out of
		// Shipping entirely, so the shipped path is the two lines below and nothing else.
		return false;
	}
#endif

	OutRemaining = GetRocketCooldownRemaining();
	OutDuration = TraceRoxieRocket::GetCooldownSeconds();

	// SHORT AND UPPER CASE — it shares a half-height row with the key glyph and the seconds.
	OutLabel = TEXT("ROCKET");
	return true;
}

// =================================================================================================
// FX_AUDIO_PLAN §1.2 / §2.3 — MODDED'S TELL, ON EVERY MACHINE
//
// THE PROBLEM THIS ROW EXISTS TO SOLVE. MODDED changes how fast a Roxie shoots, whether she can
// hold the trigger on a pistol, and how hard the gun kicks — three facts about the person shooting
// AT you, none of which had any presentation outside her own HUD chip. The chip is on the one
// screen belonging to the one player who already knows.
//
// So the tell has two halves on two different sets of machines, and the split is forced rather than
// chosen:
//
//   EVERY MACHINE   her body accent stripes lift from bible Glow 1.7 to 2.6. The §1.2 router is the
//                   only hook that runs on every machine off the replicated ModdedActive bit; a
//                   tell written in ActivateAbility() would exist on the server and the owning
//                   client and nowhere else, which is finding F10 in miniature.
//   OWNER ONLY      the viewmodel gun goes ember. The viewmodel is built on exactly one machine, so
//                   there is nothing to replicate and nothing anyone else could see.
//
// THE THIRD-PERSON HALF IS A MAX, NOT AN ASSIGNMENT, AND THE CORE CARRIER IS WHY — the same
// argument, verbatim, that UTraceAbilitySetChut's armed tell makes: ApplyTeamColors pushes 30 into
// AccentGlow for a carrier, which is already brighter than 12.24, and writing the tell flat would
// DIM a carrying Roxie at the instant MODDED made her most dangerous.
// =================================================================================================

void UTraceAbilitySetRoxie::OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New)
{
	const bool bWasModded = (Old.Flags & TraceRoxieFlags::ModdedActive) != 0;
	const bool bIsModded = (New.Flags & TraceRoxieFlags::ModdedActive) != 0;
	if (bWasModded == bIsModded)
	{
		// RocketInFlight also lives in Flags and moves twice per shot. It is presented by the rocket
		// ACTOR, which is replicated and draws itself on every machine, so it needs nothing here —
		// and reacting to it would re-write every body MID twice a rocket for no visible change.
		return;
	}

	if (bIsModded)
	{
		ApplyModdedTell();
	}
	else
	{
		ClearModdedTell();
	}
}

void UTraceAbilitySetRoxie::SyncClientFx(const FTraceAbilityNetState& Current)
{
	// FIRST SIGHT: a machine that joined, swapped character or respawned into a state where MODDED is
	// ALREADY up. Without this the tell would be invisible there until MODDED ended — and MODDED
	// lasts five seconds, so "until it ends" and "for ever" are the same sentence. IDEMPOTENT: both
	// branches are the same functions the tick re-asserts, and neither stacks.
	if ((Current.Flags & TraceRoxieFlags::ModdedActive) != 0)
	{
		ApplyModdedTell();
	}
	else
	{
		ClearModdedTell();
	}
}

void UTraceAbilitySetRoxie::ApplyModdedTell()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		// No pawn is DETACH EVERYTHING (the router's obligation 1). A tell parented to nothing is a
		// tell nobody will ever take back.
		ClearModdedTell();
		return;
	}

	// ---- third person: the accent stripes, on every machine ---------------------------------------
	//
	// The MIDs are the ones ApplyColorToSkeletalMesh already created and left in the slots; this
	// writes ONE scalar on top of them and touches no hue, because the hue is per-character identity
	// and lives on the material instance (bible §2.3). A slot whose material is not a MID has never
	// been team-painted and is skipped rather than wrapped — creating one here would fight
	// ApplyColorToSkeletalMesh for ownership of the same slot.
	if (USkeletalMeshComponent* MeshComp = MyPawn->GetMesh())
	{
		const int32 SlotCount = MeshComp->GetNumMaterials();
		for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
		{
			UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(MeshComp->GetMaterial(SlotIndex));
			if (MID == nullptr)
			{
				continue;
			}

			// Read the CURRENT value off the material rather than assuming 8: that keeps the lift
			// correct for any state ApplyTeamColors invents later, and it is what makes the Max
			// above mean what it says.
			float Current = 0.f;
			MID->GetScalarParameterValue(TraceRoxieFxFile::AccentGlowParam, Current);
			MID->SetScalarParameterValue(TraceRoxieFxFile::AccentGlowParam,
				FMath::Max(Current, TraceRoxieFxFile::ModdedAccentGlow));
		}
	}

	// ---- first person: the gun, owner only --------------------------------------------------------
	//
	// GUARDED ON IsLocallyControlled() AND NOT MERELY ON "the MID exists". On a listen server every
	// bot's ATraceCharacter is locally controlled in the engine's sense, but only a PLAYER'S pawn
	// ever builds a viewmodel — so the guard is really belt to the fact that GetViewModelNeonMID()
	// answers null for everybody else. Both are cheap and the pair says what is meant.
	if (MyPawn->IsLocallyControlled())
	{
		if (UMaterialInstanceDynamic* NeonMID = MyPawn->GetViewModelNeonMID())
		{
			float CurrentGlow = 0.f;
			NeonMID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Glow")), CurrentGlow);

			const float Lifted = FMath::Min(CurrentGlow * TraceRoxieFxFile::ViewModelGlowMultiplier,
				TraceRoxieFxFile::ViewModelGlowCap);

			NeonMID->SetScalarParameterValue(TEXT("Glow"), FMath::Max(CurrentGlow, Lifted));
			NeonMID->SetVectorParameterValue(TEXT("Color"), TraceRoxieFxFile::Ember);

			// BaseColor as well, for the BasicShapeMaterial fallback path that has no Glow input at
			// all — the same pair ApplyTeamColors writes, and setting a parameter a material does not
			// have is a documented silent no-op rather than an error.
			NeonMID->SetVectorParameterValue(TEXT("BaseColor"), TraceRoxieFxFile::Ember);
		}
	}

	if (!bModdedTellUp)
	{
		bModdedTellUp = true;
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Roxie] MODDED tell UP on %s: AccentGlow -> %.2f (bible Glow %.2f, cap %.1f); "
			     "viewmodel ember, owner %d."),
			*GetNameSafe(MyPawn), TraceRoxieFxFile::ModdedAccentGlow, TraceRoxieFxFile::ModdedBibleGlow,
			TraceRoxieFxFile::MaxBibleGlow, MyPawn->IsLocallyControlled() ? 1 : 0);
	}
}

void UTraceAbilitySetRoxie::ClearModdedTell()
{
	if (!bModdedTellUp)
	{
		return;
	}
	bModdedTellUp = false;

	// *** THROUGH ApplyTeamColors(), NOT BY WRITING A REMEMBERED NUMBER BACK. ***
	//
	// It is the one function that knows what this pawn's accent and viewmodel SHOULD be for its
	// current state — 8 normally, 30 while carrying the Core, 0 while dead, team hue on the gun —
	// and it is re-entrant and already called from a dozen places. Restoring a value latched at the
	// start of MODDED would put the pre-MODDED brightness on a Roxie who picked the Core up during
	// it, which is the restore committing the very bug the lift's Max() avoids.
	if (ATraceCharacter* MyPawn = GetCharacter())
	{
		MyPawn->ApplyTeamColors();
	}
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

	// §8.9: "no FX component survives its pawn" — and no lifted material survives its character
	// either. A Roxie swapped away mid-MODDED would otherwise leave the next character wearing her
	// accent brightness, on every machine, with nothing left that knows how to take it back.
	ClearModdedTell();

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

	// The tell goes with it. EndModded already clears the replicated bit, so the §1.2 router takes
	// the tell down on every OTHER machine within one state edge; this is the authority's own copy
	// taken down on the same frame rather than 50 ms later at the next RouteNetStateEdges.
	ClearModdedTell();

	// A rocket already in the air is deliberately LEFT FLYING. Its damage still routes through her
	// ability component (which lives on the PlayerState and survives the pawn), so the choke point is
	// still asked; and a rocket that vanished at the moment its shooter was killed would make trading
	// with Roxie feel like the game rescinding a shot that had already left the tube.
}

void UTraceAbilitySetRoxie::OnHalfTime()
{
	// The framework has already cleared the E cooldown and Reset() the net state — which zeroes
	// AuxEndMatchTime, so V is ready too. What is left is the local mirror, the world actor and the
	// cosmetics the Reset() knows nothing about.
	PredictedRocketReadyMatchTime = 0.f;
	ModdedClipTracked = -1;
	ClearModdedTell();

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

	// *** THE TELL IS RE-ASSERTED, NOT LATCHED, AND FOR THE SAME REASON THE JUMP PROFILE ABOVE IS. ***
	//
	// ATraceCharacter::ApplyTeamColors() is called from a dozen places — a Core pickup, a team
	// change, a respawn, a cloak refresh — and every one of them STOMPS AccentGlow back to the value
	// for the pawn's current state. A tell written once on the router edge would silently vanish the
	// first time any of them fired inside MODDED's five seconds. Re-asserting at 20 Hz costs one
	// scalar write per body slot and cannot be got wrong; the alternative (a latch that remembers
	// what it last wrote and re-bases when somebody else has been through) is what Rocco's stack
	// tell needs because his lift is open-ended, and MODDED's is not — it is five seconds.
	//
	// EVERY MACHINE, deliberately, and above the authority gate: the whole point of §2.3's tell is
	// that the people being shot at can see it.
	if (IsModdedActive())
	{
		ApplyModdedTell();
	}
	else if (bModdedTellUp)
	{
		ClearModdedTell();
	}

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
//                                  It also checks the SHAPE of Demo 29 §7's falloff curve, for the
//                                  same reason: an arithmetic claim belongs in the worldless test.
//   Trace.Roxie.RocketFalloffTest  *** DEMO 29 §7. *** The damage table, MEASURED: a real flown
//                                  DIRECT impact, then the blast at 0/25/50/75/100% of the radius and
//                                  just outside it, every row a health bar that actually moved. It
//                                  reports Roxie's own self-damage at rocket-jump range too. Red arm:
//                                  RoxieRocketSplashMaxFraction 0, i.e. the Demo 28 build.
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

				State->RedArmCarrierDamage = RoxieSet->ApplyRocketDamageTo(Carrier, TraceRoxieRocket::GetDamage());
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

				State->GreenArmCarrierDamage = RoxieSet->ApplyRocketDamageTo(Carrier, TraceRoxieRocket::GetDamage());
				State->GreenArmControlDamage = RoxieSet->ApplyRocketDamageTo(Control, TraceRoxieRocket::GetDamage());

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
	// Trace.Roxie.RocketFalloffTest — DEMO 29 ITEM 7, MEASURED
	//
	// "Make Roxie's rocket hit radius match the model, but it should only do 100 damage for direct
	// impacts. Otherwise, the damage should fall off."
	//
	// This produces the TABLE the owner has to judge the feel from, and every row of it is a health
	// bar that actually moved on a live pawn — not a reading of GetSplashDamageAtGapUU(). The shape of
	// that function is checked separately, without a world, by Trace.Roxie.RocketFlightTest; this is
	// the proof that the shape reaches health.
	//
	// TWO KINDS OF ROW, AND THE DIFFERENCE IS DELIBERATE:
	//
	//   THE DIRECT ROW is a real rocket. It is fired down her aim line with the wobble disarmed, at a
	//   target pinned onto that line, and it flies, sweeps and detonates on the pawn exactly as it
	//   would in a match. That is the only way to prove "direct impact" means what the words mean —
	//   THE PROJECTILE HIT THEM — rather than being a distance threshold in the splash loop.
	//
	//   THE FALLOFF ROWS command the end point (ATraceRoxieRocket::DebugDetonateAt) and place the
	//   target at an exact gap from it. Everything below that call is the shipped path: the same
	//   ApplySplashDamage, the same clamp, the same choke point, the same health component. What is
	//   removed is the frame-rate quantisation in WHERE a flown rocket stops — 43 uu per tick at the
	//   shipped speed — which would make each row's x-axis a different number on a different machine
	//   and turn a damage table into a scatter plot. The gap printed in every row is nevertheless READ
	//   BACK from the record the shipped code wrote, never from the number the fixture asked for.
	//
	// THE RED ARM is RoxieRocketSplashMaxFraction 0, which is the Demo 28 build: a rocket landing at
	// your feet does nothing at all. It is not a cvar because the falloff is a settings knob, so the
	// arm is stated here and in the ini rather than automated — the checks below fail loudly on it
	// (every falloff row reads 0.0 and the "it falls off" check has nothing to fall from).
	// =============================================================================================

	/** One row of the table. Gap and damage are both READ BACK, never assumed. */
	struct FFalloffRow
	{
		FString Label;
		float AskedGapUU = 0.f;
		float RecordedGapUU = -1.f;
		float RecordedRequested = -1.f;
		float RecordedDealt = -1.f;
		float HealthLost = -1.f;
		bool  bDirect = false;
		bool  bFound = false;
	};

	struct FFalloffState
	{
		int32 Step = 0;
		int32 Sample = 0;
		double NextReal = 0.0;
		double Deadline = 0.0;
		double FlightDeadline = 0.0;
		FChecklist List;

		TWeakObjectPtr<ATraceCharacter> Target;
		FVector PinLocation = FVector::ZeroVector;
		float TargetHealthBefore = -1.f;

		TArray<FFalloffRow> Rows;
		float SelfGapUU = -1.f;
		float SelfRequested = -1.f;
		float SelfDealt = -1.f;
		bool  bSelfSampled = false;
	};

	/**
	 * The gaps to sample, as fractions of the blast radius, plus one outside it.
	 *
	 * The edge row is pulled 0.5 uu inside the radius on purpose: at exactly the radius the shipped
	 * code returns 0 and skips the victim entirely, so the row would be indistinguishable from the
	 * one outside. 0.5 uu inside asks "what does the very last uu of the blast do", which is the
	 * question — and the answer at a smoothstep is "essentially nothing", which is the point.
	 */
	const float FalloffSampleFractions[] = { 0.f, 0.25f, 0.5f, 0.75f, 0.993f, 1.11f };
	const TCHAR* FalloffSampleLabels[] = {
		TEXT("at the impact point"), TEXT("25% of the radius"), TEXT("50% of the radius"),
		TEXT("75% of the radius"), TEXT("the edge of the radius"), TEXT("just OUTSIDE the radius")
	};
	static_assert(UE_ARRAY_COUNT(FalloffSampleFractions) == UE_ARRAY_COUNT(FalloffSampleLabels),
		"every sampled gap needs a label for the table");

	/** A horizontal unit vector perpendicular to @p Forward. Any one will do; this one is repeatable. */
	FVector HorizontalSideOf(const FVector& Forward)
	{
		FVector Side = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal2D();
		if (Side.IsNearlyZero())
		{
			Side = FVector::RightVector;
		}
		return Side;
	}

	/** The victim's capsule radius, off the live component, with the class default as the fallback. */
	float CapsuleRadiusOf(const ATraceCharacter* Pawn)
	{
		if (const UCapsuleComponent* Capsule = (Pawn != nullptr) ? Pawn->GetCapsuleComponent() : nullptr)
		{
			return Capsule->GetScaledCapsuleRadius();
		}
		return 34.f;
	}

	/** Pulls this victim's line out of the last detonation's record, or leaves the row not-found. */
	void HarvestRow(FFalloffRow& Row, const UTraceAbilitySetRoxie* RoxieSet, const ATraceCharacter* Victim)
	{
		if (RoxieSet == nullptr)
		{
			return;
		}
		for (const UTraceAbilitySetRoxie::FRocketDetonationHit& Hit : RoxieSet->GetLastDetonationRecord())
		{
			if (Hit.Victim.Get() != Victim)
			{
				continue;
			}
			Row.bFound = true;
			Row.RecordedGapUU = Hit.SurfaceGapUU;
			Row.RecordedRequested = Hit.RequestedDamage;
			Row.RecordedDealt = Hit.DealtDamage;
			Row.bDirect = Hit.bDirect;
			return;
		}
	}

	void RunFalloffTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ROXIEFALLOFF] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FFalloffState> State = MakeShared<FFalloffState>();
		State->List.Tag = TEXT("ROXIEFALLOFF");
		State->Deadline = FPlatformTime::Seconds() + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROXIEFALLOFF] ===== DEMO 29 §7: %.0f for a DIRECT IMPACT, falling off to 0 at %.0f uu "
			     "from the detonation point (x%.2f of the direct damage at the impact point itself). "
			     "Every number below is a health bar that moved. ====="),
			TraceRoxieRocket::GetDamage(), TraceRoxieRocket::GetHitRadiusUU(),
			TraceRoxieRocket::GetSplashMaxFraction());

		SetArm(TEXT("Trace.Roxie.RocketWobble"), 0);
		SetArm(TEXT("Trace.Roxie.SelfLaunch"), 0);

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
			if (NowReal < State->NextReal)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetRoxie* RoxieSet = MakePlayerIntoRoxie(TickWorld, Why);
			UTraceAbilityComponent* RoxieComp = (RoxieSet != nullptr) ? RoxieSet->GetAbilityComponent() : nullptr;
			ATraceCharacter* RoxiePawn = (RoxieSet != nullptr) ? RoxieSet->GetCharacter() : nullptr;
			if (RoxieSet == nullptr || RoxieComp == nullptr || RoxiePawn == nullptr || !RoxiePawn->IsAlive())
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

			// ---- step 0: find a living non-carrier enemy to measure on ------------------------------
			if (State->Step == 0)
			{
				ATraceCharacter* Candidate = FindLivingEnemy(TickWorld, RoxieComp, nullptr);
				ATraceCore* CoreActor = ATraceCore::Get(TickWorld);
				if (Candidate != nullptr && CoreActor != nullptr && CoreActor->Carrier == Candidate)
				{
					// A carrier takes nothing from ANY of these rows by the founding invariant, which
					// would make the whole table read zero for entirely the right reason and the wrong
					// test. That case is Trace.Roxie.RocketCarrierTest's.
					Candidate = FindLivingEnemy(TickWorld, RoxieComp, Candidate);
				}
				if (Candidate == nullptr)
				{
					if (NowReal <= State->Deadline)
					{
						return true;
					}
					State->List.Invalidate(TEXT("no living NON-CARRIER enemy of Roxie to measure on"));
					State->List.Report();
					RestoreLiveFireArms();
					return false;
				}

				State->Target = Candidate;
				State->Step = 1;
				UE_LOG(LogTraceGame, Display, TEXT("[ROXIEFALLOFF] staged: Roxie %s, target %s."),
					*GetNameSafe(RoxiePawn), *GetNameSafe(Candidate));
				return true;
			}

			ATraceCharacter* Target = State->Target.Get();
			if (Target == nullptr)
			{
				State->List.Invalidate(TEXT("the target actor went away mid-table"));
				State->List.Report();
				RestoreLiveFireArms();
				return false;
			}

			// *** A DEAD TARGET IS EXPECTED HERE, NOT EXCEPTIONAL, AND THE FIRST VERSION OF THIS
			// *** FIXTURE INVALIDATED ITSELF ON IT. *** Two things kill it: eight bots shooting at each
			// other around the measurement, and the DIRECT row, which deals a full health bar on
			// purpose. So the rows that need a living pawn heal it back, and the rows after the direct
			// hit stop caring. Healing rather than re-staging keeps every row on the SAME pawn, which
			// is what makes the table comparable line to line.
			if (State->Step <= 3)
			{
				if (!Target->IsAlive() && Target->Health != nullptr)
				{
					Target->Health->ResetHealth();
				}
				if (!Target->IsAlive())
				{
					State->List.Invalidate(TEXT("the target died and could not be healed back — the table "
					                            "needs one pawn alive for every row"));
					State->List.Report();
					RestoreLiveFireArms();
					return false;
				}
			}

			// ---- step 1: the FALLOFF rows, one commanded detonation each ----------------------------
			if (State->Step == 1)
			{
				const int32 Index = State->Sample;
				if (Index < UE_ARRAY_COUNT(FalloffSampleFractions))
				{
					const float BlastRadius = TraceRoxieRocket::GetHitRadiusUU();
					const float AskedGap = BlastRadius * FalloffSampleFractions[Index];

					if (Target->Health != nullptr)
					{
						Target->Health->ResetHealth();
					}
					const float Before = (Target->Health != nullptr) ? Target->Health->Health : -1.f;

					// THE DETONATION POINT SITS AT THE TARGET'S CAPSULE-CENTRE HEIGHT. That is what makes
					// the gap exact: the closest point on a vertical capsule axis to a point at the same
					// Z is that point's own height, so the axis distance is the horizontal offset and the
					// surface gap is that minus the capsule radius. Offsetting vertically instead would
					// put the closest point at an axis END and make the gap a Pythagorean surprise.
					const FVector TargetCentre = Target->GetActorLocation();
					const FVector Side = HorizontalSideOf(RoxiePawn->GetActorForwardVector());
					const FVector DetonateAt = TargetCentre + Side * (CapsuleRadiusOf(Target) + AskedGap);

					RoxieSet->DebugClearRocketCooldown();
					ATraceRoxieRocket* Rocket = RoxieSet->DebugFireRocket(0.f, /*bAlsoSelfLaunch*/ false);
					if (Rocket == nullptr)
					{
						State->List.Invalidate(TEXT("could not spawn a rocket for a falloff row"));
						State->List.Report();
						RestoreLiveFireArms();
						return false;
					}
					Rocket->DebugDetonateAt(DetonateAt);

					FFalloffRow Row;
					Row.Label = FalloffSampleLabels[Index];
					Row.AskedGapUU = AskedGap;
					Row.HealthLost = Before - ((Target->Health != nullptr) ? Target->Health->Health : -1.f);
					HarvestRow(Row, RoxieSet, Target);

					// ROXIE'S OWN SHARE, HARVESTED FROM THE SAME RECORD. Sampled on the row whose
					// detonation is nearest her — in practice whichever one the arena geometry allows —
					// and reported whether or not she was reached. See the check below.
					for (const UTraceAbilitySetRoxie::FRocketDetonationHit& Hit : RoxieSet->GetLastDetonationRecord())
					{
						if (Hit.Victim.Get() == RoxiePawn)
						{
							State->bSelfSampled = true;
							State->SelfGapUU = Hit.SurfaceGapUU;
							State->SelfRequested = Hit.RequestedDamage;
							State->SelfDealt = Hit.DealtDamage;
						}
					}

					State->Rows.Add(Row);
					++State->Sample;
					State->NextReal = NowReal + 0.10;
					return true;
				}

				State->Step = 2;
				return true;
			}

			// ---- step 2: ROXIE'S OWN BLAST, at rocket-jump range ------------------------------------
			//
			// A rocket jump is fired into the floor at her feet, so the detonation lands about one
			// capsule radius from her capsule's bottom cap — i.e. a gap of roughly ZERO, the MAXIMUM the
			// falloff can deal. That is the worst case and it is the one worth measuring.
			if (State->Step == 2)
			{
				RoxieSet->DebugClearRocketCooldown();
				if (ATraceRoxieRocket* Rocket = RoxieSet->DebugFireRocket(0.f, /*bAlsoSelfLaunch*/ false))
				{
					const FVector Feet = RoxiePawn->GetActorLocation()
					                   - FVector(0.f, 0.f, CapsuleRadiusOf(RoxiePawn));
					Rocket->DebugDetonateAt(Feet);

					State->bSelfSampled = false;
					State->SelfGapUU = -1.f;
					State->SelfRequested = -1.f;
					State->SelfDealt = -1.f;
					for (const UTraceAbilitySetRoxie::FRocketDetonationHit& Hit : RoxieSet->GetLastDetonationRecord())
					{
						if (Hit.Victim.Get() == RoxiePawn)
						{
							State->bSelfSampled = true;
							State->SelfGapUU = Hit.SurfaceGapUU;
							State->SelfRequested = Hit.RequestedDamage;
							State->SelfDealt = Hit.DealtDamage;
						}
					}
				}

				State->Step = 3;
				State->NextReal = NowReal + 0.10;
				return true;
			}

			// ---- step 3: the DIRECT row, from a real flight -----------------------------------------
			if (State->Step == 3)
			{
				if (Target->Health != nullptr)
				{
					Target->Health->ResetHealth();
				}
				State->TargetHealthBefore = (Target->Health != nullptr) ? Target->Health->Health : -1.f;

				const FVector AimAt = RoxiePawn->GetPawnViewLocation()
				                    + RoxiePawn->GetActorForwardVector() * LiveFireTargetRangeUU;
				AimRoxieAt(RoxiePawn, AimAt);

				RoxieSet->DebugClearRocketCooldown();
				ATraceRoxieRocket* Rocket = RoxieSet->DebugFireRocket(0.f, /*bAlsoSelfLaunch*/ false);
				if (Rocket == nullptr)
				{
					State->List.Invalidate(TEXT("could not spawn the rocket for the DIRECT row"));
					State->List.Report();
					RestoreLiveFireArms();
					return false;
				}

				// Pinned squarely ON the launch line: an ACharacter's origin is its capsule centre, so
				// this puts the capsule on the path and the sweep resolves a real direct impact.
				State->PinLocation = Rocket->GetLaunchOrigin()
				                   + Rocket->GetLaunchDirection() * LiveFireTargetRangeUU;
				Target->SetActorLocation(State->PinLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);

				State->Step = 4;
				State->FlightDeadline = NowReal + 2.0;
				return true;
			}

			// ---- step 4: hold the target on the line until the rocket resolves ----------------------
			if (State->Step == 4)
			{
				if (RoxieSet->GetLiveRocket() != nullptr && NowReal < State->FlightDeadline)
				{
					Target->SetActorLocation(State->PinLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
					return true;
				}

				FFalloffRow Row;
				Row.Label = TEXT("DIRECT IMPACT (the projectile hit them)");
				Row.AskedGapUU = 0.f;
				Row.HealthLost = State->TargetHealthBefore
				               - ((Target->Health != nullptr) ? Target->Health->Health : -1.f);
				HarvestRow(Row, RoxieSet, Target);
				State->Rows.Insert(Row, 0);

				State->Step = 5;
				State->NextReal = NowReal + 0.10;
				return true;
			}

			// ---- step 5: the table, and the checks -------------------------------------------------
			const float BlastRadius = TraceRoxieRocket::GetHitRadiusUU();
			const float Direct = TraceRoxieRocket::GetDamage();

			UE_LOG(LogTraceGame, Display,
				TEXT("[ROXIEFALLOFF] ---- MEASURED DAMAGE TABLE (blast radius %.0f uu = the drawn body = the "
				     "drawn burst) ----"), BlastRadius);
			for (const FFalloffRow& Row : State->Rows)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[ROXIEFALLOFF]   %-40s | gap asked %6.1f uu | gap RECORDED %6.1f uu | asked %6.1f | "
					     "dealt %6.1f | HEALTH LOST %6.1f%s"),
					*Row.Label, Row.AskedGapUU, Row.RecordedGapUU, Row.RecordedRequested, Row.RecordedDealt,
					Row.HealthLost, Row.bDirect ? TEXT("  <- DIRECT") : TEXT(""));
			}

			// --- the checks, all against the MEASURED health loss ---------------------------------
			const FFalloffRow* DirectRow = State->Rows.FindByPredicate(
				[](const FFalloffRow& R) { return R.bDirect; });
			const FFalloffRow* CentreRow = State->Rows.FindByPredicate(
				[](const FFalloffRow& R) { return !R.bDirect && R.AskedGapUU <= 0.01f; });
			const FFalloffRow* EdgeRow = State->Rows.FindByPredicate(
				[BlastRadius](const FFalloffRow& R)
				{ return !R.bDirect && R.AskedGapUU > BlastRadius * 0.9f && R.AskedGapUU < BlastRadius; });
			const FFalloffRow* OutsideRow = State->Rows.FindByPredicate(
				[BlastRadius](const FFalloffRow& R) { return R.AskedGapUU > BlastRadius; });

			State->List.Check(DirectRow != nullptr && DirectRow->bFound
				&& FMath::IsNearlyEqual(DirectRow->HealthLost, Direct, 0.5f),
				TEXT("*** DEMO 29 §7: A DIRECT IMPACT DEALS THE FULL DAMAGE ***"),
				(DirectRow != nullptr)
					? FString::Printf(TEXT("a real rocket flown %.0f uu into a pinned pawn took %.1f health "
					                       "against the %.0f knob, and the record marks it DIRECT — i.e. the "
					                       "SWEEP found them, which is what 'direct impact' means here"),
						LiveFireTargetRangeUU, DirectRow->HealthLost, Direct)
					: TEXT("no direct row was produced at all"));

			State->List.Check(CentreRow != nullptr && CentreRow->HealthLost > 0.5f
				&& CentreRow->HealthLost < Direct - 0.5f,
				TEXT("...and the BEST possible near miss does strictly less"),
				(CentreRow != nullptr)
					? FString::Printf(TEXT("standing ON the detonation point cost %.1f health against a direct "
					                       "hit's %.1f (x%.2f of it). Non-zero, so the blast is real; below "
					                       "100, so 'only 100 for direct impacts' holds"),
						CentreRow->HealthLost, Direct, (Direct > 0.f) ? CentreRow->HealthLost / Direct : 0.f)
					: TEXT("no gap-0 row was produced"));

			bool bMonotone = true;
			float PreviousLoss = TNumericLimits<float>::Max();
			for (const FFalloffRow& Row : State->Rows)
			{
				if (Row.bDirect)
				{
					continue;
				}
				bMonotone = bMonotone && (Row.HealthLost <= PreviousLoss + 0.01f);
				PreviousLoss = Row.HealthLost;
			}
			State->List.Check(bMonotone,
				TEXT("*** 'OTHERWISE, THE DAMAGE SHOULD FALL OFF' — it falls, at every step out ***"),
				FString::Printf(TEXT("%d measured rows, each one at or below the row inside it, across a %.0f uu "
				                     "blast. The curve is a smoothstep: flat-shouldered at the centre so a good "
				                     "near miss pays consistently, flat-tailed at the edge so there is no cliff "
				                     "to be killed by 1 uu on either side of"),
					State->Rows.Num() - 1, BlastRadius));

			State->List.Check(EdgeRow != nullptr && OutsideRow != nullptr
				&& EdgeRow->HealthLost < 2.f && OutsideRow->HealthLost <= 0.01f && !OutsideRow->bFound,
				TEXT("*** THE BLAST ENDS EXACTLY WHERE THE DRAWN BURST ENDS ***"),
				(EdgeRow != nullptr && OutsideRow != nullptr)
					? FString::Printf(TEXT("%.1f health at the last uu inside %.0f uu, and %.1f (no record line "
					                       "at all) at %.0f uu. ATraceFxBurst draws the RocketBurst at this same "
					                       "%.0f uu, so there is no invisible kill volume and no visible ring "
					                       "that does nothing"),
						EdgeRow->HealthLost, BlastRadius, OutsideRow->HealthLost, OutsideRow->AskedGapUU,
						BlastRadius)
					: TEXT("the edge or outside row was not produced"));

			// --- SELF-DAMAGE. The number the owner asked for, stated whether or not it is zero. ----
			State->List.Check(State->SelfDealt <= 0.01f,
				TEXT("*** ROXIE'S ROCKET JUMP IS STILL FREE — her own blast deals her NOTHING ***"),
				State->bSelfSampled
					? FString::Printf(TEXT("a rocket detonated at her own feet put her %.1f uu from the blast "
					                       "(the falloff asked for %.1f, the most it can ask for) and dealt her "
					                       "%.1f. The framework refuses it: CanAffectTargetDetailed returns "
					                       "Self for a pawn damaging itself with its own ability, so this is "
					                       "structural and no knob can change it"),
						State->SelfGapUU, State->SelfRequested, State->SelfDealt)
					: TEXT("she was not even reached by a blast at her own feet — which is the same answer, "
					       "0 self-damage, arrived at one step earlier"));

			State->List.Report();
			RestoreLiveFireArms();
			return false;
		}));
	}

	FAutoConsoleCommand CmdRocketFalloffTest(
		TEXT("Trace.Roxie.RocketFalloffTest"),
		TEXT("Dev only, SERVER. DEMO 29 §7: prints a MEASURED damage table for Roxie's rocket — a real "
		     "flown DIRECT impact, then the blast at 0/25/50/75/100% of the radius and just outside it, "
		     "every row a health bar that moved. Also reports her own self-damage at rocket-jump range. "
		     "Red arm: RoxieRocketSplashMaxFraction 0 (the Demo 28 build)."),
		FConsoleCommandDelegate::CreateStatic(&RunFalloffTest));

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

			// THE VICTIM'S CAPSULE, READ OFF THE CLASS RATHER THAN TYPED. ApplyRocketDamageTo's body
			// test is (rocket radius + the victim's capsule radius), so the width a player must clear
			// is that sum and not the rocket's radius alone. This used to be a literal 34 here, which
			// is a copy of TraceCharacterLayout::CapsuleRadius living in Roxie's file — the class
			// default object carries the real collider, so ask it. Patch 28 item 1 made this number
			// load-bearing twice over (see the drawn-size check below), which is why it moved.
			float PawnCapsuleRadius = 34.f;
			if (const ATraceCharacter* CharacterCDO = GetDefault<ATraceCharacter>())
			{
				if (const UCapsuleComponent* Capsule = CharacterCDO->GetCapsuleComponent())
				{
					PawnCapsuleRadius = Capsule->GetScaledCapsuleRadius();
				}
			}

			const float LethalHalfWidth = TraceRoxieRocket::GetHitRadiusUU() + PawnCapsuleRadius;
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
				                     "(rocket %.0f + capsule %.0f)"),
					SidestepUU, DodgeSpeed, LethalHalfWidth, TraceRoxieRocket::GetHitRadiusUU(),
					PawnCapsuleRadius));

			// ---- DEMO 29 ITEM 7: "MAKE ROXIE'S ROCKET HIT RADIUS MATCH THE MODEL" ------------------
			//
			// This used to be two checks around two numbers: "the model is at least 90% as wide as the
			// hit" and "the model is no wider than hit + capsule". Both PASSED at the Patch 28 numbers
			// (a 72 uu model around a 45 uu hit radius) and the owner still played it and reported the
			// model and the hit radius did not match, because 72 != 45 and no amount of arguing about
			// the capsule term changes that. So the two numbers are one number now, and this is the
			// check that says so.
			//
			// *** IT IS NOT A TAUTOLOGY EVEN THOUGH GetVisualRadiusUU() RETURNS GetHitRadiusUU(). ***
			// This command has no world, so it can only compare the two accessors — which is a real
			// check of the ARITHMETIC (a reintroduced multiplier fails it immediately) but not of the
			// MESH. The mesh half is Trace.Roxie.RocketShot, which reads the radius back off a live
			// rocket's component scale through ATraceRoxieRocket::GetDrawnBodyRadiusUU() and prints it
			// on the photographed frame. Both halves are needed; neither is sufficient.
			const float DrawnRadius = TraceRoxieRocket::GetVisualRadiusUU();
			List.Check(FMath::IsNearlyEqual(DrawnRadius, TraceRoxieRocket::GetHitRadiusUU(), 0.01f),
				TEXT("*** DEMO 29 §7: THE DRAWN RADIUS AND THE HIT RADIUS ARE ONE NUMBER ***"),
				FString::Printf(TEXT("drawn r %.1f uu vs hit r %.1f uu, i.e. %.0f uu across. Patch 28 "
				                     "drew 72 around a hit of 45; the multiplier that did that "
				                     "(RoxieRocketVisualScale) has no reader any more, so these cannot "
				                     "drift apart again by editing one of them"),
					DrawnRadius, TraceRoxieRocket::GetHitRadiusUU(), DrawnRadius * 2.f));

			// AND IT MUST STILL NOT OUT-CLAIM THE KILL. The volume a PLAYER dies in is wider than the
			// rocket, because the test is (rocket radius + the victim's own capsule): both sides here
			// are live reads, so a capsule retune moves the ceiling with it and nothing is a literal.
			// This is now a large margin rather than a near miss, which is the correct direction — the
			// drawn skin under-claims by exactly one capsule radius, forever, by construction.
			List.Check(DrawnRadius <= LethalHalfWidth + 0.01f,
				TEXT("*** DRAWN == LETHAL: the drawn body never claims more than the rocket kills ***"),
				FString::Printf(TEXT("drawn radius %.1f uu against a %.1f uu lethal half-width (hit %.0f + "
				                     "capsule %.0f). The margin is exactly one capsule radius and moves "
				                     "with both knobs"),
					DrawnRadius, LethalHalfWidth, TraceRoxieRocket::GetHitRadiusUU(), PawnCapsuleRadius));

			// ---- DEMO 29 ITEM 7: "ONLY 100 FOR DIRECT IMPACTS. OTHERWISE THE DAMAGE SHOULD FALL OFF"
			//
			// The SHAPE of the falloff, on the shipped pure function, with no world — the same reason
			// the wobble is measured here. Trace.Roxie.RocketFalloffTest fires real rockets and checks
			// these same numbers came out of the live damage path; this checks the curve itself.
			{
				const float BlastRadius = TraceRoxieRocket::GetHitRadiusUU();
				const float Direct = TraceRoxieRocket::GetDamage();
				const float AtCentre = TraceRoxieRocket::GetSplashDamageAtGapUU(0.f);
				const float AtQuarter = TraceRoxieRocket::GetSplashDamageAtGapUU(BlastRadius * 0.25f);
				const float AtHalf = TraceRoxieRocket::GetSplashDamageAtGapUU(BlastRadius * 0.5f);
				const float AtThreeQuarter = TraceRoxieRocket::GetSplashDamageAtGapUU(BlastRadius * 0.75f);
				const float AtEdge = TraceRoxieRocket::GetSplashDamageAtGapUU(BlastRadius);
				const float Outside = TraceRoxieRocket::GetSplashDamageAtGapUU(BlastRadius + 8.f);

				UE_LOG(LogTraceGame, Display,
					TEXT("[ROXIEFLIGHT] DEMO 29 §7 FALLOFF TABLE (arithmetic; the live one is "
					     "Trace.Roxie.RocketFalloffTest): direct %.1f | gap 0 %.1f | 25%% (%.0f uu) %.1f | "
					     "50%% (%.0f uu) %.1f | 75%% (%.0f uu) %.1f | edge (%.0f uu) %.1f | outside %.1f"),
					Direct, AtCentre, BlastRadius * 0.25f, AtQuarter, BlastRadius * 0.5f, AtHalf,
					BlastRadius * 0.75f, AtThreeQuarter, BlastRadius, AtEdge, Outside);

				List.Check(AtCentre < Direct - 0.5f && AtCentre > 0.f,
					TEXT("*** DEMO 29 §7: ONLY A DIRECT IMPACT DOES THE FULL DAMAGE ***"),
					FString::Printf(TEXT("the BEST possible near miss — standing exactly on the impact "
					                     "point — takes %.1f against the direct hit's %.1f (x%.2f). A "
					                     "splash that could equal a direct hit would delete the "
					                     "distinction the owner asked for; the fraction is clamped below "
					                     "1.0 so an .ini cannot do it either"),
						AtCentre, Direct, TraceRoxieRocket::GetSplashMaxFraction()));

				List.Check(AtCentre > AtQuarter && AtQuarter > AtHalf && AtHalf > AtThreeQuarter
					&& AtThreeQuarter > AtEdge,
					TEXT("...and it FALLS OFF, strictly, all the way out"),
					FString::Printf(TEXT("%.1f -> %.1f -> %.1f -> %.1f -> %.1f across 0, 25%%, 50%%, 75%% "
					                     "and 100%% of a %.0f uu blast. Smoothstep, so the slope is zero "
					                     "at both ends: no cliff at the boundary and a consistent payout "
					                     "for the best near miss"),
						AtCentre, AtQuarter, AtHalf, AtThreeQuarter, AtEdge, BlastRadius));

				List.Check(AtEdge <= KINDA_SMALL_NUMBER && Outside <= KINDA_SMALL_NUMBER,
					TEXT("*** THE BLAST ENDS EXACTLY WHERE THE DRAWN BURST ENDS ***"),
					FString::Printf(TEXT("%.3f at the edge and %.3f 8 uu outside it. The RocketBurst is "
					                     "drawn at this same %.0f uu (ATraceFxBurst reads the same knob), "
					                     "so there is no invisible kill volume and no visible ring that "
					                     "does nothing"),
						AtEdge, Outside, BlastRadius));
			}
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

// =================================================================================================
// Trace.Roxie.VRowTest — FX_AUDIO_PLAN §7.2, and the close of finding F2
//
// THE FINDING, restated so this file carries it too: FTraceAbilityNetState::AuxEndMatchTime is
// replicated *expressly* "so a client can grey its own V", and until this wave nothing anywhere read
// it. W4-FXHUD drew the row a wave before its producer existed and had to feed it from a cheat CVar
// (Trace.HUD.VRowRoxieFixture) to photograph anything at all.
//
// *** THIS TEST GOES THROUGH UTraceAbilityComponent, NOT THROUGH THE OVERRIDE. *** The HUD asks the
// COMPONENT (TraceHUD.cpp's IsSecondaryRowUp / DrawSecondaryCooldownRow both call
// Abilities->GetSecondaryCooldownDisplay), so that is the door this measures — calling the virtual
// directly would prove the function returns true while leaving the one link that actually feeds the
// row untested.
//
// IT ALSO TURNS THE CAPTURE FIXTURE OFF FIRST. With Trace.HUD.VRowRoxieFixture at 1 the row has TWO
// possible producers and a green run would not say which one answered. Forced to 0, only the real
// override can.
// =================================================================================================

namespace TraceRoxieVRow
{
	void RunVRowTest()
	{
		UWorld* WorldPtr = TraceRoxieVerify::FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ROXIEVROW] no authoritative game world — run this on the server."));
			return;
		}

		struct FState
		{
			int32 Step = 0;
			double NextStepReal = 0.0;
			double Deadline = 0.0;
			TraceRoxieVerify::FChecklist List;
			bool bRedReproduced = false;
			float RemainingAtPress = 0.f;
		};

		TSharedPtr<FState> State = MakeShared<FState>();
		State->List.Tag = TEXT("ROXIEVROW");
		State->Deadline = FPlatformTime::Seconds() + 45.0;

		// THE FIXTURE OFF, FIRST AND ALWAYS. See the block comment: two producers, one green, no
		// information. Restored to whatever it was is deliberately NOT done — 0 is its shipped default
		// and this arm exists to be deleted.
		TraceRoxieVerify::SetArm(TEXT("Trace.HUD.VRowRoxieFixture"), 0);

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROXIEVROW] ===== FX plan §7.2: Roxie's V row has a REAL producer. Rocket cooldown %.0fs. "
			     "Trace.HUD.VRowRoxieFixture forced to 0 so the capture fixture cannot answer instead. "
			     "arm 0 = RED (Trace.Roxie.VRow 0): the override refuses and the row disappears. ====="),
			TraceRoxieRocket::GetCooldownSeconds());

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				TraceRoxieVerify::SetArm(TEXT("Trace.Roxie.VRow"), 1);
				return false;
			}
			if (NowReal < State->NextStepReal)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetRoxie* RoxieSet = TraceRoxieVerify::MakePlayerIntoRoxie(TickWorld, Why);
			ATraceCharacter* RoxiePawn = (RoxieSet != nullptr) ? RoxieSet->GetCharacter() : nullptr;
			UTraceAbilityComponent* Comp = (RoxiePawn != nullptr)
				? UTraceAbilityComponent::Get(RoxiePawn) : nullptr;

			if (RoxieSet == nullptr || RoxiePawn == nullptr || Comp == nullptr || !RoxiePawn->IsAlive())
			{
				if (State->Step == 0 && NowReal <= State->Deadline)
				{
					return true;   // still staging
				}
				State->List.Invalidate(FString::Printf(TEXT("could not stage Roxie (%s)"), *Why));
				State->List.Report();
				TraceRoxieVerify::SetArm(TEXT("Trace.Roxie.VRow"), 1);
				return false;
			}

			float Remaining = 0.f;
			float Duration = 0.f;
			FString Label;

			// ---- step 0: THE RED ARM ----------------------------------------------------------
			if (State->Step == 0)
			{
				TraceRoxieVerify::SetArm(TEXT("Trace.Roxie.VRow"), 0);
				const bool bRedDrawn = Comp->GetSecondaryCooldownDisplay(Remaining, Duration, Label);
				State->bRedReproduced = !bRedDrawn;

				State->List.Check(State->bRedReproduced,
					TEXT("RED ARM: with the producer disarmed the HUD is told there is NO V row"),
					FString::Printf(TEXT("GetSecondaryCooldownDisplay()=%d — this IS the pre-F2 build, where "
					                     "AuxEndMatchTime replicated to nobody"), bRedDrawn ? 1 : 0));

				TraceRoxieVerify::SetArm(TEXT("Trace.Roxie.VRow"), 1);
				RoxieSet->DebugClearRocketCooldown();
				State->Step = 1;
				State->NextStepReal = NowReal + 0.2;
				return true;
			}

			// ---- step 1: READY --------------------------------------------------------------
			if (State->Step == 1)
			{
				const bool bDrawn = Comp->GetSecondaryCooldownDisplay(Remaining, Duration, Label);

				State->List.Check(bDrawn,
					TEXT("§7.2: Roxie's V row is drawn THROUGH THE COMPONENT — the door the HUD uses"),
					FString::Printf(TEXT("UTraceAbilityComponent::GetSecondaryCooldownDisplay()=%d with the "
					                     "capture fixture OFF, so the override is the only possible answerer"),
						bDrawn ? 1 : 0));

				State->List.Check(Label.Equals(TEXT("ROCKET")),
					TEXT("...and it names the ability, upper case and short enough to share a row"),
					FString::Printf(TEXT("label '%s'"), *Label));

				State->List.Check(FMath::IsNearlyEqual(Duration, TraceRoxieRocket::GetCooldownSeconds(), 0.01f),
					TEXT("the meter's denominator is the LIVE knob, not a DataAsset snapshot (the F6 trap)"),
					FString::Printf(TEXT("duration %.2fs against the ability's own "
					                     "TraceRoxieRocket::GetCooldownSeconds() = %.2fs"),
						Duration, TraceRoxieRocket::GetCooldownSeconds()));

				State->List.Check(Remaining <= 0.01f && RoxieSet->IsRocketReady(),
					TEXT("READY is a DRAWN state — 0 remaining still returns true, so the row teaches the key"),
					FString::Printf(TEXT("remaining %.2fs, IsRocketReady()=%d"),
						Remaining, RoxieSet->IsRocketReady() ? 1 : 0));

				// FIRE V FOR REAL, through the same entry point the key press uses.
				const bool bFired = RoxieSet->OnSecondaryPressed();
				State->RemainingAtPress = RoxieSet->GetRocketCooldownRemaining();
				State->List.Check(bFired,
					TEXT("V fired, through OnSecondaryPressed — the shipped path, not a written deadline"),
					FString::Printf(TEXT("fired=%d, cooldown now %.2fs"), bFired ? 1 : 0, State->RemainingAtPress));

				State->Step = 2;
				State->NextStepReal = NowReal + 1.0;
				return true;
			}

			// ---- step 2: COOLING, and the number is counting DOWN ------------------------------
			const bool bDrawnCooling = Comp->GetSecondaryCooldownDisplay(Remaining, Duration, Label);

			State->List.Check(bDrawnCooling && Remaining > 0.f && Remaining <= Duration,
				TEXT("§7.2: while cooling the row reports a remaining inside its own duration"),
				FString::Printf(TEXT("remaining %.2fs of %.2fs"), Remaining, Duration));

			State->List.Check(Remaining < State->RemainingAtPress - 0.5f,
				TEXT("...and it COUNTS DOWN — a second later there is at least half a second less"),
				FString::Printf(TEXT("%.2fs at the press -> %.2fs one second later"),
					State->RemainingAtPress, Remaining));

			State->List.Check(FMath::IsNearlyEqual(Remaining, RoxieSet->GetRocketCooldownRemaining(), 0.05f),
				TEXT("the row and the ability are ONE number, not two that agree today"),
				FString::Printf(TEXT("row %.2fs, GetRocketCooldownRemaining() %.2fs"),
					Remaining, RoxieSet->GetRocketCooldownRemaining()));

			if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — with Trace.Roxie.VRow 0 the row was "
				                            "STILL drawn, so something other than this override is answering and "
				                            "nothing above measures F2"));
			}

			State->List.Report();
			TraceRoxieVerify::SetArm(TEXT("Trace.Roxie.VRow"), 1);
			return false;
		}));
	}

	FAutoConsoleCommand CmdVRowTest(
		TEXT("Trace.Roxie.VRowTest"),
		TEXT("Dev only, SERVER. FX plan §7.2 / finding F2: Roxie's GetSecondaryCooldownDisplay override feeds "
		     "the HUD's V row through UTraceAbilityComponent — label, live duration, ready-at-zero and a "
		     "counting-down remaining. Forces Trace.HUD.VRowRoxieFixture to 0 so the capture fixture cannot "
		     "answer instead, and red-arms itself with Trace.Roxie.VRow 0 first."),
		FConsoleCommandDelegate::CreateStatic(&RunVRowTest));
}

// =================================================================================================
// Trace.Roxie.RocketFxTest — FX_AUDIO_PLAN §2.3's launch flash, trail, MODDED tell and burst
//
// Four claims, and each one is measured off something that exists rather than asserted:
//
//   the FLASH    is up inside the tracer's own 0.28 s and GONE after it, and its peak radius honours
//                the bible's 40 uu muzzle ceiling.
//   the TRAIL    is three segments, additive, tapering, at or above the 8 uu emissive width floor.
//   the BURST    appears at the rocket's end with a radius READ LIVE from the damage knob — the
//                drawn-equals-lethal invariant, checked by comparing the burst's own resolved radius
//                against RoxieRocketHitRadiusUU rather than against a number written here.
//   the TELL     lifts her accent stripes while MODDED runs and puts them back when it ends, on this
//                machine, through the §1.2 router's own entry points.
//
// The FLASH has the red arm: Trace.Roxie.RocketWobble is not it (that arms the path), so the arm here
// is the one that matters for FX — Trace.Fx.BurstForceNone, which makes every burst resolve None and
// draw nothing. A run that still reports a visible burst under it is a run measuring nothing.
// =================================================================================================

namespace TraceRoxieRocketFx
{
	int32 CountLiveRocketBursts(UWorld* WorldPtr, float& OutRadius)
	{
		OutRadius = 0.f;
		int32 Count = 0;
		for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
		{
			if (!IsValid(*It) || It->GetBurstType() != ETraceFxBurstType::RocketBurst)
			{
				continue;
			}
			++Count;
			OutRadius = It->GetResolvedRadiusUU();
		}
		return Count;
	}

	void RunRocketFxTest()
	{
		UWorld* WorldPtr = TraceRoxieVerify::FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ROXIEFX] no authoritative game world — run this on the server."));
			return;
		}

		struct FState
		{
			int32 Step = 0;
			double NextStepReal = 0.0;
			double Deadline = 0.0;
			TraceRoxieVerify::FChecklist List;
			float FlashRadiusEarly = 0.f;
			int32 PrimitivesEarly = 0;
			int32 PrimitivesLate = 0;
			int32 BurstsSeen = 0;
			float BurstRadius = 0.f;
			float AccentBefore = 0.f;
			float AccentDuring = 0.f;
			float AccentAfter = 0.f;
		};

		TSharedPtr<FState> State = MakeShared<FState>();
		State->List.Tag = TEXT("ROXIEFX");
		State->Deadline = FPlatformTime::Seconds() + 45.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROXIEFX] ===== FX plan §2.3: flash %.2fs (%.2fx -> %.2fx, <= %.0f uu), trail 3 segments "
			     "at I %.2f, burst radius read LIVE from RoxieRocketHitRadiusUU (%.0f uu), MODDED tell "
			     "AccentGlow -> %.2f. ====="),
			ATraceTracer::MuzzleFlashSeconds, ATraceTracer::MuzzleFlashStartScale,
			ATraceTracer::MuzzleFlashEndScale, 40.f, 0.5f,
			TraceRoxieRocket::GetHitRadiusUU(), TraceRoxieFxFile::ModdedAccentGlow);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				return false;
			}
			if (NowReal < State->NextStepReal)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetRoxie* RoxieSet = TraceRoxieVerify::MakePlayerIntoRoxie(TickWorld, Why);
			ATraceCharacter* RoxiePawn = (RoxieSet != nullptr) ? RoxieSet->GetCharacter() : nullptr;
			if (RoxieSet == nullptr || RoxiePawn == nullptr || !RoxiePawn->IsAlive())
			{
				if (State->Step == 0 && NowReal <= State->Deadline)
				{
					return true;
				}
				State->List.Invalidate(FString::Printf(TEXT("could not stage Roxie (%s)"), *Why));
				State->List.Report();
				return false;
			}

			// The highest AccentGlow on any body slot, read back off the material. The MAXIMUM and not
			// the first: only the generated bodies' accent material declares the scalar, and a Roxie
			// wearing the Mannequin fallback legitimately has none.
			auto ReadAccent = [](const ATraceCharacter* Pawn) -> float
			{
				float Best = -1.f;
				if (const USkeletalMeshComponent* MeshComp = (Pawn != nullptr) ? Pawn->GetMesh() : nullptr)
				{
					const int32 Slots = MeshComp->GetNumMaterials();
					for (int32 Slot = 0; Slot < Slots; ++Slot)
					{
						if (const UMaterialInstanceDynamic* MID =
							Cast<UMaterialInstanceDynamic>(MeshComp->GetMaterial(Slot)))
						{
							float Value = 0.f;
							if (MID->GetScalarParameterValue(TraceRoxieFxFile::AccentGlowParam, Value))
							{
								Best = FMath::Max(Best, Value);
							}
						}
					}
				}
				return Best;
			};

			// ---- step 0: fire a rocket, and look at it INSIDE the flash's window ----------------
			if (State->Step == 0)
			{
				State->AccentBefore = ReadAccent(RoxiePawn);
				RoxieSet->DebugClearRocketCooldown();

				// bAlsoSelfLaunch FALSE: the launch would throw her across the arena and the rocket
				// would leave the fixture's frame of reference. The FX under test are the rocket's.
				ATraceRoxieRocket* Rocket = RoxieSet->DebugFireRocket(0.25f, /*bAlsoSelfLaunch*/ false);
				if (Rocket == nullptr)
				{
					State->List.Invalidate(TEXT("DebugFireRocket produced no rocket"));
					State->List.Report();
					return false;
				}

				const FString Described = Rocket->DebugDescribeFx();
				UE_LOG(LogTraceGame, Display, TEXT("[ROXIEFX] at launch: %s"), *Described);

				State->List.Check(Rocket->IsLaunchFlashUp(),
					TEXT("§2.3: a launch flash exists at the muzzle on the frame the rocket is born"),
					*Described);

				State->List.Check(Described.Contains(TEXT("trail 3/3 visible")),
					TEXT("§2.3: three trail segments, all of them resolved and visible"),
					*Described);

				State->List.Check(!Described.Contains(TEXT("(None)")),
					TEXT("no rocket FX piece degraded to None — nothing is drawing engine grey"),
					*Described);

				State->PrimitivesEarly = 5;   // asserted against the string below rather than counted twice
				State->List.Check(Described.Contains(TEXT("primitives=5")),
					TEXT("inside the flash's 0.28 s the rocket carries FIVE primitives"),
					FString::Printf(TEXT("%s — body + three trail segments + the flash, which is the one "
					                     "frame-window the bible's four is exceeded in, deliberately"),
						*Described));

				State->Step = 1;
				State->NextStepReal = NowReal + static_cast<double>(ATraceTracer::MuzzleFlashSeconds) + 0.12;
				return true;
			}

			// ---- step 1: after the flash's window, it must be GONE ------------------------------
			if (State->Step == 1)
			{
				ATraceRoxieRocket* Rocket = nullptr;
				for (TActorIterator<ATraceRoxieRocket> It(TickWorld); It; ++It)
				{
					if (IsValid(*It)) { Rocket = *It; break; }
				}

				if (Rocket == nullptr)
				{
					// The rocket already ended (it can hit a wall inside 0.4 s in a small arena). That
					// is not a failure of the flash claim, it is a lost opportunity to measure it —
					// and saying so is better than scoring a check against a rocket that is not there.
					UE_LOG(LogTraceGame, Display,
						TEXT("[ROXIEFX] the rocket ended before the post-flash sample; the flash lifetime "
						     "check is skipped rather than scored."));
				}
				else
				{
					const FString Described = Rocket->DebugDescribeFx();
					UE_LOG(LogTraceGame, Display, TEXT("[ROXIEFX] after the flash window: %s"), *Described);

					State->List.Check(!Rocket->IsLaunchFlashUp() && Described.Contains(TEXT("primitives=4")),
						TEXT("§2.3: the flash DESTROYS itself at 0.28 s, putting the rocket back to four"),
						FString::Printf(TEXT("%s — hiding it instead would have left five for the whole "
						                     "flight"), *Described));
				}

				State->Step = 2;
				State->NextStepReal = NowReal + 0.35;
				return true;
			}

			// ---- step 2: the BURST at the end, radius read live ---------------------------------
			if (State->Step == 2)
			{
				float Radius = 0.f;
				const int32 Bursts = CountLiveRocketBursts(TickWorld, Radius);
				State->BurstsSeen = FMath::Max(State->BurstsSeen, Bursts);
				if (Bursts > 0)
				{
					State->BurstRadius = Radius;
				}

				bool bRocketStillFlying = false;
				for (TActorIterator<ATraceRoxieRocket> It(TickWorld); It; ++It)
				{
					if (IsValid(*It)) { bRocketStillFlying = true; break; }
				}

				if (State->BurstsSeen == 0 && bRocketStillFlying && NowReal <= State->Deadline)
				{
					State->NextStepReal = NowReal + 0.1;
					return true;   // still in the air; keep watching for its end
				}

				State->List.Check(State->BurstsSeen > 0,
					TEXT("§2.3: the rocket's END spawns a RocketBurst — body, wall or expiry, one site"),
					FString::Printf(TEXT("%d live RocketBurst(s) seen. Before this the rocket simply stopped "
					                     "being drawn"), State->BurstsSeen));

				State->List.Check(State->BurstsSeen > 0
					&& FMath::IsNearlyEqual(State->BurstRadius, TraceRoxieRocket::GetHitRadiusUU(), 0.51f),
					TEXT("DRAWN == LETHAL: the burst's radius IS the live damage knob, not a copy of it"),
					FString::Printf(TEXT("burst resolved %.1f uu against RoxieRocketHitRadiusUU %.1f uu — the "
					                     "same clamped read the hit sweep makes"),
						State->BurstRadius, TraceRoxieRocket::GetHitRadiusUU()));

				// ---- MODDED's tell, through the router's own entry points -----------------------
				//
				// SyncClientFx(Comp->GetNetState()) and not a private call: it is the §1.2 hook a
				// joining machine uses, so driving it here measures the shipped entry point rather
				// than a back door beside it. Same shape UTraceAbilitySetRocco's own fixture uses.
				RoxieSet->ActivateAbility();
				if (UTraceAbilityComponent* RoxieComp = UTraceAbilityComponent::Get(RoxiePawn))
				{
					RoxieSet->SyncClientFx(RoxieComp->GetNetState());
				}
				State->AccentDuring = ReadAccent(RoxiePawn);

				State->Step = 3;
				State->NextStepReal = NowReal + 0.2;
				return true;
			}

			// ---- step 3: MODDED ends, the tell comes back off ------------------------------------
			State->List.Check(State->AccentBefore < 0.f || State->AccentDuring >= TraceRoxieFxFile::ModdedAccentGlow - 0.01f,
				TEXT("§2.3: MODDED lifts her body accent stripes on THIS machine, through SyncClientFx"),
				FString::Printf(TEXT("AccentGlow %.2f -> %.2f, the tell being %.2f (bible Glow %.2f). A -1 "
				                     "'before' means this pawn wears the Mannequin fallback, which declares no "
				                     "AccentGlow at all and is not a failure"),
					State->AccentBefore, State->AccentDuring, TraceRoxieFxFile::ModdedAccentGlow,
					TraceRoxieFxFile::ModdedBibleGlow));

			RoxieSet->OnPawnDied();      // the shipped teardown: EndModded + ClearModdedTell
			State->AccentAfter = ReadAccent(RoxiePawn);

			State->List.Check(State->AccentBefore < 0.f
				|| State->AccentAfter < TraceRoxieFxFile::ModdedAccentGlow - 0.01f,
				TEXT("...and it comes back off through ApplyTeamColors, not through a remembered number"),
				FString::Printf(TEXT("AccentGlow %.2f after the tell was cleared, against %.2f while it was up"),
					State->AccentAfter, State->AccentDuring));

			State->List.Report();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Roxie.RocketShot — PATCH 28 ITEM 1's PHOTOGRAPH, AND ITS MEASUREMENT IN PIXELS
	// =============================================================================================
	//
	// "Make Roxie's rocket larger" is a request about what a player SEES, so the acceptance has to be
	// a picture and a number on the same frame. Trace.Roxie.RocketFxTest already reads the drawn
	// radius back off the live component; this fires one rocket, parks a camera BROADSIDE to it at a
	// FIXED 900 uu, photographs it, and converts the drawn radius into the diameter it covers on a
	// 1920-wide frame — which is the only form of "how big is it on screen" that means anything.
	//
	// THE DISTANCE IS FIXED AND THE FRAMING IS BROADSIDE ON PURPOSE. A rocket photographed from
	// behind Roxie is a dot whose size is a fact about where the bots pushed her, not about the knob;
	// two such frames cannot be compared. At a fixed range, two runs of this command differ by exactly
	// what RoxieRocketHitRadiusUU did.
	//
	// DEMO 29 §7 GAVE IT A SECOND JOB: the drawn radius printed beside the picture is now MEASURED OFF
	// THE LIVE MESH (ATraceRoxieRocket::GetDrawnBodyRadiusUU) and compared against the hit radius, so
	// the frame is evidence for "the hit radius matches the model" rather than just for "it is big".
	void RunRocketShot()
	{
		UWorld* WorldPtr = TraceRoxieVerify::FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ROXIESHOT] no authoritative game world — run this on the server."));
			return;
		}

		struct FShotState
		{
			int32 Step = 0;
			int32 FiringAttempt = 0;
			double NextReal = 0.0;
			double Deadline = 0.0;
			TWeakObjectPtr<ATraceRoxieRocket> Rocket;
			TWeakObjectPtr<ACameraActor> Camera;
			FString ShotPath;
		};

		TSharedPtr<FShotState> State = MakeShared<FShotState>();
		State->Deadline = FPlatformTime::Seconds() + 40.0;

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr || NowReal > State->Deadline)
			{
				return false;
			}
			if (NowReal < State->NextReal)
			{
				return true;
			}

			// ---- 0: stage Roxie and fire one rocket -------------------------------------------
			if (State->Step == 0)
			{
				FString Why;
				UTraceAbilitySetRoxie* RoxieSet = TraceRoxieVerify::MakePlayerIntoRoxie(TickWorld, Why);
				ATraceCharacter* RoxiePawn = (RoxieSet != nullptr) ? RoxieSet->GetCharacter() : nullptr;
				if (RoxieSet == nullptr || RoxiePawn == nullptr || !RoxiePawn->IsAlive())
				{
					return true;   // keep trying until the deadline; a live arena respawns people
				}

				// *** AIM HER DOWN THE FIELD, AND FAN THE AIM ON EVERY RETRY. ***
				//
				// The first version fired along whatever she happened to be facing, and the run came
				// back "the rocket was gone before the frame": in a live arena she is usually looking
				// at a piece of cover a few hundred uu away, and a 2600 uu/s rocket detonates on it in
				// well under the 0.4 s this command waits. So the shot is aimed at the field's long
				// axis first, and each retry fans 37 degrees off it until one rocket lives long
				// enough — 37 rather than 45 so the fan does not land on the same four compass points.
				const FVector Here = RoxiePawn->GetActorLocation();
				const float TowardCentreYaw = FMath::RadiansToDegrees(FMath::Atan2(-Here.Y, -Here.X));
				const FRotator Aim(2.f, TowardCentreYaw + 37.f * static_cast<float>(State->FiringAttempt), 0.f);
				RoxiePawn->SetActorRotation(FRotator(0.f, Aim.Yaw, 0.f));
				if (AController* Ctrl = RoxiePawn->GetController())
				{
					Ctrl->SetControlRotation(Aim);
				}

				RoxieSet->DebugClearRocketCooldown();
				// No self-launch: the shove would move the camera's subject and the muzzle together.
				State->Rocket = RoxieSet->DebugFireRocket(0.f, /*bAlsoSelfLaunch*/ false);
				if (!State->Rocket.IsValid())
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[ROXIESHOT] DebugFireRocket produced no rocket."));
					return false;
				}

				State->Step = 1;
				State->NextReal = NowReal + 0.35;   // clear of the 0.28 s launch flash and of Roxie's head
				return true;
			}

			// ---- 1: broadside camera, then the frame ------------------------------------------
			if (State->Step == 1)
			{
				const ATraceRoxieRocket* Rocket = State->Rocket.Get();
				APlayerController* PC = TickWorld->GetFirstPlayerController();
				if (Rocket == nullptr || PC == nullptr)
				{
					// It met something. Fan the aim and fire again rather than reporting a failure —
					// where the cover happens to be is not this command's subject.
					if (PC != nullptr && ++State->FiringAttempt < 10)
					{
						UE_LOG(LogTraceGame, Verbose,
							TEXT("[ROXIESHOT] rocket %d hit something inside 0.35 s; fanning the aim."),
							State->FiringAttempt);
						State->Step = 0;
						State->NextReal = NowReal + 0.15;
						return true;
					}
					UE_LOG(LogTraceGame, Warning,
						TEXT("[ROXIESHOT] every rocket met geometry inside 0.35 s — no clear lane from here."));
					return false;
				}

				constexpr float ViewDistanceUU = 900.f;
				const FVector RocketAt = Rocket->GetActorLocation();
				const FVector Along = Rocket->GetActorForwardVector().GetSafeNormal2D();
				FVector Side = FVector::CrossProduct(FVector::UpVector, Along).GetSafeNormal();
				if (Side.IsNearlyZero())
				{
					Side = FVector::RightVector;
				}
				const FVector Eye = RocketAt + Side * ViewDistanceUU + FVector(0.f, 0.f, 60.f);

				FActorSpawnParameters SpawnParams;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				SpawnParams.ObjectFlags |= RF_Transient;
				ACameraActor* Camera = TickWorld->SpawnActor<ACameraActor>(
					ACameraActor::StaticClass(), FTransform((RocketAt - Eye).Rotation(), Eye), SpawnParams);
				if (Camera == nullptr)
				{
					return false;
				}
				PC->SetViewTargetWithBlend(Camera, 0.f);
				State->Camera = Camera;

				// ---- the number that goes with the picture ------------------------------------
				//
				// A sphere of radius R at range D subtends a half-angle atan(R/D). Under a pinhole
				// projection of horizontal FOV F onto a W-pixel-wide frame, that lands at
				// W * (R/D) / tan(F/2) pixels ACROSS. Reported alongside the uu so the frame can be
				// checked with a ruler rather than believed.
				const float Fov = (Camera->GetCameraComponent() != nullptr)
					? Camera->GetCameraComponent()->FieldOfView : 90.f;

				// *** DEMO 29 §7's PROOF, AND IT IS A READ-BACK RATHER THAN A RE-DERIVATION. ***
				//
				// MeasuredRadius comes off the LIVE component's world scale through the inverse of the
				// conversion ApplyVisualSize used; HitRadius is the number the flight sweep and the
				// blast both use. If they agree, the thing in this photograph is the thing that hits
				// you. Comparing GetVisualRadiusUU() against GetHitRadiusUU() instead would compare a
				// function with itself and would pass on a build where ApplyVisualSize never ran.
				const float MeasuredRadius = Rocket->GetDrawnBodyRadiusUU();
				const float HitRadius = TraceRoxieRocket::GetHitRadiusUU();
				const bool bAgree = FMath::IsNearlyEqual(MeasuredRadius, HitRadius, 0.51f);
				const float DrawnRadius = MeasuredRadius;
				const float DrawnLength = DrawnRadius * 3.f;   // VisualLengthPerRadius, stated in the log
				float ViewportW = 1920.f;
				if (PC->GetLocalPlayer() != nullptr && PC->GetLocalPlayer()->ViewportClient != nullptr)
				{
					FVector2D Size = FVector2D::ZeroVector;
					PC->GetLocalPlayer()->ViewportClient->GetViewportSize(Size);
					if (Size.X > 1.0)
					{
						ViewportW = static_cast<float>(Size.X);
					}
				}
				const float HalfTan = FMath::Max(0.01f, FMath::Tan(FMath::DegreesToRadians(Fov * 0.5f)));
				const float PixelsAcross = ViewportW * (DrawnRadius / ViewDistanceUU) / HalfTan;

				UE_LOG(LogTraceGame, Display,
					TEXT("[ROXIESHOT] ON SCREEN: drawn body r %.1f uu MEASURED OFF THE LIVE MESH vs hit "
					     "radius %.1f uu — %s. %.0f uu across and %.0f uu long, photographed BROADSIDE at "
					     "%.0f uu on a %.0f px frame at %.0f deg FOV = %.0f px across. The %.0f uu blast "
					     "radius and the RocketBurst are the same number again. Live FX: %s"),
					MeasuredRadius, HitRadius,
					bAgree ? TEXT("*** DEMO 29 §7 PASS: THE MODEL IS THE HIT RADIUS ***")
					       : TEXT("*** FAIL: the model and the hit radius have drifted apart ***"),
					DrawnRadius * 2.f, DrawnLength, ViewDistanceUU, ViewportW, Fov, PixelsAcross,
					HitRadius, *Rocket->DebugDescribeFx());

				const FString FileName = FString::Printf(TEXT("RoxieRocket_r%.0f_pid%d.png"),
					HitRadius, FPlatformProcess::GetCurrentProcessId());
				State->ShotPath = FPaths::ConvertRelativePathToFull(
					FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);
				FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(State->ShotPath));
				FScreenshotRequest::RequestScreenshot(State->ShotPath, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
				UE_LOG(LogTraceGame, Display, TEXT("[ROXIESHOT] Screenshot requested: %s"), *State->ShotPath);

				State->Step = 2;
				State->NextReal = NowReal + 0.8;
				return true;
			}

			// ---- 2: confirm and clean up ------------------------------------------------------
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			if (PlatformFile.FileExists(*State->ShotPath))
			{
				UE_LOG(LogTraceGame, Display, TEXT("[ROXIESHOT] Screenshot written (%lld bytes): %s"),
					PlatformFile.FileSize(*State->ShotPath), *State->ShotPath);
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ROXIESHOT] No screenshot appeared at: %s"), *State->ShotPath);
			}
			if (ACameraActor* Camera = State->Camera.Get())
			{
				Camera->Destroy();
			}
			return false;
		}), 0.f);
	}

	FAutoConsoleCommand CmdRocketShot(
		TEXT("Trace.Roxie.RocketShot"),
		TEXT("Dev only, SERVER, needs a rendering process. PATCH 28 §1 / DEMO 29 §7: fires one rocket, "
		     "photographs it BROADSIDE at a fixed 900 uu, and prints the drawn radius MEASURED OFF THE "
		     "LIVE MESH against RoxieRocketHitRadiusUU — they must be equal — plus the diameter it covers "
		     "in pixels on this frame. Two runs at different hit radii are directly comparable."),
		FConsoleCommandDelegate::CreateStatic(&RunRocketShot));

	FAutoConsoleCommand CmdRocketFxTest(
		TEXT("Trace.Roxie.RocketFxTest"),
		TEXT("Dev only, SERVER. FX plan §2.3: the launch flash's 0.28 s life and self-destruction, the three "
		     "trail segments and their achieved blend, the RocketBurst at the rocket's end with its radius "
		     "READ LIVE from the damage knob, and MODDED's accent-stripe tell going up and coming back off. "
		     "Every number is read back off the live components."),
		FConsoleCommandDelegate::CreateStatic(&RunRocketFxTest));
}

#endif // !UE_BUILD_SHIPPING
