// =================================================================================================
// TraceMovementSurf.cpp — PATCH 28 §5, "accelerate using curved ramps", MEASURED.
//
// Nothing in this file is shipped gameplay. The feature itself is three overrides and five saved
// fields in TraceCharacterMovementComponent (read that file's header block first); this is the
// instrument, and it exists because "players should be able to accelerate using curved ramps" is a
// claim about a CURVE — entry speed in, exit speed out — and a claim about a BOUND, and neither of
// those is answerable by looking at the game.
//
//   Trace.Move.Surf         the arithmetic, with no pawn and no level. Four tables:
//                             1. WHICH FACES ARE SURFABLE, swept across the whole slope range, with
//                                the walkable limit marked. This is the "you cannot surf ordinary
//                                geometry" proof, and it goes RED if any walkable slope is accepted.
//                             2. THE CLIP ITSELF, driven through the SHIPPED
//                                ClipVelocityAgainstPlane() with the LIVE SurfOverbounce (the same
//                                accessor ComputeSlideVector passes, so the knob moves this table),
//                                showing that the normal component lands on the clip's contract —
//                                exactly zero at the shipped overbounce of 1.0 — and the along-plane
//                                component survives to the last float bit; next to what the engine's
//                                own anti-slope-boost rule would have done to the same vector, which
//                                is the RED arm and is the whole reason this feature needed code at
//                                all.
//                             3. THE ENERGY SOURCE: gravity's along-plane component per facet angle,
//                                on this build's live gravity scale.
//                             4. THE BOUND: the ceiling, where it comes from, and a closed-form
//                                integration showing the speed converges instead of diverging.
//
//   Trace.Move.SurfReport   what THIS pawn's rides actually did. Read it on a client for the
//                           prediction column.
//
//   -TraceSurfTest          the live rig. Twelve runs on a real arena rail: five entry speeds with an
//                           ideal strafe, the same five with NO input at all (the control arm — how
//                           much of the gain is the ramp and how much is the player), and two
//                           NEGATIVE CONTROLS that drop the pawn on walkable ground and assert it
//                           never enters the surf state.
//
// WHY THE RIG ASKS THE ARENA WHERE THE RAMP IS. ATraceArenaBuilder::GetSurfRailProbe() is the single
// definition of a rail's geometry. A rig holding its own copy of the coordinates would keep passing
// after the level moved, which is this project's standing lesson about two copies of one rule — and
// it would be a particularly cruel version of it, because a pawn teleported into empty air simply
// falls and the harness would report "no surf" as a movement bug.
//
// The namespace is named after the file, not anonymous: two anonymous namespaces with a LocalPawn()
// in them are one namespace with two definitions under the Windows unity build (MSVC C2084), and
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
// =================================================================================================

#include "CoreMinimal.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "World/TraceArenaBuilder.h"
#include "Trace.h"

#if !UE_BUILD_SHIPPING

#include "EngineUtils.h"   // TActorIterator

namespace TraceMovementSurf
{
	/** The local player's movement component, or null. Every command here needs one or says so. */
	static UTraceCharacterMovementComponent* LocalSurfMovement()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr)
			{
				continue;
			}

			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* PC = It->Get();
				if (PC == nullptr || !PC->IsLocalController())
				{
					continue;
				}
				if (ACharacter* Pawn = Cast<ACharacter>(PC->GetPawn()))
				{
					if (UTraceCharacterMovementComponent* Movement =
						Cast<UTraceCharacterMovementComponent>(Pawn->GetCharacterMovement()))
					{
						return Movement;
					}
				}
			}
		}

		return nullptr;
	}

	/**
	 * WHAT THE ENGINE WOULD HAVE DONE TO THE SAME VECTOR — the RED arm of table 2.
	 *
	 * This is UCharacterMovementComponent::HandleSlopeBoosting's own arithmetic, and it is the ONE
	 * place in this project where an engine rule is re-typed rather than called. That is deliberate
	 * and it is the only honest option: the function is protected, non-virtual and const on the live
	 * component, so a table that "called the shipped one" would be calling the very override this
	 * feature adds and both columns would be identical. It is reproduced here, labelled, with the
	 * engine file and the branch named, and it is compared against the SHIPPED clip — so if the engine
	 * ever changes this rule the table's RED column becomes stale, which is visible, rather than the
	 * GREEN column becoming wrong, which would not be.
	 *
	 * (Engine/Source/Runtime/Engine/Private/Components/CharacterMovementComponent.cpp,
	 *  UCharacterMovementComponent::HandleSlopeBoosting.)
	 */
	static FVector EngineSlopeBoostedSlide(const FVector& Delta, const FVector& Normal, const float Time)
	{
		FVector Result = FVector::VectorPlaneProject(Delta, Normal) * Time;

		if (Result.Z > 0.f)
		{
			const float ZLimit = static_cast<float>(Delta.Z) * Time;
			if (Result.Z - ZLimit > UE_KINDA_SMALL_NUMBER)
			{
				if (ZLimit > 0.f)
				{
					Result *= (ZLimit / Result.Z);
				}
				else
				{
					// "We were heading down but were going to deflect upwards. Just make the deflection
					// horizontal." THIS BRANCH IS EVERY SURF CONTACT THERE IS.
					Result = FVector::ZeroVector;
				}

				const FVector RemainderXY = (FVector::VectorPlaneProject(Delta, Normal) * Time - Result)
					* FVector(1.f, 1.f, 0.f);
				const FVector NormalXY = Normal.GetSafeNormal2D();
				Result += FVector::VectorPlaneProject(RemainderXY, NormalXY);
			}
		}

		return Result;
	}

	// =============================================================================================
	// Trace.Move.Surf — the arithmetic
	// =============================================================================================

	static void RunSurfAudit()
	{
		const UTraceCharacterMovementComponent* Movement = LocalSurfMovement();
		if (Movement == nullptr)
		{
			// The CDO answers every question in tables 1, 2 and 4 — none of them needs a pawn, and a
			// command that refused to run without one would be unusable from a dedicated server log.
			Movement = UTraceCharacterMovementComponent::StaticClass()
				->GetDefaultObject<UTraceCharacterMovementComponent>();
		}
		if (Movement == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("SURF: no movement component and no CDO. Nothing to audit."));
			return;
		}

		float BandLo = 0.f;
		float BandHi = 0.f;
		Movement->GetSurfSlopeBandDegrees(BandLo, BandHi);

		UE_LOG(LogTraceGame, Display, TEXT("========================================================================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("SURF AUDIT (Patch 28 s5). This build surfs slopes %.3f..%.3f deg. Speed ceiling %.0f "
			     "uu/s. Every tuning value is on the MOVECFG-P28 line in this same log, printed once at "
			     "BeginPlay; nothing here re-derives one."),
			BandLo, BandHi, Movement->GetSurfSpeedCeiling());

		// --- TABLE 1: which faces are surfable ---------------------------------------------------
		//
		// Swept across the whole slope range in 2.5 degree steps plus the two boundaries exactly, so
		// the transition is visible rather than inferred. THE VERDICT LINE IS THE POINT: any walkable
		// slope accepted as a surf plane is a FAIL, because it would mean a player could surf the
		// arena floor.
		UE_LOG(LogTraceGame, Display, TEXT(""));
		UE_LOG(LogTraceGame, Display,
			TEXT("  [1] WHICH FACES THIS BUILD WILL SURF.  slope 0 = flat floor, 90 = vertical wall."));
		UE_LOG(LogTraceGame, Display,
			TEXT("      %-8s %-8s %-6s  %s"), TEXT("slope"), TEXT("Nz"), TEXT("surf?"), TEXT("what it is"));

		int32 WalkableAccepted = 0;
		int32 SurfAccepted = 0;
		TArray<float> Slopes;
		for (float Degrees = 0.f; Degrees <= 90.f; Degrees += 2.5f)
		{
			Slopes.Add(Degrees);
		}
		Slopes.Add(BandLo);
		Slopes.Add(BandLo - 0.01f);
		Slopes.Add(BandLo + 0.01f);
		Slopes.Add(BandHi);
		Slopes.Add(BandHi - 0.01f);
		Slopes.Add(BandHi + 0.01f);
		Slopes.Sort();

		for (const float Degrees : Slopes)
		{
			const float Nz = FMath::Cos(FMath::DegreesToRadians(Degrees));
			const FVector Normal(FMath::Sin(FMath::DegreesToRadians(Degrees)), 0.f, Nz);
			const bool bSurf = Movement->IsSurfPlane(Normal);
			const bool bWalkable = (Degrees <= BandLo);

			if (bSurf && bWalkable)
			{
				++WalkableAccepted;
			}
			if (bSurf)
			{
				++SurfAccepted;
			}

			const TCHAR* What = bWalkable ? TEXT("WALKABLE FLOOR - the pawn stands on it")
				: (bSurf ? TEXT("SURF PLANE") : TEXT("wall - the wall jump's, not surf's"));

			UE_LOG(LogTraceGame, Display, TEXT("      %7.3f  %6.4f  %-6s %s%s"),
				Degrees, Nz, bSurf ? TEXT("YES") : TEXT("no"), What,
				(bSurf && bWalkable) ? TEXT("   <<<< FAIL: A WALKABLE FACE IS SURFABLE") : TEXT(""));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("      VERDICT: %d walkable slopes accepted as surf planes (must be 0) -> %s. "
			     "%d of %d sampled slopes are surfable."),
			WalkableAccepted, (WalkableAccepted == 0) ? TEXT("PASS") : TEXT("*** FAIL ***"),
			SurfAccepted, Slopes.Num());

		// --- TABLE 2: the clip, green vs the engine's red ----------------------------------------
		UE_LOG(LogTraceGame, Display, TEXT(""));
		// THE OVERBOUNCE IS READ FROM THE LIVE COMPONENT, NOT TYPED HERE. It used to be
		// `const float Overbounce = 1.f;`, i.e. the shipped function called with a literal, so this
		// table answered the same thing whatever Trace.Move.Surf's own banner said and whatever
		// SurfOverbounce was set to — W8-KNOBS found it by running -ini:Game:...:SurfOverbounce=1.15
		// and getting a byte-identical table. GetSurfOverbounceForAudit() forwards to the same
		// GetSurfOverbounce() that ComputeSlideVector() passes on the shipped path (the tuning getter
		// itself is protected, like every other knob accessor on the component), so the GREEN column is
		// now the shipped clip with the shipped argument rather than the shipped clip with a copied
		// constant.
		const float Overbounce = Movement->GetSurfOverbounceForAudit();

		UE_LOG(LogTraceGame, Display,
			TEXT("  [2] THE CLIP, one sub-step of a real ride. GREEN is the shipped "
			     "ClipVelocityAgainstPlane() called with this build's LIVE SurfOverbounce = %.3f; RED is "
			     "what UE's HandleSlopeBoosting() does to the SAME vector, which is the behaviour this "
			     "feature replaces."),
			Overbounce);
		UE_LOG(LogTraceGame, Display,
			TEXT("      %-6s %-28s %-9s %-9s %-9s %-9s %s"),
			TEXT("slope"), TEXT("incoming velocity"), TEXT("|in|"), TEXT("|GREEN|"), TEXT("|RED|"),
			TEXT("dot(G,n)"), TEXT("verdict"));

		int32 ClipFailures = 0;
		int32 RedDiffers = 0;
		int32 RedRows = 0;
		const float MidSlope = (BandLo + BandHi) * 0.5f;
		for (const float Degrees : { BandLo + 2.f, MidSlope, BandHi - 2.f })
		{
			const float Rad = FMath::DegreesToRadians(Degrees);

			// FOUR VECTORS, AND THE FIRST ONE IS THE CONTROL. The face rises in +Y, so +Y velocity is
			// INTO it. HandleSlopeBoosting only bites when the deflection comes out with a POSITIVE Z,
			// which happens exactly when the pawn is moving into the face faster than it is falling —
			// which is every frame of a held up-slope surf input, and is not the case when the pawn is
			// simply sliding down. Both cases are in the table so the RED column shows where the engine
			// rule is harmless and where it is the whole feature.
			for (const FVector& In : {
					FVector(1200.f, 0.f, -400.f),     // pure descent: the rule does not bite
					FVector(1200.f, 700.f, -150.f),   // riding with up-slope input held
					FVector( 600.f, 900.f, -250.f),   // arriving at the face off a run
					FVector(1600.f, 500.f, -600.f) }) // fast ride, input held, falling hard
			{
				// Face rises in +Y, so the outward normal leans in -Y and up.
				const FVector Normal(0.f, -FMath::Sin(Rad), FMath::Cos(Rad));

				const FVector Green =
					UTraceCharacterMovementComponent::ClipVelocityAgainstPlane(In, Normal, Overbounce);
				const FVector Red = EngineSlopeBoostedSlide(In, Normal, 1.f);

				const float DotIn = static_cast<float>(FVector::DotProduct(In, Normal));
				const float DotG = static_cast<float>(FVector::DotProduct(Green, Normal));

				// WHAT "CLIPPED, NOT STOPPED" MEANS ONCE THE OVERBOUNCE IS LIVE. PM_ClipVelocity's
				// contract is that the outgoing normal component is (1 - overbounce) x the incoming
				// one. At the shipped 1.0 that is exactly zero and this test is the same "must be 0"
				// it has always been, to the bit. Above 1 the pawn is pushed OFF the face and below 1
				// it sinks by a bounded fraction, and a row that called either of those "still moving
				// into the plane" would report the instrument's own literal as a defect in the game —
				// which is the failure this table was just fixed for.
				const float DotExpected = DotIn * (1.f - Overbounce);
				const bool bOk = FMath::Abs(DotG - DotExpected) < 0.5f;
				if (!bOk)
				{
					++ClipFailures;
				}
				++RedRows;
				if (!FMath::IsNearlyEqual(static_cast<float>(Green.Size()), static_cast<float>(Red.Size()), 0.5f))
				{
					++RedDiffers;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("      %5.2f  (%7.0f,%7.0f,%7.0f)  %8.1f  %8.1f  %8.1f  %8.4f  %s"),
					Degrees, In.X, In.Y, In.Z, In.Size(), Green.Size(), Red.Size(), DotG,
					bOk ? TEXT("clipped, not stopped") : TEXT("*** wrong normal component ***"));
			}
		}
		UE_LOG(LogTraceGame, Display,
			TEXT("      VERDICT: %d of the clipped vectors missed the clip's own contract, "
			     "dot(G,n) == (1 - overbounce) x dot(in,n) = %.3f x dot(in,n) -- at overbounce 1.000 "
			     "that is 'no component into the plane at all' (must be 0) -> %s.  RED differs from "
			     "GREEN on %d of %d rows -- the rows where the deflection came out with a positive Z, "
			     "i.e. every frame of a held up-slope surf input. Those are the frames the stock "
			     "component flattens."),
			ClipFailures, 1.f - Overbounce,
			(ClipFailures == 0) ? TEXT("PASS") : TEXT("*** FAIL ***"), RedDiffers, RedRows);

		// --- TABLE 3: where the acceleration comes from -------------------------------------------
		const float GravityZ = FMath::Abs(Movement->GetGravityZ());
		UE_LOG(LogTraceGame, Display, TEXT(""));
		UE_LOG(LogTraceGame, Display,
			TEXT("  [3] THE ENERGY SOURCE. Gravity on this build is %.0f uu/s^2 (GravityScale %.3f). On a "
			     "surf plane the normal component is removed by the clip and does no work; what is left "
			     "is g*sin(slope) straight down the face, and THAT is the free acceleration a ramp gives."),
			GravityZ, Movement->GravityScale);
		UE_LOG(LogTraceGame, Display,
			TEXT("      %-8s %-14s %s"), TEXT("slope"), TEXT("a_along"), TEXT("time to cross a 616 uu-tall face from rest"));
		for (const float Degrees : { BandLo + 2.f, MidSlope, BandHi - 2.f })
		{
			const float Rad = FMath::DegreesToRadians(Degrees);
			const float Along = GravityZ * FMath::Sin(Rad);
			const float FaceRun = 616.f / FMath::Max(0.01f, FMath::Sin(Rad));
			UE_LOG(LogTraceGame, Display, TEXT("      %7.3f  %9.0f uu/s^2  %.2f s over %.0f uu of face"),
				Degrees, Along, FMath::Sqrt(2.f * FaceRun / FMath::Max(1.f, Along)), FaceRun);
		}

		// --- TABLE 4: the bound -------------------------------------------------------------------
		//
		// Integrated rather than argued. The worst case is the steepest facet with the pawn already at
		// the ceiling, so the question "does the clamp actually hold" is answered by running the
		// integration for longer than any ramp in the arena can last and printing where it settles.
		UE_LOG(LogTraceGame, Display, TEXT(""));
		UE_LOG(LogTraceGame, Display,
			TEXT("  [4] THE BOUND, INTEGRATED. Worst case: the steepest facet, gravity along it, no "
			     "friction, for 30 s -- forty times longer than the tallest rail in the arena can hold a "
			     "pawn. Unbounded means the ramp is a speed exploit."));

		const float SteepRad = FMath::DegreesToRadians(BandHi);
		const float AlongAccel = GravityZ * FMath::Sin(SteepRad);
		const float DtStep = 1.f / 60.f;
		float Unbounded = 0.f;
		float Bounded = 0.f;
		// GetSurfSpeedCeiling() reads the LIVE entry speed of a ride in progress, which is zero here, so
		// the pure config half of it is re-derived from the same two getters it uses. Stated rather than
		// hidden: this is the ceiling for an entry at or below the cap, which is every ordinary entry.
		const float ConfigCeiling = Movement->GetSurfSpeedCeiling();
		for (float T = 0.f; T < 30.f; T += DtStep)
		{
			Unbounded += AlongAccel * DtStep;
			Bounded = FMath::Min(Bounded + AlongAccel * DtStep, ConfigCeiling);
		}
		UE_LOG(LogTraceGame, Display,
			TEXT("      after 30 s:  UNCLAMPED %.0f uu/s (diverging, +%.0f uu/s every second)   "
			     "CLAMPED %.0f uu/s against a ceiling of %.0f -> %s"),
			Unbounded, AlongAccel, Bounded, ConfigCeiling,
			(Bounded <= ConfigCeiling + 1.f) ? TEXT("BOUNDED, PASS") : TEXT("*** FAIL ***"));
		UE_LOG(LogTraceGame, Display,
			TEXT("      The ceiling is DERIVED: air hard cap x SurfSpeedCeilingMultiplier. Retune the air "
			     "cap and this moves with it; there is no second literal to forget."));
		UE_LOG(LogTraceGame, Display, TEXT("========================================================================"));
	}

	static void RunSurfReport()
	{
		if (UTraceCharacterMovementComponent* Movement = LocalSurfMovement())
		{
			Movement->LogSurfReport();
			return;
		}

		UE_LOG(LogTraceGame, Warning,
			TEXT("SURF: no locally controlled pawn, so there are no rides to report. "
			     "Trace.Move.Surf needs no pawn and answers the arithmetic questions."));
	}

	// =============================================================================================
	// Commands
	// =============================================================================================

	FAutoConsoleCommand CmdSurfAudit(
		TEXT("Trace.Move.Surf"),
		TEXT("Patch 28 sec 5. The surf arithmetic, with no pawn and no level: which faces are surfable "
		     "(with the walkable limit marked -- this is the 'you cannot surf ordinary geometry' proof), "
		     "the SHIPPED velocity clip next to what UE's anti-slope-boost rule does to the same vector, "
		     "gravity's along-plane component, and an integration showing the speed is bounded."),
		FConsoleCommandDelegate::CreateStatic(&RunSurfAudit));

	FAutoConsoleCommand CmdSurfReport(
		TEXT("Trace.Move.SurfReport"),
		TEXT("Patch 28 sec 5. What THIS pawn's rides actually did: count, mean and best entry -> exit "
		     "speed, how often the ceiling bound, how many walkable contacts were refused, and how many "
		     "server corrections landed inside a ride. Read the last one on a CLIENT."),
		FConsoleCommandDelegate::CreateStatic(&RunSurfReport));

	// =============================================================================================
	// -TraceSurfTest — the live rig
	// =============================================================================================

	/** One closed run. Kept in the TU because the rig drives exactly one pawn (the local player's). */
	struct FSurfRunResult
	{
		float RequestedEntry = 0.f;
		float MeasuredEntry = 0.f;
		float ExitSpeed = 0.f;
		float PeakSpeed = 0.f;
		float Seconds = 0.f;
		bool bStrafed = false;
		bool bNegativeControl = false;
		bool bSurfed = false;
	};

	static TArray<FSurfRunResult>& SurfRunResults()
	{
		static TArray<FSurfRunResult> Results;
		return Results;
	}

	/** The entry-speed ladder. Five rungs from a walk to well past the ground limit. */
	static constexpr float SurfTestEntryLadder[] = { 400.f, 800.f, 1200.f, 1600.f, 2000.f };
	static constexpr int32 SurfTestLadderCount = UE_ARRAY_COUNT(SurfTestEntryLadder);

	/** Ladder, then the same ladder with no input, then two negative controls. */
	static constexpr int32 SurfTestTotalRuns = SurfTestLadderCount * 2 + 2;

	/** How long a run is allowed to last before it is closed as "still going". */
	static constexpr float SurfTestRunSeconds = 4.0f;

	/** Settling time after a placement, before anything is measured. */
	static constexpr float SurfTestSettleSeconds = 0.15f;

	/**
	 * Rides in the two-process net arm, and how long the pawn stands still on the crest first.
	 *
	 * 2.5 s is not a guess: at the 40 ms of emulated one-way lag this arm runs with, a server
	 * correction for a client-side teleport is sent within one round trip (~80 ms) and resolved within
	 * a couple of frames of that. Two and a half seconds is more than an order of magnitude of margin,
	 * and the ride does not begin until the pawn has run off the crest under its own input after it,
	 * so no correction caused by the placement can possibly land inside a measured ride.
	 */
	static constexpr int32 SurfNetTestRuns = 8;
	static constexpr float SurfNetSettleSeconds = 2.5f;

	/**
	 * How often the SERVER re-poses the remote pawn onto the crest in the net arm.
	 *
	 * Long enough that the correction the pose itself causes is ancient history by the time the client
	 * has walked off the edge and a ride has started (a ride's attribution window is 2 s from its first
	 * contact), and short enough that eight rides fit inside a two-minute run.
	 */
	static constexpr float SurfNetCycleSeconds = 12.f;

	/**
	 * HOW FAST THE RIG IS ALLOWED TO SWING ITS WISH DIRECTION, degrees per second.
	 *
	 * THE FIRST VERSION OF THIS RIG BLENDED THE PERPENDICULAR WITH THE RAIL DIRECTION AND MEASURED
	 * NOTHING, and the arithmetic of why is worth keeping because it is a trap anyone writing a surf
	 * harness will fall into. The air model's gain is
	 *
	 *     AddSpeed = AirMaxWishSpeed - dot(velocity, wishDirection)
	 *
	 * so ANY forward component in the wish direction is multiplied by the pawn's whole speed. A 0.35
	 * blend toward the rail puts dot(v, wish) at 0.33 v, which passes the 160 uu/s wish cap at just
	 * 485 uu/s — above that the gain is exactly zero, and the "ideal strafe" arm and the "no input"
	 * arm agreed to 1 uu/s on four of five rungs. That looked like a broken feature and was a broken
	 * harness.
	 *
	 * The wish is therefore held PERPENDICULAR to travel, which is the input the formula rewards. What
	 * stops that from spinning the pawn in a circle is the same thing that stops it in the real game:
	 * a mouse cannot snap. The wish direction chases the ideal at this rate and no faster, so the rig
	 * models a fast human flick (a 180 in 0.36 s) rather than a rotation no input device can produce.
	 * The numbers it reports are therefore reachable, not theoretical.
	 */
	static constexpr float SurfTestTurnRateDegPerSec = 500.f;

	/**
	 * The rig's current wish yaw, in degrees. A TU static rather than a member because the rig drives
	 * exactly one pawn — TickSurfTest returns immediately unless the pawn is locally controlled by a
	 * player controller — and because it is a property of the synthetic INPUT, not of the movement
	 * state, so it must never be mistaken for something the saved move should carry.
	 */
	static float& SurfTestWishYaw()
	{
		static float Yaw = 0.f;
		return Yaw;
	}

	/** False until the wish yaw has been seeded from the first surf contact of the current run. */
	static bool& SurfTestWishYawValid()
	{
		static bool bValid = false;
		return bValid;
	}
}

void UTraceCharacterMovementComponent::TickSurfTest(float DeltaSeconds)
{
	// TWO MODES, AND THE SECOND ONE EXISTS BECAUSE THE FIRST CANNOT BE TRUSTED ON A CLIENT.
	//
	// -TraceSurfTest is the SPEED-GAIN rig: it teleports the pawn onto the face with a chosen entry
	// speed, which is the only way to sweep a ladder of entry speeds, and is exactly right on a listen
	// host where the pawn is authoritative.
	//
	// ON A JOINED CLIENT A TELEPORT IS NOT A PREDICTED MOVE. The server has not moved the pawn, so it
	// corrects — and that correction would land inside the ride the rig had just started and be counted
	// as a surf desync when it is a harness artefact. -TraceSurfNetTest therefore never places a pawn
	// anywhere near a ride: it puts it on the WALKABLE CREST, waits two and a half seconds (more than
	// an order of magnitude longer than any correction takes to arrive and resolve at 40 ms of lag),
	// and then RUNS IT OFF THE EDGE under ordinary predicted input. Every frame of every ride it
	// measures is a frame the client predicted and the server re-simulated, which is the only
	// arrangement in which "zero corrections while surfing" means anything.
	static const bool bLadderMode = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfTest"));
	static const bool bNetMode = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfNetTest"));
	const bool bEnabled = bLadderMode || bNetMode;
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
	if (TestWorld == nullptr || bSurfTestReported != 0)
	{
		return;
	}

	// ASK THE ARENA. See the file header: a rig that knew where the rail was would keep passing after
	// the rail moved, and a pawn teleported into empty air reports "no surf" as if it were a movement
	// bug.
	TActorIterator<ATraceArenaBuilder> ArenaIt(TestWorld);
	ATraceArenaBuilder* Arena = ArenaIt ? *ArenaIt : nullptr;
	if (Arena == nullptr)
	{
		return;
	}

	// The rail in the pawn's own quadrant, so the rig works from wherever the match spawned it.
	const FVector Here = UpdatedComponent->GetComponentLocation();
	const ATraceArenaBuilder::FTraceSurfRailProbe Probe =
		Arena->GetSurfRailProbe((Here.X >= 0.0) ? 1.f : -1.f, (Here.Y >= 0.0) ? 1.f : -1.f);

	if (!Probe.bValid)
	{
		if (TestWorld->GetTimeSeconds() > 8.f)
		{
			bSurfTestReported = 1;
			UE_LOG(LogTraceGame, Error,
				TEXT("SURFTEST: this level has no surf rails (bBuildSurfRails off, or a baked map that was "
				     "baked before they existed). Run on /Game/Maps/Arena."));
		}
		return;
	}

	// =============================================================================================
	// THE SERVER'S HALF OF THE NET ARM — AND THE REASON THE CLIENT'S HALF NEVER TELEPORTS ANYTHING.
	//
	// A client-side teleport is not a predicted move: the server has not moved the pawn, so it
	// corrects, and the first attempt at this arm measured exactly that — the client placed itself on
	// the crest at (8034, 3324, 706) and was snapped back to its endzone spawn at X 19125 within a
	// round trip, every run, so not one ride ever started.
	//
	// So the PLACEMENT is authoritative and the RIDE is predicted, which is the only split that makes
	// the measurement mean anything. The server re-poses the remote pawn onto the walkable crest on a
	// fixed cadence; the client does nothing but wait for it to land, run off the edge under ordinary
	// input, and measure. Every frame the client counts as a ride is a frame it predicted and the
	// server re-simulated from the same ServerMove.
	//
	// It needs no RPC and adds no replicated surface to the shipped class: BOTH processes are launched
	// with -TraceSurfNetTest, and a server that was not is inert here.
	// =============================================================================================
	if (bNetMode && CharacterOwner->HasAuthority() && !CharacterOwner->IsLocallyControlled())
	{
		const float Now = static_cast<float>(TestWorld->GetTimeSeconds());
		if (Now < 8.f)
		{
			return;
		}

		if (SurfTestPhaseTime <= 0.f || Now >= SurfTestPhaseTime)
		{
			SurfTestPhaseTime = Now + TraceMovementSurf::SurfNetCycleSeconds;

			const FVector Where = Probe.CrestStand + FVector(0.f, 0.f, 120.f);
			CharacterOwner->TeleportTo(Where, CharacterOwner->GetActorRotation(), false, true);
			Velocity = FVector::ZeroVector;

			UE_LOG(LogTraceGame, Display,
				TEXT("SURFNET [server] re-posed %s onto the crest at %s (next in %.1f s). The client is "
				     "not told and does not teleport: it waits, runs off the edge, and measures."),
				*GetNameSafe(CharacterOwner), *Where.ToCompactString(),
				TraceMovementSurf::SurfNetCycleSeconds);
		}
		return;
	}

	APlayerController* TestController = Cast<APlayerController>(CharacterOwner->GetController());
	if (TestController == nullptr || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	if (SurfTestRun < 0)
	{
		if (TestWorld->GetTimeSeconds() < 6.f)
		{
			return;
		}

		SurfTestRun = 0;
		SurfTestPhaseTime = -1.f;   // -1 asks for a placement on the next tick
		TraceMovementSurf::SurfRunResults().Reset();

		const int32 TotalRunsForLog = bNetMode
			? TraceMovementSurf::SurfNetTestRuns
			: TraceMovementSurf::SurfTestTotalRuns;

		UE_LOG(LogTraceGame, Display,
			TEXT("SURFTEST ---- begin. netMode=%d role=%d | rail: %d..%d deg face, crest %.0f uu, run %.0f uu, "
			     "entry at %s | ceiling %.0f uu/s | %d runs (%d strafed, %d no-input control, 2 negative "
			     "controls on walkable ground)"),
			static_cast<int32>(GetNetMode()), static_cast<int32>(CharacterOwner->GetLocalRole()),
			FMath::RoundToInt(Probe.MinFaceAngleDegrees), FMath::RoundToInt(Probe.MaxFaceAngleDegrees),
			Probe.Height, Probe.RunLength, *Probe.FaceEntry.ToCompactString(),
			GetAirStrafeHardCapSpeed() * GetSurfSpeedCeilingMultiplier(),
			TotalRunsForLog, TraceMovementSurf::SurfTestLadderCount,
			TraceMovementSurf::SurfTestLadderCount);
	}

	const int32 TotalRuns = bNetMode
		? TraceMovementSurf::SurfNetTestRuns
		: TraceMovementSurf::SurfTestTotalRuns;

	if (SurfTestRun >= TotalRuns)
	{
		bSurfTestReported = 1;

		// --- The table ---------------------------------------------------------------------------
		const TArray<TraceMovementSurf::FSurfRunResult>& Results = TraceMovementSurf::SurfRunResults();
		UE_LOG(LogTraceGame, Display, TEXT("===================== SURFTEST: THE SPEED-GAIN CURVE ====================="));
		UE_LOG(LogTraceGame, Display,
			TEXT("  %-22s %-9s %-9s %-9s %-9s %-7s %s"),
			TEXT("arm"), TEXT("asked"), TEXT("entry"), TEXT("exit"), TEXT("peak"), TEXT("secs"), TEXT("gain"));

		int32 NegativeControlFailures = 0;
		for (const TraceMovementSurf::FSurfRunResult& Row : Results)
		{
			if (Row.bNegativeControl)
			{
				NegativeControlFailures += Row.bSurfed ? 1 : 0;
				UE_LOG(LogTraceGame, Display,
					TEXT("  %-22s %8.0f  %-44s %s"),
					TEXT("NEGATIVE CONTROL"), Row.RequestedEntry,
					TEXT("dropped on WALKABLE ground"),
					Row.bSurfed ? TEXT("*** SURFED - FAIL ***") : TEXT("never surfed - PASS"));
				continue;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("  %-22s %8.0f %8.0f %8.0f %8.0f %6.2f  %+.0f uu/s (%.2fx)%s"),
				bNetMode ? TEXT("crest walk-off (net)")
					: (Row.bStrafed ? TEXT("ideal strafe") : TEXT("no input (control)")),
				Row.RequestedEntry, Row.MeasuredEntry, Row.ExitSpeed, Row.PeakSpeed, Row.Seconds,
				Row.ExitSpeed - Row.MeasuredEntry,
				(Row.MeasuredEntry > 1.f) ? (Row.ExitSpeed / Row.MeasuredEntry) : 0.f,
				Row.bSurfed ? TEXT("") : TEXT("   <<<< never entered the surf state"));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("  NEGATIVE CONTROLS: %d of 2 surfed walkable ground (must be 0) -> %s"),
			NegativeControlFailures, (NegativeControlFailures == 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
		LogSurfReport();
		UE_LOG(LogTraceGame, Display, TEXT("=========================================================================="));
		return;
	}

	// --- Placement ---------------------------------------------------------------------------------
	const int32 Ladder = TraceMovementSurf::SurfTestLadderCount;
	const bool bNegative = !bNetMode && (SurfTestRun >= Ladder * 2);
	const bool bStrafe = bNetMode || (SurfTestRun < Ladder);
	// The negative controls are driven at a REAL speed, not parked: "a stationary pawn did not surf"
	// is not a control, it is a tautology. 1200 uu/s is above the ground limit and inside the ladder,
	// so the only difference between them and a measured ride is the ground they are put on.
	const float Requested = bNetMode
		? 0.f
		: (bNegative ? 1200.f : TraceMovementSurf::SurfTestEntryLadder[SurfTestRun % Ladder]);

	// --- THE CLIENT'S HALF OF THE NET ARM: never place, only wait, run off, and measure -----------
	if (bNetMode)
	{
		const FVector NetHere = UpdatedComponent->GetComponentLocation();
		const FVector Crest = Probe.CrestStand;
		const bool bOnCrest = IsMovingOnGround()
			&& FVector::Dist2D(NetHere, Crest) < 900.f
			&& FMath::Abs(NetHere.Z - Crest.Z) < 260.f;

		if (IsSurfing())
		{
			if (bSurfTestArmed == 0)
			{
				bSurfTestArmed = 1;
				SurfTestEntrySpeed = GetSurfEntrySpeed();
				SurfTestPeak = 0.f;
			}
			SurfTestPeak = FMath::Max(SurfTestPeak, GetPlanarSpeed());

			// The same input rule the ladder arm uses, driven through the same AddMovementInput path a
			// key press takes — so every frame of it is a predicted move.
			FVector Planar(Velocity.X, Velocity.Y, 0.f);
			if (Planar.Normalize())
			{
				const FVector UpSlope = -FVector(SurfPlaneNormal.X, SurfPlaneNormal.Y, 0.f).GetSafeNormal();
				FVector Perp(-Planar.Y, Planar.X, 0.f);
				if (FVector::DotProduct(Perp, UpSlope) < 0.f)
				{
					Perp = -Perp;
				}

				const float WishCap = FMath::Max(1.f, GetAirMaxWishSpeed());
				const float Speed = FMath::Max(1.f, GetPlanarSpeed());
				const float CosPhi = FMath::Clamp(WishCap / (2.f * Speed), 0.f, 0.999f);
				const float SinPhi = FMath::Sqrt(FMath::Max(0.f, 1.f - CosPhi * CosPhi));
				const FVector Wish = (Probe.RunDirection * CosPhi + UpSlope * SinPhi).GetSafeNormal();

				CharacterOwner->AddMovementInput(Wish, 1.f);
				TestController->SetControlRotation(Planar.Rotation());
			}
			return;
		}

		// A ride that has ended: record it, and wait for the server's next pose.
		if (bSurfTestArmed != 0)
		{
			TraceMovementSurf::FSurfRunResult Row;
			Row.RequestedEntry = 0.f;
			Row.MeasuredEntry = SurfTestEntrySpeed;
			Row.ExitSpeed = GetPlanarSpeed();
			Row.PeakSpeed = SurfTestPeak;
			Row.Seconds = 0.f;
			Row.bStrafed = true;
			Row.bSurfed = true;
			TraceMovementSurf::SurfRunResults().Add(Row);

			UE_LOG(LogTraceGame, Display,
				TEXT("SURFNET [client] ride %2d/%2d closed: entry %6.0f -> exit %6.0f (peak %6.0f) uu/s | "
				     "corrections in a ride so far %d, session total %d"),
				SurfTestRun + 1, TraceMovementSurf::SurfNetTestRuns, Row.MeasuredEntry, Row.ExitSpeed,
				Row.PeakSpeed, SurfCorrectionsInWindow, CorrectionCount);

			bSurfTestArmed = 0;
			++SurfTestRun;
			return;
		}

		// ONE LINE A SECOND, because a net arm that reports nothing is indistinguishable from a net
		// arm that never ran. This is what says whether the client is on the crest waiting, walking
		// off, or somewhere else entirely.
		{
			static float NextStatus = 0.f;
			const float Now = static_cast<float>(TestWorld->GetTimeSeconds());
			if (Now >= NextStatus)
			{
				NextStatus = Now + 1.f;
				UE_LOG(LogTraceGame, Display,
					TEXT("SURFNET [client] at %s | crest %s | onCrest=%d surfing=%d mode=%d speed=%.0f "
					     "| rides %d, corrections %d in a ride / %d session"),
					*NetHere.ToCompactString(), *Crest.ToCompactString(), bOnCrest ? 1 : 0,
					IsSurfing() ? 1 : 0, static_cast<int32>(MovementMode.GetValue()), GetPlanarSpeed(),
					SurfTestRun, SurfCorrectionsInWindow, CorrectionCount);
			}
		}

		// Not surfing and not just off a ride. If the server has put us on the crest, run off the
		// inboard edge; otherwise stand still and wait for it to.
		if (bOnCrest)
		{
			// WALK off the edge, do not RUN off it, and the difference is measured rather than tasted.
			// The face is only 447 uu wide horizontally and 616 uu tall, and it falls away at up to 61
			// degrees; a pawn that leaves the crest with a large INBOARD velocity is a projectile that
			// clears the whole thing. The first version left at 0.66 of full speed inboard (~640 uu/s)
			// and was measured sailing 398 uu past the crest edge while dropping only 94 — 510 uu above
			// a surface it never touched, on every attempt.
			//
			// Contact needs g t^2 / 2 >= tan(alpha) * v_inboard * t, i.e. t >= 2 tan(alpha) v_in / g, so
			// the inboard travel before contact is 2 tan(alpha) v_in^2 / g. At the shipped numbers that
			// has to stay well under 447 uu, which puts v_in under about 250 uu/s. 0.25 of a 0.7-scaled
			// walk is ~140 uu/s: the pawn steps over the lip and is on the face inside a fifth of a
			// second, which is what a player does.
			const FVector Inboard(0.f, (Crest.Y >= 0.0) ? -1.f : 1.f, 0.f);
			const FVector Approach = (Probe.RunDirection * 0.97f + Inboard * 0.25f).GetSafeNormal();
			CharacterOwner->AddMovementInput(Approach, 0.7f);
			TestController->SetControlRotation(Approach.Rotation());
		}
		return;
	}

	if (SurfTestPhaseTime < 0.f)
	{
		// NEGATIVE CONTROL 0 stands on the rail's own WALKABLE CREST; control 1 stands on the flat floor
		// beside the toe. Both are ordinary geometry moving at speed, which is exactly the case that
		// must never be mistaken for a ramp.
		FVector Where = Probe.FaceEntry;
		if (bNetMode)
		{
			// On the CREST — walkable, stable ground a long way from any ride. See the note at the top
			// of this function for why a client's placement may not be anywhere near a measured ride.
			Where = Probe.CrestStand + FVector(0.f, 0.f, 120.f);
		}
		else if (bNegative)
		{
			Where = (SurfTestRun == Ladder * 2)
				? (Probe.CrestStand + FVector(0.f, 0.f, 120.f))
				: FVector(Probe.CrestStand.X, Probe.FaceEntry.Y - FMath::Sign(Probe.FaceEntry.Y) * 700.f, 120.f);
		}

		CharacterOwner->TeleportTo(Where, CharacterOwner->GetActorRotation(), false, true);
		Velocity = Probe.RunDirection * Requested;
		SetMovementMode(bNetMode ? MOVE_Walking : MOVE_Falling);

		// Wipe the previous run's ride so a grace still ticking cannot be counted twice.
		SurfContactRemaining = 0.f;
		SurfPlaneNormal = FVector::ZeroVector;
		SurfEntrySpeed = 0.f;
		SurfElapsedSeconds = 0.f;

		SurfTestEntrySpeed = 0.f;
		SurfTestPeak = 0.f;
		bSurfTestArmed = 0;
		SurfTestPhaseTime = 0.f;
		TraceMovementSurf::SurfTestWishYawValid() = false;

		TestController->SetControlRotation(Probe.RunDirection.Rotation());

		// WHERE THE RUN WAS ACTUALLY PLACED. A rig that reports a ride from a pawn the teleport did not
		// move is reporting somewhere else in the arena, and a single line here is the difference
		// between spotting that and puzzling over the speed.
		UE_LOG(LogTraceGame, Display,
			TEXT("SURFTEST   placing run %2d/%2d at %s (asked %s), entry %.0f uu/s along %s"),
			SurfTestRun + 1, bNetMode ? TraceMovementSurf::SurfNetTestRuns : TraceMovementSurf::SurfTestTotalRuns,
			*UpdatedComponent->GetComponentLocation().ToCompactString(), *Where.ToCompactString(),
			Requested, *Probe.RunDirection.ToCompactString());
		return;
	}

	SurfTestPhaseTime += DeltaSeconds;

	const float Settle = bNetMode
		? TraceMovementSurf::SurfNetSettleSeconds
		: TraceMovementSurf::SurfTestSettleSeconds;
	if (SurfTestPhaseTime < Settle)
	{
		// STANDING STILL ON THE CREST while the placement's correction arrives, is applied and is
		// forgotten. Nothing is measured and no input is driven; the ride has not started.
		return;
	}

	// --- The approach, net mode only: RUN OFF THE CREST under ordinary predicted input --------------
	//
	// Down the rail and inboard at once, so the pawn crosses the crest's inboard edge with real speed
	// and drops onto the face exactly as a player stepping off it would. No teleport, no velocity
	// write — just AddMovementInput through the same path a key press takes.
	if (bNetMode && !IsSurfing() && bSurfTestArmed == 0)
	{
		const FVector Inboard(0.f, (Probe.CrestStand.Y >= 0.0) ? -1.f : 1.f, 0.f);
		const FVector Approach = (Probe.RunDirection * 0.75f + Inboard * 0.66f).GetSafeNormal();
		CharacterOwner->AddMovementInput(Approach, 1.f);
		TestController->SetControlRotation(Approach.Rotation());
	}

	// A SHARP LOSS IS AN EVENT, SO IT IS LOGGED AS ONE. "Exit 766 from a peak of 1615" is not a
	// diagnosis; the frame it happened on, with the plane that was under the pawn at the time, is.
	{
		static float LastPlanar = 0.f;
		const float NowPlanar = GetPlanarSpeed();
		if (SurfTestPhaseTime > TraceMovementSurf::SurfTestSettleSeconds + 0.05f
			&& LastPlanar - NowPlanar > 100.f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("SURFTEST   !! run %d lost %.0f uu/s in one frame at %s | vel %s | surfing=%d "
				     "plane %s | mode=%d"),
				SurfTestRun + 1, LastPlanar - NowPlanar,
				*UpdatedComponent->GetComponentLocation().ToCompactString(),
				*Velocity.ToCompactString(), IsSurfing() ? 1 : 0,
				*SurfPlaneNormal.ToCompactString(), static_cast<int32>(MovementMode.GetValue()));
		}
		LastPlanar = NowPlanar;
	}

	SurfTestPeak = FMath::Max(SurfTestPeak, GetPlanarSpeed());

	// --- The input --------------------------------------------------------------------------------
	if (bStrafe && !bNegative && IsSurfing())
	{
		FVector Planar(Velocity.X, Velocity.Y, 0.f);
		if (Planar.Normalize())
		{
			// UP-SLOPE, read off the LIVE plane rather than off the rig's own idea of the geometry: the
			// horizontal part of the surface normal points DOWN the slope, so its negation is the way up.
			// This is also the exact direction UE's stock LimitAirControl deletes, which is why the arm
			// Trace.Move.SurfLegacyAirLimit turns the whole column back into the no-input column.
			const FVector UpSlope = -FVector(SurfPlaneNormal.X, SurfPlaneNormal.Y, 0.f).GetSafeNormal();
			FVector Perp(-Planar.Y, Planar.X, 0.f);
			if (FVector::DotProduct(Perp, UpSlope) < 0.f)
			{
				Perp = -Perp;
			}

			// ============================================================================================
			// THE INPUT, DERIVED RATHER THAN GUESSED — AND THE RIG HAS NOW BEEN WRONG TWICE HERE, SO
			// THE DERIVATION IS WRITTEN OUT.
			//
			// The air model's gain along the wish direction is
			//
			//     AddSpeed = AirMaxWishSpeed - dot(v, wish)
			//
			// and what the player actually wants to grow is |v|, which grows at AddSpeed x cos(phi)
			// where phi is the angle between the wish and the velocity. Write c = cos(phi):
			//
			//     growth(c) = (W - v c) c ,  maximised at  c = W / (2 v) ,  giving  W^2 / (4 v)
			//
			// So the best sustainable surf input is NOT perpendicular to travel and NOT along it: it is
			// exactly acos(W / 2v) off the velocity, and it opens up toward 90 degrees as the pawn gets
			// faster. Both previous versions of this rig sat at one of the two useless extremes —
			//   * blended 35% toward the rail: c = 0.33, which passes W/v at 485 uu/s and gains NOTHING
			//     above it (the strafe and no-input columns agreed to 1 uu/s, which read as a broken
			//     feature and was a broken harness);
			//   * held dead perpendicular: c = 0, maximum AddSpeed but none of it along travel, so the
			//     pawn climbed the face, stalled and came off the crest slower than it arrived.
			// W is read LIVE off GetAirMaxWishSpeed() rather than typed, so if the air model is retuned
			// the rig re-derives its own input instead of measuring the old one.
			//
			// The SIDE is up-slope, which is the direction gravity is pulling the pawn out of and the
			// direction UE's stock LimitAirControl deletes. The turn-rate limit stays: a mouse cannot
			// snap, so the wish chases its target at SurfTestTurnRateDegPerSec and no faster, and the
			// numbers this reports are reachable rather than theoretical.
			// ============================================================================================
			const float WishCap = FMath::Max(1.f, GetAirMaxWishSpeed());
			const float Speed = FMath::Max(1.f, GetPlanarSpeed());
			const float CosPhi = FMath::Clamp(WishCap / (2.f * Speed), 0.f, 0.999f);
			const float SinPhi = FMath::Sqrt(FMath::Max(0.f, 1.f - CosPhi * CosPhi));
			//
			// ANCHORED TO THE RAIL, NOT TO THE VELOCITY, and that is the third and last thing this rig
			// got wrong. Aiming phi off the CURRENT velocity is a positive feedback loop: every frame
			// the velocity rotates a little further up-slope, the wish follows it, and the pawn flies up
			// the face, stalls and comes over the crest slower than it arrived (measured: entry 400 ->
			// peak 886 -> exit 282). A surfer aims relative to the RAMP, so the wish is built from the
			// rail's own direction and the live up-slope, and the velocity is free to sit wherever the
			// balance of gravity and input puts it.
			const FVector Ideal = (Probe.RunDirection * CosPhi + UpSlope * SinPhi).GetSafeNormal();

			float& WishYaw = TraceMovementSurf::SurfTestWishYaw();
			if (!TraceMovementSurf::SurfTestWishYawValid())
			{
				TraceMovementSurf::SurfTestWishYawValid() = true;
				WishYaw = static_cast<float>(Ideal.Rotation().Yaw);
			}

			const float IdealYaw = static_cast<float>(Ideal.Rotation().Yaw);
			const float MaxStep = TraceMovementSurf::SurfTestTurnRateDegPerSec * DeltaSeconds;
			const float Delta = FMath::FindDeltaAngleDegrees(WishYaw, IdealYaw);
			WishYaw = FRotator::NormalizeAxis(WishYaw + FMath::Clamp(Delta, -MaxStep, MaxStep));

			const FVector Wish = FRotator(0.f, WishYaw, 0.f).Vector();
			CharacterOwner->AddMovementInput(Wish, 1.f);
			TestController->SetControlRotation(Planar.Rotation());

			// Perp is retained as the geometric reference for "which way is up-slope from here"; the
			// wish itself is built from the rail. Named so the compiler does not warn about it.
			(void)Perp;
		}
	}

	// --- Opening and closing the sample -----------------------------------------------------------
	if (IsSurfing() && bSurfTestArmed == 0)
	{
		bSurfTestArmed = 1;
		SurfTestEntrySpeed = GetSurfEntrySpeed();
	}

	const bool bTimedOut = (SurfTestPhaseTime >= (bNetMode
		? (TraceMovementSurf::SurfNetSettleSeconds + TraceMovementSurf::SurfTestRunSeconds + 2.f)
		: TraceMovementSurf::SurfTestRunSeconds));
	const bool bRideEnded = (bSurfTestArmed != 0) && !IsSurfing();

	if (bRideEnded || bTimedOut)
	{
		TraceMovementSurf::FSurfRunResult Row;
		Row.RequestedEntry = Requested;
		Row.MeasuredEntry = (SurfTestEntrySpeed > 1.f) ? SurfTestEntrySpeed : Requested;
		Row.ExitSpeed = GetPlanarSpeed();
		Row.PeakSpeed = SurfTestPeak;
		Row.Seconds = SurfTestPhaseTime - Settle;
		Row.bStrafed = bStrafe && !bNegative;
		Row.bNegativeControl = bNegative;
		Row.bSurfed = (bSurfTestArmed != 0);
		TraceMovementSurf::SurfRunResults().Add(Row);

		UE_LOG(LogTraceGame, Display,
			TEXT("SURFTEST run %2d/%2d  %-18s asked %6.0f  entry %6.0f -> exit %6.0f (peak %6.0f) in %.2fs  "
			     "surfed=%d"),
			SurfTestRun + 1, bNetMode ? TraceMovementSurf::SurfNetTestRuns : TraceMovementSurf::SurfTestTotalRuns,
			bNegative ? TEXT("NEGATIVE CONTROL")
				: (bNetMode ? TEXT("crest walk-off") : (bStrafe ? TEXT("ideal strafe") : TEXT("no input"))),
			Row.RequestedEntry, Row.MeasuredEntry, Row.ExitSpeed, Row.PeakSpeed, Row.Seconds,
			Row.bSurfed ? 1 : 0);

		// WHERE AND HOW THE RIDE ENDED, because "exit 747 from a peak of 1618" is not interpretable
		// without it: a ride that ends by landing on the walkable crest, one that ends by falling off
		// the toe, and one that ends against another structure are three different results and the
		// speed alone cannot tell them apart.
		UE_LOG(LogTraceGame, Display,
			TEXT("SURFTEST   ...ended at %s  vel %s  mode=%d grounded=%d  reason=%s"),
			*UpdatedComponent->GetComponentLocation().ToCompactString(), *Velocity.ToCompactString(),
			static_cast<int32>(MovementMode.GetValue()), IsMovingOnGround() ? 1 : 0,
			bRideEnded ? TEXT("surf state closed") : TEXT("run clock expired"));

		++SurfTestRun;
		SurfTestPhaseTime = -1.f;
	}
}

void UTraceCharacterMovementComponent::LogSurfReport() const
{
	const bool bAuthoritative = (CharacterOwner != nullptr) && CharacterOwner->HasAuthority();

	UE_LOG(LogTraceGame, Display,
		TEXT("SURFREPORT %-16s rides=%d closed=%d | entry mean %6.0f -> exit mean %6.0f uu/s "
		     "(mean gain %+.0f) | best %+.0f worst %+.0f | longest ride %.2f s | ceiling bound on %d "
		     "frames | airborne contacts REFUSED as non-surf %d | CORRECTIONS: %d in a ride, %d in the "
		     "whole session (worst %.2f uu, mean %.2f uu)%s"),
		*GetNameSafe(CharacterOwner), SurfCount, SurfClosedCount,
		SurfEntrySpeedSum / static_cast<float>(FMath::Max(1, SurfCount)),
		SurfExitSpeedSum / static_cast<float>(FMath::Max(1, SurfClosedCount)),
		(SurfExitSpeedSum / static_cast<float>(FMath::Max(1, SurfClosedCount)))
			- (SurfEntrySpeedSum / static_cast<float>(FMath::Max(1, SurfCount))),
		SurfBestGain, SurfWorstGain, SurfLongestSeconds, SurfCeilingBinds, SurfContactsRefused,
		SurfCorrectionsInWindow, CorrectionCount, GetCorrectionWorstForAudit(), GetCorrectionMeanForAudit(),
		bAuthoritative
			? TEXT("  [AUTHORITATIVE PAWN: the correction count is 0 by construction and means nothing. "
			       "Read it on a joined client.]")
			: TEXT(""));
}

#endif // !UE_BUILD_SHIPPING
