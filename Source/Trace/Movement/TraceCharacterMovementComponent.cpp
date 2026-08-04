// Trace — the client-predicted movement kit: dash (charged), slide, air fast-fall, and the
// Source/Apex momentum model (air acceleration, landing carry, momentum-preserving transitions).
// See the header for the full prediction model and the design rationale.
//
// BOOST HAS BEEN DELETED (spec v3 §1). Not disabled, not defaulted to zero — removed. If you are
// reading this because a merge resurrected `bWantsToBoost`, `BoostCooldownRemaining`, `BeginBoost`
// or FLAG_Custom_1, delete it again.
//
// THE SLIDE'S FLAT MOMENTUM BOOST HAS BEEN DELETED TOO (spec v4 §1). Same rule: `SlideImpulse`,
// `GetSlideImpulse()`, `SlideExitMinSpeedFraction` and `GetSlideExitMinSpeedFraction()` are gone,
// along with the ExitFloor term in EndSlide(). The design owner ruled the boost out explicitly. If a
// merge brings any of them back, delete them again — and read the slide-jump section of the header,
// which is what the slide is supposed to be worth now.

#include "Movement/TraceCharacterMovementComponent.h"

#include "Components/SceneComponent.h"
#include "Core/TraceCharacter.h"
#include "Engine/World.h"                      // UWorld::GetTimeSeconds (dash-active latch)
#include "GameFramework/Character.h"
#include "Math/UnrealMathUtility.h"
#include "Trace.h"                             // LogTraceGame
#include "TraceSettings.h"

#if !UE_BUILD_SHIPPING
#include "GameFramework/PlayerController.h"    // measurement harness: player-controlled check
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

// Every number this file runs on is a live UTraceSettings property, read at the point of use. There
// is deliberately NO detection shim: the spec v3 §2 knobs (Movement|Air, Movement|Landing, the slide
// entry/impulse/cooldown block, DashExitSpeedMultiplier) are real properties now, and a shim would
// only serve to swallow a rename and silently substitute a hardcoded default for the designer's
// value instead of failing the build.

namespace TraceMoveCfg
{
	/** Below this the pawn is treated as having no horizontal motion at all. */
	constexpr float SpeedEpsilon = 1.f;
}

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

#if !UE_BUILD_SHIPPING
/**
 * Slide instrumentation. Off by default; "-TraceSlideDebug" on the command line or
 * `Trace.SlideDebug 1` in the console turns it on for a measurement run.
 *
 * At Display, not Verbose. A log line nobody can see has twice now been read as a dead mechanic.
 */
int32 GTraceSlideDebug = 0;
static FAutoConsoleVariableRef CVarTraceSlideDebug(
	TEXT("Trace.SlideDebug"),
	GTraceSlideDebug,
	TEXT("Dev only. Non-zero logs every slide's duration, distance and entry/exit speed at Display, "
	     "with a running mean, so a headless match can measure the slide instead of describing it."),
	ECVF_Cheat);

static bool IsSlideDebugEnabled()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceSlideDebug"));
	return bFromCommandLine || GTraceSlideDebug != 0;
}

/**
 * Match-wide slide sample, not per-pawn: ten bots sliding two hundred times between them is one
 * sample of the mechanic, and a mean per pawn would be ten small samples of nothing in particular.
 * Game thread only, and dev-only, so plain file statics are the right amount of machinery.
 */
static int32 GTraceSlideDebugCount = 0;
static float GTraceSlideDebugTotalDuration = 0.f;
static float GTraceSlideDebugTotalDistance = 0.f;
#endif

// -------------------------------------------------------------------------------------------
// UTraceCharacterMovementComponent
// -------------------------------------------------------------------------------------------

UTraceCharacterMovementComponent::UTraceCharacterMovementComponent()
{
	bWantsToDash = 0;
	bWantsToSlide = 0;

	DashTimeRemaining = 0.f;
	DashCharges = 1;
	DashRechargeRemaining = 0.f;
	LastMaxDashCharges = 1;
	DashDirection = FVector::ZeroVector;

	SlideTimeRemaining = 0.f;
	SlideCooldownRemaining = 0.f;
	SlideCommitRemaining = 0.f;
	SlideSpeed = 0.f;
	SlideDirection = FVector::ZeroVector;
	SlideBufferRemaining = 0.f;
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;
	bSlideHeldLastMove = 0;
	bWasAirborneLastMove = 0;

	// Third-person feel: the capsule turns toward where it is moving. Aim is separate and comes
	// from the control rotation (ATraceCharacter::GetAimDirection), which is why the character
	// leaves bUseControllerRotationYaw off.
	bOrientRotationToMovement = true;
	bUseControllerDesiredRotation = false;
	RotationRate = FRotator(0.f, 900.f, 0.f);

	// Arena-shooter tuning. MaxWalkSpeed is overwritten from UTraceSettings in BeginPlay and re-pushed
	// every movement update by RefreshEngineTunablesFromSettings(), so this literal only covers the
	// window before play starts (and the CDO in the editor). Keep it equal to UTraceSettings::WalkSpeed
	// anyway — a stale value here is what an editor viewport shows before anyone presses Play.
	MaxWalkSpeed = 800.f;   // spec v4 §5: 820 -> 800. Equal to UTraceSettings::WalkSpeed by rule.
	MaxAcceleration = 4096.f;
	BrakingDecelerationWalking = 2600.f;
	GroundFriction = 8.f;
	JumpZVelocity = 640.f;
	bCanWalkOffLedges = true;

	// --- The air, spec §2.1 ---------------------------------------------------------------------
	//
	// AirControl 1.0 is not "more air control", it is "get out of the way". GetFallingLateralAcceleration
	// scales Acceleration by AirControl before CalcVelocity sees it, and our Source model has to
	// receive the raw wish vector so it can cap in SPEED (AirMaxWishSpeed) rather than in
	// ACCELERATION. Capping the acceleration is exactly what makes perpendicular input brake.
	//
	// Zero lateral friction and zero falling braking: Source has no air friction, and neither do we.
	// Letting go of the stick mid-flight must coast, not decay.
	//
	// These two are re-pushed from UTraceSettings every move by RefreshEngineTunablesFromSettings();
	// the literals only cover the CDO and the window before play starts. Keep them equal to
	// UTraceSettings::AirControl / AirFriction.
	AirControl = 1.f;
	FallingLateralFriction = 0.f;
	BrakingDecelerationFalling = 0.f;

	// Neutralise the engine's low-speed air-control boost. It exists to make the stock lerp usable
	// when you are barely moving; under the projection model it would double the wish vector at low
	// speed, i.e. change the model based on how fast you happen to be going.
	AirControlBoostMultiplier = 1.f;
	AirControlBoostVelocityThreshold = 0.f;

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

#if !UE_BUILD_SHIPPING
	// One line, once per process, naming every number the kit will actually run on. A measurement
	// run that does not print the configuration it measured is an anecdote — and this is also the
	// cheapest possible check that a "-ini:Game:..." override on the command line really landed.
	{
		static bool bLoggedKitConfig = false;
		if (!bLoggedKitConfig)
		{
			bLoggedKitConfig = true;
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG walk=%.0f | AIR srcAccel=%d accel=%.0f wishCap=%.0f maxAir=%.0f "
				     "| LAND preserve=%d overspeedFric=%.2f overspeedBrake=%.0f turn=%.1f dashExit=%.2fx "
				     "| SLIDE dur=%.2f entryMul=%.2f cooldownFromEnd=%.2f max=%.0f decel=%.0f "
				     "commit=%.2f exitRet=%.2f exitCeil=%.2f (NO impulse, NO exit floor - spec v4 s1) "
				     "| SLIDEJUMP on=%d retain=%.2f zMul=%.2f window=%.2f windowBonus=%.2f "
				     "| DASH speed=%.0f dur=%.2f cd=%.2f"),
				MaxWalkSpeed,
				IsSourceAirAccelerationEnabled() ? 1 : 0, GetAirAcceleration(), GetAirMaxWishSpeed(), GetMaxAirSpeed(),
				IsLandingMomentumPreserved() ? 1 : 0, GetGroundOverspeedFriction(), GetGroundOverspeedBraking(),
				GetGroundOverspeedTurnRate(), GetDashExitSpeedMultiplier(),
				GetSlideDuration(), GetSlideEntrySpeedMultiplier(), GetSlideCooldownSeconds(),
				Settings.SlideMaxSpeed, GetSlideDeceleration(), GetSlideMinCommitSeconds(),
				GetSlideExitSpeedRetention(), GetSlideExitMaxSpeedMultiplier(),
				IsSlideJumpEnabled() ? 1 : 0, GetSlideJumpHorizontalRetention(), GetSlideJumpZMultiplier(),
				GetSlideJumpWindowSeconds(), GetSlideJumpWindowSpeedBonus(),
				GetDashSpeed(), GetDashDuration(), GetDashCooldown());
		}
	}
#endif
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

float UTraceCharacterMovementComponent::GetDashExitSpeedMultiplier() const
{
	// Never below 1: a dash that handed back LESS than a run would be a punishment for dashing.
	return FMath::Max(1.f, UTraceSettings::Get().DashExitSpeedMultiplier);
}

float UTraceCharacterMovementComponent::GetSlideDuration() const
{
	// Floored rather than defaulted: a zero-length slide would still spend the slide cooldown.
	return FMath::Max(0.05f, UTraceSettings::Get().SlideDuration);
}

float UTraceCharacterMovementComponent::GetSlideDeceleration() const
{
	// 0 is legal and means "a slide holds its entry speed for its whole duration".
	return FMath::Max(0.f, UTraceSettings::Get().SlideDeceleration);
}

float UTraceCharacterMovementComponent::GetSlideMinCommitSeconds() const
{
	// Clamped to the slide's own length: a commit window longer than the slide is meaningless, and
	// leaving it unclamped would let one bad number make the slide feel like it ignores the key.
	return FMath::Clamp(UTraceSettings::Get().SlideMinCommitSeconds, 0.f, GetSlideDuration());
}

float UTraceCharacterMovementComponent::GetSlideExitSpeedRetention() const
{
	return FMath::Max(0.f, UTraceSettings::Get().SlideExitSpeedRetention);
}

// GetSlideExitMinSpeedFraction() WAS HERE AND IS DELETED (spec v4 §1). It read the exit FLOOR, which
// handed a decayed slide back at exactly WalkSpeed however slowly it was actually going — measured as
// a 73% speed gain for a slow slide, i.e. the flat momentum boost the design owner ruled out.

float UTraceCharacterMovementComponent::GetSlideExitMaxSpeedMultiplier() const
{
	// Never below 1: a multiplier under 1 would make a slide exit SLOWER than a walk, which is the
	// exact behaviour this pass exists to remove. Note that EndSlide() additionally floors the
	// resulting ceiling at the slide's own speed when momentum preservation is on, so this knob only
	// ever decides how much of a *fast* slide is handed back, never whether one is braked.
	return FMath::Max(1.f, UTraceSettings::Get().SlideExitMaxSpeedMultiplier);
}

float UTraceCharacterMovementComponent::GetSlideCooldownSeconds() const
{
	return FMath::Max(0.f, UTraceSettings::Get().SlideCooldownSeconds);
}

float UTraceCharacterMovementComponent::GetSlideEntrySpeedMultiplier() const
{
	return FMath::Max(0.f, UTraceSettings::Get().SlideEntrySpeedMultiplier);
}

// GetSlideImpulse() WAS HERE AND IS DELETED (spec v4 §1). It read the FLAT additive on slide entry —
// worth the same whether you entered at a walk or out of a dash, which is exactly what "the flat
// momentum boost should be ruled out" means. SlideEntrySpeedMultiplier (1.0) is now the only thing
// between entry speed and slide speed.

bool UTraceCharacterMovementComponent::IsSlideJumpEnabled() const
{
	return UTraceSettings::Get().bSlideJumpEnabled;
}

float UTraceCharacterMovementComponent::GetSlideJumpHorizontalRetention() const
{
	// Not floored at 1: the user is allowed to make a slide-jump cost speed if that is what plays
	// well. Floored at 0 only so a negative value cannot reverse the pawn's direction of travel.
	return FMath::Max(0.f, UTraceSettings::Get().SlideJumpHorizontalRetention);
}

float UTraceCharacterMovementComponent::GetSlideJumpZMultiplier() const
{
	return FMath::Max(0.f, UTraceSettings::Get().SlideJumpZMultiplier);
}

float UTraceCharacterMovementComponent::GetSlideJumpWindowSeconds() const
{
	// Clamped to the slide's own length, like SlideMinCommitSeconds: a window longer than the slide
	// means every slide-jump is well timed, which is not a window, and leaving it unclamped lets one
	// bad number quietly turn the bonus into a permanent multiplier.
	return FMath::Clamp(UTraceSettings::Get().SlideJumpWindowSeconds, 0.f, GetSlideDuration());
}

float UTraceCharacterMovementComponent::GetSlideJumpWindowSpeedBonus() const
{
	// Never below 1: the window must never be able to PUNISH a well-timed hop. Missing the timing is
	// allowed to be worth less; hitting it must never be worth less than missing it.
	return FMath::Max(1.f, UTraceSettings::Get().SlideJumpWindowSpeedBonus);
}

bool UTraceCharacterMovementComponent::IsSourceAirAccelerationEnabled() const
{
	return UTraceSettings::Get().bSourceAirAcceleration;
}

float UTraceCharacterMovementComponent::GetAirAcceleration() const
{
	return FMath::Max(0.f, UTraceSettings::Get().AirAcceleration);
}

float UTraceCharacterMovementComponent::GetAirMaxWishSpeed() const
{
	return FMath::Max(0.f, UTraceSettings::Get().AirMaxWishSpeed);
}

float UTraceCharacterMovementComponent::GetMaxAirSpeed() const
{
	return FMath::Max(1.f, UTraceSettings::Get().MaxAirSpeed);
}

bool UTraceCharacterMovementComponent::IsLandingMomentumPreserved() const
{
	return UTraceSettings::Get().bPreserveLandingMomentum;
}

float UTraceCharacterMovementComponent::GetGroundOverspeedFriction() const
{
	return FMath::Max(0.f, UTraceSettings::Get().GroundOverspeedFriction);
}

float UTraceCharacterMovementComponent::GetGroundOverspeedBraking() const
{
	return FMath::Max(0.f, UTraceSettings::Get().GroundOverspeedBraking);
}

float UTraceCharacterMovementComponent::GetGroundOverspeedTurnRate() const
{
	return FMath::Max(0.f, UTraceSettings::Get().GroundOverspeedTurnRate);
}

void UTraceCharacterMovementComponent::RefreshEngineTunablesFromSettings()
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	const float DesiredWalkSpeed = FMath::Max(1.f, Settings.WalkSpeed);
	if (!FMath::IsNearlyEqual(MaxWalkSpeed, DesiredWalkSpeed))
	{
		MaxWalkSpeed = DesiredWalkSpeed;
		MaxWalkSpeedCrouched = DesiredWalkSpeed * 0.5f;
	}

	// AirControl and the lateral air friction are engine-owned fields consumed deep inside
	// PhysFalling, not at any point this file can intercept, so they are the two air values that
	// have to be COPIED rather than read at the point of use. Copying them once a move is what keeps
	// them live in PIE like everything else.
	//
	// AirControl matters more than it looks. GetFallingLateralAcceleration multiplies Acceleration
	// by it BEFORE CalcVelocity — and therefore before ApplySourceAirAcceleration — ever sees it, so
	// anything below 1 silently scales the wish vector and the documented air model stops being what
	// actually runs. UTraceSettings ships it at 1 and says so.
	const float DesiredAirControl = FMath::Clamp(Settings.AirControl, 0.f, 1.f);
	if (!FMath::IsNearlyEqual(AirControl, DesiredAirControl))
	{
		AirControl = DesiredAirControl;
	}

	const float DesiredAirFriction = FMath::Max(0.f, Settings.AirFriction);
	if (!FMath::IsNearlyEqual(FallingLateralFriction, DesiredAirFriction))
	{
		FallingLateralFriction = DesiredAirFriction;
	}
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
	// Dev override so the charge pool can be exercised without a Core.
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

// --- Momentum readouts --------------------------------------------------------------------------

float UTraceCharacterMovementComponent::GetPlanarSpeed() const
{
	return FVector(Velocity.X, Velocity.Y, 0.f).Size();
}

bool UTraceCharacterMovementComponent::IsCarryingExcessSpeed() const
{
	return IsMovingOnGround() && GetPlanarSpeed() > (GetMaxSpeed() + TraceMoveCfg::SpeedEpsilon);
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
	//
	// FLAG_Custom_1 is unused: it was boost, and boost is gone (spec v3 §1).
	bWantsToDash  = ((Flags & FSavedMove_Character::FLAG_Custom_0) != 0) ? 1 : 0;
	bWantsToSlide = ((Flags & FSavedMove_Character::FLAG_Custom_2) != 0) ? 1 : 0;
}

// =================================================================================================
// THE MOMENTUM MODEL
// =================================================================================================

void UTraceCharacterMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	// Mirror the base class's own bail-outs before deciding anything. A simulated proxy is fed its
	// velocity by replication and must not run either branch, or it would fight the interpolation.
	const bool bBaseWouldBailOut =
		!HasValidData()
		|| HasAnimRootMotion()
		|| DeltaTime < 1.e-6f
		|| (CharacterOwner != nullptr && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy && !bWasSimulatingRootMotion)
		|| CurrentRootMotion.HasOverrideVelocity();

	if (!bBaseWouldBailOut)
	{
		// --- AIR (spec §2.1) ---------------------------------------------------------------------
		//
		// Not while dashing: the dash owns the velocity vector outright for its whole window and
		// re-asserts it in OnMovementUpdated, so letting air input add to it first would just be
		// arithmetic nobody can observe.
		if (IsFalling() && IsSourceAirAccelerationEnabled() && DashTimeRemaining <= 0.f)
		{
			ApplySourceAirAcceleration(DeltaTime);
			return;
		}

		// --- CARRIED GROUND MOMENTUM (spec §2.2, §2.4) --------------------------------------------
		//
		// This is the branch that "removes the clamp of horizontal velocity to ground max speed on
		// landing". Super::CalcVelocity, the instant IsExceedingMaxSpeed() is true, runs
		// ApplyVelocityBraking with GroundFriction × BrakingFrictionFactor (8 × 2) plus
		// BrakingDecelerationWalking (2600) — which at 1900uu/s is about -33000uu/s², i.e. the whole
		// carry is gone inside four frames. Taking the branch ourselves is the only way to defeat it
		// without lowering GroundFriction, which would also make ordinary walking feel like ice.
		//
		// Not while sliding or dashing: those states set GetMaxSpeed() to their own speed, so they
		// are never "overspeed" by this definition and never reach here anyway — but the explicit
		// test documents that they own Velocity and this does not.
		if (IsMovingOnGround() && IsLandingMomentumPreserved()
			&& DashTimeRemaining <= 0.f && SlideTimeRemaining <= 0.f
			&& GetPlanarSpeed() > (FMath::Max(1.f, GetMaxSpeed()) + TraceMoveCfg::SpeedEpsilon))
		{
			ApplyGroundOverspeedBleed(DeltaTime);
			return;
		}
	}

	Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
}

void UTraceCharacterMovementComponent::ApplySourceAirAcceleration(float DeltaTime)
{
	// PhysFalling has already stripped Velocity.Z for the duration of this call and restores it
	// afterwards, so everything here is honestly planar. Never write Z.
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
	const float SpeedBefore = PlanarVelocity.Size();

	// Acceleration here is FallAcceleration: planar, scaled by AirControl (which
	// RefreshEngineTunablesFromSettings pins at 1.0 for exactly this reason) and clamped to
	// GetMaxAcceleration(). Its LENGTH is the analog input magnitude; its direction is the wish
	// direction. Using Acceleration rather than GetLastInputVector() is what makes this replay-safe:
	// MoveAutonomous restores Acceleration from the saved move on every corrected frame.
	FVector WishDirection(Acceleration.X, Acceleration.Y, 0.f);
	const float WishMagnitude = WishDirection.Size();

	// NO INPUT MEANS NO CHANGE. Not "decay toward zero" — Source has no air friction and neither do
	// we, and a decay here would make every jump cost speed, which is precisely the complaint.
	if (WishMagnitude <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	WishDirection /= WishMagnitude;

	const float InputScale = FMath::Clamp(WishMagnitude / FMath::Max(1.f, GetMaxAcceleration()), 0.f, 1.f);

	// THE FORMULA (Quake's PM_AirAccelerate, Source's CAirAccelerate, same maths):
	//
	//   1. WishSpeed is what the player is asking for, clamped to AirMaxWishSpeed. That clamp is the
	//      entire mechanic. In Quake it is 30 units/s; making it a knob is what "expose the accel cap
	//      and max air speed" means.
	//   2. Project the CURRENT velocity onto the wish direction. If the player is already travelling
	//      that fast in that direction, there is nothing to add.
	//   3. Add at most AirAcceleration·dt along the wish direction — and only ever ADD.
	//
	// Step 3 is why perpendicular input turns you for free: with the input at 90° to travel, the
	// projection is 0, so the full allowance is available, and it is applied SIDEWAYS. The resulting
	// vector is sqrt(v² + a²) long — very slightly FASTER, and rotated. Nothing anywhere subtracts a
	// component of velocity, so strafing can never cost speed. A lerp toward the input direction,
	// which is the usual mistake, subtracts on every frame and is why "air control" feels like a
	// brake.
	//
	// InputScale is the one addition to the formula as UTraceSettings documents it, and it is a
	// no-op for a keyboard: it only scales the target for a partially deflected analog stick, where
	// asking for half speed and getting the full turn allowance would be wrong.
	const float WishSpeed = FMath::Min(GetMaxAirSpeed(), GetAirMaxWishSpeed()) * InputScale;
	const float SpeedAlongWish = FVector::DotProduct(PlanarVelocity, WishDirection);
	const float AddSpeed = WishSpeed - SpeedAlongWish;

	if (AddSpeed <= 0.f)
	{
		return;
	}

	const float AccelSpeed = FMath::Min(GetAirAcceleration() * DeltaTime, AddSpeed);
	FVector NewPlanar = PlanarVelocity + WishDirection * AccelSpeed;

	// MaxAirSpeed is a ceiling on what air input may BUILD, never a brake on what the pawn arrived
	// with: a slide-jump that leaves the ground above the cap keeps every unit of it. Floor the
	// ceiling at the entry speed and the clamp can only ever remove speed this call just added.
	const float SpeedCeiling = FMath::Max(GetMaxAirSpeed(), SpeedBefore);
	const float NewSpeed = NewPlanar.Size();
	if (NewSpeed > SpeedCeiling && NewSpeed > UE_KINDA_SMALL_NUMBER)
	{
		NewPlanar *= (SpeedCeiling / NewSpeed);
	}

	Velocity.X = NewPlanar.X;
	Velocity.Y = NewPlanar.Y;
}

void UTraceCharacterMovementComponent::ApplyGroundOverspeedBleed(float DeltaTime)
{
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
	const float CurrentSpeed = PlanarVelocity.Size();
	if (CurrentSpeed <= TraceMoveCfg::SpeedEpsilon)
	{
		return;
	}

	const float GroundSpeedLimit = FMath::Max(1.f, GetMaxSpeed());
	FVector TravelDirection = PlanarVelocity / CurrentSpeed;

	// STEERING. Carried momentum you cannot aim is a punishment, not a reward, so the player may
	// rotate it — at GroundOverspeedTurnRate, and WITHOUT the rotation costing any speed, which is
	// the same rule the air model follows. Falls back to the AI's requested velocity so that a bot
	// coming out of a dash still corners instead of ballistically overshooting: path following sets
	// RequestedVelocity but leaves Acceleration at zero.
	FVector WishDirection(Acceleration.X, Acceleration.Y, 0.f);
	if (WishDirection.SizeSquared() <= UE_KINDA_SMALL_NUMBER && bHasRequestedVelocity)
	{
		WishDirection = FVector(RequestedVelocity.X, RequestedVelocity.Y, 0.f);
	}
	if (WishDirection.Normalize())
	{
		// A fixed ANGULAR rate (degrees/second), like the slide's steering and for the same reason:
		// the result depends only on (TravelDirection, WishDirection, rate x dt), all of which the
		// replay path reproduces exactly, and rotating by a fixed angle cannot change the magnitude.
		TravelDirection = TraceMovement::SteerTowards(
			TravelDirection, WishDirection, GetGroundOverspeedTurnRate() * DeltaTime);
	}

	// BLEED THE EXCESS, NOT THE SPEED. Friction proportional to the excess makes the carry taper
	// (fast at first, gentle as it approaches a run) and the flat braking term guarantees it
	// actually terminates rather than approaching the limit asymptotically forever.
	//
	// Floored at the ground limit, never below: once the excess is gone this branch stops being
	// taken and Super::CalcVelocity resumes on exactly the speed normal movement would allow, so
	// there is no discontinuity at the handover.
	const float ExcessSpeed = CurrentSpeed - GroundSpeedLimit;
	const float Bleed = (GetGroundOverspeedFriction() * ExcessSpeed + GetGroundOverspeedBraking()) * DeltaTime;
	const float NewSpeed = FMath::Max(GroundSpeedLimit, CurrentSpeed - Bleed);

	Velocity.X = TravelDirection.X * NewSpeed;
	Velocity.Y = TravelDirection.Y * NewSpeed;
	// Z is already zero here: PhysWalking calls MaintainHorizontalGroundVelocity() before this.
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
	// overlap would mean two writers fighting over Velocity every frame. Through EndSlide() so the
	// measurement and the exit rule stay on one path; the velocity it writes is overwritten by the
	// dash launch below, which is correct — the dash's speed is strictly the larger of the two.
	//
	// A dash beats the commit window on purpose: the commit exists to stop a slide being fumbled
	// away by the crouch key, not to take the dash away from a player who is about to be shot.
	EndSlide();

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
	// hand out free speed, or "tap crouch" becomes the fastest way to cross the field. This matters
	// more now than it did: with SlideEntrySpeedMultiplier at 1.0 the slide has nothing of its own
	// to give, so entering one slowly would be strictly worse than running.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float EntrySpeed = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideEntrySpeedFraction);

	return FVector(Velocity.X, Velocity.Y, 0.f).SizeSquared() >= FMath::Square(EntrySpeed);
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

	SlideDirection = Direction;

	// --- ENTRY SPEED DETERMINES SLIDE VELOCITY (spec v4 §1) --------------------------------------
	//
	// The original formula was max(planar speed, WalkSpeed) × SlideSpeedMultiplier(1.35), which is a
	// flat momentum boost by any reading — a slide entered at walking pace came out 35% faster than a
	// run, for free. Spec v3 flagged that; spec v4 §1 settled it: "The flat momentum boost should be
	// ruled out, going with the source-style movement system instead."
	//
	// So entry is the speed the pawn actually arrived with, and there is exactly one term left:
	//
	//   SlideSpeed = max(EntrySpeed, min(EntrySpeed × SlideEntrySpeedMultiplier, SlideMaxSpeed))
	//
	//   SlideEntrySpeedMultiplier = 1.0  → slide speed IS entry speed.
	//   SlideImpulse                     → DELETED. It was the flat addition, and it is gone.
	//
	// THE OUTER max() IS NOT A BOOST, and it is the one thing here that could be misread as one. It
	// cannot manufacture speed: at multiplier 1.0 it is a no-op, and its only job is to stop
	// SlideMaxSpeed BRAKING somebody who arrived above the cap. Without it, a player landing an
	// air-strafe at 1900+ uu/s, or sliding out of a dash, would be SLOWED by pressing crouch — the
	// precise opposite of "you keep what you brought in", and a floor on losses is not a boost.
	const float EntrySpeed = FVector(Velocity.X, Velocity.Y, 0.f).Size();
	const float ScaledEntrySpeed = FMath::Min(EntrySpeed * GetSlideEntrySpeedMultiplier(),
	                                          FMath::Max(1.f, Settings.SlideMaxSpeed));

	SlideSpeed = FMath::Max(ScaledEntrySpeed, EntrySpeed);

	SlideTimeRemaining = GetSlideDuration();
	SlideCommitRemaining = GetSlideMinCommitSeconds();

	// A fresh slide owns the slide-jump window outright: any coyote grace left over from the previous
	// slide is void, or a slide started 0.1s after one ended would inherit the last one's "well
	// timed" bit and pay the bonus for a hop nobody earned.
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;

	// Cleared, not set: the between-slides buffer is charged in EndSlide() because spec §2.3 asks
	// for a gap BETWEEN slides. An active slide is already blocked by SlideTimeRemaining.
	SlideCooldownRemaining = 0.f;

	Velocity.X = SlideDirection.X * SlideSpeed;
	Velocity.Y = SlideDirection.Y * SlideSpeed;
	if (IsMovingOnGround())
	{
		Velocity.Z = 0.f;
	}

#if !UE_BUILD_SHIPPING
	if (IsSlideDebugEnabled())
	{
		SlideDebugEntrySpeed = SlideSpeed;
		SlideDebugStartLocation = (UpdatedComponent != nullptr)
			? UpdatedComponent->GetComponentLocation()
			: FVector::ZeroVector;
		SlideDebugStartTime = (GetWorld() != nullptr) ? static_cast<float>(GetWorld()->GetTimeSeconds()) : 0.f;
	}
#endif
}

void UTraceCharacterMovementComponent::EndSlide()
{
	if (SlideTimeRemaining <= 0.f && SlideSpeed <= 0.f)
	{
		// Idempotent: the cancel paths and the natural exits can all reach here, and a second call
		// must not re-write Velocity with a stale direction or re-charge the cooldown.
		SlideCommitRemaining = 0.f;
		return;
	}

	// --- Hand the momentum back, AND NEVER TOP IT UP ---------------------------------------------
	//
	// The exit speed is the slide's own live speed scaled by SlideExitSpeedRetention and capped at
	//
	//     max(SlideExitMaxSpeedMultiplier × GetMaxSpeed(), the slide's own speed)
	//
	// That max() is "state transitions should preserve velocity vectors rather than resetting them",
	// in one term. Without it, slide→jump was a hard brake: the ceiling evaluated to the walk speed
	// (800) and a 1900uu/s slide handed the player into the air at 800. With it, a slide can never end
	// below the speed it was running at, and the excess then bleeds off through
	// ApplyGroundOverspeedBleed like any other carried momentum — or survives into the air intact if
	// the exit was a jump, which is the entire Apex slide-jump.
	//
	// THERE IS NO FLOOR ANY MORE (spec v4 §1). The old code clamped this into
	// [SlideExitMinSpeedFraction × WalkSpeed, ExitCeiling], and at the shipped fraction of 1.0 that
	// lower bound was WalkSpeed itself — so a slide that had decayed to 470 uu/s was handed back at
	// 820, a measured 73% speed GAIN, for the crime of ending. That is the flat momentum boost the
	// design owner ruled out, spelled on the exit instead of the entry. A slide now ends at exactly
	// what friction left it with, and ordinary ground acceleration takes it from there.
	const float Retained = FMath::Max(0.f, SlideSpeed) * GetSlideExitSpeedRetention();

	// Clear FIRST: GetMaxSpeed() folds in SlideSpeed while IsSliding(), so the ceiling below has to
	// be computed against the speed the pawn is about to live under, not the slide's.
	//
	// The well-timed bit is read off SlideTimeRemaining before it is zeroed, because once the slide is
	// over the information is gone — see SlideJumpGraceRemaining.
	//
	// GetSlideTimeLeft(), not SlideTimeRemaining: a slide that ends by DECAY is just as much at its
	// end as one that runs its duration out, and measuring only the duration clock made every
	// walking-pace slide score as mistimed however well the hop was pressed.
	const float WellTimedWindow = GetSlideJumpWindowSeconds();
	bSlideJumpGraceWellTimed = (WellTimedWindow > 0.f && GetSlideTimeLeft() <= WellTimedWindow) ? 1 : 0;
	SlideJumpGraceRemaining = WellTimedWindow;

	SlideTimeRemaining = 0.f;
	SlideCommitRemaining = 0.f;
	const float ExitedSpeed = SlideSpeed;
	SlideSpeed = 0.f;

	// Spec §2.3: "add a .8 second buffer between slides". Charged HERE, at the end, so the knob says
	// what it means — the old cooldown was measured from slide start, which made the actual buffer
	// SlideCooldown minus SlideDuration.
	SlideCooldownRemaining = GetSlideCooldownSeconds();

	// Named ExitCeiling rather than the obvious Ceiling: "Floor" is dense with meaning inside a
	// movement component (CurrentFloor, FindFloor, FFindFloorResult) and a local that reads like the
	// walkable surface in a function about speed is a trap for the next reader. The matching ExitFloor
	// local is gone with the setting that fed it.
	float ExitCeiling = FMath::Max(1.f, GetMaxSpeed()) * GetSlideExitMaxSpeedMultiplier();
	if (IsLandingMomentumPreserved())
	{
		ExitCeiling = FMath::Max(ExitCeiling, ExitedSpeed);
	}

	// Min, not Clamp. The only bound left is the ceiling; nothing lifts a slow exit.
	const float ExitSpeed = FMath::Min(Retained, ExitCeiling);

	// Direction of TRAVEL, not the steered slide direction: on a ledge or against a wall the two can
	// differ, and the player's momentum is the one they can see.
	FVector ExitDirection(Velocity.X, Velocity.Y, 0.f);
	if (!ExitDirection.Normalize())
	{
		ExitDirection = SlideDirection;
		ExitDirection.Z = 0.f;
		if (!ExitDirection.Normalize())
		{
			ExitDirection = (UpdatedComponent != nullptr) ? UpdatedComponent->GetForwardVector() : FVector::ForwardVector;
			ExitDirection.Z = 0.f;
			if (!ExitDirection.Normalize())
			{
				ExitDirection = FVector::ForwardVector;
			}
		}
	}

	Velocity.X = ExitDirection.X * ExitSpeed;
	Velocity.Y = ExitDirection.Y * ExitSpeed;

#if !UE_BUILD_SHIPPING
	// Observation only — never feeds the simulation, so it cannot desync anything. Logged on the
	// authority alone: the server also advances a remote client's slide inside MoveAutonomous, and a
	// client replaying corrections would otherwise count the same slide several times.
	// SlideDebugStartTime > 0 rejects the one slide that could be in flight when the cvar is toggled
	// on mid-match, whose start was never recorded and would otherwise report the whole match as its
	// duration and poison the mean.
	if (IsSlideDebugEnabled() && SlideDebugStartTime > 0.f
		&& CharacterOwner != nullptr && CharacterOwner->HasAuthority() && GetWorld() != nullptr)
	{
		const float Now = static_cast<float>(GetWorld()->GetTimeSeconds());
		const float Duration = FMath::Max(0.f, Now - SlideDebugStartTime);
		const FVector Here = (UpdatedComponent != nullptr) ? UpdatedComponent->GetComponentLocation() : SlideDebugStartLocation;
		const float Distance = FVector::Dist2D(Here, SlideDebugStartLocation);

		++GTraceSlideDebugCount;
		GTraceSlideDebugTotalDuration += Duration;
		GTraceSlideDebugTotalDistance += Distance;
		SlideDebugStartTime = 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("SLIDE %-16s dur=%5.2fs dist=%6.0fuu entry=%6.0f exitSpeed=%6.0f (slideSpeed was %6.0f) | n=%3d avgDur=%5.2fs avgDist=%6.0fuu"),
			*GetNameSafe(CharacterOwner), Duration, Distance, SlideDebugEntrySpeed, ExitSpeed, ExitedSpeed,
			GTraceSlideDebugCount,
			GTraceSlideDebugTotalDuration / FMath::Max(1, GTraceSlideDebugCount),
			GTraceSlideDebugTotalDistance / FMath::Max(1, GTraceSlideDebugCount));
	}
#endif
}

// --- Slide-jump ---------------------------------------------------------------------------------

bool UTraceCharacterMovementComponent::IsSlideJumpAvailable() const
{
	return IsSlideJumpEnabled() && MovementMode != MOVE_None
		&& (IsSliding() || SlideJumpGraceRemaining > 0.f);
}

float UTraceCharacterMovementComponent::GetSlideTimeLeft() const
{
	if (SlideTimeRemaining <= 0.f)
	{
		return 0.f;
	}

	// Route 1: the duration clock.
	float TimeLeft = SlideTimeRemaining;

	// Route 2: the decay. OnMovementUpdated ends the slide as soon as SlideSpeed falls to
	// WalkSpeed x SlideExitSpeedFraction, and with the shipped numbers that happens FIRST for anybody
	// who entered at walking pace. Deceleration is constant, so the crossing time is exact rather than
	// estimated — no iteration, no drift between client and server.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Deceleration = GetSlideDeceleration();
	if (Deceleration > 0.f)
	{
		const float DecayFloor = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideExitSpeedFraction);
		const float SpeedAboveFloor = SlideSpeed - DecayFloor;
		TimeLeft = FMath::Min(TimeLeft, FMath::Max(0.f, SpeedAboveFloor) / Deceleration);
	}

	return TimeLeft;
}

bool UTraceCharacterMovementComponent::IsSlideJumpWellTimed() const
{
	if (!IsSlideJumpAvailable())
	{
		return false;
	}

	const float Window = GetSlideJumpWindowSeconds();
	if (Window <= 0.f)
	{
		return false;
	}

	// Mid-slide the window is live — the last Window seconds of the slide, by WHICHEVER exit the slide
	// is actually heading for (see GetSlideTimeLeft). During the coyote grace it is whatever the slide
	// was worth at the moment it ended.
	return IsSliding() ? (GetSlideTimeLeft() <= Window) : (bSlideJumpGraceWellTimed != 0);
}

bool UTraceCharacterMovementComponent::CanAttemptJump() const
{
	// Super, MINUS the "!bWantsToCrouch" clause. See the header: crouch is the slide key here and
	// never resizes the capsule, so the engine's "you might not have headroom to stand up" rule has
	// nothing to protect and was silently making the slide-jump unreachable for human players.
	//
	// Everything else Super checks is kept verbatim, including the IsMovingOnGround() || IsFalling()
	// test, which ACharacter::JumpIsAllowedInternal still validates against JumpMaxCount on top.
	return IsJumpAllowed() && (IsMovingOnGround() || IsFalling());
}

bool UTraceCharacterMovementComponent::DoJump(bool bReplayingMoves, float DeltaTime)
{
	// Capture BEFORE anything is touched: EndSlide() below rewrites every one of these.
	const bool bSlideJump = IsSlideJumpAvailable();
	const bool bWellTimed = IsSlideJumpWellTimed();

	// The speed the jump is entitled to carry. Mid-slide that is the slide's own live speed — which is
	// also what Velocity is, because OnMovementUpdated re-asserts it every frame — and during the
	// coyote grace it is simply whatever the pawn has now, which is the honest Source answer: you keep
	// what you brought, and if you dawdled after the slide ended, friction has already taken its cut.
	const float CarrySpeed = IsSliding()
		? FMath::Max(SlideSpeed, GetPlanarSpeed())
		: GetPlanarSpeed();

	// The slide direction, kept as a fallback for the degenerate case where Velocity has been zeroed
	// by a collision on the exact frame of the jump.
	const FVector CarryDirectionFallback = SlideDirection;

	if (bSlideJump)
	{
		// THE ONE EXIT, still. A jump out of a slide ends it through EndSlide() like the duration
		// expiring or the key coming up, so the 0.8s between-slides buffer is charged exactly once and
		// on exactly one code path — spec v4 §1 keeps that cooldown, and a slide-jump that skipped it
		// would turn the payoff move into a hop loop. The velocity EndSlide() writes is overwritten
		// below; that is intended, and it is why the retention is applied to CarrySpeed (captured
		// above) rather than to whatever the exit rule happened to leave behind.
		EndSlide();
	}

	// Super sets Velocity.Z = max(Velocity.Z, JumpZVelocity) and switches to MOVE_Falling, or returns
	// false if the jump was not legal. If it refuses, the slide has already ended — which is correct
	// and not a leak: the only way to reach here with bSlideJump true and be refused is to be dead or
	// movement-disabled, and in both cases the slide has to end anyway.
	if (!Super::DoJump(bReplayingMoves, DeltaTime))
	{
		return false;
	}

	if (!bSlideJump)
	{
		return true;
	}

	// --- The payoff -------------------------------------------------------------------------------
	//
	// Retention 1.0 is PURE PRESERVATION, not a boost. What it buys the player is escaping the ground
	// friction that would otherwise have eaten the carry over the next second — which is exactly the
	// Apex slide-hop, and exactly why the flat entry boost is no longer needed to make sliding worth
	// doing.
	const float Retention = GetSlideJumpHorizontalRetention()
		* (bWellTimed ? GetSlideJumpWindowSpeedBonus() : 1.f);
	const float LaunchSpeed = CarrySpeed * Retention;

	FVector LaunchDirection(Velocity.X, Velocity.Y, 0.f);
	if (!LaunchDirection.Normalize())
	{
		LaunchDirection = CarryDirectionFallback;
		LaunchDirection.Z = 0.f;
		if (!LaunchDirection.Normalize())
		{
			LaunchDirection = (UpdatedComponent != nullptr)
				? UpdatedComponent->GetForwardVector()
				: FVector::ForwardVector;
			LaunchDirection.Z = 0.f;
			if (!LaunchDirection.Normalize())
			{
				LaunchDirection = FVector::ForwardVector;
			}
		}
	}

	Velocity.X = LaunchDirection.X * LaunchSpeed;
	Velocity.Y = LaunchDirection.Y * LaunchSpeed;

	// Z has just been set to JumpZVelocity by Super. Scaling rather than assigning keeps this honest
	// on the multi-frame path: with a non-zero JumpMaxHoldTime the engine calls DoJump on several
	// consecutive frames, and the slide is only alive for the first of them, so only the first can be
	// a slide-jump. A second scaling cannot happen because bSlideJump is false by then.
	Velocity.Z *= GetSlideJumpZMultiplier();

	// Consumed. One slide, one slide-jump.
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;

#if !UE_BUILD_SHIPPING
	// Observation only, and on the authority alone so a client replaying corrections cannot count the
	// same hop several times. At Display, behind the same switch as the slide measurement.
	if (IsSlideDebugEnabled() && CharacterOwner != nullptr && CharacterOwner->HasAuthority())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("SLIDEJUMP %-16s carried=%6.0f -> launched=%6.0f uu/s (%.1f%%, retain=%.2f%s) velZ=%6.0f (jumpZ=%.0f x %.2f)"),
			*GetNameSafe(CharacterOwner), CarrySpeed, GetPlanarSpeed(),
			100.f * GetPlanarSpeed() / FMath::Max(1.f, CarrySpeed), Retention,
			bWellTimed ? TEXT(", WELL TIMED") : TEXT(""),
			Velocity.Z, JumpZVelocity, GetSlideJumpZMultiplier());
	}
#endif

	return true;
}

void UTraceCharacterMovementComponent::ApplyDashExitSpeed()
{
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
	const float PlanarLimit = FMath::Max(1.f, GetMaxSpeed()) * GetDashExitSpeedMultiplier();
	if (PlanarVelocity.SizeSquared() > FMath::Square(PlanarLimit))
	{
		const FVector Clamped = PlanarVelocity.GetSafeNormal() * PlanarLimit;
		Velocity.X = Clamped.X;
		Velocity.Y = Clamped.Y;
	}
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

	// 0. Pick up config a designer has just changed in Project Settings. BeginPlay's copy is the one
	//    piece of cached config in this file, and caching is exactly what this file's own rules
	//    forbid — without this, retuning WalkSpeed during PIE did nothing until the map reloaded.
	RefreshEngineTunablesFromSettings();

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
	if (SlideCommitRemaining > 0.f)
	{
		SlideCommitRemaining = FMath::Max(0.f, SlideCommitRemaining - DeltaSeconds);
	}
	if (SlideBufferRemaining > 0.f)
	{
		SlideBufferRemaining = FMath::Max(0.f, SlideBufferRemaining - DeltaSeconds);
	}
	if (SlideJumpGraceRemaining > 0.f)
	{
		// The slide-jump's coyote window. Ticked here with every other clock so a replayed move
		// advances it by exactly the same amount the original did.
		SlideJumpGraceRemaining = FMath::Max(0.f, SlideJumpGraceRemaining - DeltaSeconds);
		if (SlideJumpGraceRemaining <= 0.f)
		{
			bSlideJumpGraceWellTimed = 0;
		}
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

	// 1d. The frame a dash ends, hand the player back at DashExitSpeedMultiplier x the ground limit
	//     rather than AT the ground limit. See ApplyDashExitSpeed().
	if (bWasDashing && DashTimeRemaining <= 0.f)
	{
		ApplyDashExitSpeed();
	}

	// 1e. A slide whose duration has just run out exits through EndSlide() like every other slide
	//     exit, so it KEEPS its momentum instead of being clamped. Routing the timer expiry through
	//     the same function as the key release is what makes "hold the slide out" and "cancel it
	//     early" cost the same, which is what stops one of them becoming the only correct play.
	if (bWasSliding && SlideTimeRemaining <= 0.f)
	{
		EndSlide();
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

	// 3. Crouch: slide on the ground, fast-fall in the air. One key, resolved by where the pawn is.
	//
	//    A press that cannot be honoured yet (mid-dash, or airborne) is buffered rather than thrown
	//    away — see SlideBufferRemaining. The buffer is charged from the press EDGE only, so holding
	//    the key can never chain slides. It is also what makes "air-strafe, then slide the instant
	//    you touch down" a single input instead of a frame-perfect one.
	const bool bOnGroundNow = IsMovingOnGround();
	const bool bJustLanded = bOnGroundNow && (bWasAirborneLastMove != 0);

	if (bSlidePressedThisMove)
	{
		SlideBufferRemaining = FMath::Max(0.f, Settings.SlideInputBufferSeconds);
	}
	else if (bJustLanded && bCrouchHeld)
	{
		// LANDING WITH CROUCH HELD IS A SLIDE (spec v3 §2.4, jump->slide).
		//
		// The press edge alone cannot express this. A crouch pressed in the air is consumed by the
		// fast-fall, which zeroes the buffer on purpose so that one press does not silently mean two
		// things; and the buffer is a quarter of a second while a jump is over a second. So a player
		// who holds crouch from the apex all the way down used to land, keep nothing, and have to
		// re-press — measured: a 1293 uu/s landing produced no slide at all.
		//
		// Charging the buffer on the landing TRANSITION fixes that without reopening the "one press,
		// two meanings" problem: the key is still held, the player is still asking, and it can fire
		// only once per landing because the next move is no longer a transition.
		SlideBufferRemaining = FMath::Max(SlideBufferRemaining, FMath::Max(0.f, Settings.SlideInputBufferSeconds));
	}

	if (SlideTimeRemaining > 0.f)
	{
		// --- Maintain an active slide -----------------------------------------------------------
		//
		// Two ways out here, and only one of them is negotiable. Leaving the ground always ends the
		// slide — it is a ground state and the floor is what it is sliding on. Releasing the key ends
		// it too, but NOT during the commit window: for SlideMinCommitSeconds the slide is bought and
		// paid for. Momentum survives either exit now, so neither one is a punishment.
		const bool bCommitted = (SlideCommitRemaining > 0.f);
		if (!IsMovingOnGround() || (!bCrouchHeld && !bCommitted))
		{
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

			// The friction dial. Small on purpose: the slide is meant to be ended by SlideDuration,
			// not by having bled itself back down to a walk two thirds of the way through.
			SlideSpeed = FMath::Max(0.f, SlideSpeed - GetSlideDeceleration() * DeltaSeconds);

			const float FloorSpeed = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideExitSpeedFraction);
			if (SlideSpeed <= FloorSpeed)
			{
				// Decayed back to walking pace: stop rather than drag the player along at a speed the
				// normal movement code would have given them anyway.
				//
				// This is an EXIT CONDITION, not a floor on speed, and the distinction is the whole of
				// spec v4 §1. It ends the slide when the slide has stopped being worth anything; it
				// does not hand the player a single unit they did not already have. EndSlide() used to
				// then lift them back to WalkSpeed on the way out (SlideExitMinSpeedFraction), which
				// DID contradict "entry speed determines slide velocity" — that is deleted, so a slide
				// that decays out now leaves the player at ~SlideExitSpeedFraction of the walk speed
				// and they re-accelerate normally, exactly as if they had never pressed crouch.
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
		// horizontal carry must survive — and with the Source air model that carry can now be well
		// above walking pace, which is exactly the state a fast-fall wants to bring to the floor.
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

	// 4. Latch the last instant this pawn was inside its dash window, on the authority only.
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

	// 5. Consume the one-shot intent and remember the held one for the next move's edge test. On
	//    the server the intents are re-supplied by the next ServerMove's flags, on the client by the
	//    next StartDash(), and during replay by UpdateFromCompressedFlags — so one key press can
	//    only ever produce one dash.
	bWantsToDash = 0;
	bSlideHeldLastMove = bCrouchHeld ? 1 : 0;
	bWasAirborneLastMove = (MovementMode != MOVE_None && !IsMovingOnGround()) ? 1 : 0;

#if !UE_BUILD_SHIPPING
	TickMomentumMeasure(DeltaSeconds);
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

// =================================================================================================
// Dev-only measurement harness — "-TraceMoveMeasure". See the header.
//
// This exists because the four things spec §2 asks for are all NUMBERS, and none of them can be
// verified from a screenshot or from "it feels better". It prints, at Display:
//
//   AIRTURN   planar speed before / after a 90-degree strafe turn, and the angle actually turned
//   LAND      planar speed the frame before touchdown and 0.1s / 0.3s / 0.6s after it
//   SLIDE     entry speed vs exit speed, and the measured gap before the next slide is allowed
// =================================================================================================

#if !UE_BUILD_SHIPPING

int32 GTraceMoveKitFakeCarrier = 0;
static FAutoConsoleVariableRef CVarTraceMoveKitFakeCarrier(
	TEXT("Trace.MoveKitFakeCarrier"),
	GTraceMoveKitFakeCarrier,
	TEXT("Dev only. Non-zero pretends this pawn is carrying the Core for the purposes of the dash "
	     "charge pool, so the carrier's extra charge can be exercised without a Core."),
	ECVF_Cheat);

void UTraceCharacterMovementComponent::TickMomentumMeasure(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceMoveMeasure"));
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

	if (MeasureTime < 0.f)
	{
		// Let the pawn spawn, settle onto the floor and finish its first replication.
		if (GetWorld() == nullptr || GetWorld()->GetTimeSeconds() < 3.f || !IsMovingOnGround())
		{
			return;
		}
		MeasureTime = 0.f;
		MeasurePhase = 0;
		MeasurePhaseTime = 0.f;

		// Run toward the middle of the field, not along a world axis: spawns are in the endzones and
		// a fixed +X run walks straight into the back wall, which turns every number after it into a
		// measurement of a collision.
		MeasureRunDirection = FVector::ForwardVector;
		if (UpdatedComponent != nullptr)
		{
			FVector TowardCentre = -UpdatedComponent->GetComponentLocation();
			TowardCentre.Z = 0.f;
			if (TowardCentre.Normalize())
			{
				MeasureRunDirection = TowardCentre;
			}
		}
		UE_LOG(LogTraceGame, Display,
			TEXT("MEASURE ---- begin. walk=%.0f | air: srcModel=%d accel=%.0f wishCap=%.0f maxAir=%.0f airFric=%.2f airControl=%.2f "
			     "| land: preserve=%d fric=%.2f brake=%.0f turn=%.0fdeg/s | dashExit=%.2fx "
			     "| slide: entryMul=%.2f cooldown=%.2fs (NO impulse, NO exit floor) "
			     "| slideJump: on=%d retain=%.2f zMul=%.2f window=%.2fs windowBonus=%.2f"),
			MaxWalkSpeed, IsSourceAirAccelerationEnabled() ? 1 : 0, GetAirAcceleration(), GetAirMaxWishSpeed(),
			GetMaxAirSpeed(), FallingLateralFriction, AirControl,
			IsLandingMomentumPreserved() ? 1 : 0, GetGroundOverspeedFriction(), GetGroundOverspeedBraking(),
			GetGroundOverspeedTurnRate(), GetDashExitSpeedMultiplier(),
			GetSlideEntrySpeedMultiplier(), GetSlideCooldownSeconds(),
			IsSlideJumpEnabled() ? 1 : 0, GetSlideJumpHorizontalRetention(), GetSlideJumpZMultiplier(),
			GetSlideJumpWindowSeconds(), GetSlideJumpWindowSpeedBonus());
		MeasureHomeLocation = (UpdatedComponent != nullptr)
			? UpdatedComponent->GetComponentLocation()
			: FVector::ZeroVector;

		UE_LOG(LogTraceGame, Display, TEXT("MEASURE run direction %s from %s"),
			*MeasureRunDirection.ToCompactString(),
			*(UpdatedComponent != nullptr ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector).ToCompactString());
	}

	MeasureTime += DeltaSeconds;
	MeasurePhaseTime += DeltaSeconds;

	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
	const float PlanarSpeed = PlanarVelocity.Size();
	const FVector TravelDirection = PlanarVelocity.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);

	auto Advance = [this](int32 NextPhase)
	{
		MeasurePhase = NextPhase;
		MeasurePhaseTime = 0.f;
	};

	// Angle between the velocity vector now and where it pointed when the phase started.
	auto TurnedDegrees = [this, &TravelDirection]() -> float
	{
		return FMath::RadiansToDegrees(FMath::Acos(
			FMath::Clamp(FVector::DotProduct(MeasureMarkDirection, TravelDirection), -1.f, 1.f)));
	};

	switch (MeasurePhase)
	{
	// --- 0. A CONTROLLED DROP -----------------------------------------------------------------
	//
	// The air numbers are taken from a scripted drop rather than from a run-and-jump, because a
	// run-and-jump measures the arena as much as it measures the movement model: the first two
	// attempts at this harness sprinted into an endzone wall and a bank, and reported a jump that
	// "lost" 750 uu/s, which was a collision. Placing the pawn high over the middle of the field
	// with a known velocity isolates the model. The run-and-jump is still exercised below, on the
	// ground, where the slide numbers are taken.
	case 0:
		if (UpdatedComponent != nullptr && CharacterOwner != nullptr)
		{
			const FVector Here = UpdatedComponent->GetComponentLocation();
			CharacterOwner->SetActorLocation(FVector(0.f, 0.f, Here.Z + 1500.f), false, nullptr, ETeleportType::TeleportPhysics);
			Velocity = FVector(MaxWalkSpeed, 0.f, 0.f);
			SetMovementMode(MOVE_Falling);
			MeasureMarkA = MaxWalkSpeed;
			MeasureMarkDirection = FVector::ForwardVector;
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE DROP: placed at (0,0,%.0f) with planar=%.0f uu/s along +X"),
				Here.Z + 1500.f, MaxWalkSpeed);
			Advance(1);
		}
		break;

	// --- 1. FIXED perpendicular input -------------------------------------------------------------
	//
	// The strongest possible refutation of a lerp-style air control: hold a direction exactly 90
	// degrees from travel and do nothing else. A lerp subtracts the forward component every frame
	// and the speed FALLS. The projection formula can only add sideways, so the speed must not fall.
	case 1:
	{
		const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, MeasureMarkDirection).GetSafeNormal();
		CharacterOwner->AddMovementInput(Perpendicular, 1.f);
		UE_LOG(LogTraceGame, Display, TEXT("MEASURE   air t=%.3f planar=%7.1f velZ=%8.1f mode=%d accel=%6.0f turned=%5.1f"),
			MeasurePhaseTime, PlanarSpeed, Velocity.Z, static_cast<int32>(MovementMode.GetValue()),
			Acceleration.Size2D(), TurnedDegrees());
		if (MeasurePhaseTime > 0.30f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRTURN-FIXED: %.0f -> %.0f uu/s over %.2fs of fixed perpendicular input "
				     "(%.1f%% of entry, vector turned %.1f deg)"),
				MeasureMarkA, PlanarSpeed, MeasurePhaseTime,
				100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA), TurnedDegrees());
			MeasureMarkA = PlanarSpeed;
			MeasureMarkDirection = TravelDirection;
			Advance(2);
		}
		break;
	}

	// --- 2. CONTINUOUSLY perpendicular input (a real strafe turn) ---------------------------------
	//
	// The wish direction is recomputed every frame to stay at 90 degrees to the CURRENT velocity —
	// which is exactly what a player doing a strafe turn produces with mouse + strafe key. This is
	// the number "slightly increase efficacy of strafing in mid air" is about.
	case 2:
	{
		const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, TravelDirection).GetSafeNormal();
		CharacterOwner->AddMovementInput(Perpendicular, 1.f);
		UE_LOG(LogTraceGame, Display, TEXT("MEASURE   air t=%.3f planar=%7.1f velZ=%8.1f mode=%d accel=%6.0f turned=%5.1f"),
			MeasurePhaseTime, PlanarSpeed, Velocity.Z, static_cast<int32>(MovementMode.GetValue()),
			Acceleration.Size2D(), TurnedDegrees());
		if (MeasurePhaseTime > 0.40f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRTURN-STRAFE: %.0f -> %.0f uu/s over %.2fs of continuously perpendicular input "
				     "(%.1f%% of entry, vector turned %.1f deg)"),
				MeasureMarkA, PlanarSpeed, MeasurePhaseTime,
				100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA), TurnedDegrees());
			Advance(3);
		}
		break;
	}

	// --- 3. COAST, then LANDING CARRY ---------------------------------------------------------------
	//
	// No input at all from here: Source has no air friction, so the speed must not move a unit
	// between the last input frame and touchdown, and the touchdown must not clamp it.
	case 3:
		if (!IsMovingOnGround())
		{
			// Refreshed every airborne frame; the last value written is the frame before touchdown.
			MeasureMarkA = PlanarSpeed;
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE LAND: last airborne planar=%.0f uu/s -> first grounded planar=%.0f uu/s "
				     "(%.1f%% carried; ground limit is %.0f)"),
				MeasureMarkA, PlanarSpeed, 100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA), GetMaxSpeed());
			Advance(4);
		}
		break;

	// --- 4-6. Watch the bleed. No input at all, so this is purely the decay curve. ------------------
	case 4:
		if (MeasurePhaseTime > 0.10f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE LAND +0.10s: planar=%.0f uu/s"), PlanarSpeed);
			Advance(5);
		}
		break;

	case 5:
		if (MeasurePhaseTime > 0.20f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE LAND +0.30s: planar=%.0f uu/s"), PlanarSpeed);
			Advance(6);
		}
		break;

	case 6:
		if (MeasurePhaseTime > 0.30f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE LAND +0.60s: planar=%.0f uu/s (ground limit %.0f)"),
				PlanarSpeed, GetMaxSpeed());
			Advance(7);
		}
		break;

	// --- 7. A REAL run-and-jump, for the run->jump transition ---------------------------------------
	case 7:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		if (MeasurePhaseTime > 1.2f)
		{
			if (IsMovingOnGround())
			{
				MeasureMarkA = PlanarSpeed;
				CharacterOwner->Jump();
			}
			else
			{
				CharacterOwner->StopJumping();
				UE_LOG(LogTraceGame, Display,
					TEXT("MEASURE RUN->JUMP: %.0f uu/s on the ground -> %.0f uu/s on the first airborne frame (%.1f%%)"),
					MeasureMarkA, PlanarSpeed, 100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA));
				Advance(8);
			}
		}
		break;

	// --- 8. SLIDE 1: entry speed determines slide velocity -------------------------------------------
	case 8:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		if (MeasurePhaseTime > 1.5f)
		{
			MeasureMarkA = PlanarSpeed;
			SetWantsToSlide(true);
			Advance(9);
		}
		break;

	case 9:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(true);
		if (IsSliding())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDE-1 entry: %.0f uu/s in -> slideSpeed=%.0f uu/s (ratio %.2f, entryMul=%.2f). "
				     "Ratio must be 1.00: entry speed determines slide velocity, nothing tops it up."),
				MeasureMarkA, SlideSpeed, SlideSpeed / FMath::Max(1.f, MeasureMarkA),
				GetSlideEntrySpeedMultiplier());
			Advance(10);
		}
		else if (MeasurePhaseTime > 1.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE SLIDE-1 never latched (planar=%.0f, cooldown=%.2f)"),
				PlanarSpeed, GetSlideCooldownRemaining());
			Advance(12);
		}
		break;

	case 10:
		// Hold it out to its natural end so the exit measured is the one the duration produces.
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(true);
		if (!IsSliding())
		{
			MeasureMarkB = static_cast<float>(GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDE-1 exit: after %.2fs, planar=%.0f uu/s (entry was %.0f, %.1f%% kept), buffer now %.2fs. "
				     "THE EXIT FLOOR IS GONE: the deleted SlideExitMinSpeedFraction would have forced this to %.0f."),
				MeasurePhaseTime, PlanarSpeed, MeasureMarkA,
				100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA), GetSlideCooldownRemaining(),
				MaxWalkSpeed);
			SetWantsToSlide(false);
			Advance(11);
		}
		break;

	// --- 11. SLIDE 2: prove the between-slides buffer ------------------------------------------------
	//
	// Crouch is PULSED rather than held: the slide needs a fresh press edge, and the input buffer is
	// only 0.25s, so one press at the start would expire long before the 0.8s buffer does.
	case 11:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(FMath::Fmod(MeasurePhaseTime, 0.2f) < 0.1f);
		if (IsSliding())
		{
			const float Now = static_cast<float>(GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDE-2 started %.2fs after slide 1 ended (configured buffer %.2fs), slideSpeed=%.0f uu/s"),
				Now - MeasureMarkB, GetSlideCooldownSeconds(), SlideSpeed);
			SetWantsToSlide(false);
			Advance(12);
		}
		else if (MeasurePhaseTime > 4.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE SLIDE-2 never started within 4s (planar=%.0f, buffer left %.2fs)"),
				PlanarSpeed, GetSlideCooldownRemaining());
			SetWantsToSlide(false);
			Advance(12);
		}
		break;

	case 12:
		if (MeasurePhaseTime > 2.f)
		{
			Advance(13);
		}
		break;

	// =============================================================================================
	// THE CHAIN: land fast -> slide -> jump. This is the sequence spec v3 §2.4 is really about, and
	// the one the old code broke in two places: the landing clamped to walk speed, and EndSlide()
	// clamped the exit to walk speed so jumping out of a fast slide threw the momentum away.
	// Everything above measures one transition at a time; this measures them composed.
	// =============================================================================================
	case 13:
		if (UpdatedComponent != nullptr && CharacterOwner != nullptr)
		{
			const FVector Here = UpdatedComponent->GetComponentLocation();
			CharacterOwner->SetActorLocation(FVector(0.f, 0.f, Here.Z + 1500.f), false, nullptr, ETeleportType::TeleportPhysics);
			Velocity = FVector(1200.f, 0.f, 0.f);
			SetMovementMode(MOVE_Falling);
			MeasureMarkA = 1200.f;
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE CHAIN: dropped at planar=1200 uu/s (ground limit %.0f)"), GetMaxSpeed());
			Advance(14);
		}
		break;

	case 14:
	{
		// Strafe up to something comfortably over the ground limit, then hold crouch so the input
		// buffer converts the landing into a slide on the touchdown frame.
		const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, TravelDirection).GetSafeNormal();
		CharacterOwner->AddMovementInput(Perpendicular, 1.f);
		if (MeasurePhaseTime > 0.35f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE CHAIN air speed before landing: %.0f uu/s"), PlanarSpeed);
			SetWantsToSlide(true);
			Advance(15);
		}
		break;
	}

	case 15:
		SetWantsToSlide(true);
		if (!IsMovingOnGround())
		{
			MeasureMarkA = PlanarSpeed;
		}
		else if (IsSliding())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE CHAIN jump->slide: landed at %.0f uu/s -> slideSpeed=%.0f uu/s (%.1f%%; ground limit %.0f)"),
				MeasureMarkA, SlideSpeed, 100.f * SlideSpeed / FMath::Max(1.f, MeasureMarkA), MaxWalkSpeed);
			Advance(16);
		}
		else if (MeasurePhaseTime > 3.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE CHAIN slide never latched (planar=%.0f)"), PlanarSpeed);
			Advance(18);
		}
		break;

	case 16:
		// Ride the slide down INTO its well-timed window and hop out of it, which is the Apex
		// slide-hop the spec is asking for. Waiting for IsSlideJumpWellTimed() rather than for a flat
		// 0.60s is what makes the number below a measurement of the mechanic rather than of a
		// stopwatch: with the shipped 0.20s window against a 1.8s slide, this fires at ~1.6s in.
		SetWantsToSlide(true);
		if (IsSlideJumpWellTimed() || MeasurePhaseTime > 3.0f)
		{
			MeasureMarkA = SlideSpeed;
			MeasureMarkB = IsSlideJumpWellTimed() ? 1.f : 0.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDEJUMP arming at slideSpeed=%.0f uu/s, %.2fs of slide left, wellTimed=%d"),
				SlideSpeed, SlideTimeRemaining, IsSlideJumpWellTimed() ? 1 : 0);
			CharacterOwner->Jump();
			Advance(17);
		}
		break;

	case 17:
		if (!IsMovingOnGround())
		{
			CharacterOwner->StopJumping();
			SetWantsToSlide(false);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE CHAIN slide->jump: slideSpeed was %.0f uu/s -> airborne at %.0f uu/s (%.1f%%), "
				     "wellTimed=%.0f, velZ=%.0f (jumpZ %.0f x %.2f). The old exit ceiling would have handed back %.0f."),
				MeasureMarkA, PlanarSpeed, 100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA),
				MeasureMarkB, Velocity.Z, JumpZVelocity, GetSlideJumpZMultiplier(), MaxWalkSpeed);
			Advance(18);
		}
		else
		{
			SetWantsToSlide(true);
			if (MeasurePhaseTime > 1.f)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("MEASURE CHAIN never left the ground"));
				Advance(18);
			}
		}
		break;

	case 18:
		SetWantsToSlide(false);
		if (IsMovingOnGround() && MeasurePhaseTime > 1.2f)
		{
			Advance(19);
		}
		break;

	// --- 19-21. DASH EXIT ---------------------------------------------------------------------------
	case 19:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		if (MeasurePhaseTime > 1.2f && IsMovingOnGround())
		{
			StartDash();
			Advance(20);
		}
		break;

	case 20:
		if (IsDashing())
		{
			MeasureMarkA = PlanarSpeed;
		}
		else if (MeasureMarkA > 0.f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE DASH exit: %.0f uu/s while dashing -> %.0f uu/s on the frame it ended "
				     "(ground limit %.0f x DashExitSpeedMultiplier %.2f = %.0f)"),
				MeasureMarkA, PlanarSpeed, GetMaxSpeed(), GetDashExitSpeedMultiplier(),
				GetMaxSpeed() * GetDashExitSpeedMultiplier());
			Advance(21);
		}
		else if (MeasurePhaseTime > 2.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE DASH never started"));
			Advance(22);
		}
		break;

	case 21:
		if (MeasurePhaseTime > 0.30f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE DASH exit +0.30s: planar=%.0f uu/s"), PlanarSpeed);
			Advance(22);
		}
		break;

	// =============================================================================================
	// 22-26. THE SLIDE-JUMP, ON CLEAN GROUND.
	//
	// The CHAIN phases above already jump out of a slide, but they do it at the middle of the arena
	// (they teleport to the world origin to isolate the drop) and the first run of this harness
	// measured a slide-jump there at 110% of the slide speed AT LAUNCH and 70% one physics step later.
	// The magnitude did not fall by 30% — the VECTOR turned about 50 degrees, which is
	// SlideAlongSurface deflecting the pawn off midfield cover. That is a measurement of the arena.
	//
	// So this repeats the move back at MeasureHomeLocation, on the strip phases 7-11 already crossed
	// without a single collision (RUN->JUMP measured exactly 100.0% there). Same reason
	// MeasureRunDirection exists.
	// =============================================================================================
	case 22:
		if (UpdatedComponent != nullptr && CharacterOwner != nullptr)
		{
			SetWantsToSlide(false);
			CharacterOwner->SetActorLocation(MeasureHomeLocation + FVector(0.f, 0.f, 40.f),
				false, nullptr, ETeleportType::TeleportPhysics);
			Velocity = FVector::ZeroVector;
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE SLIDEJUMP-CLEAN: returned to %s"),
				*MeasureHomeLocation.ToCompactString());
			Advance(23);
		}
		break;

	case 23:
		// Run up to speed on known-clear ground, then ask for the slide.
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		if (MeasurePhaseTime > 1.5f && IsMovingOnGround() && GetSlideCooldownRemaining() <= 0.f)
		{
			MeasureMarkA = PlanarSpeed;
			SetWantsToSlide(true);
			Advance(24);
		}
		break;

	case 24:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(true);
		if (IsSliding())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDEJUMP-CLEAN entry: %.0f uu/s in -> slideSpeed=%.0f uu/s (ratio %.2f)"),
				MeasureMarkA, SlideSpeed, SlideSpeed / FMath::Max(1.f, MeasureMarkA));
			Advance(25);
		}
		else if (MeasurePhaseTime > 2.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE SLIDEJUMP-CLEAN slide never latched (planar=%.0f)"), PlanarSpeed);
			Advance(27);
		}
		break;

	case 25:
		// Ride down into the well-timed window and hop. Input is HELD the whole way: without it the
		// first run of this phase let ground friction stop the pawn dead after the slide decayed out,
		// and then measured a slide-jump from a standstill (0 uu/s -> 0 uu/s), which is a measurement
		// of the harness. The bail-out fires the moment the slide-jump stops being available at all,
		// so a slide that ends without ever entering the window still produces a number.
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(true);
		if (IsSlideJumpWellTimed() || !IsSlideJumpAvailable() || MeasurePhaseTime > 3.f)
		{
			MeasureMarkA = FMath::Max(SlideSpeed, PlanarSpeed);
			MeasureMarkB = IsSlideJumpWellTimed() ? 1.f : 0.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDEJUMP-CLEAN arming at slideSpeed=%.0f planar=%.0f, timeLeft=%.2fs "
				     "(durationClock=%.2fs), wellTimed=%d, available=%d"),
				SlideSpeed, PlanarSpeed, GetSlideTimeLeft(), SlideTimeRemaining,
				IsSlideJumpWellTimed() ? 1 : 0, IsSlideJumpAvailable() ? 1 : 0);
			CharacterOwner->Jump();
			Advance(26);
		}
		break;

	case 26:
		if (!IsMovingOnGround())
		{
			CharacterOwner->StopJumping();
			SetWantsToSlide(false);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDEJUMP-CLEAN: slideSpeed was %.0f uu/s -> first airborne frame %.0f uu/s "
				     "(%.1f%%), wellTimed=%.0f, velZ=%.0f. Retention %.2f x windowBonus %.2f."),
				MeasureMarkA, PlanarSpeed, 100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA),
				MeasureMarkB, Velocity.Z, GetSlideJumpHorizontalRetention(),
				MeasureMarkB > 0.f ? GetSlideJumpWindowSpeedBonus() : 1.f);
			Advance(27);
		}
		else
		{
			SetWantsToSlide(true);
			if (MeasurePhaseTime > 1.f)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("MEASURE SLIDEJUMP-CLEAN never left the ground"));
				Advance(27);
			}
		}
		break;

	case 27:
		if (MeasurePhaseTime > 0.30f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE SLIDEJUMP-CLEAN +0.30s airborne: planar=%.0f uu/s"), PlanarSpeed);
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE ---- end"));
			Advance(28);
		}
		break;

	default:
		break;
	}
}

#endif // !UE_BUILD_SHIPPING

// -------------------------------------------------------------------------------------------
// FSavedMove_Trace
// -------------------------------------------------------------------------------------------

FSavedMove_Trace::FSavedMove_Trace()
	: bSavedWantsToDash(0)
	, bSavedWantsToSlide(0)
	, bSavedMomentumActive(0)
	, SavedDashTimeRemaining(0.f)
	, SavedDashRechargeRemaining(0.f)
	, SavedDashCharges(0)
	, SavedLastMaxDashCharges(0)
	, SavedDashDirection(FVector::ZeroVector)
	, SavedSlideTimeRemaining(0.f)
	, SavedSlideCooldownRemaining(0.f)
	, SavedSlideCommitRemaining(0.f)
	, SavedSlideSpeed(0.f)
	, SavedSlideBufferRemaining(0.f)
	, SavedSlideDirection(FVector::ZeroVector)
	, bSavedSlideHeldLastMove(0)
	, bSavedWasAirborneLastMove(0)
	, SavedSlideJumpGraceRemaining(0.f)
	, bSavedSlideJumpGraceWellTimed(0)
{
}

void FSavedMove_Trace::Clear()
{
	Super::Clear();

	// Saved moves are pooled and recycled — every added field must be reset or a stale ability will
	// resurrect itself several moves later.
	bSavedWantsToDash = 0;
	bSavedWantsToSlide = 0;
	bSavedMomentumActive = 0;

	SavedDashTimeRemaining = 0.f;
	SavedDashRechargeRemaining = 0.f;
	SavedDashCharges = 0;
	SavedLastMaxDashCharges = 0;
	SavedDashDirection = FVector::ZeroVector;

	SavedSlideTimeRemaining = 0.f;
	SavedSlideCooldownRemaining = 0.f;
	SavedSlideCommitRemaining = 0.f;
	SavedSlideSpeed = 0.f;
	SavedSlideBufferRemaining = 0.f;
	SavedSlideDirection = FVector::ZeroVector;
	bSavedSlideHeldLastMove = 0;
	bSavedWasAirborneLastMove = 0;
	SavedSlideJumpGraceRemaining = 0.f;
	bSavedSlideJumpGraceWellTimed = 0;
}

uint8 FSavedMove_Trace::GetCompressedFlags() const
{
	uint8 Result = Super::GetCompressedFlags();

	if (bSavedWantsToDash)
	{
		Result |= FLAG_Custom_0;
	}
	// FLAG_Custom_1 is FREE. It was boost; spec v3 §1 deleted the ability.
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
		|| bSavedWantsToSlide != Other->bSavedWantsToSlide)
	{
		return false;
	}

	// Never merge across an active dash or slide. Combining replays one longer move from the older
	// move's start state; the clocks are linear in dt so the maths would survive, but the frame on
	// which the ability *ends* would move, and with it the velocity profile.
	//
	// The commit window is included for the same reason: it decides whether a released key ends the
	// slide, so a merged move that straddled its expiry would resolve the release differently from
	// the two moves it replaced.
	// The slide-jump's coyote window is in the same list and for the same reason: it decides whether
	// a jump is a slide-jump at all, so a merged move straddling its expiry would resolve the jump
	// differently from the two moves it replaced — and the difference is the whole carry.
	if (SavedDashTimeRemaining > 0.f || Other->SavedDashTimeRemaining > 0.f
		|| SavedSlideTimeRemaining > 0.f || Other->SavedSlideTimeRemaining > 0.f
		|| SavedSlideCommitRemaining > 0.f || Other->SavedSlideCommitRemaining > 0.f
		|| SavedSlideJumpGraceRemaining > 0.f || Other->SavedSlideJumpGraceRemaining > 0.f)
	{
		return false;
	}

	// ...and never merge across the momentum model. Both of its branches clamp PER SUB-STEP —
	// min(AirAcceleration x dt, AddSpeed) in the air, max(GroundLimit, Speed - Bleed x dt) on the
	// ground — so f(2dt) != f(dt) twice whenever a clamp binds. A merged move would hand the server
	// a trajectory the client never simulated, and the correction would arrive as a rubber-band in
	// the exact situation (mid-strafe, mid-landing) where it is most visible.
	if (bSavedMomentumActive || Other->bSavedMomentumActive)
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
			bSavedWantsToSlide = Movement->bWantsToSlide;

			// Not CMC state and deliberately not restored by PrepMoveFor: this is a property OF the
			// move, read only by CanCombineWith. See the field's comment.
			bSavedMomentumActive = (Movement->IsFalling() || Movement->IsCarryingExcessSpeed()) ? 1 : 0;

			SavedDashTimeRemaining     = Movement->DashTimeRemaining;
			SavedDashRechargeRemaining = Movement->DashRechargeRemaining;
			SavedDashCharges           = Movement->DashCharges;
			SavedLastMaxDashCharges    = Movement->LastMaxDashCharges;
			SavedDashDirection         = Movement->DashDirection;

			SavedSlideTimeRemaining     = Movement->SlideTimeRemaining;
			SavedSlideCooldownRemaining = Movement->SlideCooldownRemaining;
			SavedSlideCommitRemaining   = Movement->SlideCommitRemaining;
			SavedSlideSpeed             = Movement->SlideSpeed;
			SavedSlideBufferRemaining   = Movement->SlideBufferRemaining;
			SavedSlideDirection         = Movement->SlideDirection;
			bSavedSlideHeldLastMove     = Movement->bSlideHeldLastMove;
			bSavedWasAirborneLastMove   = Movement->bWasAirborneLastMove;

			SavedSlideJumpGraceRemaining  = Movement->SlideJumpGraceRemaining;
			bSavedSlideJumpGraceWellTimed = Movement->bSlideJumpGraceWellTimed;
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
			// will overwrite the two intent flags from the compressed flags immediately after this
			// returns; restoring them too costs nothing and keeps the snapshot complete.
			//
			// The momentum model needs nothing here: it is a pure function of Velocity and
			// Acceleration, both of which Super::PrepMoveFor and MoveAutonomous already restore.
			Movement->bWantsToDash  = bSavedWantsToDash;
			Movement->bWantsToSlide = bSavedWantsToSlide;

			Movement->DashTimeRemaining     = SavedDashTimeRemaining;
			Movement->DashRechargeRemaining = SavedDashRechargeRemaining;
			Movement->DashCharges           = SavedDashCharges;
			Movement->LastMaxDashCharges    = SavedLastMaxDashCharges;
			Movement->DashDirection         = SavedDashDirection;

			Movement->SlideTimeRemaining     = SavedSlideTimeRemaining;
			Movement->SlideCooldownRemaining = SavedSlideCooldownRemaining;
			Movement->SlideCommitRemaining   = SavedSlideCommitRemaining;
			Movement->SlideSpeed             = SavedSlideSpeed;
			Movement->SlideBufferRemaining   = SavedSlideBufferRemaining;
			Movement->SlideDirection         = SavedSlideDirection;
			Movement->bSlideHeldLastMove     = bSavedSlideHeldLastMove;
			Movement->bWasAirborneLastMove   = bSavedWasAirborneLastMove;

			// The slide-jump window. Without this a correction that landed mid-window would replay a
			// slide-jump as an ordinary jump, and client and server would disagree about several
			// hundred uu/s of horizontal velocity on the most visible frame in the kit.
			Movement->SlideJumpGraceRemaining  = SavedSlideJumpGraceRemaining;
			Movement->bSlideJumpGraceWellTimed = bSavedSlideJumpGraceWellTimed;
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
