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

#include "Components/CapsuleComponent.h"        // mantle: capsule dimensions and the clearance sweep
#include "Components/SceneComponent.h"
#include "CollisionQueryParams.h"
#include "Core/TraceCharacter.h"
#include "Engine/World.h"                      // UWorld::GetTimeSeconds (dash-active latch), traces
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"          // spec v7 §5: the aim rotation the dash is composed from
#include "Math/RotationMatrix.h"               // spec v7 §5: the yaw basis the input is decomposed in
#include "Math/UnrealMathUtility.h"
#include "Trace.h"                             // LogTraceGame
#include "TraceSettings.h"
#include "UObject/UnrealType.h"                // FProperty, for the name-bound spec v5 knobs

#if !UE_BUILD_SHIPPING
#include "Components/StaticMeshComponent.h"    // ledge test: the block it builds for itself
#include "Engine/Engine.h"
#include "EngineUtils.h"                       // TActorIterator, for the spec v8 §5 carrier harness
#include "Gameplay/TraceCore.h"                // spec v8 §5: the real pickup funnel, ATraceCore::TryPickup
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
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

// =================================================================================================
// THE SPEC v5 KNOBS, AND WHY THEY ARE BOUND BY NAME
// =================================================================================================
//
// Spec v5 §1, §3 and §7 need eighteen new tunables. UTraceSettings belongs to another ownership
// slice this pass, so the UPROPERTYs cannot be declared from here — and shipping the numbers as
// hardcoded literals would mean shipping a movement pass with nothing to tune, which is worse.
//
// So each knob is resolved ONCE, by name, against UTraceSettings' UClass, and falls back to the
// default written at the call site if the property does not exist yet. The instant the integrator
// adds `float AirStrafeSoftCapSpeed = 950.f;` (and its ini line) the binding takes over, with no
// change here and no rebuild of this file's behaviour.
//
// THIS PROJECT HAS BEEN BITTEN BY EXACTLY THE OPPOSITE ARRANGEMENT: five of eight mode-B knobs were
// dead last pass because a name-bound lookup silently missed and nobody could tell. That is why
// TraceMoveKnob::LogBindReport() prints, once per process at Display, every knob's name, the value
// actually in force and whether it came from UTraceSettings or from the fallback. "BOUND" means the
// ini can drive it; "default" means it cannot, and the report says so in as many words.
//
// Game thread only (movement is), and the cache is keyed on the property name, so the cost after the
// first move is one TMap lookup per knob per read — the same order as the UTraceSettings::Get()
// reads every other accessor in this file already does.

namespace TraceMoveKnob
{
	struct FBinding
	{
		const FProperty* Property = nullptr;
		bool bResolved = false;
	};

	/** Name -> resolved property (or a resolved miss). Never invalidated: UClass layout is static. */
	static TMap<FName, FBinding>& Bindings()
	{
		static TMap<FName, FBinding> Map;
		return Map;
	}

	/** Recorded purely so the bind report can name the knobs that fell back. */
	static TMap<FName, bool>& BindReport()
	{
		static TMap<FName, bool> Map;
		return Map;
	}

	static const FProperty* Resolve(const FName Name)
	{
		FBinding& Binding = Bindings().FindOrAdd(Name);
		if (!Binding.bResolved)
		{
			Binding.bResolved = true;
			Binding.Property = UTraceSettings::StaticClass()->FindPropertyByName(Name);
			BindReport().Add(Name, Binding.Property != nullptr);
		}
		return Binding.Property;
	}

	static float Float(const FName Name, const float Default)
	{
		if (const FProperty* Property = Resolve(Name))
		{
			if (const FFloatProperty* AsFloat = CastField<FFloatProperty>(Property))
			{
				return AsFloat->GetPropertyValue_InContainer(&UTraceSettings::Get());
			}
			if (const FDoubleProperty* AsDouble = CastField<FDoubleProperty>(Property))
			{
				return static_cast<float>(AsDouble->GetPropertyValue_InContainer(&UTraceSettings::Get()));
			}
		}
		return Default;
	}

	static int32 Int(const FName Name, const int32 Default)
	{
		if (const FProperty* Property = Resolve(Name))
		{
			if (const FIntProperty* AsInt = CastField<FIntProperty>(Property))
			{
				return AsInt->GetPropertyValue_InContainer(&UTraceSettings::Get());
			}
		}
		return Default;
	}

	static bool Bool(const FName Name, const bool bDefault)
	{
		if (const FProperty* Property = Resolve(Name))
		{
			if (const FBoolProperty* AsBool = CastField<FBoolProperty>(Property))
			{
				return AsBool->GetPropertyValue_InContainer(&UTraceSettings::Get());
			}
		}
		return bDefault;
	}
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

/**
 * Mantle instrumentation (spec v5 §7). Same rules as the slide's: off by default, Display when on.
 * Declared up here rather than beside the other dev code at the bottom because TryBeginMantle() and
 * EndMantle() sit in the middle of the file and are the two places worth watching.
 */
int32 GTraceMantleDebug = 0;
static FAutoConsoleVariableRef CVarTraceMantleDebug(
	TEXT("Trace.MantleDebug"),
	GTraceMantleDebug,
	TEXT("Dev only. Non-zero logs every mantle: ledge height, reach, duration and the exit speed."),
	ECVF_Cheat);

/**
 * Dash instrumentation (spec v7 §5). Same rules as the slide's and the mantle's: off by default,
 * Display when on.
 *
 * This exists because Trace.DashVectorTest measures the PURE function and nothing else. It cannot
 * see whether a grounded upward dash actually reaches MOVE_Falling, whether the per-frame re-assert
 * holds Z on rails, or whether the exit clamp fires — all three of which are runtime behaviour, and
 * two of which are new. A headless match full of bots produces hundreds of real dashes; this turns
 * them into the evidence.
 */
int32 GTraceDashDebug = 0;
static FAutoConsoleVariableRef CVarTraceDashDebug(
	TEXT("Trace.DashDebug"),
	GTraceDashDebug,
	TEXT("Dev only. Non-zero logs every dash's composed direction, launch velocity, movement mode "
	     "and exit velocity at Display, so the vertical dash can be measured in a live match."),
	ECVF_Cheat);

static bool IsDashDebugEnabled()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceDashDebug"));
	return bFromCommandLine || GTraceDashDebug != 0;
}

static bool IsMantleDebugEnabled()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceMantleDebug"));
	return bFromCommandLine || GTraceMantleDebug != 0;
}

/** Mantles observed process-wide, so a headless run can answer "did it ever fire" with a count. */
static int32 GTraceMantleCount = 0;

/**
 * Forward declaration. IsDashPoolDebugEnabled() is defined further down, next to the dash-charge
 * pool it reports on, but it is first CALLED from the movement-update path well above that point.
 * C++ needs the declaration before the call; without it the translation unit fails outright with
 * "use of undeclared identifier", which takes the whole module — and every agent's run rig — with it.
 * Same shape as IsDashDebugEnabled() / IsMantleDebugEnabled() above.
 */
static bool IsDashPoolDebugEnabled();
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
	ReplayAimRotation = FRotator::ZeroRotator;
	bReplayAimRotationValid = 0;

	SlideTimeRemaining = 0.f;
	SlideCooldownRemaining = 0.f;
	SlideSpeed = 0.f;
	SlideDirection = FVector::ZeroVector;
	SlideBufferRemaining = 0.f;
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;
	bSlideHeldLastMove = 0;
	bWasAirborneLastMove = 0;

	GroundGraceRemaining = 0.f;
	MantleTimeRemaining = 0.f;
	MantleTotalTime = 0.f;
	MantleTargetLocation = FVector::ZeroVector;
	MantleUpTargetZ = 0.f;
	MantleEntrySpeed = 0.f;
	MantleCooldownRemaining = 0.f;

	// Spec v8 §7. Saved-move state like everything above it.
	WallJumpNormal = FVector::ZeroVector;
	WallJumpWindowRemaining = 0.f;
	WallJumpEntryVelocity = FVector::ZeroVector;
	WallJumpsSinceGround = 0;

#if !UE_BUILD_SHIPPING
	bLedgeTestWasGrounded = 0;

	// Spec v7 §5's harness. Bitfields get no in-class initialiser here for the same reason the ledge
	// test's does not: every other one in this component is set in the constructor, and a mixed
	// convention is how one of them ends up uninitialised.
	bDashPitchTestFired = 0;
	bDashPitchTestLogged = 0;
#endif

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

	// --- LEDGE STABILITY, spec v5 §7 --------------------------------------------------------------
	//
	// FIX 1 OF 3 FOR THE "RUBBER BANDING ON THE EDGE OF A RAISED SECTION" REPORT, and the only one
	// that is a straight engine setting. See the header for the full diagnosis.
	//
	// PerchRadiusThreshold ships at 0, which disables the reduced-radius perch test entirely: a pawn
	// counts as WALKING while any part of its capsule's bottom hemisphere touches the lip, i.e. while
	// it is balanced on a fraction of a uu. Whether that sweep catches or misses is decided by
	// sub-uu geometry, and the client and the server evaluate it on DIFFERENT sub-steps of the same
	// second, so they take the coin flip independently. Each disagreement puts them on opposite sides
	// of this component's air-model / ground-model split (see CalcVelocity), which is worth hundreds
	// of uu/s rather than the fraction of a uu the geometry actually differed by.
	//
	// 15 uu gives the decision a band instead of an edge. The capsule radius is 42, so a pawn is now
	// "perched" (and falls) once it has less than 15 uu of support, and solidly walking above that —
	// a state both machines reach from the same geometry several frames before it matters.
	//
	// Deliberately NOT bUseFlatBaseForFloorChecks: that changes floor detection everywhere, including
	// on the arena's 40 uu step risers, and this fix has to be surgical.
	PerchRadiusThreshold = 15.f;
	PerchAdditionalHeight = 40.f;

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

	// Spec v8 §7 needs ACharacter::JumpMaxCount raised before the first move, not after it — see the
	// note in RefreshEngineTunablesFromSettings, which keeps it live from then on.
	RefreshEngineTunablesFromSettings();

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
				     "exitRet=%.2f exitCeil=%.2f (ONE-SHOT - spec v5 s3; NO impulse, NO exit floor, NO commit) "
				     "| SLIDEJUMP on=%d retain=%.2f zMul=%.2f window=%.2f windowBonus=%.2f "
				     "| DASH speed=%.0f dur=%.2f cd=%.2f"),
				MaxWalkSpeed,
				IsSourceAirAccelerationEnabled() ? 1 : 0, GetAirAcceleration(), GetAirMaxWishSpeed(), GetMaxAirSpeed(),
				IsLandingMomentumPreserved() ? 1 : 0, GetGroundOverspeedFriction(), GetGroundOverspeedBraking(),
				GetGroundOverspeedTurnRate(), GetDashExitSpeedMultiplier(),
				GetSlideDuration(), GetSlideEntrySpeedMultiplier(), GetSlideCooldownSeconds(),
				Settings.SlideMaxSpeed, GetSlideDeceleration(),
				GetSlideExitSpeedRetention(), GetSlideExitMaxSpeedMultiplier(),
				IsSlideJumpEnabled() ? 1 : 0, GetSlideJumpHorizontalRetention(), GetSlideJumpZMultiplier(),
				GetSlideJumpWindowSeconds(), GetSlideJumpWindowSpeedBonus(),
				GetDashSpeed(), GetDashDuration(), GetDashCooldown());

			// --- SPEC v5, and the bind report ----------------------------------------------------
			//
			// Reading every new knob here is what POPULATES TraceMoveKnob::BindReport(), so the
			// listing below is complete by construction: a knob that is never read cannot be missing
			// from the report, because it is not a knob this file uses.
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG-V5 AIRSTRAFE falloff=%d soft=%.0f hard=%.0f exp=%.2f hardCapOn=%d "
				     "| SLIDE oneShot=1 dur=%.2f hiddenCooldown=%.2f windowBonus=%.2f zBonus=%.2f "
				     "| MANTLE on=%d reach=%.0f minH=%.0f maxH=%.0f dur=%.2f upFrac=%.2f cd=%.2f minSpd=%.0f "
				     "| LEDGE grace=%.3f perchThreshold=%.1f"),
				IsAirStrafeFalloffEnabled() ? 1 : 0, GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(),
				GetAirStrafeFalloffExponent(), IsAirStrafeHardCapEnabled() ? 1 : 0,
				GetSlideDuration(), GetSlideCooldownSeconds(), GetSlideJumpWindowSpeedBonus(),
				GetSlideJumpWindowZBonus(),
				IsMantleEnabled() ? 1 : 0, GetMantleReachUU(), GetMantleMinHeightUU(), GetMantleMaxHeightUU(),
				GetMantleDurationSeconds(), GetMantleUpPhaseFraction(), GetMantleCooldownSeconds(),
				GetMantleMinForwardSpeed(), GetLedgeGroundGraceSeconds(), PerchRadiusThreshold);

			// The knob hygiene check the project's own history demands. A name-bound knob that does
			// not resolve is not a build error and not a runtime error — it is a setting that
			// silently does nothing, which is how five of eight knobs died last pass. Every one of
			// them is named here, every pass, with the word BOUND or the word FALLBACK next to it.
			int32 BoundCount = 0;
			int32 FallbackCount = 0;
			for (const TPair<FName, bool>& Entry : TraceMoveKnob::BindReport())
			{
				UE_LOG(LogTraceGame, Display, TEXT("MOVEKNOB %-28s %s"),
					*Entry.Key.ToString(),
					Entry.Value ? TEXT("BOUND to UTraceSettings (ini-tunable)")
					            : TEXT("FALLBACK to the built-in default (property missing -> ini CANNOT tune it)"));
				(Entry.Value ? BoundCount : FallbackCount)++;
			}
			UE_LOG(LogTraceGame, Display, TEXT("MOVEKNOB summary: %d bound, %d on built-in defaults"),
				BoundCount, FallbackCount);
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

float UTraceCharacterMovementComponent::GetDashExitVerticalSpeedLimit() const
{
	// SPEC v7 §5. See the header note "THE CLIMB". JumpZVelocity rather than a literal so that the
	// one number the whole kit expresses vertical launches in still governs this one, and so that
	// raising the jump raises the dash's ceiling with it instead of silently capping it lower.
	// INTEGRATED: the designer knob asked for in the v7 report now exists, as a MULTIPLE of the jump
	// rather than an absolute, so the tie to JumpZVelocity survives retuning.
	return FMath::Max(0.f, JumpZVelocity * FMath::Max(0.f, UTraceSettings::Get().DashExitVerticalSpeedMultiplier));
}

FRotator UTraceCharacterMovementComponent::GetDashAimRotation() const
{
	// SPEC v7 §5, AND THE ONLY REASON A VERTICAL DASH DOES NOT DESYNC.
	//
	// bClientUpdating is set for exactly the span of ClientUpdatePositionAfterServerUpdate's replay
	// loop, and inside that loop every move's PrepMoveFor has just written ReplayAimRotation. So the
	// gate is both necessary (a live move must use the live mouse) and sufficient (a replayed move
	// can never read a stale rotation, because one was written microseconds earlier).
	if (bReplayAimRotationValid != 0 && CharacterOwner != nullptr && CharacterOwner->bClientUpdating)
	{
		return ReplayAimRotation;
	}

	// The authority's own path and the owning client's original simulation. On the server this is
	// the rotation FCharacterNetworkMoveData delivered and ServerMove_PerformMovement applied to the
	// controller immediately before MoveAutonomous, so it matches what the client had.
	if (CharacterOwner != nullptr)
	{
		if (const AController* OwningController = CharacterOwner->GetController())
		{
			return OwningController->GetControlRotation();
		}
	}

	// No controller at all (a detached or dying pawn). The capsule's own rotation is level under
	// bOrientRotationToMovement, which degrades this to exactly the old horizontal dash.
	return (UpdatedComponent != nullptr) ? UpdatedComponent->GetComponentRotation() : FRotator::ZeroRotator;
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

// GetSlideMinCommitSeconds() WAS HERE AND IS DELETED (spec v5 §3). It read the window in which
// releasing crouch could not cancel a slide. A one-shot ability has no partial commit — the whole
// slide is committed the moment it starts — so the knob has nothing left to mean. Delete
// UTraceSettings::SlideMinCommitSeconds and its DefaultGame.ini line with it; a setting that is read
// nowhere is exactly the "silently dead knob" this project keeps getting caught by.

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
	//
	// SPEC v5 §3 RAISES THIS FROM 1.10 TO 1.25. The property is UTraceSettings::
	// SlideJumpWindowSpeedBonus and the shipped value lives in DefaultGame.ini, which wins over the
	// header default — both have to move or the retune does nothing. 1.10 was a 10% edge on a 0.9s
	// arc, which is roughly the frame-to-frame noise a player sees anyway; with the hold-to-extend
	// gone this is the only skill expression sliding has left, so it has to be legible.
	return FMath::Max(1.f, UTraceSettings::Get().SlideJumpWindowSpeedBonus);
}

float UTraceCharacterMovementComponent::GetSlideJumpWindowZBonus() const
{
	// New in spec v5 §3. Height is the channel a player can actually SEE — 25% more planar speed is
	// deniable, clearing a box you could not clear a second ago is not.
	return FMath::Max(1.f, TraceMoveKnob::Float(TEXT("SlideJumpWindowZBonus"), 1.12f));
}

// --- The air-strafe accumulation ceiling (spec v5 §1) -------------------------------------------

bool UTraceCharacterMovementComponent::IsAirStrafeFalloffEnabled() const
{
	return TraceMoveKnob::Bool(TEXT("bAirStrafeGainFalloff"), true);
}

float UTraceCharacterMovementComponent::GetAirStrafeSoftCapSpeed() const
{
	// 950 = 1.19 x the 800 walk speed. Below it a strafe is worth EXACTLY what it was in Demo 5,
	// which is the part the user called incredible and asked not to be touched.
	return FMath::Max(0.f, TraceMoveKnob::Float(TEXT("AirStrafeSoftCapSpeed"), 950.f));
}

float UTraceCharacterMovementComponent::GetAirStrafeHardCapSpeed() const
{
	// Always strictly above the soft cap: the falloff divides by (Hard - Soft), and a designer who
	// set them equal would otherwise get a divide-by-zero rather than the "cap everything at the soft
	// cap" they obviously meant.
	return FMath::Max(GetAirStrafeSoftCapSpeed() + 1.f,
		TraceMoveKnob::Float(TEXT("AirStrafeHardCapSpeed"), 1250.f));
}

float UTraceCharacterMovementComponent::GetAirStrafeFalloffExponent() const
{
	// 1 is a linear taper. 2 (shipped) keeps most of the strafe's value until well past the soft cap
	// and then collapses it, which is what "harder and harder past a certain point" describes.
	// Floored at a small positive number rather than 0, because Pow(x, 0) == 1 would silently turn
	// the falloff into a no-op that still reported itself as enabled.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("AirStrafeFalloffExponent"), 2.f), 0.05f, 16.f);
}

bool UTraceCharacterMovementComponent::IsAirStrafeHardCapEnabled() const
{
	return TraceMoveKnob::Bool(TEXT("bAirStrafeHardCap"), true);
}

float UTraceCharacterMovementComponent::GetAirStrafeGainScale(const float PlanarSpeed) const
{
	if (!IsAirStrafeFalloffEnabled())
	{
		return 1.f;
	}

	const float Soft = GetAirStrafeSoftCapSpeed();
	if (PlanarSpeed <= Soft)
	{
		return 1.f;
	}

	const float Hard = GetAirStrafeHardCapSpeed();
	if (PlanarSpeed >= Hard)
	{
		return 0.f;
	}

	// Headroom left, as a fraction of the whole falloff band, raised to the exponent. Pure function
	// of (PlanarSpeed, config) — no state, no time, so a replayed frame lands on the identical value.
	const float Headroom = (Hard - PlanarSpeed) / (Hard - Soft);
	return FMath::Pow(FMath::Clamp(Headroom, 0.f, 1.f), GetAirStrafeFalloffExponent());
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

// --- Mantle / ledge tuning (spec v5 §7) ---------------------------------------------------------

bool UTraceCharacterMovementComponent::IsMantleEnabled() const
{
	return TraceMoveKnob::Bool(TEXT("bMantleEnabled"), true);
}

float UTraceCharacterMovementComponent::GetMantleReachUU() const
{
	// Measured from the capsule's SURFACE, not its centre — a reach expressed from the centre would
	// silently change meaning if the capsule radius ever moved.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("MantleReachUU"), 70.f), 10.f, 400.f);
}

float UTraceCharacterMovementComponent::GetMantleMinHeightUU() const
{
	// Under this the engine's own step-up already carries the pawn over, and a mantle would read as a
	// stutter on a kerb. MaxStepHeight is 45 and the arena's risers are 40, so 55 leaves both alone.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("MantleMinHeightUU"), 55.f), 0.f, 400.f);
}

float UTraceCharacterMovementComponent::GetMantleMaxHeightUU() const
{
	// Hip-to-shoulder, plus what a jump adds. The capsule is 88 uu half height (176 full), and the
	// arena's smallest raised sections are exactly one player height — the ones the report is about.
	// A plain jump reaches 640^2/(2*980) = 209 uu, so 230 covers "I jumped at it and just clipped the
	// edge" without turning the 352 uu structures into free climbing.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("MantleMaxHeightUU"), 230.f),
		GetMantleMinHeightUU() + 1.f, 1000.f);
}

float UTraceCharacterMovementComponent::GetMantleDurationSeconds() const
{
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("MantleDurationSeconds"), 0.35f), 0.05f, 2.f);
}

float UTraceCharacterMovementComponent::GetMantleUpPhaseFraction() const
{
	// Never 0 and never 1: the climb has to finish before the pawn crosses the lip (or it walks into
	// the wall face) and the crossing has to have time left (or it ends hanging in the air over it).
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("MantleUpPhaseFraction"), 0.6f), 0.1f, 0.9f);
}

float UTraceCharacterMovementComponent::GetMantleCooldownSeconds() const
{
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("MantleCooldownSeconds"), 0.35f), 0.f, 10.f);
}

float UTraceCharacterMovementComponent::GetMantleMinForwardSpeed() const
{
	return FMath::Max(0.f, TraceMoveKnob::Float(TEXT("MantleMinForwardSpeed"), 120.f));
}

float UTraceCharacterMovementComponent::GetLedgeGroundGraceSeconds() const
{
	// 0.08s is about five frames at 60Hz — long enough to swallow the one- or two-frame contact blip
	// a capsule takes crossing a lip, far too short to let a pawn slide off a roof and keep sliding.
	// Setting it to 0 restores the Demo 5 behaviour exactly, which is what the desync was measured
	// against.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("LedgeGroundGraceSeconds"), 0.08f), 0.f, 0.5f);
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

	// --- SPEC v8 §7: BUY THE QUESTION, NOT THE JUMP ----------------------------------------------
	//
	// ACharacter::CheckJumpInput will not call DoJump() at all once JumpCurrentCount has reached
	// JumpMaxCount, and stepping off a ledge or jumping once already spends the only count there is.
	// With the stock JumpMaxCount of 1 the wall jump was unreachable no matter what DoJump did — the
	// engine simply never asked.
	//
	// So the count is raised to 1 + WallJumpMaxConsecutive. That does NOT grant a double jump:
	// DoJump() returns false for every mid-air press that is not a genuine wall jump, and a refused
	// DoJump leaves JumpCurrentCount exactly where it was. What the extra counts buy is the engine
	// asking us the question on each press; TryWallJump() answers it.
	//
	// Prediction-safe: JumpMaxCount is already saved-move state (FSavedMove_Character captures and
	// restores it), both ends derive the same number from the same config, and this runs once per
	// simulated move on every machine, exactly like the two engine fields above.
	if (CharacterOwner != nullptr)
	{
		const int32 DesiredJumpMaxCount = IsWallJumpEnabled() ? (1 + GetWallJumpMaxConsecutive()) : 1;
		if (CharacterOwner->JumpMaxCount != DesiredJumpMaxCount)
		{
			CharacterOwner->JumpMaxCount = DesiredJumpMaxCount;
		}
	}
}

// --- Wall-jump tuning (spec v8 §7) --------------------------------------------------------------
//
// Name-bound against UTraceSettings for the reason the spec v5 knobs are: the UPROPERTYs live in a
// file this slice does not own. Every default below is the shipped value, and BeginPlay's MOVEKNOB
// report says BOUND or FALLBACK for each one so a missing property can never be silent.

bool UTraceCharacterMovementComponent::IsWallJumpEnabled() const
{
	return TraceMoveKnob::Bool(TEXT("bWallJumpEnabled"), true);
}

float UTraceCharacterMovementComponent::GetWallJumpWindowSeconds() const
{
	// "press jump right as they hit a wall". 0.25s is about four frames of slack at 60Hz plus the
	// human reaction floor — long enough to be hittable, short enough that it is a reaction to the
	// contact rather than a state you live in. Clamped so a bad ini cannot make it permanent.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpWindowSeconds"), 0.25f), 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetWallJumpSpeedRetention() const
{
	// The "carry momentum in a new direction" dial. 0.95 rather than 1.0 so a wall is very slightly
	// lossy — otherwise a corridor is a frictionless pinball table — but nowhere near the reset the
	// request is complaining about. Capped at 1: a wall must never MANUFACTURE speed, which is the
	// same rule spec v4 §1 imposed on the slide.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpSpeedRetention"), 0.95f), 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetWallJumpOutwardImpulse() const
{
	// Flat push along the normal, on top of the reflection. Without it a player who slid down a wall
	// with almost no planar speed would wall-jump straight back into the wall on the next frame and
	// the mechanic would read as broken; with it, even a standing wall jump clears the face.
	return FMath::Max(0.f, TraceMoveKnob::Float(TEXT("WallJumpOutwardImpulse"), 420.f));
}

float UTraceCharacterMovementComponent::GetWallJumpVerticalMultiplier() const
{
	// A multiple of JumpZVelocity, like every other vertical launch in the kit (see
	// GetDashExitVerticalSpeedLimit). 1.05 makes a wall jump read as very slightly stronger than a
	// standing jump, which is what sells it as a distinct move.
	return FMath::Max(0.f, TraceMoveKnob::Float(TEXT("WallJumpVerticalMultiplier"), 1.05f));
}

int32 UTraceCharacterMovementComponent::GetWallJumpMaxConsecutive() const
{
	// THE ANTI-LADDER CAP. Two parallel walls three metres apart are an infinite staircase without it.
	// Floored at 1 (a cap of zero would be "the feature is off", which is what bWallJumpEnabled is
	// for) and ceilinged at 4 so that the JumpMaxCount this drives stays sane.
	return FMath::Clamp(TraceMoveKnob::Int(TEXT("WallJumpMaxConsecutive"), 2), 1, 4);
}

float UTraceCharacterMovementComponent::GetWallJumpMaxNormalZ() const
{
	// |Normal.Z| below this is a wall. GetWalkableFloorZ() is 0.71 at the default 45 degrees, so 0.4
	// leaves a clear band between "wall" and "slope you could have walked up" and stops a ramp from
	// being a trampoline.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpMaxNormalZ"), 0.4f), 0.f, 0.9f);
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

bool UTraceCharacterMovementComponent::RefundDashCharge()
{
	// SPEC v7 §6. The header documents the three cases; this is them, in order.
	if (CharacterOwner == nullptr || !CharacterOwner->HasAuthority())
	{
		return false;
	}

	const int32 MaxCharges = GetMaxDashCharges();
	if (DashCharges >= MaxCharges)
	{
		// "If both are already available, nothing to do." Not an error, and not worth an RPC.
		return false;
	}

	DashCharges = FMath::Min(DashCharges + 1, MaxCharges);

	// Only a FULL pool clears the clock. A carrier who had spent BOTH charges keeps the timer that
	// was already running for the second — the spec refunds one dash, not both. Leaving the clock
	// alone is also what makes this composable with the refill in TickComponent, which restarts the
	// window only when it finds the pool short with no clock running.
	if (DashCharges >= MaxCharges)
	{
		DashRechargeRemaining = 0.f;
	}

	ClientRefundDashCharge();
	return true;
}

void UTraceCharacterMovementComponent::ClientRefundDashCharge_Implementation()
{
	// The listen host already ran RefundDashCharge on this very component; applying it twice here
	// would hand back two charges for one parry kill.
	if (CharacterOwner == nullptr || CharacterOwner->HasAuthority())
	{
		return;
	}

	const int32 MaxCharges = GetMaxDashCharges();
	if (DashCharges >= MaxCharges)
	{
		return;
	}

	DashCharges = FMath::Min(DashCharges + 1, MaxCharges);
	if (DashCharges >= MaxCharges)
	{
		DashRechargeRemaining = 0.f;
	}
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
		// --- MANTLE (spec v5 §7) -----------------------------------------------------------------
		//
		// First, and unconditional: the pull-up owns the velocity vector outright for its whole
		// window, exactly as the dash owns it for the dash's. Written HERE rather than in
		// OnMovementUpdated for the reason at the top of this function — PhysFlying reads Velocity
		// immediately after CalcVelocity returns, so this is the only place a mantle can move the
		// pawn on the frame it was computed.
		if (MantleTimeRemaining > 0.f)
		{
			ApplyMantleVelocity(DeltaTime);
			return;
		}

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

	const float NewSpeed = NewPlanar.Size();
	if (NewSpeed <= UE_KINDA_SMALL_NUMBER)
	{
		Velocity.X = NewPlanar.X;
		Velocity.Y = NewPlanar.Y;
		return;
	}

	// =============================================================================================
	// SPEC v5 §1 — THE ACCUMULATION CEILING. EVERYTHING ABOVE THIS LINE IS UNCHANGED.
	//
	// "The air strafing feels incredible, but its too powerful with how much momentum can be gained."
	//
	// NewPlanar is already the fully turned vector. The only thing left to decide is HOW LONG it is,
	// and the two limiters below touch nothing else — which is the whole reason the turn survives:
	// the direction computed by the projection formula is preserved exactly, and a player at the hard
	// cap can still carve their velocity round at constant speed indefinitely, for free, forever.
	// What they cannot do is make it any longer.
	// =============================================================================================

	// The projection formula can only ever ADD along the wish direction, so this is >= 0 by
	// construction; the Max is belt and braces against float noise at very low speeds.
	const float RawGain = FMath::Max(0.f, NewSpeed - SpeedBefore);

	// DIMINISHING RETURNS. Sampled at the speed the frame STARTED at, not at NewSpeed: sampling the
	// output would make the scale depend on the frame length, and two 8ms frames would then not equal
	// one 16ms frame — which is precisely the non-linearity CanCombineWith exists to protect the
	// prediction from, and there is no reason to add another one.
	const float GainScale = GetAirStrafeGainScale(SpeedBefore);
	float TargetSpeed = SpeedBefore + RawGain * GainScale;

	// THE BACKSTOPS. Both are floored at SpeedBefore so that a ceiling can only ever remove speed
	// THIS CALL just added, and can never brake momentum carried into the air — a slide-jump that
	// leaves the ground above the cap keeps every unit of it, exactly as MaxAirSpeed always did.
	float SpeedCeiling = FMath::Max(GetMaxAirSpeed(), SpeedBefore);
	if (IsAirStrafeHardCapEnabled())
	{
		SpeedCeiling = FMath::Min(SpeedCeiling, FMath::Max(GetAirStrafeHardCapSpeed(), SpeedBefore));
	}
	TargetSpeed = FMath::Min(TargetSpeed, SpeedCeiling);

	// Rescale, KEEPING THE DIRECTION. This one line is the difference between "limit how much speed
	// can accumulate" and "undo the Source feel".
	NewPlanar *= (TargetSpeed / NewSpeed);

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

	// The mantle owns the pawn for its whole window (spec v5 §7). A dash fired mid-pull-up would
	// fight ApplyMantleVelocity for Velocity every sub-step, and the loser would be whichever machine
	// resolved the frame differently.
	if (MantleTimeRemaining > 0.f)
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

FVector UTraceCharacterMovementComponent::ComputeDashDirection(const FVector& InAcceleration, const FRotator& InAimRotation) const
{
	// SPEC v7 §5. THE ONLY PLACE THE DASH DIRECTION IS EVER DERIVED. Pure: it reads no clock, no
	// per-frame input and no mutable member except the standing-still fallback's facing, so BeginDash
	// on the server, BeginDash on the client, BeginDash on a replayed frame and Trace.DashVectorTest
	// all get bit-identical answers from identical arguments.

	// The input basis is the AIM YAW, not the capsule's — exactly as ATraceCharacter::DoMove builds
	// it. Reconstructing the same basis here is what turns a single world-space Acceleration back
	// into the two scalars the player actually pressed.
	const FRotationMatrix YawBasis(FRotator(0.f, InAimRotation.Yaw, 0.f));
	const FVector YawForward = YawBasis.GetUnitAxis(EAxis::X);
	const FVector YawRight   = YawBasis.GetUnitAxis(EAxis::Y);

	// Acceleration is level to begin with for a human (DoMove feeds it two level axes), but a bot
	// steers with a world direction that can be tilted, so flatten before decomposing rather than
	// letting a bot's climb angle leak in as a phantom forward amount.
	const FVector PlanarInput(InAcceleration.X, InAcceleration.Y, 0.f);
	const float ForwardAmount = FVector::DotProduct(PlanarInput, YawForward);   // W positive, S negative
	const float StrafeAmount  = FVector::DotProduct(PlanarInput, YawRight);     // D positive, A negative

	// THE WHOLE CHANGE IS THIS PAIR OF BASIS VECTORS.
	//   strafe → YawRight, dead level, "parallel to the ground" whatever the pitch is;
	//   forward → the full aim ray, so look up and W goes up, look 45° and W goes 45°.
	// Summed and normalised, because the spec asks for "one dash length" on the diagonals: an
	// unnormalised sum would make W+D reach ~1.4× as far as W alone.
	FVector Direction = InAimRotation.Vector() * ForwardAmount + YawRight * StrafeAmount;

	if (!Direction.Normalize())
	{
		// No directional input. Spec v7 §5 keeps the old fallback verbatim: dash straight ahead,
		// level. bOrientRotationToMovement means the capsule is already facing the last movement
		// direction, which is what a player expects — and with nothing held there is no forward axis
		// asking for the aim's pitch, so this stays horizontal on purpose.
		Direction = (UpdatedComponent != nullptr)
			? UpdatedComponent->GetForwardVector()
			: (CharacterOwner != nullptr ? CharacterOwner->GetActorForwardVector() : FVector::ForwardVector);
		Direction.Z = 0.f;

		if (!Direction.Normalize())
		{
			Direction = FVector::ForwardVector;
		}
	}

	return Direction;
}

void UTraceCharacterMovementComponent::BeginDash()
{
	// Direction is locked here and never recomputed: a dash you can steer is not a dash, and a
	// steerable dash would also have to re-derive its direction identically during replay.
	//
	// Acceleration (not GetLastInputVector()) is the input source on purpose. Acceleration is
	// restored by MoveAutonomous() from the saved move on every replayed frame, so it reproduces
	// exactly; LastControlInputVector is consumed from live per-frame input and is *not* part of
	// the saved move, so using it would desync the client on every correction. The aim rotation is
	// the second input and does NOT come free like that — see GetDashAimRotation().
	//
	// SPEC v7 §5: the Z-stripping that used to live here is GONE, and so is the contract §5 rule it
	// enforced. A dash may now be vertical. Do not reintroduce `Direction.Z = 0`.
	FVector Direction = ComputeDashDirection(Acceleration, GetDashAimRotation());

	// A GROUNDED DASH MAY NOT AIM INTO THE FLOOR. Look at your feet and press W and the composed
	// direction is straight down; PhysWalking would discard all of it and the player would have spent
	// a charge to stand still. Flatten instead, which is the same dash they would have got before
	// they looked down. Airborne is left alone — a dive is a legitimate move.
	if (IsMovingOnGround() && Direction.Z < 0.f)
	{
		FVector Flattened(Direction.X, Direction.Y, 0.f);
		Direction = Flattened.Normalize() ? Flattened : ComputeDashDirection(FVector::ZeroVector, GetDashAimRotation());
	}

	DashDirection = Direction;
	DashTimeRemaining = GetDashDuration();

#if !UE_BUILD_SHIPPING
	// SPEC v8 §1, THE MEASUREMENT. "Dash feels rubber bandy" is a claim about the correction rate
	// DURING a dash, and the previous pass answered it with a whole-session count taken on the HOST,
	// where corrections cannot happen by construction. Attributing each correction to the dash it
	// landed inside turns that into a RATE that can only be read on a client.
	//
	// bClientUpdating gates it because a REPLAYED dash is the same dash, not a new one: without this
	// every correction would inflate the denominator it is supposed to be measured against, and the
	// rate would fall towards 1 no matter how bad the prediction was.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && CharacterOwner->IsLocallyControlled())
	{
		++DashNetDashCount;

		// The window runs past the dash itself: a correction for a dash frame arrives one round trip
		// LATER, so closing the window at the dash's end would miss exactly the corrections this
		// item is about. Duration + 0.5 s covers a 250 ms round trip with room to spare.
		const UWorld* DashWorld = GetWorld();
		DashNetAttributionUntil = (DashWorld != nullptr)
			? static_cast<float>(DashWorld->GetTimeSeconds()) + GetDashDuration() + 0.5f
			: -1000.f;
	}
#endif

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
	//
	// SPEC v7 §5: the FULL vector, Z included. A grounded dash whose direction is level still ends up
	// with Velocity.Z == 0, which is the old behaviour exactly; a grounded dash that aims up needs
	// MOVE_Falling as well, because PhysWalking zeroes Z at the top of every walking step and would
	// otherwise eat the launch whole.
	ApplyDashVelocity();

	if (IsMovingOnGround() && DashDirection.Z > UE_KINDA_SMALL_NUMBER)
	{
		SetMovementMode(MOVE_Falling);
	}

#if !UE_BUILD_SHIPPING
	if (IsDashDebugEnabled())
	{
		const FRotator AimRotation = GetDashAimRotation();
		UE_LOG(LogTraceGame, Display,
			TEXT("DASH %s start: aimPitch=%6.1f accel=(%7.1f,%7.1f,%7.1f) dir=(%6.3f,%6.3f,%6.3f) "
			     "dashPitch=%6.1fdeg v=(%7.1f,%7.1f,%7.1f) mode=%d replayedAim=%d"),
			*GetNameSafe(CharacterOwner), FRotator::NormalizeAxis(AimRotation.Pitch),
			Acceleration.X, Acceleration.Y, Acceleration.Z,
			DashDirection.X, DashDirection.Y, DashDirection.Z,
			FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(DashDirection.Z, -1., 1.))),
			Velocity.X, Velocity.Y, Velocity.Z, static_cast<int32>(MovementMode.GetValue()),
			(CharacterOwner != nullptr && CharacterOwner->bClientUpdating) ? 1 : 0);
	}
#endif
}

void UTraceCharacterMovementComponent::ApplyDashVelocity()
{
	// SPEC v7 §5. One writer for the dash's velocity, used by both the launch and the per-frame
	// re-assert, so the two can never drift apart — they did not before this change either, but they
	// were two copies of the same three lines and now there are three axes to keep in step.
	Velocity = DashDirection * GetDashSpeed();

	// Still on the ground and not asking to leave it: keep Z at zero rather than handing PhysWalking
	// a vertical component it would only discard. This is also what stops a level dash across a slope
	// from being read as an attempt to launch.
	if (IsMovingOnGround() && DashDirection.Z <= UE_KINDA_SMALL_NUMBER)
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

	// SlideCooldownRemaining IS THE HIDDEN COOLDOWN (spec v5 §3). It is enforced here and nowhere
	// else, it is charged in EndSlide(), and nothing draws it. Take this test out and "trigger once,
	// like an ability, with a hidden cooldown to prevent spamming it" becomes a slide you can hold by
	// mashing.
	if (SlideTimeRemaining > 0.f || SlideCooldownRemaining > 0.f || DashTimeRemaining > 0.f
		|| MantleTimeRemaining > 0.f)
	{
		return false;
	}

	// NOTE (spec v5 §3): this is NOT "the key is still held" as a requirement to keep sliding — that
	// rule is gone with the hold. It only stops a buffered press from firing after the player has
	// already let go, which would start a slide nobody is asking for any more. Once BeginSlide() has
	// run the key is irrelevant for the rest of the slide.
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

	// ONE PRESS BUYS THE WHOLE DURATION (spec v5 §3). There is no commit window any more because
	// there is nothing left to commit against: the key cannot end this slide.
	SlideTimeRemaining = GetSlideDuration();

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

// --- The wall jump (spec v8 §7) -----------------------------------------------------------------

void UTraceCharacterMovementComponent::HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta)
{
	Super::HandleImpact(Hit, TimeSlice, MoveDelta);

	if (!IsWallJumpEnabled() || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// AIRBORNE ONLY. Running into a wall on the ground already has an answer — jump — and recording
	// the contact there would let a player walk up to a wall, step off a ledge and wall-jump off a
	// face they were never airborne against.
	//
	// The mantle owns the pawn outright while it runs, exactly as it does for the dash.
	if (!IsFalling() || MantleTimeRemaining > 0.f || MovementMode == MOVE_None)
	{
		return;
	}

	if (!Hit.bBlockingHit)
	{
		return;
	}

	// A WALL, not a ramp. PhysFalling has already refused this hit as a landing spot, but "not a
	// landing spot" includes ceilings and steep-but-walkable-adjacent geometry; the explicit test is
	// what keeps the mechanic to vertical faces and off anything the player could have walked up.
	const FVector Normal = Hit.ImpactNormal;
	if (FMath::Abs(Normal.Z) > GetWallJumpMaxNormalZ())
	{
		return;
	}

	FVector PlanarNormal(Normal.X, Normal.Y, 0.f);
	if (!PlanarNormal.Normalize())
	{
		return;
	}

	// Latch the face and open the window. Overwriting an older normal is deliberate: the wall you are
	// touching NOW is the one a jump should launch off, and in a corner that is the last one hit.
	WallJumpNormal = PlanarNormal;
	WallJumpWindowRemaining = GetWallJumpWindowSeconds();

	// AND THE MOMENTUM, WHICH ONLY EXISTS HERE. See the header on WallJumpEntryVelocity: measured on a
	// client, a head-on approach reads entry=0 by the time the jump is pressed, because PhysFalling
	// re-derives Velocity from the distance the capsule actually moved and a pawn stopped by a wall
	// moved nothing. Without this capture the "preserve and redirect momentum" of spec v8 §7 is a
	// 420 uu/s nudge and nothing else. Captured planar; the vertical component is the wall jump's own
	// number.
	//
	// Take the FASTER of this frame's velocity and whatever the window already holds: the engine can
	// deliver two HandleImpact calls for one collision (the sweep, then the slide's re-sweep), and the
	// second arrives with the speed already scrubbed off.
	const FVector PlanarEntry(Velocity.X, Velocity.Y, 0.f);
	if (WallJumpEntryVelocity.IsNearlyZero() || PlanarEntry.SizeSquared() > WallJumpEntryVelocity.SizeSquared())
	{
		WallJumpEntryVelocity = PlanarEntry;
	}
}

bool UTraceCharacterMovementComponent::IsWallJumpAvailable() const
{
	return IsWallJumpEnabled()
		&& MovementMode != MOVE_None
		&& IsFalling()
		&& MantleTimeRemaining <= 0.f
		&& WallJumpWindowRemaining > 0.f
		&& !WallJumpNormal.IsNearlyZero()
		&& WallJumpsSinceGround < GetWallJumpMaxConsecutive();
}

bool UTraceCharacterMovementComponent::TryWallJump()
{
	if (!IsWallJumpAvailable())
	{
		return false;
	}

	const FVector Normal = WallJumpNormal;

	// --- REDIRECT, DO NOT RESET -------------------------------------------------------------------
	//
	// Reflect ONLY the component that was travelling into the wall. A head-on approach comes straight
	// back, a glancing one glances off, and a pawn already moving away from the face (a corner, or a
	// frame where the collision response has already pushed it out) keeps its vector untouched rather
	// than being fired back into the geometry it just escaped.
	// THE APPROACH VELOCITY, NOT THE POST-COLLISION ONE. WallJumpEntryVelocity is what the pawn was
	// carrying on the frame it touched the wall; Velocity by now is what survived the collision, which
	// for a head-on hit is nothing at all (measured: entry=0 uu/s on a client, every jump). Falling
	// back to the live velocity keeps a window that somehow opened without a capture working.
	FVector Planar = WallJumpEntryVelocity;
	Planar.Z = 0.f;
	if (Planar.IsNearlyZero())
	{
		Planar = FVector(Velocity.X, Velocity.Y, 0.f);
	}
	const float EntrySpeed = Planar.Size();

	// Captured before the reflection mutates Planar. Measurement only (spec v8 §7's "in a new
	// direction" is an angle), but it costs one normalise and keeps the maths honest.
	const FVector EntryPlanarDirection = Planar.GetSafeNormal();
	const float IntoWall = FVector::DotProduct(Planar, Normal);
	if (IntoWall < 0.f)
	{
		Planar -= 2.f * IntoWall * Normal;
	}

	// The reflection is a rotation, so it preserves magnitude exactly; the retention is the only place
	// speed is allowed to change, and it can only ever remove.
	FVector LaunchDirection = Planar;
	if (!LaunchDirection.Normalize())
	{
		// Pressed flat against the wall with no planar speed at all. The outward impulse below is the
		// whole launch in that case, which is what makes a standing wall jump an escape rather than a
		// wasted press.
		LaunchDirection = Normal;
	}

	FVector Launch = LaunchDirection * (EntrySpeed * GetWallJumpSpeedRetention()) + Normal * GetWallJumpOutwardImpulse();

	// --- AND IT MAY NOT BEAT THE AIR-STRAFE CEILING (spec v5 §1) ----------------------------------
	//
	// Same shape as the clamp at the end of ApplySourceAirAcceleration, and for the same reason: the
	// ceiling exists to stop speed being BUILT in the air, not to brake speed that was carried into
	// it. So the cap is floored at the speed the pawn arrived with — a wall jump keeps every unit it
	// was already carrying, and can never add past the hard cap. Without this the outward impulse
	// would be a free, repeatable +420 uu/s that the whole of spec v5 §1 was written to prevent.
	if (IsAirStrafeHardCapEnabled())
	{
		const float Ceiling = FMath::Max(EntrySpeed, GetAirStrafeHardCapSpeed());
		const float LaunchSpeed = Launch.Size();
		if (LaunchSpeed > Ceiling && LaunchSpeed > UE_KINDA_SMALL_NUMBER)
		{
			Launch *= (Ceiling / LaunchSpeed);
		}
	}

	Velocity.X = Launch.X;
	Velocity.Y = Launch.Y;

	// Super::DoJump has already set Z to at least JumpZVelocity and switched to MOVE_Falling. Assign
	// rather than scale: the wall jump's vertical component is its own number (a multiple of the
	// jump, like every other launch here), not a modifier on whatever the fall had left.
	Velocity.Z = JumpZVelocity * GetWallJumpVerticalMultiplier();

	// One jump per contact, and one step up the ladder. The window is closed rather than left to
	// expire so that a second press inside the same window cannot double-dip off one wall.
	WallJumpWindowRemaining = 0.f;
	WallJumpNormal = FVector::ZeroVector;
	WallJumpEntryVelocity = FVector::ZeroVector;
	++WallJumpsSinceGround;

#if !UE_BUILD_SHIPPING
	// SPEC v8 §7, THE MEASUREMENT. Counted on the RECORD pass only, for BeginDash()'s reason: a
	// replayed wall jump is the same wall jump, and counting it again would inflate the denominator
	// that "corrections per wall jump" is measured against.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && CharacterOwner->IsLocallyControlled())
	{
		const FVector LaunchPlanar(Velocity.X, Velocity.Y, 0.f);
		const float LaunchSpeed = static_cast<float>(LaunchPlanar.Size());

		++WallJumpCount;
		WallJumpEntrySpeedSum += EntrySpeed;
		WallJumpLaunchSpeedSum += LaunchSpeed;
		WallJumpLaunchZSum += static_cast<float>(Velocity.Z);
		WallJumpMaxConsecutiveSeen = FMath::Max(WallJumpMaxConsecutiveSeen, WallJumpsSinceGround);

		// "In a NEW direction" is an angle, so measure the angle between the approach and the launch.
		if (!EntryPlanarDirection.IsNearlyZero() && LaunchSpeed > 1.f)
		{
			const float Cosine = FMath::Clamp(
				static_cast<float>(FVector::DotProduct(EntryPlanarDirection, LaunchPlanar.GetSafeNormal())),
				-1.f, 1.f);
			WallJumpTurnDegreesSum += FMath::RadiansToDegrees(FMath::Acos(Cosine));
		}

		const UWorld* WallJumpWorld = GetWorld();
		WallJumpAttributionUntil = (WallJumpWorld != nullptr)
			? static_cast<float>(WallJumpWorld->GetTimeSeconds()) + 0.75f
			: -1000.f;
	}

	if (IsDashDebugEnabled() && CharacterOwner != nullptr)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("WALLJUMP %-16s normal=(%6.3f,%6.3f) entry=%6.0f -> launch=%6.0f uu/s (retain=%.2f "
			     "outward=%.0f) velZ=%6.0f consecutive=%d/%d role=%d"),
			*GetNameSafe(CharacterOwner), Normal.X, Normal.Y, EntrySpeed, GetPlanarSpeed(),
			GetWallJumpSpeedRetention(), GetWallJumpOutwardImpulse(), Velocity.Z,
			WallJumpsSinceGround, GetWallJumpMaxConsecutive(),
			static_cast<int32>(CharacterOwner->GetLocalRole()));
	}
#endif

	return true;
}

bool UTraceCharacterMovementComponent::CanAttemptJump() const
{
	// Super, MINUS the "!bWantsToCrouch" clause. See the header: crouch is the slide key here and
	// never resizes the capsule, so the engine's "you might not have headroom to stand up" rule has
	// nothing to protect and was silently making the slide-jump unreachable for human players.
	//
	// Everything else Super checks is kept verbatim, including the IsMovingOnGround() || IsFalling()
	// test, which ACharacter::JumpIsAllowedInternal still validates against JumpMaxCount on top.
	//
	// ...plus one clause of our own: no jumping out of a mantle. The pull-up is in MOVE_Flying, so
	// Super's mode test would already refuse, but stating it here means the rule survives if the
	// mantle ever moves to a custom mode — and a jump that fired mid-pull-up would leave the mantle
	// clock running with the pawn no longer on its rail.
	return IsJumpAllowed() && !IsMantling() && (IsMovingOnGround() || IsFalling());
}

bool UTraceCharacterMovementComponent::DoJump(bool bReplayingMoves, float DeltaTime)
{
	// Capture BEFORE anything is touched: EndSlide() below rewrites every one of these.
	const bool bSlideJump = IsSlideJumpAvailable();
	const bool bWellTimed = IsSlideJumpWellTimed();

	// SPEC v8 §7. BOTH OF THESE MUST BE READ BEFORE Super::DoJump, and the first one is a trap worth
	// naming: Super calls SetMovementMode(MOVE_Falling), so IsFalling() is TRUE after it even for an
	// ordinary jump off the floor. Asking afterwards would route every ground jump into the wall-jump
	// branch and refuse it — i.e. it would delete the jump key.
	const bool bWasAirborneBeforeJump = IsFalling();
	const FVector PreJumpVelocity = Velocity;

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
		// SPEC v8 §7. An airborne press that is not a slide-jump is the wall jump's only entry point.
		//
		// ORDER MATTERS AND THE SLIDE-JUMP WINS. A jump taken inside the slide-jump's coyote window is
		// still a slide-jump even with a wall window open, so nothing about spec v4 §1 or v5 §3 changes
		// behaviour — the wall jump only ever sees presses the slide had no claim on.
		//
		// TryWallJump() refuses (and leaves Velocity exactly as Super left it) whenever the pawn is
		// grounded, past the consecutive cap, or outside the contact window. That refusal is what stops
		// the raised JumpMaxCount from being a double jump: Super has already written
		// Velocity.Z = max(Z, JumpZVelocity), which is a legitimate mid-air jump — so a refusal here
		// has to un-ask the whole thing, which is exactly what returning false does. ACharacter::
		// CheckJumpInput does not increment JumpCurrentCount for a DoJump that returned false.
		if (bWasAirborneBeforeJump && IsWallJumpEnabled())
		{
#if !UE_BUILD_SHIPPING
			// SPEC v8 §7, the anti-ladder cap, counted. A press that had a live wall under it and was
			// refused ONLY by the consecutive cap is the thing that stops two walls being an infinite
			// staircase, so it is worth its own number rather than being invisible inside "refused".
			const bool bCapRefusal = WallJumpWindowRemaining > 0.f
				&& !WallJumpNormal.IsNearlyZero()
				&& WallJumpsSinceGround >= GetWallJumpMaxConsecutive()
				&& CharacterOwner != nullptr && !CharacterOwner->bClientUpdating
				&& CharacterOwner->IsLocallyControlled();
			if (bCapRefusal)
			{
				++WallJumpCapRefusals;
			}
#endif

			if (TryWallJump())
			{
				return true;
			}

			// Not a wall jump. Undo Super's vertical launch and refuse — otherwise every mid-air press
			// would be a free jump, because the extra JumpMaxCount exists only to let the engine ask.
			// The pawn was already airborne, so Super's SetMovementMode(MOVE_Falling) was a no-op and
			// the velocity is the only thing to put back.
			Velocity = PreJumpVelocity;
			return false;
		}

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
	//
	// SPEC v5 §3 adds the well-timed VERTICAL bonus on top. Two independent multipliers rather than
	// one bigger speed number, because they buy different things: the speed bonus makes the hop go
	// further, the Z bonus makes it go higher — and it is the height a player actually perceives, so
	// this is what makes a well-timed slide-jump read as a different move rather than a slightly
	// better one. Both are collected or neither is; missing the window still costs nothing.
	Velocity.Z *= GetSlideJumpZMultiplier() * (bWellTimed ? GetSlideJumpWindowZBonus() : 1.f);

	// Consumed. One slide, one slide-jump.
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;

#if !UE_BUILD_SHIPPING
	// Observation only, and on the authority alone so a client replaying corrections cannot count the
	// same hop several times. At Display, behind the same switch as the slide measurement.
	if (IsSlideDebugEnabled() && CharacterOwner != nullptr && CharacterOwner->HasAuthority())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("SLIDEJUMP %-16s carried=%6.0f -> launched=%6.0f uu/s (%.1f%%, retain=%.2f%s) "
			     "velZ=%6.0f (jumpZ=%.0f x zMul=%.2f x zBonus=%.2f)"),
			*GetNameSafe(CharacterOwner), CarrySpeed, GetPlanarSpeed(),
			100.f * GetPlanarSpeed() / FMath::Max(1.f, CarrySpeed), Retention,
			bWellTimed ? TEXT(", WELL TIMED") : TEXT(""),
			Velocity.Z, JumpZVelocity, GetSlideJumpZMultiplier(),
			bWellTimed ? GetSlideJumpWindowZBonus() : 1.f);
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

	// SPEC v7 §5, AND THE THING THAT KEEPS A VERTICAL DASH FROM BEING A ROCKET. The dash suspends
	// gravity for its window, so a straight-up one would hand back the whole DashSpeed as upward
	// velocity — 3000 uu/s, another 4592 uu of coast, past the arena's 1640 uu ceiling. THE AIR-STRAFE
	// CEILING CANNOT HELP HERE: it is planar-only by construction (see ApplySourceAirAcceleration,
	// which never touches Z) and so bounds nothing vertical at all.
	//
	// Same shape as the planar rule directly above: hand back a fast player, not a stationary one,
	// but hand back a bounded one. Downward Z is deliberately untouched — a dive is not a climb, and
	// clamping it would turn a downward dash into a float.
	const float VerticalLimit = GetDashExitVerticalSpeedLimit();
	if (Velocity.Z > VerticalLimit)
	{
		Velocity.Z = VerticalLimit;
	}
}

// =================================================================================================
// THE MANTLE (spec v5 §7)
//
// Client-predicted with NO new input and NO new compressed flag. Everything the detection reads is
// either restored by the replay path (Velocity, Acceleration, the updated component's transform) or
// is static arena geometry, which is byte-identical on every machine. So the client, the server and
// every replayed move independently reach the same answer from the same inputs — which is the same
// contract the dash and the slide already keep, just without an intent bit to carry.
// =================================================================================================

bool UTraceCharacterMovementComponent::IsMantling() const
{
	// MOVE_None means the pawn was switched off (death, teleport) and OnMovementUpdated has stopped
	// running, so the clock is frozen wherever it was — the same guard IsDashing() carries, and for
	// the same reason.
	return MantleTimeRemaining > 0.f && MovementMode != MOVE_None;
}

bool UTraceCharacterMovementComponent::IsGroundedForAbilities() const
{
	return IsMovingOnGround() || GroundGraceRemaining > 0.f;
}

bool UTraceCharacterMovementComponent::CanAttemptMantle() const
{
	if (!IsMantleEnabled() || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return false;
	}

	if (MovementMode == MOVE_None || MantleTimeRemaining > 0.f || MantleCooldownRemaining > 0.f)
	{
		return false;
	}

	// AIRBORNE ONLY. A mantle is what a jump at a ledge should have been; a pawn standing on the
	// ground in front of a waist-high box is already handled by step-up, and letting it vacuum itself
	// up every wall it walks into would be a movement system nobody asked for.
	//
	// IsFalling(), not !IsMovingOnGround(): the grace window (which deliberately keeps reporting
	// "grounded" across a ledge blip) must not gate this, because a jump taken off a lip is exactly
	// when a mantle is wanted.
	if (!IsFalling())
	{
		return false;
	}

	// The dash owns the velocity vector for its window and the slide is a ground state; neither may
	// be interrupted by a mantle, and both end on their own within a fraction of a second.
	if (DashTimeRemaining > 0.f || SlideTimeRemaining > 0.f)
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

bool UTraceCharacterMovementComponent::TryBeginMantle(const FVector& ApproachVelocity)
{
	if (!CanAttemptMantle())
	{
		return false;
	}

	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	UWorld* MantleWorld = GetWorld();
	if (Capsule == nullptr || MantleWorld == nullptr)
	{
		return false;
	}

	// Why a refusal is worth logging at all: this function has nine independent ways to say no, all
	// of them geometric, and a silent "no mantle happened" is indistinguishable between them. Two
	// separate real bugs (a chest-only probe and a travel-direction probe) survived a full test run
	// each because the failure had no voice. Locally controlled pawns only, so ten bots refusing
	// sixty times a second cannot drown the log.
#if !UE_BUILD_SHIPPING
	const bool bLogRefusals = IsMantleDebugEnabled() && CharacterOwner->IsLocallyControlled();
	#define TRACE_MANTLE_NO(Reason, ...) \
		do { if (bLogRefusals) { UE_LOG(LogTraceGame, Display, TEXT("MANTLE-NO %s: " Reason), *GetNameSafe(CharacterOwner), ##__VA_ARGS__); } return false; } while (0)
#else
	#define TRACE_MANTLE_NO(Reason, ...) return false
#endif

	const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
	const float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const FVector Here = UpdatedComponent->GetComponentLocation();
	const float FeetZ = Here.Z - CapsuleHalfHeight;

	// --- Direction and approach speed ------------------------------------------------------------
	//
	// Velocity, not the control rotation: a mantle is a consequence of where the pawn is going, and
	// Velocity is restored by the replay path while the camera is not saved-move state at all.
	//
	// APPROACH VELOCITY, THOUGH, NOT THE CURRENT ONE, and this is not a detail — the first build of
	// this function never fired a single mantle because of it. The frame a jump's capsule meets a
	// ledge face head-on is the frame SafeMoveUpdatedComponent zeroes the planar velocity against it,
	// so by the time OnMovementUpdated runs, "how fast am I going toward that wall" reads as roughly
	// nothing and the speed gate refused every time. OnMovementUpdated's OldVelocity is the velocity
	// at the START of the move — before the collision — and it is reproduced exactly on a replay
	// (PrepMoveFor restores Velocity, then PerformMovement runs), so it is both the honest number and
	// a prediction-safe one.
	const float PlanarSpeed = FVector(Velocity.X, Velocity.Y, 0.f).Size();
	const float ApproachSpeed = FMath::Max(PlanarSpeed, FVector(ApproachVelocity.X, ApproachVelocity.Y, 0.f).Size());
	if (ApproachSpeed < GetMantleMinForwardSpeed())
	{
		TRACE_MANTLE_NO("too slow (approach %.0f < min %.0f)", ApproachSpeed, GetMantleMinForwardSpeed());
	}

	// THE PROBE FOLLOWS THE INPUT, NOT THE TRAVEL, and this is the second thing that stopped the
	// first build ever firing. Measured, from a scripted jump straight at a 260 uu block: on contact
	// the capsule's planar velocity had been deflected almost entirely ALONG the face by
	// SlideAlongSurface (dist to the face pinned at ~40 uu while the velocity swung round to run
	// parallel to it), so a probe cast along the direction of travel traced sideways down the wall
	// and found nothing — for thirty consecutive frames of being pressed flat against the ledge.
	//
	// The direction that still means "get me up there" in that moment is the one the player is
	// holding. It is also the intent test: a pawn falling past a wall with no input, or being carried
	// along one, has no wish direction into it and cannot mantle, so the game never takes the
	// controls away from somebody who did not ask.
	//
	// RequestedVelocity is the fallback for the same reason ApplyGroundOverspeedBleed carries it: AI
	// path following sets that and leaves Acceleration at zero, so without it no bot could ever
	// mantle and the raised sections would be human-only ground.
	FVector Forward(Acceleration.X, Acceleration.Y, 0.f);
	if (Forward.SizeSquared() <= UE_KINDA_SMALL_NUMBER && bHasRequestedVelocity)
	{
		Forward = FVector(RequestedVelocity.X, RequestedVelocity.Y, 0.f);
	}
	if (!Forward.Normalize())
	{
		TRACE_MANTLE_NO("no wish direction (accel %.0f, requestedVel=%d)", Acceleration.Size2D(), bHasRequestedVelocity ? 1 : 0);
	}

	const float ForwardSpeed = ApproachSpeed;

	FCollisionQueryParams QueryParams;
	FCollisionResponseParams ResponseParams;
	InitCollisionParams(QueryParams, ResponseParams);
	QueryParams.bTraceComplex = false;

	// A trace that STARTS inside a collider must not report that collider. Without this a pawn whose
	// capsule is even slightly embedded in geometry gets back a hit at distance 0 whose ImpactNormal
	// is simply the reverse of the trace direction — which is horizontal, so it passes for a wall —
	// and whose ImpactPoint is the pawn's own position. See the bStartPenetrating rejections below
	// for what that cost when it was missing.
	QueryParams.bFindInitialOverlaps = false;

	// AND THE PAWN MUST NOT FIND ITSELF. This one line is why the mantle almost never fired.
	//
	// InitCollisionParams() forwards to UPrimitiveComponent::InitSweepCollisionParams, which adds
	// MoveIgnoreActors and nothing else — it does NOT ignore the moving actor, because the sweep path
	// it was written for excludes self inside MoveComponent instead. These probes are raw line traces,
	// so nothing excluded the pawn. Every probe starts on the capsule's own axis at
	// (Here.X, Here.Y, FeetZ + ProbeHeight), and the highest of the three starts at ProbeHeight ==
	// CapsuleHalfHeight, i.e. exactly the capsule's centre — so it opened inside the pawn's own
	// collider and came back penetrating at distance 0.
	//
	// MEASURED, on a clean approach to a 260 uu block on open floor: the two frames where the mantle
	// should obviously have fired — planar 800 uu/s, face 35-42 uu ahead, ledge 99 uu above the feet,
	// dead centre of the [55, 230] window — were BOTH refused with
	// "degenerate (penetrating=1 dist=0.00 actor=TraceCharacter_0)", where TraceCharacter_0 is the
	// probing pawn. The frame after that the capsule met the face, SafeMoveUpdatedComponent zeroed the
	// planar velocity, and every later frame refused with "too slow (approach 0 < min 120)" while the
	// pawn slid up the wall and fell back down. Eight runs, eight failures, and the same shape in the
	// previous pass's logs (6825 "degenerate" refusals).
	QueryParams.AddIgnoredActor(CharacterOwner);

	const ECollisionChannel Channel = UpdatedComponent->GetCollisionObjectType();

	// --- 1. REACH: is there a near-vertical face ahead? -------------------------------------------
	//
	// THREE HEIGHTS, AND THE LOW ONE IS THE IMPORTANT ONE. The first build probed at chest height
	// only, on the reasoning that a ledge is chest-high — and it never fired on the case the report
	// is actually about. "Jumping on the edge of a raised section" is a jump that ALMOST cleared it:
	// the pawn's feet are a few uu below the top, so its chest is a metre ABOVE the ledge and a chest
	// probe sails straight over the thing it is meant to find.
	//
	// A ledge face runs from its base all the way up to its lip, so a probe just above the feet finds
	// it from any approach height — high (nearly cleared) or low (jumped short). The two higher
	// probes are there for the case where the feet are below the face's own base (standing in a
	// gutter, landing on a lower step). Nothing here decides whether it is CLIMBABLE, which is
	// entirely the height test's job below.
	//
	// EVERY HEIGHT IS TRIED, AND AN UNUSABLE HIT DOES NOT END THE SEARCH. This loop used to keep the
	// FIRST probe that hit anything, break, and validate that one hit below — so if the first probe to
	// hit was unusable, the whole mantle was refused even when a higher probe was staring straight at a
	// clean ledge face. MEASURED across the previous pass's logs: 6825 refusals reading
	// "penetrating=1 dist=0.00" against 39 real mantles. The low probe sits 2 uu above the feet, which
	// on a real approach is inside the floor slab the pawn is pressed against, so it degenerates on
	// almost every frame of contact and masked the two probes above it for the entire approach. That
	// single `break` is why the mantle fired once in eight runs while the gate logged canMantle=1.
	//
	// The guards themselves are UNCHANGED and still absolute — a degenerate hit, a ramp or a pawn can
	// never START a mantle, so the ladder-to-the-sky loop stays closed. They now disqualify only the
	// PROBE that produced them instead of the whole attempt.
	const float ProbeHeights[] = { 2.f, CapsuleHalfHeight * 0.5f, CapsuleHalfHeight };
	const float ProbeReach = CapsuleRadius + GetMantleReachUU();

	FHitResult WallHit;
	bool bFoundWall = false;

	// The refusal reason is remembered rather than logged in place: with three probe heights a refusal
	// is only real once all three have failed, and logging inside the loop would print two misleading
	// rejections for every genuine one. Seeded with the "nothing ahead at all" case.
	FString WallRejectReason = FString::Printf(TEXT("no face within %.0fuu ahead along %s (feet z=%.1f)"),
		ProbeReach, *Forward.ToCompactString(), FeetZ);

	for (const float ProbeHeight : ProbeHeights)
	{
		const FVector ProbeStart(Here.X, Here.Y, FeetZ + ProbeHeight);
		FHitResult ProbeHit;
		if (!MantleWorld->LineTraceSingleByChannel(ProbeHit, ProbeStart, ProbeStart + Forward * ProbeReach,
			Channel, QueryParams, ResponseParams))
		{
			continue;
		}

		// THE LADDER-TO-THE-SKY GUARD. MEASURED, and it is the reason this test exists rather than a
		// precaution: without it a headless match produced 289 mantles in 25 seconds and carried the
		// bot team from Z=313 to Z=4097, one 230 uu step at a time. Every single one of those mantles
		// logged reach=0.0uu, because the forward trace was starting inside a collider the pawn was
		// slightly embedded in. A penetrating hit reports ImpactPoint = the trace's own start (i.e. the
		// pawn) and ImpactNormal = -TraceDirection (horizontal, so it passes for a wall), and the
		// height probe then finds whatever happens to be overhead — so the pawn mantles onto itself,
		// ends up embedded a little higher, and does it again forever.
		//
		// bFindInitialOverlaps=false above already suppresses most of this; the explicit tests are the
		// backstop, because a distance-zero "ledge" is meaningless whatever produced it.
		if (ProbeHit.bStartPenetrating || ProbeHit.Distance < 1.f)
		{
			// The actor is named because "degenerate" alone cost this project a whole pass: it reads as
			// a mantle bug, and the 6825 that were logged were the pawn pressed into the ENDZONE by a
			// broken test harness. Knowing what was penetrated separates the two instantly.
			WallRejectReason = FString::Printf(
				TEXT("face at +%.0fuu is degenerate (penetrating=%d dist=%.2f actor=%s) - the ladder-to-the-sky guard"),
				ProbeHeight, ProbeHit.bStartPenetrating ? 1 : 0, ProbeHit.Distance, *GetNameSafe(ProbeHit.GetActor()));
			continue;
		}

		// Near-vertical. A 0.3 Z component is about a 17 degree overhang/lean either way; anything
		// flatter is a ramp the pawn should be running up, not climbing.
		if (FMath::Abs(ProbeHit.ImpactNormal.Z) > 0.3f)
		{
			WallRejectReason = FString::Printf(TEXT("face at +%.0fuu is not vertical (normal.Z=%.2f, actor %s)"),
				ProbeHeight, ProbeHit.ImpactNormal.Z, *GetNameSafe(ProbeHit.GetActor()));
			continue;
		}

		// YOU MAY NOT CLIMB PEOPLE. MEASURED: a bot mantled with wall=TraceCharacter_2, i.e. it used a
		// team-mate's capsule as the ledge face and pulled itself up over them.
		//
		// That is a gameplay exploit — a stack of pawns becomes a ladder onto any roof in the arena —
		// but the reason it is rejected HERE rather than tuned away is prediction. This whole feature
		// is safe to predict because it is derived from STATIC arena geometry, which is byte-identical
		// on every machine; another pawn's position is replicated, so the client and the server hold
		// different versions of it, and a mantle keyed off one would put the two ends on different
		// ledges. Every argument in the header about why the mantle cannot desync stops being true the
		// moment the thing being climbed can move on its own.
		if (Cast<APawn>(ProbeHit.GetActor()) != nullptr)
		{
			WallRejectReason = FString::Printf(TEXT("face at +%.0fuu belongs to a pawn (%s) - people are not ledges"),
				ProbeHeight, *GetNameSafe(ProbeHit.GetActor()));
			continue;
		}

		WallHit = ProbeHit;
		bFoundWall = true;
		break;
	}

	if (!bFoundWall)
	{
		TRACE_MANTLE_NO("%s", *WallRejectReason);
	}

	// --- 2. HEIGHT: where is the top of it? ------------------------------------------------------
	//
	// Drop a trace from above, just BEYOND the face, so it lands on the ledge's top surface rather
	// than on the face itself. The overshoot is half a capsule radius: far enough in to clear the
	// lip's own bevel, near enough to the edge that a narrow ledge still registers.
	const float MaxHeight = GetMantleMaxHeightUU();
	const FVector TopProbeXY = WallHit.ImpactPoint + Forward * (CapsuleRadius * 0.5f);
	const FVector TopProbeStart(TopProbeXY.X, TopProbeXY.Y, FeetZ + MaxHeight + CapsuleHalfHeight);
	const FVector TopProbeEnd(TopProbeXY.X, TopProbeXY.Y, FeetZ - 1.f);

	FHitResult TopHit;
	if (!MantleWorld->LineTraceSingleByChannel(TopHit, TopProbeStart, TopProbeEnd, Channel, QueryParams, ResponseParams))
	{
		TRACE_MANTLE_NO("no top found above the face (probed z %.1f down to %.1f at %s)",
			TopProbeStart.Z, TopProbeEnd.Z, *TopProbeXY.ToCompactString());
	}

	// Same rejection as the wall probe, for the same reason: a down-trace that begins inside geometry
	// reports its own start point as the "ledge top", which is a surface that does not exist.
	if (TopHit.bStartPenetrating)
	{
		TRACE_MANTLE_NO("top probe started inside geometry");
	}

	// Walkable, or it is not a ledge — it is the underside of something, or a slope the pawn would
	// slide straight back off.
	if (!IsWalkable(TopHit))
	{
		TRACE_MANTLE_NO("top is not walkable (normal.Z=%.2f)", TopHit.ImpactNormal.Z);
	}

	// Same rule as the face, same reason: standing on someone's head is not a ledge, and a
	// destination derived from a replicated actor is not a destination both machines agree on.
	if (Cast<APawn>(TopHit.GetActor()) != nullptr)
	{
		TRACE_MANTLE_NO("top belongs to a pawn (%s)", *GetNameSafe(TopHit.GetActor()));
	}

	const float LedgeHeight = TopHit.ImpactPoint.Z - FeetZ;
	if (LedgeHeight < GetMantleMinHeightUU() || LedgeHeight > MaxHeight)
	{
		TRACE_MANTLE_NO("ledge height %.1f outside [%.0f, %.0f]", LedgeHeight, GetMantleMinHeightUU(), MaxHeight);
	}

	// --- 3. CLEAR: is there room to stand up there? ----------------------------------------------
	//
	// Proved BEFORE anything moves. A mantle that starts and then finds the destination occupied has
	// to either stop dead in mid-air or push the pawn into geometry, and both of those are corrections
	// waiting to happen — which is the whole class of bug this feature exists to remove.
	const FVector Destination(TopHit.ImpactPoint.X, TopHit.ImpactPoint.Y,
		TopHit.ImpactPoint.Z + CapsuleHalfHeight + 2.f);

	// AND IT HAS TO BE SOMEWHERE ELSE. A "ledge" directly overhead is the pawn mantling onto its own
	// column — the third and last guard against the climbing loop above, and the one that still holds
	// if some future geometry finds a way past the two penetration tests.
	if (FVector::DistSquared2D(Destination, Here) < FMath::Square(CapsuleRadius * 0.5f))
	{
		TRACE_MANTLE_NO("destination is directly overhead (%.1fuu ahead)", static_cast<float>(FVector::Dist2D(Destination, Here)));
	}

	// Shrunk very slightly so a destination flush against a wall is not rejected by its own floor.
	const FCollisionShape StandShape = FCollisionShape::MakeCapsule(CapsuleRadius - 1.f, CapsuleHalfHeight - 1.f);
	if (MantleWorld->OverlapBlockingTestByChannel(Destination, FQuat::Identity, Channel, StandShape, QueryParams, ResponseParams))
	{
		TRACE_MANTLE_NO("no room to stand at %s", *Destination.ToCompactString());
	}

	// The climb has to clear the lip before the pawn moves forward, so it rises to the destination's
	// own height plus a little; the forward phase then crosses level.
	MantleUpTargetZ = Destination.Z;
	MantleTargetLocation = Destination;
	MantleTotalTime = GetMantleDurationSeconds();
	MantleTimeRemaining = MantleTotalTime;
	MantleEntrySpeed = ForwardSpeed;

	// MOVE_Flying, not a custom mode: PhysFlying already does swept movement with no gravity, which
	// is exactly a pull-up, and MovementMode is replicated in every correction (ServerMovementMode)
	// so both ends agree about which physics the pawn is under without a single new byte on the wire.
	SetMovementMode(MOVE_Flying);
	Velocity = FVector::ZeroVector;

	// A slide-jump grace that survived into a mantle would pay the bonus for a hop taken off the top
	// of the ledge, several tenths of a second and an entire ability later.
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;

#if !UE_BUILD_SHIPPING
	if (IsMantleDebugEnabled())
	{
		++GTraceMantleCount;
		UE_LOG(LogTraceGame, Display,
			TEXT("MANTLE %-16s ledgeH=%5.1fuu (min %.0f max %.0f) reach=%5.1fuu ahead=%5.1fuu "
			     "entrySpeed=%6.0f dur=%.2fs -> target %s | wall=%s | n=%d role=%d"),
			*GetNameSafe(CharacterOwner), LedgeHeight, GetMantleMinHeightUU(), GetMantleMaxHeightUU(),
			WallHit.Distance, static_cast<float>(FVector::Dist2D(Destination, Here)),
			ForwardSpeed, MantleTotalTime, *Destination.ToCompactString(),
			*GetNameSafe(WallHit.GetActor()),
			GTraceMantleCount, static_cast<int32>(CharacterOwner->GetLocalRole()));
	}
#endif

	#undef TRACE_MANTLE_NO
	return true;
}

void UTraceCharacterMovementComponent::ApplyMantleVelocity(const float DeltaTime)
{
	if (UpdatedComponent == nullptr || MantleTotalTime <= 0.f)
	{
		Velocity = FVector::ZeroVector;
		return;
	}

	const FVector Here = UpdatedComponent->GetComponentLocation();

	// Where in the pull-up are we? Elapsed rather than remaining, because the two phases are defined
	// forwards ("climb for the first 60%") and the arithmetic should read the same way.
	const float Elapsed = FMath::Max(0.f, MantleTotalTime - MantleTimeRemaining);
	const float UpPhaseEnd = MantleTotalTime * GetMantleUpPhaseFraction();

	FVector Target;
	float TimeLeftInPhase;
	if (Elapsed < UpPhaseEnd)
	{
		// PHASE 1 — straight up the face. XY is held so the capsule stays in contact with the wall
		// rather than swinging out into the air, which is what makes the move read as a climb.
		Target = FVector(Here.X, Here.Y, MantleUpTargetZ);
		TimeLeftInPhase = UpPhaseEnd - Elapsed;
	}
	else
	{
		// PHASE 2 — across the lip. Full 3D target so any Z the climb failed to reach is finished off
		// here rather than leaving the pawn standing in the wall.
		Target = MantleTargetLocation;
		TimeLeftInPhase = FMath::Max(MantleTimeRemaining, DeltaTime);
	}

	// SELF-CORRECTING BY CONSTRUCTION, and this is the property that makes the mantle safe to
	// predict. The velocity is recomputed every sub-step from (target - where I actually am), so two
	// machines that started the pull-up from positions a uu apart converge on the same destination
	// instead of accumulating the difference — and a replayed move, which restarts from a restored
	// position, lands in exactly the same place as the original did.
	const FVector ToTarget = Target - Here;
	const float Distance = ToTarget.Size();
	if (Distance <= UE_KINDA_SMALL_NUMBER)
	{
		Velocity = FVector::ZeroVector;
		return;
	}

	Velocity = ToTarget * (1.f / FMath::Max(TimeLeftInPhase, UE_KINDA_SMALL_NUMBER));
}

void UTraceCharacterMovementComponent::EndMantle()
{
	if (MantleTimeRemaining <= 0.f && MantleTotalTime <= 0.f)
	{
		return;
	}

	MantleTimeRemaining = 0.f;
	MantleTotalTime = 0.f;
	MantleCooldownRemaining = GetMantleCooldownSeconds();

	// Hand the pawn back MOVING, not standing. Contract §2.4 — transitions preserve velocity vectors
	// rather than resetting them — and a mantle that dumped the player on the lip at 0 uu/s would be
	// a full stop in the middle of a firefight, i.e. the thing the whole momentum pass exists to
	// avoid. Capped at the ground limit, though: a mantle must not become a faster way to travel than
	// running, or the arena's raised sections turn into a speed route.
	FVector ExitDirection(Velocity.X, Velocity.Y, 0.f);
	if (!ExitDirection.Normalize())
	{
		ExitDirection = (UpdatedComponent != nullptr) ? UpdatedComponent->GetForwardVector() : FVector::ForwardVector;
		ExitDirection.Z = 0.f;
		if (!ExitDirection.Normalize())
		{
			ExitDirection = FVector::ForwardVector;
		}
	}

	// MOVE_Falling, not MOVE_Walking: the pawn is a couple of uu above the ledge and the engine's own
	// floor check on the next frame is the only thing entitled to decide it has landed. Asserting
	// Walking here would be this file guessing at a floor result, which is exactly the kind of
	// client/server guess the ledge diagnosis is about.
	//
	// Done BEFORE the exit speed is computed, and that ordering is load-bearing: GetMaxSpeed() reads
	// MaxFlySpeed (600) while the mode is still MOVE_Flying and MaxWalkSpeed (800) once it is not, so
	// capping first would have quietly clipped every mantle exit to three quarters of a run.
	if (MovementMode == MOVE_Flying)
	{
		SetMovementMode(MOVE_Falling);
	}

	const float ExitSpeed = FMath::Min(MantleEntrySpeed, FMath::Max(1.f, GetMaxSpeed()));
	MantleEntrySpeed = 0.f;

	Velocity.X = ExitDirection.X * ExitSpeed;
	Velocity.Y = ExitDirection.Y * ExitSpeed;
	Velocity.Z = 0.f;
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

	// 1a-i. THE LEDGE GRACE (spec v5 §7). Refilled while grounded, bled while not, so
	//       IsGroundedForAbilities() lags the engine's own answer by LedgeGroundGraceSeconds on the
	//       way DOWN only — it can never claim the pawn is airborne when the engine says it is not.
	//       Ticked with every other clock so a replayed move advances it by the same amount.
	if (IsMovingOnGround())
	{
		GroundGraceRemaining = GetLedgeGroundGraceSeconds();
	}
	else if (GroundGraceRemaining > 0.f)
	{
		GroundGraceRemaining = FMath::Max(0.f, GroundGraceRemaining - DeltaSeconds);
	}

	// 1a-iii. THE WALL-JUMP CONTACT WINDOW (spec v8 §7). Ticked here with every other clock so a
	//         replayed move advances it by exactly the amount the original did, and cleared on the
	//         frame it expires so IsWallJumpAvailable() never has to re-test the normal's age.
	//
	//         THE LADDER CAP IS RESET BY THE GROUND, and through IsGroundedForAbilities() like every
	//         other ground test in this function — a one-frame contact blip on a ledge lip must not
	//         refill a player's wall jumps on one machine and not the other.
	if (WallJumpWindowRemaining > 0.f)
	{
		WallJumpWindowRemaining = FMath::Max(0.f, WallJumpWindowRemaining - DeltaSeconds);
		if (WallJumpWindowRemaining <= 0.f)
		{
			WallJumpNormal = FVector::ZeroVector;
			WallJumpEntryVelocity = FVector::ZeroVector;
		}
	}
	if (IsGroundedForAbilities() && WallJumpsSinceGround != 0)
	{
		WallJumpsSinceGround = 0;
	}

	// 1a-ii. THE MANTLE CLOCK. Advanced before the activations below, like every other ability, so a
	//        pull-up that finishes this frame stops driving velocity this frame.
	if (MantleCooldownRemaining > 0.f)
	{
		MantleCooldownRemaining = FMath::Max(0.f, MantleCooldownRemaining - DeltaSeconds);
	}
	if (MantleTimeRemaining > 0.f)
	{
		MantleTimeRemaining = FMath::Max(0.f, MantleTimeRemaining - DeltaSeconds);

		// Early out when the destination is reached: the timer is a budget, not a schedule, and
		// holding a pawn in MOVE_Flying for another 100ms after it is already standing on the ledge
		// is exactly the "glitchy" the report is about. Distance-based, so it fires on the same frame
		// on both machines.
		const bool bArrived = (UpdatedComponent != nullptr)
			&& FVector::DistSquared(UpdatedComponent->GetComponentLocation(), MantleTargetLocation) <= FMath::Square(6.f);

		if (MantleTimeRemaining <= 0.f || bArrived || MovementMode == MOVE_None)
		{
			EndMantle();
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
#if !UE_BUILD_SHIPPING
		const FVector PreExitVelocity = Velocity;
#endif
		ApplyDashExitSpeed();

#if !UE_BUILD_SHIPPING
		if (IsDashDebugEnabled())
		{
			// The Z column is the spec v7 §5 exit clamp doing its job: a vertical dash arrives here
			// carrying the whole DashSpeed upward and must leave carrying at most JumpZVelocity.
			UE_LOG(LogTraceGame, Display,
				TEXT("DASH %s  exit: pre=(%7.1f,%7.1f,%7.1f) post=(%7.1f,%7.1f,%7.1f) "
				     "planarLimit=%6.1f zLimit=%6.1f mode=%d"),
				*GetNameSafe(CharacterOwner),
				PreExitVelocity.X, PreExitVelocity.Y, PreExitVelocity.Z,
				Velocity.X, Velocity.Y, Velocity.Z,
				FMath::Max(1.f, GetMaxSpeed()) * GetDashExitSpeedMultiplier(),
				GetDashExitVerticalSpeedLimit(), static_cast<int32>(MovementMode.GetValue()));
		}
#endif
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
		// SPEC v7 §5: all three axes now, through the same writer the launch uses. Re-asserting Z is
		// what makes a vertical dash a straight line for its window instead of a gravity arc — the
		// same "on rails" property the horizontal dash has always had, extended to the axis the dash
		// is now allowed to use. Gravity resumes the instant the window closes.
		ApplyDashVelocity();
	}

	// 3. Crouch: slide on the ground, fast-fall in the air. One key, resolved by where the pawn is.
	//
	//    A press that cannot be honoured yet (mid-dash, or airborne) is buffered rather than thrown
	//    away — see SlideBufferRemaining. The buffer is charged from the press EDGE only, so holding
	//    the key can never chain slides. It is also what makes "air-strafe, then slide the instant
	//    you touch down" a single input instead of a frame-perfect one.
	//
	//    LEDGE GRACE (spec v5 §7): every ground test in this section goes through
	//    IsGroundedForAbilities() rather than IsMovingOnGround(). A capsule crossing the lip of a
	//    raised section loses and regains contact for a frame or two, and the client and the server
	//    do it on different frames — so the raw test made one machine fire a landing (or a fast-fall,
	//    or an EndSlide) that the other never did. The grace is saved-move state, so a replay
	//    resolves the blip exactly as the original did.
	const bool bOnGroundNow = IsGroundedForAbilities();
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
		// SPEC v5 §3: THE KEY NO LONGER ENDS A SLIDE. One press bought SlideDuration seconds and the
		// player gets all of them — releasing crouch, or a bot's hold timer expiring, is now simply
		// ignored, which is what "trigger once, like an ability" means. The old
		// `(!bCrouchHeld && !bCommitted)` clause and the partial commit window it needed are both
		// gone; every slide is committed for its whole length.
		//
		// ONE EXIT LEFT HERE: leaving the ground, because a slide is a ground state and the floor is
		// what it is sliding on. Through IsGroundedForAbilities(), not IsMovingOnGround(), so a
		// one-frame contact blip on a ledge lip cannot amputate a slide on one machine and not the
		// other — see the ledge diagnosis in the header. (The other exits live elsewhere and are
		// unchanged: the duration expiring, the decay below, a dash, and a slide-jump.)
		if (!IsGroundedForAbilities())
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
	else if (bSlidePressedThisMove && !bOnGroundNow && MantleTimeRemaining <= 0.f && MovementMode != MOVE_None)
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

	// 3b. THE MANTLE (spec v5 §7). Last of the activations, and deliberately so: it is the fallback
	//     for "this jump did not clear the ledge", so anything the player explicitly asked for on
	//     this frame — a dash, a slide, a fast-fall — gets first refusal.
	//
	//     Attempted every airborne move rather than on an input edge, because the player's input for
	//     "get on top of that" is the jump they already made plus the stick they are already holding
	//     into the wall. TryBeginMantle() is cheap to refuse (two int compares before any trace) and
	//     runs three traces only once it is genuinely airborne, moving, and pushing forward.
	if (MantleTimeRemaining <= 0.f && CanAttemptMantle())
	{
		// OldVelocity, not Velocity: see the note at the top of TryBeginMantle. The frame the capsule
		// meets the ledge is the frame the collision has already zeroed the planar velocity, and the
		// approach speed is the only honest measure of "was I moving at that thing".
		TryBeginMantle(OldVelocity);
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

	// Through the grace, like every other ground test in this function: the landing transition is
	// derived from this bit, and a ledge blip that flipped it would fake a landing (and with a held
	// crouch key, a whole slide) on one machine and not the other.
	bWasAirborneLastMove = (MovementMode != MOVE_None && !IsGroundedForAbilities()) ? 1 : 0;

#if !UE_BUILD_SHIPPING
	// SPEC v8 §5 — THE LIVE CHARGE COUNTS, ON WHICHEVER MACHINE IS RUNNING THIS PAWN.
	//
	// Not on a replayed move: a correction replays several moves in one frame and would print the same
	// second several times with rewound counts, which reads as the pool flickering when it is not.
	if (IsDashPoolDebugEnabled() && CharacterOwner != nullptr && !CharacterOwner->bClientUpdating)
	{
		if (const UWorld* PoolWorld = GetWorld())
		{
			const float Now = static_cast<float>(PoolWorld->GetTimeSeconds());
			if (Now >= DashPoolDebugNextLogTime)
			{
				DashPoolDebugNextLogTime = Now + 1.f;

				const ATraceCharacter* PoolCharacter = Cast<ATraceCharacter>(CharacterOwner);
				const UTraceSettings& PoolSettings = UTraceSettings::Get();

				UE_LOG(LogTraceGame, Display,
					TEXT("DASHPOOL %-16s netMode=%d role=%d local=%d carrier=%d | charges=%d/%d lastMax=%d "
					     "refill=%5.2f dashLeft=%5.2f | cfg base=%d carrierExtra=%d window=%.2f"),
					*GetNameSafe(CharacterOwner), static_cast<int32>(GetNetMode()),
					static_cast<int32>(CharacterOwner->GetLocalRole()),
					CharacterOwner->IsLocallyControlled() ? 1 : 0,
					(PoolCharacter != nullptr && PoolCharacter->IsCarrier()) ? 1 : 0,
					DashCharges, GetMaxDashCharges(), LastMaxDashCharges,
					DashRechargeRemaining, DashTimeRemaining,
					PoolSettings.BaseDashCharges, PoolSettings.CarrierExtraDashCharges,
					GetDashRechargeWindow());
			}
		}
	}

	TickMomentumMeasure(DeltaSeconds);
	TickLedgeTest(DeltaSeconds);
	TickDashPitchTest(DeltaSeconds);
	TickWallJumpTest(DeltaSeconds);
	TickCarrierChargeTest(DeltaSeconds);
#endif
}

float UTraceCharacterMovementComponent::GetMaxSpeed() const
{
	if (IsDashing())
	{
		return GetDashSpeed();
	}

	// The mantle drives Velocity directly from CalcVelocity and never asks for an acceleration, so
	// this exists only so that anything else sampling the pawn's speed limit mid-pull-up (the HUD,
	// the anim layer, a bot) is not told the pawn is limited to MaxFlySpeed, which is a number this
	// project never tuned and which the mantle happily exceeds on a tall ledge.
	if (IsMantling() && MantleTotalTime > 0.f)
	{
		return FMath::Max(Super::GetMaxSpeed(), GetMantleMaxHeightUU() / MantleTotalTime);
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

// --- The rubber-band instrument (spec v5 §7) ----------------------------------------------------
//
// "Feels like rubber banding" is a claim about the network. This is what turns it into a number:
// every server correction the owning client receives, with the position error that triggered it and
// the pawn's state at the time. At Display, because a diagnostic nobody can see has twice been read
// as a dead mechanic in this project.

int32 GTraceMoveCorrections = 0;
static FAutoConsoleVariableRef CVarTraceMoveCorrections(
	TEXT("Trace.MoveCorrections"),
	GTraceMoveCorrections,
	TEXT("Dev only. Non-zero logs every server movement correction this client receives, with the "
	     "position error and the movement state, so ledge desyncs can be counted instead of described."),
	ECVF_Cheat);

static bool AreMoveCorrectionsLogged()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceMoveCorrections"));
	return bFromCommandLine || GTraceMoveCorrections != 0;
}

int32 GTraceMoveKitFakeCarrier = 0;
static FAutoConsoleVariableRef CVarTraceMoveKitFakeCarrier(
	TEXT("Trace.MoveKitFakeCarrier"),
	GTraceMoveKitFakeCarrier,
	TEXT("Dev only. Non-zero pretends this pawn is carrying the Core for the purposes of the dash "
	     "charge pool, so the carrier's extra charge can be exercised without a Core."),
	ECVF_Cheat);

/**
 * SPEC v8 §1. The A/B switch for the dash-aim replay fix, so "before and after" is one build.
 *
 * 1 restores the spec v7 behaviour exactly: the replayed dash composes its direction from the base
 * class's SavedControlRotation, which FSavedMove_Character::PostUpdate(PostUpdate_Replay) has already
 * overwritten with the live mouse by the time a SECOND correction replays the same move. 0 (the
 * default) uses FSavedMove_Trace::SavedDashAimRotation, which is recorded once and is immutable.
 *
 * A cvar rather than two builds because the measurement has to be corrections-per-dash on a CLIENT at
 * 40ms, and two separately-built clients are two different populations of network jitter.
 */
/**
 * How many times -TraceDashPitchTest walks its seven-phase list. See DashPitchTestCycle in the header:
 * one lap is seven dashes, and seven dashes cannot separate a prediction change from an afternoon of
 * machine load.
 */
int32 GTraceDashPitchTestCycles = 4;
static FAutoConsoleVariableRef CVarTraceDashPitchTestCycles(
	TEXT("Trace.DashPitchTestCycles"),
	GTraceDashPitchTestCycles,
	TEXT("Dev only. Laps of the -TraceDashPitchTest phase list. 7 dashes per lap; the default 4 gives 28."),
	ECVF_Cheat);

int32 GTraceDashLegacyAimReplay = 0;
static FAutoConsoleVariableRef CVarTraceDashLegacyAimReplay(
	TEXT("Trace.DashLegacyAimReplay"),
	GTraceDashLegacyAimReplay,
	TEXT("Dev only. 1 restores the spec v7 dash-replay aim source (the base class's SavedControlRotation, "
	     "which the replay pass stomps) so the fix can be A/B'd against it in one build."),
	ECVF_Cheat);

/**
 * Live charge-pool readout (spec v8 §5), once a second, per locally-controlled pawn.
 *
 * "The two dash charges aren't working anymore, for the carrier" is a claim about two integers, and
 * the only place either of them is true or false is a running game — on a CLIENT, while carrying,
 * where the carrier bit is replicated rather than authoritative. This prints both, plus the maximum
 * the pool thinks it has and the settings that produced it.
 */
int32 GTraceDashPoolDebug = 0;
static FAutoConsoleVariableRef CVarTraceDashPoolDebug(
	TEXT("Trace.DashPoolDebug"),
	GTraceDashPoolDebug,
	TEXT("Dev only. Non-zero logs the live dash charge pool (charges / max / carrier bit / refill "
	     "clock) once a second for every locally-controlled pawn."),
	ECVF_Cheat);

static bool IsDashPoolDebugEnabled()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceDashPoolDebug"));
	return bFromCommandLine || GTraceDashPoolDebug != 0;
}

void UTraceCharacterMovementComponent::TickMomentumMeasure(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceMoveMeasure"));
	if (!bEnabled || CharacterOwner == nullptr)
	{
		return;
	}

	// THE AUTHORITY'S OWN PAWN ONLY. The schedule below advances on the simulation's own delta, and a
	// replayed move would advance it twice — so this must never run on an autonomous proxy.
	//
	// It used to say `GetNetMode() != NM_Standalone`, and spec v5 §0 silently switched it off: PLAY
	// now appends `listen` to the travel URL, so a solo session is an NM_ListenServer and every
	// measurement in this harness stopped being taken. HasAuthority() && IsLocallyControlled() is the
	// condition that was actually meant — it is true for a standalone player and for a listen host,
	// and false for exactly the case (a client replaying corrections) the restriction exists for.
	if (!CharacterOwner->HasAuthority() || !CharacterOwner->IsLocallyControlled()
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

		// --- THE AIR-STRAFE CURVE, AS A TABLE (spec v5 §1) ---------------------------------------
		//
		// The curve is a pure function, so it can be printed rather than inferred from a trajectory.
		// This is "the new curve with measured numbers at several input speeds" in one line per
		// speed: what fraction of a strafe's gain survives, and what one 60Hz frame of perpendicular
		// input is actually worth at that speed. The trajectory phases below then confirm the table.
		UE_LOG(LogTraceGame, Display,
			TEXT("MEASURE AIRCAP curve: falloff=%d soft=%.0f hard=%.0f exp=%.2f hardCapOn=%d"),
			IsAirStrafeFalloffEnabled() ? 1 : 0, GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(),
			GetAirStrafeFalloffExponent(), IsAirStrafeHardCapEnabled() ? 1 : 0);
		for (const float SampleSpeed : { 600.f, 700.f, 800.f, 835.f, 900.f, 950.f, 1000.f, 1036.f,
		                                 1100.f, 1150.f, 1200.f, 1250.f, 1300.f, 1400.f })
		{
			// One 1/60s frame of perfectly perpendicular input: the projection is 0, so the whole
			// AirAcceleration allowance is available and lands sideways, giving sqrt(v^2 + a^2).
			const float Allowance = FMath::Min(GetAirAcceleration() / 60.f,
				FMath::Min(GetMaxAirSpeed(), GetAirMaxWishSpeed()));
			const float RawFrameGain = FMath::Sqrt(SampleSpeed * SampleSpeed + Allowance * Allowance) - SampleSpeed;
			const float Scale = GetAirStrafeGainScale(SampleSpeed);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRCAP   v=%6.0f uu/s  gainScale=%.4f  frameGain %5.2f -> %5.2f uu/s "
				     "(%6.1f -> %6.1f uu/s per second of strafing)"),
				SampleSpeed, Scale, RawFrameGain, RawFrameGain * Scale,
				RawFrameGain * 60.f, RawFrameGain * Scale * 60.f);
		}
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
			Advance(28);
		}
		break;

	// =============================================================================================
	// 28-29. THE LONG STRAFE — spec v5 §1's actual question.
	//
	// Phase 2 above measures 0.40 s of strafing, which is what the Demo 5 baseline (835 -> 1036) was
	// taken over. But "how much momentum can be gained" is not a question about 0.4 s: it is about
	// what happens when a player strafes for the whole of a long jump, and then does it again off the
	// next one. So this drops the pawn from far enough up to strafe for three full seconds and prints
	// the speed every quarter of a second. Uncapped, that curve keeps climbing to MaxAirSpeed and the
	// pawn lands well over the ground limit every time; capped, it flattens onto the hard cap.
	// =============================================================================================
	case 28:
		if (UpdatedComponent != nullptr && CharacterOwner != nullptr)
		{
			const FVector Here = UpdatedComponent->GetComponentLocation();
			CharacterOwner->SetActorLocation(FVector(0.f, 0.f, Here.Z + 6000.f), false, nullptr, ETeleportType::TeleportPhysics);
			// 835 uu/s: the exact entry speed the Demo 5 baseline was measured from.
			Velocity = FVector(835.f, 0.f, 0.f);
			SetMovementMode(MOVE_Falling);
			MeasureMarkA = 835.f;
			MeasureMarkB = 0.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRCAP-LONG: dropped at planar=835 uu/s (the Demo 5 baseline entry); "
				     "strafing continuously for 3s. Demo 5 reached 1036 after 0.40s and kept climbing."));
			Advance(29);
		}
		break;

	case 29:
	{
		const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, TravelDirection).GetSafeNormal();
		CharacterOwner->AddMovementInput(Perpendicular, 1.f);

		// A sample every 0.25s, so the curve is readable as a table rather than as 180 log lines.
		if (MeasurePhaseTime >= MeasureMarkB + 0.25f)
		{
			MeasureMarkB = MeasurePhaseTime;
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRCAP-LONG t=%.2fs planar=%7.1f uu/s (turned %5.1f deg, gainScale here %.4f)"),
				MeasurePhaseTime, PlanarSpeed, TurnedDegrees(), GetAirStrafeGainScale(PlanarSpeed));
		}

		if (MeasurePhaseTime > 3.0f || IsMovingOnGround())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRCAP-LONG RESULT: 835 -> %.0f uu/s over %.2fs of continuous strafing "
				     "(%.1f%% of entry; soft cap %.0f, hard cap %.0f, falloff=%d hardCap=%d). "
				     "Vector turned %.0f deg, so the TURN still costs nothing."),
				PlanarSpeed, MeasurePhaseTime, 100.f * PlanarSpeed / 835.f,
				GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(),
				IsAirStrafeFalloffEnabled() ? 1 : 0, IsAirStrafeHardCapEnabled() ? 1 : 0,
				TurnedDegrees());
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE ---- end"));
			Advance(30);
		}
		break;
	}

	default:
		break;
	}
}

// =================================================================================================
// THE RUBBER-BAND INSTRUMENT AND THE LEDGE TEST (spec v5 §7)
// =================================================================================================

void UTraceCharacterMovementComponent::OnClientCorrectionReceived(
	FNetworkPredictionData_Client_Character& ClientData, float TimeStamp, FVector NewLocation,
	FVector NewVelocity, FMovementBaseInterfaceData* NewMovementBaseInterfaceData, FName NewBaseBoneName,
	bool bHasBase, bool bBaseRelativePosition, uint8 ServerMovementMode, FVector ServerGravityDirection)
{
	// The error the server is about to correct, measured BEFORE Super applies it — afterwards the
	// pawn is already at the corrected position and the number is zero.
	const FVector Before = (UpdatedComponent != nullptr) ? UpdatedComponent->GetComponentLocation() : NewLocation;
	const float PositionError = static_cast<float>(FVector::Dist(Before, NewLocation));
	const float VelocityError = static_cast<float>(FVector::Dist(Velocity, NewVelocity));
	const uint8 LocalMode = static_cast<uint8>(MovementMode.GetValue());

	++CorrectionCount;
	CorrectionErrorTotal += PositionError;
	CorrectionErrorWorst = FMath::Max(CorrectionErrorWorst, PositionError);

	// SPEC v8 §1. Attribute this correction to a dash if one is live or recently ended. See
	// BeginDash() for why the window outlives the dash and why replays do not count as new dashes.
	const UWorld* CorrectionWorld = GetWorld();
	const bool bInDashWindow = (CorrectionWorld != nullptr)
		&& (static_cast<float>(CorrectionWorld->GetTimeSeconds()) <= DashNetAttributionUntil);
	if (bInDashWindow)
	{
		++DashNetCorrectionsInDash;
		DashNetCorrectionErrorInDash += PositionError;
	}

	// SPEC v8 §7. The same attribution for the wall jump: "fully client-predicted" is the claim, and a
	// correction landing inside the launch window is what falsifies it.
	if (CorrectionWorld != nullptr
		&& static_cast<float>(CorrectionWorld->GetTimeSeconds()) <= WallJumpAttributionUntil)
	{
		++WallJumpCorrectionsInWindow;
	}

	if (AreMoveCorrectionsLogged())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("CORRECTION %-16s #%d t=%.3f posErr=%7.2fuu velErr=%7.1fuu/s mode(local=%d server=%d) "
			     "z=%.1f grounded=%d grace=%.3f mantle=%.3f slide=%.3f | mean=%.2f worst=%.2f"),
			*GetNameSafe(CharacterOwner), CorrectionCount, TimeStamp, PositionError, VelocityError,
			static_cast<int32>(LocalMode), static_cast<int32>(ServerMovementMode), Before.Z,
			IsMovingOnGround() ? 1 : 0, GroundGraceRemaining, MantleTimeRemaining, SlideTimeRemaining,
			CorrectionErrorTotal / FMath::Max(1, CorrectionCount), CorrectionErrorWorst);

		// The v8 §1 line. Printed next to the correction it describes so "was this one a dash?" is
		// answerable per correction rather than only in the summary.
		UE_LOG(LogTraceGame, Display,
			TEXT("DASHNET   %-16s inDash=%d | dashes=%d corrInDash=%d rate=%.2f/dash meanDashErr=%.2fuu"),
			*GetNameSafe(CharacterOwner), bInDashWindow ? 1 : 0, DashNetDashCount,
			DashNetCorrectionsInDash,
			DashNetCorrectionsInDash / static_cast<float>(FMath::Max(1, DashNetDashCount)),
			DashNetCorrectionErrorInDash / static_cast<float>(FMath::Max(1, DashNetCorrectionsInDash)));
	}

	Super::OnClientCorrectionReceived(ClientData, TimeStamp, NewLocation, NewVelocity,
		NewMovementBaseInterfaceData, NewBaseBoneName, bHasBase, bBaseRelativePosition,
		ServerMovementMode, ServerGravityDirection);
}

void UTraceCharacterMovementComponent::TickLedgeTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceLedgeTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// Locally controlled only — but in ANY net mode, unlike TickMomentumMeasure. The whole point is
	// to measure what a networked CLIENT experiences at a ledge, so restricting this to standalone
	// would measure everything except the bug.
	if (!CharacterOwner->IsLocallyControlled()
		|| Cast<APlayerController>(CharacterOwner->GetController()) == nullptr)
	{
		return;
	}

	UWorld* TestWorld = GetWorld();
	if (TestWorld == nullptr)
	{
		return;
	}

	// --- Ground-state flip counter, running the whole time -------------------------------------
	//
	// THE PRIMARY MEASUREMENT. A pawn that runs onto a raised section should change ground state
	// twice: airborne on the jump, grounded on the landing. Every flip beyond that is the capsule
	// oscillating on the lip, and every oscillation is a chance for the client and the server to
	// disagree about which velocity model to run.
	const bool bGroundedNow = IsMovingOnGround();
	if (LedgeTestTime >= 0.f && bGroundedNow != (bLedgeTestWasGrounded != 0))
	{
		++LedgeTestGroundFlips;
	}
	bLedgeTestWasGrounded = bGroundedNow ? 1 : 0;

	if (LedgeTestTime < 0.f)
	{
		if (TestWorld->GetTimeSeconds() < 3.f || !IsMovingOnGround())
		{
			return;
		}
		LedgeTestTime = 0.f;
		LedgeTestPhase = 0;
		LedgeTestPhaseTime = 0.f;
		LedgeTestGroundFlips = 0;
		LedgeTestMantles = 0;
		LedgeTestRun = 0;
		CorrectionCount = 0;
		CorrectionErrorTotal = 0.f;
		CorrectionErrorWorst = 0.f;

		// Run toward the middle of the field, same rule as the momentum harness: a fixed world axis
		// walks straight into an endzone wall from a spawn pad.
		LedgeTestRunDirection = FVector::ForwardVector;
		FVector TowardCentre = -UpdatedComponent->GetComponentLocation();
		TowardCentre.Z = 0.f;
		if (TowardCentre.Normalize())
		{
			LedgeTestRunDirection = TowardCentre;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE ---- begin. netMode=%d role=%d mantle=%d grace=%.3f perch=%.1f jumpZ=%.0f"),
			static_cast<int32>(GetNetMode()), static_cast<int32>(CharacterOwner->GetLocalRole()),
			IsMantleEnabled() ? 1 : 0, GetLedgeGroundGraceSeconds(), PerchRadiusThreshold, JumpZVelocity);
	}

	LedgeTestTime += DeltaSeconds;
	LedgeTestPhaseTime += DeltaSeconds;

	auto Advance = [this](int32 NextPhase)
	{
		LedgeTestPhase = NextPhase;
		LedgeTestPhaseTime = 0.f;
	};

	switch (LedgeTestPhase)
	{
	// --- 0. BUILD A LEDGE -----------------------------------------------------------------------
	//
	// The harness makes its own geometry on purpose. The arena's raised sections are real ledges,
	// but where they are depends on the arena builder's tuning, and a repeatable measurement of a
	// prediction bug has to hit the SAME lip every run. 176 uu is one player height — exactly the
	// class of structure the report is about, and exactly the height a 640 uu/s jump (apex 209 uu)
	// only just clears, which is why landing on its edge is so common.
	//
	// NOT REPLICATED, AND QUANTISED. Each machine spawns its OWN copy, at a position rounded to a
	// 50 uu grid: replicating an AStaticMeshActor spawned at runtime would not carry its mesh or its
	// scale to the client, and a block placed from an un-rounded pawn position would sit a couple of
	// uu apart on the two machines — which would MANUFACTURE the desync this run exists to measure.
	// Rounding makes both ends agree exactly as long as they agree to within 25 uu, which they must,
	// or the correction had nothing to do with the ledge.
	case 0:
	{
		if (LedgeTestBlock.IsValid())
		{
			Advance(1);
			break;
		}

		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (Cube == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("LEDGE could not load /Engine/BasicShapes/Cube"));
			Advance(9);
			break;
		}

		const FVector Here = UpdatedComponent->GetComponentLocation();
		const float FeetZ = Here.Z - 88.f;

		// "-TraceLedgeHeight=N", default 260.
		//
		// 260 AND NOT 176, AND THE FIRST RUN OF THIS HARNESS IS WHY. A 176 uu block is one player
		// height, which is the arena's smallest raised section — but a 640 uu/s jump apexes at
		// 640^2/(2*980) = 209 uu, so the pawn sails clean over it and never touches the face at all.
		// Eight consecutive runs measured exactly that: canMantle=1 the whole way, feet passing 207 uu
		// with the block's 176 uu top already below them, no wall in reach, no mantle, and no bug
		// either. To exercise a LEDGE CLIMB the block has to be taller than the jump, and 260 is the
		// first round number that is. The 176 case is a different test (landing on the lip) and is
		// what PerchRadiusThreshold and the ledge grace are for.
		float BlockHeight = 260.f;
		FParse::Value(FCommandLine::Get(), TEXT("TraceLedgeHeight="), BlockHeight);
		BlockHeight = FMath::Clamp(BlockHeight, 60.f, 800.f);

		auto Quantise = [](double Value) { return FMath::GridSnap(Value, 50.0); };

		// --- PICK AN OPEN STRETCH OF FLOOR, AND PROVE IT IS OPEN --------------------------------
		//
		// THIS HARNESS PREVIOUSLY MEASURED A COLLISION WITH THE ARENA, NOT THE MANTLE, and every
		// mantle number reported off it — "1 mantle in 8 runs", "2 in 8" — is void. It ran from
		// wherever the pawn happened to spawn and put its block 900 uu along the line to the field
		// centre, without ever checking anything was there. Spawn pads are inside the endzone, so on
		// a 33600-long field the block landed at X=-14700, i.e. inside the mode A endzone structure
		// (half-extent 1200 about the end line, so X <= -14400 is solid). MEASURED: the pawn jammed
		// against the endzone 300 uu short of its own test block and stayed there — z pinned at 90.1,
		// feet 2.1, planar 800 uu/s going nowhere, dist to the face stuck around 300 while the mantle
		// probe (reach 104 uu) reported "degenerate, penetrating=1 dist=0.00" against the endzone it
		// was pressed into. Eight runs, eight collisions, zero arrivals at the ledge.
		//
		// This is precisely the failure TickMomentumMeasure documents for itself ("the first two
		// attempts sprinted into an endzone wall and reported a jump that lost 750 uu/s, which was a
		// collision") — the momentum harness teleports to the middle of the field to escape it, and
		// this one never got the same treatment.
		//
		// So: run down the long axis from a candidate on the centre line, and accept a candidate only
		// once three sweeps say it is genuinely clear — standing room at the mark, an unobstructed
		// run-up to where the block will stand, and room for the block itself. Fixed round
		// coordinates rather than pawn-relative ones also make the block land on the SAME world
		// position on the host and on a client regardless of which pad each spawned at, which is what
		// the quantisation was reaching for and could not deliver from a spawn-relative origin.
		const UCapsuleComponent* TestCapsule = CharacterOwner->GetCapsuleComponent();
		const float TestRadius = (TestCapsule != nullptr) ? TestCapsule->GetScaledCapsuleRadius() : 34.f;
		const float TestHalfHeight = (TestCapsule != nullptr) ? TestCapsule->GetScaledCapsuleHalfHeight() : 88.f;
		const FCollisionShape RunShape = FCollisionShape::MakeCapsule(TestRadius, TestHalfHeight);

		FCollisionQueryParams ClearParams;
		FCollisionResponseParams ClearResponse;
		InitCollisionParams(ClearParams, ClearResponse);
		ClearParams.bTraceComplex = false;
		const ECollisionChannel ClearChannel = UpdatedComponent->GetCollisionObjectType();

		// Down the long axis, on the centre line: the flat playfield is 6600 wide about Y=0, so Y=0 is
		// the one line guaranteed clear of the banks that start at Y=+/-3300.
		const FVector RunDirection = FVector::ForwardVector;
		const double StandZ = Quantise(Here.Z);
		const double BlockBottomZ = StandZ - TestHalfHeight;

		bool bFoundSpot = false;
		FVector ChosenStart = FVector(Quantise(Here.X), Quantise(Here.Y), Here.Z);
		for (const double CandidateX : { -6000.0, -4000.0, -2000.0, 0.0, 2000.0, 4000.0, 6000.0, -8000.0, 8000.0 })
		{
			const FVector Candidate(CandidateX, 0.0, StandZ);
			const FVector BlockSpot = Candidate + RunDirection * 900.f;

			// 1. Room to stand at the mark.
			if (TestWorld->OverlapBlockingTestByChannel(Candidate, FQuat::Identity, ClearChannel,
				RunShape, ClearParams, ClearResponse))
			{
				continue;
			}

			// 2. An unobstructed run-up. Without this the pawn can start in the open and still meet a
			//    pillar on the way, which is the same void measurement in a different place.
			FHitResult PathHit;
			if (TestWorld->SweepSingleByChannel(PathHit, Candidate, BlockSpot, FQuat::Identity,
				ClearChannel, RunShape, ClearParams, ClearResponse))
			{
				continue;
			}

			// 3. Room for the block. Lifted 2 uu so the floor it stands on is not read as an overlap.
			const FCollisionShape BlockVolume = FCollisionShape::MakeBox(
				FVector(300.f, 300.f, static_cast<float>(BlockHeight * 0.5)));
			const FVector BlockVolumeCentre(BlockSpot.X, BlockSpot.Y, BlockBottomZ + BlockHeight * 0.5 + 2.0);
			if (TestWorld->OverlapBlockingTestByChannel(BlockVolumeCentre, FQuat::Identity, ClearChannel,
				BlockVolume, ClearParams, ClearResponse))
			{
				continue;
			}

			ChosenStart = Candidate;
			bFoundSpot = true;
			break;
		}

		if (!bFoundSpot)
		{
			// Loud, because the alternative is another pass reporting mantle counts taken from a pawn
			// wedged against a wall.
			UE_LOG(LogTraceGame, Warning,
				TEXT("LEDGE found no clear stretch on the centre line - the numbers below measure the "
				     "arena, not the mantle. Falling back to %s."), *ChosenStart.ToCompactString());
		}

		LedgeTestRunDirection = RunDirection;
		CharacterOwner->SetActorLocation(ChosenStart, false, nullptr, ETeleportType::TeleportPhysics);
		Velocity = FVector::ZeroVector;

		const FVector BlockCentre(Quantise(ChosenStart.X + RunDirection.X * 900.f),
			Quantise(ChosenStart.Y + RunDirection.Y * 900.f),
			BlockBottomZ + BlockHeight * 0.5);

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Block = TestWorld->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), BlockCentre, FRotator::ZeroRotator, SpawnParams);
		if (Block == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("LEDGE could not spawn the test block"));
			Advance(9);
			break;
		}

		if (UStaticMeshComponent* BlockMesh = Block->GetStaticMeshComponent())
		{
			BlockMesh->SetMobility(EComponentMobility::Movable);
			BlockMesh->SetStaticMesh(Cube);
			// The engine cube is 100 uu. 600 uu across so a slightly off run still meets the same face.
			BlockMesh->SetWorldScale3D(FVector(6.f, 6.f, BlockHeight / 100.f));
			BlockMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		}

		LedgeTestBlock = Block;
		LedgeTestStart = ChosenStart;
		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE built a %.0fuu block at %s (top z=%.1f, feet z=%.1f, jump apex is %.0fuu), running from %s "
			     "(clear stretch found=%d)"),
			BlockHeight, *BlockCentre.ToCompactString(), BlockCentre.Z + BlockHeight * 0.5f, FeetZ,
			(JumpZVelocity * JumpZVelocity) / (2.f * FMath::Max(1.f, GetGravityZ() * -1.f)),
			*LedgeTestStart.ToCompactString(), bFoundSpot ? 1 : 0);
		Advance(1);
		break;
	}

	// --- 1. RESET to the run-up mark ------------------------------------------------------------
	case 1:
		CharacterOwner->SetActorLocation(LedgeTestStart + FVector(0.f, 0.f, 20.f), false, nullptr, ETeleportType::TeleportPhysics);
		Velocity = FVector::ZeroVector;
		UE_LOG(LogTraceGame, Display, TEXT("LEDGE run %d: reset to %s"), LedgeTestRun + 1, *LedgeTestStart.ToCompactString());
		Advance(2);
		break;

	// --- 2. RUN AT IT, AND JUMP AT THE EDGE -----------------------------------------------------
	//
	// The jump is triggered on DISTANCE TO THE FACE, not on a stopwatch. "Jumping on the edge of a
	// raised section" is a specific input — a jump made close enough that the capsule arrives at the
	// lip rather than sailing over it — and a fixed 0.85s produced a different jump every time the
	// frame rate moved, which is how the first version of this harness measured nothing at all.
	case 2:
	{
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);

		const AActor* Block = LedgeTestBlock.Get();
		const float DistanceToBlock = (Block != nullptr)
			? static_cast<float>(FVector::Dist2D(UpdatedComponent->GetComponentLocation(), Block->GetActorLocation())) - 300.f
			: 0.f;

		if (Block != nullptr && DistanceToBlock < 340.f && IsMovingOnGround())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE run %d: JUMP at %.0fuu from the face, planar=%.0f uu/s"),
				LedgeTestRun + 1, DistanceToBlock, GetPlanarSpeed());
			CharacterOwner->Jump();
			Advance(3);
		}
		else if (LedgeTestPhaseTime > 4.f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("LEDGE run %d: never reached the block (dist=%.0f, planar=%.0f, grounded=%d)"),
				LedgeTestRun + 1, DistanceToBlock, GetPlanarSpeed(), IsMovingOnGround() ? 1 : 0);
			Advance(4);
		}
		break;
	}

	// --- 3. HOLD FORWARD THROUGH THE CONTACT ----------------------------------------------------
	//
	// One log line per frame here, and only here: about thirty lines per run, which is the whole
	// contact event at full resolution. Without it "the mantle did not fire" is a dead end — this is
	// what says whether the pawn was even airborne and pushing when it met the lip.
	case 3:
	{
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);
		CharacterOwner->StopJumping();

		const AActor* Block = LedgeTestBlock.Get();
		const FVector Here = UpdatedComponent->GetComponentLocation();
		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE   contact t=%.3f z=%7.1f feet=%7.1f planar=%6.0f velZ=%7.0f falling=%d "
			     "canMantle=%d mantling=%d dist=%6.0f"),
			LedgeTestPhaseTime, Here.Z, Here.Z - 88.f, GetPlanarSpeed(), Velocity.Z,
			IsFalling() ? 1 : 0, CanAttemptMantle() ? 1 : 0, IsMantling() ? 1 : 0,
			Block != nullptr ? static_cast<float>(FVector::Dist2D(Here, Block->GetActorLocation())) - 300.f : 0.f);

		if (IsMantling())
		{
			++LedgeTestMantles;
			Advance(4);
			break;
		}

		if (LedgeTestPhaseTime > 1.6f)
		{
			Advance(4);
		}
		break;
	}

	// --- 4. SETTLE, then report this run and go again -------------------------------------------
	case 4:
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);
		if (LedgeTestPhaseTime > 1.0f)
		{
			++LedgeTestRun;
			const FVector Here = UpdatedComponent->GetComponentLocation();
			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE run %2d: z=%7.1f (start %7.1f, +%6.1f) grounded=%d | cumulative: flips=%d "
				     "mantles=%d corrections=%d meanErr=%.2fuu worstErr=%.2fuu"),
				LedgeTestRun, Here.Z, LedgeTestStart.Z, Here.Z - LedgeTestStart.Z,
				IsMovingOnGround() ? 1 : 0, LedgeTestGroundFlips, LedgeTestMantles,
				CorrectionCount, CorrectionErrorTotal / FMath::Max(1, CorrectionCount),
				CorrectionErrorWorst);

			if (LedgeTestRun >= 8)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("LEDGE ---- end. %d runs | groundFlips=%d (2/run is the floor: one jump, one "
					     "landing) | mantles=%d | corrections=%d meanErr=%.2fuu worstErr=%.2fuu | "
					     "mantleOn=%d grace=%.3f perch=%.1f"),
					LedgeTestRun, LedgeTestGroundFlips, LedgeTestMantles, CorrectionCount,
					CorrectionErrorTotal / FMath::Max(1, CorrectionCount), CorrectionErrorWorst,
					IsMantleEnabled() ? 1 : 0, GetLedgeGroundGraceSeconds(), PerchRadiusThreshold);
				Advance(9);
			}
			else
			{
				Advance(1);
			}
		}
		break;

	default:
		break;
	}
}

// -------------------------------------------------------------------------------------------
// Trace.DashVectorTest — the measured verification of spec v7 §5
// -------------------------------------------------------------------------------------------
//
// Offline and deterministic: it calls the SHIPPING ComputeDashDirection() with synthetic
// (acceleration, aim rotation) pairs, so what it prints is the function the game runs, not a
// re-derivation of it. Yaw is pinned to 0 so every expected answer is a world axis and a wrong sign
// is obvious by eye; MaxAcceleration is used as the input magnitude because that is what
// ATraceCharacter::DoMove actually produces once UCharacterMovementComponent has scaled the stick.
//
// The row that matters most is the last pair: W-only and W+D must print the SAME |v| and the same
// reach. "add the two vectors and normalize to one dash length" is a claim about distance, and this
// is the only place it is checked.

// -------------------------------------------------------------------------------------------
// -TraceDashPitchTest — spec v7 §5 measured on a real pawn
// -------------------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::TickDashPitchTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceDashPitchTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	APlayerController* TestController = Cast<APlayerController>(CharacterOwner->GetController());
	if (TestController == nullptr || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// NEVER ON A REPLAYED MOVE. OnMovementUpdated runs again for every saved move a correction
	// replays, and this harness has a clock and fires abilities — advancing it twice would schedule
	// phantom dashes that the server never saw, which is the harness manufacturing the very desync
	// it was written to look for. TickMomentumMeasure sidesteps this by refusing to run on an
	// autonomous proxy at all; this one has to run there, because a networked CLIENT predicting a
	// vertical dash is exactly the case worth measuring.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	const UWorld* TestWorld = GetWorld();
	if (TestWorld == nullptr)
	{
		return;
	}

	// One phase per row. Forward/Strafe are the W/S and A/D amounts; Pitch is where the mouse points,
	// positive up. 85 rather than 90 because APlayerController::LimitViewRotation clamps to the
	// camera manager's ViewPitchMax (89.9 by default) and a request at the clamp is a request whose
	// answer depends on the clamp.
	struct FDashPitchPhase
	{
		const TCHAR* Label;
		float Forward;
		float Strafe;
		float Pitch;
	};

	static const FDashPitchPhase Phases[] =
	{
		{ TEXT("A only,  level    "),  0.f, -1.f,  0.f },
		{ TEXT("D only,  level    "),  0.f,  1.f,  0.f },
		{ TEXT("D only,  look up45"),  0.f,  1.f, 45.f },
		{ TEXT("W only,  level    "),  1.f,  0.f,  0.f },
		{ TEXT("W only,  look up45"),  1.f,  0.f, 45.f },
		{ TEXT("W only,  look up85"),  1.f,  0.f, 85.f },
		{ TEXT("W+D,     look up45"),  1.f,  1.f, 45.f },
	};

	static const int32 NumPhases = UE_ARRAY_COUNT(Phases);

	// The dash is charged, and one charge refills over DashDuration + DashCooldown (3.68s shipped).
	// A phase shorter than that would silently measure "no dash happened" from the second row on.
	const float PhaseLength = FMath::Max(4.0f, GetDashRechargeWindow() + 0.5f);
	const float FireAt = 0.5f;      // input has to be held for a frame or two before Acceleration exists
	const float ReportAt = FireAt + 2.5f;

	if (DashPitchTestTime < 0.f)
	{
		// Wait for the match to settle and the pawn to be standing on something.
		if (TestWorld->GetTimeSeconds() < 4.f || !IsMovingOnGround())
		{
			return;
		}

		DashPitchTestTime = 0.f;
		DashPitchTestPhase = 0;
		DashPitchTestPhaseTime = 0.f;
		bDashPitchTestFired = 0;
		bDashPitchTestLogged = 0;

		// Face the middle of the field, exactly as the other two harnesses do: a fixed world axis
		// runs the pawn straight into an endzone wall from a spawn pad and measures the wall.
		FVector TowardCentre = -UpdatedComponent->GetComponentLocation();
		TowardCentre.Z = 0.f;
		DashPitchTestYaw = TowardCentre.Normalize() ? TowardCentre.Rotation().Yaw : 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("DASHPITCH ---- begin. netMode=%d speed=%.0f duration=%.3f reach=%.1fuu "
			     "exitZLimit=%.0f runYaw=%.1f"),
			static_cast<int32>(GetNetMode()), GetDashSpeed(), GetDashDuration(),
			GetDashSpeed() * GetDashDuration(), GetDashExitVerticalSpeedLimit(), DashPitchTestYaw);
	}

	if (DashPitchTestPhase >= NumPhases)
	{
		return;
	}

	DashPitchTestTime += DeltaSeconds;
	DashPitchTestPhaseTime += DeltaSeconds;

	const FDashPitchPhase& Phase = Phases[DashPitchTestPhase];

	// Aim. Held every frame: the pawn is under a PlayerController whose UpdateRotation would
	// otherwise leave the rotation wherever the last frame put it, and the whole measurement is
	// about pitch.
	//
	// SPEC v8 §1 — AND WHY THE AIM MOVES AFTER THE DASH HAS LAUNCHED.
	//
	// The v7 version of this harness held the aim rigidly still for the whole phase, and that made the
	// rubber-band it was supposed to catch INVISIBLE BY CONSTRUCTION. The legacy failure is that
	// PostUpdate(PostUpdate_Replay) stomps the move's SavedControlRotation with the aim at CORRECTION
	// time; if the aim has not moved since the dash was pressed, the stomped value equals the recorded
	// one and the broken path and the fixed path produce identical numbers. A real player is still
	// tracking a target while their dash is in the air, so the honest test is a moving aim.
	//
	// The sweep starts AFTER the launch frame (the dash direction is locked in BeginDash and must be
	// composed from the phase's stated pitch, or the DASHPITCH rows below stop measuring spec v7 §5)
	// and runs through the whole correction window, which is where the disagreement would land.
	// Deterministic in phase time, so both A/B arms sweep identically.
	const float AimSweepStart = FireAt + 0.08f;
	float AimPitch = Phase.Pitch;
	float AimYaw   = DashPitchTestYaw;
	if (DashPitchTestPhaseTime > AimSweepStart)
	{
		const float SweepTime = DashPitchTestPhaseTime - AimSweepStart;
		AimYaw   += 100.f * FMath::Sin(SweepTime * 6.0f);
		AimPitch  = FMath::Clamp(Phase.Pitch - 55.f * FMath::Sin(SweepTime * 4.0f), -80.f, 85.f);
	}
	TestController->SetControlRotation(FRotator(AimPitch, AimYaw, 0.f));

	// Hold the movement keys. Same basis ATraceCharacter::DoMove uses, so Acceleration arrives at
	// BeginDash shaped exactly as a human's would be.
	const FRotationMatrix YawBasis(FRotator(0.f, DashPitchTestYaw, 0.f));
	const FVector InputDirection =
		(YawBasis.GetUnitAxis(EAxis::X) * Phase.Forward + YawBasis.GetUnitAxis(EAxis::Y) * Phase.Strafe)
		.GetSafeNormal();
	if (!InputDirection.IsNearlyZero() && DashPitchTestPhaseTime < ReportAt)
	{
		CharacterOwner->AddMovementInput(InputDirection, 1.f);
	}

	if (DashPitchTestPhaseTime >= FireAt && bDashPitchTestFired == 0)
	{
		DashPitchTestStart = UpdatedComponent->GetComponentLocation();
		DashPitchTestPeakRise = 0.f;
		DashPitchTestLaunchVelocity = FVector::ZeroVector;
		StartDash();
		bDashPitchTestFired = 1;
	}

	if (bDashPitchTestFired != 0)
	{
		// First frame the dash is actually running is the launch this phase is measuring.
		if (IsDashing() && DashPitchTestLaunchVelocity.IsNearlyZero())
		{
			DashPitchTestLaunchVelocity = Velocity;
		}

		DashPitchTestPeakRise = FMath::Max<float>(
			DashPitchTestPeakRise,
			static_cast<float>(UpdatedComponent->GetComponentLocation().Z - DashPitchTestStart.Z));
	}

	if (DashPitchTestPhaseTime >= ReportAt && bDashPitchTestLogged == 0)
	{
		const FVector Here = UpdatedComponent->GetComponentLocation();
		const FVector Travel = Here - DashPitchTestStart;
		const FVector PlanarTravel(Travel.X, Travel.Y, 0.f);

		UE_LOG(LogTraceGame, Display,
			TEXT("DASHPITCH %s dir=(%6.3f,%6.3f,%6.3f) launchV=(%7.1f,%7.1f,%7.1f) |v|=%7.1f "
			     "peakRise=%7.1fuu planarTravel=%7.1fuu netZ=%7.1fuu grounded=%d"),
			Phase.Label, DashDirection.X, DashDirection.Y, DashDirection.Z,
			DashPitchTestLaunchVelocity.X, DashPitchTestLaunchVelocity.Y, DashPitchTestLaunchVelocity.Z,
			DashPitchTestLaunchVelocity.Size(), DashPitchTestPeakRise, PlanarTravel.Size(), Travel.Z,
			IsMovingOnGround() ? 1 : 0);

		bDashPitchTestLogged = 1;
	}

	if (DashPitchTestPhaseTime >= PhaseLength)
	{
		++DashPitchTestPhase;
		DashPitchTestPhaseTime = 0.f;
		bDashPitchTestFired = 0;
		bDashPitchTestLogged = 0;

		if (DashPitchTestPhase >= NumPhases)
		{
			// Another lap, unless the requested number of laps is done. See DashPitchTestCycle: the
			// corrections-per-dash figure this harness exists to produce is worthless at seven dashes.
			++DashPitchTestCycle;
			if (DashPitchTestCycle < FMath::Max(1, GTraceDashPitchTestCycles))
			{
				DashPitchTestPhase = 0;
				UE_LOG(LogTraceGame, Display,
					TEXT("DASHPITCH ---- cycle %d of %d complete (%d dashes so far)."),
					DashPitchTestCycle, FMath::Max(1, GTraceDashPitchTestCycles),
					DashPitchTestCycle * NumPhases);
				LogDashNetReport();
				return;
			}

			UE_LOG(LogTraceGame, Display, TEXT("DASHPITCH ---- end. %d phases x %d cycles."),
				NumPhases, DashPitchTestCycle);

			// SPEC v8 §1. The rubber-band number, printed by the harness that produced the dashes rather
			// than left to a console command nobody can type into an offscreen -game process. On a
			// listen host this reads 0.00 and means nothing (see TraceReportDashNet); on a JOINED CLIENT
			// at 40 ms it is the answer to "does the dash rubber-band".
			LogDashNetReport();
		}
	}
}

int32 UTraceCharacterMovementComponent::RunDashVectorTest() const
{
	const float DashSpeed = GetDashSpeed();
	const float DashDuration = GetDashDuration();
	const float ExitVerticalLimit = GetDashExitVerticalSpeedLimit();
	const float GravityMagnitude = FMath::Max(1.f, -GetGravityZ());
	const float InputMagnitude = FMath::Max(1.f, GetMaxAcceleration());

	UE_LOG(LogTraceGame, Display,
		TEXT("DASHVEC ---- spec v7 5, yaw pinned to 0 so +X is forward and +Y is right. "
		     "speed=%.0f duration=%.3f reach=%.1fuu exitZLimit=%.0f gravity=%.0f"),
		DashSpeed, DashDuration, DashSpeed * DashDuration, ExitVerticalLimit, GravityMagnitude);

	struct FDashVectorCase
	{
		const TCHAR* Label;
		float Forward;   // W = +1, S = -1
		float Strafe;    // D = +1, A = -1
		float Pitch;     // degrees, positive is looking UP
	};

	// Every case the task asks for, plus the degenerate ones that would silently break.
	static const FDashVectorCase Cases[] =
	{
		{ TEXT("A only,      level     "),  0.f, -1.f,   0.f },
		{ TEXT("D only,      level     "),  0.f,  1.f,   0.f },
		{ TEXT("A only,      look up 60"),  0.f, -1.f,  60.f },
		{ TEXT("D only,      look dn 60"),  0.f,  1.f, -60.f },
		{ TEXT("W only,      level     "),  1.f,  0.f,   0.f },
		{ TEXT("W only,      look up 90"),  1.f,  0.f,  90.f },
		{ TEXT("W only,      look up 45"),  1.f,  0.f,  45.f },
		{ TEXT("W only,      look dn 45"),  1.f,  0.f, -45.f },
		{ TEXT("S only,      look up 45"), -1.f,  0.f,  45.f },
		{ TEXT("W+D,         level     "),  1.f,  1.f,   0.f },
		{ TEXT("W+D,         look up 45"),  1.f,  1.f,  45.f },
		{ TEXT("W+A,         look up 45"),  1.f, -1.f,  45.f },
		{ TEXT("no input,    look up 45"),  0.f,  0.f,  45.f },
	};

	int32 Failures = 0;

	for (const FDashVectorCase& Case : Cases)
	{
		// Yaw 0: forward is +X, right is +Y. This is the same construction ATraceCharacter::DoMove
		// uses, so the acceleration fed in is shaped exactly like a real frame's.
		const FVector InAccel = FVector(Case.Forward, Case.Strafe, 0.f).GetSafeNormal() * InputMagnitude;
		const FRotator AimRotation(Case.Pitch, 0.f, 0.f);

		const FVector Direction = ComputeDashDirection(InAccel, AimRotation);
		const FVector DashVelocity = Direction * DashSpeed;
		const float Magnitude = DashVelocity.Size();

		// The normalisation claim, checked rather than described: every case must be one dash length.
		if (!FMath::IsNearlyEqual(Direction.Size(), 1.f, 1.e-4f))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("DASHVEC FAIL  %s direction is not unit length (%.6f)"),
				Case.Label, Direction.Size());
			++Failures;
		}

		// "If only A or D is held, dash horizontally only: parallel to the ground" — stated as an
		// assertion so a future edit that lets pitch leak into the strafe axis fails here loudly.
		if (FMath::IsNearlyZero(Case.Forward) && !FMath::IsNearlyZero(Case.Strafe)
			&& !FMath::IsNearlyZero(Direction.Z, 1.e-4f))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("DASHVEC FAIL  %s is strafe-only but has Z=%.6f"),
				Case.Label, Direction.Z);
			++Failures;
		}

		// Climb accounting, so "how high does a vertical dash get me" is a printed number rather than
		// an argument: the rails phase, then the clamped exit coast.
		const float RailsRise = FMath::Max(0.f, DashVelocity.Z) * DashDuration;
		const float ExitZ = FMath::Min(FMath::Max(0.f, DashVelocity.Z), ExitVerticalLimit);
		const float CoastRise = (ExitZ * ExitZ) / (2.f * GravityMagnitude);

		UE_LOG(LogTraceGame, Display,
			TEXT("DASHVEC %s dir=(%7.4f,%7.4f,%7.4f) v=(%8.1f,%8.1f,%8.1f) |v|=%7.1f "
			     "reach=%6.1fuu pitchOut=%6.1fdeg climb=%6.1f+%6.1f=%6.1fuu"),
			Case.Label, Direction.X, Direction.Y, Direction.Z,
			DashVelocity.X, DashVelocity.Y, DashVelocity.Z, Magnitude,
			Magnitude * DashDuration,
			FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Direction.Z, -1.f, 1.f))),
			RailsRise, CoastRise, RailsRise + CoastRise);
	}

	UE_LOG(LogTraceGame, Display, TEXT("DASHVEC ---- end. %d failure(s)."), Failures);
	return Failures;
}

static void TraceRunDashVectorTest()
{
	// Prefer a live pawn's component so the numbers reported are the ones the running match would
	// get; the CDO is a correct fallback because ComputeDashDirection touches no instance state
	// except in its standing-still branch, and every setting it reads is global.
	const UTraceCharacterMovementComponent* Movement = nullptr;
	if (GEngine != nullptr)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			const UWorld* ContextWorld = Context.World();
			if (ContextWorld == nullptr)
			{
				continue;
			}

			if (const APlayerController* LocalPC = ContextWorld->GetFirstPlayerController())
			{
				if (const ACharacter* LocalCharacter = Cast<ACharacter>(LocalPC->GetPawn()))
				{
					if (const UTraceCharacterMovementComponent* Found =
						Cast<UTraceCharacterMovementComponent>(LocalCharacter->GetCharacterMovement()))
					{
						Movement = Found;
						break;
					}
				}
			}
		}
	}

	if (Movement == nullptr)
	{
		Movement = GetDefault<UTraceCharacterMovementComponent>();
	}

	Movement->RunDashVectorTest();
}

void UTraceCharacterMovementComponent::LogDashNetReport() const
{
	const UWorld* ReportWorld = GetWorld();

	// netMode and role are printed because the whole point of this command is that the answer is only
	// meaningful for ROLE_AutonomousProxy on a client. A reader who forgets that can see it in the
	// line itself rather than having to remember which window they are in.
	UE_LOG(LogTraceGame, Display,
		TEXT("DASHNET REPORT %-16s netMode=%d role=%d | dashes=%d corrections(all)=%d "
		     "corrections(in dash)=%d => %.3f per dash | meanDashErr=%.2fuu "
		     "meanAllErr=%.2fuu worstErr=%.2fuu%s"),
		*GetNameSafe(CharacterOwner),
		(ReportWorld != nullptr) ? static_cast<int32>(ReportWorld->GetNetMode()) : -1,
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1,
		DashNetDashCount, CorrectionCount, DashNetCorrectionsInDash,
		DashNetCorrectionsInDash / static_cast<float>(FMath::Max(1, DashNetDashCount)),
		DashNetCorrectionErrorInDash / static_cast<float>(FMath::Max(1, DashNetCorrectionsInDash)),
		CorrectionErrorTotal / static_cast<float>(FMath::Max(1, CorrectionCount)),
		CorrectionErrorWorst,
		(CharacterOwner != nullptr && CharacterOwner->HasAuthority())
			? TEXT("  [AUTHORITY - this number is meaningless here]") : TEXT(""));
}

void UTraceCharacterMovementComponent::LogWallJumpReport() const
{
	const UWorld* ReportWorld = GetWorld();
	const float Denominator = static_cast<float>(FMath::Max(1, WallJumpCount));

	UE_LOG(LogTraceGame, Display,
		TEXT("WALLJUMP REPORT %-16s netMode=%d role=%d | jumps=%d entry=%6.0f -> launch=%6.0f uu/s "
		     "(%.1f%% carried, turned %.1fdeg, launchZ=%6.0f) maxConsecutive=%d/%d capRefusals=%d "
		     "corrections(in wall jump)=%d => %.3f per jump | hardCap=%.0f%s"),
		*GetNameSafe(CharacterOwner),
		(ReportWorld != nullptr) ? static_cast<int32>(ReportWorld->GetNetMode()) : -1,
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1,
		WallJumpCount,
		WallJumpEntrySpeedSum / Denominator,
		WallJumpLaunchSpeedSum / Denominator,
		100.f * WallJumpLaunchSpeedSum / FMath::Max(1.f, WallJumpEntrySpeedSum),
		WallJumpTurnDegreesSum / Denominator,
		WallJumpLaunchZSum / Denominator,
		WallJumpMaxConsecutiveSeen, GetWallJumpMaxConsecutive(), WallJumpCapRefusals,
		WallJumpCorrectionsInWindow, WallJumpCorrectionsInWindow / Denominator,
		GetAirStrafeHardCapSpeed(),
		(CharacterOwner != nullptr && CharacterOwner->HasAuthority())
			? TEXT("  [AUTHORITY - the correction column is meaningless here]") : TEXT(""));
}

/**
 * SPEC v8 §7 — the wall jump driven from code, so it can be measured offscreen and ON A CLIENT.
 *
 * Runs the pawn at the nearest perimeter wall and presses jump through ACharacter::Jump() every frame
 * IsWallJumpAvailable() is true. Jump() is the human entry point, so the press rides CheckJumpInput ->
 * DoJump -> TryWallJump and the saved move exactly as a player's would; nothing here teleports, rotates
 * or writes Velocity, so it cannot manufacture the desync it is measuring.
 */
void UTraceCharacterMovementComponent::TickWallJumpTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceWallJumpTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	APlayerController* TestController = Cast<APlayerController>(CharacterOwner->GetController());
	if (TestController == nullptr || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// TickDashPitchTest's reason, verbatim: a replayed move must not advance a clock that fires input.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	const UWorld* TestWorld = GetWorld();
	if (TestWorld == nullptr)
	{
		return;
	}

	if (WallJumpTestTime < 0.f)
	{
		if (TestWorld->GetTimeSeconds() < 6.f || !IsMovingOnGround())
		{
			return;
		}

		// Straight at the nearer of the two long side walls. A perimeter wall is 4800uu from the
		// centreline and its face is exactly vertical, which is the geometry the mechanic is for.
		const FVector Here = UpdatedComponent->GetComponentLocation();
		WallJumpTestYaw = (Here.Y >= 0.0) ? 90.f : -90.f;
		WallJumpTestTime = 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("WALLJUMP ---- begin. netMode=%d role=%d at %s runYaw=%.0f window=%.2fs retain=%.2f "
			     "outward=%.0f zMul=%.2f cap=%d"),
			static_cast<int32>(GetNetMode()),
			static_cast<int32>(CharacterOwner->GetLocalRole()), *Here.ToCompactString(), WallJumpTestYaw,
			GetWallJumpWindowSeconds(), GetWallJumpSpeedRetention(), GetWallJumpOutwardImpulse(),
			GetWallJumpVerticalMultiplier(), GetWallJumpMaxConsecutive());
	}

	WallJumpTestTime += DeltaSeconds;

	// Hold the aim and the forward key at the wall for the whole run. Pressing INTO the wall is what
	// makes the pawn re-contact it after a launch, which is the only way to reach the cap.
	TestController->SetControlRotation(FRotator(0.f, WallJumpTestYaw, 0.f));
	const FRotationMatrix YawBasis(FRotator(0.f, WallJumpTestYaw, 0.f));
	CharacterOwner->AddMovementInput(YawBasis.GetUnitAxis(EAxis::X), 1.f);

	// One press per open window. bPressedJump is consumed by CheckJumpInput on the same frame, and
	// IsWallJumpAvailable() goes false the instant TryWallJump takes the window — so this cannot mash.
	if (IsWallJumpAvailable())
	{
		CharacterOwner->Jump();
	}
	else if (IsMovingOnGround() && WallJumpTestTime > 1.f && GetPlanarSpeed() > 300.f)
	{
		// Grounded against the wall: the mechanic is airborne-only, so get airborne. An ordinary jump.
		CharacterOwner->Jump();
	}

	if (WallJumpTestTime >= 40.f && bWallJumpTestReported == 0)
	{
		bWallJumpTestReported = 1;
		UE_LOG(LogTraceGame, Display, TEXT("WALLJUMP ---- end."));
		LogWallJumpReport();
	}
}

/**
 * SPEC v8 §5, THE MEASUREMENT. See the header for why it has a server half and a client half.
 */
void UTraceCharacterMovementComponent::TickCarrierChargeTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceCarrierChargeTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// TickDashPitchTest's reason, verbatim: a replayed move must not advance a clock that fires input.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	UWorld* TestWorld = GetWorld();
	if (TestWorld == nullptr)
	{
		return;
	}

	ATraceCharacter* TraceOwner = Cast<ATraceCharacter>(CharacterOwner);
	if (TraceOwner == nullptr)
	{
		return;
	}

	// --- THE SERVER HALF: hand the Core to the JOINED CLIENT's pawn --------------------------------
	//
	// Authority, over a pawn this process does NOT control, that is driven by a PlayerController — on a
	// listen server that is precisely the pawn of the player who joined, which is the only pawn spec
	// v8 §0 accepts a measurement from. Bots are excluded by the PlayerController test.
	if (CharacterOwner->HasAuthority() && !CharacterOwner->IsLocallyControlled())
	{
		if (bCarrierTestCoreGiven != 0 || Cast<APlayerController>(CharacterOwner->GetController()) == nullptr)
		{
			return;
		}

		// After the client's control phase has had time to run and to spend its single charge, so the
		// "before" number is measured on a pawn that genuinely was not carrying.
		if (TestWorld->GetTimeSeconds() < 16.f)
		{
			return;
		}

		for (TActorIterator<ATraceCore> It(TestWorld); It; ++It)
		{
			ATraceCore* Core = *It;
			if (Core == nullptr || Core->IsHeld())
			{
				continue;
			}

			// The real funnel, not a poke at bIsCarrier: the Core attaches, the PlayerState updates and
			// bIsCarrier replicates, which is the whole thing being tested on the far end.
			Core->TryPickup(TraceOwner);
			bCarrierTestCoreGiven = 1;

			UE_LOG(LogTraceGame, Display,
				TEXT("CARRIERTEST [server] gave the Core to the joined client's pawn %s at t=%.1f (carrier=%d)"),
				*GetNameSafe(CharacterOwner), TestWorld->GetTimeSeconds(), TraceOwner->IsCarrier() ? 1 : 0);
			break;
		}

		return;
	}

	// --- THE CLIENT HALF --------------------------------------------------------------------------
	if (!CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// Count launches from the dash's own clock. A press that was refused for want of a charge produces
	// no edge here, which is exactly the symptom being measured.
	const bool bDashingNow = (DashTimeRemaining > 0.f);
	if (bDashingNow && bCarrierTestWasDashing == 0)
	{
		++CarrierTestLaunches;
		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST [client] launch %d in phase %d (carrier=%d charges now %d/%d)"),
			CarrierTestLaunches, CarrierTestPhase, TraceOwner->IsCarrier() ? 1 : 0,
			DashCharges, GetMaxDashCharges());
	}
	bCarrierTestWasDashing = bDashingNow ? 1 : 0;

	if (CarrierTestTime < 0.f)
	{
		if (TestWorld->GetTimeSeconds() < 8.f || !IsMovingOnGround())
		{
			return;
		}

		CarrierTestTime = 0.f;
		CarrierTestPhaseTime = 0.f;
		CarrierTestPhase = 0;
		CarrierTestPresses = 0;
		CarrierTestLaunches = 0;
		CarrierTestChargesAtStart = DashCharges;
		CarrierTestMaxAtStart = GetMaxDashCharges();

		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST ---- begin phase 0 (CONTROL, not carrying). netMode=%d role=%d carrier=%d "
			     "charges=%d/%d cfg base=%d carrierExtra=%d"),
			static_cast<int32>(GetNetMode()), static_cast<int32>(CharacterOwner->GetLocalRole()),
			TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges(),
			UTraceSettings::Get().BaseDashCharges, UTraceSettings::Get().CarrierExtraDashCharges);
	}

	CarrierTestTime += DeltaSeconds;
	CarrierTestPhaseTime += DeltaSeconds;

	if (CarrierTestPhase > 1)
	{
		return;
	}

	// Two presses, far enough apart that the first dash has ended (so the second is refused only by an
	// empty pool, never by "a dash is already running") and close enough that no charge can refill.
	const float FirstPressAt = 0.4f;
	const float SecondPressAt = FirstPressAt + FMath::Max(0.35f, GetDashDuration() + 0.15f);
	const float ReportAt = SecondPressAt + 1.2f;

	if (CarrierTestPhaseTime >= FirstPressAt && CarrierTestPresses == 0)
	{
		++CarrierTestPresses;
		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST [client] press 1 phase %d: carrier=%d charges=%d/%d"),
			CarrierTestPhase, TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges());
		StartDash();
	}
	else if (CarrierTestPhaseTime >= SecondPressAt && CarrierTestPresses == 1)
	{
		++CarrierTestPresses;
		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST [client] press 2 phase %d: carrier=%d charges=%d/%d"),
			CarrierTestPhase, TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges());
		StartDash();
	}

	if (CarrierTestPhaseTime >= ReportAt && CarrierTestPresses >= 2)
	{
		const int32 Expected = (CarrierTestPhase == 0) ? 1 : 2;
		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST ==== phase %d (%s) presses=%d launches=%d expected=%d %s | carrier=%d "
			     "chargesAtStart=%d/%d chargesNow=%d/%d lastMax=%d"),
			CarrierTestPhase, (CarrierTestPhase == 0) ? TEXT("CONTROL, no Core") : TEXT("CARRYING"),
			CarrierTestPresses, CarrierTestLaunches, Expected,
			(CarrierTestLaunches == Expected) ? TEXT("PASS") : TEXT("FAIL"),
			TraceOwner->IsCarrier() ? 1 : 0, CarrierTestChargesAtStart, CarrierTestMaxAtStart,
			DashCharges, GetMaxDashCharges(), LastMaxDashCharges);

		++CarrierTestPhase;
		CarrierTestPhaseTime = 0.f;
		CarrierTestPresses = 0;
		CarrierTestLaunches = 0;

		if (CarrierTestPhase == 1)
		{
			// Phase 1 starts only once the pawn IS the carrier AND the pool has refilled to the carrier's
			// maximum. Both conditions are the claim: the Core arrived, and the extra charge came with it.
			CarrierTestPhaseTime = -1000.f;
		}
	}

	if (CarrierTestPhase == 1 && CarrierTestPhaseTime < -1.f)
	{
		if (TraceOwner->IsCarrier() && DashCharges >= GetMaxDashCharges() && GetMaxDashCharges() >= 2)
		{
			CarrierTestPhaseTime = 0.f;
			CarrierTestChargesAtStart = DashCharges;
			CarrierTestMaxAtStart = GetMaxDashCharges();
			UE_LOG(LogTraceGame, Display,
				TEXT("CARRIERTEST ---- begin phase 1 (CARRYING). carrier=%d charges=%d/%d lastMax=%d t=%.1f"),
				TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges(), LastMaxDashCharges,
				TestWorld->GetTimeSeconds());
		}
		else if (TestWorld->GetTimeSeconds() > 60.f)
		{
			CarrierTestPhase = 2;
			UE_LOG(LogTraceGame, Warning,
				TEXT("CARRIERTEST ==== phase 1 NEVER STARTED by t=60: carrier=%d charges=%d/%d lastMax=%d. "
				     "Either the Core never reached this client or the pool never grew with it."),
				TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges(), LastMaxDashCharges);
		}
	}
}

static void TraceReportWallJump()
{
	if (GEngine == nullptr)
	{
		return;
	}

	int32 Reported = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		const UWorld* ContextWorld = Context.World();
		if (ContextWorld == nullptr)
		{
			continue;
		}

		for (FConstPlayerControllerIterator It = ContextWorld->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			const ACharacter* PawnCharacter = (PC != nullptr) ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
			if (PawnCharacter == nullptr || !PawnCharacter->IsLocallyControlled())
			{
				continue;
			}

			const UTraceCharacterMovementComponent* Movement =
				Cast<UTraceCharacterMovementComponent>(PawnCharacter->GetCharacterMovement());
			if (Movement == nullptr)
			{
				continue;
			}

			++Reported;
			Movement->LogWallJumpReport();
		}
	}

	if (Reported == 0)
	{
		UE_LOG(LogTraceGame, Display, TEXT("WALLJUMP REPORT: no locally-controlled pawn in this process."));
	}
}

static FAutoConsoleCommand GTraceWallJumpReportCmd(
	TEXT("Trace.WallJumpReport"),
	TEXT("Dev only. Spec v8 sec 7: entry vs launch speed, turn angle, the consecutive cap and "
	     "corrections-per-wall-jump for each locally-controlled pawn. Read it on a JOINED CLIENT."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceReportWallJump(); }));

/**
 * SPEC v8 §1 — "dash feels rubber bandy", as a number, on the machine that can have the problem.
 *
 * Prints corrections-per-dash for every locally-controlled pawn in this process. ON A LISTEN HOST IT
 * MUST READ 0.00 AND THAT PROVES NOTHING: an authoritative pawn cannot be corrected by definition,
 * which is exactly how the previous pass reported "no corrections" for a dash the user could feel
 * rubber-banding. Read it on a JOINED CLIENT with NetEmulation.PktLag 40 or it is not an answer.
 */
static void TraceReportDashNet()
{
	if (GEngine == nullptr)
	{
		return;
	}

	int32 Reported = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		const UWorld* ContextWorld = Context.World();
		if (ContextWorld == nullptr)
		{
			continue;
		}

		for (FConstPlayerControllerIterator It = ContextWorld->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			const ACharacter* PawnCharacter = (PC != nullptr) ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
			if (PawnCharacter == nullptr || !PawnCharacter->IsLocallyControlled())
			{
				continue;
			}

			const UTraceCharacterMovementComponent* Movement =
				Cast<UTraceCharacterMovementComponent>(PawnCharacter->GetCharacterMovement());
			if (Movement == nullptr)
			{
				continue;
			}

			++Reported;
			Movement->LogDashNetReport();
		}
	}

	if (Reported == 0)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("DASHNET REPORT: no locally-controlled pawn in this process."));
	}
}

static FAutoConsoleCommand GTraceDashNetReportCmd(
	TEXT("Trace.DashNetReport"),
	TEXT("Dev only. Spec v8 sec 1: prints CORRECTIONS PER DASH for each locally-controlled pawn. Only "
	     "meaningful on a JOINED CLIENT - a listen host cannot be corrected and always reads 0."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceReportDashNet(); }));

static FAutoConsoleCommand GTraceDashVectorTestCmd(
	TEXT("Trace.DashVectorTest"),
	TEXT("Dev only. Prints the composed dash vector for every input/aim combination in spec v7 sec 5 "
	     "(A only, D only, W while looking up, W at 45 degrees, W+D) and asserts that each one is a "
	     "single dash length and that the strafe axis stays level."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceRunDashVectorTest(); }));

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
	, bSavedDashAimRotationValid(0)
	, SavedSlideTimeRemaining(0.f)
	, SavedSlideCooldownRemaining(0.f)
	, SavedSlideSpeed(0.f)
	, SavedSlideBufferRemaining(0.f)
	, SavedSlideDirection(FVector::ZeroVector)
	, bSavedSlideHeldLastMove(0)
	, bSavedWasAirborneLastMove(0)
	, SavedSlideJumpGraceRemaining(0.f)
	, bSavedSlideJumpGraceWellTimed(0)
	, SavedGroundGraceRemaining(0.f)
	, SavedMantleTimeRemaining(0.f)
	, SavedMantleTotalTime(0.f)
	, SavedMantleTargetLocation(FVector::ZeroVector)
	, SavedMantleUpTargetZ(0.f)
	, SavedMantleEntrySpeed(0.f)
	, SavedMantleCooldownRemaining(0.f)
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
	SavedSlideSpeed = 0.f;
	SavedSlideBufferRemaining = 0.f;
	SavedSlideDirection = FVector::ZeroVector;
	bSavedSlideHeldLastMove = 0;
	bSavedWasAirborneLastMove = 0;
	SavedSlideJumpGraceRemaining = 0.f;
	bSavedSlideJumpGraceWellTimed = 0;

	// Spec v5 §7. Moves are pooled: a stale MantleTimeRemaining left in a recycled move would
	// resurrect a pull-up several moves later, in mid-air, with a target from a different ledge.
	SavedGroundGraceRemaining = 0.f;
	SavedMantleTimeRemaining = 0.f;
	SavedMantleTotalTime = 0.f;
	SavedMantleTargetLocation = FVector::ZeroVector;
	SavedMantleUpTargetZ = 0.f;
	SavedMantleEntrySpeed = 0.f;
	SavedMantleCooldownRemaining = 0.f;

	// Spec v8 §7. Same pooling argument as the mantle above: a stale wall-jump window left in a
	// recycled move would let a replay take a wall jump off a wall that is no longer there, from a
	// normal belonging to a different surface.
	SavedWallJumpNormal = FVector::ZeroVector;
	SavedWallJumpWindowRemaining = 0.f;
	SavedWallJumpEntryVelocity = FVector::ZeroVector;
	SavedWallJumpsSinceGround = 0;

	// Spec v8 §1. Zeroed with everything else; SetMoveFor seeds it and PostUpdate_Record refines it.
	SavedDashAimRotation = FRotator::ZeroRotator;
	bSavedDashAimRotationValid = 0;
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
	// The slide-jump's coyote window is in the same list and for the same reason: it decides whether
	// a jump is a slide-jump at all, so a merged move straddling its expiry would resolve the jump
	// differently from the two moves it replaced — and the difference is the whole carry.
	//
	// SPEC v5 §7 adds the mantle. It is the strictest case in the whole kit: its velocity is
	// (target - here)/time-left, which is non-linear in dt by construction, and the pull-up ends on a
	// DISTANCE test, so merging two moves across it would move the frame the pawn arrives on.
	//
	// THE LEDGE GRACE IS DELIBERATELY NOT IN THIS LIST, and it would be a bandwidth bug if it were.
	// GroundGraceRemaining is refilled on every grounded move, so testing it here would refuse to
	// merge ANY ground movement at all — every walking move would go to the server separately. It
	// needs no test: the grace only ever changes a decision while the pawn is airborne, and an
	// airborne move already sets bSavedMomentumActive below, which refuses the merge.
	//
	// (SavedSlideCommitRemaining was in this list and is deleted with the commit window — spec v5 §3.
	// Nothing is lost: SavedSlideTimeRemaining already refuses every move the commit window could
	// have been open on.)
	if (SavedDashTimeRemaining > 0.f || Other->SavedDashTimeRemaining > 0.f
		|| SavedSlideTimeRemaining > 0.f || Other->SavedSlideTimeRemaining > 0.f
		|| SavedSlideJumpGraceRemaining > 0.f || Other->SavedSlideJumpGraceRemaining > 0.f
		|| SavedMantleTimeRemaining > 0.f || Other->SavedMantleTimeRemaining > 0.f)
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

void FSavedMove_Trace::PostUpdate(ACharacter* C, EPostUpdateMode PostUpdateMode)
{
	Super::PostUpdate(C, PostUpdateMode);

	// RECORD PASS ONLY — this is the whole reason this override exists.
	//
	// The base class stores the move's aim in SavedControlRotation, and it would be the obvious place
	// to read a dash's aim from on a replay. It is not safe: Super::PostUpdate WRITES that field, and
	// ClientUpdatePositionAfterServerUpdate calls PostUpdate(PostUpdate_Replay) on every move it has
	// just replayed. So the first correction after a dash overwrites that move's stored aim with
	// wherever the mouse happens to be pointing at correction time, and a second correction covering
	// the same move compounds it — the client replays the dash along a direction it never took, which
	// is exactly the rubber-band being chased. See the header comment on this method.
	//
	// Capturing into our own field, and only on the record pass, means the aim a move was SENT with
	// is written once and no number of replays can move it.
	if (PostUpdateMode == FSavedMove_Character::PostUpdate_Record && C != nullptr)
	{
		// Super has just written SavedControlRotation with the record-time aim, owner fallback and
		// .Clamp() included, and that is bit-for-bit the rotation FCharacterNetworkMoveData packs into
		// this move's ServerMove. Copying it rather than recomputing it is deliberate: a hand-rolled
		// copy of the engine's fallback logic is one engine change away from disagreeing with the wire.
		SavedDashAimRotation = SavedControlRotation;
		bSavedDashAimRotationValid = 1;
	}
	else if (bSavedDashAimRotationValid != 0)
	{
		// ================================================================================================
		// THE REPLAY PASS — AND THE HALF OF SPEC v8 §1 THAT THE REPLAY-SIDE FIX ALONE MADE WORSE.
		// ================================================================================================
		//
		// MEASURED, on a joined client at PktLag 40 both ways: restoring only the CLIENT's replay aim
		// (Trace.DashLegacyAimReplay 0, PrepMoveFor reading SavedDashAimRotation) took corrections-per-dash
		// from 0.43 to 2.29 — it made the rubber-band FIVE TIMES WORSE. The reason is the second consumer
		// of this field, which the replay-side fix does not reach:
		//
		//   FCharacterNetworkMoveData::ClientFillNetworkMoveData does
		//       ControlRotation = ClientMove.SavedControlRotation;
		//
		// and a correction does not just replay the unacknowledged moves, it RE-SENDS them. So the aim the
		// server re-simulates each replayed move from is whatever is in SavedControlRotation AT RESEND
		// TIME — which Super::PostUpdate(PostUpdate_Replay) has just overwritten with wherever the mouse is
		// pointing NOW (engine CharacterMovementComponent.cpp: the SavedControlRotation write is in the
		// block common to BOTH passes, not in the Record branch).
		//
		// That gives three possible worlds, and only one of them is right:
		//
		//   v7 (legacy):  replay reads the stomped aim, resend carries the stomped aim. Client and server
		//                 agree — on an aim the player never held. The dash goes somewhere neither of them
		//                 asked for, but they agree about it, so it corrects rarely.
		//   replay-only:  replay reads the recorded aim, resend still carries the stomped one. Client and
		//                 server now compose the dash from DIFFERENT rotations, every single time. This is
		//                 the 2.29/dash measurement.
		//   this:         the stomp is undone, so the replay AND the resend AND the server all use the aim
		//                 the move was recorded with.
		//
		// The aim a move was made with is a historical fact about that move. Letting a replay rewrite it is
		// the bug in both directions, and putting it back here is the only place that fixes both consumers
		// at once — PrepMoveFor cannot, because it runs before the resend and does not own this field.
		//
		// This is not falsifying what the server was told: the ORIGINAL ServerMove for this timestamp
		// already carried exactly this rotation. A resend that carried anything else would be handing the
		// server different input for a timestamp it has already seen, which is the definition of a
		// mispredicted move.
#if !UE_BUILD_SHIPPING
		// The A/B arm. Trace.DashLegacyAimReplay 1 leaves the engine's stomp in place, so the legacy
		// number can be reproduced in this build rather than argued about.
		extern int32 GTraceDashLegacyAimReplay;
		if (GTraceDashLegacyAimReplay == 0)
#endif
		{
			SavedControlRotation = SavedDashAimRotation;
		}
	}
}

void FSavedMove_Trace::CombineWith(const FSavedMove_Character* OldMove, ACharacter* InCharacter,
	APlayerController* PC, const FVector& OldStartLocation)
{
	Super::CombineWith(OldMove, InCharacter, PC, OldStartLocation);

	// When two moves merge, the combined move starts where the OLDER one did, so any state captured
	// at the start of a move has to be re-based onto the older move's start or the replay begins from
	// the wrong values.
	//
	// CanCombineWith already refuses to merge moves that differ in any ability or momentum state, so
	// by the time we get here the two moves agree on all of it and there is nothing left to reconcile.
	//
	// THE AIM IS DELIBERATELY NOT RE-BASED ONTO THE OLDER MOVE, and an earlier revision of this file
	// did exactly that. SavedDashAimRotation's invariant is "the rotation this move's ServerMove
	// carries", i.e. it must equal SavedControlRotation as recorded — because PostUpdate's replay
	// branch now writes it BACK into SavedControlRotation, which is what the resend reads. Combining
	// runs in ReplicateMoveToServer BEFORE the combined move is simulated and before its
	// PostUpdate(PostUpdate_Record), so the aim is re-recorded from the combined move immediately
	// after this returns. Assigning the older move's rotation here would be overwritten in the happy
	// path and would silently feed the server a rotation it was never sent in any other, so the
	// honest thing is to leave the field alone and let the record pass own it.
	(void)OldMove;
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

			// The WHOLE vector, Z included (spec v7 §5). Nothing here had to change — it was always
			// an FVector — but the field is now load-bearing in three axes rather than two, so a
			// future "optimisation" to a yaw or an FVector2D would silently flatten every vertical
			// dash on the replay path only, which is the hardest class of bug this file can have.
			SavedDashDirection         = Movement->DashDirection;

			// NOTE: the aim rotation needs nothing here. Super::SetMoveFor has already captured it
			// into the base class's SavedControlRotation, which is the same field the ServerMove
			// packs; PrepMoveFor is where it is handed back to the component.

			SavedSlideTimeRemaining     = Movement->SlideTimeRemaining;
			SavedSlideCooldownRemaining = Movement->SlideCooldownRemaining;
			SavedSlideSpeed             = Movement->SlideSpeed;
			SavedSlideBufferRemaining   = Movement->SlideBufferRemaining;
			SavedSlideDirection         = Movement->SlideDirection;
			bSavedSlideHeldLastMove     = Movement->bSlideHeldLastMove;
			bSavedWasAirborneLastMove   = Movement->bWasAirborneLastMove;

			SavedSlideJumpGraceRemaining  = Movement->SlideJumpGraceRemaining;
			bSavedSlideJumpGraceWellTimed = Movement->bSlideJumpGraceWellTimed;

			// Spec v5 §7. The mantle's target and its up-phase Z are snapshotted with the clocks:
			// they are recomputed identically on the server from the same geometry, but the CLIENT's
			// replay must restart from the target the original move actually used, or a correction
			// mid-pull-up would re-derive it from a rewound position and aim somewhere else.
			SavedGroundGraceRemaining    = Movement->GroundGraceRemaining;
			SavedMantleTimeRemaining     = Movement->MantleTimeRemaining;
			SavedMantleTotalTime         = Movement->MantleTotalTime;
			SavedMantleTargetLocation    = Movement->MantleTargetLocation;
			SavedMantleUpTargetZ         = Movement->MantleUpTargetZ;
			SavedMantleEntrySpeed        = Movement->MantleEntrySpeed;
			SavedMantleCooldownRemaining = Movement->MantleCooldownRemaining;

			// SPEC v8 §7. The wall jump is only predicted if its state round-trips. HandleImpact does
			// re-run on a replay (PhysFalling re-sweeps the same static geometry), so the NORMAL and the
			// WINDOW are partly self-healing — but WallJumpsSinceGround is not: nothing in a replay can
			// re-derive it, so without this capture a replay would keep incrementing the ladder counter
			// and eventually refuse a wall jump the client had already taken.
			SavedWallJumpNormal          = Movement->WallJumpNormal;
			SavedWallJumpWindowRemaining = Movement->WallJumpWindowRemaining;
			SavedWallJumpEntryVelocity   = Movement->WallJumpEntryVelocity;
			SavedWallJumpsSinceGround    = Movement->WallJumpsSinceGround;
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

			// SavedDashDirection IS THE FULL 3D VECTOR (spec v7 §5). It always was an FVector; what
			// changed is that its Z is now non-zero, so a correction landing mid-dash restores the
			// vertical component too and the replay re-launches along the same ray rather than a
			// flattened one.
			Movement->DashDirection         = SavedDashDirection;

			// SPEC v7 §5 — THE AIM ROTATION, AND THE REASON A VERTICAL DASH DOES NOT RUBBER-BAND.
			//
			// Acceleration round-trips for free (MoveAutonomous restores it), but the dash direction
			// now has a SECOND input, and FSavedMove_Character::PrepMoveFor does not restore the
			// control rotation — so a replayed BeginDash would compose its direction from wherever
			// the mouse is pointing at correction time, not where it pointed when the dash was
			// pressed. With the old horizontal dash that was invisible because only the yaw mattered
			// and the yaw arrived inside Acceleration; with a vertical dash it is a Z-velocity
			// disagreement between client and server, i.e. exactly the rubber-band this project has
			// already been reported for.
			//
			// SavedControlRotation is the base class's own field, filled by Super::SetMoveFor from
			// the same GetControlRotation() that FCharacterNetworkMoveData packs into this move's
			// ServerMove and that ServerMove_PerformMovement applies to the controller before running
			// MoveAutonomous. So restoring it here makes the replay compose from the identical pair
			// of inputs the server did — for free, with no new saved-move field and no bandwidth.
			// GetDashAimRotation() gates the read on ACharacter::bClientUpdating, so this value can
			// never escape the replay loop and colour a live move.
			// SPEC v8 §1 — AND WHY IT IS *NOT* SavedControlRotation.
			//
			// SavedControlRotation is written by FSavedMove_Character::PostUpdate in the block that runs
			// for BOTH passes (engine CharacterMovementComponent.cpp:12902), and
			// ClientUpdatePositionAfterServerUpdate calls PostUpdate(PostUpdate_Replay) on every move it
			// has just replayed. So the first correction after a dash stomps that move's stored aim with
			// wherever the mouse is pointing at correction time; a second correction covering the same
			// unacknowledged move then replays the dash from the stomped rotation while the server keeps
			// composing from the rotation the ServerMove actually carried. On the spec v7 vectorized
			// dash that disagreement is in Z — the rubber-band.
			//
			// SavedDashAimRotation is our own memo, written once on the record pass and immutable
			// thereafter. Fall back to the base field only for a move that was never recorded, where it
			// is the best (and only) value available.
			// Trace.DashLegacyAimReplay 1 restores the v7 source in the SAME build, which is the only
			// honest way to A/B this: two separately-built clients are two different populations of
			// network jitter. The cvar was declared for exactly this and was reading nothing.
#if !UE_BUILD_SHIPPING
			extern int32 GTraceDashLegacyAimReplay;
			const bool bUseRecordedAim = (GTraceDashLegacyAimReplay == 0) && (bSavedDashAimRotationValid != 0);
#else
			const bool bUseRecordedAim = (bSavedDashAimRotationValid != 0);
#endif
			Movement->ReplayAimRotation        = bUseRecordedAim ? SavedDashAimRotation : SavedControlRotation;
			Movement->bReplayAimRotationValid  = 1;

			Movement->SlideTimeRemaining     = SavedSlideTimeRemaining;
			Movement->SlideCooldownRemaining = SavedSlideCooldownRemaining;
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

			// The mantle and the ledge grace (spec v5 §7). Without these a correction landing
			// mid-pull-up replays it as a plain fall: the pawn ends up on top of the ledge on one
			// machine and at the bottom of it on the other — the largest rubber-band the kit could
			// produce, and the exact bug the mantle was added to remove.
			Movement->GroundGraceRemaining    = SavedGroundGraceRemaining;
			Movement->MantleTimeRemaining     = SavedMantleTimeRemaining;
			Movement->MantleTotalTime         = SavedMantleTotalTime;
			Movement->MantleTargetLocation    = SavedMantleTargetLocation;
			Movement->MantleUpTargetZ         = SavedMantleUpTargetZ;
			Movement->MantleEntrySpeed        = SavedMantleEntrySpeed;
			Movement->MantleCooldownRemaining = SavedMantleCooldownRemaining;

			// SPEC v8 §7. Rewind the wall to where it stood before this move ran. Without these a
			// correction landing inside the contact window replays the wall jump as an ordinary refused
			// mid-air jump — DoJump returns false, Velocity is put back, and client and server disagree
			// about the entire redirected launch on the most visible frame of the move.
			Movement->WallJumpNormal          = SavedWallJumpNormal;
			Movement->WallJumpWindowRemaining = SavedWallJumpWindowRemaining;
			Movement->WallJumpEntryVelocity   = SavedWallJumpEntryVelocity;
			Movement->WallJumpsSinceGround    = SavedWallJumpsSinceGround;
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
