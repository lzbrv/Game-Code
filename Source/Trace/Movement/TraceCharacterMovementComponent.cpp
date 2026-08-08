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
#include "Core/TracePlayerController.h"        // spec v9 §2: the REAL HUD feed, GetDashHudState
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

// =================================================================================================
// SPEC v9 §0 — THE A/B ARM FOR THE §§5-8 TUNING ITEMS.
// =================================================================================================
//
// §0's complaint is that a harness which never went red proves nothing. That applies to the TUNING
// items as much as to the §2 bug: "slide length is 30% shorter" is a claim about a DIFFERENCE, and a
// single number measured after the change cannot show a difference.
//
// Rebuilding the old code to get the "before" number would mean comparing two binaries, which the
// Trace.DashLegacyAimReplay comment in this file already calls out as dishonest — a second build can
// differ in ways nobody accounted for. So instead every v9 §§5-8 change is expressed as a NAMED
// SCALAR ON TOP of the designer's existing value, and this one switch forces every one of those
// scalars back to its identity. One binary, one harness, both arms.
//
// Identity values, stated once so the arm cannot drift from the shipped numbers:
//   §5 WallJumpMomentumScale        0.90 -> 1.00   (retention back to the designer's 0.95)
//   §5 WallJumpWindowScale          0.60 -> 1.00   (window back to the designer's 0.25 s)
//   §6 SlideMaxLengthScale          0.70 -> 1.00
//   §7 SlideJumpBonusScale          1.30 -> 1.00
//   §8 AirStrafeAsymptoteScale      1.10 -> 1.00
//   §8 MovementGravityScale         1.12 -> 1.00
//
// Defined with its CVar down with the other dev globals; declared here because the tuning getters
// above the CVar block need it.
extern int32 GTraceV9LegacyTuning;

/**
 * True while the pre-v9 tuning is in force.
 *
 * BOTH A CVAR AND A COMMAND-LINE SWITCH, for the reason GetDashCooldownRemaining() spells out at
 * length: -ExecCmds fires at PostEngineInit and an ECVF_Cheat variable set that early does not
 * reliably survive into a session on a client that has not connected yet. FParse of the command line
 * cannot miss. GRAVITY IS THE REASON THIS MATTERS MORE HERE than it did there: GravityScale is
 * pushed onto the engine field once per simulated move, so an arm that failed to apply would look
 * exactly like an arm that applied and changed nothing.
 *
 * Read on both machines and on every replayed frame, and a pure function of config either way, so it
 * needs no saved-move state and cannot rubber-band. It must never be flipped mid-session on one end
 * of a live connection — that is a config change, not a prediction input.
 */
static bool IsV9LegacyTuning()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyTuning"));
	return GTraceV9LegacyTuning != 0 || bFromCommandLine;
}

// =================================================================================================
// SPEC v10 §5 — THE A/B ARM FOR THE WALL-JUMP STICK FIX.
//
// THE TESTING RULE FOR THIS PROJECT IS "REPRODUCE THE SYMPTOM FAILING FIRST, THEN SHOW THE SAME
// REPRODUCTION PASSING", and the only honest way to do that for a feel bug is to run the SAME
// harness, in the SAME build, with the fix on and off. Two separately-built binaries are two
// different populations of frame timing and network jitter, and the previous pass's "verified" claim
// was built on exactly that kind of comparison.
//
// Non-zero (or -TraceLegacyWallJump) restores the shipped-v9 wall jump precisely: no post-launch
// into-wall input lockout, no buffered press, and the v9 retention. Everything else is untouched.
//
// Read on both machines and on every replayed frame, and a pure function of config either way, so it
// needs no saved-move state. Do not flip it mid-session on one end of a live connection.
// =================================================================================================
extern int32 GTraceV10LegacyWallJump;

static bool IsV10LegacyWallJump()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyWallJump"));
	return GTraceV10LegacyWallJump != 0 || bFromCommandLine || IsV9LegacyTuning();
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

// Trace.MantleDebug and its -TraceMantleDebug command-line twin lived here (spec v5 §7). Both are
// gone with the mantle itself in v12 §5, rather than left registered and reporting nothing: a CVar
// that accepts a value and changes no behaviour is the same dead knob as an ini key that does
// nothing, and this file has to be able to say the mechanic is absent, not merely quiet.

/**
 * Dash instrumentation (spec v7 §5). Same rules as the slide's: off by default,
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

/**
 * Forward declaration. IsDashPoolDebugEnabled() is defined further down, next to the dash-charge
 * pool it reports on, but it is first CALLED from the movement-update path well above that point.
 * C++ needs the declaration before the call; without it the translation unit fails outright with
 * "use of undeclared identifier", which takes the whole module — and every agent's run rig — with it.
 * Same shape as IsDashDebugEnabled() above.
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

	// Spec v8 §7. Saved-move state like everything above it.
	WallJumpNormal = FVector::ZeroVector;
	WallJumpWindowRemaining = 0.f;
	WallJumpEntryVelocity = FVector::ZeroVector;
	WallJumpsSinceGround = 0;

	// Spec v10 §5 / §1. Saved-move state like everything above it. The bitfield gets no in-class
	// initialiser for the reason the harness bitfields below spell out: every other one in this
	// component is set here, and a mixed convention is how one of them ends up uninitialised.
	WallJumpLaunchNormal = FVector::ZeroVector;
	WallJumpControlLockoutRemaining = 0.f;
	WallJumpInputBufferRemaining = 0.f;
	bKnifeMovementProfile = 0;

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
	// FIX 1 OF 2 FOR THE "RUBBER BANDING ON THE EDGE OF A RAISED SECTION" REPORT, and the only one
	// that is a straight engine setting. See the header for the full diagnosis.
	//
	// SPEC v12 §5: THIS LINE IS NOT MANTLE CODE AND MUST NOT BE DELETED WITH IT. The mantle was fix 3
	// and is gone; this and the ledge grace are fixes 1 and 2 and are what actually keep the client
	// and the server agreeing about a lip. Removing this while removing the mantle is the single
	// mistake that would hand the Demo 5 complaint straight back.
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
				     "| MANTLE removed (spec v12 §5) "
				     "| LEDGE grace=%.3f perchThreshold=%.1f"),
				IsAirStrafeFalloffEnabled() ? 1 : 0, GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(),
				GetAirStrafeFalloffExponent(), IsAirStrafeHardCapEnabled() ? 1 : 0,
				GetSlideDuration(), GetSlideCooldownSeconds(), GetSlideJumpWindowSpeedBonus(),
				GetSlideJumpWindowZBonus(),
				GetLedgeGroundGraceSeconds(), PerchRadiusThreshold);

			// --- SPEC v10, and the same bind-report argument ------------------------------------
			//
			// EVERY v10 KNOB IS READ HERE EXPLICITLY, and that is not decoration. The knife
			// multipliers are only touched by the ceiling accessors while bKnifeMovementProfile is
			// set — which it never is at BeginPlay — so without these calls they would never be
			// resolved, never appear in the bind report, and a rename could kill all three in silence.
			// The report is only "complete by construction" for knobs this block actually reads.
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG-V10 WALLJUMP retention=%.4f (v9 x %.2f v10) outward=%.0f window=%.3f "
				     "controlLockout=%.3f inputBuffer=%.3f legacyArm=%d "
				     "| KNIFE speed=%.2fx softCap=%.2fx hardCap=%.2fx (base soft=%.0f hard=%.0f maxAir=%.0f "
				     "-> knife soft=%.0f hard=%.0f maxAir=%.0f) "
				     "| DASH firegate=AreWeaponActionsBlocked() [=IsDashing(), consumed by "
				     "UTraceWeaponComponent::CanFire/CanSwing via ATraceCharacter]"),
				GetWallJumpSpeedRetention(),
				TraceMoveKnob::Float(TEXT("WallJumpMomentumScaleV10"), 0.90f),
				GetWallJumpOutwardImpulse(), GetWallJumpWindowSeconds(),
				GetWallJumpControlLockoutSeconds(), GetWallJumpInputBufferSeconds(),
				IsV10LegacyWallJump() ? 1 : 0,
				GetKnifeMoveSpeedMultiplier(), GetKnifeAirStrafeSoftCapMultiplier(),
				GetKnifeAirStrafeHardCapMultiplier(),
				GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(), GetMaxAirSpeed(),
				GetAirStrafeSoftCapSpeed() * GetKnifeAirStrafeSoftCapMultiplier(),
				GetAirStrafeHardCapSpeed() * GetKnifeAirStrafeHardCapMultiplier(),
				GetMaxAirSpeed() * GetKnifeAirStrafeHardCapMultiplier());

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
	//
	// SPEC v9 §6 — "Reduce max slide length by 30%", x0.7.
	//
	// LENGTH IS DURATION HERE, and that is not an approximation. Since spec v4 §1 a slide's speed is
	// purely what the player carried in (SlideEntrySpeedMultiplier is 1.00, the flat impulse is
	// deleted) and the only thing that ends it early is decaying to SlideExitSpeedFraction. So the
	// MAXIMUM distance a slide can cover is entry speed integrated over this clock.
	//
	// SCALING THE CLOCK BY 0.7 IS NOT A 30% CUT IN DISTANCE, AND THE DIFFERENCE IS NOT NOISE. Distance
	// is v0.T - ½.a.T², so shortening T also removes part of the QUADRATIC term the player was never
	// going to travel anyway. Measured (Trace.V9.Tuning, entry held at 1250 uu/s, a = 260 uu/s²):
	//
	//     T = 1.80 s -> 1828.8 uu        T = 1.26 s -> 1368.6 uu       = -25.2%, not -30%.
	//
	// The spec's §6 [ASSUMPTION] is explicit — "the maximum distance/duration a slide can cover, ×0.7"
	// — so ×0.7 ON THE CLOCK is what ships, and DURATION is down exactly 30%. If the design owner
	// meant 30% off the DISTANCE, the value that delivers it is SlideMaxLengthScale = 0.647 (solving
	// v0.T - ½.a.T² = 0.7 × 1828.8 gives T = 1.165 s); it is one ini line and it is flagged in the
	// report rather than applied, because the two readings are 5% of a slide apart and that is the
	// design owner's call, not this file's.
	//
	// Scaling the clock rather than the speed is also the only reading that does not contradict
	// spec v4 §1: capping SlideMaxSpeed instead would take momentum the player brought in, which is
	// the exact behaviour Demo 4 asked to have removed.
	const float Base = FMath::Max(0.05f, UTraceSettings::Get().SlideDuration);
	const float Scale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("SlideMaxLengthScale"), 0.7f), 0.05f, 4.f);
	return FMath::Max(0.05f, Base * Scale);
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
	//
	// =============================================================================================
	// SPEC v9 §7 — "Increase the bonus of timing a slide jump right by 30%". ONE NUMBER TO SWITCH.
	// =============================================================================================
	//
	// The sentence is genuinely ambiguous and the two readings are far apart, so both are
	// implemented and the choice is a single bool:
	//
	//   bSlideJumpBonusScalesGainOnly = true  (SHIPPED, and the spec's [ASSUMPTION])
	//       "The bonus" is the part above 1.0 — the thing the timing actually buys.
	//       1 + (1.3125 - 1) x 1.30 = 1 + 0.40625 = 1.40625.
	//       A well-timed hop at 1900 uu/s carries 2672 uu/s instead of 2494 uu/s: +178 uu/s, a
	//       legible step up from a bonus that was already legible.
	//
	//   bSlideJumpBonusScalesGainOnly = false (THE ALTERNATIVE, flagged as the spec asks)
	//       "The bonus" is the whole multiplier. 1.3125 x 1.30 = 1.70625.
	//       The same hop carries 3242 uu/s — 71% over entry speed, which beats DashSpeed's own
	//       3000 uu/s. A slide-hop that is faster than a dash inverts the game's counterplay
	//       (the dash is the only answer to a carrier), so this reading is NOT shipped by default.
	//
	// Base stays the designer's DefaultGame.ini value (1.3125, which wins over the header) and the
	// spec's increase is a separate named scalar on top — so re-tuning the base and re-tuning the
	// v9 increase never fight.
	const float Base = FMath::Max(1.f, UTraceSettings::Get().SlideJumpWindowSpeedBonus);
	const float Scale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("SlideJumpBonusScale"), 1.30f), 0.1f, 4.f);
	const bool bGainOnly = TraceMoveKnob::Bool(TEXT("bSlideJumpBonusScalesGainOnly"), true);

	return FMath::Max(1.f, bGainOnly ? (1.f + (Base - 1.f) * Scale) : (Base * Scale));
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

float UTraceCharacterMovementComponent::GetAirStrafeAsymptoteScale() const
{
	// SPEC v9 §8 — "Move the asymptote on momentum slightly higher, to allow for slightly faster
	// speeds." ONE scalar over BOTH caps, because they are two points on one curve: the soft cap is
	// where gain starts to taper and the hard cap is where it reaches zero, and moving only one of
	// them changes the SHAPE of the falloff rather than its position. +10% keeps the shape identical
	// and slides the whole asymptote up.
	//
	// A NUDGE, NOT A REMOVAL. The spec is explicit that the cap the user asked for in Demo 5 stays;
	// this is 950 -> 1045 and 1250 -> 1375. MaxAirSpeed (1600) is deliberately untouched, so the
	// tighter of the two is still the hard cap and spec v5 §1 still governs.
	if (IsV9LegacyTuning())
	{
		return 1.f;
	}
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("AirStrafeAsymptoteScale"), 1.10f), 0.5f, 2.f);
}

float UTraceCharacterMovementComponent::GetAirStrafeSoftCapSpeed() const
{
	// 950 = 1.19 x the 800 walk speed. Below it a strafe is worth EXACTLY what it was in Demo 5,
	// which is the part the user called incredible and asked not to be touched.
	//
	// Spec v9 §8 slides this up by GetAirStrafeAsymptoteScale(); at x1.10 the untouched band grows
	// from "below 950" to "below 1045", i.e. Demo 5's feel now survives 10% further up the range.
	//
	// SPEC v10 §1 raises it again, and only while the knife is out — "a higher momentum ceiling".
	return FMath::Max(0.f, TraceMoveKnob::Float(TEXT("AirStrafeSoftCapSpeed"), 950.f)
		* GetAirStrafeAsymptoteScale()
		* (bKnifeMovementProfile ? GetKnifeAirStrafeSoftCapMultiplier() : 1.f));
}

float UTraceCharacterMovementComponent::GetAirStrafeHardCapSpeed() const
{
	// Always strictly above the soft cap: the falloff divides by (Hard - Soft), and a designer who
	// set them equal would otherwise get a divide-by-zero rather than the "cap everything at the soft
	// cap" they obviously meant.
	//
	// Scaled by the same asymptote knob as the soft cap — see GetAirStrafeAsymptoteScale() for why
	// they have to move together. The Max() below is applied AFTER the scale so the invariant still
	// holds at any scale.
	//
	// SPEC v10 §1: raised while the knife is out. The Max() below still runs AFTER both scales, and
	// the soft cap is scaled by its own knob, so a designer who sets the soft multiplier higher than
	// the hard one gets the "cap everything at the soft cap" behaviour rather than an inverted band.
	return FMath::Max(GetAirStrafeSoftCapSpeed() + 1.f,
		TraceMoveKnob::Float(TEXT("AirStrafeHardCapSpeed"), 1250.f) * GetAirStrafeAsymptoteScale()
			* (bKnifeMovementProfile ? GetKnifeAirStrafeHardCapMultiplier() : 1.f));
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
	// SPEC v10 §1 — SCALED BY THE KNIFE'S HARD-CAP MULTIPLIER, AND IT HAS TO BE.
	//
	// ApplySourceAirAcceleration takes min(MaxAirSpeed, AirStrafeHardCapSpeed) as its ceiling. The
	// shipped numbers are 1600 and 1375, so MaxAirSpeed is 225 uu/s of headroom and no more; a knife
	// hard cap of 1375 x 1.35 = 1856 under an unraised 1600 would be capped by MaxAirSpeed and the
	// knife's "higher momentum ceiling" would be worth 225 uu/s instead of 481. The knob would look
	// bound, print BOUND in the MOVEKNOB report, and quietly do a third of what it says.
	return FMath::Max(1.f, UTraceSettings::Get().MaxAirSpeed
		* (bKnifeMovementProfile ? GetKnifeAirStrafeHardCapMultiplier() : 1.f));
}

// --- SPEC v10 §1: the knife movement profile ----------------------------------------------------

void UTraceCharacterMovementComponent::SetKnifeMovementProfileActive(const bool bActive)
{
	bKnifeMovementProfile = bActive ? 1 : 0;
}

// SPEC v12 §3 SCALED ALL THREE OF THESE, AND THE ARITHMETIC IS STATED ONCE HERE.
//
// "Reduce max speed with the knife from the previous 30% increase to 22% and adjust momentum
// accordingly." The ground multiplier is the number they named: 1.30 -> 1.22. The two air ceilings
// are the "momentum" half, and the rule applied is that the BONUS — the part above 1.0, which is
// the only part the knife adds — is scaled by 22/30, so the whole mobility package shrinks in
// proportion instead of the ground speed dropping while the ceilings stay at their +30% values.
//
//   ground   1.30      bonus 0.30  -> 0.30 * 22/30 = 0.22      -> 1.22
//   softCap  1.25      bonus 0.25  -> 0.25 * 22/30 = 0.183333  -> 1.183333
//   hardCap  1.35      bonus 0.35  -> 0.35 * 22/30 = 0.256667  -> 1.256667
//
// THESE LITERALS ARE FALLBACKS, NOT THE SHIPPED VALUES. All three bind by name into UTraceSettings
// and Config/DefaultGame.ini overrides them, so the ini keys must move with these or the defaults
// here are decoration. Trace.DumpSettings from a running game is the only honest check.
// At the shipped asymptote the ceilings become soft 1045 -> 1236, hard 1375 -> 1728.

float UTraceCharacterMovementComponent::GetKnifeMoveSpeedMultiplier() const
{
	// "Players should move 22% faster with a knife" (v12 §3, down from v10 §1's 30%). A multiplier
	// over WalkSpeed rather than an absolute, so retuning the walk moves the knife with it: 800 -> 976.
	//
	// Floored at 1.0: a "knife profile" that made the player SLOWER would be a config typo silently
	// inverting the design, and there is no reading of the spec that wants it.
	//
	// FLAGGED, NOT FIXED (spec v12 §3): CarrierSpeedMultiplier is 1.30 because the user previously
	// asked for the carrier to MATCH the knife. Dropping the knife to 1.22 breaks that parity and
	// leaves the carrier faster than the knife. Only the knife was asked for, so only the knife moved.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("KnifeMoveSpeedMultiplier"), 1.22f), 1.f, 3.f);
}

float UTraceCharacterMovementComponent::GetKnifeAirStrafeSoftCapMultiplier() const
{
	// The soft cap is where air-strafe gain STARTS to taper. Raising it by less than the hard cap
	// widens the free band and the falloff band together, which is what "a higher ceiling" means for a
	// mobility weapon: the knife does not just cap out higher, it keeps its full turn value further up
	// the range. 1045 -> 1236 at the shipped asymptote (was 1306 at +30%).
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("KnifeAirStrafeSoftCapMultiplier"), 1.183333f), 1.f, 3.f);
}

float UTraceCharacterMovementComponent::GetKnifeAirStrafeHardCapMultiplier() const
{
	// Where gain reaches zero — the actual momentum ceiling. 1375 -> 1728 at the shipped asymptote
	// (was 1856 at +30%), i.e. the knife can build 353 uu/s more than the gun before the air strafe
	// stops paying. Also applied to MaxAirSpeed; see GetMaxAirSpeed() for why leaving that alone
	// would gut this knob.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("KnifeAirStrafeHardCapMultiplier"), 1.256667f), 1.f, 3.f);
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

// --- Ledge tuning (spec v5 §7) ------------------------------------------------------------------
//
// The eight Mantle* accessors used to live here (bMantleEnabled, MantleReachUU, MantleMinHeightUU,
// MantleMaxHeightUU, MantleDurationSeconds, MantleUpPhaseFraction, MantleCooldownSeconds,
// MantleMinForwardSpeed) plus WallJumpMantleLockoutSeconds further down. All nine are deleted in
// spec v12 §5 along with the mechanic they tuned. The matching UPROPERTYs in UTraceSettings and the
// keys in Config/DefaultGame.ini go with them: a knob nothing reads is worse than no knob, because
// a designer will set it and believe something happened.
//
// GetLedgeGroundGraceSeconds() below is NOT one of them. It is fix 2 of the two that actually
// address the Demo 5 ledge rubber-band, it still ships, and it is still read every move.

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

	// --- SPEC v9 §8: GRAVITY x1.12 ---------------------------------------------------------------
	//
	// Verbatim: "Increase gravity by 12%, to make players feel less floaty when air strafing."
	//
	// GravityScale is an engine-owned field for the same reason AirControl above is: PhysFalling
	// reads it through UCharacterMovementComponent::GetGravityZ() deep inside the fall integration,
	// at no point this file can intercept. So it is COPIED here rather than read at the point of
	// use, and pushed once per simulated move — which keeps it live under Trace.LiveEdit like every
	// other knob, and makes it identical on client, server and every replayed frame (it is a pure
	// function of config, so it needs no saved-move state and cannot rubber-band).
	//
	// AGAINST 1.0, NOT AGAINST ITSELF. Multiplying GravityScale by 1.12 every move would compound to
	// infinity in about two seconds. The authored value is the engine default 1.0 and nothing else
	// in the project writes this field (the mode-B throw arc has its own CoreThrowGravityScale on
	// the Core's projectile, which reads WORLD gravity and is therefore untouched by this).
	const float DesiredGravityScale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("MovementGravityScale"), 1.12f), 0.1f, 4.f);
	if (!FMath::IsNearlyEqual(GravityScale, DesiredGravityScale))
	{
		GravityScale = DesiredGravityScale;
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
	//
	// SPEC v9 §5: "Make the window of time for performing a wall jump shorter and make the action
	// happen faster." The base number stays where the designer put it (WallJumpWindowSeconds in
	// DefaultGame.ini, which WINS over the fallback here) and the spec's cut is applied as its own
	// named scalar on top, so the two are never confused and the cut is one number to revert.
	//
	// The scalar also shortens how long the auto-mantle defers to a live wall-jump opportunity —
	// see the mantle gate in OnMovementUpdated — so the two halves of §5 move together by
	// construction.
	const float Base = FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpWindowSeconds"), 0.25f), 0.f, 1.f);
	const float Scale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpWindowScale"), 0.6f), 0.05f, 1.f);
	return FMath::Clamp(Base * Scale, 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetWallJumpSpeedRetention() const
{
	// The "carry momentum in a new direction" dial. 0.95 rather than 1.0 so a wall is very slightly
	// lossy — otherwise a corridor is a frictionless pinball table — but nowhere near the reset the
	// request is complaining about. Capped at 1: a wall must never MANUFACTURE speed, which is the
	// same rule spec v4 §1 imposed on the slide.
	//
	// SPEC v9 §5: "Reduce momentum gained from wall jumping by 10%." The spec's [ASSUMPTION] is
	// explicit — "scale the retention knob by 0.9" — so that is what this does, as a separate named
	// scalar rather than by editing the designer's 0.95.
	//
	// THE ALTERNATIVE READING, flagged because the two differ by an order of magnitude. Measured
	// end-to-end retention (launch speed / entry speed) is ~104.6-105.4%, because the outward
	// impulse adds on top of the 0.95. If "the momentum GAINED" means only the part above 100%,
	// then a 10% cut is 1.050 -> 1.045 and is invisible. Scaling the knob is the spec's call and is
	// the change a player will actually feel: 0.95 -> 0.855, which lands measured retention near
	// ~95%. To switch readings, set WallJumpMomentumScale to 1.0 and cut GetWallJumpOutwardImpulse
	// instead — it is the only other term in the launch.
	//
	// SPEC v10 §5, THE SAME SENTENCE AGAIN: "Reduce the momentum boost from wall jumping by 10%."
	// A SECOND named scalar rather than a re-edit of the v9 one, for two reasons that both matter:
	//
	//   1. Config/DefaultGame.ini pins WallJumpMomentumScale=0.9, AND THE INI WINS. Lowering the
	//      default in this file would have changed nothing at all in a running game — which is the
	//      single most common way a change in this project has silently not shipped.
	//   2. The two cuts stay separable. WallJumpMomentumScaleV10 = 1.0 reverts exactly the v10 cut and
	//      leaves v9's, which is what an A/B on feel needs.
	//
	// AND IT IS APPLIED HERE, NOT TO THE OUTWARD IMPULSE. The ini invites the other reading ("set the
	// scale to 1.0 and cut WallJumpOutwardImpulse instead, the only other launch term") and this pass
	// deliberately refuses it: the §5 investigation found the outward impulse is the pawn's escape
	// velocity from the face and that it is ALREADY too weak to beat the player's own held input (11 uu
	// of separation before it is cancelled — see the header). Cutting it would have made the stickiness
	// in the first half of the same sentence measurably worse while satisfying the second half.
	//
	// Net: 0.95 x 0.90 x 0.90 = 0.7695. NOTE FOR THE READING OF THE NUMBERS: at approach speeds at or
	// above the air-strafe hard cap the launch is clamped to max(EntrySpeed, HardCap) anyway, so the
	// cut is invisible on the fastest wall jumps and does its whole job on the mid-speed ones.
	const float Base = FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpSpeedRetention"), 0.95f), 0.f, 1.f);
	const float Scale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpMomentumScale"), 0.9f), 0.1f, 1.f);
	const float ScaleV10 = IsV10LegacyWallJump()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpMomentumScaleV10"), 0.90f), 0.1f, 1.f);
	return FMath::Clamp(Base * Scale * ScaleV10, 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetWallJumpControlLockoutSeconds() const
{
	// SPEC v10 §5, CAUSE 1. See the header for the derivation; the short version is that an air-to-air
	// wall jump launches at ~543 uu/s outward against 8000 uu/s² of air acceleration pointed back at
	// the wall, and peaks 18 uu out — half a capsule radius — before it is being driven back in.
	//
	// 0.20 s is chosen as "long enough that the impulse alone clears the face". At 420 uu/s the pawn
	// travels 84 uu in that time — two and a half capsule radii, unambiguously off the wall — and the
	// window then ends while the player still has most of their airtime to strafe with. Deliberately
	// SHORTER than the mantle lockout (0.30 s) so the two do not read as one long dead zone, and long
	// enough to cover the 0.15 s wall window so a second contact cannot re-open a launch inside it.
	if (IsV10LegacyWallJump())
	{
		return 0.f;
	}
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpControlLockoutSeconds"), 0.20f), 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetWallJumpInputBufferSeconds() const
{
	// SPEC v10 §5, CAUSE 2 — the press eaten by frame ordering. See the header.
	//
	// 0.12 s is a little under two frames at 15 Hz and about seven at 60, which is the range a human
	// "pressed it just before I hit" actually lands in. It is deliberately SHORTER than the 0.15 s
	// contact window: a buffer longer than the window would let a press made well before the wall
	// survive past the point where the wall itself has stopped counting, and a wall jump the player
	// did not ask for on this contact is a worse bug than the one being fixed.
	if (IsV10LegacyWallJump())
	{
		return 0.f;
	}
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpInputBufferSeconds"), 0.12f), 0.f, 0.5f);
}

// GetWallJumpMantleLockoutSeconds() (spec v9 §5) was defined here. It existed only to stop the
// automatic mantle from re-grabbing the ledge a frame after a wall jump had thrown the pawn off it.
// The mantle is gone (spec v12 §5), so the lockout has nothing to lock out and is deleted rather
// than left returning a value no caller reads.
//
// WHAT THIS MEANS FOR THE WALL JUMP, STATED PLAINLY BECAUSE IT IS THE RISK IN THIS CHANGE: the wall
// jump used to have to WIN A RACE. The mantle needed no input and was attempted on every airborne
// move, so on the frame the capsule met a wall the mantle could claim the pawn before the player's
// press ever reached DoJump — and once it had, IsWallJumpAvailable() was false for the rest of the
// contact. Spec v9 §5 patched that with a priority rule (the mantle yields while a wall window is
// live) and this lockout (the mantle stays off for 0.30 s after a launch). BOTH halves are now
// unnecessary, not merely disabled: there is no second consumer of a wall contact left in the
// component. TryWallJump()'s only gates are its own window, its own consecutive cap and being
// airborne, which is what "the wall jump still fires cleanly at a wall" reduces to.

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
		// The mantle's unconditional "the pull-up owns Velocity" branch was the first thing in this
		// block (spec v5 §7). Removed in v12 §5. Nothing replaces it: with the mantle gone, a pawn at
		// a ledge is either falling or walking, and the two branches below are the whole story again.

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

	// =============================================================================================
	// SPEC v10 §5, CAUSE 1 — THE PLAYER MAY NOT PULL THEMSELVES BACK ONTO THE WALL THEY JUST LEFT.
	//
	// THIS IS THE STICKINESS. Not the window, not the mantle — those were v9, and the complaint came
	// back unchanged. A player who wall-jumps got to the wall by holding the stick INTO it, and they
	// are still holding it on the frame after the launch. The arithmetic is one-sided and brutal:
	// peak separation is v_out²/2a with a = 8000 uu/s², and AirMaxWishSpeed (160) caps how fast a
	// pawn can return to a wall under air control — so every wall jump taken FROM THE AIR has almost
	// nothing to reflect, launches at roughly the flat 420 impulse alone, and peaks at ~18 uu against
	// a 34 uu capsule radius. It is glued to the face by its own input, and no launch value could
	// have outrun it: doubling the impulse buys 4x the separation and still loses to 8000 uu/s².
	//
	// SO REMOVE ONLY THE OFFENDING COMPONENT, AND ONLY BRIEFLY. The wish direction is projected onto
	// the wall plane for GetWallJumpControlLockoutSeconds(); tangential and outward input keep their
	// full magnitude and their full allowance, so the Source strafe is entirely intact — the player
	// can still carve the launch anywhere in the half-space away from the wall, at the same rate as
	// always. Only "accelerate back into the face I just launched off" is refused, and only until the
	// pawn is clear of it.
	//
	// NOTHING IS SUBTRACTED FROM VELOCITY, which is this function's founding rule: the projection
	// touches the INPUT direction before the formula runs, never the velocity vector. A lockout that
	// braked the pawn would be the "air control feels like a brake" mistake the comment below warns
	// about, wearing a different hat.
	//
	// Predicted: WallJumpControlLockoutRemaining and WallJumpLaunchNormal are saved-move state, and
	// Acceleration is restored by MoveAutonomous, so a replayed frame lands on the identical vector.
	// =============================================================================================
	if (WallJumpControlLockoutRemaining > 0.f && !WallJumpLaunchNormal.IsNearlyZero())
	{
		const float IntoWall = -static_cast<float>(FVector::DotProduct(WishDirection, WallJumpLaunchNormal));
		if (IntoWall > 0.f)
		{
			FVector Allowed = WishDirection + WallJumpLaunchNormal * IntoWall;
			if (!Allowed.Normalize())
			{
				// Input aimed dead-on at the face. There is no tangential component to keep, so the
				// honest answer is "no acceleration this frame" — not a redirect to some direction the
				// player did not ask for.
				return;
			}
			WishDirection = Allowed;
		}
	}

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

	// A "no dashing during a mantle" gate was here (spec v5 §7). Gone with the mantle in v12 §5.

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
	// =============================================================================================
	// SPEC v9 §2 — THE CARRIER'S "ONE DASH". THIS FUNCTION WAS THE WHOLE BUG.
	// =============================================================================================
	//
	// The user's three symptoms, verbatim: "The carrier still has only one dash, despite the hud
	// showing two. When the first refills, they both do. When dash is used, both charges are
	// consumed." Spec v9 §0 is explicit that the last pass measured the POOL, found it correct, and
	// declared victory. The pool IS correct. The READOUT was not, and the readout is the only thing
	// the player can see.
	//
	// THE OLD CONTRACT WAS BROKEN. ATracePlayerController::GetDashHudState feeds this number
	// straight into FTraceDashHudState::Remaining, whose own doc comment reads "Seconds until the
	// NEXT CHARGE lands. 0 when nothing is recharging", and derives
	// RechargeFraction = 1 - Remaining/(DashDuration + DashCooldown) from it. ATraceHUD::
	// DrawChargePips then draws pip[Charges] at exactly that fraction.
	//
	// The old body returned 0 whenever ANY charge was in hand. So for a carrier at 1 of 2 — one
	// banked, one 3.68 s away — it answered "0 seconds", the controller computed RechargeFraction
	// = 1.0, and DrawChargePips filled the regenerating pip SOLID. The meter read 2 of 2 while the
	// pawn held 1. Every symptom falls out of that single lie:
	//
	//   "the hud shows two"            — 1 of 2 renders as two full pips. Directly.
	//   "when dash is used, BOTH are   — the first spend is invisible (2 pips before, 2 pips
	//    consumed"                       after), so the second spend is the first one the player
	//                                    ever sees, and it empties the whole row at once.
	//   "when the first refills, they  — at 0 of 2 the row is empty and honest. The instant the
	//    both do"                        refill grants charge #1, this function flipped from ~3.6
	//                                    to 0, RechargeFraction snapped to 1.0, and BOTH pips lit
	//                                    on the same frame.
	//   "the carrier still has only    — the meter only ever tells the truth at zero, so the pool
	//    one dash"                       behaves, to the eye, exactly like a single charge.
	//
	// It also explains why the previous harness passed: it pressed twice from a FULL pool and
	// counted two launches. Both launches really do happen. Nothing about that test could see a
	// display that was wrong only in the 1-of-2 and the 0->1 states.
	//
	// THE RULE NOW, and it is the struct's documented contract restated: this is the time until the
	// pool GAINS ITS NEXT CHARGE. Full pool -> 0. Short pool -> the refill clock, whether or not a
	// charge happens to be banked. DashTimeRemaining is deliberately NOT part of it any more: the
	// dash window is not a wait for a charge, and reporting it made the meter dip for 0.18 s on a
	// pawn whose next charge was seconds away.
	//
	// "Can I dash right now" is GetDashCharges() > 0 (which is what ATraceHUD::DrawAbilityRows
	// already uses for its READY tint) and CanDash() — never this.
#if !UE_BUILD_SHIPPING
	// The A/B arm (spec v9 §0). The shipped body, verbatim, so -TraceSingleDashTest can be shown
	// FAILING and then PASSING in the same binary.
	//
	// BOTH A COMMAND-LINE SWITCH AND A CVAR, and the switch is not redundant: -ExecCmds fires at
	// PostEngineInit, and an ECVF_Cheat variable set that early on a client that has not yet
	// connected does not reliably survive into the session (measured — the sibling
	// "NetEmulation.PktLag 40" in the same -ExecCmds list applied and this one did not). FParse of
	// the command line cannot miss, and it is what every other harness in this file already uses.
	extern int32 GTraceDashLegacyChargeReadout;
	static const bool bLegacyFromCommandLine =
		FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyChargeReadout"));
	if (GTraceDashLegacyChargeReadout != 0 || bLegacyFromCommandLine)
	{
		if (DashCharges > 0 && DashTimeRemaining <= 0.f)
		{
			return 0.f;
		}
		if (DashCharges > 0)
		{
			return FMath::Max(0.f, DashTimeRemaining);
		}
		return FMath::Max(0.f, DashRechargeRemaining);
	}
#endif

	if (DashCharges >= GetMaxDashCharges())
	{
		return 0.f;
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
	if (SlideTimeRemaining > 0.f || SlideCooldownRemaining > 0.f || DashTimeRemaining > 0.f)
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
	// The "and not mid-mantle" clause that used to sit here went with the mantle (spec v12 §5). This
	// is one of the two places that made the wall jump lose to it; see TryWallJump().
	if (!IsFalling() || MovementMode == MOVE_None)
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

#if !UE_BUILD_SHIPPING
	// SPEC v10 §5, THE STICK METER. This is the frame the complaint's clock starts on.
	BeginWallStickSample(PlanarNormal);
#endif
}

bool UTraceCharacterMovementComponent::IsWallJumpAvailable() const
{
	// The "&& MantleTimeRemaining <= 0.f" clause was here (spec v8 §7). It is the clause spec v9 §5
	// identified as the mechanism by which the mantle deleted wall jumps: a mantle that started on
	// the contact frame made this false for the whole contact, so the player's press landed on a pawn
	// the mantle already owned. Gone with the mantle in v12 §5 — availability is now purely the
	// window, the cap, and being airborne.
	return IsWallJumpEnabled()
		&& MovementMode != MOVE_None
		&& IsFalling()
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

	// SPEC v10 §5, CAUSE 1. ARM THE INTO-WALL INPUT LOCKOUT.
	//
	// Held on its own field rather than on WallJumpNormal because that one has just been cleared two
	// lines up — correctly, since it means "a face a jump COULD launch off" and this launch has spent
	// it. What the lockout needs is the face this launch left, which only exists from here.
	//
	// Also consumes any buffered press: this launch IS the press being remembered, and leaving the
	// buffer charged would let it fire a second time off the next contact the player did not ask for.
	WallJumpLaunchNormal = Normal;
	WallJumpControlLockoutRemaining = GetWallJumpControlLockoutSeconds();
	WallJumpInputBufferRemaining = 0.f;

	// SPEC v9 §5's mantle lockout was applied here — the second half of "a wall jump overrides a
	// mantle". Deleted in v12 §5 with the mantle. NOTHING now touches Velocity after this point in
	// the move: the launch written above is what the player gets, on the client, on the server and on
	// every replayed move, with no second consumer of the wall contact to argue with.

#if !UE_BUILD_SHIPPING
	// SPEC v10 §5, THE STICK METER. Stamp the launch into the open sample so the report can split the
	// total stick into "waiting for the launch" and "getting away from the wall afterwards" — the two
	// halves have different causes and only a split can say which fix moved which number.
	if (WallStickContactTime >= 0.f && WallStickLaunchTime < 0.f)
	{
		const UWorld* StickWorld = GetWorld();
		WallStickLaunchTime = (StickWorld != nullptr) ? static_cast<float>(StickWorld->GetTimeSeconds()) : -1.f;
	}

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
	// The "&& !IsMantling()" clause that used to be here is gone with the mantle (spec v12 §5). That
	// clause was the other reason the wall jump felt broken: a mantle held the pawn in MOVE_Flying
	// for 0.35 s during which this function refused EVERY press, which is most of what "it feels like
	// the player is sticking to the wall for a second" was. There is now no state in this component
	// that can refuse a jump the engine would have allowed.
	return IsJumpAllowed() && (IsMovingOnGround() || IsFalling());
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

			// SPEC v10 §5, CAUSE 2 — REMEMBER THE PRESS THAT ARRIVED ONE TICK EARLY.
			//
			// THE ORDERING BUG THIS CLOSES. ACharacter::JumpMaxHoldTime is 0, so bPressedJump lives for
			// exactly one tick. CheckJumpInput (which calls this function) runs at the START of
			// PerformMovement; HandleImpact — the ONLY thing that opens a wall window — runs during the
			// physics step that follows it. So a press made on the tick before the capsule touches the
			// wall reaches here, is correctly refused, and is then gone forever. The player pressed jump
			// "right as they hit the wall", nothing happened, and they are left scraping down the face
			// waiting to press again. v9's shorter window made this the EASY way to miss, not a rare one.
			//
			// Buffering converts that dead press into a wall jump on the contact frame itself (see
			// OnMovementUpdated), which is also one whole frame earlier than the CheckJumpInput path can
			// ever deliver a launch.
			//
			// ONLY WHEN THE REFUSAL WAS "NO WALL YET". A press refused by the anti-ladder cap, or while
			// a window was open and something else declined it, must stay refused — buffering those
			// would turn the cap into a queue and hand back the infinite staircase spec v8 §7 removed.
			// The window itself still bounds the buffer's reach: OnMovementUpdated only ever spends it
			// through IsWallJumpAvailable(), which re-checks the cap, the mode and the mantle.
			if (WallJumpNormal.IsNearlyZero()
				&& WallJumpWindowRemaining <= 0.f
				&& WallJumpsSinceGround < GetWallJumpMaxConsecutive())
			{
				WallJumpInputBufferRemaining = GetWallJumpInputBufferSeconds();
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
// THE MANTLE (spec v5 §7) WAS HERE. IT IS GONE — spec v12 §5.
//
// Deleted in full: IsMantling(), CanAttemptMantle(), TryBeginMantle(), ApplyMantleVelocity() and
// EndMantle(), together with six pieces of saved-move state, eight tuning knobs, one CVar and the
// v9 §5 priority rule that existed only to keep it from eating wall jumps. Around 400 lines.
//
// WHAT REPLACES IT: nothing, and that is the point of the change. The Demo 5 report the mantle was
// written for — "when jumping on the edge of a raised section, it's glitchy and feels like rubber
// banding" — is a claim about client/server disagreement, and the two fixes that address it are
// PerchRadiusThreshold (set in the constructor) and the ledge grace (GroundGraceRemaining, kept
// below). Both are still here and both are untouched. The mantle was a third fix layered on top,
// and unlike the other two it changed where the pawn ENDS UP rather than only stabilising the
// agreement about where it is — which is why it could be removed without giving the bug back, and
// why -TraceLedgeTest was rewritten to prove that rather than assert it. See TickLedgeTest.
// =================================================================================================

bool UTraceCharacterMovementComponent::IsGroundedForAbilities() const
{
	return IsMovingOnGround() || GroundGraceRemaining > 0.f;
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

	// 1a-iv. SPEC v10 §5, CAUSE 1 — THE INTO-WALL INPUT LOCKOUT'S CLOCK.
	//
	//        Ticked here with every other clock so a replayed move advances it by exactly the amount
	//        the original did, and the face is dropped on the frame it expires so
	//        ApplySourceAirAcceleration never has to re-test the normal's age.
	//
	//        THE GROUND ENDS IT EARLY, and that is not cosmetic. Once the pawn is walking, the air
	//        strafe is not running at all, so a lockout still counting down would be invisible until
	//        the player left the ground again — at which point it would silently refuse an input
	//        direction on a jump that had nothing to do with any wall.
	if (WallJumpControlLockoutRemaining > 0.f)
	{
		WallJumpControlLockoutRemaining = FMath::Max(0.f, WallJumpControlLockoutRemaining - DeltaSeconds);
		if (WallJumpControlLockoutRemaining <= 0.f || IsGroundedForAbilities())
		{
			WallJumpControlLockoutRemaining = 0.f;
			WallJumpLaunchNormal = FVector::ZeroVector;
		}
	}

	// 1a-v. SPEC v10 §5, CAUSE 2 — SPEND THE BUFFERED PRESS.
	//
	//       ORDER IS THE WHOLE POINT AND IT IS LOAD-BEARING. This runs AFTER the window clock above,
	//       which means after HandleImpact has had its say for this frame — so a press buffered on the
	//       previous tick is converted into a launch on the very frame the capsule touched the wall,
	//       rather than waiting for the next CheckJumpInput.
	//
	//       TryWallJump() is self-contained: it writes both the planar launch and Velocity.Z itself and
	//       does not depend on anything Super::DoJump would have done first (the pawn is already
	//       MOVE_Falling — IsWallJumpAvailable() requires it). Skipping ACharacter's JumpCurrentCount
	//       bookkeeping is deliberate and harmless: those counts exist only to make the engine ASK
	//       (see RefreshEngineTunablesFromSettings), and the real cap is WallJumpsSinceGround, which
	//       TryWallJump increments.
	//
	//       Spent before it is ticked, so a buffer charged this frame is live for its full length.
	//
	//       NOT INTO A DASH. The dash owns the velocity vector outright and re-asserts it a few lines
	//       below, so a wall jump spent here would be silently overwritten on the same frame — the
	//       press would be consumed and the player would get nothing. The clock keeps running, so a
	//       press made just before a short dash can still land after it if the wall and the clock both
	//       survive.
	if (WallJumpInputBufferRemaining > 0.f)
	{
		if (DashTimeRemaining <= 0.f && IsWallJumpAvailable() && TryWallJump())
		{
			WallJumpInputBufferRemaining = 0.f;
		}
		else
		{
			// The clock runs whatever happened above, INCLUDING through a dash — a buffer frozen for
			// the length of a dash would be a buffer of unbounded length, which is the one property
			// GetWallJumpInputBufferSeconds() exists to bound.
			WallJumpInputBufferRemaining = FMath::Max(0.f, WallJumpInputBufferRemaining - DeltaSeconds);

			// Landing spends it too. A remembered air press must not survive a touchdown and fire off
			// the first wall of the NEXT jump, which the player would read as a wall jump they did not
			// press — the mirror image of the bug this buffer fixes, and a worse one.
			if (IsGroundedForAbilities())
			{
				WallJumpInputBufferRemaining = 0.f;
			}
		}
	}

	// 1a-ii. The mantle clock and its cooldown were advanced here (spec v5 §7). Gone in v12 §5.

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
	else if (bSlidePressedThisMove && !bOnGroundNow && MovementMode != MOVE_None)
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

	// 3b. THE MANTLE WAS ACTIVATED HERE (spec v5 §7), last of the activations, attempted on every
	//     airborne move. Deleted in v12 §5, and with it the whole spec v9 §5 priority block that used
	//     to sit above this line ("THE WALL JUMP OUTRANKS THE MANTLE") plus the WallJumpMantleSteals
	//     counter that measured how often the mantle won anyway.
	//
	//     WHY THE PRIORITY CODE GOING AWAY IS SAFE, since that is the one thing the removal could
	//     plausibly break. The rule existed because two mechanics wanted the same frame: the mantle
	//     needed no input and ran HERE, at the end of the move, while the wall jump needed a press
	//     that PerformMovement delivers through CheckJumpInput EARLIER in the same move. So the
	//     mantle could claim a contact before the press for it had arrived, and IsWallJumpAvailable()
	//     then refused for the rest of the contact. With the mantle deleted there is exactly one
	//     consumer of a wall contact left, so there is no race to arbitrate — the priority rule is
	//     not disabled, it is unnecessary. HandleImpact still latches the face on the frame the
	//     capsule touches it, TryWallJump() still reads it from DoJump, and neither now has a clause
	//     that any other ability can make false.

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

	// SPEC v10 §5. Advanced on the RECORD pass only, for the reason BeginWallStickSample() gives: a
	// replay re-runs the same frames and would close the same sample several times over. Always on
	// (it costs a dot product per frame and only while a sample is open) so a human play session can
	// dump the same number with Trace.WallStickReport that the harness prints — the complaint is
	// about feel, and a meter that only exists inside a synthetic run cannot be checked against it.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && CharacterOwner->IsLocallyControlled())
	{
		TickWallStickSample();
	}

	TickWallJumpTest(DeltaSeconds);
	TickCarrierChargeTest(DeltaSeconds);
	TickSingleDashTest(DeltaSeconds);
#endif
}

float UTraceCharacterMovementComponent::GetMaxSpeed() const
{
	if (IsDashing())
	{
		return GetDashSpeed();
	}

	// A MOVE_Flying carve-out for the mantle sat here (spec v5 §7), so that nothing sampling the
	// pawn's speed limit mid-pull-up was told it was capped at MaxFlySpeed. Deleted in v12 §5: the
	// component never enters MOVE_Flying of its own accord any more.

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

	// SPEC v10 §1 — "Players should move 30% faster with a knife." 800 -> 1040 at the shipped walk
	// speed. Applied AFTER the carrier multiplier and BEFORE the slide floor: the slide floor is an
	// absolute speed the knife has no business scaling (the slide is a separate ability with its own
	// tuning, not a faster walk).
	//
	// THE TWO MULTIPLIERS DO NOT STACK, and this comment used to claim they did. The ordering above
	// makes stacking arithmetically possible — 1.08 x 1.30 = 1.40x — but it never happens, because
	// nothing ever sets bKnifeMovementProfile on a carrier: TraceMelee::ShouldUseKnifeMovementProfile
	// is the sole definition of the bit and its carrier clause is explicit, a carrier is refused a
	// swap outright, and UTraceWeaponComponent::RefreshMovementProfile re-asserts the bit every tick
	// on every machine so a pickup while holding the knife clears it within a frame. A carrier's
	// knife is STOWED — they cannot swing it and cannot shoot — so the movement bonus for "the knife
	// is the active weapon" is false by the plain reading of the rule.
	//
	// The guard therefore lives one slice away, on purpose (one definition, not two opinions). If
	// that clause is ever removed, 1.40x lands HERE and silently retires the one number the
	// carrier's speed was ever tuned with. Do not add a second carrier test below to "be safe" —
	// two tests that can disagree is how this became wrong in the first place.
	//
	// A DASH is unaffected in either case: the IsDashing() branch at the top of this function returns
	// GetDashSpeed() before any multiplier is reached, so the knife does not buy a faster dash.
	//
	// bKnifeMovementProfile is saved-move state, so a replayed move is clamped by the same ceiling
	// the original was. The remaining seam is the one-RTT gap between the two ends learning about a
	// swap, which is exactly the carrier seam documented immediately above and is absorbed the same
	// way.
	if (bKnifeMovementProfile)
	{
		Speed *= GetKnifeMoveSpeedMultiplier();
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
 * SPEC v9 §2 / §0 — THE A/B ARM FOR THE CHARGE READOUT.
 *
 * Spec v9 §0's whole complaint is that the previous pass produced a harness that never went red. A
 * fix demonstrated only by a green run is not demonstrated at all, and rebuilding the old code to
 * get a red run means comparing two binaries — which the Trace.DashLegacyAimReplay comment already
 * calls out as dishonest for exactly this reason.
 *
 * So set this to 1 and GetDashCooldownRemaining() answers the way the shipped build did: 0 whenever
 * any charge is in hand. Run -TraceSingleDashTest with it on and the reproduction fails; run it with
 * it off and the same reproduction passes. One binary, one harness, both results.
 */
int32 GTraceDashLegacyChargeReadout = 0;
static FAutoConsoleVariableRef CVarTraceDashLegacyChargeReadout(
	TEXT("Trace.DashLegacyChargeReadout"),
	GTraceDashLegacyChargeReadout,
	TEXT("Dev only. 1 restores the pre-v9 GetDashCooldownRemaining() (returns 0 whenever any charge is "
	     "banked, so the HUD draws a still-recharging pip as full) to reproduce the spec v9 §2 bug in "
	     "this build."),
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

/**
 * SPEC v9 §0 — the A/B arm for the §§5-8 TUNING items. See IsV9LegacyTuning() at the top of the file
 * for the identity values it restores and for why the command-line switch (-TraceLegacyTuning) is not
 * redundant with this variable.
 */
int32 GTraceV9LegacyTuning = 0;
static FAutoConsoleVariableRef CVarTraceV9LegacyTuning(
	TEXT("Trace.V9LegacyTuning"),
	GTraceV9LegacyTuning,
	TEXT("Dev only. 1 restores the pre-v9 movement tuning (wall-jump retention and window, no "
	     "wall-jump-over-mantle priority, full-length slide, the 1.3125 slide-jump bonus, the "
	     "un-nudged air-strafe asymptote and gravity x1.0) so the spec v9 secs 5-8 changes can be "
	     "measured as a BEFORE/AFTER in one binary."),
	ECVF_Cheat);

/**
 * SPEC v10 §5 — the A/B arm for the wall-jump STICK fix specifically. See IsV10LegacyWallJump() at the
 * top of the file. Separate from V9LegacyTuning on purpose: that switch reverts eight unrelated tuning
 * values as well, and a stick number measured against it would be measuring gravity and the slide too.
 */
int32 GTraceV10LegacyWallJump = 0;
static FAutoConsoleVariableRef CVarTraceV10LegacyWallJump(
	TEXT("Trace.V10LegacyWallJump"),
	GTraceV10LegacyWallJump,
	TEXT("Dev only. 1 restores the spec v9 wall jump exactly: no post-launch into-wall input lockout, "
	     "no buffered jump press, and the v9 momentum retention. The RED arm for the spec v10 sec 5 "
	     "stick meter -- run -TraceWallJumpTest with and without it in ONE binary."),
	ECVF_Cheat);

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
			     "z=%.1f grounded=%d grace=%.3f slide=%.3f | mean=%.2f worst=%.2f"),
			*GetNameSafe(CharacterOwner), CorrectionCount, TimeStamp, PositionError, VelocityError,
			static_cast<int32>(LocalMode), static_cast<int32>(ServerMovementMode), Before.Z,
			IsMovingOnGround() ? 1 : 0, GroundGraceRemaining, SlideTimeRemaining,
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

	// ============================================================================================
	// NOT ON A REPLAYED MOVE. SPEC v12 §5 — AND THE PREVIOUS VERSION OF THIS HARNESS PRODUCED
	// NOTHING BUT GARBAGE FOR WANT OF THIS LINE.
	//
	// OnMovementUpdated runs once per move on the record pass AND once per move on every replay. A
	// correction replays the whole unacknowledged move queue inside a single frame, so without this
	// guard the phase clock below advances by the sum of a dozen moves' DeltaSeconds in one frame,
	// the harness burns through all eight runs in under a second of wall time, and the input it
	// issues is issued from inside a replay where it means nothing. MEASURED, on a 40 ms client:
	// eight "runs" completed in 0.85 s of wall clock, every one of them reporting "never reached the
	// block", and zero contact frames recorded. Every other measurement path in this file (BeginDash,
	// TickWallStickSample, the wall-jump counters) already carries this guard; this one did not.
	if (CharacterOwner->bClientUpdating)
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
		if (TestWorld->GetTimeSeconds() < 5.f || !IsMovingOnGround())
		{
			return;
		}
		LedgeTestTime = static_cast<float>(TestWorld->GetTimeSeconds());
		LedgeTestPhase = 0;
		LedgeTestPhaseTime = LedgeTestTime;
		LedgeTestGroundFlips = 0;
		LedgeTestRun = 0;
		CorrectionCount = 0;
		CorrectionErrorTotal = 0.f;
		CorrectionErrorWorst = 0.f;

		LedgeTestContacts = 0;
		LedgeTestContactFlips = 0;
		LedgeTestContactCorrections = 0;
		LedgeTestWorstContactFlips = 0;
		LedgeTestWorstContactErr = 0.f;
		LedgeTestKeptFractionTotal = 0.f;
		LedgeTestWorstKeptFraction = 1.f;
		LedgeTestLandedOnTop = 0;
		LedgeTestPulledBack = 0;

		// --- "-TraceLedgeLegacy": THE DEMO 5 ARM, AND THE REASON THIS TEST CAN GO RED ------------
		//
		// A harness that has never failed is not evidence. This one measures a prediction desync at a
		// lip, and if the shipped build simply has no desync then every number it prints is a pass by
		// default and proves nothing about whether the fixes are load-bearing — which is exactly how
		// the Demo 5 verification went wrong the first time.
		//
		// So the arm restores the pre-fix geometry handling: PerchRadiusThreshold back to the engine
		// default of 0, which disables the reduced-radius perch test and puts the walking/falling
		// decision back on a sub-uu knife edge. Pair it with
		//   -ini:Game:[/Script/Trace.TraceSettings]:LedgeGroundGraceSeconds=0.0
		// and the component is behaving exactly as it did when the user reported the rubber-band.
		//
		// Component-local and dev-only: it writes this pawn's own field, so it cannot leak into a real
		// match, and it is deliberately NOT a designer knob — there is no shipping reason to want the
		// broken behaviour back.
		if (FParse::Param(FCommandLine::Get(), TEXT("TraceLedgeLegacy")))
		{
			PerchRadiusThreshold = 0.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE ---- LEGACY ARM: PerchRadiusThreshold forced to 0 (the Demo 5 state). "
				     "Pair with -ini:Game:[/Script/Trace.TraceSettings]:LedgeGroundGraceSeconds=0.0"));
		}

		// netMode 3 is NM_Client. THE ONLY ARM OF THIS TEST THAT ANSWERS THE QUESTION IS netMode=3:
		// a correction count taken on a listen server is structurally zero, because the server never
		// corrects itself. A run that prints netMode=0 or 2 here has measured the geometry and nothing
		// about prediction, and this project has already shipped one "verification" of that shape.
		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE ---- begin. netMode=%d role=%d (mantle: REMOVED, spec v12 §5) grace=%.3f "
			     "perch=%.1f jumpZ=%.0f apex=%.0fuu"),
			static_cast<int32>(GetNetMode()), static_cast<int32>(CharacterOwner->GetLocalRole()),
			GetLedgeGroundGraceSeconds(), PerchRadiusThreshold, JumpZVelocity,
			(JumpZVelocity * JumpZVelocity) / (2.f * FMath::Max(1.f, GetGravityZ() * -1.f)));
	}

	// ============================================================================================
	// THE PHASE CLOCKS ARE WORLD TIME, NOT A SUM OF DeltaSeconds. SPEC v12 §5, AND THIS IS THE
	// SECOND HARNESS BUG THE MANTLE-REMOVAL PASS HAD TO FIX BEFORE ANY NUMBER HERE MEANT ANYTHING.
	//
	// The old clocks accumulated the DeltaSeconds handed to OnMovementUpdated. MEASURED on a 40 ms
	// client: the accumulated clock passed 40 "seconds" in 1.4 s of wall time — roughly 28x — so
	// every phase timed out almost immediately and the run reported "found no arena ledge after 40s
	// of searching" having actually searched for about a second. The bClientUpdating guard above
	// removes the replay passes but not whatever else re-enters this path per frame, and chasing that
	// is beside the point: A HARNESS CLOCK MUST NOT DEPEND ON HOW MANY TIMES THE FUNCTION RUNS.
	// World time is the quantity the phases are actually reasoning about ("run at it for up to 8
	// seconds"), it is frozen during a replay so a replayed move cannot advance it, and it is immune
	// to the whole class of bug. LedgeTestTime and LedgeTestPhaseTime are therefore START STAMPS.
	//
	// DeltaSeconds is still right for the flip counter above: that counts events, not time.
	// ============================================================================================
	const float NowSeconds = static_cast<float>(TestWorld->GetTimeSeconds());
	const float PhaseElapsed = NowSeconds - LedgeTestPhaseTime;
	const float TotalElapsed = NowSeconds - LedgeTestTime;

	// HOW MANY CONTACTS ONE SESSION IS, AND WHY IT IS FIVE RATHER THAN EIGHT.
	//
	// The harness pawn is a live player in a live match: it sprints across the arena past bots that
	// are laying lethal trails, and MEASURED it dies on a ~34 s cycle. Every death respawns it, which
	// builds a new movement component, which resets this harness to run 1 — so a session longer than
	// one life NEVER REPORTS. Three consecutive attempts at eight contacts got to run 2 and restarted.
	// Five contacts at ~3.3 s each is about 17 s, which fits comfortably inside a life.
	//
	// Overridable with "-TraceLedgeRuns=N" so the two arms can be held at the SAME N — comparing a
	// five-contact arm against an eight-contact one would be comparing sample sizes as well as
	// behaviour, and the per-contact averages are the whole point.
	static const int32 RequiredRuns = []
	{
		int32 Value = 5;
		FParse::Value(FCommandLine::Get(), TEXT("TraceLedgeRuns="), Value);
		return FMath::Clamp(Value, 1, 32);
	}();

	// A HARD DEADLINE ON THE WHOLE SESSION. Phases 1 and 2 re-probe when they cannot reach their
	// target, which is the right recovery but is also a loop; without this a run that can never find
	// usable geometry would sit in it forever and the operator would read the silence as "still
	// working". Reported as a failure, in the same words as the search failure, for the same reason.
	// THE VERDICT, IN ONE PLACE, SO A PARTIAL SESSION STILL REPORTS.
	//
	// MEASURED, and it is why this is a lambda rather than a block at the end of phase 4: the match
	// relocates the harness pawn to a spawn pad roughly 13 s into every session (a goal, a kickoff or
	// a death — the harness cannot tell and does not need to). Sessions that lost their target after
	// two contacts used to print NOTHING AT ALL and simply sat there, which is the worst possible
	// failure mode for a diagnostic: indistinguishable from "still running" and from "all clear".
	// Now every session reports what it actually collected, with the count attached so a thin sample
	// cannot be mistaken for a thorough one.
	auto LogVerdict = [this, Runs = RequiredRuns](const TCHAR* Why)
	{
		if (LedgeTestContacts <= 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("LEDGE ---- %s with ZERO contacts. NO MEASUREMENT - do NOT read this as a pass."), Why);
			return;
		}

		const float Denominator = static_cast<float>(LedgeTestContacts);

		// Read it in this order:
		//   netMode      must be 3 (NM_Client) or the correction columns are structurally zero and the
		//                run has proved nothing about prediction.
		//   corr/contact server corrections landing between the jump and the settle. THIS IS THE DESYNC
		//                NUMBER. Non-zero with a meaningful worstErr is a real client/server
		//                disagreement at the lip; zero means the "rubber banding" was never a network
		//                symptom at these settings.
		//   flips        ground-state changes per contact. 2 is clean (leave, arrive). 3+ means the
		//                capsule is chattering on the edge, which is the mechanism that MAKES
		//                corrections, so it leads the correction number.
		//   kept         planar speed retained across the lip. The stall measure.
		//   pulledBack   contacts that ended BEHIND the jump position. The literal rubber-band.
		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE ---- %s. netMode=%d contacts=%d/%d ledgeHeight=%.1fuu | flips/contact=%.2f "
			     "(2.00 is clean, worst=%d) | corr/contact=%.2f (total=%d, worstErr=%.2fuu) | "
			     "kept=%.3f (worst=%.3f) | onTop=%d pulledBack=%d | mantle=REMOVED grace=%.3f perch=%.1f"),
			Why, static_cast<int32>(GetNetMode()), LedgeTestContacts, Runs, LedgeTestLedgeHeight,
			LedgeTestContactFlips / Denominator, LedgeTestWorstContactFlips,
			LedgeTestContactCorrections / Denominator, LedgeTestContactCorrections,
			LedgeTestWorstContactErr,
			LedgeTestKeptFractionTotal / Denominator, LedgeTestWorstKeptFraction,
			LedgeTestLandedOnTop, LedgeTestPulledBack,
			GetLedgeGroundGraceSeconds(), PerchRadiusThreshold);
	};

	// 90 s, not 420: the pawn is relocated about every 13 s, so a session that has not finished in a
	// minute and a half is not going to. Reporting early beats reporting nothing.
	if (LedgeTestPhase != 9 && TotalElapsed > 90.f && LedgeTestRun < RequiredRuns)
	{
		LogVerdict(TEXT("CUT SHORT (the match relocated the pawn)"));
		LedgeTestPhase = 9;
		return;
	}

	auto Advance = [this, NowSeconds](int32 NextPhase)
	{
		LedgeTestPhase = NextPhase;
		LedgeTestPhaseTime = NowSeconds;
	};

	const UCapsuleComponent* TestCapsule = CharacterOwner->GetCapsuleComponent();
	const float TestRadius = (TestCapsule != nullptr) ? TestCapsule->GetScaledCapsuleRadius() : 42.f;
	const float TestHalfHeight = (TestCapsule != nullptr) ? TestCapsule->GetScaledCapsuleHalfHeight() : 88.f;

	// Planar distance from the capsule SURFACE to the face it is running at. Every phase below is
	// driven off this one number, so it is computed once.
	const FVector Here = UpdatedComponent->GetComponentLocation();
	const float DistToFace = static_cast<float>(FVector::Dist2D(Here, LedgeTestFacePoint)) - TestRadius;

	switch (LedgeTestPhase)
	{
	// --- 0. FIND A REAL LEDGE IN THE ARENA ------------------------------------------------------
	//
	// SPEC v12 §5 REPLACED THE SPAWNED TEST BLOCK WITH THE ARENA'S OWN GEOMETRY, AND THE OLD
	// APPROACH WAS NOT SALVAGEABLE ON A CLIENT. It spawned an AStaticMeshActor locally and teleported
	// the pawn to a mark in front of it. Both are illegal from a client:
	//
	//   * A runtime-spawned AStaticMeshActor does not replicate its mesh or its collision. Only the
	//     machine that spawned it has the block. Running the harness on the client alone meant the
	//     client climbed a solid box the server believed was empty air; running it on both meant two
	//     independently-spawned boxes that agree only if both searches happen to pick the same spot.
	//   * SetActorLocation on a client's own autonomous proxy is a position the server never
	//     simulated, so the very next ServerMove is rejected and corrected. MEASURED: corrections of
	//     10030 uu, 10144 uu, 10312 uu — three orders of magnitude larger than any ledge effect, and
	//     manufactured entirely by the harness. The pawn was yanked back on every reset and never
	//     reached its own block on any of eight runs.
	//
	// The arena already contains exactly the geometry this test wants: TraceArenaBuilder scatters
	// cover boxes at 1x player height (176 uu — see its "1x / 2x / 3.5x player height" comment),
	// which is precisely the "raised section" class the complaint is about. It is built from a seed
	// at map load, identically on every machine, and it is real level geometry rather than something
	// this file invented — so client and server are guaranteed to agree about it, and the harness
	// cannot be the source of the disagreement it exists to detect.
	//
	// So: probe outward for a vertical face with a walkable top of the right height, and then drive
	// the pawn with MOVEMENT INPUT ONLY for the rest of the test. No spawn, no teleport, nothing that
	// the prediction path does not already carry.
	case 0:
	{
		FCollisionQueryParams ProbeParams;
		FCollisionResponseParams ProbeResponse;
		InitCollisionParams(ProbeParams, ProbeResponse);
		ProbeParams.bTraceComplex = false;
		ProbeParams.AddIgnoredActor(CharacterOwner);
		const ECollisionChannel ProbeChannel = UpdatedComponent->GetCollisionObjectType();

		const float FeetZ = Here.Z - TestHalfHeight;

		// The jump apex, which is what makes a ledge "the top edge of an obstacle" rather than "a
		// wall". Only ledges the pawn can actually get on top of are of any interest here.
		const float Apex = (JumpZVelocity * JumpZVelocity) / (2.f * FMath::Max(1.f, GetGravityZ() * -1.f));
		const float MinLedge = 100.f;
		const float MaxLedge = Apex - 5.f;

		bool bFound = false;
		float BestDistance = TNumericLimits<float>::Max();

		for (int32 Step = 0; Step < 24; ++Step)
		{
			const float Yaw = Step * (360.f / 24.f);
			const FVector Direction = FRotator(0.f, Yaw, 0.f).Vector();

			// Knee height, so a 176 uu box is hit on its face rather than missed over the top.
			const FVector ProbeStart(Here.X, Here.Y, FeetZ + 45.f);
			FHitResult FaceHit;
			if (!TestWorld->LineTraceSingleByChannel(FaceHit, ProbeStart, ProbeStart + Direction * 4000.f,
				ProbeChannel, ProbeParams, ProbeResponse))
			{
				continue;
			}

			// A FACE, not a ramp and not the floor.
			if (FMath::Abs(FaceHit.ImpactNormal.Z) > 0.3f)
			{
				continue;
			}

			// LEVEL GEOMETRY, NOT A PLAYER. MEASURED, and it is the trap this probe falls into by
			// default: a Trace character's capsule is 176 uu tall with vertical sides and a walkable
			// cap, so it passes every geometric test a 1x-player-height cover box passes. The first
			// client run of this probe locked onto "TraceCharacter_8" at 2887 uu, reported a 168.7 uu
			// "ledge", and then spent the whole test walking toward a bot that was walking away — the
			// harness never got within 1800 uu of its own target. A moving obstacle is also the one
			// thing guaranteed to make client and server disagree for reasons that have nothing to do
			// with a lip, which would have poisoned the very number this test exists to produce.
			if (Cast<APawn>(FaceHit.GetActor()) != nullptr)
			{
				continue;
			}

			// SQUARE ON, NOT GLANCING. The pawn runs along Direction; if the face is steeply angled to
			// that, the capsule slides along it instead of arriving at the lip, and the contact being
			// measured is a wall-slide rather than a ledge landing.
			if (FVector::DotProduct(-FaceHit.ImpactNormal, Direction) < 0.85f)
			{
				continue;
			}

			// Far enough away to build up to full speed on the approach, near enough that the run is
			// short. 800 uu/s over ~1.2 s of run-up is the shape wanted.
			const float FaceDistance = static_cast<float>(FVector::Dist2D(Here, FaceHit.ImpactPoint));
			if (FaceDistance < 700.f || FaceDistance > 9000.f || FaceDistance >= BestDistance)
			{
				continue;
			}

			// How tall is it? Trace down from above, just past the face.
			const FVector TopProbeXY = FaceHit.ImpactPoint + Direction * (TestRadius + 20.f);
			const FVector TopStart(TopProbeXY.X, TopProbeXY.Y, FeetZ + Apex + 200.f);
			const FVector TopEnd(TopProbeXY.X, TopProbeXY.Y, FeetZ - 20.f);
			FHitResult TopHit;
			if (!TestWorld->LineTraceSingleByChannel(TopHit, TopStart, TopEnd, ProbeChannel,
				ProbeParams, ProbeResponse))
			{
				continue;
			}

			const float LedgeHeight = static_cast<float>(TopHit.ImpactPoint.Z) - FeetZ;
			if (LedgeHeight < MinLedge || LedgeHeight > MaxLedge)
			{
				continue;
			}

			// Walkable on top, or landing on it is not the test.
			if (TopHit.ImpactNormal.Z < GetWalkableFloorZ())
			{
				continue;
			}

			// Room to STAND up there. A lip with a pillar on it is a different experiment.
			const FCollisionShape StandShape = FCollisionShape::MakeCapsule(TestRadius, TestHalfHeight);
			const FVector StandSpot(TopProbeXY.X, TopProbeXY.Y, TopHit.ImpactPoint.Z + TestHalfHeight + 4.f);
			if (TestWorld->OverlapBlockingTestByChannel(StandSpot, FQuat::Identity, ProbeChannel,
				StandShape, ProbeParams, ProbeResponse))
			{
				continue;
			}

			// An unobstructed FINAL APPROACH — the last 1500 uu before the face, which is the only
			// part the pawn sprints through. Deliberately NOT the whole line from where the pawn is
			// standing right now: the arena is 33600 uu long and full of cover, so demanding a clear
			// capsule sweep across several thousand uu rejected every candidate in the level. MEASURED:
			// with the full-path test the first client run reported "found no arena ledge between 100
			// and 182uu within 3500uu" and took no measurement at all. Phase 1 walks the pawn to the
			// mark; only what happens after the mark has to be clear.
			const FCollisionShape RunShape = FCollisionShape::MakeCapsule(TestRadius, TestHalfHeight);
			const FVector FaceAtRunZ(FaceHit.ImpactPoint.X, FaceHit.ImpactPoint.Y, Here.Z);
			const FVector RunStart = FaceAtRunZ - Direction * 1500.f;
			const FVector RunEnd = FaceAtRunZ - Direction * (TestRadius + 10.f);
			FHitResult PathHit;
			if (TestWorld->SweepSingleByChannel(PathHit, RunStart, RunEnd, FQuat::Identity, ProbeChannel,
				RunShape, ProbeParams, ProbeResponse))
			{
				continue;
			}

			// ...and the mark itself has to be somewhere the pawn can stand.
			if (TestWorld->OverlapBlockingTestByChannel(RunStart, FQuat::Identity, ProbeChannel,
				RunShape, ProbeParams, ProbeResponse))
			{
				continue;
			}

			BestDistance = FaceDistance;
			LedgeTestFacePoint = FaceHit.ImpactPoint;
			LedgeTestTopPoint = TopHit.ImpactPoint;
			LedgeTestLedgeHeight = LedgeHeight;
			LedgeTestRunDirection = Direction;
			LedgeTestBlock = FaceHit.GetActor();
			bFound = true;
		}

		if (!bFound)
		{
			// Nothing in reach from here — so WALK, and probe again next frame. The 24-direction probe
			// is a snapshot of one standing position, and a spawn pad sits in an endzone with the open
			// field (and its cover) several thousand uu away. Moving toward the field centre costs
			// nothing and turns "no ledge visible from the spawn" into "no ledge in the arena", which
			// are very different claims.
			FVector TowardCentre = -Here;
			TowardCentre.Z = 0.f;
			if (TowardCentre.Normalize())
			{
				CharacterOwner->AddMovementInput(TowardCentre, 1.f);
			}

			if (PhaseElapsed > 90.f)
			{
				// Loud, and it gives up rather than measuring something else. THE ABSENCE OF A
				// MEASUREMENT IS NOT A PASS, and this line says so in the log so that a later reader
				// cannot mistake a silent run for a clean one.
				UE_LOG(LogTraceGame, Warning,
					TEXT("LEDGE found no arena ledge between %.0f and %.0fuu after 90s of searching "
					     "(now at %s). NO MEASUREMENT TAKEN - do NOT read the absence of corrections "
					     "as a pass."),
					MinLedge, MaxLedge, *Here.ToCompactString());
				Advance(9);
			}
			break;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE found an arena ledge: actor=%s height=%.1fuu (apex=%.0fuu, clearance=%+.1fuu) "
			     "face=%s top=%s distance=%.0fuu"),
			*GetNameSafe(LedgeTestBlock.Get()), LedgeTestLedgeHeight, Apex, Apex - LedgeTestLedgeHeight,
			*LedgeTestFacePoint.ToCompactString(), *LedgeTestTopPoint.ToCompactString(), BestDistance);
		Advance(1);
		break;
	}

	// --- 1. GET TO THE RUN-UP MARK, ON FOOT -----------------------------------------------------
	//
	// Both the first approach and the reset between runs, and it is movement input rather than a
	// teleport for the reason phase 0 spells out. Walks toward the face when too far and away from it
	// when too near, so it also carries the pawn back DOWN off the top of the ledge it just landed on.
	//
	// The band is 1000-1600 uu: far enough to reach the 800 uu/s ground cap before the jump (which
	// takes about 250 uu from a standing start at 4096 uu/s^2), near enough that the walk is short.
	// Kept deliberately tight in TIME rather than generous in distance — this rig shares a machine
	// with several other agents' editors, and a session that takes three minutes gets OOM-killed
	// before it reports. A measurement that never finishes is a measurement you do not have.
	case 1:
	{
		const bool bTooFar = DistToFace > 1600.f;
		CharacterOwner->AddMovementInput(bTooFar ? LedgeTestRunDirection : -LedgeTestRunDirection, 1.f);

		const bool bAtMark = DistToFace >= 1000.f && DistToFace <= 1600.f && IsMovingOnGround();
		if (bAtMark)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE run %d: at the mark, %.0fuu from the face"), LedgeTestRun + 1, DistToFace);
			Advance(2);
		}
		else if (PhaseElapsed > 45.f)
		{
			// RE-PROBE RATHER THAN MEASURE ANYWAY. Failing to reach the mark means the target is not
			// what the probe thought it was (something moved, or the path is blocked), and running the
			// contact from the wrong place would produce a number that looks like a result.
			UE_LOG(LogTraceGame, Warning,
				TEXT("LEDGE run %d: could not reach the mark in 45s (dist=%.0f) - re-probing"),
				LedgeTestRun + 1, DistToFace);
			Advance(0);
		}
		break;
	}

	// --- 2. RUN AT IT, AND JUMP AT THE EDGE -----------------------------------------------------
	//
	// The jump is triggered on DISTANCE TO THE FACE, not on a stopwatch. "Jumping on the edge of a
	// raised section" is a specific input — a jump made close enough that the capsule arrives at the
	// lip rather than sailing over it — and a fixed delay produced a different jump every time the
	// frame rate moved.
	//
	// SPEC v12 §5 SWEEPS THE DISTANCE ACROSS THE RUNS instead of using one value, and that is
	// deliberate: at 800 uu/s the pawn's feet are above a 176 uu lip only between t=0.39 s and
	// t=0.91 s of a 640 uu/s jump, so the jump distance decides whether the capsule clips the face,
	// catches the very corner, or lands cleanly on the top. All three are "hitting the top edge of an
	// obstacle" and a single distance would only ever exercise one of them. The sweep spans 260-400 uu
	// whatever the run count is, so changing -TraceLedgeRuns changes the SAMPLE SIZE and not the band
	// being sampled — which is what makes two arms at different N still qualitatively comparable, and
	// two arms at the same N directly so.
	case 2:
	{
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);

		const float JumpDistance = 260.f + (140.f * static_cast<float>(LedgeTestRun))
			/ static_cast<float>(FMath::Max(1, RequiredRuns - 1));

		if (DistToFace < JumpDistance && IsMovingOnGround() && GetPlanarSpeed() > 600.f)
		{
			// SPEC v12 §5. SNAPSHOT EVERYTHING THE CONTACT WILL BE MEASURED AGAINST, on the jump frame.
			// Deltas of counters that already exist, rather than a second set of counters:
			// CorrectionCount and CorrectionErrorWorst are maintained by OnClientCorrectionReceived for
			// every correction this pawn takes, so a delta of them cannot fall out of step with the
			// corrections themselves the way a parallel attribution clock can.
			LedgeTestFlipsAtJump = LedgeTestGroundFlips;
			LedgeTestCorrAtJump = CorrectionCount;
			LedgeTestWorstErrAtJump = CorrectionErrorWorst;
			LedgeTestSpeedAtJump = GetPlanarSpeed();
			LedgeTestPosAtJump = Here;

			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE run %d: JUMP at %.0fuu from the face (target %.0f), planar=%.0f uu/s"),
				LedgeTestRun + 1, DistToFace, JumpDistance, GetPlanarSpeed());
			CharacterOwner->Jump();
			Advance(3);
		}
		else if (PhaseElapsed > 8.f)
		{
			// BACK TO THE MARK, NOT BACK TO THE SEARCH. MEASURED: a goal reset teleports every pawn to
			// its spawn pad mid-test — "never reached the face (dist=6318, planar=180)" is the harness
			// finding itself 6300 uu from the ledge it was running at — and re-probing from a spawn pad
			// finds nothing, because the pads sit inside an endzone with no 1x-height cover in range.
			// The run then sat in the search for the rest of the session. The ledge that was found is
			// still there and still valid; phase 1 already knows how to walk to it.
			UE_LOG(LogTraceGame, Warning,
				TEXT("LEDGE run %d: never reached the face (dist=%.0f, planar=%.0f, grounded=%d) "
				     "- walking back to the mark"),
				LedgeTestRun + 1, DistToFace, GetPlanarSpeed(), IsMovingOnGround() ? 1 : 0);
			Advance(1);
		}
		break;
	}

	// --- 3. HOLD FORWARD THROUGH THE CONTACT ----------------------------------------------------
	//
	// One log line per frame here, and only here: the whole contact event at full resolution. This is
	// what says whether the pawn was airborne and pushing when it met the lip, whether it stalled,
	// and on which frame it changed ground state.
	//
	// The per-frame line prints the engine's own movement mode and ground answer — the quantity the
	// client and the server have to agree about — and the running per-contact flip and correction
	// deltas, so the log shows the moment a disagreement lands rather than only the total at the end.
	case 3:
	{
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);
		CharacterOwner->StopJumping();

		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE   contact t=%.3f z=%7.1f feet=%7.1f planar=%6.0f velZ=%7.0f mode=%d "
			     "grounded=%d grace=%.3f dist=%6.0f | flips=%d corr=%d"),
			PhaseElapsed, Here.Z, Here.Z - TestHalfHeight, GetPlanarSpeed(), Velocity.Z,
			static_cast<int32>(MovementMode), IsMovingOnGround() ? 1 : 0, GroundGraceRemaining,
			DistToFace, LedgeTestGroundFlips - LedgeTestFlipsAtJump,
			CorrectionCount - LedgeTestCorrAtJump);

		// THE CONTACT WINDOW ENDS WHEN THE PAWN HAS SETTLED, NOT ON A STOPWATCH — but with a stopwatch
		// backstop, because "never settles" is itself a result this test has to be able to report.
		// 0.45 s is past the earliest possible landing (the feet cross 176 uu at t=0.39 s), so a
		// window cannot close while the pawn is still on the way up.
		const bool bSettled = IsMovingOnGround() && PhaseElapsed > 0.42f;
		if (bSettled || PhaseElapsed > 1.5f)
		{
			Advance(4);
		}
		break;
	}

	// --- 4. SETTLE, THEN SCORE THE CONTACT ------------------------------------------------------
	//
	// SPEC v12 §5. This is where the diagnosis actually happens, and every number here is a delta
	// against the jump-frame snapshot rather than a session total, so "which contact was bad" is
	// answerable instead of only "how many were there in eight runs".
	case 4:
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);
		if (PhaseElapsed > 0.4f)
		{
			++LedgeTestRun;

			const int32 ContactFlips = LedgeTestGroundFlips - LedgeTestFlipsAtJump;
			const int32 ContactCorr = CorrectionCount - LedgeTestCorrAtJump;
			const float ContactWorstErr = FMath::Max(0.f, CorrectionErrorWorst - LedgeTestWorstErrAtJump);

			// KEPT: planar speed after the lip over planar speed at the jump. A clean crossing keeps
			// essentially all of it (the ground model bleeds overspeed, but a walk-speed approach has
			// none to bleed). Well under 1.0 is the "stall" the request asks about, in a number.
			const float Kept = GetPlanarSpeed() / FMath::Max(1.f, LedgeTestSpeedAtJump);

			// ADVANCE: how far the pawn actually got along its run direction across the whole contact.
			// Negative is a PULL-BACK — the pawn ended up behind where it jumped from — which is the
			// literal reading of "rubber banding" and is worth counting separately from a stall.
			const float Advance2D = static_cast<float>(
				FVector::DotProduct(Here - LedgeTestPosAtJump, LedgeTestRunDirection));

			// ON TOP: did the jump actually end up on the raised section?
			const bool bOnTop = (Here.Z - TestHalfHeight) > (LedgeTestTopPoint.Z - 20.f);

			++LedgeTestContacts;
			LedgeTestContactFlips += ContactFlips;
			LedgeTestContactCorrections += ContactCorr;
			LedgeTestWorstContactFlips = FMath::Max(LedgeTestWorstContactFlips, ContactFlips);
			LedgeTestWorstContactErr = FMath::Max(LedgeTestWorstContactErr, ContactWorstErr);
			LedgeTestKeptFractionTotal += Kept;
			LedgeTestWorstKeptFraction = FMath::Min(LedgeTestWorstKeptFraction, Kept);
			LedgeTestLandedOnTop += bOnTop ? 1 : 0;
			LedgeTestPulledBack += (Advance2D < 0.f) ? 1 : 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE run %2d: feet=%7.1f (ledgeTop %7.1f) onTop=%d grounded=%d | THIS CONTACT: "
				     "flips=%d corr=%d worstErr=%6.2fuu kept=%.3f (%4.0f -> %4.0f uu/s) advance=%+7.1fuu"),
				LedgeTestRun, Here.Z - TestHalfHeight, LedgeTestTopPoint.Z,
				bOnTop ? 1 : 0, IsMovingOnGround() ? 1 : 0,
				ContactFlips, ContactCorr, ContactWorstErr, Kept,
				LedgeTestSpeedAtJump, GetPlanarSpeed(), Advance2D);

			if (LedgeTestRun >= RequiredRuns)
			{
				LogVerdict(TEXT("end"));
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
		     "corrections(in wall jump)=%d => %.3f per jump | hardCap=%.0f | "
		     "v9: legacyTuning=%d window=%.3fs | v12 §5: mantle removed, so mantleSteals is "
		     "identically 0 and the mantle lockout no longer exists%s"),
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
		IsV9LegacyTuning() ? 1 : 0,
		GetWallJumpWindowSeconds(),
		(CharacterOwner != nullptr && CharacterOwner->HasAuthority())
			? TEXT("  [AUTHORITY - the correction column is meaningless here]") : TEXT(""));
}

// =================================================================================================
// SPEC v10 §5 — THE STICK METER.
//
// "Wall jumping still feels like the player is sticking to the wall for a moment too long." That is a
// claim about MILLISECONDS, and v9 answered it by shortening a config value and then reporting that
// the config value was shorter — which is not a measurement of the symptom, and is why the same
// sentence came back a demo later. This measures the symptom itself:
//
//   from the frame the capsule first TOUCHES a wall, to the frame the pawn has moved
//   WallStickClearUU away from that face measured along the face's own normal.
//
// It is agnostic about the cause on purpose. Waiting for the window, a press that got eaten, the
// launch itself, and the launch being clawed back by held input are ALL inside the interval, so the
// number moves if and only if the player's experience does.
//
// Re-contacts do not re-anchor an open sample: a player scraping down a face is having one sticky
// experience, not thirty. A sample closes when the pawn clears the face, when it lands, or on a
// timeout — and a timeout is reported separately, because "never got off the wall at all" is the
// worst case and must not be averaged into a number that looks merely bad.
// =================================================================================================

/** Clear of the wall: half a capsule diameter out along the normal. CapsuleRadius is 34 uu. */
static constexpr float WallStickClearUU = 50.f;

/** A sample that has not cleared by here is a stick, not a slow escape. Reported in its own column. */
static constexpr float WallStickTimeoutSeconds = 1.5f;

void UTraceCharacterMovementComponent::BeginWallStickSample(const FVector& PlanarNormal)
{
	const UWorld* StickWorld = GetWorld();
	if (StickWorld == nullptr || UpdatedComponent == nullptr || CharacterOwner == nullptr)
	{
		return;
	}

	// RECORD PASS ONLY. A replayed move re-runs HandleImpact against the same static geometry, and a
	// re-anchored sample would restart the clock mid-bout on every correction — turning the client's
	// measurement into a measurement of its own correction rate. TickDashPitchTest's rule, verbatim.
	// ...and only on a pawn this process drives, to match TickWallStickSample()'s own gate. A sample
	// opened on a pawn nothing ticks would never be closed and would block every later one.
	if (CharacterOwner->bClientUpdating || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// An open sample keeps its anchor. See the block comment: re-contact is part of the stick, not a
	// new one.
	if (WallStickContactTime >= 0.f)
	{
		return;
	}

	WallStickContactTime = static_cast<float>(StickWorld->GetTimeSeconds());
	WallStickAnchor = UpdatedComponent->GetComponentLocation();
	WallStickNormal = PlanarNormal;
	WallStickLaunchTime = -1.f;
	WallStickPeakOutUU = 0.f;

	// Latched AT CONTACT, not at close: WallJumpsSinceGround is incremented by the launch this sample
	// is about, so reading it later would relabel every first jump as a chained one.
	bWallStickSampleChained = (WallJumpsSinceGround > 0) ? 1 : 0;
}

void UTraceCharacterMovementComponent::TickWallStickSample()
{
	if (WallStickContactTime < 0.f)
	{
		return;
	}

	const UWorld* StickWorld = GetWorld();
	if (StickWorld == nullptr || UpdatedComponent == nullptr)
	{
		CloseWallStickSample(false);
		return;
	}

	// Displacement from the anchor ALONG THE FACE'S NORMAL. Not straight-line distance: sliding 300 uu
	// along a wall you are still touching is not getting off the wall, and a distance test would score
	// the stickiest case in the game as a clean escape.
	const float OutUU = static_cast<float>(FVector::DotProduct(
		UpdatedComponent->GetComponentLocation() - WallStickAnchor, WallStickNormal));
	WallStickPeakOutUU = FMath::Max(WallStickPeakOutUU, OutUU);

	if (OutUU >= WallStickClearUU)
	{
		CloseWallStickSample(true);
		return;
	}

	// Landed without ever getting off the face. Counted as a non-clear rather than discarded — a wall
	// jump that dumped the player at the foot of the wall is exactly the experience being complained
	// about, and dropping it would bias the mean toward the samples that worked.
	if (IsGroundedForAbilities()
		|| static_cast<float>(StickWorld->GetTimeSeconds()) - WallStickContactTime >= WallStickTimeoutSeconds)
	{
		CloseWallStickSample(false);
	}
}

void UTraceCharacterMovementComponent::CloseWallStickSample(const bool bCleared)
{
	const UWorld* StickWorld = GetWorld();
	if (WallStickContactTime < 0.f || StickWorld == nullptr)
	{
		WallStickContactTime = -1.f;
		return;
	}

	const int32 Phase = FMath::Clamp(WallStickPhase, 0, 1);
	const float Now = static_cast<float>(StickWorld->GetTimeSeconds());
	const float ClearMs = 1000.f * (Now - WallStickContactTime);

	// A CONTACT THAT WAS NEVER WALL-JUMPED IS NOT A STICKY WALL JUMP. Brushing a face in passing, or
	// touching one with both ladder charges already spent, is a contact the player asked nothing of.
	// Counted on its own line and kept out of the mean — see WallStickNoLaunch in the header.
	if (WallStickLaunchTime < 0.f)
	{
		++WallStickNoLaunch[Phase];
		if (bWallStickSampleChained != 0)
		{
			++WallStickChainedNoLaunch;
		}
		WallStickContactTime = -1.f;
		WallStickPeakOutUU = 0.f;
		return;
	}

	++WallStickSamples[Phase];
	WallStickPeakOutSum[Phase] += WallStickPeakOutUU;

	if (bWallStickSampleChained != 0)
	{
		++WallStickChainedSamples;
		WallStickChainedPeakOutSum += WallStickPeakOutUU;
		const float ChainedMs = bCleared ? ClearMs : (1000.f * WallStickTimeoutSeconds);
		WallStickChainedClearMsSum += ChainedMs;
		WallStickChainedClearMsWorst = FMath::Max(WallStickChainedClearMsWorst, ChainedMs);
		if (!bCleared)
		{
			++WallStickChainedNeverCleared;
		}
	}

	if (bCleared)
	{
		WallStickClearMsSum[Phase] += ClearMs;
		WallStickClearMsWorst[Phase] = FMath::Max(WallStickClearMsWorst[Phase], ClearMs);
	}
	else
	{
		// Never cleared. Charged at the full timeout so it cannot flatter the mean — the alternative,
		// excluding it, would let a fix that turned clean escapes into permanent sticks report an
		// IMPROVEMENT, which is the one result this meter must be incapable of producing.
		++WallStickNeverCleared[Phase];
		WallStickClearMsSum[Phase] += 1000.f * WallStickTimeoutSeconds;
		WallStickClearMsWorst[Phase] = FMath::Max(WallStickClearMsWorst[Phase], 1000.f * WallStickTimeoutSeconds);
	}

	if (WallStickLaunchTime >= 0.f)
	{
		WallStickPressMsSum[Phase] += 1000.f * (WallStickLaunchTime - WallStickContactTime);
	}

	WallStickContactTime = -1.f;
	WallStickLaunchTime = -1.f;
	WallStickPeakOutUU = 0.f;
}

void UTraceCharacterMovementComponent::LogWallStickReport() const
{
	static const TCHAR* PhaseNames[2] = { TEXT("HEAD-ON "), TEXT("GLANCING") };

	UE_LOG(LogTraceGame, Display,
		TEXT("WALLSTICK REPORT %-16s netMode=%d role=%d | arm=%s clearAt=%.0fuu | lockout=%.2fs "
		     "buffer=%.2fs retention=%.4f outward=%.0f"),
		*GetNameSafe(CharacterOwner),
		(GetWorld() != nullptr) ? static_cast<int32>(GetWorld()->GetNetMode()) : -1,
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1,
		IsV10LegacyWallJump() ? TEXT("RED (v9 behaviour)") : TEXT("GREEN (v10 fix)"),
		WallStickClearUU,
		GetWallJumpControlLockoutSeconds(), GetWallJumpInputBufferSeconds(),
		GetWallJumpSpeedRetention(), GetWallJumpOutwardImpulse());

	for (int32 Phase = 0; Phase < 2; ++Phase)
	{
		const float Denominator = static_cast<float>(FMath::Max(1, WallStickSamples[Phase]));
		UE_LOG(LogTraceGame, Display,
			TEXT("WALLSTICK   %s presses early=%2d onTime=%2d -> wallJumps=%3d (contacts with NO launch"
			     "=%d) | STICK=%7.1f ms mean (worst %7.1f) | contact->launch=%6.1f ms | peakOut=%6.1f uu "
			     "| neverCleared=%d/%d"),
			PhaseNames[Phase],
			WallStickEarlyPresses[Phase], WallStickOnTimePresses[Phase],
			WallStickSamples[Phase], WallStickNoLaunch[Phase],
			WallStickClearMsSum[Phase] / Denominator,
			WallStickClearMsWorst[Phase],
			WallStickPressMsSum[Phase] / Denominator,
			WallStickPeakOutSum[Phase] / Denominator,
			WallStickNeverCleared[Phase], WallStickSamples[Phase]);
	}

	// THE HEADLINE. A cross-cut of the two lines above, not a third phase — see the header. This is the
	// jump taken from the air, where the outward impulse is the whole launch and the player's held
	// input is strong enough to beat it, and it is the number the fix is supposed to move.
	const float ChainedDenominator = static_cast<float>(FMath::Max(1, WallStickChainedSamples));
	UE_LOG(LogTraceGame, Display,
		TEXT("WALLSTICK   CHAINED  (2nd+ jump of a chain, arrived under air control) wallJumps=%3d "
		     "(contacts with NO launch=%d) | STICK=%7.1f ms mean (worst %7.1f) | peakOut=%6.1f uu "
		     "| neverCleared=%d/%d"),
		WallStickChainedSamples, WallStickChainedNoLaunch,
		WallStickChainedClearMsSum / ChainedDenominator,
		WallStickChainedClearMsWorst,
		WallStickChainedPeakOutSum / ChainedDenominator,
		WallStickChainedNeverCleared, WallStickChainedSamples);
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

	// --- SPEC v10 §5: TWO APPROACH PHASES, BECAUSE THEY ARE TWO DIFFERENT MECHANICS ---------------
	//
	// PHASE 0, HEAD-ON (0-22 s). What the v8 harness always did. The FIRST jump of each chain arrives
	// off a full-speed ground run and reflects a large outward launch, so it escapes on its own — and
	// it is the case every previous measurement looked at. The SECOND, taken from the air, does not:
	// AirMaxWishSpeed caps the return at 160 uu/s, so there is nothing left to reflect.
	//
	// PHASE 1, GLANCING (22-44 s). Running along a face at ~20° to it — which is what a player
	// actually does in a corridor. There is almost nothing pointing into the wall to reflect even on
	// the first jump, so WallJumpOutwardImpulse (420 uu/s, flat) is essentially the whole outward
	// launch, and it is the case the player's held input can beat. THIS IS THE COLUMN THE COMPLAINT
	// LIVES IN, and the old harness never ran it — a large part of why v9 "verified" a fix the
	// players did not feel.
	//
	// The glancing run alternates its along-wall direction every 5 s so the pawn stays inside the
	// arena instead of running 26000 uu down it and measuring the end wall instead.
	const bool bGlancing = (WallJumpTestTime >= 30.f);
	const float WallSign = (WallJumpTestYaw >= 0.f) ? 1.f : -1.f;
	const float AlongSign = ((static_cast<int32>(WallJumpTestTime) / 5) % 2 == 0) ? 1.f : -1.f;
	const float RunYaw = bGlancing
		? (90.f * WallSign - 70.f * WallSign * AlongSign)
		: WallJumpTestYaw;

	WallStickPhase = bGlancing ? 1 : 0;

	if (bGlancing && WallJumpTestTime - DeltaSeconds < 30.f)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("WALLJUMP ---- phase 1: GLANCING approach (~20deg to the face), the case the flat "
			     "outward impulse has to carry on its own."));
		LogWallStickReport();
	}

	// --- BACK OFF AND TAKE ANOTHER RUN AT IT ------------------------------------------------------
	//
	// See WallJumpTestRunUpUntil in the header: without this the harness pinned the pawn against the
	// face at zero speed and took THREE wall jumps in 22 seconds before seizing up entirely. A mean
	// over three samples is not a measurement, and it is how a harness reports a healthy mechanic
	// while measuring almost nothing.
	//
	// The 1.5 s guard on re-arming matters: coming out of a run-up the pawn is still moving AWAY, so
	// it passes back down through 300 uu/s while it turns around, and without the guard that would
	// immediately re-trigger a second run-up and the pawn would oscillate on the spot forever.
	const bool bRunningUp = (WallJumpTestTime < WallJumpTestRunUpUntil);
	if (!bRunningUp
		&& WallJumpTestTime > 1.f
		&& WallJumpTestTime > WallJumpTestRunUpUntil + 1.5f
		&& IsMovingOnGround()
		&& GetPlanarSpeed() < 300.f)
	{
		WallJumpTestRunUpUntil = WallJumpTestTime + 1.2f;
	}

	// THE AIM STAYS ON THE WALL EVEN WHILE BACKING OFF, and only the movement input reverses — that is
	// a player strafing back for another go, and it keeps the approach direction (which is what the
	// reflection is computed from) honest for the frame the pawn actually arrives.
	TestController->SetControlRotation(FRotator(0.f, RunYaw, 0.f));
	const FRotationMatrix DriveBasis(FRotator(0.f, bRunningUp ? (RunYaw + 180.f) : RunYaw, 0.f));
	CharacterOwner->AddMovementInput(DriveBasis.GetUnitAxis(EAxis::X), 1.f);

	// --- THE PRESS. See WallJumpTestApproach in the header for why the old rule was blind. --------
	//
	// Approaches alternate: EVEN presses EARLY (a human aiming at the wall), ODD presses on
	// IsWallJumpAvailable() (the v8 rule). An approach begins the moment the pawn leaves the ground.
	const bool bAirborneNow = !IsMovingOnGround();
	if (bAirborneNow && bWallJumpTestWasAirborne == 0)
	{
		++WallJumpTestApproach;
		bWallJumpTestPressLatched = 0;
	}
	if (!bAirborneNow)
	{
		bWallJumpTestPressLatched = 0;
	}
	bWallJumpTestWasAirborne = bAirborneNow ? 1 : 0;

	// CHAIN THE JUMPS — see WallJumpTestLastChainCount in the header. A launch, and only a launch,
	// re-arms the latch: the second jump of a chain is taken from the air, where AirMaxWishSpeed caps
	// the return at 160 uu/s and the flat outward impulse is the entire launch, and that is the case
	// the complaint is actually about. Without this the harness reported the healthy first jump.
	if (WallJumpsSinceGround > WallJumpTestLastChainCount)
	{
		bWallJumpTestPressLatched = 0;
	}
	WallJumpTestLastChainCount = bAirborneNow ? WallJumpsSinceGround : 0;

	bool bPressedThisFrame = false;
	if (IsFalling() && bWallJumpTestPressLatched == 0)
	{
		if ((WallJumpTestApproach % 2) == 0)
		{
			// "PRESS JUMP RIGHT AS THEY HIT A WALL" — the spec's own words, and the press this
			// project has never actually tested. A lead in TIME rather than in distance, so it is the
			// same two-frame anticipation at any speed and at any approach angle: the trace runs along
			// the direction of travel, so its length IS the time to contact.
			const float LeadReach = FMath::Max(30.f, GetPlanarSpeed() * 0.035f);
			const FVector LeadFrom = UpdatedComponent->GetComponentLocation();
			const FVector LeadTo = LeadFrom + FRotationMatrix(FRotator(0.f, RunYaw, 0.f)).GetUnitAxis(EAxis::X) * LeadReach;

			// THE PAWN'S OWN CHANNEL, not ECC_Visibility. Measured: on ECC_Visibility this trace hit
			// NOTHING — "presses early= 0" for a whole run — because the arena's walls do not block
			// the visibility channel. A wall is, by definition, a thing that blocks THIS capsule, so
			// ask the question in the capsule's own terms and the answer cannot be a collision-setup
			// detail. (This is a harness bug the report would have hidden as "the fix did nothing".)
			FHitResult LeadHit;
			FCollisionQueryParams LeadParams(SCENE_QUERY_STAT(TraceWallJumpLead), false, CharacterOwner);
			if (TestWorld->LineTraceSingleByChannel(LeadHit, LeadFrom, LeadTo,
				UpdatedComponent->GetCollisionObjectType(), LeadParams))
			{
				CharacterOwner->Jump();
				bPressedThisFrame = true;
				++WallStickEarlyPresses[FMath::Clamp(WallStickPhase, 0, 1)];
			}
		}
		else if (IsWallJumpAvailable())
		{
			CharacterOwner->Jump();
			bPressedThisFrame = true;
			++WallStickOnTimePresses[FMath::Clamp(WallStickPhase, 0, 1)];
		}
	}

	if (bPressedThisFrame)
	{
		// ONE PRESS PER APPROACH. A human presses once and then wonders why nothing happened; a
		// harness that mashed would paper over the eaten-press bug it is here to measure.
		bWallJumpTestPressLatched = 1;
	}
	else if (!bRunningUp && IsMovingOnGround() && WallJumpTestTime > 1.f && GetPlanarSpeed() > 300.f)
	{
		// Grounded with a run-up: the mechanic is airborne-only, so get airborne. An ordinary jump.
		// Refused mid-run-up, or the pawn would launch itself backwards away from the wall.
		CharacterOwner->Jump();
	}

	if (WallJumpTestTime >= 60.f && bWallJumpTestReported == 0)
	{
		bWallJumpTestReported = 1;
		UE_LOG(LogTraceGame, Display, TEXT("WALLJUMP ---- end."));
		LogWallJumpReport();
		LogWallStickReport();
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

// =================================================================================================
// SPEC v9 §2 / §0 — THE SINGLE-DASH REPRODUCTION (-TraceSingleDashTest)
// =================================================================================================
//
// Spec v9 §0 exists because the last pass's harness pressed dash TWICE FROM A FULL POOL, saw two
// launches, and called the bug fixed. That test could not fail: both launches genuinely happen. It
// never touched the state the user described.
//
// This one reproduces the user's sentences literally, on a CLIENT, WHILE CARRYING:
//
//   "When dash is used, both charges are consumed."   -> dash ONCE from a full pool and read the
//                                                        pool AND THE HUD.
//   "When the first refills, they both do."           -> drain to zero and watch the 0 -> 1 refill,
//                                                        reading the HUD on the frame it lands.
//   "despite the hud showing two"                     -> every sample compares the true pool with
//                                                        the number of pips ATraceHUD would DRAW.
//
// THE PIP COUNT IS THE POINT. It is recomputed here by the identical rule ATraceHUD::DrawChargePips
// uses (pip i is full when i < Charges, and pip[Charges] is drawn at RechargeFraction), from the
// real ATracePlayerController::GetDashHudState — not from a local reimplementation of the maths.
// What the player sees is a count of full pips, so that is what is asserted against.
//
// A test that only reads DashCharges would have gone green on the broken build, because the pool was
// never the thing that was wrong.

/** One line of the ledger: what the pawn holds, and what the HUD would draw for it. */
struct FTraceSingleDashSample
{
	int32 Charges = 0;
	int32 MaxCharges = 1;
	bool  bHudValid = false;
	int32 HudCharges = 0;
	int32 HudMaxCharges = 1;
	float HudRechargeFraction = 0.f;
	float HudRemaining = 0.f;

	/** Pips ATraceHUD::DrawChargePips would render SOLID. This is the number the player reads. */
	int32 HudFullPips = 0;
};

static void TraceSingleDashCheck(const TCHAR* Name, const bool bPass, int32& InOutFailures, const FString& Detail)
{
	if (!bPass)
	{
		++InOutFailures;
	}

	UE_LOG(LogTraceGame, Display, TEXT("SINGLEDASH  [%s] %-46s %s"),
		bPass ? TEXT("PASS") : TEXT("FAIL"), Name, *Detail);
}

FTraceSingleDashSample UTraceCharacterMovementComponent::SampleSingleDashTest() const
{
	FTraceSingleDashSample Sample;
	Sample.Charges = DashCharges;
	Sample.MaxCharges = FMath::Max(1, GetMaxDashCharges());

	// THE REAL HUD PATH, deliberately. Reimplementing GetDashHudState here would test this harness's
	// idea of the HUD rather than the HUD, which is the exact failure spec v9 §0 is about.
	const ATracePlayerController* PC = (CharacterOwner != nullptr)
		? Cast<ATracePlayerController>(CharacterOwner->GetController())
		: nullptr;

	FTraceDashHudState HudState;
	if (PC != nullptr && PC->GetDashHudState(HudState))
	{
		Sample.bHudValid = true;
		Sample.HudCharges = HudState.Charges;
		Sample.HudMaxCharges = FMath::Max(1, HudState.MaxCharges);
		Sample.HudRechargeFraction = HudState.RechargeFraction;
		Sample.HudRemaining = HudState.Remaining;

		// ATraceHUD::DrawChargePips, verbatim: banked charges are full, pip[Charges] shows the
		// recharge progress, everything past it is empty. A pip at fraction 1.0 is indistinguishable
		// from a banked one on screen, so that is what "full" means here.
		for (int32 Index = 0; Index < Sample.HudMaxCharges; ++Index)
		{
			float Fraction = 0.f;
			if (Index < HudState.Charges)
			{
				Fraction = 1.f;
			}
			else if (Index == HudState.Charges)
			{
				Fraction = FMath::Clamp(HudState.RechargeFraction, 0.f, 1.f);
			}

			if (Fraction >= 0.999f)
			{
				++Sample.HudFullPips;
			}
		}
	}

	return Sample;
}

void UTraceCharacterMovementComponent::TickSingleDashTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceSingleDashTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// TickDashPitchTest's reason: a replayed move must not advance a clock that fires input.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	UWorld* TestWorld = GetWorld();
	ATraceCharacter* TraceOwner = Cast<ATraceCharacter>(CharacterOwner);
	if (TestWorld == nullptr || TraceOwner == nullptr)
	{
		return;
	}

	// --- THE SERVER HALF: hand the Core to the JOINED CLIENT's pawn -------------------------------
	//
	// Trace.DebugTakeCore only ever targets the LOCAL pawn, which on a listen host is the host — the
	// one machine spec v9 §0 says does not count. So the authority pushes the Core through
	// ATraceCore::TryPickup(), the same funnel the pickup sphere uses, and bIsCarrier reaches the
	// client by replication exactly as it would in a real match.
	if (CharacterOwner->HasAuthority() && !CharacterOwner->IsLocallyControlled())
	{
		if (bSingleDashCoreGiven != 0 || Cast<APlayerController>(CharacterOwner->GetController()) == nullptr)
		{
			return;
		}

		if (TestWorld->GetTimeSeconds() < 10.f)
		{
			return;
		}

		// WAITING FOR A LOOSE CORE IS NOT GOOD ENOUGH, and this cost two whole runs. The original loop
		// skipped every Core that IsHeld() and simply retried next frame, so it depended on the Core
		// happening to be on the floor while the ten bots in the match are fighting over it. Measured:
		// in one arm the wait ended at t=53.7 (the harness arms at t=75, so it barely made it) and in
		// the next it never ended at all — the client reported "NEVER ARMED ... carrier=0" and the run
		// produced no measurement. A harness whose ability to take a reading depends on bot behaviour
		// is not a harness.
		//
		// So: prefer a loose Core (TryPickup is the ordinary player funnel and is what a real pickup
		// does), but after a short grace TAKE it from whoever is holding it. GrantTo() is documented in
		// TraceCore.h as "the single funnel: every other path ends up here" — a kill, a pass and a
		// kickoff all reach it — so robbing a bot exercises exactly the code a real turnover does, and
		// ETraceCoreGrantReason::Debug is the reason the enum reserves for a harness grant.
		//
		// This changes NOTHING about what is measured. It only decides which pawn is carrying when the
		// measurement starts; every check runs afterwards, on the client, through the ordinary
		// replicated carrier bit.
		const bool bMayRob = TestWorld->GetTimeSeconds() >= 18.f;

		ATraceCore* Robbable = nullptr;
		for (TActorIterator<ATraceCore> It(TestWorld); It; ++It)
		{
			ATraceCore* Core = *It;
			if (Core == nullptr)
			{
				continue;
			}

			if (!Core->IsHeld())
			{
				Core->TryPickup(TraceOwner);
				bSingleDashCoreGiven = 1;
				UE_LOG(LogTraceGame, Display,
					TEXT("SINGLEDASH [server] gave the LOOSE Core to the joined client's pawn %s at t=%.1f (carrier=%d)"),
					*GetNameSafe(CharacterOwner), TestWorld->GetTimeSeconds(), TraceOwner->IsCarrier() ? 1 : 0);
				return;
			}

			if (Robbable == nullptr)
			{
				Robbable = Core;
			}
		}

		if (bMayRob && Robbable != nullptr)
		{
			Robbable->GrantTo(TraceOwner, ETraceCoreGrantReason::Debug);
			bSingleDashCoreGiven = 1;
			UE_LOG(LogTraceGame, Display,
				TEXT("SINGLEDASH [server] TOOK the Core from its holder for the joined client's pawn %s "
				     "at t=%.1f (carrier=%d)"),
				*GetNameSafe(CharacterOwner), TestWorld->GetTimeSeconds(), TraceOwner->IsCarrier() ? 1 : 0);
		}

		return;
	}

	// A HUMAN'S PAWN, NOT A BOT'S. Bot pawns are locally controlled ON THE SERVER too, and they have
	// no ATracePlayerController — so GetDashHudState has nothing to answer with and every bot would
	// report "the HUD draws 0 pips" forever. (Measured: eight bots did exactly that, and one of them
	// also spent a second charge on its own AI dash mid-phase and failed the consumption check with
	// it.) The claim under test is about what a PLAYER sees, so only a player's pawn qualifies.
	if (!CharacterOwner->IsLocallyControlled() || bSingleDashReported != 0
		|| Cast<ATracePlayerController>(CharacterOwner->GetController()) == nullptr)
	{
		return;
	}

	const FTraceSingleDashSample Sample = SampleSingleDashTest();
	const float Window = GetDashRechargeWindow();

	// --- PHASE -1: ARM. Wait for the Core to land and the pool to reach the carrier's maximum. -----
	if (SingleDashPhase < 0)
	{
		if (!TraceOwner->IsCarrier() || Sample.MaxCharges < 2 || Sample.Charges < Sample.MaxCharges
			|| !IsMovingOnGround() || DashTimeRemaining > 0.f)
		{
			if (TestWorld->GetTimeSeconds() > 75.f)
			{
				bSingleDashReported = 1;
				UE_LOG(LogTraceGame, Warning,
					TEXT("SINGLEDASH ==== NEVER ARMED by t=75: carrier=%d charges=%d/%d grounded=%d. "
					     "The Core never reached this client, or the pool never grew with it."),
					TraceOwner->IsCarrier() ? 1 : 0, Sample.Charges, Sample.MaxCharges,
					IsMovingOnGround() ? 1 : 0);
			}
			return;
		}

		SingleDashPhase = 0;
		SingleDashPhaseTime = 0.f;
		SingleDashPresses = 0;
		SingleDashPrevCharges = Sample.Charges;
		SingleDashPrevHudPips = Sample.HudFullPips;
		SingleDashMaxHudDivergence = 0;
		SingleDashDivergentSeconds = 0.f;
		SingleDashMaxTrueGain = 0;
		SingleDashMaxHudGain = 0;
		SingleDashHudPipsAtFirstRefill = -1;
		SingleDashNextSampleTime = 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("SINGLEDASH ==== ARMED at t=%.1f. netMode=%d role=%d carrier=%d | true=%d/%d  "
			     "hud=%d/%d frac=%.3f rem=%.2f pips=%d | rechargeWindow=%.2f"),
			TestWorld->GetTimeSeconds(), static_cast<int32>(GetNetMode()),
			static_cast<int32>(CharacterOwner->GetLocalRole()), TraceOwner->IsCarrier() ? 1 : 0,
			Sample.Charges, Sample.MaxCharges, Sample.HudCharges, Sample.HudMaxCharges,
			Sample.HudRechargeFraction, Sample.HudRemaining, Sample.HudFullPips, Window);
		return;
	}

	SingleDashPhaseTime += DeltaSeconds;

	// --- THE LEDGER. Every transition of either number, plus a heartbeat, plus the running worsts. --
	const int32 TrueGain = Sample.Charges - SingleDashPrevCharges;
	const int32 HudGain = Sample.HudFullPips - SingleDashPrevHudPips;
	const bool bChanged = (TrueGain != 0) || (HudGain != 0);

	SingleDashMaxHudDivergence = FMath::Max(SingleDashMaxHudDivergence,
		FMath::Abs(Sample.HudFullPips - Sample.Charges));

	// See SingleDashDivergentSeconds in the header for why this is timed rather than counted.
	if (Sample.HudFullPips != Sample.Charges)
	{
		SingleDashDivergentSeconds += DeltaSeconds;
	}
	SingleDashMaxTrueGain = FMath::Max(SingleDashMaxTrueGain, TrueGain);
	SingleDashMaxHudGain = FMath::Max(SingleDashMaxHudGain, HudGain);

	// THE "WHEN THE FIRST REFILLS, THEY BOTH DO" MEASUREMENT: the pip count on the exact frame the
	// pool climbs off zero. If the readout is honest this is 1. Recorded once.
	if (SingleDashPrevCharges == 0 && Sample.Charges == 1 && SingleDashHudPipsAtFirstRefill < 0)
	{
		SingleDashHudPipsAtFirstRefill = Sample.HudFullPips;
	}

	if (bChanged || TestWorld->GetTimeSeconds() >= SingleDashNextSampleTime)
	{
		SingleDashNextSampleTime = static_cast<float>(TestWorld->GetTimeSeconds()) + 0.5f;
		UE_LOG(LogTraceGame, Display,
			TEXT("SINGLEDASH t=%6.2f ph%d %-7s | TRUE %d/%d | HUD pips %d/%d (charges=%d frac=%.3f rem=%.2f) "
			     "| refillClock=%5.2f dash=%4.2f%s"),
			TestWorld->GetTimeSeconds(), SingleDashPhase, bChanged ? TEXT("CHANGE") : TEXT(""),
			Sample.Charges, Sample.MaxCharges, Sample.HudFullPips, Sample.HudMaxCharges,
			Sample.HudCharges, Sample.HudRechargeFraction, Sample.HudRemaining,
			DashRechargeRemaining, DashTimeRemaining,
			(Sample.HudFullPips != Sample.Charges) ? TEXT("   <<< HUD DISAGREES WITH THE POOL") : TEXT(""));
	}

	SingleDashPrevCharges = Sample.Charges;
	SingleDashPrevHudPips = Sample.HudFullPips;

	switch (SingleDashPhase)
	{
	case 0:
	{
		// --- "When dash is used, both charges are consumed." ONE press, from a full pool. ---------
		if (SingleDashPhaseTime >= 0.2f && SingleDashPresses == 0)
		{
			SingleDashPresses = 1;
			UE_LOG(LogTraceGame, Display,
				TEXT("SINGLEDASH ---- PHASE 0: ONE dash press from a full pool (true=%d/%d, hud pips=%d)"),
				Sample.Charges, Sample.MaxCharges, Sample.HudFullPips);
			StartDash();
		}
		else if (SingleDashPhaseTime >= 1.4f)
		{
			TraceSingleDashCheck(TEXT("one press spends exactly one charge"),
				Sample.Charges == Sample.MaxCharges - 1, SingleDashFailures,
				FString::Printf(TEXT("pool is %d/%d after one press, expected %d/%d"),
					Sample.Charges, Sample.MaxCharges, Sample.MaxCharges - 1, Sample.MaxCharges));

			TraceSingleDashCheck(TEXT("HUD shows one charge gone after one press"),
				Sample.HudFullPips == Sample.MaxCharges - 1, SingleDashFailures,
				FString::Printf(TEXT("HUD draws %d full pips, pool holds %d  (frac=%.3f rem=%.2f)"),
					Sample.HudFullPips, Sample.Charges, Sample.HudRechargeFraction, Sample.HudRemaining));

			SingleDashPhase = 1;
			SingleDashPhaseTime = 0.f;
		}
		break;
	}

	case 1:
	{
		// --- The 1 -> 2 refill. One charge, not two. ---------------------------------------------
		if (Sample.Charges >= Sample.MaxCharges || SingleDashPhaseTime > Window + 3.f)
		{
			TraceSingleDashCheck(TEXT("the pool refilled back to full"),
				Sample.Charges >= Sample.MaxCharges, SingleDashFailures,
				FString::Printf(TEXT("pool is %d/%d after %.1fs (window %.2fs)"),
					Sample.Charges, Sample.MaxCharges, SingleDashPhaseTime, Window));

			SingleDashPhase = 2;
			SingleDashPhaseTime = 0.f;
			SingleDashPresses = 0;
		}
		break;
	}

	case 2:
	{
		// --- Drain the pool to ZERO, so the 0 -> 1 refill can be watched. -------------------------
		const float SecondPressAt = 0.2f + FMath::Max(0.35f, GetDashDuration() + 0.2f);
		if (SingleDashPhaseTime >= 0.2f && SingleDashPresses == 0)
		{
			SingleDashPresses = 1;
			StartDash();
		}
		else if (SingleDashPhaseTime >= SecondPressAt && SingleDashPresses == 1)
		{
			SingleDashPresses = 2;
			StartDash();
		}
		else if (SingleDashPhaseTime >= SecondPressAt + 1.2f)
		{
			TraceSingleDashCheck(TEXT("two presses spend exactly two charges"),
				Sample.Charges == 0, SingleDashFailures,
				FString::Printf(TEXT("pool is %d/%d after two presses, expected 0/%d"),
					Sample.Charges, Sample.MaxCharges, Sample.MaxCharges));

			TraceSingleDashCheck(TEXT("HUD reads empty when the pool is empty"),
				Sample.HudFullPips == 0, SingleDashFailures,
				FString::Printf(TEXT("HUD draws %d full pips on a pool of %d"),
					Sample.HudFullPips, Sample.Charges));

			SingleDashPhase = 3;
			SingleDashPhaseTime = 0.f;
			SingleDashHudPipsAtFirstRefill = -1;
			SingleDashMaxTrueGain = 0;
			SingleDashMaxHudGain = 0;
		}
		break;
	}

	case 3:
	{
		// --- "When the first refills, they both do." THE MONEY SHOT. -----------------------------
		if (Sample.Charges >= Sample.MaxCharges || SingleDashPhaseTime > (2.f * Window) + 4.f)
		{
			TraceSingleDashCheck(TEXT("charges return ONE at a time"),
				SingleDashMaxTrueGain <= 1, SingleDashFailures,
				FString::Printf(TEXT("largest single-frame pool gain was +%d"), SingleDashMaxTrueGain));

			TraceSingleDashCheck(TEXT("when the FIRST refills, the HUD gains ONE pip"),
				SingleDashHudPipsAtFirstRefill == 1, SingleDashFailures,
				FString::Printf(TEXT("HUD drew %d full pips on the frame the pool went 0 -> 1"),
					SingleDashHudPipsAtFirstRefill));

			TraceSingleDashCheck(TEXT("HUD pips never jump by more than one"),
				SingleDashMaxHudGain <= 1, SingleDashFailures,
				FString::Printf(TEXT("largest single-frame pip gain was +%d"), SingleDashMaxHudGain));

			// One 60 Hz frame is 16.7 ms; 50 ms is three of them, which is slack for a hitching
			// offscreen run and still two orders of magnitude below the 3.68 s lie the bug produced.
			TraceSingleDashCheck(TEXT("HUD agrees with the pool (no visible lie)"),
				SingleDashDivergentSeconds <= 0.05f, SingleDashFailures,
				FString::Printf(TEXT("meter disagreed with the pool for %.3fs total "
					"(worst |pips - charges| = %d); budget 0.050s"),
					SingleDashDivergentSeconds, SingleDashMaxHudDivergence));

			bSingleDashReported = 1;
			UE_LOG(LogTraceGame, Display,
				TEXT("SINGLEDASH ==== %s  (%d failing check%s) — client, carrying, netMode=%d role=%d"),
				(SingleDashFailures == 0) ? TEXT("ALL CHECKS PASSED") : TEXT("FAILED"),
				SingleDashFailures, (SingleDashFailures == 1) ? TEXT("") : TEXT("s"),
				static_cast<int32>(GetNetMode()), static_cast<int32>(CharacterOwner->GetLocalRole()));
		}
		break;
	}

	default:
		break;
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

			// SPEC v10 §5. Printed alongside, always: the two answer different questions (what the
			// launch DID vs how long the player was stuck), and the second is the one the complaint is
			// about. Splitting them across two commands is how a measurement gets forgotten.
			Movement->LogWallStickReport();
		}
	}

	if (Reported == 0)
	{
		UE_LOG(LogTraceGame, Display, TEXT("WALLJUMP REPORT: no locally-controlled pawn in this process."));
	}
}

void UTraceCharacterMovementComponent::LogV9TuningReport() const
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	// GetGravityZ() is the engine's own accessor and already includes GravityScale, which is the field
	// spec v9 §8 moves. Reading it (rather than WorldGravityZ x GravityScale by hand) is what makes
	// this report the SAME number PhysFalling integrates.
	const float GravityZ = GetGravityZ();
	const float G = FMath::Max(1.f, FMath::Abs(GravityZ));

	// --- The launches, and the two things gravity does to each of them ----------------------------
	// apex = v^2 / 2g and hang time = 2v / g. Both scale as 1/g exactly, so a 12% heavier gravity is
	// -10.71% on every height and every air time in the game. Printing them per-launch rather than
	// quoting that one percentage is deliberate: the ABSOLUTE loss is what decides whether a specific
	// ledge is still clearable, and 10.71% of a big number is a big number.
	const float JumpZ = FMath::Max(1.f, JumpZVelocity);
	const float JumpApex = (JumpZ * JumpZ) / (2.f * G);
	const float JumpAirTime = (2.f * JumpZ) / G;

	const float WallJumpZ = JumpZ * GetWallJumpVerticalMultiplier();
	const float WallApex = (WallJumpZ * WallJumpZ) / (2.f * G);
	const float WallAirTime = (2.f * WallJumpZ) / G;

	// A slide-jump's Z is the jump's, times the slide-jump multiplier, times the well-timed Z bonus.
	const float SlideJumpZ = JumpZ * GetSlideJumpZMultiplier() * GetSlideJumpWindowZBonus();
	const float SlideJumpApex = (SlideJumpZ * SlideJumpZ) / (2.f * G);

	// The vertical dash. Its rise has TWO parts and only the second is gravity's: for
	// GetDashDuration() the dash holds DashSpeed on rails, then the exit clamp hands the pawn back at
	// no more than GetDashExitVerticalSpeedLimit() and the rest is ordinary ballistics.
	const float DashRailRise = GetDashSpeed() * GetDashDuration();
	const float DashExitZ = GetDashExitVerticalSpeedLimit();
	const float DashBallisticRise = (DashExitZ * DashExitZ) / (2.f * G);

	// --- The slide, integrated rather than asserted -----------------------------------------------
	// v(t) = v0 - a.t, so the slide ends at whichever comes first: the clock, or decaying to the exit
	// threshold. Distance is the integral to that moment. The reference entry speed is the air-strafe
	// hard cap, i.e. the fastest a player can legitimately arrive.
	const float SlideRefEntry = GetAirStrafeHardCapSpeed();
	const float SlideDecel = GetSlideDeceleration();
	const float SlideExitSpeed = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideExitSpeedFraction);
	const float SlideDecayTime = (SlideDecel > 1.f)
		? FMath::Max(0.f, (SlideRefEntry - SlideExitSpeed) / SlideDecel)
		: 1.0e9f;   // a literal, not BIG_NUMBER/MAX_FLT: those macros are UE_-prefixed in UE5.
	const float SlideT = FMath::Min(GetSlideDuration(), SlideDecayTime);
	const float SlideLength = SlideRefEntry * SlideT - 0.5f * SlideDecel * SlideT * SlideT;
	const float SlideEndSpeed = SlideRefEntry - SlideDecel * SlideT;

	// --- The slide-jump bonus, as the speed a player actually leaves at ---------------------------
	const float SlideJumpMissed = SlideEndSpeed * GetSlideJumpHorizontalRetention();
	const float SlideJumpTimed = SlideJumpMissed * GetSlideJumpWindowSpeedBonus();

	UE_LOG(LogTraceGame, Display, TEXT("V9TUNING ============================================================"));
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING arm=%s  pawn=%s netMode=%d role=%d"),
		IsV9LegacyTuning() ? TEXT("LEGACY (pre-v9)") : TEXT("V9 (shipped)"),
		*GetNameSafe(CharacterOwner),
		(GetWorld() != nullptr) ? static_cast<int32>(GetWorld()->GetNetMode()) : -1,
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1);

	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 gravity   scale=%.3f  gravityZ=%8.1f uu/s^2"),
		GravityScale, GravityZ);
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 airstrafe softCap=%7.1f  hardCap=%7.1f  (asymptoteScale=%.3f, maxAirSpeed=%7.1f)"),
		GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(), GetAirStrafeAsymptoteScale(),
		Settings.MaxAirSpeed);

	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 knock-on  jump:      z=%6.1f apex=%7.1fuu airTime=%.3fs"),
		JumpZ, JumpApex, JumpAirTime);
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 knock-on  wallJump:  z=%6.1f apex=%7.1fuu airTime=%.3fs"),
		WallJumpZ, WallApex, WallAirTime);
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 knock-on  slideJump: z=%6.1f apex=%7.1fuu (well-timed, zBonus=%.3f)"),
		SlideJumpZ, SlideJumpApex, GetSlideJumpWindowZBonus());
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 knock-on  dash(up):  railRise=%7.1fuu + ballistic=%7.1fuu = %7.1fuu (exitZ=%6.1f)"),
		DashRailRise, DashBallisticRise, DashRailRise + DashBallisticRise, DashExitZ);
	// The "MANTLE HEADROOM" line (jump apex vs the tallest climbable ledge) was here. Deleted with the
	// mantle in v12 §5. The jump apex itself is still printed two lines up, which is the number that
	// actually matters now: it is what decides whether a raised section is CLEARED and landed on —
	// the case the ledge complaint is about — or run into face-first.
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §5 wallJump  retention=%.4f  window=%.4fs  outward=%5.1f  zMul=%.3f  cap=%d "
		     "(v12 §5: no mantle lockout - nothing left to lock out)"),
		GetWallJumpSpeedRetention(), GetWallJumpWindowSeconds(), GetWallJumpOutwardImpulse(),
		GetWallJumpVerticalMultiplier(), GetWallJumpMaxConsecutive());

	// THE END-TO-END §5 NUMBER, AT A FIXED ENTRY SPEED — and it has to be fixed, because the aggregate
	// "% carried" in WALLJUMP REPORT is NOT comparable between arms. Two things confound it: the runs
	// arrive at the wall at different speeds (the §8 asymptote nudge alone moves the approach), and
	// GetWallJumpOutwardImpulse() is a FLAT addition, so it is a larger fraction of a slower approach.
	// That is why the measured legacy run reads 111.5% carried and the v9 run reads 118.3% — the v9 run
	// simply approached slower. Held at one speed the comparison is clean and monotonic.
	//
	// Head-on, so the reflection returns the whole planar component: launch = entry x retention +
	// outward. This is the arithmetic TryWallJump() performs, not a model of it.
	const float WallRefEntry = 1100.f;
	const float WallLaunch = WallRefEntry * GetWallJumpSpeedRetention() + GetWallJumpOutwardImpulse();
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §5 wallJump  HEAD-ON at %6.1f uu/s -> launch %7.1f uu/s = %.1f%% carried "
		     "(entry x %.4f + %.1f)"),
		WallRefEntry, WallLaunch, 100.f * WallLaunch / WallRefEntry,
		GetWallJumpSpeedRetention(), GetWallJumpOutwardImpulse());

	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §6 slide     duration=%.4fs  MAX LENGTH=%8.1fuu  (entry=%7.1f decel=%6.1f "
		     "endSpeed=%7.1f endedBy=%s)"),
		GetSlideDuration(), SlideLength, SlideRefEntry, SlideDecel, SlideEndSpeed,
		(SlideDecayTime <= GetSlideDuration()) ? TEXT("decay") : TEXT("clock"));

	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §7 slideJump multiplier=%.5f (base=%.5f gainOnly=%d)  missed=%7.1f -> timed=%7.1f "
		     "uu/s (+%6.1f)"),
		GetSlideJumpWindowSpeedBonus(), Settings.SlideJumpWindowSpeedBonus,
		TraceMoveKnob::Bool(TEXT("bSlideJumpBonusScalesGainOnly"), true) ? 1 : 0,
		SlideJumpMissed, SlideJumpTimed, SlideJumpTimed - SlideJumpMissed);
	UE_LOG(LogTraceGame, Display, TEXT("V9TUNING ============================================================"));
}

/**
 * Trace.V9.Tuning — the §§5-8 numbers for every locally-controlled pawn in this process.
 *
 * Safe on a host: nothing in the report is a prediction quantity, it is all config arithmetic, so
 * unlike the correction counters it means the same thing on either machine. (The wall-jump RETENTION
 * measurement is a different matter and still has to come from a client — see Trace.WallJumpReport.)
 */
static void TraceReportV9Tuning()
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
			Movement->LogV9TuningReport();
			break;
		}

		if (Reported > 0)
		{
			break;
		}
	}

	if (Reported == 0)
	{
		// The CDO still answers every getter correctly (they are pure config reads), so a process with
		// no pawn yet gets the tuning block rather than nothing at all. JumpZVelocity and GravityScale
		// are the authored defaults there, which is exactly right for a config report.
		GetDefault<UTraceCharacterMovementComponent>()->LogV9TuningReport();
	}
}

static FAutoConsoleCommand GTraceV9TuningCmd(
	TEXT("Trace.V9.Tuning"),
	TEXT("Dev only. Spec v9 secs 5-8: every tuned number plus the gravity knock-ons (jump apex, air "
	     "time, wall-jump apex, the vertical dash arc and the mantle headroom). Run it in both arms - "
	     "add -TraceLegacyTuning to the command line for the BEFORE numbers - and diff."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceReportV9Tuning(); }));

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
	, SavedWallJumpLaunchNormal(FVector::ZeroVector)
	, SavedWallJumpControlLockoutRemaining(0.f)
	, SavedWallJumpInputBufferRemaining(0.f)
	, bSavedKnifeMovementProfile(0)
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

	// Spec v5 §7. Moves are pooled, so this is reset like everything else: a stale ledge grace left
	// in a recycled move would tell a mid-air replay it was standing on something. The six Mantle*
	// companions that used to be cleared here went with the mantle in v12 §5.
	SavedGroundGraceRemaining = 0.f;

	// Spec v8 §7. Same pooling argument as the ledge grace above: a stale wall-jump window left in a
	// recycled move would let a replay take a wall jump off a wall that is no longer there, from a
	// normal belonging to a different surface.
	SavedWallJumpNormal = FVector::ZeroVector;
	SavedWallJumpWindowRemaining = 0.f;
	SavedWallJumpEntryVelocity = FVector::ZeroVector;
	SavedWallJumpsSinceGround = 0;

	// Spec v10 §5. Same pooling argument again, and it bites harder here than anywhere: a stale
	// lockout normal left in a recycled move would silently refuse an input direction on a replayed
	// frame that has no wall anywhere near it, and a stale buffer would fire a wall jump the player
	// never pressed.
	SavedWallJumpLaunchNormal = FVector::ZeroVector;
	SavedWallJumpControlLockoutRemaining = 0.f;
	SavedWallJumpInputBufferRemaining = 0.f;

	// Spec v10 §1. A stale knife bit would replay a move at 130% speed with the gun in hand.
	bSavedKnifeMovementProfile = 0;

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

	// SPEC v10 §1. A weapon swap changes GetMaxSpeed() by 30% and all three air ceilings by 25-35%,
	// so two moves either side of one are simulated against different limits. Merging them replays a
	// single move under whichever profile happens to be live, and the server — which ran them
	// separately — corrects the difference. A LEVEL, not an edge, exactly like the slide flag above.
	if (bSavedKnifeMovementProfile != Other->bSavedKnifeMovementProfile)
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
	// SPEC v5 §7's mantle used to be in this list — the strictest case in the kit, since its velocity
	// was (target - here)/time-left and its exit was a distance test. Removed in v12 §5 with the
	// mechanic. No merge that was refused for the mantle alone was refused for any other reason, so
	// this list gets slightly less strict; that is a bandwidth saving and not a behaviour change,
	// because there is no longer any move a mantle could have been live on.
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
	//
	// SPEC v10 §5 ADDS TWO MORE, and both belong in this list for the list's own stated reason.
	// The into-wall lockout makes the air-strafe non-linear in dt in a NEW way — the wish direction
	// itself changes when the clock expires mid-move — so one merged move of length 2dt straddling
	// the expiry is not the two moves of length dt it replaced. The buffered press is stronger still:
	// it decides whether a wall jump happens at all, and on which frame, and a merged move would
	// resolve it against a wall contact that landed somewhere else inside the merged interval.
	if (SavedDashTimeRemaining > 0.f || Other->SavedDashTimeRemaining > 0.f
		|| SavedSlideTimeRemaining > 0.f || Other->SavedSlideTimeRemaining > 0.f
		|| SavedSlideJumpGraceRemaining > 0.f || Other->SavedSlideJumpGraceRemaining > 0.f
		|| SavedWallJumpControlLockoutRemaining > 0.f || Other->SavedWallJumpControlLockoutRemaining > 0.f
		|| SavedWallJumpInputBufferRemaining > 0.f || Other->SavedWallJumpInputBufferRemaining > 0.f)
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

			// SPEC v8 §7. The wall jump is only predicted if its state round-trips. HandleImpact does
			// re-run on a replay (PhysFalling re-sweeps the same static geometry), so the NORMAL and the
			// WINDOW are partly self-healing — but WallJumpsSinceGround is not: nothing in a replay can
			// re-derive it, so without this capture a replay would keep incrementing the ladder counter
			// and eventually refuse a wall jump the client had already taken.
			SavedWallJumpNormal          = Movement->WallJumpNormal;
			SavedWallJumpWindowRemaining = Movement->WallJumpWindowRemaining;
			SavedWallJumpEntryVelocity   = Movement->WallJumpEntryVelocity;
			SavedWallJumpsSinceGround    = Movement->WallJumpsSinceGround;

			// SPEC v10 §5. NEITHER OF THESE IS SELF-HEALING ON A REPLAY, which is why they are here.
			// HandleImpact re-runs and partly rebuilds the window, but nothing in a replay can
			// re-derive "a jump was pressed 40 ms ago and found no wall" or "this pawn launched off a
			// face 80 ms ago and may not steer back into it" — and both change the velocity the
			// replayed frame produces.
			SavedWallJumpLaunchNormal            = Movement->WallJumpLaunchNormal;
			SavedWallJumpControlLockoutRemaining = Movement->WallJumpControlLockoutRemaining;
			SavedWallJumpInputBufferRemaining    = Movement->WallJumpInputBufferRemaining;

			// SPEC v10 §1. The weapon in hand at the START of this move, which is the profile the
			// move was actually simulated under.
			bSavedKnifeMovementProfile = Movement->bKnifeMovementProfile;
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

			// SPEC v8 §7. Rewind the wall to where it stood before this move ran. Without these a
			// correction landing inside the contact window replays the wall jump as an ordinary refused
			// mid-air jump — DoJump returns false, Velocity is put back, and client and server disagree
			// about the entire redirected launch on the most visible frame of the move.
			Movement->WallJumpNormal          = SavedWallJumpNormal;
			Movement->WallJumpWindowRemaining = SavedWallJumpWindowRemaining;
			Movement->WallJumpEntryVelocity   = SavedWallJumpEntryVelocity;
			Movement->WallJumpsSinceGround    = SavedWallJumpsSinceGround;

			// SPEC v10 §5. Without these a correction landing in the 0.20 s after a wall jump replays
			// the launch and then lets the replayed input drag the pawn straight back at the wall,
			// while the server's copy sailed away — several hundred uu/s of disagreement on the frames
			// immediately after the most visible event in the move. The buffer is worse still: losing
			// it makes the replay refuse a wall jump the server took.
			Movement->WallJumpLaunchNormal            = SavedWallJumpLaunchNormal;
			Movement->WallJumpControlLockoutRemaining = SavedWallJumpControlLockoutRemaining;
			Movement->WallJumpInputBufferRemaining    = SavedWallJumpInputBufferRemaining;

			// SPEC v10 §1. Put the right weapon back in the pawn's hand before the move is replayed:
			// GetMaxSpeed() and all three air ceilings read this bit, so a replay under the wrong one
			// is simulated against a ceiling 481 uu/s away from the server's.
			Movement->bKnifeMovementProfile = bSavedKnifeMovementProfile;
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
