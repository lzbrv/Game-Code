// Trace — ROCCO. See the header for the spec v14 §6 reading.

#include "Abilities/Characters/TraceAbilitySetRocco.h"

#include "Components/SkeletalMeshComponent.h"   // §2.9's stack tell writes the body MIDs directly
#include "Containers/Ticker.h"                  // FTSTicker — Trace.Rocco.FxTest's two phases
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                        // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "Abilities/Characters/TraceRippleActor.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"                  // spec v26 §9 - the second jump IS a jump, and was silent
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceFxBurst.h"             // §2.9 — the second jump's GenericRing
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// FX_AUDIO_PLAN §2.9's numbers. Everything visual about Rocco that is NOT in TraceRippleActor.
// =================================================================================================

namespace TraceRoccoFxFile
{
	/**
	 * ART_BIBLE §4.5: a generated body's emissive accents sit at Glow 1.7. That is the base the
	 * stack tell scales, and it is written here rather than read from the material because it is a
	 * DESIGN tier — the number the bible fixed — and not whatever a particular MI happens to hold.
	 * The material's own value is latched separately at run time (see ApplyStackAccentTell).
	 */
	constexpr float AccentBaseGlow = 1.7f;

	/** §2.9: "+ 0.35/stack". */
	constexpr float AccentGlowPerStack = 0.35f;

	/** §2.9: "cap 3.0". Four stacks reach it; the tell is a state, not a gauge. */
	constexpr float AccentGlowCap = 3.0f;

	/**
	 * §2.9: the second jump's ring is "r 30 -> 70". ATraceFxBurst's GenericRing default is 80 uu,
	 * chosen there to sit between this and Elle's snap ring — passing the number makes Rocco's ring
	 * exactly the one §2.9 asked for, which is what FTraceFxBurstSpec::RadiusUU exists for.
	 */
	constexpr float SecondJumpRingRadiusUU = 70.f;
}

// =================================================================================================
// THE RED ARMS.
//
// Each of these removes one of Rocco's three abilities and NOTHING else. They exist so
// Trace.Rocco.Verify can be made to FAIL on a build that is otherwise identical — a harness whose
// red arm cannot be made to reproduce is a harness whose green means nothing, which is exactly how
// the wall-clip bug survived two passes in this project.
//
// Cheat-flagged, default ON (the shipped behaviour), and never read anywhere but the one function
// each of them arms.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarRoccoRippleEnabled(
	TEXT("Trace.Rocco.RippleEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = pressing E spawns the Ripple. 0 = it charges the cooldown and spawns "
	     "nothing, so every ride assertion in Trace.Rocco.Verify must go red."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarRoccoSecondJumpEnabled(
	TEXT("Trace.Rocco.SecondJumpEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = the midair second jump redirects. 0 = the hook declines the jump, so the "
	     "redirect assertion must go red."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarRoccoStackCapEnabled(
	TEXT("Trace.Rocco.StackCapEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = the headshot stack is capped at RoccoHeadshotSpeedStackMax (spec §6's "
	     "[ASSUMPTION]). 0 = uncapped, so the cap assertion must go red."),
	ECVF_Cheat);

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetRocco::OnUnequipped()
{
	// THE ACCENT GOES BACK BEFORE THE SET DOES. A character swap leaves the pawn alive and wearing a
	// different body a moment later; an accent lift left behind would be Rocco's tell glowing on
	// somebody else's stripes with nothing left alive to take it down again.
	ClearStackAccentTell();
	DestroyActiveRipple();
}

void UTraceAbilitySetRocco::OnPawnSpawned()
{
	// A fresh pawn is standing on the floor with its extra jump in hand. The cooldown is NOT touched
	// — spec §5 is explicit that a player can spawn with an ability timer still counting down, and
	// the framework owns that timer anyway.
	bSecondJumpUsed = false;
}

void UTraceAbilitySetRocco::OnPawnDied()
{
	bSecondJumpUsed = false;

	// §1.2 obligation 2: attached presentation must never survive onto a corpse. ApplyTeamColors()
	// is about to be called by the death presentation anyway and would take the lift down with it —
	// but "about to" is not a guarantee, and this hook is where the router contract says to be
	// explicit. Idempotent, so doing it twice costs one no-op.
	ClearStackAccentTell();

	// [ASSUMPTION] THE RIPPLE OUTLIVES ROCCO. §6 says it "lasts 4 s"; it says nothing about its
	// author. A path that vanished the instant its owner was shot would make the ability strictly
	// worse for the team it was laid for, and the choke point already answers entry without needing
	// Rocco to be alive (the instigator is his PlayerState, which survives).
}

void UTraceAbilitySetRocco::OnHalfTime()
{
	// The framework has already cleared the cooldown and Reset() the net state. All that is left is
	// the world actor, which is exactly what this hook is documented to be for — plus the accent,
	// which is on the PAWN rather than in the state pad and therefore is not covered by that Reset().
	DestroyActiveRipple();
	ClearStackAccentTell();
	bSecondJumpUsed = false;
}

void UTraceAbilitySetRocco::DestroyActiveRipple()
{
	if (ATraceRippleActor* Ripple = ActiveRipple.Get())
	{
		Ripple->Destroy();
	}
	ActiveRipple = nullptr;
}

void UTraceAbilitySetRocco::TickAbilities(float DeltaSeconds)
{
	// --- every machine: the extra jump comes back when the feet touch the floor --------------------
	// Landing is the one event both ends see without a message, which is why the flag is local and
	// cleared here rather than replicated.
	if (const UTraceCharacterMovementComponent* Move = GetMovement())
	{
		if (Move->IsMovingOnGround())
		{
			bSecondJumpUsed = false;
		}
	}

	// --- EVERY MACHINE, BEFORE THE AUTHORITY GATE: §2.9's stack tell ------------------------------
	//
	// Above the early return on purpose. The tell is what a player's TEAM-MATES and his VICTIMS see;
	// a version of it that only ran on the server would be the F10 blocker this whole FX pass exists
	// to close. It is driven by GetLiveStackCount(), which reads the REPLICATED stack and honours the
	// timer, so every machine computes the same lift from the same two numbers.
	//
	// The stack window is three seconds long and only opens on a headshot KILL, so this does nothing
	// at all for the overwhelming majority of ticks — see ApplyStackAccentTell's first two lines.
	ApplyStackAccentTell();

	if (!HasAuthority())
	{
		return;
	}

	const float Now = MatchTimeNow();
	const FTraceAbilityNetState& Current = State();

	// --- the ONE shared stack window closing ------------------------------------------------------
	// Edge-triggered: the whole stack drops together, which is the difference between §6's "each kill
	// extends the timer on the entire boost" and a per-stack decay nobody asked for.
	if ((Current.Flags & TraceAbilityFlags::EffectActive) != 0 && Now >= Current.EffectEndMatchTime)
	{
		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::EffectActive);
		Writable.Stacks = 0;
		Writable.EffectEndMatchTime = 0.f;
		MarkStateDirty();
	}

	// --- the ripple's bookkeeping bit -------------------------------------------------------------
	if ((Current.Flags & TraceAbilityFlags::AuxActive) != 0
		&& (!ActiveRipple.IsValid() || Now >= Current.AuxEndMatchTime))
	{
		ActiveRipple = nullptr;
		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::AuxActive);
		Writable.AuxEndMatchTime = 0.f;
		MarkStateDirty();
	}
}

// =================================================================================================
// FX_AUDIO_PLAN §1.2's router, and §2.9's stack tell
// =================================================================================================

void UTraceAbilitySetRocco::OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New)
{
	// EDGE-TRIGGERED, ON EVERY MACHINE. The tell reacts to the stack count and to nothing else: the
	// AuxActive bit (the live ripple) is presented by ATraceRippleActor, which is its own replicated
	// fact and needs no help from here.
	if (Old.Stacks == New.Stacks)
	{
		return;
	}

	if (New.Stacks == 0)
	{
		ClearStackAccentTell();
		return;
	}

	// THE RISING EDGE IS AN INSTANT REFRESH AND NOT A SEPARATE CODE PATH. Waiting for the next
	// TickAbilities would put up to 50 ms between the kill and the tell, and having two functions
	// that both know how to compute the lift is how the two of them eventually disagree.
	ApplyStackAccentTell();
}

void UTraceAbilitySetRocco::SyncClientFx(const FTraceAbilityNetState& Current)
{
	// FIRST SIGHT: a client that joined, swapped character or respawned into a state that is ALREADY
	// carrying stacks. Without this the tell would be invisible on that machine until the next kill
	// moved the number — the join-in-progress case §1.2 exists for. Idempotent by construction: it is
	// the same poll TickAbilities runs.
	if (Current.Stacks > 0)
	{
		ApplyStackAccentTell();
	}
	else
	{
		ClearStackAccentTell();
	}
}

float UTraceAbilitySetRocco::GetAccentGlowMultiplier() const
{
	const int32 Stacks = GetLiveStackCount();
	if (Stacks <= 0)
	{
		return 1.f;
	}

	const float Lifted = FMath::Min(
		TraceRoccoFxFile::AccentBaseGlow + TraceRoccoFxFile::AccentGlowPerStack * static_cast<float>(Stacks),
		TraceRoccoFxFile::AccentGlowCap);

	return Lifted / TraceRoccoFxFile::AccentBaseGlow;
}

void UTraceAbilitySetRocco::ApplyStackAccentTell()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive() || GetLiveStackCount() <= 0)
	{
		ClearStackAccentTell();
		return;
	}

	USkeletalMeshComponent* MeshComp = MyPawn->GetMesh();
	if (MeshComp == nullptr)
	{
		return;   // the Mannequin fallback path has no accent to lift; nothing to do and nothing wrong
	}

	const float Multiplier = GetAccentGlowMultiplier();
	const int32 SlotCount = MeshComp->GetNumMaterials();

	for (int32 Slot = 0; Slot < SlotCount; ++Slot)
	{
		// ApplyColorToSkeletalMesh has already wrapped every slot in a MID; anything that is not one
		// is a slot that has never been team-painted, and writing to it would do nothing anyway.
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(MeshComp->GetMaterial(Slot));
		if (MID == nullptr)
		{
			continue;
		}

		// A SLOT WITHOUT THE PARAMETER IS NOT AN ERROR. Only the generated bodies' accent material
		// declares AccentGlow; the suit, the inset and the Mannequin's own materials do not, and
		// GetScalarParameterValue answering false is how this loop finds the one that matters
		// without this file knowing the slot layout of ten different bodies.
		float Current = 0.f;
		if (!MID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("AccentGlow")), Current))
		{
			continue;
		}

		// *** THE STOMP LATCH. *** If what is in the material is not what this function last wrote,
		// then ApplyColorToSkeletalMesh has been through since — a Core pickup, a team change, a
		// respawn — and whatever it left IS the new base. This is what makes the lift survive the
		// stomp instead of fighting it, and it is why the restore does not need a remembered value.
		if (!FMath::IsNearlyEqual(Current, LastWrittenAccentGlow))
		{
			AccentBaseGlow = Current;
		}

		const float Want = AccentBaseGlow * Multiplier;
		MID->SetScalarParameterValue(TEXT("AccentGlow"), Want);
		LastWrittenAccentGlow = Want;
		bAccentLifted = true;
	}
}

void UTraceAbilitySetRocco::ClearStackAccentTell()
{
	if (!bAccentLifted)
	{
		return;
	}

	bAccentLifted = false;
	AccentBaseGlow = 0.f;
	LastWrittenAccentGlow = -1.f;

	// THROUGH ApplyTeamColors(), NOT BY WRITING BACK A NUMBER. It is the one function that knows what
	// the accent should be for this pawn's CURRENT state — 8 normally, 30 while carrying the Core, 0
	// while dead — and it is re-entrant and called from a dozen places already. Restoring a
	// remembered base instead would put the pre-boost brightness on a Rocco who picked up the Core
	// during the boost, which is MASTER_PLAN risk 4 happening in the restore rather than in the lift.
	if (ATraceCharacter* MyPawn = GetCharacter())
	{
		MyPawn->ApplyTeamColors();
	}
}

// =================================================================================================
// ACTIVATED — the Ripple
// =================================================================================================

float UTraceAbilitySetRocco::GetActivatedCooldownSeconds() const
{
	// §6: "20 s cooldown", and it is a DIFFERENT NUMBER IN A DIFFERENT PLACE from the movement
	// component's dash charge pool — which is the whole of "a separate cooldown from the standard
	// dash". Firing this spends no dash charge and never touches DashTimeRemaining.
	return UTraceSettings::Get().RoccoRippleCooldownSeconds;
}

bool UTraceAbilitySetRocco::ComputeRipplePath(FVector& OutStart, FVector& OutDirection, float& OutLength) const
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* Move = GetMovement();
	if (MyPawn == nullptr || Move == nullptr)
	{
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// "DASH IN ANY DIRECTION" — composed by the SAME pure function the standard dash uses, so the
	// Ripple's direction rules are the dash's direction rules by construction rather than by a copy
	// that would drift. W/S carry the aim pitch, which is what makes straight up and diagonals work.
	OutDirection = Move->ComputeDashDirection(Move->GetCurrentAcceleration(), MyPawn->GetControlRotation());
	OutStart = MyPawn->GetActorLocation();
	OutLength = (Settings.DashSpeed * Settings.RoccoRippleDashSpeedMultiplier)
	          * (Settings.DashDuration * Settings.RoccoRippleDashDurationMultiplier);

	return !OutDirection.IsNearlyZero() && OutLength > 0.f;
}

bool UTraceAbilitySetRocco::ActivateAbility()
{
	FVector Start = FVector::ZeroVector;
	FVector Direction = FVector::ZeroVector;
	float Length = 0.f;
	if (!ComputeRipplePath(Start, Direction, Length))
	{
		return false;   // a fizzle: no pawn, no movement component. Do not charge for it.
	}

	if (!HasAuthority())
	{
		// PREDICTED HALF. There is deliberately nothing here yet. The ride is driven by the
		// REPLICATED ripple actor, which each client already simulates for its own pawn, so the only
		// thing a local spawn would buy is the ~1 RTT before the actor arrives — at the cost of two
		// ripples briefly existing on one machine and both writing Velocity. That trade is called
		// out in the report as a known, measurable gap rather than papered over.
		return true;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	if (CVarRoccoRippleEnabled.GetValueOnAnyThread() == 0)
	{
		// RED ARM: charge the cooldown, lay no path. Everything else about the two arms is identical.
		return true;
	}

	UWorld* WorldPtr = GetWorld();
	ATraceCharacter* MyPawn = GetCharacter();
	if (WorldPtr == nullptr || MyPawn == nullptr)
	{
		return false;
	}

	// ONE RIPPLE PER ROCCO. Firing again replaces the old path rather than stacking two, which keeps
	// "lasts 4 s" a statement about a thing rather than about a set of things.
	DestroyActiveRipple();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Instigator = MyPawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	ATraceRippleActor* Ripple = WorldPtr->SpawnActor<ATraceRippleActor>(
		ATraceRippleActor::StaticClass(), Start, FRotator::ZeroRotator, SpawnParams);
	if (Ripple == nullptr)
	{
		return false;
	}

	const float RideSpeed = Settings.DashSpeed * Settings.RoccoRippleRideSpeedMultiplier;
	const float Expiry = MatchTimeNow() + Settings.RoccoRippleLifetimeSeconds;

	APlayerState* SourceState = (GetAbilityComponent() != nullptr)
		? GetAbilityComponent()->GetOwningPlayerState()
		: nullptr;

	Ripple->InitialiseRipple(SourceState, Start, Direction, Length, RideSpeed,
		Settings.RoccoRippleEntryRadiusUU, Expiry);

	ActiveRipple = Ripple;

	// SPEC v29 §1f — RoccoRipple. The third WAV with no stated trigger, and the name settles it:
	// "Rocco's Ripple ability firing". This is the moment it fires — authority only (the early return
	// above means a client never reaches here), once per cast, after the path actually exists so a
	// fizzle is silent.
	//
	// GAME-SIDE, and that is the judgement §1f asks for. A Ripple is a thing OTHER players ride and
	// other players have to react to; it belongs in the same class as Dash and Parry, which spec v26
	// §9 already made game-side. At the START RING rather than at Rocco, because the start ring is
	// where the path begins and where a team-mate has to go to get on it.
	TraceAudio::PlayAt(MyPawn, TraceSoundEvents::RoccoRipple, Start);

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags |= TraceAbilityFlags::AuxActive;
	Writable.AuxEndMatchTime = Expiry;
	Writable.AuxLocation = Start;
	Writable.AuxDirection = Direction;
	MarkStateDirty();

	UE_LOG(LogTraceGame, Log,
		TEXT("[Rocco] RIPPLE laid by %s: start (%s) dir (%s) length %.0f uu, ride %.0f uu/s, entry radius %.0f uu, "
		     "expires at match time %.2f (%.1fs). Cooldown %.0fs (separate from the dash pool)."),
		*GetNameSafe(SourceState), *Start.ToCompactString(), *Direction.ToCompactString(), Length, RideSpeed,
		Settings.RoccoRippleEntryRadiusUU, Expiry, Settings.RoccoRippleLifetimeSeconds,
		Settings.RoccoRippleCooldownSeconds);

	return true;
}

// =================================================================================================
// MOVEMENT — "a very small second jump, which allows Rocco to change direction midair, instantly"
// =================================================================================================

bool UTraceAbilitySetRocco::OnJumpPressed()
{
	if (CVarRoccoSecondJumpEnabled.GetValueOnAnyThread() == 0)
	{
		return false;   // RED ARM: decline, and the normal jump runs as if Rocco were a Mannequin.
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* Move = GetMovement();
	if (MyPawn == nullptr || Move == nullptr || !MyPawn->IsAlive())
	{
		return false;
	}

	// ON THE GROUND THE NORMAL JUMP OWNS THE KEY. This ability is a SECOND jump; consuming the first
	// one would replace a 600 uu/s launch with a 260 uu/s one and read as the jump being broken.
	if (Move->IsMovingOnGround())
	{
		return false;
	}

	if (bSecondJumpUsed)
	{
		return false;
	}
	bSecondJumpUsed = true;

	const UTraceSettings& Settings = UTraceSettings::Get();

	const FVector VelocityBefore = Move->Velocity;
	FVector NewVelocity = VelocityBefore;

	// --- THE POINT OF THE ABILITY: the instant direction change -----------------------------------
	const FVector Wish = Move->GetCurrentAcceleration().GetSafeNormal2D();
	if (!Wish.IsNearlyZero())
	{
		// Speed is PRESERVED, not granted: the redirect turns the existing planar momentum, it does
		// not add to it. [ASSUMPTION] a floor of the ground speed limit, so that a Rocco who is
		// falling straight down with no planar speed still gets a usable change of direction —
		// without it the ability would silently do nothing in the one situation it reads as being
		// for ("change direction midair").
		const float PlanarSpeed = FMath::Max(NewVelocity.Size2D(), Move->GetMaxSpeed());
		const FVector CurrentPlanar(NewVelocity.X, NewVelocity.Y, 0.f);
		const FVector TargetPlanar = Wish * PlanarSpeed;

		const float Fraction = FMath::Clamp(Settings.RoccoSecondJumpRedirectFraction, 0.f, 1.f);
		const FVector BlendedPlanar = FMath::Lerp(CurrentPlanar, TargetPlanar, Fraction);

		NewVelocity.X = BlendedPlanar.X;
		NewVelocity.Y = BlendedPlanar.Y;
	}

	// --- "a VERY SMALL second jump" — the height is the smaller half of the ability ----------------
	// Max, not assignment: a Rocco still rising from his first jump must not be SLOWED by pressing
	// jump again. The floor is what makes it a jump; the ceiling is whatever he already had.
	NewVelocity.Z = FMath::Max(NewVelocity.Z, Settings.RoccoSecondJumpZVelocity);

	Move->Velocity = NewVelocity;

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Rocco] second jump: redirect fraction %.2f, planar (%.0f,%.0f) -> (%.0f,%.0f), Z %.0f -> %.0f."),
		Settings.RoccoSecondJumpRedirectFraction, VelocityBefore.X, VelocityBefore.Y,
		NewVelocity.X, NewVelocity.Y, VelocityBefore.Z, NewVelocity.Z);

	// SPEC v26 §9 — Jump, client-side, and it has to be HERE because of the line below.
	//
	// FOUND BY THE INTEGRATION HARNESS, not by reading. Returning true consumes the press, so
	// ACharacter::Jump is never called and UTraceCharacterMovementComponent::DoJump — where the Jump
	// sound lives for everybody else — never runs. Rocco's second jump was therefore the one jump in
	// the game that made no sound at all: the player presses the jump key, leaves the ground, and
	// hears nothing. (The same press next to a wall also becomes a second jump rather than a wall
	// jump, which is why the harness picks MACE for the wall-jump step.)
	//
	// Exactly one play, by construction: DoJump is unreachable on this path, so this cannot double
	// with the movement layer's call. Same client-side gate as every other Jump — only the machine
	// whose player pressed the key hears it.
	TraceAudio::Play(MyPawn, TraceSoundEvents::Jump);

	// =============================================================================================
	// *** FX_AUDIO_PLAN §2.9's SECOND-JUMP ROW. The half of this ability nobody else could see. ***
	//
	// Until this pass the second jump was, to everyone but its owner, a player changing direction in
	// mid-air for no visible reason — and after spec v26 it was the one jump that made no sound to
	// anybody else either (the Jump above is client-side and reaches only the presser's machine).
	//
	// AUTHORITY ONLY, AND THAT IS THE WHOLE OF THE DOUBLE-FIRE RULE. This function runs on BOTH the
	// owning client and the server (ATracePlayerController::OnJumpStarted, then
	// UTraceAbilityComponent::ServerHandleJumpPressed), so an unguarded spawn here would put two
	// rings and two sounds on a listen host and one of each on the owner a round trip early. The
	// burst actor's own replication IS the multicast (W3-FXBURST §7 note 1), so the server's single
	// copy reaches every machine including the presser's — one press, one ring, everywhere.
	//
	// GenericRing is the only burst type that takes a tint, and its DEFAULT hue is already Rocco
	// amber, so nothing is passed: this call site is the one the type's default was written for.
	// UpVector because for GenericRing the direction is the ring's NORMAL, and a ground ring under
	// the feet is what "air-burst ring under feet" means.
	//
	// RoccoJump is World-side and therefore authority-only too, which is what makes it AUDIBLE TO
	// OTHER PLAYERS — the point of the row. It does not replace the client-side Jump above; §2.9 says
	// so in as many words ("replaces nothing; the generic Jump at :344 stays"), because the two are
	// answering different questions: Jump is the presser's own feedback, this is the world's.
	// =============================================================================================
	if (HasAuthority())
	{
		if (UWorld* WorldPtr = MyPawn->GetWorld())
		{
			const FVector Feet = MyPawn->GetActorLocation()
				- FVector(0.f, 0.f, MyPawn->GetSimpleCollisionHalfHeight());

			ATraceFxBurst::Burst(WorldPtr, ETraceFxBurstType::GenericRing, Feet, FVector::UpVector,
				TraceRoccoFxFile::SecondJumpRingRadiusUU);
		}

		TraceAudio::PlayAt(MyPawn, TraceSoundEvents::RoccoJump, MyPawn->GetActorLocation());
	}

	// TRUE CONSUMES THE JUMP. The normal jump must not also run — that would be a double launch.
	return true;
}

// =================================================================================================
// PASSIVE — "3% speed boost from headshot kills for 1 second, stacking, each kill extends the timer
//            on the entire boost"
// =================================================================================================

void UTraceAbilitySetRocco::OnKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot)
{
	if (!HasAuthority() || !bHeadshot)
	{
		// HEADSHOT KILLS ONLY. A body-shot kill, a knife kill and a trace kill all leave the stack
		// exactly where it was — including its timer, which is not refreshed either.
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// Spec §6 [ASSUMPTION], and the spec asks for it: "cap the stack (say 10 = +30%) — unbounded is
	// a bug waiting to happen; make the cap a knob."
	const int32 Cap = (CVarRoccoStackCapEnabled.GetValueOnAnyThread() != 0)
		? FMath::Max(1, Settings.RoccoHeadshotSpeedStackMax)
		: 255;                                   // RED ARM: uncapped, bounded only by the uint8

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Stacks = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Writable.Stacks) + 1, 0, FMath::Min(Cap, 255)));

	// *** ONE TIMER OVER THE WHOLE STACK. *** "each kill extends the timer on the entire boost" —
	// so this is an assignment, not a per-stack timer and not an addition. Absolute match-clock
	// time, like every other timer in the framework.
	Writable.EffectEndMatchTime = MatchTimeNow() + FMath::Max(0.f, Settings.RoccoHeadshotSpeedDurationSeconds);
	Writable.Flags |= TraceAbilityFlags::EffectActive;
	MarkStateDirty();

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Rocco] headshot kill on %s (%s): stack %d (cap %d) = +%.0f%% speed, whole-stack window refreshed "
		     "to %.1fs."),
		*GetNameSafe(Victim), *Cause.ToString(), Writable.Stacks, Cap,
		Writable.Stacks * Settings.RoccoHeadshotSpeedBonusPerStack * 100.f,
		Settings.RoccoHeadshotSpeedDurationSeconds);
}

int32 UTraceAbilitySetRocco::GetLiveStackCount() const
{
	const FTraceAbilityNetState& Current = State();
	if ((Current.Flags & TraceAbilityFlags::EffectActive) == 0)
	{
		return 0;
	}
	return (MatchTimeNow() < Current.EffectEndMatchTime) ? static_cast<int32>(Current.Stacks) : 0;
}

float UTraceAbilitySetRocco::GetStackSecondsRemaining() const
{
	const FTraceAbilityNetState& Current = State();
	if ((Current.Flags & TraceAbilityFlags::EffectActive) == 0)
	{
		return 0.f;
	}
	return FMath::Max(0.f, Current.EffectEndMatchTime - MatchTimeNow());
}

float UTraceAbilitySetRocco::GetMoveSpeedMultiplier() const
{
	// Read from the REPLICATED state, so a client's own movement prediction and the server compute
	// the same multiplier from the same numbers. Cheap and pure — this is called every movement tick.
	const int32 Stacks = GetLiveStackCount();
	if (Stacks <= 0)
	{
		return 1.f;
	}
	return 1.f + (static_cast<float>(Stacks) * UTraceSettings::Get().RoccoHeadshotSpeedBonusPerStack);
}

#if !UE_BUILD_SHIPPING

// =================================================================================================
// Trace.Rocco.FxTest — FX_AUDIO_PLAN §2.9's acceptance
//
// Trace.Rocco.Verify already proves the three ABILITIES; Trace.Rocco.RippleShot already photographs
// the two ring colours. What neither of them can say is whether the six §2.9 presentation elements
// exist on a machine that is not the caster's — which is the whole of the F10 blocker this pass is
// closing. So this measures each of them off the live objects:
//
//   RING COLOURS      the two knobs, printed, so "the starting ring should be a different colour"
//                     (Demo 13) is a readable fact in the log and not only in a screenshot.
//   START-RING PULSE  which of the two §2.9 routes carried it — M_TraceNeon's PulseAmp/PulseRate, or
//                     the actor's per-frame Glow fallback. Both are correct; knowing WHICH is what
//                     stops a future "the pulse is gone" from being debugged in the wrong file.
//   DISSOLVE          sampled twice, once mid-life and once inside the last 0.3 s, so the fade is
//                     measured as a CHANGE rather than asserted from the presence of the code.
//   RIDE FX + LOOP    a real pawn is walked into the entrance and the ripple's own presented-rider
//                     count and speed-line instances are read on the next tick.
//   SECOND JUMP       the ring burst and the RoccoJump sound, counted the same way Mortimer's are:
//                     new actors of the right type, and the audio subsystem's own play map.
//   STACK TELL        the accent multiplier AND the value that actually landed on the body MID —
//                     "we called SetScalarParameterValue" and "the material is brighter" are
//                     different claims, and the stomp latch is exactly the kind of code where they
//                     come apart.
//
// TWO PHASES on a ticker, because a ride cannot be staged inside one call stack: ATraceRippleActor
// picks riders up in its own Tick, and the FX are drawn from the pass after that.
// =================================================================================================

// NAMED after the file rather than anonymous: UBT builds this module as a unity blob, so two files
// that each open `namespace { }` become one. See Scripts/check-jumbo-build-collisions.py.
namespace TraceRoccoFxTestFile
{
	struct FRoccoFxRun
	{
		int32 SetupAttemptsLeft = 30;
		int32 Phase = 0;
		double NextRealTime = 0.0;

		TWeakObjectPtr<UTraceAbilitySetRocco> Rocco;
		TWeakObjectPtr<ATraceRippleActor> Ripple;
		TWeakObjectPtr<ATraceCharacter> Rider;

		/** Readings taken in phase 0 and reported in phase 1. */
		float DissolveEarly = -1.f;
		int32 JumpBursts = 0;
		int32 JumpSoundPlays = -1;
		float AccentBefore = -1.f;
		float AccentAfter = -1.f;
		float AccentMultiplier = 1.f;
		bool bAccentParamFound = false;

		/** Carried from phase 1 to phase 2, which is where the verdict is given. */
		int32 Beads = 0;
		int32 PeakRiders = 0;
		float DissolveMidLife = -1.f;
		bool bPulseInMaterial = false;
	};

	UWorld* FindAuthoritativeGameWorld()
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

	/** The first HUMAN player's ability component. A bot's would fight the game mode's 4 Hz fill. */
	UTraceAbilityComponent* FindHumanComponent(UWorld* WorldPtr)
	{
		const AGameStateBase* GS = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (GS == nullptr)
		{
			return nullptr;
		}
		for (APlayerState* PS : GS->PlayerArray)
		{
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PS);
			if (Comp != nullptr && !Comp->IsBot())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/** The AccentGlow currently on @p Pawn's body, or -1 when no slot declares the parameter. */
	float ReadAccentGlow(const ATraceCharacter* Pawn, bool& bOutFound)
	{
		bOutFound = false;
		const USkeletalMeshComponent* MeshComp = (Pawn != nullptr) ? Pawn->GetMesh() : nullptr;
		if (MeshComp == nullptr)
		{
			return -1.f;
		}

		for (int32 Slot = 0; Slot < MeshComp->GetNumMaterials(); ++Slot)
		{
			const UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(MeshComp->GetMaterial(Slot));
			float Value = 0.f;
			if (MID != nullptr && MID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("AccentGlow")), Value))
			{
				bOutFound = true;
				return Value;
			}
		}
		return -1.f;
	}

	bool TickRoccoFx(TSharedPtr<FRoccoFxRun> Run);

	void Schedule(TSharedPtr<FRoccoFxRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + DelaySeconds;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickRoccoFx(Run);
			}), 0.f);
	}

	bool TickRoccoFx(TSharedPtr<FRoccoFxRun> Run)
	{
		const TCHAR* const Tag = TEXT("ROCCOFX");

		UWorld* WorldPtr = FindAuthoritativeGameWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. The ripple is server-spawned; "
				     "run this on the host."), Tag);
			return false;
		}

		// ---- PHASE 0: stage everything ----------------------------------------------------------
		if (Run->Phase == 0)
		{
			UTraceAbilityComponent* Comp = FindHumanComponent(WorldPtr);
			ATraceCharacter* Pawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (Comp == nullptr || Pawn == nullptr)
			{
				if (Run->SetupAttemptsLeft-- > 0)
				{
					Schedule(Run, 1.0f);
					return false;
				}
				UE_LOG(LogTraceGame, Warning,
					TEXT("[%s] VERDICT: INVALID — no human player with a pawn inside the budget."), Tag);
				return false;
			}

			Comp->ServerSetCharacter(ETraceCharacterId::Rocco);
			Comp->OnHalfTime();   // clears the activated cooldown so E is available right now

			UTraceAbilitySetRocco* Rocco = Comp->GetAbilitySetAs<UTraceAbilitySetRocco>();
			if (Rocco == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[%s] VERDICT: INVALID — the ability set did not build as Rocco."), Tag);
				return false;
			}
			Run->Rocco = Rocco;

			UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(WorldPtr);
			auto JumpPlays = [Audio]() -> int32
			{
				if (Audio == nullptr)
				{
					return -1;
				}
				const int32* Found = Audio->GetPlaysByEvent().Find(TraceSoundEvents::RoccoJump);
				return (Found != nullptr) ? *Found : 0;
			};

			// ---- the ripple, through the SHIPPED activation path ---------------------------------
			const bool bFired = Comp->TryActivate();

			ATraceRippleActor* Found = nullptr;
			for (TActorIterator<ATraceRippleActor> It(WorldPtr); It; ++It)
			{
				Found = *It;
			}
			Run->Ripple = Found;

			if (Found == nullptr)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[%s] VERDICT: INVALID — TryActivate returned %d and no ripple exists. Nothing "
					     "below could have been measured."), Tag, bFired ? 1 : 0);
				return false;
			}

			// THE RINGS ARE NOT BUILT YET AND THAT IS NOT A BUG. ATraceRippleActor is spawned with a
			// plain SpawnActor, so its BeginPlay runs INSIDE the spawn call — before InitialiseRipple
			// has landed the path — and BuildRingsIfNeeded therefore does nothing until the actor's
			// first Tick. Reading the bead count here reported "0 beads, blends None" for a ripple
			// that built 40 beads one frame later. Everything about the rings is read in phase 1.
			Run->DissolveEarly = Found->GetDissolveAlpha();

			// ---- the second jump: airborne, then the shipped hook --------------------------------
			//
			// LAUNCHED FIRST, because OnJumpPressed declines on the ground by design ("the normal jump
			// owns the key"), so a test that pressed from a standing start would measure the refusal.
			TSet<const AActor*> BurstsBefore;
			for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
			{
				BurstsBefore.Add(*It);
			}
			const int32 PlaysBefore = JumpPlays();

			// *** MOVE_Falling FIRST, THEN THE PRESS. *** OnJumpPressed declines on the ground by
			// design ("the normal jump owns the key"), and LaunchCharacter does NOT leave the ground
			// in the frame it is called — the movement component decides that on its next update. The
			// first version of this test launched and pressed in the same call stack, read
			// IsMovingOnGround() as still true, and recorded a refusal as "the second jump did not
			// fire". Setting the mode is what the ripple's own UpdateRides does for the same reason.
			if (UTraceCharacterMovementComponent* Move = Pawn->GetTraceMovement())
			{
				Move->SetMovementMode(MOVE_Falling);
				Move->Velocity = FVector(0.f, 0.f, 400.f);
			}
			const bool bJumped = Rocco->OnJumpPressed();

			for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
			{
				ATraceFxBurst* Burst = *It;
				if (Burst != nullptr && !BurstsBefore.Contains(Burst)
					&& Burst->GetBurstType() == ETraceFxBurstType::GenericRing)
				{
					++Run->JumpBursts;
				}
			}
			Run->JumpSoundPlays = (PlaysBefore >= 0) ? (JumpPlays() - PlaysBefore) : -1;

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] JUMP    second jump accepted=%d -> %d GenericRing burst(s), RoccoJump plays +%d."),
				Tag, bJumped ? 1 : 0, Run->JumpBursts, Run->JumpSoundPlays);

			// ---- the stack tell -------------------------------------------------------------------
			//
			// The stack is written straight into the replicated pad rather than staged through four
			// headshot kills: the tell is a presentation of THAT NUMBER, and OnKill's arithmetic is
			// already Trace.Rocco.Verify's job. Four stacks is the first count that reaches §2.9's cap.
			Run->AccentBefore = ReadAccentGlow(Pawn, Run->bAccentParamFound);

			FTraceAbilityNetState& Writable = Comp->GetMutableNetState();
			Writable.Stacks = 4;
			Writable.Flags |= TraceAbilityFlags::EffectActive;
			Writable.EffectEndMatchTime = Found->GetExpireMatchTime() + 30.f;   // well past this run
			Comp->MarkNetStateDirty();

			// SyncClientFx and not a private call: it is the §1.2 hook a joining machine uses, so
			// driving the tell through it tests the join-in-progress path at the same time.
			Rocco->SyncClientFx(Comp->GetNetState());

			Run->AccentMultiplier = Rocco->GetAccentGlowMultiplier();
			Run->AccentAfter = ReadAccentGlow(Pawn, Run->bAccentParamFound);

			// ---- stage a rider --------------------------------------------------------------------
			//
			// A DIFFERENT PAWN, deliberately: §2.9 notes that the rider may be any character and that
			// the FX are Rocco's amber regardless, and a test that rode its own ripple would never
			// exercise the derived-rider path at all.
			for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
			{
				ATraceCharacter* Candidate = *It;
				if (Candidate != nullptr && Candidate != Pawn && Candidate->IsAlive())
				{
					Candidate->SetActorLocation(Found->GetRippleStart(), false, nullptr,
						ETeleportType::TeleportPhysics);
					Run->Rider = Candidate;
					break;
				}
			}

			Run->Phase = 1;
			Schedule(Run, 0.5f);
			return false;
		}

		// ---- PHASE 1: read what the world did with it --------------------------------------------
		if (Run->Phase == 1)
		{
		ATraceRippleActor* Ripple = Run->Ripple.Get();
		UTraceAbilitySetRocco* Rocco = Run->Rocco.Get();
		if (Ripple == nullptr || Rocco == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the ripple or the ability set went away between phases."), Tag);
			return false;
		}

		Run->PeakRiders = Ripple->GetPeakPresentedRiderCount();
		Run->Beads = Ripple->GetDrawnBeadCount();
		Run->DissolveMidLife = Ripple->GetDissolveAlpha();
		Run->bPulseInMaterial = Ripple->IsPulseInMaterial();

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] RINGS   start colour %s / trail colour %s (Demo 13's \"a different colour\"), "
			     "%d beads drawn, blends %s, start pulse carried by %s."),
			Tag, *Settings.RoccoRippleStartRingColor.ToString(),
			*Settings.RoccoRippleTrailRingColor.ToString(),
			Ripple->GetDrawnBeadCount(), *Ripple->DescribeBlends(),
			Ripple->IsPulseInMaterial() ? TEXT("M_TraceNeon PulseAmp/PulseRate")
			                            : TEXT("the actor's per-frame Glow write (§2.9 fallback)"));

		// THE PEAK, NOT THE CURRENT COUNT. A ride down a 378 uu path is over in a third of a second;
		// see GetPeakPresentedRiderCount for the sampling failure this replaced.
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] RIDE    %d rider(s) simulated now, %d presented now, %d presented at the PEAK "
			     "(speed lines + RoccoRideLoop). Staged rider: %s."),
			Tag, Ripple->GetRiderCount(), Ripple->GetPresentedRiderCount(), Run->PeakRiders,
			*GetNameSafe(Run->Rider.Get()));

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] ACCENT  %d live stack(s) -> x%.2f lift; body AccentGlow %.2f -> %.2f (parameter "
			     "present on the body: %d)."),
			Tag, Rocco->GetLiveStackCount(), Run->AccentMultiplier,
			Run->AccentBefore, Run->AccentAfter, Run->bAccentParamFound ? 1 : 0);

		// ---- and hand over to phase 2, which is the only place the DISSOLVE can be measured ------
		//
		// §2.9's fade is the last 0.3 s of a five-and-a-half second life, so it cannot be sampled from
		// here: a reading taken now would always be 1.00 and the claim "the rings fade" would rest on
		// the code existing rather than on the code running. The wait is computed from the ripple's OWN
		// replicated deadline rather than from a constant, so a retuned RoccoRippleLifetimeSeconds
		// cannot silently move the sample out of the window.
		{
			// The match clock, read the same way every timer in the framework reads it. Rocco's own
			// MatchTimeNow() is protected, and the ripple's is private — both correctly, since neither
			// is a public service.
			const AGameStateBase* Clock = WorldPtr->GetGameState();
			const float MatchNow = (Clock != nullptr) ? static_cast<float>(Clock->GetServerWorldTimeSeconds()) : 0.f;
			const float Remaining = Ripple->GetExpireMatchTime() - MatchNow;
			const float WaitSeconds = FMath::Max(0.05f, Remaining - 0.15f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] FADE    dissolve alpha %.2f at stage time, %.2f mid-life. Sampling again in "
				     "%.2fs, i.e. %.2fs before the ripple's own deadline."),
				Tag, Run->DissolveEarly, Run->DissolveMidLife, WaitSeconds, Remaining - WaitSeconds);

			Run->Phase = 2;
			Schedule(Run, WaitSeconds);
			return false;
		}
	}

	// ---- PHASE 2: the expiry dissolve, and the verdict -------------------------------------------
	{
		ATraceRippleActor* Ripple = Run->Ripple.Get();
		const float DissolveLate = (Ripple != nullptr) ? Ripple->GetDissolveAlpha() : 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] FADE    dissolve alpha %.2f mid-life -> %.2f inside the last 0.3 s (ripple still "
			     "alive: %d). Cosmetic only: the ride, the entry radius and the destroy all still run "
			     "off the one ExpireMatchTime."),
			Tag, Run->DissolveMidLife, DissolveLate, (Ripple != nullptr) ? 1 : 0);

		// ---- the verdict ---------------------------------------------------------------------------
		const bool bRingsDrawn = Run->Beads > 0;
		const bool bJumpFired = Run->JumpBursts > 0;
		const bool bRideDrawn = Run->PeakRiders > 0;

		// MEASURED AS A CHANGE. A dissolve that is present in the code but never reaches the material
		// would read 1.00 here, and 1.00 is exactly what the mid-life sample already said.
		const bool bDissolved = (Run->DissolveMidLife > 0.99f) && (DissolveLate < 0.9f);

		// The accent half is only ASSERTED on a body that actually has the parameter. The Mannequin
		// fallback (TraceCharacter.cpp's roster fallback) has no accent stripes to lift, and failing a
		// run for that would be failing it for a documented, supported state of the project.
		const bool bAccentOk = !Run->bAccentParamFound
			|| (Run->AccentAfter > Run->AccentBefore && Run->AccentMultiplier > 1.f);

		if (bRingsDrawn && bJumpFired && bRideDrawn && bAccentOk && bDissolved)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — every §2.9 element fired. %d ring beads in two colours with the "
				     "start ring on a %s pulse, %d GenericRing burst and RoccoJump +%d on the second jump, "
				     "%d rider(s) presented at the peak with amber speed lines and the ride loop, the "
				     "accent lifted %.2f -> %.2f (x%.2f) on the body, and the rings dissolved %.2f -> %.2f "
				     "instead of popping out."),
				Tag, Run->Beads, Run->bPulseInMaterial ? TEXT("material") : TEXT("per-frame"),
				Run->JumpBursts, Run->JumpSoundPlays, Run->PeakRiders,
				Run->AccentBefore, Run->AccentAfter, Run->AccentMultiplier,
				Run->DissolveMidLife, DissolveLate);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] VERDICT: FAIL — rings drawn=%d, second-jump burst=%d, ride presented=%d, "
				     "accent lifted=%d, rings dissolved=%d. (A zero ride count with a staged rider usually "
				     "means the pawn was not inside RoccoRippleEntryRadiusUU when the ripple ticked, or "
				     "that it had already ridden this path — one ride per player per ripple.)"),
				Tag, bRingsDrawn ? 1 : 0, bJumpFired ? 1 : 0, bRideDrawn ? 1 : 0, bAccentOk ? 1 : 0,
				bDissolved ? 1 : 0);
		}

		return false;
	}
	}

	FAutoConsoleCommand CmdRoccoFxTest(
		TEXT("Trace.Rocco.FxTest"),
		TEXT("FX_AUDIO_PLAN §2.9. Lays a Ripple through the shipped E path and measures every presentation "
		     "element off the live objects: the two ring colours and which route carries the start-ring "
		     "pulse, the expiry dissolve, the second jump's GenericRing burst and RoccoJump sound, a staged "
		     "rider's speed lines and ride loop, and the headshot-stack accent lift on the body material."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			Schedule(MakeShared<FRoccoFxRun>(), 0.f);
		}));
}

#endif // !UE_BUILD_SHIPPING
