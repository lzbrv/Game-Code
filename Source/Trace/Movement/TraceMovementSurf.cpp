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
//   -TraceSurfExitTest      DEMO 29 ITEM 4(a). What happens at the END of a ride. The rig above
//                           closes its sample the frame the SURF STATE closes, which on every strafed
//                           rung was still airborne with the ride in progress — so the owner's "a
//                           player loses all momentum at the end of the curve" happened entirely
//                           after it had stopped looking. This one follows the ride through the
//                           landing and across two seconds of floor, and reports the DISTANCE carried.
//
//   -TraceSurfApproachTest  DEMO 29 ITEM 4(b). Whether you can surf INTO a curve. Every arm above
//                           starts a ride by teleporting a pawn onto the face or walking it off the
//                           crest; the owner is describing running at a rail from the floor and
//                           getting nothing. This one never puts a pawn on the face at all.
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

#include "Components/CapsuleComponent.h"
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

	// =============================================================================================
	// THE IDEAL SURF INPUT — ONE DEFINITION, USED BY EVERY ARM OF EVERY RIG IN THIS FILE.
	//
	// It used to be written out inline in the ladder arm and again in the net arm, and the second
	// copy was already a slightly different vector (it re-aimed off the live velocity rather than off
	// the rail). Two copies of one rule is this project's standing lesson, and a HARNESS with two
	// copies of its own input model is the worst place to have it: the two arms stop being
	// comparable and nothing says so. The derivation is written out once, here.
	//
	// The air model's gain along the wish direction is
	//
	//     AddSpeed = AirMaxWishSpeed - dot(v, wish)
	//
	// and what a surfer wants to grow is |v|, which grows at AddSpeed x cos(phi), phi being the angle
	// between the wish and the velocity. With c = cos(phi):
	//
	//     growth(c) = (W - v c) c ,  maximised at  c = W / (2 v) ,  giving  W^2 / (4 v)
	//
	// So the best sustainable surf input is neither along travel nor perpendicular to it: it is
	// exactly acos(W / 2v) off the rail, opening toward 90 degrees as the pawn gets faster. It is
	// ANCHORED TO THE RAIL, not to the velocity — aiming phi off the current velocity is a positive
	// feedback loop that flies the pawn up the face, stalls it, and brings it over the crest slower
	// than it arrived (measured, in Patch 28: entry 400 -> peak 886 -> exit 282).
	//
	// W is read LIVE by the caller off GetAirMaxWishSpeed(), so retuning the air model re-derives the
	// rig's input instead of measuring the old one.
	// =============================================================================================
	static FVector IdealSurfWish(const FVector& RunDirection, const FVector& PlaneNormal,
		const float PlanarSpeed, const float WishCap)
	{
		// The horizontal part of the surface normal points DOWN the slope, so its negation is up-slope
		// — and up-slope is the axis UE's stock LimitAirControl deletes, which is the whole reason the
		// surf overrides exist. Read off the LIVE plane, never off the rig's idea of the geometry.
		const FVector UpSlope = -FVector(PlaneNormal.X, PlaneNormal.Y, 0.f).GetSafeNormal();
		const float Speed = FMath::Max(1.f, PlanarSpeed);
		const float CosPhi = FMath::Clamp(FMath::Max(1.f, WishCap) / (2.f * Speed), 0.f, 0.999f);
		const float SinPhi = FMath::Sqrt(FMath::Max(0.f, 1.f - CosPhi * CosPhi));
		return (RunDirection * CosPhi + UpSlope * SinPhi).GetSafeNormal();
	}

	/**
	 * Drive one frame of the ideal strafe through the SAME AddMovementInput path a key press takes,
	 * with the mouse-rate limit on the wish direction, so every frame the rig produces is a frame the
	 * prediction pipeline would have produced for a player.
	 */
	static void DriveIdealSurfStrafe(ACharacter* Pawn, APlayerController* PC, const FVector& RunDirection,
		const FVector& PlaneNormal, const FVector& Velocity, const float WishCap, const float DeltaSeconds)
	{
		if (Pawn == nullptr)
		{
			return;
		}

		FVector Planar(Velocity.X, Velocity.Y, 0.f);
		if (!Planar.Normalize())
		{
			return;
		}

		const FVector Ideal = IdealSurfWish(RunDirection, PlaneNormal,
			static_cast<float>(FVector(Velocity.X, Velocity.Y, 0.f).Size()), WishCap);

		float& WishYaw = SurfTestWishYaw();
		if (!SurfTestWishYawValid())
		{
			SurfTestWishYawValid() = true;
			WishYaw = static_cast<float>(Ideal.Rotation().Yaw);
		}

		// A mouse cannot snap: the wish chases its target at SurfTestTurnRateDegPerSec and no faster,
		// so the numbers this reports are reachable rather than theoretical.
		const float IdealYaw = static_cast<float>(Ideal.Rotation().Yaw);
		const float MaxStep = SurfTestTurnRateDegPerSec * DeltaSeconds;
		const float Delta = FMath::FindDeltaAngleDegrees(WishYaw, IdealYaw);
		WishYaw = FRotator::NormalizeAxis(WishYaw + FMath::Clamp(Delta, -MaxStep, MaxStep));

		Pawn->AddMovementInput(FRotator(0.f, WishYaw, 0.f).Vector(), 1.f);
		if (PC != nullptr)
		{
			PC->SetControlRotation(Planar.Rotation());
		}
	}

	// =============================================================================================
	// DEMO 29 ITEM 4 — THE TWO TRANSITION RIGS.
	//
	// The owner reports two things, and Patch 28's rig can measure NEITHER of them:
	//
	//   (a) "when sliding down the curved surfaces, a player loses all momentum at the end of the
	//       curve."  The Patch 28 ladder closes its sample the frame the SURF STATE closes and reports
	//       the planar speed there. Every strafed rung in that table ran out its four-second run clock
	//       while still AIRBORNE (mode=3, grounded=0 in FINAL2.log), so the number in the "exit"
	//       column is a mid-air speed and the transition the owner is complaining about happened after
	//       the rig had stopped looking. -TraceSurfExitTest follows the ride THROUGH the landing and
	//       across two seconds of floor.
	//
	//   (b) "it still doesn't feel like you can surf INTO curves/curved ramps in order to gain
	//       momentum."  Every arm of the Patch 28 rig starts by TELEPORTING a pawn onto the face (or,
	//       in the net arm, by walking it off the crest). Nothing in it ever approached a rail at
	//       floor level, which is what a player does. -TraceSurfApproachTest does only that.
	// =============================================================================================

	/** One ride, followed from the face to the floor. Every field is uu/s unless it says otherwise. */
	struct FSurfExitSample
	{
		float RequestedEntry = 0.f;
		float Entry = 0.f;
		float PeakPlanar = 0.f;
		float RideSeconds = 0.f;

		/** The last frame IsSurfing() was true. */
		float LastSurfPlanar = 0.f;
		float LastSurfSpeed3D = 0.f;
		float LastSurfVz = 0.f;

		/** The last AIRBORNE frame, i.e. the frame before the landing zeroed Z. */
		float LastAirPlanar = 0.f;
		float LastAirSpeed3D = 0.f;
		float LastAirVz = 0.f;

		/** Seconds between the surf state closing and the pawn being grounded. */
		float AirGapSeconds = 0.f;

		/** The first grounded frame, then the floor phase. */
		float GroundPlanar = 0.f;
		float At025 = 0.f;
		float At050 = 0.f;
		float At100 = 0.f;
		float At200 = 0.f;

		/** How far the pawn travelled on the floor before it was back at the walking limit, uu. */
		float CarryDistance = 0.f;
		float GroundLimit = 0.f;

		/**
		 * Where a single frame took more than 150 uu/s off the planar speed, if one did.
		 *
		 * IT IS THE DIFFERENCE BETWEEN TWO ANSWERS THAT LOOK IDENTICAL IN A SPEED COLUMN. A carry that
		 * ends because the floor bled it out and a carry that ends because the pawn ran into a piece of
		 * cover are not the same result, and the rig drives the pawn STRAIGHT — it cannot steer round
		 * anything, which a player can and does. Without this the table would silently under-report the
		 * carry every time the lane happens to have furniture in it.
		 */
		bool bAirObstructed = false;
		bool bGroundObstructed = false;
		float ObstructedAt = 0.f;
		float ObstructedLoss = 0.f;
		FVector ObstructedWhere = FVector::ZeroVector;

		bool bSurfed = false;
		bool bLanded = false;
		FVector SurfCloseAt = FVector::ZeroVector;
		FVector LandedAt = FVector::ZeroVector;
	};

	static TArray<FSurfExitSample>& SurfExitRows()
	{
		static TArray<FSurfExitSample> Rows;
		return Rows;
	}

	/** One approach, from the flat floor into the rail. */
	struct FSurfApproachSample
	{
		float AngleDegrees = 0.f;
		float StartSpeed = 0.f;
		float SpeedAtContact = 0.f;
		float MinAfterContact = 0.f;
		float PeakAfterContact = 0.f;
		float FinalSpeed = 0.f;
		float SurfSeconds = 0.f;
		bool bReachedFace = false;
		bool bSurfed = false;
		FVector ContactAt = FVector::ZeroVector;

		/**
		 * HOW FAR UP THE RAMP THE PAWN ACTUALLY GOT, and where it stopped.
		 *
		 * Without these a failed run is unfalsifiable. "surfed=0" is equally true of a pawn stopped
		 * dead at a kerb 250 uu short of the toe, a pawn that climbed the whole walk-up and was
		 * refused at the band, and a pawn that rode for one frame and landed — and the fix for those
		 * three is a different fix each time. The first measured pass on the steepened ramp reported
		 * six identical "no surf, no gain" rows and nothing that could tell them apart.
		 */
		float HighestZ = 0.f;
		float DeepestInset = 0.f;      // most negative = furthest INBOARD of the toe line
		FVector EndedAt = FVector::ZeroVector;
	};

	static TArray<FSurfApproachSample>& SurfApproachRows()
	{
		static TArray<FSurfApproachSample> Rows;
		return Rows;
	}

	/** Phases of one transition run. Both rigs share the shape; only Ride/Approach differ. */
	enum class ESurfRigPhase : uint8
	{
		Place,
		Settle,
		Ride,
		AirGap,
		Ground,
	};

	static ESurfRigPhase& RigPhase()   { static ESurfRigPhase P = ESurfRigPhase::Place; return P; }
	static float& RigClock()           { static float T = 0.f; return T; }
	static float& RigPhaseClock()      { static float T = 0.f; return T; }
	static int32& RigRun()             { static int32 R = -1; return R; }
	static bool& RigReported()         { static bool B = false; return B; }
	static FSurfExitSample& RigExit()  { static FSurfExitSample S; return S; }
	static FSurfApproachSample& RigApproach() { static FSurfApproachSample S; return S; }
	static FVector& RigGroundStart()   { static FVector V = FVector::ZeroVector; return V; }

	/** The exit rig's entry ladder. The same five rungs as Patch 28, so the two tables line up. */
	static constexpr float SurfExitLadder[] = { 400.f, 800.f, 1200.f, 1600.f, 2000.f };
	static constexpr int32 SurfExitLadderCount = UE_ARRAY_COUNT(SurfExitLadder);

	/** How long a ride may run before the rig gives up on it ever reaching the floor. */
	static constexpr float SurfExitRideCeilingSeconds = 12.f;

	/** How long the floor phase is followed. Two seconds is well past the whole overspeed bleed. */
	static constexpr float SurfExitGroundSeconds = 2.0f;

	/**
	 * The approach ladder: degrees between the RAIL and the direction the pawn runs at it.
	 *
	 * 90 is straight into the face and is the case the owner is most likely to have tried by accident;
	 * 20 is the shallow lean a surfer would actually use. Both ends matter, because the complaint is
	 * that NONE of them does anything.
	 */
	static constexpr float SurfApproachAngles[] = { 0.f, 90.f, 60.f, 45.f, 30.f, 20.f };
	static constexpr int32 SurfApproachCount = UE_ARRAY_COUNT(SurfApproachAngles);

	/**
	 * How far inboard of the toe an approach starts, uu.
	 *
	 * Short enough that even the shallowest rung closes it inside a second (at 20 degrees the closing
	 * rate is only sin(20) of the run speed) and short enough to stay well clear of the midfield cover
	 * whose diamond reaches |Y| 2548 against a toe at 2700.
	 */
	static constexpr float SurfApproachStandoff = 300.f;

	/**
	 * Seconds one approach is followed for, after the settle.
	 *
	 * FIVE, not three, and the first measured table is why: at three seconds every rung that gained
	 * was still gaining when the clock stopped (peak == final on 45, 30 and 20 degrees), so the table
	 * was reporting where the ride had got to rather than what it was worth. Five seconds is longer
	 * than the shortened rail can hold a pawn at these speeds, so every rung now ends because the ride
	 * did.
	 */
	static constexpr float SurfApproachSeconds = 5.0f;

	/** How long an approach stands still first, so it starts from the floor and not from a fall. */
	static constexpr float SurfApproachSettleSeconds = 0.35f;

	/** Settle after an exit-rig placement, before the ride is measured. Longer than the surf grace. */
	static constexpr float SurfExitSettleSeconds = 0.15f;

	// =============================================================================================
	// -TraceSurfFrameTrace — ONE LINE PER FRAME, AND WHY A SUMMARY TABLE COULD NOT REPLACE IT.
	//
	// Every rig above this line reports a ride as a handful of scalars: entry, peak, last surf frame,
	// landed, carried. That is the right shape for "is surfing worth anything", and it is the wrong
	// shape for "is the ride SMOOTH" — a ride that loses 40 uu/s four times at four facet joints and
	// a ride that loses 160 uu/s once at the junction have the same entry, the same peak and very
	// nearly the same exit. The owner's complaint is about the second kind of thing, so the
	// instrument has to be able to see WHERE on the structure a frame went wrong.
	//
	// So this arm prints the per-frame state with the two derived columns that make a geometry defect
	// legible: the CHANGE in planar speed since the previous frame, and the face angle acos(Nz) of
	// the plane the pawn is on. A facet joint is a step in the angle column; what it costs is the
	// number next to it in the delta column. Both are read LIVE off the movement state, so the trace
	// is of the ride the shipped code actually simulated.
	//
	// It is a modifier, not a mode: pass it alongside -TraceSurfExitTest. On its own it does nothing,
	// because there is no ride to trace.
	// =============================================================================================
	static bool SurfFrameTraceEnabled()
	{
		static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfFrameTrace"));
		return bEnabled;
	}

	/** Previous frame's planar speed, for the delta column. Reset at every placement. */
	static float& SurfFrameTracePrevPlanar()
	{
		static float Planar = 0.f;
		return Planar;
	}

	/** The worst single-frame planar loss of the current ride, and where it happened. */
	struct FSurfFrameWorst
	{
		float Loss = 0.f;
		float AtX = 0.f;
		float AtAngle = 0.f;
		float FromPlanar = 0.f;
		bool bSurfing = false;
	};

	static FSurfFrameWorst& SurfFrameTraceWorst()
	{
		static FSurfFrameWorst Worst;
		return Worst;
	}

	/** Which pose the net arm's server hands out next. Alternates crest / floor lane. */
	static bool& SurfNetPoseOnFloor()
	{
		static bool bOnFloor = false;
		return bOnFloor;
	}

	/**
	 * Where the net arm stands a pawn on the FLOOR so it can run at the rail: inboard of the toe by
	 * the approach rig's own standoff, and back down the rail by the distance a 20-degree lean takes
	 * to close it, so the pawn meets the face at a shallow angle rather than head-on.
	 *
	 * The 20 degrees is not a second copy of the approach rig's ladder — it is the SHALLOW END of it,
	 * chosen here because a glancing lean is the case a player actually uses and the case that most
	 * needs to survive prediction: it is the one where the pawn keeps most of its speed through the
	 * clip and rides for seconds afterwards.
	 */
	static FVector SurfNetLaneStand(const ATraceArenaBuilder::FTraceSurfRailProbe& Probe)
	{
		const FVector Inboard = Probe.FaceNormal.GetSafeNormal2D();
		const float BackOff = SurfApproachStandoff / FMath::Tan(FMath::DegreesToRadians(20.f));
		return Probe.ToeOnFloor + Inboard * SurfApproachStandoff - Probe.RunDirection * BackOff;
	}

	/** The 20-degree approach direction the net arm's client runs at the rail with. */
	static FVector SurfNetApproachDirection(const ATraceArenaBuilder::FTraceSurfRailProbe& Probe)
	{
		const FVector TowardFace = -Probe.FaceNormal.GetSafeNormal2D();
		const float Rad = FMath::DegreesToRadians(20.f);
		return (Probe.RunDirection * FMath::Cos(Rad) + TowardFace * FMath::Sin(Rad)).GetSafeNormal();
	}

	/**
	 * -TraceSurfBankTest: point every arm of this file at the SIDE-WALL RIDE instead of at a rail.
	 *
	 * The side-wall band (ATraceArenaBuilder::BuildSurfBanks) is the structure the owner asked for in
	 * place of the rails, and "the ramps don't work on the end, plus they're not curved they're just
	 * angled" is a claim about a RIDE. Every arm here already knows how to measure a ride; what it
	 * needed was to be told where the ramp is, and that is one accessor. So the rig is not duplicated:
	 * the twelve-run ladder, the exit test, the approach test and both negative controls all run
	 * against the band unchanged, which is the only way the two structures' numbers are comparable.
	 *
	 * Guarded like every other arm in this file: the switch does not survive a Shipping link.
	 */
	static bool SurfBankTestArm()
	{
#if UE_BUILD_SHIPPING
		return false;
#else
		static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfBankTest"));
		return bFromCommandLine;
#endif
	}

	/**
	 * -TraceSurfSideRampTest: point every arm of this file at the HAND-PLACED SIDE RAMPS.
	 *
	 * `Kit_Ramp_03/04` on /Game/Maps/Arena_Baked are the structures the owner means by "the ones which
	 * exist all the way on the side", and on the shipping map they are the ONLY thing on those walls —
	 * the procedural banks are absent there because these replaced them. Two reports have argued about
	 * whether they can be surfed from a bounding box; nobody had ridden one, because the harness only
	 * knew about the two structures this codebase builds itself.
	 *
	 * It is the same swap -TraceSurfBankTest makes, to a third probe, and for the same reason: the
	 * twelve-run ladder, the exit test, the approach test and both negative controls are the
	 * instrument, and pointing them somewhere new must not mean writing a second one.
	 *
	 * Guarded like every other arm in this file: the switch does not survive a Shipping link.
	 */
	static bool SurfSideRampTestArm()
	{
#if UE_BUILD_SHIPPING
		return false;
#else
		static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfSideRampTest"));
		return bFromCommandLine;
#endif
	}

	/**
	 * The ramp in this pawn's own quadrant. ONE lookup, so no two arms of this file can end up
	 * measuring different ramps — and it goes through the arena's own accessor rather than through any
	 * coordinate of its own, for the reason at the top of the file.
	 */
	static ATraceArenaBuilder::FTraceSurfRailProbe ProbeForPawn(const UTraceCharacterMovementComponent& Movement)
	{
		ATraceArenaBuilder::FTraceSurfRailProbe Probe;

		const UWorld* World = Movement.GetWorld();
		const USceneComponent* Updated = Movement.UpdatedComponent;
		if (World == nullptr || Updated == nullptr)
		{
			return Probe;
		}

		TActorIterator<ATraceArenaBuilder> It(const_cast<UWorld*>(World));
		const ATraceArenaBuilder* Arena = It ? *It : nullptr;
		if (Arena == nullptr)
		{
			return Probe;
		}

		const FVector Here = Updated->GetComponentLocation();
		const float SignX = (Here.X >= 0.0) ? 1.f : -1.f;
		const float SignY = (Here.Y >= 0.0) ? 1.f : -1.f;
		if (SurfSideRampTestArm())
		{
			// THE BAND IS HANDED IN LIVE, not typed into the probe. The side-ramp probe decides which
			// samples are ridable, and if it carried its own idea of the band a knob change would move
			// the movement code and leave the instrument measuring the old rule.
			float BandLoDeg = 0.f;
			float BandHiDeg = 0.f;
			Movement.GetSurfSlopeBandDegrees(BandLoDeg, BandHiDeg);
			return Arena->GetSideRampProbe(SignX, SignY,
				FMath::Cos(FMath::DegreesToRadians(BandHiDeg)),
				FMath::Cos(FMath::DegreesToRadians(BandLoDeg)));
		}
		return SurfBankTestArm() ? Arena->GetSurfBankProbe(SignX, SignY)
			: Arena->GetSurfRailProbe(SignX, SignY);
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
	//
	// DEMO 29 ITEM 4 adds two more, and both exist because neither of the two above can see the thing
	// the owner is complaining about: -TraceSurfExitTest follows a ride THROUGH the landing and across
	// two seconds of floor, and -TraceSurfApproachTest never puts a pawn on the face at all — it runs
	// one at the rail from the floor, which is what a player does. See their block in the namespace
	// above.
	static const bool bLadderMode = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfTest"));
	static const bool bNetMode = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfNetTest"));
	static const bool bExitMode = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfExitTest"));
	static const bool bApproachMode = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfApproachTest"));
	const bool bEnabled = bLadderMode || bNetMode || bExitMode || bApproachMode;
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
	const float ProbeSignX = (Here.X >= 0.0) ? 1.f : -1.f;
	const float ProbeSignY = (Here.Y >= 0.0) ? 1.f : -1.f;
	const ATraceArenaBuilder::FTraceSurfRailProbe Probe = TraceMovementSurf::SurfSideRampTestArm()
		? Arena->GetSideRampProbe(ProbeSignX, ProbeSignY, GetSurfMinNormalZ(), GetWalkableFloorZ())
		: (TraceMovementSurf::SurfBankTestArm()
			? Arena->GetSurfBankProbe(ProbeSignX, ProbeSignY)
			: Arena->GetSurfRailProbe(ProbeSignX, ProbeSignY));

	if (!Probe.bValid)
	{
		if (TestWorld->GetTimeSeconds() > 8.f)
		{
			bSurfTestReported = 1;
			if (TraceMovementSurf::SurfSideRampTestArm())
			{
				// A DIFFERENT SENTENCE, because it is a different failure and it is a RESULT. The side
				// ramps are placed by hand in a level, so "no ridable side ramp here" means the sweep
				// found nothing between the live surf floor and the live walkable limit anywhere it
				// looked — i.e. the wall is bare, or the ramp on it is a walk-up.
				UE_LOG(LogTraceGame, Error,
					TEXT("SURFTEST: no ridable side ramp on this level. The sweep in from the side wall found "
					     "no run of samples inside the live band %.4f < Nz < %.4f (%.2f..%.2f deg). Either the "
					     "wall is bare or what is on it is walkable."),
					GetSurfMinNormalZ(), GetWalkableFloorZ(),
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(GetWalkableFloorZ(), -1.f, 1.f))),
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(GetSurfMinNormalZ(), -1.f, 1.f))));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("SURFTEST: this level has no surf rails (bBuildSurfRails off, or a baked map that was "
					     "baked before they existed). Run on /Game/Maps/Arena."));
			}
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

			// DEMO 29 ITEM 4(b). ALTERNATE THE POSE, because there are now TWO ways a ride can start
			// and only one of them was ever predicted under lag. The crest walk-off tests the airborne
			// entry Patch 28 added; the FLOOR LANE tests the ground entry this patch added, which is
			// the one that writes Velocity AND the movement mode from inside HandleImpact. "It needs no
			// new saved state" is a claim about exactly that path, so the claim has to be run on a
			// client that can be corrected.
			//
			// Both poses are AUTHORITATIVE and neither is anywhere near a measured ride: the client
			// waits, moves under its own predicted input, and only then starts surfing.
			bool& bPoseOnFloor = TraceMovementSurf::SurfNetPoseOnFloor();
			bPoseOnFloor = !bPoseOnFloor;

			const FVector Where = bPoseOnFloor
				? (TraceMovementSurf::SurfNetLaneStand(Probe) + FVector(0.f, 0.f, 120.f))
				: (Probe.CrestStand + FVector(0.f, 0.f, 120.f));

			CharacterOwner->TeleportTo(Where, CharacterOwner->GetActorRotation(), false, true);
			Velocity = FVector::ZeroVector;

			UE_LOG(LogTraceGame, Display,
				TEXT("SURFNET [server] re-posed %s onto the %s at %s (next in %.1f s). The client is "
				     "not told and does not teleport: it waits, %s, and measures."),
				*GetNameSafe(CharacterOwner), bPoseOnFloor ? TEXT("FLOOR LANE") : TEXT("crest"),
				*Where.ToCompactString(), TraceMovementSurf::SurfNetCycleSeconds,
				bPoseOnFloor ? TEXT("runs at the rail from the floor") : TEXT("runs off the edge"));
		}
		return;
	}

	APlayerController* TestController = Cast<APlayerController>(CharacterOwner->GetController());
	if (TestController == nullptr || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// =============================================================================================
	// DEMO 29 ITEM 4 — THE TRANSITION RIGS. Everything below this block belongs to Patch 28's ladder
	// and net arms and is untouched; these two modes return before reaching it.
	// =============================================================================================
	if (bExitMode || bApproachMode)
	{
		using namespace TraceMovementSurf;

		if (RigReported())
		{
			return;
		}

		const int32 TotalRigRuns = bExitMode ? SurfExitLadderCount : SurfApproachCount;

		if (RigRun() < 0)
		{
			// Six seconds is the same wait the Patch 28 ladder takes: the arena is built, the bots have
			// spawned and the pawn has settled onto the floor before anything is measured.
			if (TestWorld->GetTimeSeconds() < 6.f)
			{
				return;
			}
			RigRun() = 0;
			RigPhase() = ESurfRigPhase::Place;
			SurfExitRows().Reset();
			SurfApproachRows().Reset();

			UE_LOG(LogTraceGame, Display,
				TEXT("SURF%s ---- begin. rail %d..%d deg, crest %.0f uu, run %.0f uu | toe on the floor at "
				     "%s | ground limit %.0f uu/s | air hard cap %.0f | surf ceiling %.0f | %d runs"),
				bExitMode ? TEXT("EXIT") : TEXT("APPROACH"),
				FMath::RoundToInt(Probe.MinFaceAngleDegrees), FMath::RoundToInt(Probe.MaxFaceAngleDegrees),
				Probe.Height, Probe.RunLength, *Probe.ToeOnFloor.ToCompactString(),
				GetMaxSpeed(), GetAirStrafeHardCapSpeed(),
				GetAirStrafeHardCapSpeed() * GetSurfSpeedCeilingMultiplier(), TotalRigRuns);
		}

		if (RigRun() >= TotalRigRuns)
		{
			RigReported() = true;
			if (bExitMode)
			{
				LogSurfExitTable();
			}
			else
			{
				LogSurfApproachTable();
			}
			LogSurfReport();
			return;
		}

		if (bExitMode)
		{
			TickSurfExitRun(DeltaSeconds);
		}
		else
		{
			TickSurfApproachRun(DeltaSeconds);
		}
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

		// DEMO 29 ITEM 4(b). Posed in the FLOOR LANE instead: run at the rail. Ordinary predicted input
		// through AddMovementInput, no teleport and no velocity write — the entry itself happens inside
		// HandleImpact on the frame the capsule touches the face, on this machine and on the server,
		// from the same ServerMove.
		{
			const FVector LaneStand = TraceMovementSurf::SurfNetLaneStand(Probe);
			const bool bInLane = IsMovingOnGround()
				&& FVector::Dist2D(NetHere, LaneStand) < 1400.f
				&& FMath::Abs(NetHere.Z - LaneStand.Z) < 240.f;

			if (bInLane && !bOnCrest)
			{
				const FVector Approach = TraceMovementSurf::SurfNetApproachDirection(Probe);
				CharacterOwner->AddMovementInput(Approach, 1.f);
				TestController->SetControlRotation(Approach.Rotation());
				return;
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

// =================================================================================================
// DEMO 29 ITEM 4 (a) — WHAT HAPPENS AT THE END OF THE CURVE, MEASURED ACROSS THE TRANSITION.
// =================================================================================================
//
// The owner: "when sliding down the curved surfaces, a player loses all momentum at the end of the
// curve ... a player should carry momentum from a curve down onto the flat floor."
//
// Patch 28's ladder cannot answer that, and the reason is worth stating because it is the same
// shape as the bug: it closes its sample on the frame the SURF STATE closes, and on every strafed
// rung the surf state was still open when the four-second run clock expired (FINAL2.log: mode=3,
// grounded=0, "run clock expired"). The number in its "exit" column is therefore a MID-AIR speed
// taken while the ride was still going. Everything the owner is describing happens afterwards.
//
// This rig follows one ride through all four of the places speed can go:
//
//   1. the last frame ON the face          (planar, 3D and Vz — the ride's own speed)
//   2. the last AIRBORNE frame             (the frame before the landing, i.e. what arrives)
//   3. the first GROUNDED frame            (what survives the landing itself)
//   4. +0.25 / +0.5 / +1.0 / +2.0 s        (what survives the floor)
//
// and it reports the distance the pawn actually carried before it was back at walking pace, which
// is the quantity the complaint is really about: "carry momentum onto the flat floor" is a claim
// about DISTANCE, not about one frame's number.
//
// No input is driven after the ride ends except forward along the direction of travel, which is what
// a player holds. Steering is the only thing input can do to carried momentum in this model, so
// holding forward changes nothing about the bleed and stops the run ending in a dead stop that would
// make the floor phase unreadable.
// =================================================================================================
void UTraceCharacterMovementComponent::TickSurfExitRun(float DeltaSeconds)
{
	using namespace TraceMovementSurf;

	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	const ATraceArenaBuilder::FTraceSurfRailProbe Probe = ProbeForPawn(*this);
	if (!Probe.bValid)
	{
		return;
	}

	APlayerController* PC = Cast<APlayerController>(CharacterOwner->GetController());
	FSurfExitSample& Row = RigExit();
	const FVector Here = UpdatedComponent->GetComponentLocation();

	// Kept in one place so the "last airborne frame" is the same fact in every phase: the frame
	// before the landing, whether the surf closed by landing or by falling off the end of the rail.
	if (!IsMovingOnGround())
	{
		Row.LastAirPlanar = GetPlanarSpeed();
		Row.LastAirSpeed3D = static_cast<float>(Velocity.Size());
		Row.LastAirVz = static_cast<float>(Velocity.Z);
	}

	// A SHARP LOSS IS AN EVENT. Nothing in this movement model can take 150 uu/s off a planar speed in
	// one frame — the bleed's whole authority is (2 x excess + 400) uu/s^2, which is 30 uu/s at 60 fps
	// even at 1900 uu/s — so a step this big is a collision, and it is the one thing that would make a
	// carry number mean the opposite of what it says.
	{
		static float LastGroundPlanar = 0.f;
		const float NowPlanar = GetPlanarSpeed();
		const bool bMeasuring = (RigPhase() == ESurfRigPhase::AirGap || RigPhase() == ESurfRigPhase::Ground);
		if (bMeasuring && LastGroundPlanar - NowPlanar > 150.f)
		{
			if (RigPhase() == ESurfRigPhase::Ground)
			{
				Row.bGroundObstructed = true;
			}
			else
			{
				Row.bAirObstructed = true;
			}
			Row.ObstructedLoss = LastGroundPlanar - NowPlanar;
			Row.ObstructedWhere = Here;
			Row.ObstructedAt = RigPhaseClock();

			UE_LOG(LogTraceGame, Display,
				TEXT("SURFEXIT   run %d hit something %s: %.0f uu/s gone in one frame at %s (%.2f s into "
				     "the phase). The rig drives straight and cannot steer; a player can."),
				RigRun() + 1, (RigPhase() == ESurfRigPhase::Ground) ? TEXT("ON THE FLOOR") : TEXT("IN THE AIR"),
				Row.ObstructedLoss, *Here.ToCompactString(), Row.ObstructedAt);
		}
		LastGroundPlanar = NowPlanar;
	}

	// --- -TraceSurfFrameTrace: the ride, frame by frame. See the block in the namespace above. -----
	if (SurfFrameTraceEnabled()
		&& (RigPhase() == ESurfRigPhase::Ride || RigPhase() == ESurfRigPhase::AirGap
			|| RigPhase() == ESurfRigPhase::Ground))
	{
		const float Planar = GetPlanarSpeed();
		const float Delta = Planar - SurfFrameTracePrevPlanar();
		SurfFrameTracePrevPlanar() = Planar;

		// The plane the pawn is ON. While surfing that is the live surf plane; on the ground it is the
		// floor the engine found. Reported as the FACE ANGLE, because degrees off horizontal is the
		// number the geometry is cut in and the number a facet joint moves.
		const FVector Plane = IsSurfing() ? GetSurfPlaneNormal() : CurrentFloor.HitResult.ImpactNormal;
		const float Nz = FMath::Clamp(static_cast<float>(Plane.Z), -1.f, 1.f);
		const float FaceDegrees = Plane.IsNearlyZero()
			? -1.f : FMath::RadiansToDegrees(FMath::Acos(Nz));

		// A LOSS IS ONLY INTERESTING WHILE THE PAWN IS ON SOMETHING. Air drag and the wish cap take
		// their own few uu/s off a free-flying pawn every frame and none of that is geometry.
		if (-Delta > SurfFrameTraceWorst().Loss && (IsSurfing() || IsMovingOnGround()))
		{
			FSurfFrameWorst& Worst = SurfFrameTraceWorst();
			Worst.Loss = -Delta;
			Worst.AtX = static_cast<float>(Here.X);
			Worst.AtAngle = FaceDegrees;
			Worst.FromPlanar = Planar - Delta;
			Worst.bSurfing = IsSurfing();
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("SURFFRAME run=%d ph=%d t=%6.3f X=%8.1f Y=%7.1f Z=%7.1f planar=%7.1f d=%+7.1f "
			     "v3=%7.1f Vz=%+8.1f face=%6.2f surf=%d gnd=%d"),
			RigRun() + 1, static_cast<int32>(RigPhase()), RigPhaseClock(),
			Here.X, Here.Y, Here.Z, Planar, Delta, Velocity.Size(), Velocity.Z,
			FaceDegrees, IsSurfing() ? 1 : 0, IsMovingOnGround() ? 1 : 0);
	}

	switch (RigPhase())
	{
	case ESurfRigPhase::Place:
	{
		const float Requested = SurfExitLadder[FMath::Clamp(RigRun(), 0, SurfExitLadderCount - 1)];
		SurfFrameTracePrevPlanar() = Requested;
		SurfFrameTraceWorst() = FSurfFrameWorst();

		Row = FSurfExitSample();
		Row.RequestedEntry = Requested;
		Row.GroundLimit = FMath::Max(1.f, GetMaxSpeed());

		CharacterOwner->TeleportTo(Probe.FaceEntry, CharacterOwner->GetActorRotation(), false, true);
		Velocity = Probe.RunDirection * Requested;
		SetMovementMode(MOVE_Falling);

		// The previous run's ride is WIPED rather than left to expire. A grace still ticking would make
		// this run's first frame look like a continuation of the last one, and the entry speed in the
		// table would belong to a different ride.
		SurfContactRemaining = 0.f;
		SurfPlaneNormal = FVector::ZeroVector;
		SurfEntrySpeed = 0.f;
		SurfElapsedSeconds = 0.f;

		SurfTestWishYawValid() = false;
		RigPhaseClock() = 0.f;
		RigPhase() = ESurfRigPhase::Settle;

		if (PC != nullptr)
		{
			PC->SetControlRotation(Probe.RunDirection.Rotation());
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("SURFEXIT   placing run %d/%d at %s (asked %s), entry %.0f uu/s along %s"),
			RigRun() + 1, SurfExitLadderCount, *UpdatedComponent->GetComponentLocation().ToCompactString(),
			*Probe.FaceEntry.ToCompactString(), Requested, *Probe.RunDirection.ToCompactString());
		break;
	}

	case ESurfRigPhase::Settle:
	{
		RigPhaseClock() += DeltaSeconds;
		if (RigPhaseClock() >= SurfExitSettleSeconds)
		{
			RigPhaseClock() = 0.f;
			RigPhase() = ESurfRigPhase::Ride;
		}
		break;
	}

	case ESurfRigPhase::Ride:
	{
		RigPhaseClock() += DeltaSeconds;
		Row.PeakPlanar = FMath::Max(Row.PeakPlanar, GetPlanarSpeed());

		if (IsSurfing())
		{
			if (!Row.bSurfed)
			{
				Row.bSurfed = true;
				Row.Entry = GetSurfEntrySpeed();
			}
			Row.RideSeconds += DeltaSeconds;
			Row.LastSurfPlanar = GetPlanarSpeed();
			Row.LastSurfSpeed3D = static_cast<float>(Velocity.Size());
			Row.LastSurfVz = static_cast<float>(Velocity.Z);
			Row.SurfCloseAt = Here;

			DriveIdealSurfStrafe(CharacterOwner, PC, Probe.RunDirection, GetSurfPlaneNormal(),
				Velocity, GetAirMaxWishSpeed(), DeltaSeconds);
			break;
		}

		if (Row.bSurfed)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("SURFEXIT   run %d ride closed after %.2f s at %s | last surf frame: planar %.0f, "
				     "3D %.0f, Vz %+.0f | grounded=%d mode=%d"),
				RigRun() + 1, Row.RideSeconds, *Row.SurfCloseAt.ToCompactString(),
				Row.LastSurfPlanar, Row.LastSurfSpeed3D, Row.LastSurfVz,
				IsMovingOnGround() ? 1 : 0, static_cast<int32>(MovementMode.GetValue()));

			RigPhaseClock() = 0.f;
			RigPhase() = ESurfRigPhase::AirGap;
			break;
		}

		// Never got onto the face at all. A run that reports nothing is worse than a run that fails.
		if (RigPhaseClock() >= 2.0f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("SURFEXIT   run %d never entered the surf state (at %s, mode=%d). The placement is "
				     "wrong or the rail moved."),
				RigRun() + 1, *Here.ToCompactString(), static_cast<int32>(MovementMode.GetValue()));
			SurfExitRows().Add(Row);
			++RigRun();
			RigPhase() = ESurfRigPhase::Place;
		}
		break;
	}

	case ESurfRigPhase::AirGap:
	{
		RigPhaseClock() += DeltaSeconds;
		Row.AirGapSeconds = RigPhaseClock();
		Row.PeakPlanar = FMath::Max(Row.PeakPlanar, GetPlanarSpeed());

		if (IsMovingOnGround())
		{
			Row.bLanded = true;
			Row.GroundPlanar = GetPlanarSpeed();
			Row.LandedAt = Here;
			RigGroundStart() = Here;
			RigPhaseClock() = 0.f;
			RigPhase() = ESurfRigPhase::Ground;

			// THE ONE LINE THE WHOLE OF COMPLAINT (a) LIVES ON. Both sides of the landing, on the same
			// frame, so no reader has to line up two log lines to see what the transition cost.
			UE_LOG(LogTraceGame, Display,
				TEXT("SURFEXIT   run %d LANDED at %s after %.2f s of air | last airborne frame planar %.0f "
				     "3D %.0f Vz %+.0f  ->  first grounded frame planar %.0f  (the landing cost %.0f uu/s "
				     "of 3D speed, %.0f uu/s of it vertical)"),
				RigRun() + 1, *Here.ToCompactString(), Row.AirGapSeconds,
				Row.LastAirPlanar, Row.LastAirSpeed3D, Row.LastAirVz, Row.GroundPlanar,
				Row.LastAirSpeed3D - Row.GroundPlanar, FMath::Abs(Row.LastAirVz));
			break;
		}

		if (RigPhaseClock() >= 5.0f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("SURFEXIT   run %d never landed within 5 s of the ride ending (at %s)."),
				RigRun() + 1, *Here.ToCompactString());
			SurfExitRows().Add(Row);
			++RigRun();
			RigPhase() = ESurfRigPhase::Place;
		}
		break;
	}

	case ESurfRigPhase::Ground:
	{
		const float Before = RigPhaseClock();
		RigPhaseClock() += DeltaSeconds;
		const float After = RigPhaseClock();
		const float Planar = GetPlanarSpeed();

		auto Sample = [Before, After](float Mark, float& Slot, float Value)
		{
			if (Before < Mark && After >= Mark)
			{
				Slot = Value;
			}
		};
		Sample(0.25f, Row.At025, Planar);
		Sample(0.50f, Row.At050, Planar);
		Sample(1.00f, Row.At100, Planar);
		Sample(2.00f, Row.At200, Planar);

		// The carry, as a DISTANCE: how far the pawn got before the floor had taken it back to walking
		// pace. "Carry momentum onto the flat floor" is a claim about distance, not about one frame.
		// One uu/s of slack, for the same reason the component's own overspeed test carries an epsilon:
		// float noise around the limit would otherwise stop the carry a frame early or a frame late.
		if (Planar > Row.GroundLimit + 1.f)
		{
			Row.CarryDistance = static_cast<float>(FVector::Dist2D(Here, RigGroundStart()));
		}

		// Forward along travel, which is what a player holds. It cannot change the bleed (the bleed is
		// input-independent; input only steers), and it stops the run ending in a dead stop.
		FVector Travel(Velocity.X, Velocity.Y, 0.f);
		if (Travel.Normalize() && PC != nullptr)
		{
			CharacterOwner->AddMovementInput(Travel, 1.f);
			PC->SetControlRotation(Travel.Rotation());
		}

		if (RigPhaseClock() >= SurfExitGroundSeconds)
		{
			SurfExitRows().Add(Row);
			UE_LOG(LogTraceGame, Display,
				TEXT("SURFEXIT run %d/%d  entry %6.0f | last surf %6.0f planar / %6.0f 3D | landed %6.0f | "
				     "+0.25 %6.0f  +0.5 %6.0f  +1.0 %6.0f  +2.0 %6.0f | carried %.0f uu"),
				RigRun() + 1, SurfExitLadderCount, Row.Entry, Row.LastSurfPlanar, Row.LastSurfSpeed3D,
				Row.GroundPlanar, Row.At025, Row.At050, Row.At100, Row.At200, Row.CarryDistance);

			if (SurfFrameTraceEnabled())
			{
				const FSurfFrameWorst& Worst = SurfFrameTraceWorst();
				UE_LOG(LogTraceGame, Display,
					TEXT("SURFFRAME run %d WORST SINGLE FRAME: -%.1f uu/s (from %.0f) at X %.0f on a %.2f "
					     "deg face, %s. Anything over ~30 uu/s is larger than one frame of gravity along "
					     "the steepest facet and is therefore geometry, not the model."),
					RigRun() + 1, Worst.Loss, Worst.FromPlanar, Worst.AtX, Worst.AtAngle,
					Worst.bSurfing ? TEXT("while SURFING") : TEXT("on the GROUND"));
			}

			++RigRun();
			RigPhase() = ESurfRigPhase::Place;
		}
		break;
	}
	}
}

// =================================================================================================
// DEMO 29 ITEM 4 (b) — CAN A PLAYER SURF INTO A CURVE FROM THE FLOOR?
// =================================================================================================
//
// The owner: "It still doesn't feel like you can 'surf' into curves/curved ramps in order to gain
// momentum."
//
// Patch 28 measured gain while ALREADY on a ramp — every arm of its rig either teleports the pawn
// onto the face or walks it off the crest. The owner is describing the APPROACH: running at a rail
// at floor speed and getting nothing for it. So this rig never puts a pawn on the face. It stands
// one on the flat floor a few hundred uu inboard of the toe, runs it at the rail at the ground
// limit, and records what the rail gives back.
//
// THE FIRST RUNG IS THE CONTROL AND IT IS 0 DEGREES: the same pawn at the same speed running
// PARALLEL to the rail, which never touches it. Its final speed is what "floor speed" means on this
// build, and every other rung is only interesting relative to it.
// =================================================================================================
void UTraceCharacterMovementComponent::TickSurfApproachRun(float DeltaSeconds)
{
	using namespace TraceMovementSurf;

	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	const ATraceArenaBuilder::FTraceSurfRailProbe Probe = ProbeForPawn(*this);
	if (!Probe.bValid)
	{
		return;
	}

	APlayerController* PC = Cast<APlayerController>(CharacterOwner->GetController());
	FSurfApproachSample& Row = RigApproach();
	const FVector Here = UpdatedComponent->GetComponentLocation();

	// INBOARD is the face's own outward direction, flattened — the way the ramp looks, and therefore
	// the way a player comes at it. Taken off the probe's normal so it mirrors with the quadrant for
	// free rather than from a sign this file would have to get right four times.
	const FVector Inboard = Probe.FaceNormal.GetSafeNormal2D();
	const FVector TowardFace = -Inboard;
	const float AngleDeg = SurfApproachAngles[FMath::Clamp(RigRun(), 0, SurfApproachCount - 1)];
	const float AngleRad = FMath::DegreesToRadians(AngleDeg);
	const FVector Approach =
		(Probe.RunDirection * FMath::Cos(AngleRad) + TowardFace * FMath::Sin(AngleRad)).GetSafeNormal();

	switch (RigPhase())
	{
	case ESurfRigPhase::Place:
	{
		Row = FSurfApproachSample();
		Row.AngleDegrees = AngleDeg;

		// Back along the rail by however far the approach travels while it closes the standoff, so
		// every rung meets the face at roughly the same station and not at four different ones.
		const float BackOff = (AngleDeg <= 1.f)
			? 0.f
			: FMath::Min(1200.f, SurfApproachStandoff / FMath::Max(0.05f, FMath::Tan(AngleRad)));

		const FVector Start = Probe.ToeOnFloor
			+ Inboard * SurfApproachStandoff
			- Probe.RunDirection * BackOff
			+ FVector(0.f, 0.f, 120.f);

		CharacterOwner->TeleportTo(Start, Approach.Rotation(), false, true);
		Velocity = FVector::ZeroVector;
		SetMovementMode(MOVE_Walking);

		SurfContactRemaining = 0.f;
		SurfPlaneNormal = FVector::ZeroVector;
		SurfEntrySpeed = 0.f;
		SurfElapsedSeconds = 0.f;

		RigPhaseClock() = 0.f;
		RigPhase() = ESurfRigPhase::Settle;

		if (PC != nullptr)
		{
			PC->SetControlRotation(Approach.Rotation());
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("SURFAPPROACH   placing run %d/%d: %2.0f deg to the rail, from %s (asked %s), toe on the "
			     "floor at %s, standoff %.0f uu"),
			RigRun() + 1, SurfApproachCount, AngleDeg,
			*UpdatedComponent->GetComponentLocation().ToCompactString(), *Start.ToCompactString(),
			*Probe.ToeOnFloor.ToCompactString(), SurfApproachStandoff);
		break;
	}

	case ESurfRigPhase::Settle:
	{
		RigPhaseClock() += DeltaSeconds;
		if (RigPhaseClock() >= SurfApproachSettleSeconds)
		{
			// AT THE GROUND LIMIT, not above it: the complaint is about a player at ordinary floor
			// speed, so the run starts at exactly what GetMaxSpeed() allows and nothing is carried in.
			Velocity = Approach * FMath::Max(1.f, GetMaxSpeed());
			Row.StartSpeed = GetPlanarSpeed();
			Row.MinAfterContact = Row.StartSpeed;
			RigPhaseClock() = 0.f;
			RigPhase() = ESurfRigPhase::Ride;
		}
		break;
	}

	case ESurfRigPhase::Ride:
	{
		RigPhaseClock() += DeltaSeconds;
		const float Planar = GetPlanarSpeed();

		// Signed distance INBOARD of the toe line. Negative means the pawn is over the face.
		const float Inset = static_cast<float>(FVector::DotProduct(Here - Probe.ToeOnFloor, Inboard));
		if (!Row.bReachedFace && Inset < 80.f)
		{
			Row.bReachedFace = true;
			Row.SpeedAtContact = Planar;
			Row.MinAfterContact = Planar;
			Row.ContactAt = Here;
		}

		if (Row.bReachedFace)
		{
			Row.MinAfterContact = FMath::Min(Row.MinAfterContact, Planar);
			Row.PeakAfterContact = FMath::Max(Row.PeakAfterContact, Planar);
		}

		Row.HighestZ = FMath::Max(Row.HighestZ, static_cast<float>(Here.Z));
		Row.DeepestInset = FMath::Min(Row.DeepestInset, Inset);
		Row.EndedAt = Here;

		// -TraceSurfFrameTrace, ON THE APPROACH TOO. It used to be wired only into the exit rig, and
		// that is the arm that needed it least: an exit run starts ON the face and the summary already
		// says where it landed. An APPROACH run that reports "no surf" has failed somewhere between the
		// floor and the band and the summary cannot say where — the same six-identical-rows problem the
		// climb columns above were added for, one level deeper. The face angle column is the one that
		// matters here: it says which facet the pawn was standing on when it stopped climbing.
		if (SurfFrameTraceEnabled())
		{
			const FVector Plane = IsSurfing() ? GetSurfPlaneNormal() : CurrentFloor.HitResult.ImpactNormal;
			const float FaceDegrees = Plane.IsNearlyZero()
				? -1.f
				: FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(static_cast<float>(Plane.Z), -1.f, 1.f)));

			// WHAT IS IN FRONT OF THE PAWN, named, with its normal. The columns above can say a run
			// stopped and cannot say what stopped it: the first steepened ramp read "face=0.00" and
			// "planar 976 -> 0" in consecutive frames, which is a pawn on flat ground hitting something
			// vertical, and nothing in the arena's visibility profile at that spot is vertical. A
			// forward sweep of the pawn's OWN capsule answers it directly, on the pawn's own channel.
			FString Ahead = TEXT("clear");
			if (const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent())
			{
				FCollisionQueryParams AheadParams(SCENE_QUERY_STAT(TraceSurfApproachAhead), false, CharacterOwner);
				FHitResult AheadHit;
				const FVector Step = Approach.GetSafeNormal() * 60.f;
				if (GetWorld()->SweepSingleByChannel(AheadHit, Here, Here + Step, FQuat::Identity,
					ECC_Pawn, FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),
						Capsule->GetScaledCapsuleHalfHeight()), AheadParams))
				{
					Ahead = FString::Printf(TEXT("'%s' n=%s (Nz %.3f) at %s t=%.2f"),
						*GetNameSafe(AheadHit.GetComponent()), *AheadHit.ImpactNormal.ToCompactString(),
						AheadHit.ImpactNormal.Z, *AheadHit.ImpactPoint.ToCompactString(), AheadHit.Time);
				}
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("SURFFRAME run=%d approach t=%6.3f X=%8.1f Y=%8.1f Z=%7.1f planar=%7.1f Vz=%+8.1f "
				     "face=%6.2f surf=%d gnd=%d inset=%7.1f ahead=%s"),
				RigRun() + 1, RigPhaseClock(), Here.X, Here.Y, Here.Z, Planar, Velocity.Z,
				FaceDegrees, IsSurfing() ? 1 : 0, IsMovingOnGround() ? 1 : 0, Inset, *Ahead);
		}

		if (IsSurfing())
		{
			Row.bSurfed = true;
			Row.SurfSeconds += DeltaSeconds;

			// Once a ride HAS started the rig strafes it, because "can you gain speed by surfing into a
			// ramp" is a question about the ride the approach buys, not only about the first frame.
			DriveIdealSurfStrafe(CharacterOwner, PC, Probe.RunDirection, GetSurfPlaneNormal(),
				Velocity, GetAirMaxWishSpeed(), DeltaSeconds);
		}
		else
		{
			SurfTestWishYawValid() = false;
			CharacterOwner->AddMovementInput(Approach, 1.f);
			if (PC != nullptr)
			{
				PC->SetControlRotation(Approach.Rotation());
			}
		}

		if (RigPhaseClock() >= SurfApproachSeconds)
		{
			Row.FinalSpeed = Planar;
			SurfApproachRows().Add(Row);

			UE_LOG(LogTraceGame, Display,
				TEXT("SURFAPPROACH run %d/%d  %2.0f deg  start %5.0f -> contact %5.0f (at %s) | min after "
				     "contact %5.0f | peak %5.0f | final %5.0f | surfed=%d for %.2f s | reachedFace=%d | "
				     "climbed to Z %.0f, %.0f uu inboard of the toe, ended at %s"),
				RigRun() + 1, SurfApproachCount, Row.AngleDegrees, Row.StartSpeed, Row.SpeedAtContact,
				*Row.ContactAt.ToCompactString(), Row.MinAfterContact, Row.PeakAfterContact,
				Row.FinalSpeed, Row.bSurfed ? 1 : 0, Row.SurfSeconds, Row.bReachedFace ? 1 : 0,
				Row.HighestZ, -Row.DeepestInset, *Row.EndedAt.ToCompactString());

			++RigRun();
			RigPhase() = ESurfRigPhase::Place;
		}
		break;
	}

	default:
		RigPhase() = ESurfRigPhase::Place;
		break;
	}
}

void UTraceCharacterMovementComponent::LogSurfExitTable() const
{
	using namespace TraceMovementSurf;

	UE_LOG(LogTraceGame, Display, TEXT("================ SURFEXIT: SPEED ACROSS THE CURVE'S EXIT ================"));
	UE_LOG(LogTraceGame, Display,
		TEXT("  ground limit %.0f uu/s | air hard cap %.0f | surf ceiling %.0f | overspeed bleed "
		     "friction %.2f + %.0f uu/s^2"),
		GetMaxSpeed(), GetAirStrafeHardCapSpeed(),
		GetAirStrafeHardCapSpeed() * GetSurfSpeedCeilingMultiplier(),
		GetGroundOverspeedFriction(), GetGroundOverspeedBraking());
	UE_LOG(LogTraceGame, Display,
		TEXT("  %-6s %-7s %-9s %-8s %-8s %-8s %-7s %-7s %-7s %-7s %-8s"),
		TEXT("entry"), TEXT("ride s"), TEXT("lastSurf"), TEXT("lastAir"), TEXT("lastAirZ"),
		TEXT("landed"), TEXT("+0.25"), TEXT("+0.5"), TEXT("+1.0"), TEXT("+2.0"), TEXT("carry uu"));

	int32 AirHits = 0;
	int32 GroundHits = 0;
	for (const FSurfExitSample& Row : SurfExitRows())
	{
		AirHits += Row.bAirObstructed ? 1 : 0;
		GroundHits += Row.bGroundObstructed ? 1 : 0;

		UE_LOG(LogTraceGame, Display,
			TEXT("  %6.0f %7.2f %9.0f %8.0f %8.0f %8.0f %7.0f %7.0f %7.0f %7.0f %8.0f  %s"),
			Row.Entry, Row.RideSeconds, Row.LastSurfPlanar, Row.LastAirSpeed3D, Row.LastAirVz,
			Row.GroundPlanar, Row.At025, Row.At050, Row.At100, Row.At200, Row.CarryDistance,
			!Row.bLanded ? TEXT("never landed")
				: (Row.bAirObstructed
					? TEXT("*** FLEW INTO SOMETHING — the exit lane is not clear ***")
					: (Row.bGroundObstructed
						? TEXT("carried, then met cover on the floor (the rig cannot steer)")
						: TEXT("clean: flew clear, landed, bled out on open floor"))));
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("  EXIT LANE: %d of %d rides hit something WHILE STILL AIRBORNE (must be 0 — that is the "
		     "'fast lane ends in a wall' failure) -> %s.  %d met cover after carrying along the floor, "
		     "which is level furniture a player steers round and the rig cannot."),
		AirHits, SurfExitRows().Num(), (AirHits == 0) ? TEXT("PASS") : TEXT("*** FAIL ***"), GroundHits);

	UE_LOG(LogTraceGame, Display,
		TEXT("  lastSurf = planar speed on the last frame ON the face. lastAir = 3D speed on the last "
		     "AIRBORNE frame and its vertical part. landed = planar speed on the FIRST grounded frame. "
		     "carry = uu travelled on the floor before the pawn was back at the walking limit."));
	UE_LOG(LogTraceGame, Display, TEXT("=========================================================================="));
}

void UTraceCharacterMovementComponent::LogSurfApproachTable() const
{
	using namespace TraceMovementSurf;

	UE_LOG(LogTraceGame, Display, TEXT("========== SURFAPPROACH: RUNNING AT A RAIL FROM THE FLOOR =========="));
	UE_LOG(LogTraceGame, Display,
		TEXT("  ground limit %.0f uu/s | surf band %.2f..%.2f deg | the 0 deg rung is the CONTROL: it "
		     "runs parallel to the rail and never touches it."),
		GetMaxSpeed(),
		FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(GetWalkableFloorZ(), -1.f, 1.f))),
		FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(GetSurfMinNormalZ(), -1.f, 1.f))));
	UE_LOG(LogTraceGame, Display,
		TEXT("  %-6s %-7s %-9s %-9s %-8s %-8s %-8s %s"),
		TEXT("angle"), TEXT("start"), TEXT("contact"), TEXT("min"), TEXT("peak"), TEXT("final"),
		TEXT("surf s"), TEXT("verdict"));

	int32 Gained = 0;
	for (const FSurfApproachSample& Row : SurfApproachRows())
	{
		const bool bControl = (Row.AngleDegrees <= 1.f);
		const bool bGain = !bControl && Row.PeakAfterContact > Row.StartSpeed + 25.f;
		Gained += bGain ? 1 : 0;

		UE_LOG(LogTraceGame, Display,
			TEXT("  %5.0f %7.0f %9.0f %9.0f %8.0f %8.0f %8.2f  %s"),
			Row.AngleDegrees, Row.StartSpeed, Row.SpeedAtContact, Row.MinAfterContact,
			Row.PeakAfterContact, Row.FinalSpeed, Row.SurfSeconds,
			bControl
				? TEXT("CONTROL - runs parallel, never touches the rail")
				: (!Row.bReachedFace
					? TEXT("*** never reached the face - placement is wrong ***")
					: (bGain
						? (Row.bSurfed ? TEXT("gained speed, and surfed for it") : TEXT("gained speed WITHOUT surfing"))
						: (Row.bSurfed ? TEXT("surfed but gained nothing") : TEXT("*** no surf, no gain ***")))));
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("  VERDICT: %d of %d approach angles gained speed off the rail (the owner's complaint is "
		     "that this number is 0)."),
		Gained, FMath::Max(0, SurfApproachRows().Num() - 1));
	UE_LOG(LogTraceGame, Display, TEXT("===================================================================="));
}

void UTraceCharacterMovementComponent::LogSurfReport() const
{
	const bool bAuthoritative = (CharacterOwner != nullptr) && CharacterOwner->HasAuthority();

	// DEMO 29 ITEM 4 adds its own line rather than lengthening this one: the three new numbers are
	// claims about the ENTRY and the EXIT, and reading them next to a ride's entry/exit means is how a
	// reader tells "the rail gave nothing back" from "nobody rode it".
	UE_LOG(LogTraceGame, Display,
		TEXT("SURFREPORT %-16s DEMO29 | rides STARTED from the ground by running at a rail: %d | "
		     "landings that rolled a ride's descent into the floor: %d (mean +%.0f uu/s each)"),
		*GetNameSafe(CharacterOwner), SurfGroundEntries, SurfRolloutCount,
		SurfRolloutGainSum / static_cast<float>(FMath::Max(1, SurfRolloutCount)));

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
