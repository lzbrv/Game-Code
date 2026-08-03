// Trace — the client-predicted movement kit: dash (charged), slide, air fast-fall, boost.
// See the header for the full prediction model and the design rationale.

#include "Movement/TraceCharacterMovementComponent.h"

#include "Components/SceneComponent.h"
#include "Core/TraceCharacter.h"
#include "Engine/World.h"                      // UWorld::GetTimeSeconds (dash-active latch)
#include "GameFramework/Character.h"
#include "Math/UnrealMathUtility.h"
#include "Trace.h"                             // LogTraceGame
#include "TraceSettings.h"

#if !UE_BUILD_SHIPPING
#include "GameFramework/PlayerController.h"    // self-test: player-controlled check
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

namespace TraceMovement
{
	/**
	 * Rotates a unit vector toward another by at most MaxDegrees, in the plane containing both.
	 *
	 * Used for the slide's weak steering. Deliberately a fixed angular rate rather than an
	 * interpolation toward a target: the result depends only on (Current, Desired, MaxDegrees), all
	 * of which the replay path reproduces exactly, so client and server land on the same vector.
	 */
	FVector SteerTowards(const FVector& Current, const FVector& Desired, float MaxDegrees)
	{
		const float Dot = FMath::Clamp(FVector::DotProduct(Current, Desired), -1.f, 1.f);
		const float AngleRad = FMath::Acos(Dot);
		const float MaxRad = FMath::DegreesToRadians(FMath::Max(0.f, MaxDegrees));

		if (AngleRad <= MaxRad || MaxRad <= 0.f)
		{
			return (AngleRad <= MaxRad) ? Desired : Current;
		}

		FVector Axis = FVector::CrossProduct(Current, Desired);
		if (!Axis.Normalize())
		{
			// Exactly opposed (or degenerate): spin around Z, which for two planar vectors is the
			// only axis that can produce the turn at all.
			Axis = FVector::UpVector;
		}

		const FVector Result = FQuat(Axis, MaxRad).RotateVector(Current);
		return Result.GetSafeNormal(UE_SMALL_NUMBER, Current);
	}
}

// -------------------------------------------------------------------------------------------
// UTraceCharacterMovementComponent
// -------------------------------------------------------------------------------------------

UTraceCharacterMovementComponent::UTraceCharacterMovementComponent()
{
	bWantsToDash = 0;
	bWantsToBoost = 0;
	bWantsToSlide = 0;

	DashTimeRemaining = 0.f;
	DashCharges = 1;
	DashRechargeRemaining = 0.f;
	LastMaxDashCharges = 1;
	DashDirection = FVector::ZeroVector;

	SlideTimeRemaining = 0.f;
	SlideCooldownRemaining = 0.f;
	SlideSpeed = 0.f;
	SlideDirection = FVector::ZeroVector;
	SlideBufferRemaining = 0.f;
	bSlideHeldLastMove = 0;

	BoostCooldownRemaining = 0.f;

	// Third-person feel: the capsule turns toward where it is moving. Aim is separate and comes
	// from the control rotation (ATraceCharacter::GetAimDirection), which is why the character
	// leaves bUseControllerRotationYaw off.
	bOrientRotationToMovement = true;
	bUseControllerDesiredRotation = false;
	RotationRate = FRotator(0.f, 900.f, 0.f);

	// Arena-shooter tuning. MaxWalkSpeed is overwritten from UTraceSettings in BeginPlay; the
	// literal here only covers the window before play starts (and the CDO in the editor).
	MaxWalkSpeed = 720.f;
	MaxAcceleration = 4096.f;
	BrakingDecelerationWalking = 2600.f;
	GroundFriction = 8.f;
	JumpZVelocity = 640.f;
	AirControl = 0.45f;
	bCanWalkOffLedges = true;

	// Lets ACharacter::Crouch() raise bWantsToCrouch at all — CanEverCrouch() gates it. The capsule is
	// still never resized, because CanCrouchInCurrentState() is overridden to false; this only opens
	// the already-predicted FLAG_WantsToCrouch channel so the crouch key can reach the slide.
	NavAgentProps.bCanCrouch = true;

	// Snap to small corrections rather than sliding: at these speeds a visible slide reads as lag.
	NetworkSimulatedSmoothLocationTime = 0.05f;
	NetworkSimulatedSmoothRotationTime = 0.05f;
}

void UTraceCharacterMovementComponent::BeginPlay()
{
	Super::BeginPlay();

	const UTraceSettings& Settings = UTraceSettings::Get();
	MaxWalkSpeed = FMath::Max(1.f, Settings.WalkSpeed);
	MaxWalkSpeedCrouched = MaxWalkSpeed * 0.5f;

	// Start full. GetMaxDashCharges() is safe this early — a pawn that has not been told it is the
	// carrier simply reads the base pool.
	LastMaxDashCharges = GetMaxDashCharges();
	DashCharges = LastMaxDashCharges;
	DashRechargeRemaining = 0.f;
}

bool UTraceCharacterMovementComponent::CanCrouchInCurrentState() const
{
	// See the header. bWantsToCrouch is an INPUT to the slide, never a request to shrink the capsule.
	return false;
}

// --- Settings accessors ---------------------------------------------------------------------
//
// Read live rather than cached: client and server must resolve the same numbers, and the config
// CDO is the single source of truth for both.

float UTraceCharacterMovementComponent::GetDashSpeed() const
{
	return FMath::Max(1.f, UTraceSettings::Get().DashSpeed);
}

float UTraceCharacterMovementComponent::GetDashDuration() const
{
	// A zero-length dash would be a one-frame teleport that the trail trip test could never see.
	return FMath::Max(0.01f, UTraceSettings::Get().DashDuration);
}

float UTraceCharacterMovementComponent::GetDashCooldown() const
{
	return FMath::Max(0.f, UTraceSettings::Get().DashCooldown);
}

float UTraceCharacterMovementComponent::GetDashRechargeWindow() const
{
	// The cooldown has always been measured from dash START (UTraceSettings::DashCooldown), and the
	// HUD's meter divides by exactly this quantity. Keeping every refill on the same window means a
	// second charge refills on the same rhythm as the first.
	return GetDashDuration() + GetDashCooldown();
}

int32 UTraceCharacterMovementComponent::GetMaxDashCharges() const
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	int32 Max = FMath::Max(1, Settings.BaseDashCharges);

	// Contract §5: the Core carrier gets one extra charge for as long as they carry. See the header
	// for why this one read is the kit's only prediction seam.
	bool bCarrying = false;
	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		bCarrying = TraceCharacter->IsCarrier();
	}

#if !UE_BUILD_SHIPPING
	// Dev override so the charge pool can be exercised without a Core. See TickSelfTest().
	extern int32 GTraceMoveKitFakeCarrier;
	if (GTraceMoveKitFakeCarrier != 0 && CharacterOwner != nullptr && CharacterOwner->IsLocallyControlled())
	{
		bCarrying = true;
	}
#endif

	if (bCarrying)
	{
		Max += FMath::Max(0, Settings.CarrierExtraDashCharges);
	}

	return Max;
}

// --- Prediction pipeline ---------------------------------------------------------------------

FNetworkPredictionData_Client* UTraceCharacterMovementComponent::GetPredictionData_Client() const
{
	// Deliberately no check(PawnOwner) (the shape most tutorials copy): the engine only ever asks
	// for prediction data on a pawn it is about to simulate, and asserting on a pointer a
	// disconnect can null out is exactly what contract §10 forbids. The base FNetworkPredictionData
	// constructor does not touch PawnOwner, so allocating unconditionally is safe.
	if (ClientPredictionData == nullptr)
	{
		UTraceCharacterMovementComponent* MutableThis = const_cast<UTraceCharacterMovementComponent*>(this);
		MutableThis->ClientPredictionData = new FNetworkPredictionData_Client_Trace(*this);
		MutableThis->ClientPredictionData->MaxSmoothNetUpdateDist = 92.f;
		MutableThis->ClientPredictionData->NoSmoothNetUpdateDist = 140.f;
	}

	return ClientPredictionData;
}

void UTraceCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);

	// Pure state restore. The actual activations happen in OnMovementUpdated, on every machine,
	// from these flags — so server, client and replay all go through one code path.
	bWantsToDash  = ((Flags & FSavedMove_Character::FLAG_Custom_0) != 0) ? 1 : 0;
	bWantsToBoost = ((Flags & FSavedMove_Character::FLAG_Custom_1) != 0) ? 1 : 0;
	bWantsToSlide = ((Flags & FSavedMove_Character::FLAG_Custom_2) != 0) ? 1 : 0;
}

// --- Dash ------------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::StartDash()
{
	if (!CanDash())
	{
		return;
	}

	// Do not mutate velocity here. Raising the intent and letting the next simulated move consume
	// it is what keeps the local prediction and the server's replay bit-for-bit identical: the
	// flag is captured by SetMoveFor and re-applied by UpdateFromCompressedFlags.
	bWantsToDash = 1;
}

bool UTraceCharacterMovementComponent::CanDash() const
{
	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return false;
	}

	// A charge is the whole gate now. DashRechargeRemaining is only the refill clock; with two
	// charges a carrier may dash again while it is still running.
	if (DashCharges <= 0)
	{
		return false;
	}

	// One dash at a time, whatever the pool says — chaining two dashes into one 5200uu/s smear is
	// not the mechanic, and the trail trip test reasons about a single dash window.
	if (DashTimeRemaining > 0.f)
	{
		return false;
	}

	// MOVE_None is what a dead or fully disabled pawn sits in.
	if (MovementMode == MOVE_None)
	{
		return false;
	}

	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		if (!TraceCharacter->IsAlive())
		{
			return false;
		}
	}

	return true;
}

bool UTraceCharacterMovementComponent::IsDashing() const
{
	// MOVE_None means the pawn has been switched off (dead, or being teleported between spawns), and
	// in that state OnMovementUpdated stops running — so the dash clock freezes wherever it was.
	// Without this guard a player who dies mid-dash would read as "dashing" forever, and the trail
	// trip test keys off exactly this function.
	return DashTimeRemaining > 0.f && MovementMode != MOVE_None;
}

float UTraceCharacterMovementComponent::GetDashCooldownRemaining() const
{
	// "Ready" means a charge is in hand AND no dash is currently running.
	if (DashCharges > 0 && DashTimeRemaining <= 0.f)
	{
		return 0.f;
	}

	// Mid-dash with a spare charge: the only thing in the way is the dash itself.
	if (DashCharges > 0)
	{
		return FMath::Max(0.f, DashTimeRemaining);
	}

	return FMath::Max(0.f, DashRechargeRemaining);
}

void UTraceCharacterMovementComponent::BeginDash()
{
	// Direction is locked here and never recomputed: a dash you can steer is not a dash, and a
	// steerable dash would also have to re-derive its direction identically during replay.
	//
	// Acceleration (not GetLastInputVector()) is the input source on purpose. Acceleration is
	// restored by MoveAutonomous() from the saved move on every replayed frame, so it reproduces
	// exactly; LastControlInputVector is consumed from live per-frame input and is *not* part of
	// the saved move, so using it would desync the client on every correction.
	//
	// Z is stripped here and never re-added anywhere in the dash: contract §5 is explicit that a
	// dash is a horizontal-plane burst and must not add vertical velocity. A dash in the air keeps
	// whatever Z the fall had.
	FVector Direction = Acceleration;
	Direction.Z = 0.f;

	if (!Direction.Normalize())
	{
		// Standing still: dash straight ahead. bOrientRotationToMovement means the capsule is
		// already facing the last movement direction, which is what a player expects.
		Direction = (UpdatedComponent != nullptr)
			? UpdatedComponent->GetForwardVector()
			: (CharacterOwner != nullptr ? CharacterOwner->GetActorForwardVector() : FVector::ForwardVector);
		Direction.Z = 0.f;

		if (!Direction.Normalize())
		{
			Direction = FVector::ForwardVector;
		}
	}

	DashDirection = Direction;
	DashTimeRemaining = GetDashDuration();

	// Spend a charge and make sure the refill clock is running. Only START it if it is idle: a
	// carrier who spends both charges in quick succession must wait out two sequential windows, not
	// have the first charge's progress thrown away by the second spend.
	DashCharges = FMath::Max(0, DashCharges - 1);
	if (DashRechargeRemaining <= 0.f)
	{
		DashRechargeRemaining = GetDashRechargeWindow();
	}

	// A dash cancels a slide — they are both "planar velocity is on rails" states and letting them
	// overlap would mean two writers fighting over Velocity every frame.
	if (SlideTimeRemaining > 0.f)
	{
		SlideTimeRemaining = 0.f;
		SlideSpeed = 0.f;
	}

	// Launch on the same frame the intent arrived. The move for this frame has already been
	// simulated by the time OnMovementUpdated runs, so this velocity lands on the next one — a
	// single frame, identically on both ends of the wire.
	const FVector Planar = DashDirection * GetDashSpeed();
	Velocity.X = Planar.X;
	Velocity.Y = Planar.Y;
	if (IsMovingOnGround())
	{
		Velocity.Z = 0.f;
	}
}

// --- Slide -----------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::SetWantsToSlide(bool bWants)
{
	// A level, not an edge: it is re-sent with every move and the edge is derived inside the
	// simulation (bSlideHeldLastMove), which is what lets the replay path reproduce a fast-fall.
	bWantsToSlide = bWants ? 1 : 0;
}

bool UTraceCharacterMovementComponent::IsSliding() const
{
	return SlideTimeRemaining > 0.f && MovementMode != MOVE_None;
}

bool UTraceCharacterMovementComponent::CanStartSlide() const
{
	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return false;
	}

	if (MovementMode == MOVE_None || !IsMovingOnGround())
	{
		return false;
	}

	if (SlideTimeRemaining > 0.f || SlideCooldownRemaining > 0.f || DashTimeRemaining > 0.f)
	{
		return false;
	}

	// The key must still be down when the buffered press finally lands, or a tap would start a
	// slide that the very next frame's "released early" test immediately cancels.
	if (!IsCrouchHeld())
	{
		return false;
	}

	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		if (!TraceCharacter->IsAlive())
		{
			return false;
		}
	}

	// A slide is a way of spending momentum you already have. Crouching from a standstill must not
	// hand out free speed, or "tap crouch" becomes the fastest way to cross the field.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float EntrySpeed = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideEntrySpeedFraction);
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);

	return PlanarVelocity.SizeSquared() >= FMath::Square(EntrySpeed);
}

void UTraceCharacterMovementComponent::BeginSlide()
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	// Direction comes from where the pawn is actually MOVING, not from where it is looking: a slide
	// is momentum. Velocity is restored by the replay path, so this reproduces exactly.
	FVector Direction(Velocity.X, Velocity.Y, 0.f);
	if (!Direction.Normalize())
	{
		Direction = Acceleration;
		Direction.Z = 0.f;
		if (!Direction.Normalize())
		{
			Direction = (UpdatedComponent != nullptr) ? UpdatedComponent->GetForwardVector() : FVector::ForwardVector;
			Direction.Z = 0.f;
			if (!Direction.Normalize())
			{
				Direction = FVector::ForwardVector;
			}
		}
	}

	const float PlanarSpeed = FVector(Velocity.X, Velocity.Y, 0.f).Size();
	const float Entry = FMath::Max(PlanarSpeed, FMath::Max(1.f, Settings.WalkSpeed));

	SlideDirection = Direction;
	SlideSpeed = FMath::Min(Entry * FMath::Max(1.f, Settings.SlideSpeedMultiplier),
	                        FMath::Max(1.f, Settings.SlideMaxSpeed));
	SlideTimeRemaining = FMath::Max(0.05f, Settings.SlideDuration);

	// Measured from slide START, same convention as the dash, so the knob reads the same way.
	SlideCooldownRemaining = SlideTimeRemaining + FMath::Max(0.f, Settings.SlideCooldown);

	Velocity.X = SlideDirection.X * SlideSpeed;
	Velocity.Y = SlideDirection.Y * SlideSpeed;
	if (IsMovingOnGround())
	{
		Velocity.Z = 0.f;
	}
}

void UTraceCharacterMovementComponent::EndSlide()
{
	SlideTimeRemaining = 0.f;
	SlideSpeed = 0.f;
	ClampPlanarSpeedToMax();
}

void UTraceCharacterMovementComponent::ClampPlanarSpeedToMax()
{
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
	const float PlanarLimit = GetMaxSpeed();
	if (PlanarVelocity.SizeSquared() > FMath::Square(PlanarLimit))
	{
		const FVector Clamped = PlanarVelocity.GetSafeNormal() * PlanarLimit;
		Velocity.X = Clamped.X;
		Velocity.Y = Clamped.Y;
	}
}

// --- Boost -----------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::StartBoost()
{
	if (!CanBoost())
	{
		return;
	}

	bWantsToBoost = 1;
}

bool UTraceCharacterMovementComponent::CanBoost() const
{
	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return false;
	}

	if (BoostCooldownRemaining > 0.f)
	{
		return false;
	}

	// Contract §5: ground only. This is the whole reason boost is not a second jump.
	if (MovementMode == MOVE_None || !IsMovingOnGround())
	{
		return false;
	}

	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		if (!TraceCharacter->IsAlive())
		{
			return false;
		}
	}

	return true;
}

float UTraceCharacterMovementComponent::GetBoostCooldownRemaining() const
{
	return FMath::Max(0.f, BoostCooldownRemaining);
}

float UTraceCharacterMovementComponent::GetBoostCooldown() const
{
	return FMath::Max(0.f, UTraceSettings::Get().BoostCooldown);
}

void UTraceCharacterMovementComponent::BeginBoost()
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	BoostCooldownRemaining = GetBoostCooldown();

	Velocity.Z = FMath::Max(1.f, Settings.BoostZVelocity);

	// MOVE_Walking discards vertical velocity outright — PhysWalking projects movement onto the
	// floor — so the mode change is not decoration, it is the only thing that makes the launch
	// happen at all. This mirrors ACharacter::DoJump, which does exactly the same two lines.
	// Called from OnMovementUpdated (inside the scoped move, after the physics step), so the launch
	// lands on the NEXT frame — one frame, identically on both ends of the wire, like the dash.
	SetMovementMode(MOVE_Falling);
}

// --- Simulation --------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::OnMovementUpdated(float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity)
{
	Super::OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);

	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// 1. Advance every clock first, so an ability that expires this frame stops driving velocity
	//    this frame and a cooldown that expires this frame permits an activation this frame.
	const bool bWasDashing = (DashTimeRemaining > 0.f);
	if (bWasDashing)
	{
		DashTimeRemaining = FMath::Max(0.f, DashTimeRemaining - DeltaSeconds);
	}

	const bool bWasSliding = (SlideTimeRemaining > 0.f);
	if (bWasSliding)
	{
		SlideTimeRemaining = FMath::Max(0.f, SlideTimeRemaining - DeltaSeconds);
	}
	if (SlideCooldownRemaining > 0.f)
	{
		SlideCooldownRemaining = FMath::Max(0.f, SlideCooldownRemaining - DeltaSeconds);
	}
	if (BoostCooldownRemaining > 0.f)
	{
		BoostCooldownRemaining = FMath::Max(0.f, BoostCooldownRemaining - DeltaSeconds);
	}
	if (SlideBufferRemaining > 0.f)
	{
		SlideBufferRemaining = FMath::Max(0.f, SlideBufferRemaining - DeltaSeconds);
	}

	// 1b. Resize the charge pool from the TRANSITION in GetMaxDashCharges(), never from its value.
	//     Picking the Core up must hand the extra charge over immediately — a carrier who has to
	//     wait out a cooldown before their bonus dash exists does not have a bonus dash during the
	//     four seconds that decide the run. Losing the Core takes exactly one back, and can never
	//     take back a charge that was not granted by carrying.
	{
		const int32 MaxCharges = GetMaxDashCharges();
		if (MaxCharges != LastMaxDashCharges)
		{
			if (MaxCharges > LastMaxDashCharges)
			{
				DashCharges += (MaxCharges - LastMaxDashCharges);
			}
			LastMaxDashCharges = MaxCharges;
		}
		DashCharges = FMath::Clamp(DashCharges, 0, MaxCharges);

		// 1c. Refill. One timer, restarted while the pool is still short, so charges come back
		//     sequentially rather than all at once.
		if (DashCharges >= MaxCharges)
		{
			DashRechargeRemaining = 0.f;
		}
		else if (DashRechargeRemaining > 0.f)
		{
			DashRechargeRemaining = FMath::Max(0.f, DashRechargeRemaining - DeltaSeconds);
			if (DashRechargeRemaining <= 0.f)
			{
				DashCharges = FMath::Min(DashCharges + 1, MaxCharges);
				if (DashCharges < MaxCharges)
				{
					DashRechargeRemaining = GetDashRechargeWindow();
				}
			}
		}
		else
		{
			// Short a charge with no clock running (e.g. the pool shrank and grew again). Start one
			// rather than stranding the player a charge down forever.
			DashRechargeRemaining = GetDashRechargeWindow();
		}
	}

	// 1d. The frame a dash or a slide ends, hand the player back at walking speed. See
	//     ClampPlanarSpeedToMax() for why this is a discrete step rather than a velocity cap.
	if (bWasDashing && DashTimeRemaining <= 0.f)
	{
		ClampPlanarSpeedToMax();
	}
	if (bWasSliding && SlideTimeRemaining <= 0.f)
	{
		SlideSpeed = 0.f;
		ClampPlanarSpeedToMax();
	}

	// 2. Activations, in priority order. Dash first: it is the mechanic the whole game is built
	//    around and it must never be eaten by another ability in the same frame.
	const bool bCrouchHeld = IsCrouchHeld();
	const bool bSlidePressedThisMove = bCrouchHeld && (bSlideHeldLastMove == 0);

	if (bWantsToDash && CanDash())
	{
		BeginDash();
	}
	else if (DashTimeRemaining > 0.f)
	{
		// Re-assert the locked velocity every frame. CalcVelocity applies friction and braking
		// against whatever the player is (or is not) holding, and without this the dash would
		// decay toward the walk speed and, worse, decay by a different amount on a replayed frame.
		//
		// Planar only, always: the dash never writes a positive Z (contract §5).
		const FVector Planar = DashDirection * GetDashSpeed();
		Velocity.X = Planar.X;
		Velocity.Y = Planar.Y;
		if (IsMovingOnGround())
		{
			Velocity.Z = 0.f;
		}
	}

	// 3. Boost. Ground-only, so it cannot be chained in the air, and it deliberately runs after the
	//    dash: a dash+boost in one frame is a diagonal launch, which is fine, but the dash's planar
	//    velocity must be established first or the boost's mode change would be applied to stale
	//    horizontal speed.
	if (bWantsToBoost && CanBoost())
	{
		BeginBoost();

		// Boost leaves the ground, and a slide is a ground state.
		if (SlideTimeRemaining > 0.f)
		{
			SlideTimeRemaining = 0.f;
			SlideSpeed = 0.f;
		}
	}

	// 4. Crouch: slide on the ground, fast-fall in the air. One key, resolved by where the pawn is.
	//
	//    A press that cannot be honoured yet (mid-dash, or airborne) is buffered rather than thrown
	//    away — see SlideBufferRemaining. The buffer is charged from the press EDGE only, so holding
	//    the key can never chain slides.
	if (bSlidePressedThisMove)
	{
		SlideBufferRemaining = FMath::Max(0.f, Settings.SlideInputBufferSeconds);
	}

	if (SlideTimeRemaining > 0.f)
	{
		// --- Maintain an active slide -----------------------------------------------------------
		if (!IsMovingOnGround() || !bCrouchHeld)
		{
			// Released early, or slid off a ledge. Either way the slide is over and the speed
			// bonus goes with it — otherwise "slide off every ledge" would be free travel.
			EndSlide();
		}
		else
		{
			// Weak steering. A slide you cannot aim at all is unusable in a corridor; a slide you
			// can steer freely is just fast walking. SlideTurnRateDegrees is the dial.
			FVector Desired = Acceleration;
			Desired.Z = 0.f;
			if (Desired.Normalize())
			{
				SlideDirection = TraceMovement::SteerTowards(
					SlideDirection, Desired, FMath::Max(0.f, Settings.SlideTurnRateDegrees) * DeltaSeconds);
			}

			SlideSpeed = FMath::Max(0.f, SlideSpeed - FMath::Max(0.f, Settings.SlideDeceleration) * DeltaSeconds);

			const float ExitSpeed = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideExitSpeedFraction);
			if (SlideSpeed <= ExitSpeed)
			{
				// Decayed back to walking pace: stop rather than drag the player along at a speed
				// the normal movement code would have given them anyway.
				EndSlide();
			}
			else
			{
				Velocity.X = SlideDirection.X * SlideSpeed;
				Velocity.Y = SlideDirection.Y * SlideSpeed;
				if (IsMovingOnGround())
				{
					Velocity.Z = 0.f;
				}
			}
		}
	}
	else if (bSlidePressedThisMove && !IsMovingOnGround() && MovementMode != MOVE_None)
	{
		// --- FAST-FALL (contract §5) -------------------------------------------------------------
		// Zero POSITIVE Z only, leave horizontal speed alone. This is a fall you chose, not a stop:
		// cutting a jump short to drop behind cover or to beat a shot is the whole point, so the
		// horizontal carry must survive. Runs on the press edge, not continuously, so holding crouch
		// does not pin the pawn under every subsequent jump or boost.
		if (Velocity.Z > 0.f)
		{
			Velocity.Z = 0.f;
		}

		// One press, one meaning. Without this the same press would also slide the instant the pawn
		// landed, which is a surprise rather than a combo.
		SlideBufferRemaining = 0.f;
	}
	else if (SlideBufferRemaining > 0.f && CanStartSlide())
	{
		// CanStartSlide() already refuses while dashing, off the ground, on cooldown, below the
		// entry speed, or with the key released.
		BeginSlide();
		SlideBufferRemaining = 0.f;
	}

	// 5. Latch the last instant this pawn was inside its dash window, on the authority only.
	//    The trail's trip test ticks once per SERVER frame, but the server advances a remote
	//    client's dash clock here inside MoveAutonomous - possibly several client moves deep in a
	//    single server frame. Without this latch, a dash that starts and finishes between two trail
	//    ticks credits its displacement to the sweep while IsDashing() already reads false, and the
	//    single most important mechanic in the game silently no-ops under jitter.
	if (DashTimeRemaining > 0.f && CharacterOwner->HasAuthority())
	{
		if (const UWorld* DashWorld = GetWorld())
		{
			LastDashActiveWorldTime = static_cast<float>(DashWorld->GetTimeSeconds());
		}
	}

	// 6. Consume the one-shot intents and remember the held one for the next move's edge test. On
	//    the server the intents are re-supplied by the next ServerMove's flags, on the client by the
	//    next StartDash()/StartBoost(), and during replay by UpdateFromCompressedFlags — so one key
	//    press can only ever produce one dash and one boost.
	bWantsToDash = 0;
	bWantsToBoost = 0;
	bSlideHeldLastMove = bCrouchHeld ? 1 : 0;

#if !UE_BUILD_SHIPPING
	TickSelfTest(DeltaSeconds);
#endif
}

float UTraceCharacterMovementComponent::GetMaxSpeed() const
{
	if (IsDashing())
	{
		return GetDashSpeed();
	}

	float Speed = Super::GetMaxSpeed();

	// The Core carrier is slightly faster (UTraceSettings::CarrierSpeedMultiplier). bIsCarrier is
	// replicated, so the client applies this a fraction of a second after the server does; the
	// resulting sub-frame divergence is exactly what the correction path exists to absorb.
	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		if (TraceCharacter->IsCarrier())
		{
			Speed *= FMath::Max(0.01f, UTraceSettings::Get().CarrierSpeedMultiplier);
		}
	}

	// A slide is faster than a walk, and CalcVelocity clamps to this value during the physics step
	// that runs BEFORE OnMovementUpdated re-asserts the slide velocity. Leaving it at the walk speed
	// would mean the frame's actual displacement was computed at walking pace and only the reported
	// velocity looked like a slide.
	if (IsSliding())
	{
		Speed = FMath::Max(Speed, SlideSpeed);
	}

	return Speed;
}

// -------------------------------------------------------------------------------------------
// Dev-only scripted self-test — see the header. "-TraceMoveKitTest".
// -------------------------------------------------------------------------------------------

#if !UE_BUILD_SHIPPING

int32 GTraceMoveKitFakeCarrier = 0;
static FAutoConsoleVariableRef CVarTraceMoveKitFakeCarrier(
	TEXT("Trace.MoveKitFakeCarrier"),
	GTraceMoveKitFakeCarrier,
	TEXT("Dev only. Non-zero pretends this pawn is carrying the Core for the purposes of the dash "
	     "charge pool, so the carrier's extra charge can be exercised without a Core."),
	ECVF_Cheat);

void UTraceCharacterMovementComponent::TickSelfTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceMoveKitTest"));
	if (!bEnabled || CharacterOwner == nullptr)
	{
		return;
	}

	// Standalone only: the schedule below advances on the simulation's own delta, and a replayed
	// move would advance it twice.
	if (GetNetMode() != NM_Standalone || !CharacterOwner->IsLocallyControlled()
		|| Cast<APlayerController>(CharacterOwner->GetController()) == nullptr)
	{
		return;
	}

	if (SelfTestTime < 0.f)
	{
		// Let the pawn spawn, settle onto the floor and finish its first replication.
		if (GetWorld() == nullptr || GetWorld()->GetTimeSeconds() < 3.f)
		{
			return;
		}
		SelfTestTime = 0.f;
		UE_LOG(LogTraceGame, Display, TEXT("MOVEKIT ---- self-test begin, WalkSpeed=%.0f DashCooldown=%.1f MaxCharges=%d"),
			MaxWalkSpeed, GetDashCooldown(), GetMaxDashCharges());
	}

	const float T = SelfTestTime;
	SelfTestTime += DeltaSeconds;

	// Hold "forward" for the whole test so there is momentum to dash and slide with.
	if (T < 12.f)
	{
		CharacterOwner->AddMovementInput(CharacterOwner->GetActorForwardVector(), 1.f);
	}

	auto Report = [this, T](const TCHAR* Tag)
	{
		const FVector Planar(Velocity.X, Velocity.Y, 0.f);
		UE_LOG(LogTraceGame, Display,
			TEXT("MOVEKIT t=%5.2f %-14s mode=%d planar=%7.1f velZ=%8.1f z=%8.1f charges=%d/%d recharge=%4.2f dash=%4.2f slide=%4.2f(%6.1f) boostCd=%5.2f"),
			T, Tag, static_cast<int32>(MovementMode.GetValue()), Planar.Size(), Velocity.Z,
			UpdatedComponent != nullptr ? UpdatedComponent->GetComponentLocation().Z : 0.f,
			DashCharges, GetMaxDashCharges(), DashRechargeRemaining,
			DashTimeRemaining, SlideTimeRemaining, SlideSpeed, BoostCooldownRemaining);
	};

	// A step fires once, the first move at or after its scheduled time.
	auto Step = [this, T](int32 Index, float At) -> bool
	{
		if (SelfTestStep == Index && T >= At)
		{
			SelfTestStep = Index + 1;
			return true;
		}
		return false;
	};

	// --- the schedule ---------------------------------------------------------------------------
	if (Step(0, 1.5f))  { Report(TEXT("RUN"));            }
	if (Step(1, 2.0f))  { StartDash();          Report(TEXT("DASH-req")); }
	if (Step(2, 2.1f))  { Report(TEXT("DASH-mid"));       }
	if (Step(3, 2.4f))  { Report(TEXT("DASH-end"));       }
	if (Step(4, 3.5f))  { SetWantsToSlide(true);  Report(TEXT("SLIDE-req")); }
	if (Step(5, 3.7f))  { Report(TEXT("SLIDE-mid"));      }
	if (Step(6, 4.2f))  { Report(TEXT("SLIDE-late"));     }
	if (Step(7, 4.8f))  { SetWantsToSlide(false); Report(TEXT("SLIDE-rel")); }
	if (Step(8, 5.2f))  { StartBoost();           Report(TEXT("BOOST-req")); }
	if (Step(9, 5.4f))  { Report(TEXT("BOOST-rise"));     }
	if (Step(10, 5.6f)) { SetWantsToSlide(true);  Report(TEXT("FASTFALL-req")); }
	if (Step(11, 5.7f)) { SetWantsToSlide(false); Report(TEXT("FASTFALL-after")); }
	if (Step(12, 7.0f)) { Report(TEXT("LANDED"));         }
	// The carrier's second charge: grant on the transition, two dashes inside one cooldown, then
	// take it back and prove the pool clamps.
	if (Step(13, 7.5f)) { GTraceMoveKitFakeCarrier = 1;  Report(TEXT("CARRIER-on")); }
	if (Step(14, 7.7f)) { Report(TEXT("CARRIER-granted")); }
	if (Step(15, 8.0f)) { StartDash();            Report(TEXT("CDASH-1")); }
	if (Step(16, 8.6f)) { StartDash();            Report(TEXT("CDASH-2")); }
	if (Step(17, 8.9f)) { StartDash();            Report(TEXT("CDASH-3-should-fail")); }
	if (Step(18, 9.5f)) { GTraceMoveKitFakeCarrier = 0;  Report(TEXT("CARRIER-off")); }
	if (Step(19, 9.7f)) { Report(TEXT("CARRIER-clamped")); }
	if (Step(20, 13.0f)) { Report(TEXT("REFILLED"));      }
	if (Step(21, 13.2f)) { UE_LOG(LogTraceGame, Display, TEXT("MOVEKIT ---- self-test end")); }
}

#endif // !UE_BUILD_SHIPPING

// -------------------------------------------------------------------------------------------
// FSavedMove_Trace
// -------------------------------------------------------------------------------------------

FSavedMove_Trace::FSavedMove_Trace()
	: bSavedWantsToDash(0)
	, bSavedWantsToBoost(0)
	, bSavedWantsToSlide(0)
	, SavedDashTimeRemaining(0.f)
	, SavedDashRechargeRemaining(0.f)
	, SavedDashCharges(0)
	, SavedLastMaxDashCharges(0)
	, SavedDashDirection(FVector::ZeroVector)
	, SavedSlideTimeRemaining(0.f)
	, SavedSlideCooldownRemaining(0.f)
	, SavedSlideSpeed(0.f)
	, SavedSlideBufferRemaining(0.f)
	, SavedSlideDirection(FVector::ZeroVector)
	, bSavedSlideHeldLastMove(0)
	, SavedBoostCooldownRemaining(0.f)
{
}

void FSavedMove_Trace::Clear()
{
	Super::Clear();

	// Saved moves are pooled and recycled — every added field must be reset or a stale ability will
	// resurrect itself several moves later.
	bSavedWantsToDash = 0;
	bSavedWantsToBoost = 0;
	bSavedWantsToSlide = 0;

	SavedDashTimeRemaining = 0.f;
	SavedDashRechargeRemaining = 0.f;
	SavedDashCharges = 0;
	SavedLastMaxDashCharges = 0;
	SavedDashDirection = FVector::ZeroVector;

	SavedSlideTimeRemaining = 0.f;
	SavedSlideCooldownRemaining = 0.f;
	SavedSlideSpeed = 0.f;
	SavedSlideBufferRemaining = 0.f;
	SavedSlideDirection = FVector::ZeroVector;
	bSavedSlideHeldLastMove = 0;

	SavedBoostCooldownRemaining = 0.f;
}

uint8 FSavedMove_Trace::GetCompressedFlags() const
{
	uint8 Result = Super::GetCompressedFlags();

	if (bSavedWantsToDash)
	{
		Result |= FLAG_Custom_0;
	}
	if (bSavedWantsToBoost)
	{
		Result |= FLAG_Custom_1;
	}
	if (bSavedWantsToSlide)
	{
		Result |= FLAG_Custom_2;
	}

	return Result;
}

bool FSavedMove_Trace::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const
{
	// NewMove is a TSharedPtr; the widely-copied `(FMySavedMove*)&NewMove` idiom casts the address
	// of the *pointer* and reads garbage. Go through Get().
	const FSavedMove_Trace* Other = static_cast<const FSavedMove_Trace*>(NewMove.Get());
	if (Other == nullptr)
	{
		return false;
	}

	// Different intent means different simulation — merging them would drop or duplicate an
	// activation on the server. The slide flag is included even though it is a level rather than an
	// edge, because the edge is DERIVED from it: merging a held frame with a released one would
	// erase a press the fast-fall depends on.
	if (bSavedWantsToDash != Other->bSavedWantsToDash
		|| bSavedWantsToBoost != Other->bSavedWantsToBoost
		|| bSavedWantsToSlide != Other->bSavedWantsToSlide)
	{
		return false;
	}

	// Never merge across an active dash or slide. Combining replays one longer move from the older
	// move's start state; the clocks are linear in dt so the maths would survive, but the frame on
	// which the ability *ends* would move, and with it the velocity profile.
	if (SavedDashTimeRemaining > 0.f || Other->SavedDashTimeRemaining > 0.f
		|| SavedSlideTimeRemaining > 0.f || Other->SavedSlideTimeRemaining > 0.f)
	{
		return false;
	}

	return Super::CanCombineWith(NewMove, InCharacter, MaxDelta);
}

void FSavedMove_Trace::SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData)
{
	Super::SetMoveFor(C, InDeltaTime, NewAccel, ClientData);

	// Runs before PerformMovement, so this captures the state at the *start* of the move — which
	// is precisely what PrepMoveFor must restore before a replay.
	if (C != nullptr)
	{
		if (const UTraceCharacterMovementComponent* Movement = Cast<UTraceCharacterMovementComponent>(C->GetCharacterMovement()))
		{
			bSavedWantsToDash  = Movement->bWantsToDash;
			bSavedWantsToBoost = Movement->bWantsToBoost;
			bSavedWantsToSlide = Movement->bWantsToSlide;

			SavedDashTimeRemaining     = Movement->DashTimeRemaining;
			SavedDashRechargeRemaining = Movement->DashRechargeRemaining;
			SavedDashCharges           = Movement->DashCharges;
			SavedLastMaxDashCharges    = Movement->LastMaxDashCharges;
			SavedDashDirection         = Movement->DashDirection;

			SavedSlideTimeRemaining     = Movement->SlideTimeRemaining;
			SavedSlideCooldownRemaining = Movement->SlideCooldownRemaining;
			SavedSlideSpeed             = Movement->SlideSpeed;
			SavedSlideBufferRemaining   = Movement->SlideBufferRemaining;
			SavedSlideDirection         = Movement->SlideDirection;
			bSavedSlideHeldLastMove     = Movement->bSlideHeldLastMove;

			SavedBoostCooldownRemaining = Movement->BoostCooldownRemaining;
		}
	}
}

void FSavedMove_Trace::PrepMoveFor(ACharacter* C)
{
	Super::PrepMoveFor(C);

	if (C != nullptr)
	{
		if (UTraceCharacterMovementComponent* Movement = Cast<UTraceCharacterMovementComponent>(C->GetCharacterMovement()))
		{
			// Rewind every ability to exactly where it stood before this move ran. MoveAutonomous
			// will overwrite the three intent flags from the compressed flags immediately after this
			// returns; restoring them too costs nothing and keeps the snapshot complete.
			Movement->bWantsToDash  = bSavedWantsToDash;
			Movement->bWantsToBoost = bSavedWantsToBoost;
			Movement->bWantsToSlide = bSavedWantsToSlide;

			Movement->DashTimeRemaining     = SavedDashTimeRemaining;
			Movement->DashRechargeRemaining = SavedDashRechargeRemaining;
			Movement->DashCharges           = SavedDashCharges;
			Movement->LastMaxDashCharges    = SavedLastMaxDashCharges;
			Movement->DashDirection         = SavedDashDirection;

			Movement->SlideTimeRemaining     = SavedSlideTimeRemaining;
			Movement->SlideCooldownRemaining = SavedSlideCooldownRemaining;
			Movement->SlideSpeed             = SavedSlideSpeed;
			Movement->SlideBufferRemaining   = SavedSlideBufferRemaining;
			Movement->SlideDirection         = SavedSlideDirection;
			Movement->bSlideHeldLastMove     = bSavedSlideHeldLastMove;

			Movement->BoostCooldownRemaining = SavedBoostCooldownRemaining;
		}
	}
}

// -------------------------------------------------------------------------------------------
// FNetworkPredictionData_Client_Trace
// -------------------------------------------------------------------------------------------

FNetworkPredictionData_Client_Trace::FNetworkPredictionData_Client_Trace(const UCharacterMovementComponent& ClientMovement)
	: Super(ClientMovement)
{
}

FSavedMovePtr FNetworkPredictionData_Client_Trace::AllocateNewMove()
{
	return FSavedMovePtr(new FSavedMove_Trace());
}
