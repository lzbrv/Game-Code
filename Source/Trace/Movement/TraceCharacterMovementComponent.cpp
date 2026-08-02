// Trace — character movement with a client-predicted dash. See the header for the full model.

#include "Movement/TraceCharacterMovementComponent.h"

#include "Components/SceneComponent.h"
#include "Core/TraceCharacter.h"
#include "Engine/World.h"                      // UWorld::GetTimeSeconds (dash-active latch)
#include "GameFramework/Character.h"
#include "Math/UnrealMathUtility.h"
#include "TraceSettings.h"

// -------------------------------------------------------------------------------------------
// UTraceCharacterMovementComponent
// -------------------------------------------------------------------------------------------

UTraceCharacterMovementComponent::UTraceCharacterMovementComponent()
{
	bWantsToDash = 0;
	DashTimeRemaining = 0.f;
	DashCooldownRemaining = 0.f;
	DashDirection = FVector::ZeroVector;

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

	// Pure state restore. The actual dash activation happens in OnMovementUpdated, on every
	// machine, from this flag — so server, client and replay all go through one code path.
	bWantsToDash = ((Flags & FSavedMove_Character::FLAG_Custom_0) != 0) ? 1 : 0;
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

	// Covers both "already dashing" and "cooling down" — the cooldown starts at dash activation
	// and spans the dash window (UTraceSettings::DashCooldown is measured from dash start).
	if (DashCooldownRemaining > 0.f || DashTimeRemaining > 0.f)
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
	return FMath::Max(0.f, DashCooldownRemaining);
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
	DashCooldownRemaining = DashTimeRemaining + GetDashCooldown();

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

void UTraceCharacterMovementComponent::OnMovementUpdated(float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity)
{
	Super::OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);

	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// 1. Advance the clocks first, so a dash that expires this frame stops driving velocity this
	//    frame and a cooldown that expires this frame permits a dash this frame.
	const bool bWasDashing = (DashTimeRemaining > 0.f);
	if (bWasDashing)
	{
		DashTimeRemaining = FMath::Max(0.f, DashTimeRemaining - DeltaSeconds);
	}
	if (DashCooldownRemaining > 0.f)
	{
		DashCooldownRemaining = FMath::Max(0.f, DashCooldownRemaining - DeltaSeconds);
	}

	// 1b. The frame the dash ends, hand the player back at walking speed.
	//
	// Without this, CalcVelocity only bleeds the excess off through braking friction, which at
	// DashSpeed takes the better part of a second — so the dash would keep moving the player long
	// after IsDashing() (and therefore the trail rule) says it is over. Doing it here rather than
	// through a velocity cap is what keeps it predictable: it happens on exactly the frame the saved
	// dash timer crosses zero, and that frame replays identically on the client and the server.
	if (bWasDashing && DashTimeRemaining <= 0.f)
	{
		const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
		const float PlanarLimit = GetMaxSpeed();   // no longer dashing, so this is the walk speed
		if (PlanarVelocity.SizeSquared() > FMath::Square(PlanarLimit))
		{
			const FVector Clamped = PlanarVelocity.GetSafeNormal() * PlanarLimit;
			Velocity.X = Clamped.X;
			Velocity.Y = Clamped.Y;
		}
	}

	// 2. Activate, or keep an active dash on rails.
	if (bWantsToDash && CanDash())
	{
		BeginDash();
	}
	else if (DashTimeRemaining > 0.f)
	{
		// Re-assert the locked velocity every frame. CalcVelocity applies friction and braking
		// against whatever the player is (or is not) holding, and without this the dash would
		// decay toward the walk speed and, worse, decay by a different amount on a replayed frame.
		const FVector Planar = DashDirection * GetDashSpeed();
		Velocity.X = Planar.X;
		Velocity.Y = Planar.Y;
		if (IsMovingOnGround())
		{
			Velocity.Z = 0.f;
		}
	}

	// 2b. Latch the last instant this pawn was inside its dash window, on the authority only.
	//     The trail's trip test ticks once per SERVER frame, but the server advances a remote
	//     client's dash clock here inside MoveAutonomous - possibly several client moves deep in a
	//     single server frame. Without this latch, a dash that starts and finishes between two trail
	//     ticks credits its displacement to the sweep while IsDashing() already reads false, and the
	//     single most important mechanic in the game silently no-ops under jitter.
	if (DashTimeRemaining > 0.f && CharacterOwner->HasAuthority())
	{
		if (const UWorld* DashWorld = GetWorld())
		{
			LastDashActiveWorldTime = static_cast<float>(DashWorld->GetTimeSeconds());
		}
	}

	// 3. Consume the intent. On the server it is re-supplied by the next ServerMove's flags, on
	//    the client by the next StartDash(), and during replay by UpdateFromCompressedFlags — so
	//    one key press can only ever produce one dash.
	bWantsToDash = 0;
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

	return Speed;
}

// -------------------------------------------------------------------------------------------
// FSavedMove_Trace
// -------------------------------------------------------------------------------------------

FSavedMove_Trace::FSavedMove_Trace()
	: bSavedWantsToDash(0)
	, SavedDashTimeRemaining(0.f)
	, SavedDashCooldownRemaining(0.f)
	, SavedDashDirection(FVector::ZeroVector)
{
}

void FSavedMove_Trace::Clear()
{
	Super::Clear();

	// Saved moves are pooled and recycled — every added field must be reset or a stale dash will
	// resurrect itself several moves later.
	bSavedWantsToDash = 0;
	SavedDashTimeRemaining = 0.f;
	SavedDashCooldownRemaining = 0.f;
	SavedDashDirection = FVector::ZeroVector;
}

uint8 FSavedMove_Trace::GetCompressedFlags() const
{
	uint8 Result = Super::GetCompressedFlags();

	if (bSavedWantsToDash)
	{
		Result |= FLAG_Custom_0;
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

	// Different dash intent means different simulation — merging them would drop or duplicate a
	// dash on the server.
	if (bSavedWantsToDash != Other->bSavedWantsToDash)
	{
		return false;
	}

	// Never merge across an active dash. Combining replays one longer move from the older move's
	// start state; the dash clocks are linear in dt so the maths would survive, but the frame on
	// which the dash *ends* would move, and with it the velocity profile.
	if (SavedDashTimeRemaining > 0.f || Other->SavedDashTimeRemaining > 0.f)
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
			bSavedWantsToDash = Movement->bWantsToDash;
			SavedDashTimeRemaining = Movement->DashTimeRemaining;
			SavedDashCooldownRemaining = Movement->DashCooldownRemaining;
			SavedDashDirection = Movement->DashDirection;
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
			// Rewind the dash to exactly where it stood before this move ran. MoveAutonomous will
			// overwrite bWantsToDash from the compressed flags immediately after this returns;
			// restoring it too costs nothing and keeps the snapshot complete.
			Movement->bWantsToDash = bSavedWantsToDash;
			Movement->DashTimeRemaining = SavedDashTimeRemaining;
			Movement->DashCooldownRemaining = SavedDashCooldownRemaining;
			Movement->DashDirection = SavedDashDirection;
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
