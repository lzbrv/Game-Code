// =================================================================================================
// TraceMovementV18.cpp — spec v18 §1, the three reported movement bugs, MEASURED.
//
// Nothing in this file is shipped gameplay. It is the instrument for §1, and it exists because all
// three complaints are about FEEL and this project's standing rule is that a harness which cannot go
// red is not evidence.
//
//   1a  "reversing in the air does nothing"          -> Trace.Move.V18.AirReverse
//   1b  "velocity appears mid-jump with no input"    -> Trace.Move.V18.AirDrift
//   1c  "input sometimes takes about a second"       -> Trace.Move.V18.InputLatency
//
// WHAT MAKES EACH ONE EVIDENCE RATHER THAN A NUMBER:
//
//   1a runs BOTH ARMS OF ITS OWN A/B IN ONE CALL. Trace.Move.V18.LegacyAirReverse forces the new
//      opposition brake to zero, which is not an approximation of the shipped v17 model — it IS the
//      shipped v17 model, because the brake is the only term v18 §1a adds. So the RED column has to
//      reproduce the user's exact sentence ("your momentum doesn't change at all") and the GREEN
//      column has to slow, and the rows at 90 degrees and inside it have to be EQUAL TO THE LAST
//      FLOAT BIT in both — which is the whole "do not flatten the skill ceiling" requirement,
//      checked as an equality instead of asserted in a comment.
//
//      It drives UTraceCharacterMovementComponent::ComputeAirStrafeStep() — the SHIPPED arithmetic,
//      the same function ApplySourceAirAcceleration() calls on every airborne frame — rather than a
//      re-typed copy of the formula. ComputeDashDirection()/Trace.DashVectorTest set that precedent
//      and the reason is the same: a harness holding its own copy of a rule reports PASS forever
//      after the shipped rule moves.
//
//   1b IS A LEDGER, NOT A TEST. "New movement vectors happen without any player inputs" is a claim
//      that something wrote velocity when nothing asked it to, so the honest instrument records, for
//      every airborne move on which the input acceleration is ZERO, how far the planar velocity
//      moved and which of this component's mid-air velocity writers was live at the time. It reports
//      a cause, not a verdict. The spec's own instruction is "measure velocity per frame with no
//      input held and find what moves it".
//
//   1c INJECTS A REAL KEY THROUGH THE REAL PATH, and through the project's OWN injector rather than
//      a second one. Trace.SimInput -> UGameViewportClient::InputKey (what FSceneViewport::OnKeyDown
//      calls for a physical keystroke) -> UPlayerInput -> the Enhanced Input mapping context ->
//      IA_Move -> ATracePlayerController::OnMoveInput -> ATraceCharacter::DoMove -> Acceleration.
//      Every layer the v17 §6 asset migration touched is inside that chain.
//
//      THE FIRST VERSION OF THIS HARNESS WROTE ITS OWN INJECTOR AND WAS WRONG. It called
//      APlayerController::InputKey with FInputKeyEventArgs::CreateSimulated, and measured the key
//      going DOWN in one frame while the pawn never accelerated at all — on every arm, including the
//      steady-state one. Reported as-is that would have been a spectacular false positive for §1c.
//      It was the harness failing to press the key. The rule this file now follows: use the injector
//      Trace.InputSelfTest already proves moves the pawn, and report "the key never went down" as
//      INVALID rather than as latency.
//
//      It reports the raw key state separately from the acceleration, and that SPLIT is the
//      diagnostic: a key that is down while acceleration is still zero puts the delay unambiguously
//      in Enhanced Input rather than in the window, the OS or the pawn.
//
// The namespace is named after the file, not anonymous: two anonymous namespaces with a LocalPawn()
// in them are one namespace with two definitions under the Windows unity build (MSVC C2084), and
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
// =================================================================================================

#include "CoreMinimal.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"

#include "EnhancedInputSubsystems.h"

#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Settings/TraceUserSettings.h"
#include "Trace.h"
#include "TraceSettings.h"

#if !UE_BUILD_SHIPPING

// The A/B arm for 1a. Defined at the top of TraceCharacterMovementComponent.cpp (outside the dev
// block, because GetAirStrafeOpposingDeceleration() reads it and that is shipping code). Written
// directly rather than through the console variable so the two arms can be run inside ONE command
// without a frame of console round-trip between them; it is always put back.
extern int32 GTraceV18LegacyAirReverse;

// =================================================================================================
// The per-move ledger for 1b. A member of the movement component (it reads protected state), defined
// here next to the command that arms and reads it.
// =================================================================================================

namespace TraceMovementV18
{
	/** Bits recorded alongside every drift sample, so the report can NAME a writer. */
	enum EDriftFlags : int32
	{
		Drift_None            = 0,
		Drift_WallWindowOpen  = 1 << 0,   // a wall face is latched: a press (or a buffered one) could launch
		Drift_BufferArmed     = 1 << 1,   // a refused mid-air press is remembered — v10 §5 CAUSE 2
		Drift_WallLaunched    = 1 << 2,   // a wall jump actually fired ON THIS MOVE
		Drift_DashExited      = 1 << 3,   // the dash window closed on this move (ApplyDashExitSpeed ran)
		Drift_SlideExited     = 1 << 4,   // a slide ended on this move (EndSlide rewrote Velocity)
		Drift_GroundGrace     = 1 << 5,   // airborne but still "grounded for abilities" (ledge grace)
		Drift_DashActive      = 1 << 6,   // mid-dash: the dash owns Velocity outright
		Drift_SlideJumpGrace  = 1 << 7,   // inside the slide-jump coyote window
	};

	FString DescribeDriftFlags(const int32 Flags)
	{
		if (Flags == Drift_None)
		{
			return TEXT("<nothing live>");
		}

		TArray<FString> Parts;
		if (Flags & Drift_WallLaunched)   { Parts.Add(TEXT("WALL-JUMP FIRED")); }
		if (Flags & Drift_DashExited)     { Parts.Add(TEXT("dash-exit clamp")); }
		if (Flags & Drift_SlideExited)    { Parts.Add(TEXT("slide-exit rewrite")); }
		if (Flags & Drift_BufferArmed)    { Parts.Add(TEXT("buffered jump press armed")); }
		if (Flags & Drift_WallWindowOpen) { Parts.Add(TEXT("wall window open")); }
		if (Flags & Drift_GroundGrace)    { Parts.Add(TEXT("ledge grace")); }
		if (Flags & Drift_DashActive)     { Parts.Add(TEXT("dashing")); }
		if (Flags & Drift_SlideJumpGrace) { Parts.Add(TEXT("slide-jump grace")); }
		return FString::Join(Parts, TEXT(" + "));
	}

	/** The one world this harness should drive. Same shape every harness in this project uses. */
	UWorld* LocalGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetFirstPlayerController() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	ATraceCharacter* LocalPawn()
	{
		const UWorld* World = LocalGameWorld();
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		return (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	}

	/**
	 * The component the 1a table is computed against.
	 *
	 * The local pawn when there is one, so a run inside a live match reads the knife profile and any
	 * live retune; the CDO otherwise, so the table can still be printed from the title screen. It is
	 * a PURE function of config either way — ComputeAirStrafeStep() reads no per-pawn state at all —
	 * so the two agree, and the report says which one it used rather than leaving it to be guessed.
	 */
	UTraceCharacterMovementComponent* AirModelSource(bool& bOutFromLivePawn)
	{
		if (const ATraceCharacter* Pawn = LocalPawn())
		{
			if (UTraceCharacterMovementComponent* Move = Pawn->GetTraceMovement())
			{
				bOutFromLivePawn = true;
				return Move;
			}
		}
		bOutFromLivePawn = false;
		return GetMutableDefault<UTraceCharacterMovementComponent>();
	}
}

void UTraceCharacterMovementComponent::ArmAirDriftMeter(const float Seconds)
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	AirDriftUntilTime = static_cast<float>(World->GetTimeSeconds()) + FMath::Max(0.5f, Seconds);
	AirDriftSamples = 0;
	AirDriftMovedSamples = 0;
	AirDriftWorstStep = 0.f;
	AirDriftTotalStep = 0.f;
	AirDriftWorstFlags = 0;
	AirDriftWorstBefore = 0.f;
	AirDriftWorstAfter = 0.f;
	AirDriftWallLaunches = 0;
	AirDriftDashExits = 0;
	AirDriftSlideExits = 0;
	AirDriftUnexplained = 0;
	AirDriftCorrectionsAtArm = GetCorrectionCountForAudit();
	AirDriftBufferedLaunchesAtArm = WallJumpBufferedLaunches;
	AirDriftPrevWallJumps = WallJumpsSinceGround;
	AirDriftPrevDashTime = DashTimeRemaining;
	AirDriftPrevSlideTime = SlideTimeRemaining;

	UE_LOG(LogTraceGame, Display,
		TEXT("V18DRIFT armed on %s for %.1f s (netMode=%d role=%d). Recording every AIRBORNE move whose "
		     "input acceleration is exactly zero — jump, then let go of everything."),
		*GetNameSafe(CharacterOwner), FMath::Max(0.5f, Seconds),
		static_cast<int32>(GetNetMode()),
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1);
}

void UTraceCharacterMovementComponent::TickAirDriftMeter(const float DeltaSeconds, const FVector& OldVelocity)
{
	using namespace TraceMovementV18;

	if (AirDriftUntilTime < 0.f)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		AirDriftUntilTime = -1.f;
		return;
	}

	// TRANSITIONS FIRST, AND UNCONDITIONALLY. OnMovementUpdated has already advanced every clock by
	// the time this runs, so "the dash ended" and "a wall jump fired" only exist as a difference
	// against the previous move — and a difference that is only sampled on qualifying moves would
	// silently skip the event whenever the frame before it did not qualify.
	const bool bWallLaunchedThisMove = (WallJumpsSinceGround > AirDriftPrevWallJumps);
	const bool bDashExitedThisMove = (AirDriftPrevDashTime > 0.f && DashTimeRemaining <= 0.f);
	const bool bSlideExitedThisMove = (AirDriftPrevSlideTime > 0.f && SlideTimeRemaining <= 0.f);
	AirDriftPrevWallJumps = WallJumpsSinceGround;
	AirDriftPrevDashTime = DashTimeRemaining;
	AirDriftPrevSlideTime = SlideTimeRemaining;

	if (static_cast<float>(World->GetTimeSeconds()) > AirDriftUntilTime)
	{
		AirDriftUntilTime = -1.f;

		const int32 Corrections = GetCorrectionCountForAudit() - AirDriftCorrectionsAtArm;

		UE_LOG(LogTraceGame, Display,
			TEXT("V18DRIFT ===== spec v18 §1b: mid-air velocity change with NO input (%s) ====="),
			(GetNetMode() == NM_Client)
				? TEXT("CLIENT — a prediction shove would be visible here")
				: TEXT("HOST/STANDALONE — a prediction shove is invisible here by construction"));
		UE_LOG(LogTraceGame, Display,
			TEXT("V18DRIFT samples: %d airborne moves with zero input; %d of them moved the planar "
			     "velocity at all"),
			AirDriftSamples, AirDriftMovedSamples);

		if (AirDriftSamples == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("V18DRIFT INVALID — the pawn was never airborne with the stick released during the "
				     "window. Re-arm and jump without holding a direction; a ledger of zero samples is "
				     "not a clean bill of health."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("V18DRIFT worst single move: %.2f uu/s (planar %.0f -> %.0f), live at the time: %s"),
			AirDriftWorstStep, AirDriftWorstBefore, AirDriftWorstAfter,
			*DescribeDriftFlags(AirDriftWorstFlags));
		UE_LOG(LogTraceGame, Display,
			TEXT("V18DRIFT total drift %.1f uu/s over %d moves (mean %.3f uu/s per move)"),
			AirDriftTotalStep, AirDriftSamples,
			AirDriftTotalStep / static_cast<float>(FMath::Max(1, AirDriftSamples)));
		UE_LOG(LogTraceGame, Display,
			TEXT("V18DRIFT attributed: wall-jump launches on a hands-off move=%d (of which BUFFERED, i.e. "
			     "no press on that frame at all=%d) | dash-exit clamps=%d | slide-exit rewrites=%d | "
			     "server corrections=%d"),
			AirDriftWallLaunches,
			WallJumpBufferedLaunches - AirDriftBufferedLaunchesAtArm,
			AirDriftDashExits, AirDriftSlideExits, Corrections);
		// THE SPEC'S FOUR SUSPECTS, EACH WITH ITS OWN COUNTER. Printed together so a reader can see
		// which ones the run refutes rather than only which one it convicts.
		UE_LOG(LogTraceGame, Display,
			TEXT("V18DRIFT the four §1b suspects, as measured over %d hands-off airborne moves: "
			     "[1] air-strafe falloff/asymptote drifting on its own = %d (nothing else live); "
			     "[2] slide-jump bonus applying late = %d; "
			     "[3] wall-jump / ledge impulse off geometry = %d; "
			     "[4] client prediction correction = %d"),
			AirDriftSamples, AirDriftUnexplained, AirDriftSlideExits,
			AirDriftMovedSamples - AirDriftUnexplained, Corrections);

		// The verdict is deliberately about the WORST move and not the mean: the complaint is a
		// perceptible shove, and a shove is a single frame. A tenth of a uu/s of float noise per frame
		// is not what "new movement vectors" means.
		if (AirDriftWorstStep <= 1.f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("V18DRIFT VERDICT: NO unexplained mid-air planar change was produced in this "
				     "window. The worst move moved the vector by %.2f uu/s, which is float noise."),
				AirDriftWorstStep);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("V18DRIFT VERDICT: REPRODUCED — %.2f uu/s of planar velocity appeared on a single "
				     "move with the stick released. Cause: %s"),
				AirDriftWorstStep, *DescribeDriftFlags(AirDriftWorstFlags));
		}
		return;
	}

	// --- Only airborne moves with genuinely no input count ---------------------------------------
	//
	// MOVE_None is excluded with the same care every other ground test in this component uses: a dead
	// or movement-disabled pawn is not "in the air with no input", it is not simulating.
	if (MovementMode == MOVE_None || !IsFalling())
	{
		return;
	}

	// The wish vector the air model itself reads, tested the way the air model tests it. Anything the
	// player is asking for disqualifies the sample — the question is what moves velocity when NOTHING
	// is asking.
	if (FVector(Acceleration.X, Acceleration.Y, 0.f).SizeSquared() > UE_KINDA_SMALL_NUMBER)
	{
		return;
	}

	const FVector BeforePlanar(OldVelocity.X, OldVelocity.Y, 0.f);
	const FVector AfterPlanar(Velocity.X, Velocity.Y, 0.f);
	const float Step = static_cast<float>((AfterPlanar - BeforePlanar).Size());

	int32 Flags = Drift_None;
	if (WallJumpWindowRemaining > 0.f)          { Flags |= Drift_WallWindowOpen; }
	if (WallJumpInputBufferRemaining > 0.f)     { Flags |= Drift_BufferArmed; }
	if (bWallLaunchedThisMove)                  { Flags |= Drift_WallLaunched; }
	if (bDashExitedThisMove)                    { Flags |= Drift_DashExited; }
	if (bSlideExitedThisMove)                   { Flags |= Drift_SlideExited; }
	if (GroundGraceRemaining > 0.f)             { Flags |= Drift_GroundGrace; }
	if (DashTimeRemaining > 0.f)                { Flags |= Drift_DashActive; }
	if (SlideJumpGraceRemaining > 0.f)          { Flags |= Drift_SlideJumpGrace; }

	++AirDriftSamples;
	AirDriftTotalStep += Step;
	if (Step > 0.01f)
	{
		++AirDriftMovedSamples;
		if (Flags == Drift_None)
		{
			++AirDriftUnexplained;
		}
	}
	if (bWallLaunchedThisMove) { ++AirDriftWallLaunches; }
	if (bDashExitedThisMove)   { ++AirDriftDashExits; }
	if (bSlideExitedThisMove)  { ++AirDriftSlideExits; }

	if (Step > AirDriftWorstStep)
	{
		AirDriftWorstStep = Step;
		AirDriftWorstFlags = Flags;
		AirDriftWorstBefore = static_cast<float>(BeforePlanar.Size());
		AirDriftWorstAfter = static_cast<float>(AfterPlanar.Size());
	}

	// A per-frame line for the moves that actually moved something, so a reader can see the shape of
	// the event rather than only its summary. Bounded by the arming window, and only on moves that
	// already qualified as "airborne, no input", so it cannot flood a log.
	if (Step > 1.f)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("V18DRIFT  t=%7.3f dt=%.4f planar %7.1f -> %7.1f (step %6.2f uu/s) velZ=%7.1f | %s"),
			static_cast<float>(World->GetTimeSeconds()), DeltaSeconds,
			static_cast<float>(BeforePlanar.Size()), static_cast<float>(AfterPlanar.Size()), Step,
			static_cast<float>(Velocity.Z), *DescribeDriftFlags(Flags));
	}
}

namespace TraceMovementV18
{
	// =============================================================================================
	// 1a — Trace.Move.V18.AirReverse
	// =============================================================================================

	/** One angle's worth of the table, in one arm. */
	struct FAngleSample
	{
		float Angle = 0.f;
		float SpeedDelta = 0.f;      // uu/s added (+) or removed (-) by ONE sub-step
		float TurnDegrees = 0.f;     // how far the vector rotated in that sub-step
	};

	FAngleSample SampleOneStep(const UTraceCharacterMovementComponent& Move, const float EntrySpeed,
	                           const float AngleDegrees, const float StepSeconds)
	{
		// Travel along +X; the wish direction sits AngleDegrees off it. 0 = straight ahead,
		// 90 = the pure Source strafe, 180 = the dead reversal the user described.
		const FVector Entry(EntrySpeed, 0.f, 0.f);
		const FVector Wish = FRotator(0.f, AngleDegrees, 0.f).Vector();

		const FVector Result = Move.ComputeAirStrafeStep(Entry, Wish, 1.f, StepSeconds);

		FAngleSample Out;
		Out.Angle = AngleDegrees;
		Out.SpeedDelta = static_cast<float>(Result.Size()) - EntrySpeed;

		const FVector EntryDir = Entry.GetSafeNormal();
		const FVector ResultDir = Result.GetSafeNormal();
		if (!EntryDir.IsNearlyZero() && !ResultDir.IsNearlyZero())
		{
			Out.TurnDegrees = FMath::RadiansToDegrees(FMath::Acos(
				FMath::Clamp(static_cast<float>(FVector::DotProduct(EntryDir, ResultDir)), -1.f, 1.f)));
		}
		return Out;
	}

	/**
	 * Hold one wish angle for @p Seconds and report the speed it settles at.
	 *
	 * This is the row that answers "did the skill ceiling move": a sustained 85-90 degree strafe is
	 * how a player builds speed, and it has to arrive at the identical number in both arms.
	 */
	float SimulateSustained(const UTraceCharacterMovementComponent& Move, const float EntrySpeed,
	                        const float AngleDegrees, const float Seconds, const float StepSeconds,
	                        const bool bTrackWish)
	{
		FVector Planar(EntrySpeed, 0.f, 0.f);
		const int32 Steps = FMath::Max(1, FMath::RoundToInt(Seconds / StepSeconds));

		for (int32 Step = 0; Step < Steps; ++Step)
		{
			// bTrackWish models a real air strafe: the player keeps the input at a fixed angle to
			// where they are NOW travelling, which is what turning the mouse while holding A does.
			// Without it the wish direction is world-locked and the "strafe" decays into a
			// straight-ahead push after a few frames, which measures nothing.
			const FVector Reference = bTrackWish ? Planar.GetSafeNormal() : FVector::ForwardVector;
			if (Reference.IsNearlyZero())
			{
				break;
			}
			const FVector Wish = FRotator(0.f, AngleDegrees, 0.f).RotateVector(Reference);
			Planar = Move.ComputeAirStrafeStep(Planar, Wish, 1.f, StepSeconds);
		}
		return static_cast<float>(Planar.Size());
	}

	void RunAirReverse()
	{
		bool bFromLivePawn = false;
		UTraceCharacterMovementComponent* Move = AirModelSource(bFromLivePawn);
		if (Move == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("V18AIR: no movement component to read the air model from."));
			return;
		}

		// 60 Hz. Fixed rather than sampled from the live frame so the two arms are compared over the
		// identical arithmetic and the table is reproducible between runs.
		const float StepSeconds = 1.f / 60.f;
		const float EntrySpeed = 1000.f;

		const int32 SavedArm = GTraceV18LegacyAirReverse;

		// RED first, deliberately: the point of the exercise is to show the failure before the fix,
		// and printing GREEN first invites reading the table as a confirmation.
		GTraceV18LegacyAirReverse = 1;
		const float RedDecel = Move->GetAirStrafeOpposingDecelerationForAudit();

		const float Angles[] = { 0.f, 30.f, 60.f, 85.f, 90.f, 95.f, 120.f, 150.f, 180.f };
		const int32 AngleCount = static_cast<int32>(UE_ARRAY_COUNT(Angles));

		TArray<FAngleSample> Red;
		Red.Reserve(AngleCount);
		for (const float Angle : Angles)
		{
			Red.Add(SampleOneStep(*Move, EntrySpeed, Angle, StepSeconds));
		}
		const float RedSustained90 = SimulateSustained(*Move, 700.f, 90.f, 1.0f, StepSeconds, true);
		const float RedSustained85 = SimulateSustained(*Move, 700.f, 85.f, 1.0f, StepSeconds, true);
		const float RedReversal = SimulateSustained(*Move, EntrySpeed, 180.f, 0.5f, StepSeconds, false);

		GTraceV18LegacyAirReverse = 0;
		const float GreenDecel = Move->GetAirStrafeOpposingDecelerationForAudit();

		TArray<FAngleSample> Green;
		Green.Reserve(AngleCount);
		for (const float Angle : Angles)
		{
			Green.Add(SampleOneStep(*Move, EntrySpeed, Angle, StepSeconds));
		}
		const float GreenSustained90 = SimulateSustained(*Move, 700.f, 90.f, 1.0f, StepSeconds, true);
		const float GreenSustained85 = SimulateSustained(*Move, 700.f, 85.f, 1.0f, StepSeconds, true);
		const float GreenReversal = SimulateSustained(*Move, EntrySpeed, 180.f, 0.5f, StepSeconds, false);

		GTraceV18LegacyAirReverse = SavedArm;

		// --- the report ---------------------------------------------------------------------------

		UE_LOG(LogTraceGame, Display,
			TEXT("V18AIR ===== spec v18 §1a: reversing in the air (%s) ====="),
			bFromLivePawn ? TEXT("driven off the LIVE local pawn") : TEXT("driven off the CDO — no pawn yet"));
		UE_LOG(LogTraceGame, Display,
			TEXT("V18AIR config: entry=%.0f uu/s along +X, dt=%.5f s, opposingDecel RED=%.0f GREEN=%.0f uu/s^2. "
			     "RED is Trace.Move.V18.LegacyAirReverse 1, which IS the shipped v17 air model."),
			EntrySpeed, StepSeconds, RedDecel, GreenDecel);

		if (RedDecel != 0.f || GreenDecel <= 0.f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("V18AIR INVALID — the arms did not stage (RED decel %.0f must be 0, GREEN decel %.0f "
				     "must be > 0). Check Trace.Move.AirOpposingDecel; a table whose two arms are the same "
				     "arm proves nothing."),
				RedDecel, GreenDecel);
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("V18AIR  wish angle |  RED dv/step |  GREEN dv/step |  turn  | verdict"));

		int32 Failures = 0;
		for (int32 Index = 0; Index < AngleCount; ++Index)
		{
			const FAngleSample& R = Red[Index];
			const FAngleSample& G = Green[Index];

			FString Verdict;
			if (R.Angle <= 90.f)
			{
				// THE SKILL-CEILING CHECK, AS AN EQUALITY. Opposition is max(0, -dot) and the dot is
				// >= 0 for every one of these angles, so the brake term is not merely small here, it is
				// arithmetically absent — the two arms must land on the SAME FLOAT. A tolerance would
				// hide exactly the regression this row exists to catch.
				const bool bIdentical = (R.SpeedDelta == G.SpeedDelta) && (R.TurnDegrees == G.TurnDegrees);
				Verdict = bIdentical
					? FString(TEXT("PASS  gain path bit-identical — the air strafe is untouched"))
					: FString::Printf(TEXT("FAIL  the strafe changed by %.6f uu/s"), G.SpeedDelta - R.SpeedDelta);
				if (!bIdentical)
				{
					++Failures;
				}
			}
			else if (R.Angle >= 179.f)
			{
				const bool bRedUnchanged = FMath::Abs(R.SpeedDelta) <= 0.01f;
				const bool bGreenSlows = G.SpeedDelta < -1.f;
				Verdict = FString::Printf(
					TEXT("%s  RED %s / GREEN %s"),
					(bRedUnchanged && bGreenSlows) ? TEXT("PASS") : TEXT("FAIL"),
					bRedUnchanged ? TEXT("momentum UNCHANGED (the reported bug)") : TEXT("<did not reproduce>"),
					bGreenSlows ? TEXT("slows") : TEXT("<did not slow>"));
				if (!bRedUnchanged || !bGreenSlows)
				{
					++Failures;
				}
			}
			else
			{
				// The ramp. Each opposed angle must bite, and must bite LESS than the one past it —
				// "a slight counter-steer barely bites and a full reversal bites properly" is a
				// monotonicity claim, so it is checked as one.
				const bool bBites = G.SpeedDelta < R.SpeedDelta;
				const bool bMonotonic = (Index + 1 >= AngleCount) || (Green[Index + 1].SpeedDelta <= G.SpeedDelta);
				Verdict = FString::Printf(TEXT("%s  counter-steer bites %.1f%% of the full reversal"),
					(bBites && bMonotonic) ? TEXT("PASS") : TEXT("FAIL"),
					(Green[AngleCount - 1].SpeedDelta < 0.f)
						? 100.f * G.SpeedDelta / Green[AngleCount - 1].SpeedDelta
						: 0.f);
				if (!bBites || !bMonotonic)
				{
					++Failures;
				}
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("V18AIR    %6.1f deg | %+11.4f  | %+13.4f  | %5.2f  | %s"),
				R.Angle, R.SpeedDelta, G.SpeedDelta, G.TurnDegrees, *Verdict);
		}

		// --- sustained: the two rows that decide whether the mechanic still exists -----------------

		const bool bStrafe90Same = (RedSustained90 == GreenSustained90);
		const bool bStrafe85Same = (RedSustained85 == GreenSustained85);
		if (!bStrafe90Same || !bStrafe85Same) { ++Failures; }

		UE_LOG(LogTraceGame, Display,
			TEXT("V18AIR  SUSTAINED 90 deg strafe, 700 uu/s for 1.00 s: RED %.4f -> GREEN %.4f | %s"),
			RedSustained90, GreenSustained90,
			bStrafe90Same ? TEXT("PASS  identical — accumulation untouched") : TEXT("FAIL  the ceiling moved"));
		UE_LOG(LogTraceGame, Display,
			TEXT("V18AIR  SUSTAINED 85 deg strafe, 700 uu/s for 1.00 s: RED %.4f -> GREEN %.4f | %s"),
			RedSustained85, GreenSustained85,
			bStrafe85Same ? TEXT("PASS  identical — turning INTO travel still gains exactly as much")
			              : TEXT("FAIL  the gain changed"));

		const bool bReversalWorks = (FMath::Abs(RedReversal - EntrySpeed) <= 0.5f) && (GreenReversal < EntrySpeed - 100.f);
		if (!bReversalWorks) { ++Failures; }
		UE_LOG(LogTraceGame, Display,
			TEXT("V18AIR  SUSTAINED 180 deg reversal, %.0f uu/s held 0.50 s: RED %.1f uu/s -> GREEN %.1f uu/s | %s"),
			EntrySpeed, RedReversal, GreenReversal,
			bReversalWorks
				? TEXT("PASS  RED keeps every unit (the bug), GREEN sheds it")
				: TEXT("FAIL"));

		UE_LOG(LogTraceGame, Display,
			TEXT("V18AIR  derived: a dead 180 reversal sheds %.0f uu/s per second, so 1000 uu/s takes "
			     "%.2f s to kill. AirAcceleration is %.0f uu/s^2 for comparison — the brake is %.0f%% of it."),
			GreenDecel, 1000.f / FMath::Max(1.f, GreenDecel),
			UTraceSettings::Get().AirAcceleration,
			100.f * GreenDecel / FMath::Max(1.f, UTraceSettings::Get().AirAcceleration));

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display, TEXT("V18AIR ===== PASS: 0 failures ====="));
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("V18AIR ===== FAIL: %d row(s) ====="), Failures);
		}
	}

	// =============================================================================================
	// 1b — Trace.Move.V18.AirDrift
	// =============================================================================================

	void RunAirDrift(const TArray<FString>& Args)
	{
		float Seconds = 12.f;
		if (Args.Num() > 0)
		{
			Seconds = FCString::Atof(*Args[0]);
		}

		ATraceCharacter* Pawn = LocalPawn();
		UTraceCharacterMovementComponent* Move = (Pawn != nullptr) ? Pawn->GetTraceMovement() : nullptr;
		if (Move == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("V18DRIFT: no local pawn. This one needs a live match — it measures what happens to a "
				     "real capsule in real geometry, which is half the question."));
			return;
		}

		Move->ArmAirDriftMeter(Seconds);
	}

	/**
	 * The self-driving arm of the drift ledger.
	 *
	 * IT MUST BUILD FORWARD MOMENTUM FIRST, and the first run of this harness is why that sentence is
	 * here. Jumping straight up from a standstill produced 2061 airborne zero-input moves and a worst
	 * planar change of 0.00 uu/s — a perfect score against a condition the user never described. The
	 * report is "mid jump WITH SOME FORWARD MOMENTUM ALREADY", so the cycle is: run to full speed,
	 * jump, then release absolutely everything and watch.
	 *
	 * @param 0  seconds to run for (default 20)
	 * @param 1  1 to also press jump ONCE in mid-air on each cycle (default 0). That press finds no
	 *           wall, is refused, and arms the spec v10 §5 CAUSE 2 buffer — which is the one thing in
	 *           this component that can launch a pawn with no press on the frame it launches. Having
	 *           it as an argument makes the two populations comparable in one build.
	 *
	 * Deliberately separate from RunAirDrift: a human tuning by feel wants to arm the ledger and then
	 * play, and a harness that also drove their pawn would be measuring the harness.
	 */
	struct FDriftDriveState
	{
		double Deadline = 0.0;
		float Seconds = 0.f;
		float Elapsed = 0.f;
		float PhaseTime = 0.f;
		int32 Phase = 0;
		int32 Cycles = 0;
		bool bMidAirJump = false;
		bool bMidAirJumpDone = false;
		FVector RunDirection = FVector::ForwardVector;
		TWeakObjectPtr<ATraceCharacter> Pawn;
	};

	void RunAirDriftDrive(const TArray<FString>& Args)
	{
		float Seconds = 20.f;
		bool bMidAirJump = false;
		if (Args.Num() > 0) { Seconds = FCString::Atof(*Args[0]); }
		if (Args.Num() > 1) { bMidAirJump = (FCString::Atoi(*Args[1]) != 0); }

		ATraceCharacter* Pawn = LocalPawn();
		UTraceCharacterMovementComponent* Move = (Pawn != nullptr) ? Pawn->GetTraceMovement() : nullptr;
		if (Move == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("V18DRIFT drive: no local pawn."));
			return;
		}

		Move->ArmAirDriftMeter(Seconds);

		TSharedPtr<FDriftDriveState> State = MakeShared<FDriftDriveState>();
		State->Seconds = Seconds;
		State->Deadline = FPlatformTime::Seconds() + Seconds + 5.0;
		State->Pawn = Pawn;
		State->bMidAirJump = bMidAirJump;
		State->RunDirection = Pawn->GetActorForwardVector().GetSafeNormal2D();
		if (State->RunDirection.IsNearlyZero())
		{
			State->RunDirection = FVector::ForwardVector;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("V18DRIFT drive: run to full speed along %s, jump, then RELEASE EVERYTHING and watch. "
			     "%.1f s, midAirJump=%d (a refused mid-air press arms the v10 §5 buffer)."),
			*State->RunDirection.ToCompactString(), Seconds, bMidAirJump ? 1 : 0);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float Delta) -> bool
		{
			ATraceCharacter* DrivePawn = State->Pawn.Get();
			UTraceCharacterMovementComponent* DriveMove =
				(DrivePawn != nullptr) ? DrivePawn->GetTraceMovement() : nullptr;
			if (DrivePawn == nullptr || DriveMove == nullptr || FPlatformTime::Seconds() > State->Deadline)
			{
				return false;
			}

			State->Elapsed += Delta;
			State->PhaseTime += Delta;
			if (State->Elapsed > State->Seconds)
			{
				DrivePawn->StopJumping();
				UE_LOG(LogTraceGame, Display, TEXT("V18DRIFT drive: %d run-jump cycles completed."),
					State->Cycles);
				return false;
			}

			switch (State->Phase)
			{
			case 0:
				// RUN. Through AddMovementInput, the same entry point ATraceCharacter::DoMove uses, so
				// Acceleration is set exactly as a held key would set it.
				DrivePawn->AddMovementInput(State->RunDirection, 1.f);
				if (State->PhaseTime > 1.2f)
				{
					State->Phase = 1;
					State->PhaseTime = 0.f;
				}
				return true;

			case 1:
				// JUMP, through the real predicted entry point. A harness that wrote Velocity.Z itself
				// would skip DoJump, and DoJump is one of the suspects.
				DrivePawn->Jump();
				State->Phase = 2;
				State->PhaseTime = 0.f;
				State->bMidAirJumpDone = false;
				++State->Cycles;
				return true;

			case 2:
				// AIRBORNE, HANDS OFF. Nothing is pressed and nothing is added — every sample the ledger
				// takes from here is a velocity change nobody asked for.
				DrivePawn->StopJumping();

				if (State->bMidAirJump && !State->bMidAirJumpDone && State->PhaseTime > 0.30f)
				{
					State->bMidAirJumpDone = true;
					DrivePawn->Jump();
				}

				// Back to running once the pawn is down again (or if it never left, so a cycle blocked
				// against geometry cannot stall the whole session).
				if ((State->PhaseTime > 0.25f && DriveMove->IsMovingOnGround()) || State->PhaseTime > 3.0f)
				{
					State->Phase = 0;
					State->PhaseTime = 0.f;

					// Turn a little each cycle so the session samples several bits of the arena rather
					// than one clear lane. A drift bug that only fires near geometry is still a drift
					// bug, and a harness that never approaches geometry cannot see it.
					State->RunDirection = FRotator(0.f, 37.f, 0.f).RotateVector(State->RunDirection);
				}
				return true;

			default:
				return false;
			}
		}));
	}

	// =============================================================================================
	// 1c — Trace.Move.V18.InputLatency
	//
	// Presses a real key and counts the frames until the movement simulation reacts. Three arms, run
	// back to back off one command so they share a process, a frame rate and a pawn:
	//
	//   COLD   the first press this harness makes. If the mapping context is not live yet, or is being
	//          rebuilt, this is where it shows.
	//   WARM   two more presses a second apart, i.e. the steady-state cost of a press.
	//   REBUILT a press issued on the frame AFTER RequestRebuildControlMappings(), which is the tail of
	//          ATracePlayerController::ApplyControlSettings and therefore what every spawn does.
	//
	// The raw key state is sampled independently of the acceleration, and that split is the whole
	// diagnostic value: PlayerInput's key map is filled by InputKey with no reference to Enhanced
	// Input at all, so a key that reads DOWN while acceleration is still zero puts the missing time
	// unambiguously in the mapping/binding layer rather than in the window, the OS or the pawn.
	// =============================================================================================

	struct FLatencyState
	{
		int32 Phase = 0;
		int32 Press = 0;
		float PhaseTime = 0.f;
		double Deadline = 0.0;

		TWeakObjectPtr<ATraceCharacter> Pawn;
		FKey Key;

		double PressWallTime = 0.0;
		uint64 PressFrame = 0;
		int32 FramesToKeyDown = -1;
		int32 FramesToAccel = -1;

		float ResultsMs[3] = { -1.f, -1.f, -1.f };
		int32 ResultsFrames[3] = { -1, -1, -1 };
		int32 ResultsKeyFrames[3] = { -1, -1, -1 };

		/** Whether Trace.SimInput accepted the injection at all. See HoldKeyThroughViewport(). */
		bool ResultsAccepted[3] = { false, false, false };

		/**
		 * Whether a menu owned gameplay input while this press was in flight.
		 *
		 * NOT A LATENCY RESULT, AND THAT DISTINCTION COST A WHOLE RUN. The first pass of this harness
		 * fired at 6 s into the match, when spec v14 §3's character-select screen is up and
		 * ATracePlayerController::SetGameInputSuppressed(true) is in force — so OnMoveInput returns
		 * before it ever reaches the pawn, by design. The harness dutifully reported "the pawn NEVER
		 * accelerated" on all three arms, which is true, meaningless, and would have been filed as a
		 * one-second input bug.
		 */
		bool ResultsSuppressed[3] = { false, false, false };

		/** Seconds the harness spent waiting for a menu to hand input back before the first press. */
		float WaitedForMenuSeconds = 0.f;
	};

	const TCHAR* PressLabel(const int32 Index)
	{
		switch (Index)
		{
			case 0:  return TEXT("COLD    (first press of the session)");
			case 1:  return TEXT("WARM    (steady state)");
			default: return TEXT("REBUILT (press on the frame after RequestRebuildControlMappings)");
		}
	}

	APlayerController* LatencyController()
	{
		const UWorld* World = LocalGameWorld();
		return (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
	}

	/**
	 * Press @p Key for @p HoldSeconds THROUGH THE PROJECT'S OWN INJECTOR, and nothing else.
	 *
	 * Trace.SimInput (Debug/TraceInputHarness.cpp) drives UGameViewportClient::InputKey — exactly what
	 * FSceneViewport::OnKeyDown calls once Slate has routed a real keystroke — and it resolves the
	 * input device by asking which id actually maps to a local player. This harness's first attempt
	 * did neither: it called APlayerController::InputKey with FInputKeyEventArgs::CreateSimulated, and
	 * the measured result was UPlayerInput registering the key as DOWN after 1 frame while the pawn
	 * never accelerated at all — on ALL THREE arms including the steady-state one.
	 *
	 * THAT WOULD HAVE BEEN REPORTED AS A ONE-SECOND INPUT BUG. It was the harness failing to press the
	 * key, which is the exact failure mode this project's testing rules exist to prevent. Reusing the
	 * shipped injector rather than writing a second one is the fix and the lesson: Trace.InputSelfTest
	 * already proves that path moves and fires the pawn, so a run that stalls on it is the game.
	 */
	bool HoldKeyThroughViewport(const FKey& Key, const float HoldSeconds)
	{
		UWorld* World = LocalGameWorld();
		if (World == nullptr || GEngine == nullptr || !Key.IsValid())
		{
			return false;
		}
		return GEngine->Exec(World,
			*FString::Printf(TEXT("Trace.SimInput %s %.2f"), *Key.ToString(), HoldSeconds));
	}

	void ReportLatency(const FLatencyState& State)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("V18INPUT ===== spec v18 §1c: key-down -> the pawn actually accelerating ====="));
		UE_LOG(LogTraceGame, Display,
			TEXT("V18INPUT key=%s injected through Trace.SimInput, i.e. UGameViewportClient::InputKey — the "
			     "entry point FSceneViewport::OnKeyDown calls for a real keystroke. Measured to the first "
			     "frame the movement component's Acceleration is non-zero."),
			*State.Key.ToString());

		UE_LOG(LogTraceGame, Display,
			TEXT("V18INPUT the harness waited %.2f s at the start for a menu to hand gameplay input back "
			     "(spec v14 §3's character-select screen suppresses it while it is up). A press made "
			     "under that is a measurement of the menu, not of the input path."),
			State.WaitedForMenuSeconds);

		int32 Failures = 0;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			if (State.ResultsSuppressed[Index])
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("V18INPUT %-64s | INVALID — a menu owned gameplay input for this press. "
					     "Not a latency number."),
					PressLabel(Index));
				++Failures;
				continue;
			}

			if (State.ResultsFrames[Index] < 0)
			{
				// THE TWO FAILURES ARE NOT THE SAME FAILURE, and saying so is the point. A key that
				// UPlayerInput never even registered as DOWN is the harness failing to inject, not the
				// game failing to respond, and reporting the first as the second would be exactly the
				// "harness manufactures the defect it is measuring" trap this project keeps hitting.
				if (State.ResultsKeyFrames[Index] < 0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("V18INPUT %-64s | INVALID — the injected key never even read as DOWN "
						     "(Trace.SimInput accepted=%d). The harness could not press the key; this says "
						     "NOTHING about the game's input latency."),
						PressLabel(Index), State.ResultsAccepted[Index] ? 1 : 0);
				}
				else
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("V18INPUT %-64s | REPRODUCED (worst case) — the key read as DOWN after %d "
						     "frame(s) and the pawn NEVER accelerated inside the timeout. The time is "
						     "being spent between UPlayerInput and the pawn, i.e. inside Enhanced Input."),
						PressLabel(Index), State.ResultsKeyFrames[Index]);
				}
				++Failures;
				continue;
			}

			// A SECOND IS THE COMPLAINT. Anything at or over half of it is the reported bug; three
			// frames is the honest floor for "press, next PerformMovement, next tick observes it".
			const bool bSlow = State.ResultsMs[Index] >= 500.f;
			if (bSlow)
			{
				++Failures;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("V18INPUT %-64s | key down after %d frame(s) | pawn accelerating after %d frame(s) / "
				     "%.1f ms | %s"),
				PressLabel(Index), State.ResultsKeyFrames[Index], State.ResultsFrames[Index],
				State.ResultsMs[Index],
				bSlow ? TEXT("REPRODUCED — this is the '~1 second' complaint") : TEXT("within one or two frames"));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("V18INPUT reading it: 'key down after 0-1 frames' means UPlayerInput saw the key. If "
			     "acceleration lags THAT by more than a frame, the time is being spent inside Enhanced "
			     "Input (the IMC_Trace context rebuild), not in the window, the OS or the pawn."));

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("V18INPUT ===== PASS: every press reached the movement simulation promptly ====="));
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("V18INPUT ===== %d press(es) were slow or lost ====="), Failures);
		}
	}

	void RunInputLatency()
	{
		APlayerController* PC = LatencyController();
		ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		if (PC == nullptr || Pawn == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("V18INPUT: no local player controller with a pawn. This measurement is about the "
				     "player's own input path and cannot be taken anywhere else."));
			return;
		}

		TSharedPtr<FLatencyState> State = MakeShared<FLatencyState>();
		State->Pawn = Pawn;
		State->Key = UTraceUserSettings::Get().GetKey(ETraceInputAction::MoveForward);
		// Generous, because phase 0 may legitimately spend most of it waiting for the character-select
		// screen's auto-pick timeout to hand gameplay input back. A deadline shorter than that menu
		// turns "the harness never got to press anything" into "the game never responded".
		State->Deadline = FPlatformTime::Seconds() + 120.0;

		if (!State->Key.IsValid())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("V18INPUT: MoveForward has no key bound, so there is nothing to press."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("V18INPUT starting: three presses of %s (cold, warm, and one straight after a mapping "
			     "rebuild). netMode=%d role=%d"),
			*State->Key.ToString(), static_cast<int32>(PC->GetNetMode()),
			static_cast<int32>(Pawn->GetLocalRole()));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float Delta) -> bool
		{
			// Named apart from RunInputLatency's own Pawn/PC on purpose. The lambda captures neither,
			// so this is not shadowing today — but MSVC's C4457/C4459 family is exactly the class of
			// warning macOS cannot raise (UBT hard-disables shadow warnings on the clang this project
			// builds with), so the safe convention is applied rather than reasoned about.
			ATraceCharacter* TickPawn = State->Pawn.Get();
			APlayerController* TickPC = LatencyController();
			UTraceCharacterMovementComponent* TickMove =
				(TickPawn != nullptr) ? TickPawn->GetTraceMovement() : nullptr;
			if (TickPawn == nullptr || TickPC == nullptr || TickMove == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("V18INPUT ABORTED: the pawn or controller went away."));
				return false;
			}
			if (FPlatformTime::Seconds() > State->Deadline)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("V18INPUT ABORTED: out of time at phase %d."), State->Phase);
				ReportLatency(*State);
				return false;
			}

			State->PhaseTime += Delta;

			switch (State->Phase)
			{
			case 0:
			{
				// WAIT FOR A MENU TO LET GO FIRST. The character-select screen suppresses gameplay
				// input for as long as it is up, so a press made under it is a measurement of the menu.
				// Waiting is reported (WaitedForMenuSeconds) rather than hidden, because "how long
				// after the match starts can I actually move" is itself a §1c number.
				ATracePlayerController* TracePC = Cast<ATracePlayerController>(TickPC);
				if (TracePC != nullptr && TracePC->IsGameInputSuppressed())
				{
					State->WaitedForMenuSeconds += Delta;
					State->PhaseTime = 0.f;
					return true;
				}

				// Settle. The pawn must be at rest and holding nothing, or "acceleration became
				// non-zero" is already true before the press and the measurement is zero by accident.
				// Longer than the hold below, so the previous arm's auto-release has certainly landed.
				if (State->PhaseTime > 1.2f)
				{
					State->Phase = (State->Press == 2) ? 1 : 2;
					State->PhaseTime = 0.f;
				}
				return true;
			}

			case 1:
				// The REBUILD arm. This is the tail of ATracePlayerController::ApplyControlSettings,
				// which every spawn runs through AcknowledgePossession -> AddInputMappings ->
				// BuildInputData. Calling it here reproduces that cost without touching Core/.
				if (const ULocalPlayer* LocalPlayer = TickPC->GetLocalPlayer())
				{
					if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
							LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
					{
						Subsystem->RequestRebuildControlMappings();
					}
				}
				State->Phase = 2;
				State->PhaseTime = 0.f;
				return true;

			case 2:
			{
				// PRESS. Everything about the sample is stamped on this exact frame.
				State->PressWallTime = FPlatformTime::Seconds();
				State->PressFrame = GFrameCounter;
				State->FramesToKeyDown = -1;
				State->FramesToAccel = -1;
				const bool bAccepted = HoldKeyThroughViewport(State->Key, 0.60f);
				State->ResultsAccepted[State->Press] = bAccepted;
				if (const ATracePlayerController* TracePC = Cast<ATracePlayerController>(TickPC))
				{
					State->ResultsSuppressed[State->Press] = TracePC->IsGameInputSuppressed();
				}
				UE_LOG(LogTraceGame, Display,
					TEXT("V18INPUT press %d: Trace.SimInput %s 0.60 accepted=%d, ignoreMoveInput=%d, pawn=%s"),
					State->Press, *State->Key.ToString(), bAccepted ? 1 : 0,
					TickPC->IsMoveInputIgnored() ? 1 : 0, *GetNameSafe(TickPawn));
				State->Phase = 3;
				State->PhaseTime = 0.f;
				return true;
			}

			case 3:
			{
				const int32 FramesSince = static_cast<int32>(GFrameCounter - State->PressFrame);

				// The key releases itself 0.60 s after the press — Trace.SimInput owns that half of the
				// hold, which is why nothing here has to remember to let go.
				if (State->FramesToKeyDown < 0 && TickPC->IsInputKeyDown(State->Key))
				{
					State->FramesToKeyDown = FramesSince;
					State->ResultsKeyFrames[State->Press] = FramesSince;
				}
				if (State->FramesToAccel < 0
					&& FVector(TickMove->GetCurrentAcceleration().X, TickMove->GetCurrentAcceleration().Y, 0.f)
						.SizeSquared() > 1.f)
				{
					State->FramesToAccel = FramesSince;
					State->ResultsMs[State->Press] =
						static_cast<float>((FPlatformTime::Seconds() - State->PressWallTime) * 1000.0);
					State->ResultsFrames[State->Press] = FramesSince;
				}

				// A 2.0 s ceiling because the complaint is "about a second": a press that has not
				// reached the pawn in two whole seconds has already answered the question, and waiting
				// longer only lengthens the run.
				if (State->FramesToAccel >= 0 || State->PhaseTime > 2.0f)
				{
					TickMove->StopMovementImmediately();

					++State->Press;
					if (State->Press >= 3)
					{
						ReportLatency(*State);
						return false;
					}
					State->Phase = 0;
					State->PhaseTime = 0.f;
				}
				return true;
			}

			default:
				return false;
			}
		}));
	}

	// =============================================================================================
	// Commands
	// =============================================================================================

	FAutoConsoleCommand CmdAirReverse(
		TEXT("Trace.Move.V18.AirReverse"),
		TEXT("Spec v18 sec 1a. Drives the SHIPPED air-strafe arithmetic at a sweep of wish angles in BOTH "
		     "arms (Trace.Move.V18.LegacyAirReverse on and off) and prints the table. The rows at 90 "
		     "degrees and inside it must be bit-identical; the 180 row must show RED unchanged and GREEN "
		     "slowing. Needs no pawn."),
		FConsoleCommandDelegate::CreateStatic(&RunAirReverse));

	FAutoConsoleCommand CmdAirDrift(
		TEXT("Trace.Move.V18.AirDrift"),
		TEXT("Spec v18 sec 1b. Arms a per-move ledger on the local pawn for N seconds (default 12) that "
		     "records every AIRBORNE move whose input acceleration is zero, how far the planar velocity "
		     "moved on it, and which mid-air velocity writer was live. Jump and let go of everything."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunAirDrift));

	FAutoConsoleCommand CmdAirDriftDrive(
		TEXT("Trace.Move.V18.AirDriftDrive"),
		TEXT("Spec v18 sec 1b, self-driving. Arms the ledger AND jumps the local pawn every 1.2 s with no "
		     "directional input, so a headless run produces airborne zero-input moves without a human."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunAirDriftDrive));

	FAutoConsoleCommand CmdInputLatency(
		TEXT("Trace.Move.V18.InputLatency"),
		TEXT("Spec v18 sec 1c. Injects real key presses through APlayerController::InputKey and counts the "
		     "frames until the movement component actually accelerates -- cold, warm, and immediately "
		     "after an Enhanced Input mapping rebuild (which is what every spawn does)."),
		FConsoleCommandDelegate::CreateStatic(&RunInputLatency));
}

#endif // !UE_BUILD_SHIPPING
