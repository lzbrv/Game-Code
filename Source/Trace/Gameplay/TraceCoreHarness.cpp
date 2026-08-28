// Trace — the Core's development harnesses. See TraceCore.h for the Core itself and
// TraceCoreInternal.h for the tables both files read.
//
// WHAT IS IN HERE. Every `#if !UE_BUILD_SHIPPING` measurement the Core carries, in the order it sat
// in TraceCore.cpp: the mode-B throw batteries (Trace.ModeB.ThrowMomentum / RunThrowTest /
// ThrowSpread / CatchTest / ContestTest / ChargeTest / CoreProbe / AutoReleaseReport), the mode-B
// rule verifier and its surface census (Trace.ModeB.Verify, Trace.ModeB.VerifySurfaces), the spec
// v13 §8 mid-air turnover reproduction, the v31 §4 / v32 §3 art probes and photographers
// (Trace.Core.FxProbe, Trace.Core.CarryProbe, Trace.Core.ArtShots), the goal-teleport audit and its
// reproduction, and the spec v25 §2 turnover verifier with its integration stage.
//
// That is about six thousand lines of code that never runs in a shipped build, and it was more than
// a third of TraceCore.cpp. RESTRUCTURE tranche D2 moved it out VERBATIM — guards, banners, essays
// and red arms unchanged. Tranche A had already fenced every one of these blocks for Shipping, so
// this file is an extraction, not a new guard: in a Shipping build it compiles to nothing.
//
// THE SEAM IS ATraceCore'S PUBLIC SURFACE plus TraceCoreInternal.h. Nothing here is a member of the
// Core and nothing here can reach a private, which is exactly why these harnesses are worth
// something: they ask the questions any other caller could ask. The functions they grade against —
// TraceModeBTuning::SteerTowardCatchPoint, ::PickContestedCatcher, ::SweepLooseCore — are THE
// FUNCTIONS THE GAME CALLS, reached through the shared header rather than reimplemented here. This
// project has already had one verification "pass" that never ran the thing it claimed to test.

#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceCoreInternal.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"
#include "Net/UnrealNetwork.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TraceMatchTypes.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceEndzone.h"
#include "Gameplay/TraceFxShapes.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "World/TraceArenaBuilder.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Containers/Ticker.h"                  // FTSTicker (Trace.Integ.TurnoverDemo)
#include "Engine/Engine.h"                      // GEngine->GetWorldContexts() (Trace.ModeB.CoreProbe)
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                        // TActorIterator
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"        // GetServerWorldTimeSeconds()
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"                    // IFileManager::Move (Trace.Core.ArtShots renames)
#include "HAL/PlatformFileManager.h"            // CreateDirectoryTree (Trace.Core.ArtShots)
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CommandLine.h"                   // -TraceTurnoverRepro= / -TraceLegacyLanding
#include "Misc/DateTime.h"                      // screenshot filenames (Trace.Core.ArtShots)
#include "Misc/Parse.h"
#include "Misc/Paths.h"                         // FPaths::Combine  (Trace.Integ.TurnoverDemo)
#include "TimerManager.h"                       // the art-shot beat schedule
#include "UnrealClient.h"                       // FScreenshotRequest (Trace.Integ.TurnoverDemo)

// Same reason as in TraceCore.cpp: the two team questions are shared, so both files ask them of the
// one implementation and every call site below reads exactly as it did when it lived there.
using namespace TraceCoreLocal;

#if !UE_BUILD_SHIPPING

static FAutoConsoleCommand GTraceModeBThrowMomentumCmd(
	TEXT("Trace.ModeB.ThrowMomentum"),
	TEXT("MODE B, spec v8 §4. Prints the last throw broken into its parts: the impulse, the velocity ")
	TEXT("inherited from the thrower, and the launch velocity that left the hand."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		const ATraceCore::FThrowMomentumSample& S = ATraceCore::LastThrow;
		if (!S.bValid)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeB] THROW MOMENTUM: no throw recorded on this machine yet (the record is made ")
				TEXT("on the SERVER, where the throw is resolved). Inheritance fraction is %.2f."),
				ATraceCore::GetThrowVelocityInheritance());
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] THROW MOMENTUM (spec v8 §4 + v13 §6): %s was %s at %.0f uu/s horizontal, %+.0f ")
			TEXT("uu/s vertical | held %.2fs -> charge x%.2f | impulse %.0f uu/s (charged) + inherited ")
			TEXT("%.0f uu/s (x%.2f) = LAUNCH %.0f uu/s, launch Z %+.0f uu/s"),
			*S.ThrowerName, S.bThrowerFalling ? TEXT("AIRBORNE") : TEXT("on the ground"),
			S.ThrowerSpeed2D, S.ThrowerVelocityZ, S.HeldSeconds, S.ChargeScale,
			S.ImpulseSpeed, S.InheritedSpeed, S.Inheritance, S.LaunchSpeed, S.LaunchVelocityZ);

		// SPEC v31 §2. The vertical term split out, because it is the one the owner reported and the
		// aggregate above cannot show it: an inherited SPEED of 900 says nothing about whether those
		// 900 were pointing at the sky or at the floor.
		const float LegacyLaunchZ = S.LaunchVelocityZ - S.InheritedVelocityZ + S.LegacyInheritedVelocityZ;
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB]   v31 §2: down-scale x%.2f | thrower Z %+.0f -> inherited Z %+.0f (pre-v31 ")
			TEXT("%+.0f) | launch Z %+.0f, pre-v31 launch Z would have been %+.0f%s"),
			S.InheritanceDown, S.ThrowerVelocityZ, S.InheritedVelocityZ, S.LegacyInheritedVelocityZ,
			S.LaunchVelocityZ, LegacyLaunchZ,
			(LegacyLaunchZ < 0.f && S.LaunchVelocityZ >= 0.f)
				? TEXT("  <- THIS IS THE REPORTED BUG: the old rule aimed this throw at the floor.")
				: TEXT(""));

		// DEMO 27. WHAT BECAME OF IT, which is the half this command could not answer when the owner
		// reported the same complaint for the second time. Every failing throw printed a perfect
		// launch line above and then lost the whole of it on the next frame.
		if (S.LaunchRetained > 0.f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeB]   Demo 27: %.2fs after that launch the Core still had %.0f uu/s - %.0f%% ")
				TEXT("of it - and was %.0f uu clear of the thrower.%s"),
				TraceModeBTuning::LaunchAuditSeconds, S.SpeedAfterLaunch, 100.f * S.LaunchRetained,
				S.DistanceFromThrowerAfterLaunch,
				(S.LaunchRetained < TraceModeBTuning::LaunchAuditMinRetained)
					? TEXT("  <- IT HIT SOMETHING IMMEDIATELY. If the contact log names a player, the ")
					  TEXT("flight sweep is seeing pawns again (Trace.ModeB.FlightHitsPawns).")
					: TEXT(""));
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeB]   Demo 27: that launch was not audited - the flight ended inside %.2fs ")
				TEXT("(caught, scored or reset), or this is not the machine that threw it."),
				TraceModeBTuning::LaunchAuditSeconds);
		}
	}));

// =================================================================================================
// *** SPEC v29 §6 — "SOMETIMES I CHARGE UP A THROW AND LET GO AND IT DOESN'T GO THE FULL DISTANCE."
// ***
// *** `Trace.ModeB.ThrowSpread <throws> [holdSeconds] [jitterMs] [auto|manual]`
// =================================================================================================
//
// AN INTERMITTENT BUG IS PROVEN BY A SPREAD, NOT BY ONE GOOD THROW, so this runs a whole population
// of throws through the REAL door — ATraceCore::RequestPassInput, the same function mouse 1 reaches
// — and prints the DISTRIBUTION of the launch speed that came out. One throw cannot distinguish
// "this is correct" from "this one happened to be correct".
//
// WHAT EACH ARGUMENT IS FOR, AND WHY THE JITTER ONE IS THE WHOLE INSTRUMENT
//
//   throws        population size. 40 is enough to see a 5% tail.
//   holdSeconds   how long the player MEANT to hold. Defaults to exactly one full charge
//                 (CoreThrowChargeSeconds), which is the case the owner described: watch the ring
//                 fill, let go.
//   jitterMs      *** THE VARIABLE UNDER TEST. *** The server never sees the player's hold. It sees
//                 two RPC ARRIVALS, and each one is late by that packet's own upstream lag. So with
//                 a true press at Tp and a true release at Tp+H, and lags up1 and up2:
//
//                     RED  (anchor at arrival) Held = (Tp+H+up2) - (Tp+up1) = H + up2 - up1
//                     GREEN(anchor at press)   Held = (Tp+H+up2) - (Tp)     = H + up2
//
//                 The red term is SIGNED and is negative half the time — that is the bug, and it is
//                 worst exactly when the player releases on the instant the ring completes. This
//                 argument draws up1 and up2 independently from U(0, jitterMs) per throw and drives
//                 the real code with them: the press is delivered `up1` after the modelled button-down
//                 and CARRIES that button-down instant as its stamp (which is precisely what a client
//                 sends), and the release is delivered `up2` after the modelled button-up. 0 is a
//                 single-process control with no lag at all, on which the two arms MUST agree.
//   auto          hold PAST the full-charge deadline so spec v28 §7's 0.6 s auto-release is what
//                 fires. This is the arm that answers the spec's own prime suspect: does the
//                 automatic release throw at a stale charge? Every sample in this arm should be
//                 identical and full, because the server derives the hold from its own two stamps.
//
// WHAT IT MEASURES, STATED PLAINLY SO THE NUMBER CANNOT BE OVER-READ: this is a SINGLE PROCESS, so
// there is no wire and no real jitter. `jitterMs` is a MODEL of the wire, applied at the one place
// the wire actually bites. That makes this instrument honest about the MECHANISM (which subtraction
// the launch depends on) and silent about the MAGNITUDE on any particular network. Run it with
// jitter 0 against the red arm and the green arm and they agree — as they must, because with no skew
// there is nothing for the fix to fix, and a harness whose arms cannot agree when the bug is absent
// is not measuring its rule. The evidence is the jitter > 0 pair.
//
// THE RED ARM IS `Trace.ModeB.ThrowChargeAnchorAtPress 0`.
//
static void TraceModeBRunThrowSpread(UWorld* World, int32 Throws, float HoldSeconds, float JitterSeconds, bool bAutoArm)
{
	ATraceCore* const Core = ATraceCore::Get(World);
	if (Core == nullptr || !Core->HasAuthority())
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ThrowSpread] server only — a throw is resolved on the authority, and the launch ")
			TEXT("speed this measures exists nowhere else."));
		return;
	}
	if (!Core->IsModeB())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ThrowSpread] mode A has no throw. Launch with ?mode=b."));
		return;
	}

	// Shared with the ticker below by value; it outlives this scope by design, exactly as
	// DebugTakeCore's does.
	struct FSpreadState
	{
		TWeakObjectPtr<ATraceCore> Core;
		TWeakObjectPtr<ATraceCharacter> Thrower;
		TArray<float> Speeds;
		TArray<float> Scales;
		TArray<float> Holds;
		int32 Remaining = 0;
		int32 Refusals = 0;
		int32 Foreign = 0;
		int32 LastSerial = 0;
		int32 Phase = 0;
		double NextActionTime = 0.0;
		double PressedAt = 0.0;
		double ReleaseAt = 0.0;
		float Hold = 0.f;
		float Jitter = 0.f;
		bool bAuto = false;
		FRandomStream Rng;
	};

	TSharedRef<FSpreadState> State = MakeShared<FSpreadState>();
	State->Core = Core;
	State->Remaining = FMath::Clamp(Throws, 1, 500);
	State->Hold = HoldSeconds;
	State->Jitter = FMath::Max(0.f, JitterSeconds);
	State->bAuto = bAutoArm;
	State->LastSerial = ATraceCore::LastThrow.Serial;
	// SEEDED, so two runs of the same arm draw the SAME release skews. A/B-ing a fix against a
	// different random sequence measures the sequence as much as the fix.
	State->Rng.Initialize(20290605);

	const float FullScale = ATraceCore::GetThrowChargeScaleForHold(ATraceCore::GetThrowChargeSeconds());
	UE_LOG(LogTraceGame, Display,
		TEXT("[ThrowSpread] %d throws | intended hold %.3fs (a full charge is %.3fs) | release skew ")
		TEXT("U(0,%.0fms) | arm=%s | anchorAtPress=%d | full charge = x%.3f"),
		State->Remaining, State->Hold, ATraceCore::GetThrowChargeSeconds(), 1000.f * State->Jitter,
		State->bAuto ? TEXT("AUTO-RELEASE (spec v28 s7)") : TEXT("manual release"),
		TraceModeBTuning::ThrowChargeAnchorsAtPress() ? 1 : 0, FullScale);

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[State](float /*Delta*/) -> bool
	{
		ATraceCore* const TheCore = State->Core.Get();
		if (!IsValid(TheCore))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ThrowSpread] the Core went away; aborting."));
			return false;
		}
		UWorld* const World = TheCore->GetWorld();
		if (!IsValid(World))
		{
			return false;
		}
		const double Now = World->GetTimeSeconds();

		switch (State->Phase)
		{
		case 0:
		{
			// ---- ARRANGE. A living pawn, holding the Core, with the pickup cooldown spent. --------
			if (Now < State->NextActionTime)
			{
				return true;
			}

			ATraceCharacter* Thrower = State->Thrower.Get();
			if (!IsValid(Thrower) || !Thrower->IsAlive())
			{
				TArray<ATraceCharacter*> Characters;
				TheCore->GatherCharacters(Characters);
				Thrower = nullptr;
				for (ATraceCharacter* Candidate : Characters)
				{
					if (IsValid(Candidate) && Candidate->IsAlive())
					{
						Thrower = Candidate;
						break;
					}
				}
				if (Thrower == nullptr)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[ThrowSpread] nobody alive to throw it."));
					return false;
				}
				State->Thrower = Thrower;
			}

			if (TheCore->GetCarrier() != Thrower)
			{
				TheCore->GrantTo(Thrower, ETraceCoreGrantReason::Debug);
			}

			// THE TWO UPSTREAM LAGS, drawn independently — see the header comment for the algebra.
			// Independent is the point: a single shared lag cancels in the red arm's subtraction and
			// the bug disappears, which is how this could have been "measured" and called fixed.
			const float Up1 = (State->Jitter > 0.f) ? State->Rng.FRandRange(0.f, State->Jitter) : 0.f;
			const float Up2 = (State->Jitter > 0.f) ? State->Rng.FRandRange(0.f, State->Jitter) : 0.f;

			// THE PRESS, through the real door. Not ThrowFromHolder: the bug under test is entirely
			// in how the hold BETWEEN the two input edges is measured, so a harness that called the
			// launch directly would be measuring the one part of the path that was never in doubt.
			//
			// `Now - Up1` IS THE STAMP A CLIENT SENDS. RequestPassInput's client half passes
			// LocalThrowChargeStartTime — the shared-clock instant its own button went down — and
			// this frame is that press ARRIVING Up1 later. Passing it here is standing in for a
			// client, not reaching around the code under test: the server's clamp-and-anchor runs on
			// the authority path exactly as it does for a real RPC.
			State->PressedAt = Now - static_cast<double>(Up1);
			TheCore->RequestPassInput(true, Thrower, static_cast<float>(State->PressedAt));

			if (State->bAuto)
			{
				// Past full charge AND past the §7 window, so the SERVER releases it. Nothing is sent
				// on this arm's release edge at all until the throw has already happened.
				State->ReleaseAt = Now + ATraceCore::GetThrowChargeSeconds()
					+ ATraceCore::GetThrowAutoReleaseSeconds() + 0.30;
			}
			else
			{
				// The player's button goes up at (their press + the hold they meant); the server hears
				// about it Up2 later. Both terms are on the same clock as the press above.
				State->ReleaseAt = State->PressedAt + static_cast<double>(State->Hold) + static_cast<double>(Up2);
			}
			State->Phase = 1;
			return true;
		}

		case 1:
		{
			// ---- HOLD, then release on the first frame at or after the deadline. -----------------
			//
			// "First frame at or after" is not an approximation of a player's release, it IS one: a
			// button that goes up between two frames is delivered on the next one. The residual
			// quantisation is therefore part of what is being measured, not noise added by the rig.
			if (State->bAuto && ATraceCore::LastThrow.Serial != State->LastSerial)
			{
				// The auto-release already fired. Send the matching release anyway so no latch is
				// left set, then score it.
				if (ATraceCharacter* Thrower = State->Thrower.Get())
				{
					TheCore->RequestPassInput(false, Thrower);
				}
				State->Phase = 2;
				return true;
			}
			if (Now >= State->ReleaseAt)
			{
				if (ATraceCharacter* Thrower = State->Thrower.Get())
				{
					TheCore->RequestPassInput(false, Thrower);
				}
				State->Phase = 2;
			}
			return true;
		}

		default:
		{
			// ---- SCORE. --------------------------------------------------------------------------
			if (ATraceCore::LastThrow.Serial == State->LastSerial)
			{
				// No throw came out of that press/release pair. Counted rather than retried: a run
				// that silently re-rolled its refusals would report a population it did not sample.
				++State->Refusals;
			}
			else if (ATraceCore::LastThrow.ThrowerName != GetNameSafe(State->Thrower.Get()))
			{
				// *** SOMEBODY ELSE'S THROW. *** This runs in a LIVE MATCH with bots in it, and a bot
				// that intercepts the loose Core and throws it moves the same serial. Without this
				// test that throw is scored as ours — which is exactly how a harness talks itself into
				// a spread it did not cause. It cost one measured outlier at x0.701 to find, an order
				// of magnitude outside the ±60 ms this rig injects, which is what made it obvious the
				// sample did not belong to the population.
				State->LastSerial = ATraceCore::LastThrow.Serial;
				++State->Foreign;
				UE_LOG(LogTraceGame, Verbose,
					TEXT("[ThrowSpread] discarding a throw by %s (this run's thrower is %s)."),
					*ATraceCore::LastThrow.ThrowerName, *GetNameSafe(State->Thrower.Get()));
			}
			else
			{
				State->LastSerial = ATraceCore::LastThrow.Serial;
				State->Speeds.Add(ATraceCore::LastThrow.LaunchSpeed);
				State->Scales.Add(ATraceCore::LastThrow.ChargeScale);
				State->Holds.Add(ATraceCore::LastThrow.HeldSeconds);
			}

			if (--State->Remaining > 0)
			{
				State->Phase = 0;
				// Clear of Trace.ModeB.ThrowCooldown (0.35 s from the pickup) with margin, so a
				// refusal in the sample can never be this rig's own impatience.
				State->NextActionTime = Now + 0.50;
				return true;
			}

			// ---- REPORT. -------------------------------------------------------------------------
			const int32 N = State->Speeds.Num();
			if (N == 0)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[ThrowSpread] NO THROWS LANDED (%d refusals, %d foreign). Nothing measured."),
					State->Refusals, State->Foreign);
				return false;
			}

			const float FullScale = ATraceCore::GetThrowChargeScaleForHold(ATraceCore::GetThrowChargeSeconds());

			float MinSpeed = TNumericLimits<float>::Max();
			float MaxSpeed = 0.f;
			double SumSpeed = 0.0;
			int32 ShortThrows = 0;
			for (int32 Index = 0; Index < N; ++Index)
			{
				MinSpeed = FMath::Min(MinSpeed, State->Speeds[Index]);
				MaxSpeed = FMath::Max(MaxSpeed, State->Speeds[Index]);
				SumSpeed += State->Speeds[Index];
				// SHORT means "the charge curve gave this throw less than a full charge", which is the
				// owner's sentence. Judged on the CHARGE SCALE and not on the speed, because the speed
				// also carries the thrower's inherited velocity (spec v8 §4) and a standing pawn's
				// 0 uu/s would otherwise be indistinguishable from a nerfed impulse.
				if (State->Scales[Index] < FullScale - 1e-4f)
				{
					++ShortThrows;
				}
			}
			const double Mean = SumSpeed / N;
			double SumSq = 0.0;
			for (float Speed : State->Speeds)
			{
				SumSq += (Speed - Mean) * (Speed - Mean);
			}
			const double StdDev = (N > 1) ? FMath::Sqrt(SumSq / (N - 1)) : 0.0;

			UE_LOG(LogTraceGame, Display, TEXT("================ [ThrowSpread] SPEC v29 s6 ================"));
			UE_LOG(LogTraceGame, Display,
				TEXT("arm=%s  anchorAtPress=%d  intendedHold=%.3fs  releaseSkew=U(0,%.0fms)  n=%d (%d refused, ")
				TEXT("%d discarded as another pawn's throw)"),
				State->bAuto ? TEXT("AUTO-RELEASE") : TEXT("manual"),
				TraceModeBTuning::ThrowChargeAnchorsAtPress() ? 1 : 0,
				State->Hold, 1000.f * State->Jitter, N, State->Refusals, State->Foreign);
			UE_LOG(LogTraceGame, Display,
				TEXT("LAUNCH SPEED  min %.1f  mean %.1f  max %.1f  stddev %.2f  spread %.1f uu/s (%.2f%% of mean)"),
				MinSpeed, Mean, MaxSpeed, StdDev, MaxSpeed - MinSpeed,
				100.0 * (MaxSpeed - MinSpeed) / FMath::Max(1.0, Mean));
			UE_LOG(LogTraceGame, Display,
				TEXT("CHARGE SCALE  full charge is x%.3f; %d of %d throws (%.0f%%) came out BELOW it"),
				FullScale, ShortThrows, N, 100.f * ShortThrows / N);

			// Per-throw, so the distribution can be read rather than believed. Eight to a line.
			FString Line;
			for (int32 Index = 0; Index < N; ++Index)
			{
				// HELD IS PRINTED, and it is the column that attributes a short throw. A launch speed
				// carries the thrower's inherited velocity and a charge scale carries their character's
				// curve; only the HOLD says whether the server measured the wind-up the player
				// performed, which is the entire claim of spec v29 §6.
				Line += FString::Printf(TEXT("%6.0f/x%.3f/%.3fs "),
					State->Speeds[Index], State->Scales[Index], State->Holds[Index]);
				if (((Index + 1) % 5) == 0 || Index == N - 1)
				{
					UE_LOG(LogTraceGame, Display, TEXT("  speed/scale: %s"), *Line);
					Line.Reset();
				}
			}

			// THE VERDICT IS PRINTED, not left to the reader, because this is the line a later pass
			// will grep for.
			if (ShortThrows == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[ThrowSpread] PASS: every throw at a nominally full charge launched at full charge."));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[ThrowSpread] FAIL: %d of %d nominally-full throws launched SHORT (worst x%.3f ")
					TEXT("against x%.3f). This is spec v29 s6's bug reproduced."),
					ShortThrows, N, FMath::Min(State->Scales), FullScale);
			}
			UE_LOG(LogTraceGame, Display, TEXT("==========================================================="));
			return false;
		}
		}
	}));
}

static FAutoConsoleCommandWithWorldAndArgs GTraceModeBThrowSpreadCmd(
	TEXT("Trace.ModeB.ThrowSpread"),
	TEXT("SPEC v29 s6. Server. Runs N throws at a nominally FULL charge through the real press/release ")
	TEXT("door and prints the DISTRIBUTION of the launch speed. Args: <throws> [holdSeconds] [jitterMs] ")
	TEXT("[auto]. 'auto' holds past the spec v28 s7 deadline so the AUTOMATIC release is what fires. ")
	TEXT("Red arm: Trace.ModeB.ThrowChargeAnchorAtPress 0."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
	{
		const int32 Throws = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 40;
		const float Hold = (Args.Num() > 1)
			? FCString::Atof(*Args[1])
			: ATraceCore::GetThrowChargeSeconds();
		const float JitterSeconds = (Args.Num() > 2) ? (FCString::Atof(*Args[2]) * 0.001f) : 0.f;
		bool bAuto = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
			{
				bAuto = true;
			}
		}
		TraceModeBRunThrowSpread(World, Throws, Hold, JitterSeconds, bAuto);
	}));

bool ATraceCore::HasRemoteClientPawn() const
{
	if (!HasAuthority())
	{
		return false;
	}

	TArray<ATraceCharacter*> Characters;
	GatherCharacters(Characters);

	for (const ATraceCharacter* Candidate : Characters)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive())
		{
			continue;
		}

		// A bare read of the inherited Controller; nothing is declared, so there is nothing to shadow.
		const APlayerController* CandidateController = Cast<APlayerController>(Candidate->GetController());
		if (CandidateController != nullptr && !CandidateController->IsLocalController())
		{
			return true;
		}
	}

	return false;
}

void ATraceCore::TickFlightLog()
{
	if (CVarModeBFlightLog.GetValueOnGameThread() == 0)
	{
		bFlightLogWasLoose = bLoose;
		return;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const TCHAR* Machine = HasAuthority()
		? (World->GetNetMode() == NM_ListenServer ? TEXT("HOST") : TEXT("SERVER"))
		: TEXT("CLIENT");

	if (!bLoose)
	{
		if (bFlightLogWasLoose)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBFlight] %s: no longer loose (holder %s)."),
				Machine, *GetNameSafe(Carrier));
		}
		bFlightLogWasLoose = false;
		return;
	}

	// The first frame of a flight is the one the momentum question is about, so it is never dropped
	// by the throttle: a 10 Hz sample of a 2800 uu/s launch can easily miss the launch itself.
	const float NowReal = static_cast<float>(World->GetTimeSeconds());
	const bool bFirst = !bFlightLogWasLoose;
	bFlightLogWasLoose = true;

	if (!bFirst && NowReal < NextFlightLogTime)
	{
		return;
	}
	NextFlightLogTime = NowReal + 0.1f;

	const FVector Velocity = LooseVelocity;
	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBFlight] %s%s pos %s vel %s | speed %.0f uu/s (horiz %.0f, Z %+.0f)"),
		Machine, bFirst ? TEXT(" LAUNCH") : TEXT(""),
		*FVector(LooseLocation).ToCompactString(), *Velocity.ToCompactString(),
		Velocity.Size(), Velocity.Size2D(), Velocity.Z);
}

void ATraceCore::RunThrowMomentumTest()
{
	if (!HasAuthority())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] server only - a throw is resolved on the authority."));
		return;
	}

	if (!IsModeB())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] mode A: there is no throw. Launch with ?mode=b."));
		return;
	}

	// A JOINING CLIENT'S PAWN FIRST, and that preference is spec v8 §0, not tidiness. Measuring this
	// on the listen host's own pawn would prove only that the arithmetic runs somewhere - the host is
	// the machine on which every one of this pass's three complaints is invisible by definition. Using
	// a remote client's pawn puts the whole path under test: the server reads ITS copy of a
	// client-owned pawn's velocity, releases a client-owned holder, and replicates the loose Core back
	// to the very client that threw it.
	ATraceCharacter* Thrower = nullptr;

	TArray<ATraceCharacter*> Characters;
	GatherCharacters(Characters);

	for (ATraceCharacter* Candidate : Characters)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive())
		{
			continue;
		}

		// A bare read of the inherited Controller, which is correct code - nothing is declared here.
		const APlayerController* CandidateController = Cast<APlayerController>(Candidate->GetController());
		if (CandidateController != nullptr && !CandidateController->IsLocalController())
		{
			Thrower = Candidate;
			break;
		}
	}

	if (Thrower != nullptr)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBMomentum] thrower is a REMOTE CLIENT's pawn (%s) - spec v8 §0."), *GetNameSafe(Thrower));
	}
	else if (IsValid(Carrier) && Carrier->IsAlive())
	{
		Thrower = Carrier;
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBMomentum] NO REMOTE CLIENT CONNECTED - falling back to the current holder (%s). ")
			TEXT("This is a HOST-side measurement and spec v8 §0 does not accept it on its own."),
			*GetNameSafe(Thrower));
	}
	else
	{
		for (ATraceCharacter* Candidate : Characters)
		{
			if (IsValid(Candidate) && Candidate->IsAlive())
			{
				Thrower = Candidate;
				break;
			}
		}
	}

	if (!IsValid(Thrower))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] nobody alive to throw it."));
		return;
	}

	if (Carrier != Thrower)
	{
		GrantTo(Thrower, ETraceCoreGrantReason::Debug);
	}

	UCharacterMovementComponent* Movement = Thrower->GetCharacterMovement();
	if (Movement == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] %s has no character movement."), *GetNameSafe(Thrower));
		return;
	}

	// Restored at the end: this command is a measurement, not a shove.
	const FVector SavedVelocity = Movement->Velocity;
	const EMovementMode SavedMode = Movement->MovementMode;

	const FVector Forward = Thrower->GetActorForwardVector().GetSafeNormal2D();
	const float RunSpeed = Movement->GetMaxSpeed();
	const float JumpZ = Movement->JumpZVelocity;

	struct FCase
	{
		const TCHAR* Name;
		FVector Velocity;
		EMovementMode Mode;
	};

	// SPEC v31 §2 ADDS THE TWO CASES THE BUG IS ACTUALLY IN. The original three could not have found
	// it: standing and running have no vertical term at all, and JUMPING is the one sign of the one
	// that does. RISING is the old JUMPING case under its true name; the two FALLING cases are the
	// report. -JumpZ is what a player has a moment after the apex of their own jump; -2 x JumpZ is a
	// drop off arena geometry, which is where the Core is thrown from most often in practice.
	const FCase Cases[] =
	{
		{ TEXT("STANDING"), FVector::ZeroVector,                                   MOVE_Walking },
		{ TEXT("RUNNING"),  Forward * RunSpeed,                                    MOVE_Walking },
		{ TEXT("RISING"),   Forward * RunSpeed + FVector(0.f, 0.f,  JumpZ),        MOVE_Falling },
		{ TEXT("FALLING"),  Forward * RunSpeed + FVector(0.f, 0.f, -JumpZ),        MOVE_Falling },
		{ TEXT("PLUMMET"),  Forward * RunSpeed + FVector(0.f, 0.f, -2.f * JumpZ),  MOVE_Falling },
	};

	const float CoreGravity = FMath::Abs(GetThrowGravityZ(GetWorld()));

	/**
	 * Ballistic range from the launch back to the plane the thrower's FEET are on, uu.
	 *
	 * Not a simulation and does not claim to be: no bounce, no geometry, no catch magnet. It is the
	 * one number that turns "the launch Z is -551" into something a designer can judge - "the Core
	 * lands 4 m in front of you instead of 34 m" - and it is computed from the Core's own gravity so
	 * it moves when the flight model is retuned. The real flight is longer than this whenever the
	 * Core bounces, and shorter whenever it hits something; both arms are wrong by the same amount,
	 * which is what makes the COMPARISON honest even though the absolute is an estimate.
	 */
	auto BallisticRange = [CoreGravity](float Speed2D, float VelocityZ, float HeightAboveFeet) -> float
	{
		if (CoreGravity < 1.f)
		{
			return 0.f;
		}
		const float Discriminant = VelocityZ * VelocityZ + 2.f * CoreGravity * FMath::Max(0.f, HeightAboveFeet);
		if (Discriminant <= 0.f)
		{
			return 0.f;
		}
		const float Flight = (VelocityZ + FMath::Sqrt(Discriminant)) / CoreGravity;
		return Speed2D * FMath::Max(0.f, Flight);
	};

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBMomentum] spec v8 §4 + v31 §2, thrower %s, inheritance x%.2f, DOWN-scale x%.2f ")
		TEXT("(0 = fixed, 1 = the pre-v31 bug), run speed %.0f, jump Z %.0f, core gravity %.0f uu/s2"),
		*GetNameSafe(Thrower), TraceModeBTuning::ThrowInheritance(),
		TraceModeBTuning::ThrowInheritanceDown(), RunSpeed, JumpZ, CoreGravity);

	for (const FCase& Case : Cases)
	{
		if (!IsValid(Thrower) || !Thrower->IsAlive())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] thrower died mid-measurement."));
			return;
		}

		// Put the Core back and forgive the cooldown: the cooldown is not what is being measured.
		if (Carrier != Thrower)
		{
			GrantTo(Thrower, ETraceCoreGrantReason::Debug);
		}
		ThrowCooldownEndServerTime = 0.f;

		Movement->SetMovementMode(Case.Mode);
		Movement->Velocity = Case.Velocity;

		LastThrow.bValid = false;
		if (!ThrowFromHolder(Thrower))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] %s: ThrowFromHolder refused."), Case.Name);
			continue;
		}

		// The pre-v8 launch is the impulse on its own, which LastThrow already carries - so the A/B is
		// printed from the SAME throw rather than from a second run with the knob at zero.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBMomentum] %-8s thrower %.0f uu/s horiz %+.0f vert (%s) | pre-v8 launch %.0f uu/s ")
			TEXT("(Z %+.0f) -> v8 launch %.0f uu/s (Z %+.0f) | inherited %.0f uu/s, %+.0f%%"),
			Case.Name, LastThrow.ThrowerSpeed2D, LastThrow.ThrowerVelocityZ,
			LastThrow.bThrowerFalling ? TEXT("AIRBORNE") : TEXT("grounded"),
			// THE PRE-v8 Z IS THE LAUNCH WITH THE INHERITED TERM AS ACTUALLY APPLIED TAKEN BACK OFF,
			// not with (thrower Z x inheritance) taken off. Those were the same number until spec v31
			// §2 stopped a falling thrower's Z from reaching the launch; afterwards the old expression
			// over-subtracts and prints a pre-v8 baseline that is HIGHER than the live launch, which
			// is nonsense (v8 only ever added). Caught in the first measured run of the fix.
			LastThrow.ImpulseSpeed, LastThrow.LaunchVelocityZ - LastThrow.InheritedVelocityZ,
			LastThrow.LaunchSpeed, LastThrow.LaunchVelocityZ, LastThrow.InheritedSpeed,
			100.f * (LastThrow.LaunchSpeed - LastThrow.ImpulseSpeed) / FMath::Max(1.f, LastThrow.ImpulseSpeed));

		// SPEC v31 §2's BEFORE AND AFTER, OFF THIS ONE THROW. The horizontal launch is identical under
		// both arms by construction (the fix touches Z only), so the legacy launch differs from the
		// live one in exactly one component and the difference in RANGE is attributable to it and to
		// nothing else. On the STANDING and RUNNING rows the two columns are equal - a thrower with no
		// downward velocity has nothing for the fix to change - and that equality is the check that
		// this did not quietly retune the throws nobody complained about.
		const float LegacyLaunchZ = LastThrow.LaunchVelocityZ
			- LastThrow.InheritedVelocityZ + LastThrow.LegacyInheritedVelocityZ;
		const float LegacyLaunchSpeed = FMath::Sqrt(
			LastThrow.LaunchSpeed2D * LastThrow.LaunchSpeed2D + LegacyLaunchZ * LegacyLaunchZ);
		const float LiveRange = BallisticRange(LastThrow.LaunchSpeed2D, LastThrow.LaunchVelocityZ,
			LastThrow.LaunchHeightAboveFeet);
		const float LegacyRange = BallisticRange(LastThrow.LaunchSpeed2D, LegacyLaunchZ,
			LastThrow.LaunchHeightAboveFeet);

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBMomentum] %-8s v31 §2  BEFORE (down x1.00): launch %.0f uu/s, Z %+.0f, travels ")
			TEXT("%.0f uu   ->   AFTER (down x%.2f): launch %.0f uu/s, Z %+.0f, travels %.0f uu   | ")
			TEXT("%+.0f uu/s, %+.0f uu (%+.0f%% range), launch height %.0f uu"),
			Case.Name,
			LegacyLaunchSpeed, LegacyLaunchZ, LegacyRange,
			LastThrow.InheritanceDown, LastThrow.LaunchSpeed, LastThrow.LaunchVelocityZ, LiveRange,
			LastThrow.LaunchSpeed - LegacyLaunchSpeed, LiveRange - LegacyRange,
			100.f * (LiveRange - LegacyRange) / FMath::Max(1.f, LegacyRange),
			LastThrow.LaunchHeightAboveFeet);
	}

	if (IsValid(Thrower) && Movement != nullptr)
	{
		Movement->SetMovementMode(SavedMode);
		Movement->Velocity = SavedVelocity;
	}

	UE_LOG(LogTraceGame, Display, TEXT("[ModeBMomentum] done. The last throw is still loose; play resumes normally."));
}

// NAMED "...Now", NOT "Trace.ModeB.MomentumTest". A console OBJECT name is a single namespace shared
// by variables and commands, and registering a command under the same name as the CVar above is a
// FATAL error at startup ("can't be replaced with the new one of different type") - which is how this
// first run died. The CVar is the armable form (-ExecCmds, polled); this is the fire-it-right-now form.
static FAutoConsoleCommand GTraceModeBMomentumTestCmd(
	TEXT("Trace.ModeB.MomentumTestNow"),
	TEXT("MODE B, spec v8 §4. Server. Throws the Core three times - standing, running and airborne - ")
	TEXT("and prints the pre-v8 and post-v8 launch velocity of each. Immediate; ")
	TEXT("Trace.ModeB.MomentumTest 1 is the armable form for -ExecCmds."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || World->GetNetMode() == NM_Client)
			{
				continue;
			}

			if (ATraceCore* Core = ATraceCore::Get(World))
			{
				Core->RunThrowMomentumTest();
			}
		}
	}));

// =================================================================================================
// DEMO 27 — Trace.ModeB.RunThrowTest. THE RUNNING THROW, STAGED AND SCORED.
//
// See the declaration in TraceCore.h for what it is for and why it drives the pawn for real. What
// follows is how it stages the state, in the order the phases run.
// =================================================================================================

namespace TraceModeBRunThrow
{
	/** How long the pawn runs before it throws. Past the acceleration ramp at any walk speed. */
	constexpr double RunUpSeconds = 0.60;

	/**
	 * How long after the launch the verdict is read.
	 *
	 * LaunchAuditSeconds plus a couple of frames, ON PURPOSE: the numbers this scores are the ones
	 * ATraceCore::ServerTickLaunchAudit already wrote into LastThrow. The test and the shipped
	 * instrument therefore cannot disagree about what happened - if the audit is wrong, this is wrong
	 * in exactly the same way and the two stop being independent evidence, which is honest. A second
	 * measurement of the same flight, taken here, would look like corroboration and would not be.
	 */
	constexpr double JudgeAfterLaunchSeconds = TraceModeBTuning::LaunchAuditSeconds + 0.05;

	/** The pawn must actually have been running this fast at the release, or the run proves nothing. */
	constexpr float MinThrowerSpeed = 300.f;

	/**
	 * How far clear of the thrower's capsule the Core must be when the verdict is read.
	 *
	 * THE HARNESS MAY USE A DISTANCE WHERE THE SHIPPED ALARM MAY NOT, and that difference is the
	 * point rather than an oversight. This test STAGES the throw: it puts the pawn on open ground
	 * (ClearAheadUU below), makes it run, and then reads the gap - so nothing but the thrower's own
	 * body can be responsible for a small one. The always-on alarm sees a bot throw into whatever
	 * happens to be a metre away, where the same number means nothing; it asks what the Core hit.
	 *
	 * The arithmetic behind it: the Core leaves at thousands of uu/s and the thrower keeps running
	 * under it, so a tenth of a second separates them by a couple of hundred uu - the measured green
	 * runs read 265 and 276. What the bug produced was 19, and before the depenetration guard it was
	 * a Core AT REST 87 uu from the launch point with the thrower running over the top of it. 150 sits
	 * between the two with room on both sides.
	 */
	constexpr float MinClearance = 150.f;

	/** How far ahead of the muzzle the staging demands open air, on top of the run-up's own distance. */
	constexpr double ClearAheadUU = 700.0;

	/**
	 * *** HOW MUCH FARTHER THAN ITS OWN LAUNCH CLEARANCE THE PAWN IS MADE TO TRAVEL IN ONE FRAME. ***
	 *
	 * THIS CONSTANT IS THE WHOLE STAGING, so here is the arithmetic it comes from. The launch point
	 * is ThrowMuzzleForward ahead of the eye; the Core's sphere clears the thrower's own capsule by
	 * MeasureLaunchClearance() uu, about 15 at the shipped capsule and eye height. The bug needs the
	 * capsule to cover that gap BETWEEN the frame that computes the launch point and the frame that
	 * sweeps it - i.e. it needs
	 *
	 *     run speed x frame time  >  clearance
	 *
	 * At 60 fps that is a run of about 880 uu/s, which the fastest characters exceed and the slower
	 * ones do not, and at 120 fps nobody does. THAT IS WHY THE FIRST VERSION OF THIS TEST PASSED ON
	 * THE RED ARM: it ran at whatever the pawn's own top speed happened to be (800 uu/s that run) on a
	 * machine drawing fast frames, the capsule advanced 13 uu into a 15 uu gap, and the old sweep
	 * found nothing to hit. The arms agreed, which means the test measured nothing.
	 *
	 * So the run speed is now SOLVED from the clearance and the live frame time instead of being
	 * whatever the character sheet says, and this is the margin it aims past the gap by. The test
	 * therefore stages the same geometry on a slow hero, a fast hero, a 30 Hz frame and a 240 Hz one -
	 * and the green arm passing at a speed no character can reach is a stronger statement than it
	 * passing at 800 uu/s, not a weaker one: it says the throw leaves cleanly however fast you are
	 * moving when you let go.
	 */
	constexpr double StagedOvershootUU = 8.0;

	/**
	 * Ceiling on the solved run speed. A very fast frame would otherwise ask for a pawn that crosses
	 * a wall between two ticks, and the movement component's own sweep would then be the thing under
	 * test. If the solve is clamped the verdict says so rather than quietly measuring something else.
	 */
	constexpr double MaxStagedSpeed = 4000.0;

	/**
	 * The gap, in uu, between the Core's collision sphere at the launch point and the thrower's own
	 * capsule. Positive means the throw leaves in clear air.
	 *
	 * Measured off the SAME two expressions the launch uses - ThrowFromHolder's muzzle point and the
	 * catch zone's capsule-surface distance - so that if either is retuned this moves with it. A
	 * character with a wider capsule, a lower eye or a shorter muzzle offset gets a smaller number
	 * here and needs less speed to stage the bug, which is exactly right.
	 */
	double MeasureLaunchClearance(const ATraceCharacter& Thrower, const FVector& Aim)
	{
		const FVector Muzzle = Thrower.GetPawnViewLocation() + Aim * TraceModeBTuning::ThrowMuzzleForward;

		const UCapsuleComponent* const Capsule = Thrower.GetCapsuleComponent();
		if (Capsule == nullptr)
		{
			return TraceModeBTuning::ThrowMuzzleForward;
		}

		const FVector Centre = Capsule->GetComponentLocation();
		const double HalfHeight = static_cast<double>(Capsule->GetScaledCapsuleHalfHeight());
		const double Radius = static_cast<double>(Capsule->GetScaledCapsuleRadius());
		const double CapHalf = FMath::Max(0.0, HalfHeight - Radius);

		const double ToAxis = FMath::PointDistToSegment(Muzzle,
			Centre - FVector(0.0, 0.0, CapHalf), Centre + FVector(0.0, 0.0, CapHalf));

		return ToAxis - Radius - static_cast<double>(TraceModeBTuning::CollisionRadius);
	}
}

void ATraceCore::RunRunningThrowTest()
{
	if (!HasAuthority())
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBRunThrow] server only - a throw and its first frame of flight both resolve on ")
			TEXT("the authority."));
		return;
	}

	if (!IsModeB())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBRunThrow] mode A has no throw. Launch with ?mode=b."));
		return;
	}

	UWorld* const World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// THE LOCAL PAWN FIRST, and unlike the momentum test that preference is about determinism rather
	// than about the network: this test WRITES a velocity every frame for the better part of a second,
	// and a bot's own movement is being written by its behaviour tree at the same time. The listen
	// host's pawn has nobody else steering it.
	ATraceCharacter* Thrower = nullptr;
	if (const APlayerController* const PC = World->GetFirstPlayerController())
	{
		Thrower = Cast<ATraceCharacter>(PC->GetPawn());
	}
	if (!IsValid(Thrower) || !Thrower->IsAlive())
	{
		Thrower = Carrier;
	}
	if (!IsValid(Thrower) || !Thrower->IsAlive())
	{
		TArray<ATraceCharacter*> Characters;
		GatherCharacters(Characters);
		Thrower = nullptr;
		for (ATraceCharacter* Candidate : Characters)
		{
			if (IsValid(Candidate) && Candidate->IsAlive())
			{
				Thrower = Candidate;
				break;
			}
		}
	}

	if (!IsValid(Thrower) || Thrower->GetCharacterMovement() == nullptr)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBRunThrow] SKIPPED: nobody alive with a movement component to throw it."));
		return;
	}

	// Shared with the ticker by value and outliving this scope by design, exactly as the throw-spread
	// harness above does it.
	struct FRunThrowState
	{
		TWeakObjectPtr<ATraceCore> Core;
		TWeakObjectPtr<ATraceCharacter> Thrower;
		FVector SavedVelocity = FVector::ZeroVector;
		TEnumAsByte<EMovementMode> SavedMode = MOVE_Walking;
		double ThrowAt = 0.0;
		double JudgeAt = 0.0;
		int32 SerialBefore = 0;
		int32 Phase = 0;

		/** CLAUSE A: did the loose-Core sweep, run through a living body, refuse to see it? */
		bool bRuleHolds = false;
		FString RuleDetail;

		/** The staged run: the clearance it was solved from, the speed that solved it, and the last
		 *  speed actually written. See StagedOvershootUU. */
		double Clearance = 0.0;
		double StagedSpeed = 0.0;
		bool bSpeedClamped = false;
	};

	TSharedRef<FRunThrowState> State = MakeShared<FRunThrowState>();
	State->Core = this;
	State->Thrower = Thrower;
	State->SavedVelocity = Thrower->GetCharacterMovement()->Velocity;
	State->SavedMode = Thrower->GetCharacterMovement()->MovementMode;
	State->ThrowAt = World->GetTimeSeconds() + TraceModeBRunThrow::RunUpSeconds;
	State->SerialBefore = LastThrow.Serial;

	// =============================================================================================
	// CLAUSE A — THE RULE ITSELF, ASKED DIRECTLY AND WITHOUT A THROW: *** A PAWN IS NOT GEOMETRY. ***
	//
	// The running throw below is the SYMPTOM the owner reported, and a symptom is worth reproducing.
	// But whether it reproduces depends on the frame time and the character's top speed, which are
	// not facts about the rule - the first version of this test staged a 800 uu/s run on a fast
	// machine, failed to close a 15 uu gap, and PASSED ON THE RED ARM. So the rule is now scored on
	// its own terms as well: sweep the loose Core's own query straight through a living player's
	// body and see whether it comes back saying it hit one. That question has the same answer at 30
	// fps and 240, on every character in the game.
	// =============================================================================================
	{
		const FVector Through = Thrower->GetActorForwardVector().GetSafeNormal2D();
		const FVector Body = Thrower->GetActorLocation();

		FCollisionQueryParams RuleParams(SCENE_QUERY_STAT(TraceCoreRunThrowRule),
			/*bTraceComplex=*/false, this);
		RuleParams.AddIgnoredActor(this);

		FHitResult RuleHit;
		const bool bBlocked = TraceModeBTuning::SweepLooseCore(*World, RuleHit,
			Body - Through * 250.0, Body + Through * 250.0,
			TraceModeBTuning::CollisionRadius, RuleParams);

		// NOT "did it hit nothing" - a wall behind the pawn is a perfectly good blocking hit and has
		// nothing to do with the rule. The question is whether what it stopped on was a BODY.
		const UPrimitiveComponent* const RuleComponent = bBlocked ? RuleHit.GetComponent() : nullptr;
		const bool bHitAPawn = (RuleComponent != nullptr)
			&& RuleComponent->GetCollisionObjectType() == ECC_Pawn;

		State->bRuleHolds = !bHitAPawn;
		State->RuleDetail = bHitAPawn
			? FString::Printf(TEXT("BLOCKED BY %s (%s)"), *GetNameSafe(RuleHit.GetActor()),
				*GetNameSafe(RuleHit.GetComponent()))
			: (bBlocked
				? FString::Printf(TEXT("passed through the body, stopped on %s"),
					*GetNameSafe(RuleHit.GetActor()))
				: TEXT("passed through the body, hit nothing"));
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBRunThrow] staging a RUNNING throw by %s: %.2fs of forward run, then a full charge ")
		TEXT("released while still moving. The run speed is SOLVED each frame so the capsule covers ")
		TEXT("its own %.1f uu launch clearance between two frames - see StagedOvershootUU. Arm: ")
		TEXT("FlightHitsPawns=%d (1 = the pre-Demo-27 sweep, which must FAIL)."),
		*GetNameSafe(Thrower), TraceModeBRunThrow::RunUpSeconds,
		TraceModeBRunThrow::MeasureLaunchClearance(*Thrower,
			Thrower->GetActorForwardVector().GetSafeNormal()),
		CVarModeBFlightHitsPawns.GetValueOnAnyThread());

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[State](float /*Delta*/) -> bool
	{
		ATraceCore* const TheCore = State->Core.Get();
		ATraceCharacter* const Runner = State->Thrower.Get();

		// EVERY ABANDONED PATH RESTORES THE PAWN. A harness that leaves a pawn sprinting at a wall
		// because its Core was destroyed mid-run has broken the match it was measuring.
		auto Restore = [&State](ATraceCharacter* Pawn)
		{
			if (IsValid(Pawn) && Pawn->GetCharacterMovement() != nullptr)
			{
				Pawn->GetCharacterMovement()->SetMovementMode(State->SavedMode);
				Pawn->GetCharacterMovement()->Velocity = State->SavedVelocity;
			}
		};

		if (!IsValid(TheCore) || !IsValid(Runner) || !Runner->IsAlive()
			|| Runner->GetCharacterMovement() == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBRunThrow] SKIPPED: the thrower or the Core went away mid-run."));
			Restore(Runner);
			return false;
		}

		UWorld* const TickWorld = TheCore->GetWorld();
		if (!IsValid(TickWorld))
		{
			Restore(Runner);
			return false;
		}
		const double Now = TickWorld->GetTimeSeconds();
		UCharacterMovementComponent* const Movement = Runner->GetCharacterMovement();

		// THE RUN ITSELF, re-applied every frame and through both phases 0 and 1. Written rather than
		// requested through AddMovementInput because the number that matters is HOW FAR THE CAPSULE
		// MOVES BETWEEN TWO FRAMES, and a velocity the movement component reaches by its own
		// acceleration curve makes that number a property of the character's tuning instead of a
		// constant of the test. It is restored at the end, and the direction is re-read every frame
		// so the run stays under the pawn's own facing - which is where its aim points too.
		if (State->Phase <= 1)
		{
			const FVector Forward = Runner->GetActorForwardVector().GetSafeNormal2D();

			// THE SPEED IS SOLVED, NOT LOOKED UP. See StagedOvershootUU for the whole argument: the
			// bug is "capsule advance in one frame beats launch clearance", so the run is set to
			// whatever makes that true on THIS frame and THIS character, and never below the pawn's
			// own top speed (a staged run must not be slower than a real one).
			const double FrameSeconds = FMath::Clamp(
				static_cast<double>(TickWorld->GetDeltaSeconds()), 1.0 / 240.0, 1.0 / 10.0);

			State->Clearance = TraceModeBRunThrow::MeasureLaunchClearance(*Runner, Forward);
			const double Needed = (State->Clearance + TraceModeBRunThrow::StagedOvershootUU) / FrameSeconds;

			const double Wanted = FMath::Max(static_cast<double>(Movement->GetMaxSpeed()), Needed);
			State->bSpeedClamped = (Wanted > TraceModeBRunThrow::MaxStagedSpeed);
			State->StagedSpeed = FMath::Min(Wanted, TraceModeBRunThrow::MaxStagedSpeed);

			Movement->SetMovementMode(MOVE_Walking);
			Movement->Velocity = Forward * State->StagedSpeed;
		}

		switch (State->Phase)
		{
		case 0:
		{
			if (Now < State->ThrowAt)
			{
				return true;
			}

			// --- THE STAGING CHECK. Is there open air in front of the muzzle? -----------------------
			//
			// A throw into a wall three metres away loses its speed for a completely legitimate
			// reason and would be scored as the bug. So the same query the flight uses is asked along
			// the aim first, and a blocked one SKIPS the run instead of failing it - the same rule
			// Trace.Integ.WalkCore applies to a jump that lands on a ledge.
			FVector Aim = Runner->GetAimDirection();
			if (Aim.IsNearlyZero())
			{
				Aim = Runner->GetActorForwardVector();
			}
			Aim = Aim.GetSafeNormal();

			const FVector Muzzle = Runner->GetPawnViewLocation()
				+ Aim * TraceModeBTuning::ThrowMuzzleForward;

			FCollisionQueryParams ClearParams(SCENE_QUERY_STAT(TraceCoreRunThrowClear),
				/*bTraceComplex=*/false, TheCore);
			ClearParams.AddIgnoredActor(TheCore);

			FHitResult ClearHit;
			if (TraceModeBTuning::SweepLooseCore(*TickWorld, ClearHit, Muzzle,
				Muzzle + Aim * (TraceModeBRunThrow::ClearAheadUU
					+ State->StagedSpeed * TraceModeBRunThrow::JudgeAfterLaunchSeconds),
				TraceModeBTuning::CollisionRadius, ClearParams))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ModeBRunThrow] SKIPPED: %s is aimed at %s, %.0f uu ahead. Nothing about the ")
					TEXT("throw rule can be measured through a wall - move and run it again."),
					*GetNameSafe(Runner), *GetNameSafe(ClearHit.GetActor()), ClearHit.Distance);
				Restore(Runner);
				return false;
			}

			// The Core, and a forgiven cooldown: the cooldown is not what is under test.
			if (TheCore->GetCarrier() != Runner)
			{
				TheCore->GrantTo(Runner, ETraceCoreGrantReason::Debug);
			}
			TheCore->ThrowCooldownEndServerTime = 0.f;

			// A FULL CHARGE, stated as the hold that buys one rather than as a scale, because
			// ThrowFromHolder takes a hold and derives the scale (spec v13 §6).
			if (!TheCore->ThrowFromHolder(Runner, ATraceCore::GetThrowChargeSeconds()))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ModeBRunThrow] SKIPPED: ThrowFromHolder refused the release."));
				Restore(Runner);
				return false;
			}

			State->JudgeAt = Now + TraceModeBRunThrow::JudgeAfterLaunchSeconds;
			State->Phase = 1;
			return true;
		}

		case 1:
		{
			// KEEP RUNNING. This is not padding: the thrower walking forward INTO the ball it has
			// just released is the entire mechanism under test, and a pawn that stopped at the
			// release would clear its own launch point by accident.
			if (Now < State->JudgeAt)
			{
				return true;
			}
			State->Phase = 2;
			return true;
		}

		default:
			break;
		}

		// --- SCORE, off the numbers the shipped launch audit already wrote. -------------------------
		Restore(Runner);

		const ATraceCore::FThrowMomentumSample& Throw = ATraceCore::LastThrow;

		if (Throw.Serial == State->SerialBefore)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBRunThrow] VERDICT: SKIPPED - no throw was recorded at all."));
			return false;
		}
		if (Throw.ThrowerName != GetNameSafe(Runner))
		{
			// Somebody else threw inside our window - a bot intercepting the loose Core. Scoring
			// their throw as ours is exactly how a harness talks itself into a result.
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBRunThrow] VERDICT: SKIPPED - the last throw on record is %s's, not %s's."),
				*Throw.ThrowerName, *GetNameSafe(Runner));
			return false;
		}
		if (Throw.LaunchRetained <= 0.f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBRunThrow] VERDICT: SKIPPED - the flight ended before the launch audit came ")
				TEXT("due (caught, scored or reset inside %.2fs)."),
				TraceModeBRunThrow::JudgeAfterLaunchSeconds);
			return false;
		}

		const bool bWasRunning = (Throw.ThrowerSpeed2D >= TraceModeBRunThrow::MinThrowerSpeed);
		const bool bKeptItsSpeed = (Throw.LaunchRetained >= TraceModeBTuning::LaunchAuditMinRetained);
		const bool bGotClear = (Throw.DistanceFromThrowerAfterLaunch >= TraceModeBRunThrow::MinClearance);

		// BOTH CLAUSES, AND THE RULE IS NOT THE OPTIONAL ONE. A build that passes the symptom because
		// the pawn happened to be slow, while its flight sweep still treats a body as a wall, has not
		// fixed anything - it is one dash or one dropped frame away from the owner's report.
		const bool bPass = State->bRuleHolds && bWasRunning && bKeptItsSpeed && bGotClear;

		// One string, two log calls. UE_LOG takes its verbosity as a literal, and a FAIL that only
		// ever printed at Display is a FAIL nobody greps for.
		const FString Verdict = FString::Printf(
			TEXT("[ModeBRunThrow] VERDICT: %s | A, THE RULE - a pawn is not geometry: %s (%s) | B, THE ")
			TEXT("SYMPTOM - %s threw at %.0f uu/s while running %.0f uu/s (needs >= %.0f, %s; staged ")
			TEXT("%.0f uu/s against a %.1f uu launch clearance%s) | %.2fs later the Core still had ")
			TEXT("%.0f uu/s, %.0f%% of the launch (needs >= %.0f%%, %s) and was %.0f uu clear of them ")
			TEXT("(needs >= %.0f, %s) | arm: FlightHitsPawns=%d"),
			bPass ? TEXT("PASS") : TEXT("FAIL"),
			State->bRuleHolds ? TEXT("held") : TEXT("BROKEN"), *State->RuleDetail,
			*Throw.ThrowerName, Throw.LaunchSpeed, Throw.ThrowerSpeed2D,
			TraceModeBRunThrow::MinThrowerSpeed, bWasRunning ? TEXT("ok") : TEXT("NOT RUNNING"),
			State->StagedSpeed, State->Clearance,
			State->bSpeedClamped ? TEXT(", CLAMPED - the frame was too fast to stage it fully") : TEXT(""),
			TraceModeBRunThrow::JudgeAfterLaunchSeconds, Throw.SpeedAfterLaunch,
			100.f * Throw.LaunchRetained, 100.f * TraceModeBTuning::LaunchAuditMinRetained,
			bKeptItsSpeed ? TEXT("ok") : TEXT("LOST IT"),
			Throw.DistanceFromThrowerAfterLaunch, TraceModeBRunThrow::MinClearance,
			bGotClear ? TEXT("ok") : TEXT("STILL ON TOP OF THEM"),
			CVarModeBFlightHitsPawns.GetValueOnAnyThread());

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s"), *Verdict);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("%s"), *Verdict);
		}

		return false;
	}));
}

static FAutoConsoleCommand GTraceModeBRunThrowTestCmd(
	TEXT("Trace.ModeB.RunThrowTest"),
	TEXT("MODE B, Demo 27. Server. Runs the local pawn forward, throws at a full charge while it is ")
	TEXT("still moving, and PASSES only if the Core keeps its launch speed and gets clear of the ")
	TEXT("thrower. The red arm is Trace.ModeB.FlightHitsPawns 1, on which it must FAIL."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || World->GetNetMode() == NM_Client)
			{
				continue;
			}

			if (ATraceCore* Core = ATraceCore::Get(World))
			{
				Core->RunRunningThrowTest();
			}
		}
	}));

/**
 * Prints THIS MACHINE'S view of the loose Core.
 *
 * Deliberately not authority-gated and deliberately not a server RPC: spec v8 §0 is a client-experience
 * pass, and the question "does the CLIENT see the Core carrying the throw's momentum" cannot be
 * answered from the server's copy by definition. LooseVelocity is replicated so a client can
 * dead-reckon; this prints what the client actually received.
 */
static FAutoConsoleCommand GTraceModeBCoreProbeCmd(
	TEXT("Trace.ModeB.CoreProbe"),
	TEXT("MODE B. Prints the LOCAL machine's view of the Core: held/loose, its replicated position and ")
	TEXT("velocity, and the actor transform the local renderer is drawing. Run it on a CLIENT."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || (World->GetNetMode() != NM_Client && World->GetNetMode() != NM_ListenServer
				&& World->GetNetMode() != NM_Standalone))
			{
				continue;
			}

			ATraceCore* Core = ATraceCore::Get(World);
			if (Core == nullptr)
			{
				continue;
			}

			const TCHAR* Machine = (World->GetNetMode() == NM_Client) ? TEXT("CLIENT") : TEXT("SERVER/LOCAL");
			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeB] CORE PROBE (%s): mode %s | holder %s | loose %d | repl pos %s | repl vel %s ")
				TEXT("(%.0f uu/s, Z %+.0f) | drawn at %s"),
				Machine, Core->IsModeB() ? TEXT("B") : TEXT("A"), *GetNameSafe(Core->GetCarrier()),
				Core->IsLoose() ? 1 : 0, *FVector(Core->LooseLocation).ToCompactString(),
				*FVector(Core->LooseVelocity).ToCompactString(),
				FVector(Core->LooseVelocity).Size(), FVector(Core->LooseVelocity).Z,
				*Core->GetActorLocation().ToCompactString());
		}
	}));

// =================================================================================================
// SPEC v12 §4 — MEASURING THE MAGNET. Trace.ModeB.CatchTest
//
// The change is one number (CoreCatchRadius 500 -> 450) and the request is one sentence, but "does
// the magnet still catch" is not answerable by reading it. So this sweeps the two axes that decide
// the answer — HOW BADLY THE THROW MISSES and HOW FAST THE CORE IS — and counts catches.
//
// IT CALLS THE SHIPPED STEERING FUNCTION. TraceModeBTuning::SteerTowardCatchPoint is the same
// function ATraceCore::ServerApplyCatchZone calls every frame of every real throw, and the catch
// test below is the same surface-distance-vs-PickupRadius test ServerTryPickup uses. Nothing here
// is a second implementation of the magnet: this project has already had a verification declare
// PASS without ever executing the thing it was verifying, and a re-implemented magnet would fail
// the same way while looking more thorough. The knobs come from the live accessors too, so what is
// measured is what UTraceSettings and DefaultGame.ini actually resolved to.
//
// WHY THE SWEEP IS FLAT AND UNGRAVITIED, stated plainly rather than buried: gravity would add a
// second reason for a trial to fail (a slow Core lands short no matter what the magnet does) and
// the two causes would be indistinguishable in the total. This isolates the magnet's steering
// authority, which is the only thing the radius changes. The BALLISTIC case — a real throw that
// inherits a jumping thrower's velocity — is covered by running the sweep at that throw's speed;
// what the arc does to the Core's height is the throw's business, not the magnet's.
//
// WHY IT IS A CONSOLE COMMAND AND NOT A UNIT TEST: the values have to come from a RUNNING game.
// The ini wins over the header on this project and the header has been wrong before.
//
// THE COMMAND TAKES NO ARGUMENTS, AND THAT IS DELIBERATE. It runs BOTH arms itself: the shipped
// radius, and that radius divided by 0.9, which is the pre-v12 §4 value the cut was made from. Two
// reasons, and the second one has already cost this project a pass:
//
//   1. The comparison IS the deliverable. A command that measures one arm can be run once, reported
//      as "the catch rate", and mean nothing. Both arms in one invocation cannot be half-run.
//   2. -TraceExec / -ExecCmds AND SPACES DO NOT MIX HERE. Setting the arm from the command line
//      would need `Trace.ModeB.CatchRadius 500`, and a quoted argument on this project's command
//      line has already broken into the URL parser and produced a "verification" whose commands
//      never executed at all. No argument, no space, no quote, nothing to get wrong.
//
// Usage, headless:  -TraceExec=Trace.VerifyKnobs|Trace.DumpSettings|Trace.ModeB.CatchTest
// =================================================================================================
static void RunModeBCatchTestAtRadius(const float Radius, const TCHAR* ArmLabel)
{
	// Live knobs, not literals. If CoreCatchRadius did not bind, the caller passes the fallback and
	// the numbers below are the fallback's numbers - itself the answer to a different question.
	const float Curve = TraceModeBTuning::CatchCurveStrength();
	const float Pickup = TraceModeBTuning::PickupRadius();

	// The catcher's capsule, taken from a LIVE pawn where there is one: the magnet aims at the
	// capsule's centre line and measures to its surface, so the capsule's radius is part of the
	// answer and a guessed 34 would quietly shift every number.
	double CapsuleRadius = 34.0;
	double CapsuleHalfHeight = 88.0;
	const TCHAR* CapsuleSource = TEXT("defaults (no live pawn found)");

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World == nullptr)
		{
			continue;
		}

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			if (const UCapsuleComponent* Capsule = It->GetCapsuleComponent())
			{
				CapsuleRadius = static_cast<double>(Capsule->GetScaledCapsuleRadius());
				CapsuleHalfHeight = static_cast<double>(Capsule->GetScaledCapsuleHalfHeight());
				CapsuleSource = TEXT("a live pawn");
				break;
			}
		}
	}

	// Seven speeds spanning a walked-in throw to the fastest launch the game can produce: spec v8 §4
	// has the throw inherit the thrower's velocity, and a jumping throw was measured at ~3357 uu/s.
	static const float Speeds[] = { 800.f, 1200.f, 1600.f, 2000.f, 2400.f, 2800.f, 3357.f };

	// Miss distances from dead-on to well outside the zone, in 25 uu steps. 25 uu is finer than the
	// difference the 10% cut makes, so the boundary it moves is resolvable rather than inferred.
	constexpr double MissStep = 25.0;
	constexpr int32 MissCount = 25;                 // 0 .. 600 uu
	constexpr double ApproachDistance = 1500.0;     // where the Core starts, uu from the catcher
	constexpr float DeltaSeconds = 1.f / 60.f;
	constexpr int32 MaxSteps = 600;                 // 10 s at 60 Hz; every trial resolves far sooner

	const FVector Catcher(0.0, 0.0, 0.0);

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] CATCHTEST [%s]: radius=%.0f curve=%.1f pickup=%.0f | capsule r=%.0f halfH=%.0f from %s | ")
		TEXT("%d speeds x %d miss offsets (0-%.0f uu, %.0f uu steps), flat approach from %.0f uu, %d Hz"),
		ArmLabel, Radius, Curve, Pickup, CapsuleRadius, CapsuleHalfHeight, CapsuleSource,
		static_cast<int32>(UE_ARRAY_COUNT(Speeds)), MissCount, (MissCount - 1) * MissStep, MissStep,
		ApproachDistance, static_cast<int32>(1.f / DeltaSeconds));

	int32 TotalTrials = 0;
	int32 TotalCatches = 0;
	int32 TotalBaselineCatches = 0;

	for (const float Speed : Speeds)
	{
		int32 Catches = 0;
		int32 BaselineCatches = 0;
		double WidestCatchableMiss = -1.0;

		for (int32 MissIndex = 0; MissIndex < MissCount; ++MissIndex)
		{
			const double Miss = MissIndex * MissStep;

			// TWO ARMS PER TRIAL, and the second one is what stops this being a magnet-shaped
			// tautology. MAGNET ON is the game; MAGNET OFF is the identical trial with the steering
			// skipped, i.e. what a straight throw would have done on its own. A miss the Core would
			// have caught anyway is not evidence the magnet works, and without the baseline every
			// small offset would be counted as a save the magnet did not make.
			for (int32 Arm = 0; Arm < 2; ++Arm)
			{
				const bool bMagnet = (Arm == 0);

				FVector Position = Catcher + FVector(-ApproachDistance, 0.0, 0.0);
				FVector Velocity = (FVector(ApproachDistance, Miss, 0.0)).GetSafeNormal() * static_cast<double>(Speed);

				bool bCaught = false;

				for (int32 Step = 0; Step < MaxSteps && !bCaught; ++Step)
				{
					// Surface distance to the catcher's capsule, computed exactly as
					// ServerApplyCatchZone and ServerTryPickup both compute it.
					FVector CatchPoint(Catcher.X, Catcher.Y,
						FMath::Clamp(Position.Z, Catcher.Z - CapsuleHalfHeight, Catcher.Z + CapsuleHalfHeight));
					const double SurfaceDistance = FMath::Max(0.0, FVector::Dist(Position, CatchPoint) - CapsuleRadius);

					// ServerTryPickup's test, unchanged: surface distance inside the pickup radius.
					if (SurfaceDistance <= static_cast<double>(Pickup))
					{
						bCaught = true;
						break;
					}

					if (bMagnet && SurfaceDistance <= static_cast<double>(Radius))
					{
						// The forward-only gate, copied from ServerApplyCatchZone: a catcher the Core
						// has already passed is refused, or the magnet would drag it backwards.
						const FVector ToCatcher = CatchPoint - Position;
						if (!ToCatcher.IsNearlyZero()
							&& FVector::DotProduct(ToCatcher.GetSafeNormal(), Velocity.GetSafeNormal()) >= 0.0)
						{
							Velocity = TraceModeBTuning::SteerTowardCatchPoint(
								Position, Velocity, CatchPoint, SurfaceDistance, Radius, Curve, DeltaSeconds);
						}
					}

					Position += Velocity * static_cast<double>(DeltaSeconds);

					// Once the Core is past the catcher and opening the range, the trial is decided.
					if (Position.X > Catcher.X + ApproachDistance)
					{
						break;
					}
				}

				if (bMagnet)
				{
					if (bCaught)
					{
						++Catches;
						WidestCatchableMiss = FMath::Max(WidestCatchableMiss, Miss);
					}
					++TotalTrials;
					TotalCatches += bCaught ? 1 : 0;
				}
				else
				{
					BaselineCatches += bCaught ? 1 : 0;
					TotalBaselineCatches += bCaught ? 1 : 0;
				}
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] CATCHTEST [%s]  speed %6.0f uu/s : caught %2d/%d (%5.1f%%)  vs no-magnet %2d/%d (%5.1f%%)  ")
			TEXT("| magnet saved %2d  | widest catchable miss %.0f uu"),
			ArmLabel, Speed, Catches, MissCount, 100.f * Catches / MissCount,
			BaselineCatches, MissCount, 100.f * BaselineCatches / MissCount,
			Catches - BaselineCatches, WidestCatchableMiss);
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] CATCHTEST TOTAL [%s] at radius %.0f: %d/%d caught (%.1f%%), no-magnet %d/%d (%.1f%%), ")
		TEXT("magnet saved %d throws"),
		ArmLabel, Radius, TotalCatches, TotalTrials, 100.f * TotalCatches / FMath::Max(1, TotalTrials),
		TotalBaselineCatches, TotalTrials, 100.f * TotalBaselineCatches / FMath::Max(1, TotalTrials),
		TotalCatches - TotalBaselineCatches);
}

static void RunModeBCatchTest()
{
	// THE SHIPPED RADIUS, resolved live: UTraceSettings first, DefaultGame.ini having already won
	// over the header, the CVar only if somebody set it. This is the AFTER arm by definition — it is
	// whatever the game is actually going to be played at, not a literal typed here.
	const float After = TraceModeBTuning::CatchRadius();

	// The BEFORE arm, derived from the request rather than hard-coded: spec v12 §4 is "reduce the
	// magnet radius by 10%", so the value it was reduced FROM is the shipped radius / 0.9. Deriving
	// it means the two arms cannot drift apart if the shipped number is retuned again — re-run and
	// the comparison is still "this value against the 10% it was cut from".
	const float Before = After / 0.9f;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] CATCHTEST: spec v12 §4 is a 10%% cut, so this runs BOTH arms - BEFORE %.0f uu ")
		TEXT("(= shipped / 0.9) and AFTER %.0f uu (the shipped value, live from UTraceSettings/ini). ")
		TEXT("Same sweep, same steering function, same frame rate; the only difference is the radius."),
		Before, After);

	RunModeBCatchTestAtRadius(Before, TEXT("BEFORE 500"));
	RunModeBCatchTestAtRadius(After, TEXT("AFTER  450"));
}

static FAutoConsoleCommand GTraceModeBCatchTestCmd(
	TEXT("Trace.ModeB.CatchTest"),
	TEXT("MODE B, spec v12 §4. Sweeps miss distance x Core speed through the SHIPPED catch-zone steering ")
	TEXT("and reports the catch rate against a no-magnet baseline, at BOTH the shipped radius and the ")
	TEXT("radius it was cut from (shipped / 0.9). Takes no arguments on purpose - see the block comment."),
	FConsoleCommandDelegate::CreateStatic(&RunModeBCatchTest));

// =================================================================================================
// SPEC v13 §5 — TESTING THE CONTEST. Trace.ModeB.ContestTest
//
// It drives TraceModeBTuning::PickContestedCatcher, WHICH IS THE FUNCTION THE GAME CALLS, with
// scripted distances. It needs no world, no pawns and no match, which is the point: the claim being
// made ("nearest wins, and the answer is stable and deterministic") is a claim about a selection, and
// a selection can be tested exhaustively in a millisecond where a live match can only be watched.
//
// AND IT HAS A RED ARM. Case 3 runs the same oscillating sequence twice - once at the shipped margin
// and once at zero, which is strict nearest-wins every frame - and asserts that the second one
// FLICKERS. If the harness cannot show the flicker it is not measuring stability, and its "no
// flicker" result on the shipped margin would mean nothing.
// =================================================================================================

static void RunModeBContestTest()
{
	using TraceModeBTuning::FCatchContender;
	using TraceModeBTuning::PickContestedCatcher;

	const float Margin = TraceModeBTuning::CatchContestHysteresis();

	int32 Passes = 0;
	int32 Fails = 0;

	auto Check = [&Passes, &Fails](bool bCondition, const FString& What)
	{
		(bCondition ? Passes : Fails)++;
		if (bCondition)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBContest]   PASS  %s"), *What);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBContest]   FAIL  %s"), *What);
		}
	};

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBContest] ===== spec v13 §5: the contested magnet, margin %.0f uu ====="), Margin);

	// --- 1. NEAREST WINS, with nobody holding the pull. -------------------------------------------
	{
		TArray<FCatchContender> Set;
		Set.Add({ 300.0, 11u, false });
		Set.Add({ 120.0, 22u, false });   // nearest
		Set.Add({ 260.0, 33u, false });

		const int32 Winner = PickContestedCatcher(Set, Margin);
		Check(Winner == 1, FString::Printf(
			TEXT("three contenders at 300/120/260 uu -> the 120 wins (got index %d)"), Winner));
	}

	// --- 2. A CLEARLY NEARER CHALLENGER TAKES THE PULL. -------------------------------------------
	{
		TArray<FCatchContender> Set;
		Set.Add({ 400.0, 11u, true });    // incumbent, far
		Set.Add({ 100.0, 22u, false });   // challenger, much nearer

		const int32 Winner = PickContestedCatcher(Set, Margin);
		Check(Winner == 1, FString::Printf(
			TEXT("an incumbent at 400 uu loses to a challenger at 100 uu (got index %d)"), Winner));
	}

	// --- 3. A NEAR TIE DOES NOT OSCILLATE — AND THE ZERO-MARGIN ARM SHOWS THAT IT WOULD. ----------
	//
	// Two players closing on the Core, their distances crossing back and forth by a few uu each frame,
	// which is what two defenders converging actually looks like frame to frame.
	{
		auto RunOscillation = [](float TestMargin) -> int32
		{
			int32 Incumbent = INDEX_NONE;
			int32 Switches = 0;

			for (int32 Frame = 0; Frame < 60; ++Frame)
			{
				// A and B converge on the Core together, separated by a jitter of +/-8 uu that changes
				// sign every few frames. Neither is meaningfully nearer at any point.
				const double Base = 400.0 - Frame * 4.0;
				const double Jitter = 8.0 * FMath::Sin(Frame * 0.9);

				TArray<FCatchContender> Set;
				Set.Add({ Base + Jitter, 11u, Incumbent == 0 });
				Set.Add({ Base - Jitter, 22u, Incumbent == 1 });

				const int32 Winner = PickContestedCatcher(Set, TestMargin);
				if (Incumbent != INDEX_NONE && Winner != Incumbent)
				{
					++Switches;
				}
				Incumbent = Winner;
			}

			return Switches;
		};

		const int32 SwitchesShipped = RunOscillation(Margin);
		const int32 SwitchesNoMargin = RunOscillation(0.f);

		Check(SwitchesShipped == 0, FString::Printf(
			TEXT("60 frames of a +/-8 uu near-tie at the shipped margin: %d target switches (want 0)"),
			SwitchesShipped));

		// THE RED ARM. If this does not flicker, the sequence above is not a near tie and the result
		// above proves nothing about hysteresis.
		Check(SwitchesNoMargin >= 5, FString::Printf(
			TEXT("the SAME 60 frames with the margin at 0 flicker %d times, so the test can detect a ")
			TEXT("flicker (want >= 5 - if this fails, the case above is not measuring anything)"),
			SwitchesNoMargin));
	}

	// --- 4. THE MARGIN IS NOT A PERMANENT HANDICAP. -----------------------------------------------
	//
	// Once a challenger is nearer by MORE than the margin the pull moves, and the incumbency moves
	// with it. A margin that accumulated would let the first player into the zone keep the Core for the
	// whole flight, which is the opposite of "it should go to the player closest to the core".
	{
		TArray<FCatchContender> Set;
		Set.Add({ 200.0, 11u, true });
		Set.Add({ 200.0 - static_cast<double>(Margin) - 1.0, 22u, false });

		const int32 Winner = PickContestedCatcher(Set, Margin);
		Check(Winner == 1, FString::Printf(
			TEXT("a challenger nearer by margin+1 uu takes the pull (got index %d)"), Winner));

		TArray<FCatchContender> Barely;
		Barely.Add({ 200.0, 11u, true });
		Barely.Add({ 200.0 - static_cast<double>(Margin) + 1.0, 22u, false });

		bool bHeld = false;
		const int32 HeldWinner = PickContestedCatcher(Barely, Margin, &bHeld);
		Check(HeldWinner == 0 && bHeld, FString::Printf(
			TEXT("a challenger nearer by only margin-1 uu does NOT (got index %d, held=%d)"),
			HeldWinner, bHeld ? 1 : 0));
	}

	// --- 5. AN EXACT TIE IS DETERMINISTIC, AND DOES NOT DEPEND ON ROSTER ORDER. --------------------
	{
		TArray<FCatchContender> Forward;
		Forward.Add({ 250.0, 77u, false });
		Forward.Add({ 250.0, 33u, false });

		TArray<FCatchContender> Reversed;
		Reversed.Add({ 250.0, 33u, false });
		Reversed.Add({ 250.0, 77u, false });

		const uint32 WinnerA = Forward[PickContestedCatcher(Forward, Margin)].StableKey;
		const uint32 WinnerB = Reversed[PickContestedCatcher(Reversed, Margin)].StableKey;

		Check(WinnerA == WinnerB && WinnerA == 33u, FString::Printf(
			TEXT("two players at an identical 250 uu resolve to the same one (%u) whichever order the ")
			TEXT("roster returns them in (%u vs %u)"), 33u, WinnerA, WinnerB));
	}

	// --- 6. AN INCUMBENT WHO LEAVES THE SET KEEPS NOTHING. ----------------------------------------
	//
	// Dead, out of range, or behind the Core: the caller simply stops listing them, and the pull must
	// move to whoever is left rather than being held by a player who is no longer a candidate.
	{
		TArray<FCatchContender> Set;
		Set.Add({ 380.0, 44u, false });   // the old incumbent is not in this list at all
		Set.Add({ 390.0, 55u, false });

		const int32 Winner = PickContestedCatcher(Set, Margin);
		Check(Winner == 0, FString::Printf(
			TEXT("with the incumbent gone from the set the nearest survivor takes it (got index %d)"),
			Winner));
	}

	if (Fails == 0)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBContest] ===== ALL %d CHECKS PASSED (spec v13 §5) ====="), Passes);
	}
	else
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBContest] ===== %d PASSED, %d FAILED (spec v13 §5) ====="), Passes, Fails);
	}
}

static FAutoConsoleCommand GTraceModeBContestTestCmd(
	TEXT("Trace.ModeB.ContestTest"),
	TEXT("MODE B, spec v13 §5. Drives the SHIPPED contested-magnet selection with scripted distances: ")
	TEXT("nearest wins, a near tie does not oscillate (and the zero-margin arm proves the test can see ")
	TEXT("a flicker), the margin is not cumulative, and an exact tie is order-independent."),
	FConsoleCommandDelegate::CreateStatic(&RunModeBContestTest));

// =================================================================================================
// SPEC v13 §6 — TESTING THE CHARGE CURVE. Trace.ModeB.ChargeTest
//
// The note makes four checkable claims about the mapping from hold time to momentum, and every one of
// them is a property of ATraceCore::GetThrowChargeScaleForHold - the function the throw, the HUD meter
// and the bots all call. So they are checked against that function directly rather than by watching
// throws, which cannot separate "the curve is wrong" from "the bot held the button for the wrong
// length of time".
// =================================================================================================

static void RunModeBChargeTest()
{
	int32 Passes = 0;
	int32 Fails = 0;

	auto Check = [&Passes, &Fails](bool bCondition, const FString& What)
	{
		(bCondition ? Passes : Fails)++;
		if (bCondition)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBCharge]   PASS  %s"), *What);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBCharge]   FAIL  %s"), *What);
		}
	};

	const float ChargeSeconds = ATraceCore::GetThrowChargeSeconds();
	const float Floor = ATraceCore::GetThrowChargeFloor();
	const float MaxHoldFraction = ATraceCore::GetThrowChargeMaxFraction();
	const float FullSpeed = ATraceCore::GetThrowSpeed();

	// FULL POWER IS 1.0 BY DEFINITION - a full hold reaches exactly the pre-v13 throw. The MAX knob
	// caps the HOLD, not the power, and is only reachable with the clamp off.
	constexpr float FullPower = 1.f;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBCharge] ===== spec v13 §6: charge %.2fs, floor %.2f, hold cap x%.2f, clamp %d | full ")
		TEXT("throw speed %.0f uu/s ====="),
		ChargeSeconds, Floor, MaxHoldFraction, ATraceCore::DoesThrowChargeClamp() ? 1 : 0, FullSpeed);

	// 1. "So if the player just clicks the throw button it will throw with very low momentum."
	const float Instant = ATraceCore::GetThrowChargeScaleForHold(0.f);
	Check(FMath::IsNearlyEqual(Instant, Floor, 0.001f) && Instant > 0.f,
		FString::Printf(TEXT("an instant click throws at x%.3f of full - low, and NOT zero (a zero throw ")
			TEXT("drops at the thrower's feet and reads as a bug)"), Instant));

	// 2. "Start by making a one second charge up time to reach the current core throw momentum."
	const float Full = ATraceCore::GetThrowChargeScaleForHold(ChargeSeconds);
	Check(FMath::IsNearlyEqual(Full, FullPower, 0.001f),
		FString::Printf(TEXT("a %.2fs hold reaches x%.3f, i.e. the current throw momentum (%.0f uu/s)"),
			ChargeSeconds, Full, FullSpeed * Full));

	// 3. "Charge time to throw momentum should be a linear correlation." Checked as linearity itself -
	//    equal steps in hold produce equal steps in momentum - rather than by spot-checking a midpoint,
	//    which any monotonic curve would pass.
	{
		bool bLinear = true;
		float WorstError = 0.f;
		for (int32 Step = 0; Step <= 20; ++Step)
		{
			const float T = ChargeSeconds * (static_cast<float>(Step) / 20.f);
			const float Expected = Floor + (FullPower - Floor) * (static_cast<float>(Step) / 20.f);
			const float Actual = ATraceCore::GetThrowChargeScaleForHold(T);
			WorstError = FMath::Max(WorstError, FMath::Abs(Actual - Expected));
			bLinear &= FMath::IsNearlyEqual(Actual, Expected, 0.0005f);
		}
		Check(bLinear, FString::Printf(
			TEXT("21 samples across the wind-up are linear in hold time (worst error %.5f)"), WorstError));
	}

	// 4. "[ASSUMPTION] clamp there." Holding past the charge time buys nothing more.
	if (ATraceCore::DoesThrowChargeClamp())
	{
		const float Overheld = ATraceCore::GetThrowChargeScaleForHold(ChargeSeconds * 5.f);
		Check(FMath::IsNearlyEqual(Overheld, FullPower, 0.001f),
			FString::Printf(TEXT("holding 5x the charge time still throws at x%.3f - it clamps"), Overheld));
	}

	// 5. Monotonic: a longer hold is never a weaker throw. Trivially true of a line, and the check that
	//    would catch a future non-linear curve being dropped in with the sign wrong.
	{
		bool bMonotonic = true;
		float Previous = -1.f;
		for (int32 Step = 0; Step <= 40; ++Step)
		{
			const float Scale = ATraceCore::GetThrowChargeScaleForHold(ChargeSeconds * Step / 20.f);
			bMonotonic &= (Scale >= Previous - 0.0001f);
			Previous = Scale;
		}
		Check(bMonotonic, TEXT("a longer hold is never a weaker throw"));
	}

	// 6. The inverse round-trips, which is what the bots depend on: they pick a POWER and have to turn
	//    it into a HOLD. A round-trip error here is a bot that throws short or long by that error.
	{
		bool bRoundTrips = true;
		float WorstError = 0.f;
		for (int32 Step = 0; Step <= 10; ++Step)
		{
			const float WantedScale = FMath::Lerp(Floor, FullPower, static_cast<float>(Step) / 10.f);
			const float Hold = ATraceCore::GetThrowHoldSecondsForScale(WantedScale);
			const float GotScale = ATraceCore::GetThrowChargeScaleForHold(Hold);
			WorstError = FMath::Max(WorstError, FMath::Abs(GotScale - WantedScale));
			bRoundTrips &= FMath::IsNearlyEqual(GotScale, WantedScale, 0.002f);
		}
		Check(bRoundTrips, FString::Printf(
			TEXT("power -> hold -> power round-trips for the bots (worst error %.5f)"), WorstError));
	}

	if (Fails == 0)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBCharge] ===== ALL %d CHECKS PASSED (spec v13 §6) ====="), Passes);
	}
	else
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBCharge] ===== %d PASSED, %d FAILED (spec v13 §6) ====="), Passes, Fails);
	}
}

static FAutoConsoleCommand GTraceModeBChargeTestCmd(
	TEXT("Trace.ModeB.ChargeTest"),
	TEXT("MODE B, spec v13 §6. Checks the SHIPPED charge curve: an instant click is low but not zero, a ")
	TEXT("full hold reaches exactly the current throw momentum, the ramp between them is linear, it ")
	TEXT("clamps, and the inverse the bots use round-trips."),
	FConsoleCommandDelegate::CreateStatic(&RunModeBChargeTest));

#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING

// --- Trace.ModeB.Verify: a scripted proof that each mode-B rule fires ------------------------------
//
// WHY THIS EXISTS. Mode B has four rules and a live match exercises them on its own schedule: a
// throw when a bot decides to throw, an interception when somebody happens to be in the way, a goal
// when a throw happens to go in, a reset when a Core happens to be abandoned. Waiting for all four
// to coincide inside one run is not a test, it is a hope — and "we never saw it fail" is not
// evidence a rule works. This drives each of them deliberately, in order, and prints PASS or FAIL
// with the fact it checked.
//
// It drives the REAL functions: ThrowFromHolder for the throws (so the aim, the launch, the release
// of the holder and the trail are all the shipping path) and the ordinary pickup poll for the takes.
// The only thing it fakes is WHERE the Core starts for the goal and reset cases, which is exactly the
// part a match cannot be asked to arrange on cue.

static TAutoConsoleVariable<int32> CVarModeBVerifyRequested(
	TEXT("Trace.ModeB.Verify"),
	0,
	TEXT("1: run the mode B verification scenario once (throw -> teammate recovery with NO grace, ")
	TEXT("throw -> enemy interception WITH grace, a Core thrown into the ring goal, the loose reset ")
	TEXT("timer, a bot carrying it in, a bot throwing it in, and spec v6's ground turnover)."),
	ECVF_Default);

/**
 * SPEC v7 §4. The surface steps on their own.
 *
 * A separate arming switch rather than a seventh and eighth step everybody has to sit through,
 * because steps 4 and 5 of the full scenario wait on BOTS to score and legitimately take tens of
 * seconds each. A rule about hit normals should be measurable in a few seconds, and a check that is
 * slow to run is a check that stops being run.
 */
static TAutoConsoleVariable<int32> CVarModeBVerifySurfacesRequested(
	TEXT("Trace.ModeB.VerifySurfaces"),
	0,
	TEXT("1: run spec v7 §4's surface steps only - drop the Core on the TOP of a piece of cover and ")
	TEXT("assert a TURNOVER, then fire it at a WALL and assert a BOUNCE with no turnover."),
	ECVF_Default);

bool ATraceCore::DebugLaunchLoose(const FVector& From, const FVector& LaunchVelocity, ETraceTeam FromTeam,
	bool bAsThrow)
{
	if (!HasAuthority() || !IsModeB() || bCoreStateLocked)
	{
		return false;
	}

	FCoreStateLock Lock(this);

	CancelPass(nullptr);
	ReleaseHolder();

	bLoose = true;
	bLooseAtRest = false;
	// Spec v6 §4.2 is a rule about a THROW. The scenario's reset-timer step deliberately parks a Core
	// on the floor, and a Core that was never thrown must not turn over on contact with it - otherwise
	// that step would silently stop testing the timer and start testing the turnover.
	bLooseFromThrow = bAsThrow;
	CatchZoneTarget = nullptr;
	ForgetLastContact();      // As the real throw above.
	ClearPendingTurnover();   // Spec v19 §1.5: as the real throw above.
	bTurnoverRegisteredThisFlight = false;   // Spec v25 §2: as the real throw above.
	LooseFromTeam = FromTeam;
	LooseThrower = nullptr;
	LooseStartServerTime = GetServerTimeSeconds();
	LooseLocation = From;
	LooseVelocity = LaunchVelocity;

	// SPEC v10 §10: the debug launch is a teleport like any other, and it goes through the same funnel
	// so the verification scenario is testing the shipping path rather than a private one.
	ServerTeleport(From, TEXT("debug launch"));
	ApplyAttachment();
	UpdateVisuals();
	ForceNetUpdate();
	return true;
}

void ATraceCore::TickModeBVerification()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	// --- Arm --------------------------------------------------------------------------------------
	if (VerifyStep < 0)
	{
		const bool bFullRequested = (CVarModeBVerifyRequested.GetValueOnAnyThread() != 0);
		const bool bSurfacesRequested = (CVarModeBVerifySurfacesRequested.GetValueOnAnyThread() != 0);

		if (!bFullRequested && !bSurfacesRequested)
		{
			return;
		}
		if (!IsModeB())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] refused: the match is playing mode A."));
			CVarModeBVerifyRequested->Set(0, ECVF_SetByConsole);
			CVarModeBVerifySurfacesRequested->Set(0, ECVF_SetByConsole);
			return;
		}
		// Wait for a settled, running half. Arming during the pre-match window put the very first
		// throw on the same frame as the 1st-half kickoff, which cancelled it and made the scenario
		// report a failure of a rule that had never run.
		const ATraceGameState* GameState = World->GetGameState<ATraceGameState>();
		if (GameState == nullptr
			|| GameState->TraceMatchState != ETraceMatchState::InProgress
			|| GameState->IsHalfTimeBreak()
			|| !IsValid(Carrier))
		{
			return;
		}

		// SURFACES-ONLY starts at step 7. The full scenario still runs 0..8, so the v7 steps are also
		// exercised by whatever already runs Trace.ModeB.Verify — one set of steps, two ways in.
		bVerifySurfacesOnly = (!bFullRequested && bSurfacesRequested);
		VerifyStep = bVerifySurfacesOnly ? 7 : 0;
		VerifyPassCount = 0;
		VerifyFailCount = 0;
		VerifyStepDeadline = 0.f;
		UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] ===== mode B verification starting (%s) ====="),
			bVerifySurfacesOnly ? TEXT("spec v7 §4 surface rules only") : TEXT("all rules"));
	}

	// --- A step that is waiting on an outcome -----------------------------------------------------
	if (VerifyStepDeadline > 0.f)
	{
		const bool bTimedOut = (Now >= VerifyStepDeadline);

		// STEP 8 ARMS ITSELF LATE, and that is the whole design of it. It parks a Core on top of an
		// object as something that was NOT thrown, so no rule may touch it, and waits for it to come to
		// rest. Only then does it declare the Core "thrown" - at which point the flight integration is
		// already switched off for good and NO further sweep will ever run. The turnover that follows
		// can therefore only have come from the at-rest probe, which is the exact code path the user's
		// "stuck up top of an object" report is about. Anything else would be testing the contact test
		// step 7 already covers.
		if (VerifyStep == 8 && !bVerifyRestArmed && !bTimedOut)
		{
			// SPEC v25 §2 SIDE EFFECT, and it is a change in the WORLD rather than in this step. Step 7
			// now leaves its Core lying on the crate for the whole lockout window, so the bots gather
			// there — and step 8's parked Core is then taken on its first frame, before it can settle,
			// and the at-rest probe this step exists to exercise never runs. Take the subject back
			// rather than reporting a rule that was never given a chance.
			if (!bLoose && VerifyRestParkRetriesLeft > 0)
			{
				--VerifyRestParkRetriesLeft;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ModeBVerify] step 8: the parked Core was taken before it could settle (%s has it) ")
					TEXT("- re-parking, %d retries left."),
					*GetNameSafe(Carrier), VerifyRestParkRetriesLeft);

				VerifyStepDeadline = 0.f;   // Re-enters case 8 below, which parks it again.
				return;
			}

			if (!bLoose || !bLooseAtRest)
			{
				return;   // Still falling. Nothing to arm yet.
			}

			bVerifyRestArmed = true;
			bVerifyAwaitingTake = true;
			bVerifyTakeSeen = false;
			bVerifyAwaitingTurnover = true;   // Spec v25 §2: the registration is what this now judges.
			bVerifyTurnoverSeen = false;
			bLooseFromThrow = true;
			VerifyTurnoversAtStart = SurfaceStats.TopTurnovers;
			VerifyRescuesAtStart = SurfaceStats.RestProbeRescues;

			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeBVerify] step 8: the Core is now AT REST at %s with the flight integration off - ")
				TEXT("arming the throw flag. Only the at-rest probe can turn it over from here."),
				*FVector(LooseLocation).ToCompactString());
			return;
		}

		// =========================================================================================
		// SPEC v25 §2 CHANGED WHAT STEPS 6-8 ARE WAITING FOR, AND THIS IS WHERE THAT LANDS.
		//
		// Those three steps have always asserted "a throw settled on a surface and the enemy ended up
		// with it". Under v25 the second half is no longer what the rule does: the Core STAYS on the
		// ground and the enemy gets a window in which they alone may pull it or run over it. So the
		// assertion moves one step earlier, onto the REGISTRATION - the throwing team is locked out,
		// its opponent is the side that may take it - and the surface tallies and the at-rest-probe
		// clause, which are the parts that actually prove which geometry fired, are unchanged.
		//
		// The pre-v25 arm (Trace.ModeB.TurnoverPull 0) still falls through to the take-based judgement
		// below, so ONE scenario measures whichever rule is armed rather than two that could drift.
		if (VerifyStep >= 6 && VerifyStep <= 8 && TraceModeBTuning::TurnoverPullEnabled())
		{
			if (bVerifyTurnoverSeen)
			{
				bVerifyTurnoverSeen = false;
				bVerifyAwaitingTurnover = false;
				bVerifyAwaitingTake = false;

				const bool bLockedTheThrower = (VerifyTurnoverLockedTeam != ETraceTeam::None)
					&& (TraceOpposingTeam(VerifyTurnoverLockedTeam) == VerifyExpectTeam);
				const bool bSurfaceOk = (VerifyStep == 6)
					|| (SurfaceStats.TopTurnovers > VerifyTurnoversAtStart);
				const bool bProbeOk = (VerifyStep != 8)
					|| (SurfaceStats.RestProbeRescues > VerifyRescuesAtStart);
				const bool bStepOk = bLockedTheThrower && bSurfaceOk && bProbeOk && bLoose;

				(bStepOk ? VerifyPassCount : VerifyFailCount)++;

				const FString StepDetail = FString::Printf(
					TEXT("a throw settled on %s and REGISTERED a turnover: %s locked out for %.1fs, %s may ")
					TEXT("pull or run over it, Core still loose=%d | top-of-object turnovers %d -> %d | ")
					TEXT("at-rest probe catches %d -> %d%s%s"),
					(VerifyStep == 6) ? TEXT("the ground") : TEXT("THE TOP OF AN OBJECT"),
					*TraceTeamName(VerifyTurnoverLockedTeam).ToString(), GetTurnoverLockoutSeconds(),
					*TraceTeamName(TraceOpposingTeam(VerifyTurnoverLockedTeam)).ToString(),
					bLoose ? 1 : 0,
					VerifyTurnoversAtStart, SurfaceStats.TopTurnovers,
					VerifyRescuesAtStart, SurfaceStats.RestProbeRescues,
					bSurfaceOk ? TEXT("") : TEXT(" *** it did not land on a raised surface ***"),
					bProbeOk ? TEXT("") : TEXT(" *** the at-rest probe is not what caught it ***"));

				if (bStepOk)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step %d PASS: %s"), VerifyStep, *StepDetail);
				}
				else
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: %s"), VerifyStep, *StepDetail);
				}

				VerifyStepDeadline = 0.f;
				++VerifyStep;
				return;
			}

			if (!bTimedOut)
			{
				return;   // Still falling, or still settling. The timeout below is the only way out.
			}
		}

		// Steps 0, 1, 6, 7 and 8 are waiting for a taker; steps 2 and 3 are waiting for the Core to
		// stop being loose (a goal or a reset both end with a kickoff); step 9 is waiting for a bounce.
		if (VerifyStep <= 1 || (VerifyStep >= 6 && VerifyStep <= 8))
		{
			if (bVerifyTakeSeen)
			{
				bVerifyTakeSeen = false;

				// --- Steps 6, 7 and 8: the turnover rule. --------------------------------------------
				//
				// Judged apart because they make a STRONGER claim than steps 0 and 1 do. Those two are
				// happy with whoever won the race to the Core; these assert the specific outcome the
				// rule promises - the enemy of the throwing team has it, and (because that crosses
				// sides) with the turnover grace applied. Anything else is a failure even if the grace
				// bookkeeping was internally consistent.
				//
				// Step 6 lands on the FLOOR (spec v6 §4.2), step 7 on the TOP OF AN OBJECT by contact,
				// step 8 on the top of an object with the contact test unable to run at all (spec v7
				// §4). One assertion for all three, because that is exactly the claim being made: they
				// are the same event and they have to end the same way.
				if (VerifyStep >= 6 && VerifyStep <= 8)
				{
					const bool bWentToTheEnemy = (VerifyFromTeam != ETraceTeam::None)
						&& (VerifyTookTeam == TraceOpposingTeam(VerifyFromTeam));
					const bool bGraceOk = bVerifyTookGrace
						&& bLastGrantTeamChanged && LastGrantGraceSeconds > 0.f;

					// STEPS 7 AND 8 ASSERT THE SURFACE TOO, and they have to: a turnover that fired
					// because the Core rolled off the crate and landed on the FLOOR would satisfy every
					// other clause here while testing precisely the rule that already worked. Step 8
					// additionally asserts that the AT-REST PROBE is what caught it. Both read the tally
					// the rule itself keeps, rather than re-deriving the geometry.
					const bool bSurfaceOk = (VerifyStep == 6)
						|| (SurfaceStats.TopTurnovers > VerifyTurnoversAtStart);
					const bool bProbeOk = (VerifyStep != 8)
						|| (SurfaceStats.RestProbeRescues > VerifyRescuesAtStart);
					const bool bStepOk = bWentToTheEnemy && bGraceOk && bSurfaceOk && bProbeOk;

					(bStepOk ? VerifyPassCount : VerifyFailCount)++;

					// Two calls rather than a ternary verbosity: UE_LOG pastes its second argument
					// into ELogVerbosity::<token>, so the level has to be a literal.
					const FString StepDetail = FString::Printf(
						TEXT("a %s throw settled on %s and turned over to %s (%s) | expected the nearest %s ")
						TEXT("player, with grace | grace %s | top-of-object turnovers %d -> %d | at-rest ")
						TEXT("probe catches %d -> %d%s%s"),
						*TraceTeamName(VerifyFromTeam).ToString(),
						(VerifyStep == 6) ? TEXT("the ground") : TEXT("THE TOP OF AN OBJECT"),
						*VerifyTakerName, *TraceTeamName(VerifyTookTeam).ToString(),
						*TraceTeamName(TraceOpposingTeam(VerifyFromTeam)).ToString(),
						bGraceOk ? *FString::Printf(TEXT("APPLIED %.2fs"), LastGrantGraceSeconds) : TEXT("MISSING"),
						VerifyTurnoversAtStart, SurfaceStats.TopTurnovers,
						VerifyRescuesAtStart, SurfaceStats.RestProbeRescues,
						bSurfaceOk ? TEXT("") : TEXT(" *** it did not land on a raised surface ***"),
						bProbeOk ? TEXT("") : TEXT(" *** the at-rest probe is not what caught it ***"));

					if (bStepOk)
					{
						UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step %d PASS: %s"), VerifyStep, *StepDetail);
					}
					else
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: %s"), VerifyStep, *StepDetail);
					}

					VerifyStepDeadline = 0.f;
					++VerifyStep;
					return;
				}

				// THE RULE, judged on the take that actually happened rather than on who was predicted
				// to win the race. Spec v4 §7 is a statement about TEAMS, not about players: grace iff
				// the Core crossed sides. "First contact takes it" means the thrower can legitimately
				// beat the intended receiver to their own throw, and that is not a failure - the rule
				// still has to hold for whoever got there.
				const bool bShouldGrace = (VerifyTookTeam != VerifyFromTeam);
				const bool bOk = (bVerifyTookGrace == bShouldGrace)
					&& (bVerifyTookGrace == (bLastGrantTeamChanged && LastGrantGraceSeconds > 0.f));

				(bOk ? VerifyPassCount : VerifyFailCount)++;

				UE_LOG(LogTraceGame, Display,
					TEXT("[ModeBVerify] step %d %s: %s (%s) took the thrown Core | grace %s, rule says %s"),
					VerifyStep, bOk ? TEXT("PASS") : TEXT("FAIL"),
					*VerifyTakerName, *TraceTeamName(VerifyTookTeam).ToString(),
					bVerifyTookGrace ? *FString::Printf(TEXT("APPLIED %.2fs"), LastGrantGraceSeconds) : TEXT("none"),
					bShouldGrace ? TEXT("APPLIED (crossed teams)") : TEXT("none (same team)"));

				VerifyStepDeadline = 0.f;
				++VerifyStep;
				return;
			}
		}
		else if (VerifyStep == 9)
		{
			// --- Step 9: SPEC v7 §4, "walls should not [turn over], the core should bounce off those".
			//
			// TWO facts, and both are needed. That a bounce HAPPENED is the positive half - counted by
			// the rule itself at the moment it classified the normal as a wall. That NO TURNOVER
			// happened is the negative half, and it is the one the user actually asked for: a wall that
			// handed the Core to the nearest enemy would be the bug, not the bounce.
			//
			// Judged the instant the bounce is seen rather than at the end of a fixed window, because
			// the Core CORRECTLY falls to the floor afterwards and turns over there - waiting would
			// measure the floor rule and report the wall rule broken.
			const int32 BouncesNow = SurfaceStats.WallBounces;
			const int32 TurnoversNow = SurfaceStats.GroundTurnovers + SurfaceStats.TopTurnovers;

			if (BouncesNow > VerifyWallBouncesAtStart)
			{
				const bool bNoTurnover = (TurnoversNow == VerifyTurnoversAtStart);
				const bool bStillLoose = bLoose;
				const bool bStepOk = bNoTurnover && bStillLoose;

				(bStepOk ? VerifyPassCount : VerifyFailCount)++;

				const FString StepDetail = FString::Printf(
					TEXT("the Core struck a WALL and bounced (wall bounces %d -> %d) | turnovers %d -> %d ")
					TEXT("(must not move) | still loose=%d | velocity %s"),
					VerifyWallBouncesAtStart, BouncesNow, VerifyTurnoversAtStart, TurnoversNow,
					bStillLoose ? 1 : 0, *FVector(LooseVelocity).ToCompactString());

				if (bStepOk)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step 9 PASS: %s"), *StepDetail);
				}
				else
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 9 FAIL: %s"), *StepDetail);
				}

				VerifyStepDeadline = 0.f;
				++VerifyStep;
				return;
			}

			if (bTimedOut)
			{
				// RE-FIRE RATHER THAN FAIL, up to a few times, and this is a correction to the harness
				// rather than a leniency. [DIAGNOSED] from three runs of its own log: the shot is fired
				// across open pitch with ten bots playing on it, and what actually happens on a failing
				// run is that a bot standing in the corridor INTERCEPTS the Core 0.09s after launch —
				// the catch-zone magnet even curves it into them — so the step never tests the wall at
				// all. It then reports the WALL RULE broken on the strength of where a bot was standing.
				//
				// It surfaced this pass because spec v19 §1.5's settle moves step 8's turnover 0.15s
				// later, which reshuffles where everybody is when step 9 fires; the underlying weakness
				// is older than that and was passing by luck. A retry makes the step measure the thing
				// it names. When the retries run out it still FAILS, loudly, and says which it was.
				if (VerifyWallShotRetriesLeft > 0)
				{
					--VerifyWallShotRetriesLeft;
					UE_LOG(LogTraceGame, Display,
						TEXT("[ModeBVerify] step 9: the shot never reached the wall (a bot took it, or it was ")
						TEXT("reset) - RE-FIRING, %d attempt(s) left."),
						VerifyWallShotRetriesLeft);

					VerifyStepDeadline = 0.f;
					return;   // VerifyStep is unchanged, so the launcher below fires it again.
				}

				++VerifyFailCount;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ModeBVerify] step 9 FAIL: the Core never struck the wall in any attempt (loose=%d, ")
					TEXT("at rest=%d, at %s, turnovers %d -> %d)."),
					bLoose ? 1 : 0, bLooseAtRest ? 1 : 0, *FVector(LooseLocation).ToCompactString(),
					VerifyTurnoversAtStart, TurnoversNow);

				VerifyStepDeadline = 0.f;
				++VerifyStep;
			}
			return;
		}
		else if (VerifyStep == 4 || VerifyStep == 5)
		{
			// --- Steps 4 and 5: the BOT paths. -----------------------------------------------------
			//
			// Judged on ATraceCore::GoalsByMethod rather than on the scoreboard, because the whole
			// point of these two steps is WHICH path scored. A carrier that wanders in and a Core that
			// is thrown in both add one to the same score.
			const int32 MethodIndex = (VerifyStep == 4)
				? static_cast<int32>(EGoalMethod::Carried) : static_cast<int32>(EGoalMethod::Thrown);

			if (VerifyStep == 5 && bLoose)
			{
				bVerifyThrowSeen = true;   // A throw left the bot's hands. Whether it goes in is next.
			}

			const int32 TallyNow = GoalsByMethod[MethodIndex];
			if (TallyNow > VerifyGoalTallyAtStart)
			{
				++VerifyPassCount;
				UE_LOG(LogTraceGame, Display,
					TEXT("[ModeBVerify] step %d PASS: a bot scored by %s (%s goals %d -> %d)."),
					VerifyStep, (VerifyStep == 4) ? TEXT("CARRYING THE CORE IN") : TEXT("THROWING AT THE GOAL"),
					(VerifyStep == 4) ? TEXT("carried") : TEXT("thrown"), VerifyGoalTallyAtStart, TallyNow);

				VerifyStepDeadline = 0.f;
				bVerifyThrowSeen = false;
				++VerifyStep;
				return;
			}

			if (bTimedOut)
			{
				++VerifyFailCount;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ModeBVerify] step %d FAIL: no goal by %s inside the window (carrier=%s, loose=%d, ")
					TEXT("a throw did%s leave)."),
					VerifyStep, (VerifyStep == 4) ? TEXT("carrying in") : TEXT("throwing at the goal"),
					*GetNameSafe(Carrier), bLoose ? 1 : 0, bVerifyThrowSeen ? TEXT("") : TEXT(" NOT"));

				VerifyStepDeadline = 0.f;
				bVerifyThrowSeen = false;
				++VerifyStep;
			}
			return;
		}
		else if (!bLoose)
		{
			// Step 2 is the goal, and "the Core stopped being loose" is not enough on its own — a
			// pickup or a reset would look the same from here. The point on the scoreboard is the
			// fact under test, so that is what is checked.
			bool bOk = true;
			FString Detail = TEXT("the loose Core left play as expected");

			if (VerifyStep == 2)
			{
				int32 ScoreNow = 0;
				if (const ATraceGameState* GameState = World->GetGameState<ATraceGameState>())
				{
					ScoreNow = GameState->GetScore(VerifyExpectTeam);
				}
				bOk = (ScoreNow > VerifyGoalsAtStart);
				Detail = FString::Printf(TEXT("%s score %d -> %d"),
					*TraceTeamName(VerifyExpectTeam).ToString(), VerifyGoalsAtStart, ScoreNow);
			}

			(bOk ? VerifyPassCount : VerifyFailCount)++;
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step %d %s: %s."),
				VerifyStep, bOk ? TEXT("PASS") : TEXT("FAIL"), *Detail);

			VerifyStepDeadline = 0.f;
			++VerifyStep;
			return;
		}

		if (bTimedOut)
		{
			++VerifyFailCount;
			bVerifyAwaitingTake = false;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: timed out (loose=%d, carrier=%s)."),
				VerifyStep, bLoose ? 1 : 0, *GetNameSafe(Carrier));
			VerifyStepDeadline = 0.f;
			++VerifyStep;
		}
		return;
	}

	// --- Start the next step ----------------------------------------------------------------------
	TArray<ATraceCharacter*> Everyone;
	GatherCharacters(Everyone);

	switch (VerifyStep)
	{
	case 0:
	case 1:
	{
		// A real throw, aimed at a real target, through ThrowFromHolder.
		if (!IsValid(Carrier) || !Carrier->IsAlive())
		{
			return;
		}

		const bool bWantTeammate = (VerifyStep == 0);
		const ETraceTeam HolderTeam = Carrier->GetTeam();

		ATraceCharacter* Target = nullptr;
		double BestDistSq = TNumericLimits<double>::Max();
		for (ATraceCharacter* Candidate : Everyone)
		{
			if (!IsValid(Candidate) || Candidate == Carrier || !Candidate->IsAlive())
			{
				continue;
			}
			const bool bIsMate = AreAllies(HolderTeam, Candidate->GetTeam());
			if (bIsMate != bWantTeammate)
			{
				continue;
			}
			const double DistSq = FVector::DistSquared(Carrier->GetActorLocation(), Candidate->GetActorLocation());
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Target = Candidate;
			}
		}

		if (Target == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d SKIPPED: no living %s to throw at."),
				VerifyStep, bWantTeammate ? TEXT("teammate") : TEXT("enemy"));
			++VerifyStep;
			return;
		}

		// Put the target within arm's reach of the throw so the outcome is about the RULE (who may
		// take it, and what grace they get) rather than about a bot's ability to run somewhere.
		const FVector Behind = Carrier->GetActorLocation()
			+ (Carrier->GetActorForwardVector().GetSafeNormal2D() * 420.0);
		Target->SetActorLocation(FVector(Behind.X, Behind.Y, Target->GetActorLocation().Z),
			false, nullptr, ETeleportType::TeleportPhysics);

		if (AController* HolderController = Carrier->GetController())
		{
			const FVector ToTarget = Target->GetActorLocation() - Carrier->GetPawnViewLocation();
			if (!ToTarget.IsNearlyZero())
			{
				HolderController->SetControlRotation(ToTarget.Rotation());
			}
		}

		VerifyThrower = Carrier;
		VerifyExpectTeam = Target->GetTeam();
		VerifyExpectGrace = !AreAllies(HolderTeam, Target->GetTeam());

		ThrowCooldownEndServerTime = 0.f;   // The scenario is not testing the cooldown.
		bVerifyAwaitingTake = true;
		bVerifyTakeSeen = false;

		if (!ThrowFromHolder(Carrier))
		{
			++VerifyFailCount;
			bVerifyAwaitingTake = false;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: ThrowFromHolder refused."), VerifyStep);
			++VerifyStep;
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step %d: thrown at %s (%s) - expecting %s with %s grace."),
			VerifyStep, *GetNameSafe(Target), bWantTeammate ? TEXT("teammate") : TEXT("enemy"),
			*TraceTeamName(VerifyExpectTeam).ToString(), VerifyExpectGrace ? TEXT("a") : TEXT("no"));

		VerifyStepDeadline = Now + 4.f;
		return;
	}

	case 2:
	{
		// A Core thrown INTO the goal. Launched from just outside the mouth, moving into it, so the
		// swept goal test in ServerTickLooseCore is what has to catch it.
		const ETraceTeam Attacker = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::Blue;

		FVector GoalCentre = FVector::ZeroVector;
		if (!GetAttackGoalCentre(World, Attacker, GoalCentre))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 2 FAIL: no goal box resolved for %s."),
				*TraceTeamName(Attacker).ToString());
			++VerifyStep;
			return;
		}

		// Approach along X from the field side, so "into the goal" is unambiguous whichever end it is.
		const double FieldCentreX = [World]() -> double
		{
			if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
			{
				return Arena->GetFieldBounds().GetCenter().X;
			}
			return 0.0;
		}();

		const double Sign = (GoalCentre.X >= FieldCentreX) ? 1.0 : -1.0;
		const FVector Start = GoalCentre - FVector(Sign * 1200.0, 0.0, 0.0);
		const FVector LaunchVelocity = FVector(Sign * 2400.0, 0.0, 200.0);

		VerifyGoalsAtStart = 0;
		VerifyExpectTeam = Attacker;
		if (const ATraceGameState* GameState = World->GetGameState<ATraceGameState>())
		{
			VerifyGoalsAtStart = GameState->GetScore(Attacker);
		}

		if (!DebugLaunchLoose(Start, LaunchVelocity, Attacker))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 2 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 2: %s Core launched at the goal mouth %s from %s - expecting a GOAL."),
			*TraceTeamName(Attacker).ToString(), *GoalCentre.ToCompactString(), *Start.ToCompactString());

		VerifyStepDeadline = Now + 4.f;
		return;
	}

	case 3:
	{
		// The reset timer. Launched straight down into the floor in a corner with a back-dated start
		// time, so it comes to rest untouched and the timer is the only thing that can end it.
		const ETraceTeam FromTeam = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::Blue;

		FVector Corner = GetHomeLocation() + FVector(0.0, 0.0, 400.0);
		if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
		{
			const FBox FieldBox = Arena->GetFieldBounds();
			if (FieldBox.IsValid != 0)
			{
				Corner = FVector(FieldBox.GetCenter().X, FieldBox.Max.Y - 600.0, FieldBox.Min.Z + 400.0);
			}
		}

		// bAsThrow = FALSE. Spec v6 §4.2 turns a thrown Core over the instant it touches the floor, and
		// this step's entire premise is a Core that LIES on the floor untouched until the timer fires.
		// Launching it as a throw would hand it to the nearest enemy on the first frame and this step
		// would silently become a second (worse) test of the turnover rule.
		if (!DebugLaunchLoose(Corner, FVector(0.0, 0.0, -50.0), FromTeam, /*bAsThrow=*/false))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 3 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		// Back-date the clock rather than waiting out the real timer: the rule under test is "a loose
		// Core is put back into play once it has been ignored for long enough", not the wall clock.
		LooseStartServerTime = Now - (TraceModeBTuning::LooseResetSeconds() - 2.f);

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 3: Core parked at %s with its reset timer 2s from expiry - expecting a reset."),
			*Corner.ToCompactString());

		VerifyStepDeadline = Now + 8.f;
		return;
	}

	case 4:
	case 5:
	{
		// =========================================================================================
		// THE TWO BOT PATHS (spec v5 §4 follow-up).
		//
		// Step 4: a bot with the Core, 2800 uu from its own attacking mouth, must RUN IT IN.
		// Step 5: a bot with the Core, 5200 uu out, must SHOOT AT THE GOAL.
		//
		// WHY THIS IS SCRIPTED AND NOT OBSERVED. Both branches are gated on the carrier being within
		// ballistic range of the mouth, and a measured run says a carrier never is: over three runs
		// the closest any carrier came to the goal was 16676 uu, against a Core that carries ~7300 uu
		// on its best arc. So "we watched for ten minutes and never saw it" is evidence about the
		// pitch, not about the branch — which is exactly the trap the previous pass fell into when it
		// reported the carry-in path dead. This puts a carrier where the decision is taken, and then
		// changes NOTHING else: the bot's own UpdateThrow / UpdateCarryInCommit / BehaviourCarryToGoal
		// make every choice from there, through the same inputs a human's mouse reaches.
		// =========================================================================================
		if (!IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;   // Between possessions. Try again next tick; the scenario is not on a clock yet.
		}

		if (Carrier->GetController() == nullptr || Carrier->GetController()->IsPlayerController())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] step %d SKIPPED: the carrier is not a bot, so there is no bot decision to test."),
				VerifyStep);
			++VerifyStep;
			return;
		}

		const ETraceTeam Attacker = Carrier->GetTeam();

		FBox GoalBox(ForceInit);
		if (!GetAttackGoalBox(World, Attacker, GoalBox))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: no goal box resolved for %s."),
				VerifyStep, *TraceTeamName(Attacker).ToString());
			++VerifyStep;
			return;
		}

		// The MOUTH, not the box centre: the box runs from the goal line back to the end wall.
		const double FieldCentreX = [World]() -> double
		{
			if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
			{
				return Arena->GetFieldBounds().GetCenter().X;
			}
			return 0.0;
		}();

		const double Sign = (GoalBox.GetCenter().X >= FieldCentreX) ? 1.0 : -1.0;
		const double MouthX = (Sign > 0.0) ? GoalBox.Min.X : GoalBox.Max.X;

		// 700 uu is inside the carry-in commit band and about a second of running - long enough that
		// the bot has to CHOOSE to run rather than being dropped on the line, short enough that the
		// step measures that decision and not the carrier's odds of surviving a defended mouth.
		// MEASURED: at 2800 and again at 1500 the bot committed (the [BotCarryIn] line proves it) and
		// was then shot off the Core inside 1.2 s by the two defenders holding the mouth, so those
		// distances tested the defence, not the branch.
		// 5200 is outside the commit band and inside throwing range.
		//
		// STEP 5 IS 4800, AND THE FLOOR UNDER IT IS NOT THE BALLISTICS - IT IS THE CARRY-IN BAND.
		// MEASURED, twice, and worth writing down because the obvious reasoning is wrong twice over.
		// Spec v6 moved the goal 2400 uu further away and 1100 uu up, so the first instinct was that
		// the shot must now be taken from closer and this step was set to 2600. It failed three runs
		// running with "a throw did NOT leave" - and the reason is that UpdateThrow returns early
		// while bCommitCarryIn is set, and TraceBotConstants::CarryInCommitDistance is 4200 uu. At
		// 2600 the bot was not refusing the shot, it was correctly RUNNING IT IN, and the step was
		// asking a carrier inside the commit band to do the one thing the commit exists to stop.
		//
		// The range worry was unfounded in the bargain: the same run measured a live bot taking (and
		// scoring) a shot at the hoop from 4835 uu, because MaxThrowRange solves over launch ANGLE and
		// a lofted arc carries far further than the flat-throw figure the tuning log prints.
		//
		// So: outside the commit band, inside the measured reach.
		const double Standoff = (VerifyStep == 4) ? 700.0 : 4800.0;
		// PICK A SPOT WITH A CLEAR VIEW OF THE MOUTH. The arena is full of cover, and the first
		// attempt at this step dropped the carrier 58 uu in front of a 3x-height cover box: the bot
		// correctly refused every shot ("blocked=18 of 18") and the step looked like a broken
		// decision when it was a broken placement. Try the centre line first, then a few lanes
		// either side, and take the first one that can actually see what it is being asked to
		// attack. ECC_Visibility is the channel the bot's own lane test uses.
		const double GoalZ = GoalBox.Min.Z + (GoalBox.Max.Z - GoalBox.Min.Z) * 0.5;
		const double CarrierZ = Carrier->GetActorLocation().Z;

		FVector Where(MouthX - Sign * Standoff, GoalBox.GetCenter().Y, CarrierZ);
		{
			const FVector MouthPoint(MouthX, GoalBox.GetCenter().Y, GoalZ);
			FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreVerifyPlacement), false, Carrier);

			static const double LaneOffsets[] = { 0.0, 600.0, -600.0, 1200.0, -1200.0 };
			for (const double Offset : LaneOffsets)
			{
				const FVector Candidate(MouthX - Sign * Standoff, GoalBox.GetCenter().Y + Offset, CarrierZ);
				const FVector Eye = Candidate + FVector(0.0, 0.0, 64.0);
				if (!World->LineTraceTestByChannel(Eye, MouthPoint, ECC_Visibility, Params))
				{
					Where = Candidate;
					break;
				}
			}
		}

		Carrier->SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);
		LastCarrierGoalTestLocation = Where;   // Do not sweep the teleport itself through the goal.

		if (AController* BotController = Carrier->GetController())
		{
			const FVector ToGoal = FVector(MouthX, GoalBox.GetCenter().Y, GoalZ) - Carrier->GetPawnViewLocation();
			if (!ToGoal.IsNearlyZero())
			{
				BotController->SetControlRotation(ToGoal.Rotation());
			}
		}

		VerifyGoalTallyAtStart = GoalsByMethod[(VerifyStep == 4)
			? static_cast<int32>(EGoalMethod::Carried) : static_cast<int32>(EGoalMethod::Thrown)];
		bVerifyThrowSeen = false;
		ThrowCooldownEndServerTime = 0.f;   // The scenario is not testing the cooldown.

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step %d: %s (%s) placed %.0f uu from its goal mouth at %s, facing it - expecting a goal by %s."),
			VerifyStep, *GetNameSafe(Carrier), *TraceTeamName(Attacker).ToString(), Standoff,
			*Where.ToCompactString(), (VerifyStep == 4) ? TEXT("CARRYING IN") : TEXT("THROWING"));

		// Generous: the bot has to run 2800 uu (step 4) or wait out a throw cooldown, slew onto a
		// lofted solution and watch a 2-3 second arc land (step 5).
		VerifyStepDeadline = Now + ((VerifyStep == 4) ? 16.f : 14.f);
		return;
	}

	case 6:
	{
		// =========================================================================================
		// SPEC v6 §4.2: A THROWN CORE THAT HITS THE GROUND IS THE ENEMY'S.
		//
		// Scripted for the same reason steps 2 and 3 are: waiting for a bot to happen to throw one
		// into the dirt in front of an enemy is a hope, not a test. This throws it at the floor
		// deliberately - from the real holder, through the same DebugLaunchLoose the goal step uses,
		// flagged as a throw so the rule is armed - and then asserts the outcome the rule promises.
		// =========================================================================================
		if (!IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;   // Between possessions. Not on a clock yet.
		}

		const ETraceTeam FromTeam = Carrier->GetTeam();
		const ETraceTeam ToTeam = TraceOpposingTeam(FromTeam);

		// Put an enemy where the throw will land, so "the CLOSEST player on the enemy team" is a fact
		// the step controls rather than an accident of where ten bots happen to be standing. A second
		// enemy is deliberately left wherever they are: if the rule picked the wrong one, the taker
		// name in the PASS line is what says so.
		ATraceCharacter* Nearest = nullptr;
		for (ATraceCharacter* Candidate : Everyone)
		{
			if (IsValid(Candidate) && Candidate->IsAlive() && Candidate->GetTeam() == ToTeam)
			{
				Nearest = Candidate;
				break;
			}
		}

		if (Nearest == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] step 6 SKIPPED: no living %s player to award the turnover to."),
				*TraceTeamName(ToTeam).ToString());
			++VerifyStep;
			return;
		}

		// 2200 uu AHEAD OF THE THROWER, not 900. MEASURED: at 900 the ex-carrier - who becomes a loose
		// Core chaser the instant they are released - was inside their own 500 uu catch zone of the
		// falling Core within 140 ms and caught it in the air, and the step reported the turnover rule
		// broken when what it had actually observed was §4.1 working. The drop has to land somewhere
		// nobody can reach first, or this measures the magnet.
		const FVector CarrierLocation = Carrier->GetActorLocation();
		const FVector Forward = Carrier->GetActorForwardVector().GetSafeNormal2D();
		const FVector LandingSpot = CarrierLocation + Forward * 2200.0;

		// 700 uu, NOT arm's length. The catch zone (§4.1) reaches 500 uu from a capsule's surface, so
		// an enemy any closer would magnet the falling Core into their hands BEFORE it landed and the
		// step would quietly become a second test of the magnet instead of a test of the turnover.
		Nearest->SetActorLocation(
			FVector(LandingSpot.X + 700.0, LandingSpot.Y, Nearest->GetActorLocation().Z),
			false, nullptr, ETeleportType::TeleportPhysics);

		// Aimed DOWN, HARD and from LOW: a throw that is going to hit the ground and nothing else,
		// inside two or three frames, so there is no flight for anybody to intercept and the step
		// measures the landing. Low enough that it cannot cross a goal or clip a cover block on the
		// way, which would end the step for the wrong reason.
		const FVector Start = LandingSpot + FVector(0.0, 0.0, 220.0);

		VerifyThrower = Carrier;
		VerifyExpectTeam = ToTeam;
		VerifyExpectGrace = true;
		bVerifyAwaitingTake = true;
		bVerifyTakeSeen = false;
		bVerifyAwaitingTurnover = true;   // Spec v25 §2: the registration is what this now judges.
		bVerifyTurnoverSeen = false;

		if (!DebugLaunchLoose(Start, FVector(0.0, 0.0, -2000.0), FromTeam, /*bAsThrow=*/true))
		{
			++VerifyFailCount;
			bVerifyAwaitingTake = false;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 6 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 6: a %s throw dropped at %s with %s (%s) standing 700 uu away - ")
			TEXT("expecting a GROUND TURNOVER to them, with grace."),
			*TraceTeamName(FromTeam).ToString(), *LandingSpot.ToCompactString(),
			*GetNameSafe(Nearest), *TraceTeamName(ToTeam).ToString());

		VerifyStepDeadline = Now + 4.f;
		return;
	}

	case 7:
	{
		// =========================================================================================
		// SPEC v7 §4, HALF ONE: THE TOP OF AN OBJECT IS A TURNOVER.
		//
		// "Sometimes the core gets stuck up top of an object in gamemode b. This should also count as
		// a turnover."
		//
		// Step 6 proves the FLOOR case. This proves the case the user actually reported, and it has to
		// be driven rather than waited for: a bot throw that happens to come down on the roof of a
		// crate, with an enemy near enough to receive it, is not something a test run can be promised.
		// The Core is dropped from just above a real, world-sampled raised surface, and the step then
		// asserts the same three things step 6 does PLUS that the tally recorded a TOP turnover.
		// =========================================================================================
		if (!IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;   // Between possessions. Not on a clock yet.
		}

		FVector TopPoint = FVector::ZeroVector;
		FVector WallPoint = FVector::ZeroVector;
		FVector WallNormal = FVector::ZeroVector;
		if (!FindVerificationSurfaces(TopPoint, WallPoint, WallNormal))
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] steps 7-9 SKIPPED: no raised cover found in the arena to land on."));
			VerifyStep = 10;
			return;
		}

		const ETraceTeam FromTeam = Carrier->GetTeam();
		const ETraceTeam ToTeam = TraceOpposingTeam(FromTeam);

		ATraceCharacter* Nearest = nullptr;
		for (ATraceCharacter* Candidate : Everyone)
		{
			if (IsValid(Candidate) && Candidate->IsAlive() && Candidate->GetTeam() == ToTeam)
			{
				Nearest = Candidate;
				break;
			}
		}

		if (Nearest == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] step 7 SKIPPED: no living %s player to award the turnover to."),
				*TraceTeamName(ToTeam).ToString());
			++VerifyStep;
			return;
		}

		// 900 uu away, for the reason step 6 documents at length: the catch zone reaches 500 uu from a
		// capsule's surface, so an enemy any closer would magnet the Core out of the air and this step
		// would quietly become a third test of §4.1.
		Nearest->SetActorLocation(
			FVector(TopPoint.X + 900.0, TopPoint.Y, Nearest->GetActorLocation().Z),
			false, nullptr, ETeleportType::TeleportPhysics);

		const FVector Start = TopPoint + FVector(0.0, 0.0, 160.0);

		VerifyThrower = Carrier;
		VerifyExpectTeam = ToTeam;
		VerifyExpectGrace = true;
		bVerifyAwaitingTake = true;
		bVerifyTakeSeen = false;
		bVerifyAwaitingTurnover = true;   // Spec v25 §2: the registration is what this now judges.
		bVerifyTurnoverSeen = false;
		VerifyTurnoversAtStart = SurfaceStats.TopTurnovers;

		if (!DebugLaunchLoose(Start, FVector(0.0, 0.0, -1400.0), FromTeam, /*bAsThrow=*/true))
		{
			++VerifyFailCount;
			bVerifyAwaitingTake = false;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 7 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		const ATraceArenaBuilder* VerifyArena = ATraceArenaBuilder::Get(World);
		const double VerifyFloorZ = (VerifyArena != nullptr) ? VerifyArena->GetFieldBounds().Min.Z : 0.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 7: a %s throw dropped onto the TOP of an object at %s (%.0f uu above ")
			TEXT("the floor) with %s (%s) standing 900 uu away - expecting a SURFACE TURNOVER to them, with grace."),
			*TraceTeamName(FromTeam).ToString(), *TopPoint.ToCompactString(),
			TopPoint.Z - VerifyFloorZ,
			*GetNameSafe(Nearest), *TraceTeamName(ToTeam).ToString());

		VerifyStepDeadline = Now + 5.f;
		return;
	}

	case 8:
	{
		// =========================================================================================
		// SPEC v7 §4: THE STUCK CORE ITSELF — a turnover with the flight integration switched OFF.
		//
		// THIS IS THE STEP THAT TESTS THE REPORTED BUG. Step 7 drops a Core onto a crate and the
		// CONTACT test catches it, which is the path that already existed; a Core that reaches rest
		// without a qualifying contact - an edge hit, a hitch, a state lock held on the landing frame -
		// switches the integration off and is never asked again, and that is the Core the user watched
		// sit on top of an object until the reset timer.
		//
		// Reproduced exactly: park a Core on the crate as something NOBODY threw (so no rule may touch
		// it), let it settle, and only then declare it thrown. From that instant the contact test is
		// unreachable by construction - there is no sweep - so a turnover can only come from the
		// at-rest probe. If the probe is broken this step hangs and fails, and nothing else does.
		// =========================================================================================
		if (!IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;
		}

		FVector RestTopPoint = FVector::ZeroVector;
		FVector UnusedWallPoint = FVector::ZeroVector;
		FVector UnusedWallNormal = FVector::ZeroVector;
		if (!FindVerificationSurfaces(RestTopPoint, UnusedWallPoint, UnusedWallNormal))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 8 SKIPPED: no raised cover to park a Core on."));
			++VerifyStep;
			return;
		}

		const ETraceTeam RestFromTeam = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::Blue;
		const ETraceTeam RestToTeam = TraceOpposingTeam(RestFromTeam);

		ATraceCharacter* RestNearest = nullptr;
		for (ATraceCharacter* Candidate : Everyone)
		{
			if (IsValid(Candidate) && Candidate->IsAlive() && Candidate->GetTeam() == RestToTeam)
			{
				RestNearest = Candidate;
				break;
			}
		}

		if (RestNearest == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] step 8 SKIPPED: no living %s player to award the turnover to."),
				*TraceTeamName(RestToTeam).ToString());
			++VerifyStep;
			return;
		}

		RestNearest->SetActorLocation(
			FVector(RestTopPoint.X + 900.0, RestTopPoint.Y, RestNearest->GetActorLocation().Z),
			false, nullptr, ETeleportType::TeleportPhysics);

		// bAsThrow = FALSE. That is the whole trick: while it is falling and settling, NO rule is
		// allowed to fire, so the Core reaches rest exactly as an abandoned one does.
		if (!DebugLaunchLoose(RestTopPoint + FVector(0.0, 0.0, 60.0), FVector(0.0, 0.0, -200.0),
			RestFromTeam, /*bAsThrow=*/false))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 8 FAIL: could not park the Core."));
			++VerifyStep;
			return;
		}

		VerifyThrower = Carrier;
		VerifyExpectTeam = RestToTeam;
		VerifyExpectGrace = true;
		bVerifyRestArmed = false;
		bVerifyAwaitingTake = false;
		bVerifyTakeSeen = false;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 8: parking an unthrown %s Core on the TOP of an object at %s with %s (%s) ")
			TEXT("900 uu away - it must settle first, and the AT-REST PROBE alone must then turn it over."),
			*TraceTeamName(RestFromTeam).ToString(), *RestTopPoint.ToCompactString(),
			*GetNameSafe(RestNearest), *TraceTeamName(RestToTeam).ToString());

		VerifyStepDeadline = Now + 6.f;
		return;
	}

	case 9:
	{
		// =========================================================================================
		// SPEC v7 §4, THE OTHER HALF: A WALL IS A BOUNCE, NOT A TURNOVER.
		//
		// "Walls should not, the core should bounce off those."
		//
		// Fired horizontally at the SIDE of the same piece of cover step 7 landed on, hard enough and
		// from close enough that gravity cannot drop it onto anything horizontal on the way. What is
		// asserted is the pair: a bounce was registered, and the turnover tally did not move.
		// =========================================================================================
		FVector TopPoint = FVector::ZeroVector;
		FVector WallPoint = FVector::ZeroVector;
		FVector WallNormal = FVector::ZeroVector;
		if (!FindVerificationSurfaces(TopPoint, WallPoint, WallNormal))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 9 SKIPPED: no wall found to bounce off."));
			++VerifyStep;
			return;
		}

		const ETraceTeam FromTeam = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::Blue;

		// 420 uu out along the wall's own normal, moving straight back down it at 2400 uu/s: the flight
		// lasts under 0.2 s, in which the Core falls ~20 uu under the mode-B gravity model, so it
		// cannot clip the floor or the top of the block before it reaches the face under test.
		const FVector Start = WallPoint + WallNormal * 420.0;
		const FVector LaunchVelocity = -WallNormal * 2400.0;

		VerifyWallBouncesAtStart = SurfaceStats.WallBounces;
		VerifyTurnoversAtStart = SurfaceStats.GroundTurnovers + SurfaceStats.TopTurnovers;
		bVerifyAwaitingTake = false;
		bVerifyTakeSeen = false;

		// Only on the FIRST attempt: a retry must not top its own allowance back up, or a step that can
		// never reach the wall would retry forever instead of failing.
		if (VerifyWallShotRetriesLeft <= 0 && !bVerifyWallShotFiredOnce)
		{
			bVerifyWallShotFiredOnce = true;
			VerifyWallShotRetriesLeft = 3;
		}

		if (!DebugLaunchLoose(Start, LaunchVelocity, FromTeam, /*bAsThrow=*/true))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 8 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 9: a %s throw fired at a WALL at %s (normal %s, %.0f deg from up) from ")
			TEXT("%s - expecting a BOUNCE and NO turnover."),
			*TraceTeamName(FromTeam).ToString(), *WallPoint.ToCompactString(), *WallNormal.ToCompactString(),
			FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(WallNormal.Z, -1.0, 1.0))),
			*Start.ToCompactString());

		VerifyStepDeadline = Now + 4.f;
		return;
	}

	default:
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] ===== finished: %d PASS, %d FAIL ====="), VerifyPassCount, VerifyFailCount);
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] spec v7 §4 tally: turnovers off the ground %d, off the TOP of an object %d ")
			TEXT("| wall bounces %d (rest refused on a wall %d) | landings the at-rest probe caught %d"),
			SurfaceStats.GroundTurnovers, SurfaceStats.TopTurnovers, SurfaceStats.WallBounces,
			SurfaceStats.WallRestRefusals, SurfaceStats.RestProbeRescues);

		CVarModeBVerifyRequested->Set(0, ECVF_SetByConsole);
		CVarModeBVerifySurfacesRequested->Set(0, ECVF_SetByConsole);
		bVerifySurfacesOnly = false;
		VerifyStep = -1;
		return;
	}
	}
}

bool ATraceCore::FindVerificationSurfaces(FVector& OutTopPoint, FVector& OutWallPoint, FVector& OutWallNormal) const
{
	const UWorld* World = GetWorld();
	const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
	if (World == nullptr || Arena == nullptr)
	{
		return false;
	}

	const FBox FieldBox = Arena->GetFieldBounds();
	if (FieldBox.IsValid == 0)
	{
		return false;
	}

	const double FloorZ = FieldBox.Min.Z;
	const FVector Centre = FieldBox.GetCenter();

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreSurfaceProbe), /*bTraceComplex=*/false, this);
	Params.AddIgnoredActor(this);

	const float UpNormalZ = TraceModeBTuning::SurfaceUpNormalZ();
	const float Radius = TraceModeBTuning::CollisionRadius;

	// SAMPLED, NOT CONSTRUCTED. The rule under test reads world geometry, so the test has to find its
	// crate the same way - a probe grid over the MIDDLE of the pitch, deliberately away from both goal
	// mouths so a dropped Core cannot score and end the step for the wrong reason.
	const double SpanX = FieldBox.GetSize().X * 0.25;
	const double SpanY = FieldBox.GetSize().Y * 0.40;

	for (int32 IndexX = -6; IndexX <= 6; ++IndexX)
	{
		for (int32 IndexY = -6; IndexY <= 6; ++IndexY)
		{
			const FVector Column(
				Centre.X + (SpanX * IndexX) / 6.0,
				Centre.Y + (SpanY * IndexY) / 6.0,
				0.0);

			FHitResult TopHit;
			const bool bHitTop = World->SweepSingleByChannel(
				TopHit,
				FVector(Column.X, Column.Y, FieldBox.Max.Z),
				FVector(Column.X, Column.Y, FloorZ - 50.0),
				FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(Radius), Params);

			// Raised, horizontal-ish, and not so tall that it is a wall cap or the roof: the point is a
			// piece of cover a thrown Core could plausibly settle on.
			if (!bHitTop
				|| TopHit.ImpactNormal.Z < UpNormalZ
				|| TopHit.ImpactPoint.Z < FloorZ + 150.0
				|| TopHit.ImpactPoint.Z > FloorZ + 1200.0)
			{
				continue;
			}

			// Its SIDE. Probed from four directions at a height comfortably below the top face, so the
			// hit is the flank of the same block and not its lip.
			const FVector SideSample = FVector(TopHit.ImpactPoint.X, TopHit.ImpactPoint.Y,
				FMath::Max(FloorZ + 60.0, TopHit.ImpactPoint.Z - 90.0));

			static const FVector Directions[] =
			{
				FVector(1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0),
				FVector(0.0, 1.0, 0.0), FVector(0.0, -1.0, 0.0)
			};

			for (const FVector& Direction : Directions)
			{
				FHitResult SideHit;
				const bool bHitSide = World->SweepSingleByChannel(
					SideHit, SideSample + Direction * 900.0, SideSample,
					FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(Radius), Params);

				// A WALL by the rule's own definition, not by ours: whatever the shipping threshold
				// currently says is not a floor. Asking the same accessor is what keeps the test honest
				// if the threshold is ever retuned.
				if (!bHitSide || SideHit.ImpactNormal.Z >= UpNormalZ || SideHit.bStartPenetrating)
				{
					continue;
				}

				// The launch point has to be in open air, or the step tests whatever is between them.
				FHitResult ClearHit;
				const FVector LaunchPoint = SideHit.ImpactPoint + SideHit.ImpactNormal * 420.0;
				if (World->SweepSingleByChannel(ClearHit, LaunchPoint, LaunchPoint,
					FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(Radius * 2.f), Params))
				{
					continue;
				}

				OutTopPoint = TopHit.ImpactPoint;
				OutWallPoint = SideHit.ImpactPoint;
				OutWallNormal = SideHit.ImpactNormal.GetSafeNormal();
				return true;
			}
		}
	}

	return false;
}

#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING

// =================================================================================================
// SPEC v13 §8 — THE REPRODUCTION
//
// "Sometimes the core is thrown and it turns over before it touches the ground." The instruction was
// to REPRODUCE IT FIRST, and this is what does it: a throw fired repeatedly ACROSS the flat top of a
// piece of arena cover, at a shallow angle, exactly the shape of throw a player makes over a crate.
//
// WHY IT CAN GO RED, WHICH IS THE PART THAT MATTERS. Trace.ModeB.LandingRule 0 restores the pre-v13
// behaviour verbatim - any upward-facing contact is a landing - and the two arms are otherwise the
// same code driving the same geometry. So the pass criterion is not "we saw nothing bad": it is a
// number that MOVES between the arms, measured by a counter (SurfaceStats.MidAirTurnovers) that is
// computed identically in both and that reads the event's geometry rather than the rule's verdict.
// A harness that could only ever print zero would be the wall-clip harness this project has already
// been burned by, and it would prove nothing.
//
// It also fires DROP shots - straight down onto open floor - in the same run, so the same tally shows
// spec v6 §4.2's ordinary ground turnover still firing. A "fix" that stopped every turnover would
// pass a graze-only harness and would be a much worse bug than the one being fixed.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarModeBTurnoverRepro(
	TEXT("Trace.ModeB.TurnoverRepro"),
	0,
	TEXT("MODE B, spec v13 §8. Set to N: fire N scripted throws - alternating a GRAZE across the flat ")
	TEXT("top of a piece of cover and a DROP onto open floor - and print how many turnovers fired in ")
	TEXT("mid-air. Run it twice: once as shipped, and once with Trace.ModeB.LandingRule 0, which arms ")
	TEXT("the pre-v13 rule and is the arm that must go RED."),
	ECVF_Default);

/**
 * How many shots the repro was asked for, from the CVar or from -TraceTurnoverRepro=N.
 *
 * The command-line form exists because arming this through -ExecCmds requires a SPACE
 * ("Trace.ModeB.TurnoverRepro 12"), which requires quoting, and a quoted -ExecCmds argument has
 * already broken a command line into the URL parser on this project and produced a verification that
 * "passed" because none of its commands ran. `-TraceTurnoverRepro=12` cannot do that.
 */
static int32 TraceModeBTurnoverReproShots()
{
	static const int32 FromCommandLine = []() -> int32
	{
		int32 Value = 0;
		return FParse::Value(FCommandLine::Get(), TEXT("TraceTurnoverRepro="), Value) ? Value : 0;
	}();

	return FMath::Max(FromCommandLine, CVarModeBTurnoverRepro.GetValueOnGameThread());
}

double ATraceCore::MeasureTopFaceExtent(const FVector& FromPoint, FVector& OutDirection) const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0.0;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreTopExtent), /*bTraceComplex=*/false, this);
	Params.AddIgnoredActor(this);

	static const FVector Compass[] =
	{
		FVector(1.0, 0.0, 0.0),  FVector(-1.0, 0.0, 0.0),
		FVector(0.0, 1.0, 0.0),  FVector(0.0, -1.0, 0.0),
		FVector(0.707, 0.707, 0.0),  FVector(-0.707, 0.707, 0.0),
		FVector(0.707, -0.707, 0.0), FVector(-0.707, -0.707, 0.0)
	};

	constexpr double StepUU = 40.0;
	constexpr int32 MaxSteps = 24;          // 960 uu, comfortably longer than any cover block here.
	constexpr double SameFaceTolerance = 8.0;

	double BestRun = 0.0;
	OutDirection = FVector::ForwardVector;

	for (const FVector& Direction : Compass)
	{
		double Run = 0.0;

		for (int32 Step = 1; Step <= MaxSteps; ++Step)
		{
			const FVector Column = FromPoint + Direction * (StepUU * Step);

			FHitResult Hit;
			const bool bHit = World->SweepSingleByChannel(
				Hit,
				FVector(Column.X, Column.Y, FromPoint.Z + 120.0),
				FVector(Column.X, Column.Y, FromPoint.Z - 40.0),
				FQuat::Identity, ECC_WorldStatic,
				FCollisionShape::MakeSphere(TraceModeBTuning::CollisionRadius), Params);

			// THE SAME FACE, not merely SOMETHING. A run that wandered onto a taller block beside this
			// one would aim the graze into a wall and the shot would test the bounce rule instead.
			if (!bHit
				|| Hit.ImpactNormal.Z < TraceModeBTuning::SurfaceUpNormalZ()
				|| FMath::Abs(Hit.ImpactPoint.Z - FromPoint.Z) > SameFaceTolerance)
			{
				break;
			}

			Run = StepUU * Step;
		}

		if (Run > BestRun)
		{
			BestRun = Run;
			OutDirection = Direction.GetSafeNormal();
		}
	}

	return BestRun;
}

void ATraceCore::TickTurnoverRepro()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	// --- Arm ---------------------------------------------------------------------------------------
	if (!bTurnoverReproArmed)
	{
		const int32 Requested = TraceModeBTurnoverReproShots();
		if (Requested <= 0 || bTurnoverReproReported)
		{
			return;
		}

		// The same settled-half gate Trace.ModeB.Verify uses, and for the same reason: arming during
		// the pre-match window puts the first shot on the kickoff frame, which cancels it and makes the
		// harness report on a rule that never ran.
		const ATraceGameState* GameState = World->GetGameState<ATraceGameState>();
		if (!IsModeB() || GameState == nullptr
			|| GameState->TraceMatchState != ETraceMatchState::InProgress
			|| GameState->IsHalfTimeBreak()
			|| !IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;
		}

		FVector TopPoint = FVector::ZeroVector;
		FVector UnusedWallPoint = FVector::ZeroVector;
		FVector UnusedWallNormal = FVector::ZeroVector;
		if (!FindVerificationSurfaces(TopPoint, UnusedWallPoint, UnusedWallNormal))
		{
			bTurnoverReproReported = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBTurnoverRepro] REFUSED: no raised cover with a flat top anywhere in the middle ")
				TEXT("of the pitch, so there is nothing to graze. The harness is NOT reporting a pass - it ")
				TEXT("could not run."));
			return;
		}

		FVector GrazeDirection = FVector::ForwardVector;
		const double Extent = MeasureTopFaceExtent(TopPoint, GrazeDirection);

		// The graze needs a face long enough for the Core to descend onto it INSIDE the face rather than
		// sailing off the far edge. Refusing loudly is the honest answer; a shot fired at a 40 uu ledge
		// would miss and the run would report "no mid-air turnovers" while having tested nothing.
		if (Extent < 240.0)
		{
			bTurnoverReproReported = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBTurnoverRepro] REFUSED: the flat top at %s runs only %.0f uu, which is too ")
				TEXT("short to graze across. The harness is NOT reporting a pass - it could not run."),
				*TopPoint.ToCompactString(), Extent);
			return;
		}

		bTurnoverReproArmed = true;
		TurnoverReproShotsLeft = FMath::Clamp(Requested, 1, 100);
		TurnoverReproTopPoint = TopPoint;
		TurnoverReproGrazeDirection = GrazeDirection;
		TurnoverReproTopExtent = Extent;
		TurnoverReproGrazeShots = 0;
		TurnoverReproDropShots = 0;
		TurnoverReproSkipped = 0;
		TurnoverReproMidAirAtStart = SurfaceStats.MidAirTurnovers;
		TurnoverReproLandedAtStart = SurfaceStats.LandedTurnovers;
		TurnoverReproRejectedAtStart = SurfaceStats.GlancingContactsRejected;
		TurnoverReproUnseenAtStart = SurfaceStats.UnseenTurnovers;
		TurnoverReproSeenAtStart = SurfaceStats.SeenTurnovers;
		TurnoverReproUngroundedAtStart = SurfaceStats.UngroundedTurnovers;
		TurnoverReproGroundedAtStart = SurfaceStats.GroundedTurnovers;
		SurfaceStats.FewestTurnoverContactFrames = -1;
		SurfaceStats.ShortestTurnoverDwellSeconds = -1.f;
		SurfaceStats.WorstTurnoverGapUU = 0.f;
		TurnoverReproNextShotTime = Now + 1.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBTurnoverRepro] ===== spec v13 §8 + v19 §1.5: %d shots across the flat top at %s ")
			TEXT("(the face runs %.0f uu along %s) | landing rule = %s | grounded rule = %s (settle %.2fs, ")
			TEXT("slack %.0f uu) | mid-air test: faster than %.0f uu/s AND shallower than %.0f deg ====="),
			TurnoverReproShotsLeft, *TopPoint.ToCompactString(), Extent,
			*GrazeDirection.ToCompactString(),
			(!TraceModeBLegacyLandingRule())
				? TEXT("v13 (an actual landing)") : TEXT("PRE-v13 (any upward normal) - THE BUG, ARMED"),
			(!TraceModeBLegacyGroundedRule())
				? TEXT("v19 (settle, then award)") : TEXT("PRE-v19 (award on the contact frame) - THE BUG, ARMED"),
			TraceModeBLegacyGroundedRule() ? 0.f : CVarModeBTurnoverSettleSeconds.GetValueOnAnyThread(),
			CVarModeBTurnoverContactSlack.GetValueOnAnyThread(),
			CVarModeBMidAirTurnoverSpeed.GetValueOnAnyThread(),
			CVarModeBMidAirTurnoverDegrees.GetValueOnAnyThread());
		return;
	}

	// --- Report and stop ---------------------------------------------------------------------------
	if (TurnoverReproShotsLeft <= 0)
	{
		if (bTurnoverReproReported)
		{
			return;
		}

		// Wait for the last shot to actually resolve before judging it. A tally printed while a Core is
		// still in the air is a tally that is missing its last result.
		if (bLoose)
		{
			return;
		}

		bTurnoverReproReported = true;

		const int32 MidAir = SurfaceStats.MidAirTurnovers - TurnoverReproMidAirAtStart;
		const int32 Landed = SurfaceStats.LandedTurnovers - TurnoverReproLandedAtStart;
		const int32 Rejected = SurfaceStats.GlancingContactsRejected - TurnoverReproRejectedAtStart;

		// SPEC v19 §1.5's half of the same run.
		const int32 Unseen = SurfaceStats.UnseenTurnovers - TurnoverReproUnseenAtStart;
		const int32 Seen = SurfaceStats.SeenTurnovers - TurnoverReproSeenAtStart;
		const int32 Ungrounded = SurfaceStats.UngroundedTurnovers - TurnoverReproUngroundedAtStart;
		const int32 Grounded = SurfaceStats.GroundedTurnovers - TurnoverReproGroundedAtStart;

		// FOUR CLAUSES, and the two "> 0" ones are what stop this from being a harness that passes by
		// doing nothing. Zero mid-air and zero unseen turnovers are both trivially achievable by never
		// turning the Core over at all, which would be a far worse bug than the ones under repair, so
		// the run must ALSO show ordinary landings still handing possession over — and, since v19,
		// show that those landings were WATCHED.
		const bool bNoMidAir = (MidAir == 0);
		const bool bStillTurningOver = (Landed > 0);
		const bool bNoneUnseen = (Unseen == 0);
		const bool bNoneUngrounded = (Ungrounded == 0);
		const bool bSomeWereSeen = (Seen > 0);
		const bool bPass = bNoMidAir && bStillTurningOver && bNoneUnseen && bNoneUngrounded && bSomeWereSeen;

		const FString Detail = FString::Printf(
			TEXT("%d shots (%d grazes across the top, %d drops onto the floor, %d skipped) | MID-AIR ")
			TEXT("TURNOVERS %d (must be 0) | landed turnovers %d (must be > 0, or the fix has simply ")
			TEXT("switched the rule off) | glancing contacts refused a landing %d | landing rule = %s ")
			TEXT("|| v19 §1.5: UNSEEN TURNOVERS %d (must be 0 - fired on the same frame the ball landed, ")
			TEXT("so nobody could see it touch) | SEEN turnovers %d (must be > 0) | UNGROUNDED turnovers ")
			TEXT("%d (must be 0) | grounded %d | fewest frames of contact behind any turnover %d | ")
			TEXT("shortest hold %.3fs | worst gap under any turnover %.0f uu | grounded rule = %s"),
			TurnoverReproGrazeShots + TurnoverReproDropShots,
			TurnoverReproGrazeShots, TurnoverReproDropShots, TurnoverReproSkipped,
			MidAir, Landed, Rejected,
			(!TraceModeBLegacyLandingRule())
				? TEXT("v13 (an actual landing)") : TEXT("PRE-v13 (any upward normal) - THE BUG, ARMED"),
			Unseen, Seen, Ungrounded, Grounded,
			SurfaceStats.FewestTurnoverContactFrames, SurfaceStats.ShortestTurnoverDwellSeconds,
			SurfaceStats.WorstTurnoverGapUU,
			(!TraceModeBLegacyGroundedRule())
				? TEXT("v19 (settle, then award)")
				: TEXT("PRE-v19 (award on the contact frame) - THE BUG, ARMED"));

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBTurnoverRepro] PASS: %s"), *Detail);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBTurnoverRepro] FAIL: %s"), *Detail);
		}

		// SPEC v13 §5's live number, printed here because this is the one scripted run that reliably
		// puts the Core in the air with ten players chasing it. The rule itself is asserted by
		// Trace.ModeB.ContestTest against the same selection function; this says how often a real match
		// actually reaches the situation the note is about, which no unit check can.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBTurnoverRepro] magnet during this run (spec v13 §5): %d frames with somebody in ")
			TEXT("range (%d uncontested, %d CONTESTED, %d of those cross-team) | %d contests | largest set ")
			TEXT("%d | %d target switches | %d switches refused by the %.0f uu hysteresis"),
			CatchStats.UncontestedFrames + CatchStats.ContestedFrames,
			CatchStats.UncontestedFrames, CatchStats.ContestedFrames, CatchStats.CrossTeamContestedFrames,
			CatchStats.Contests, CatchStats.MaxContenders, CatchStats.TargetSwitches,
			CatchStats.HysteresisHolds, TraceModeBTuning::CatchContestHysteresis());
		return;
	}

	// --- Fire the next shot ------------------------------------------------------------------------
	//
	// One at a time, and only once the previous one has resolved: two Cores in the air at once is not a
	// state this game has, and a shot fired into a loose Core would be a state lock refusal counted as
	// a result.
	if (Now < TurnoverReproNextShotTime || bLoose || bCoreStateLocked)
	{
		return;
	}

	if (!IsValid(Carrier) || !Carrier->IsAlive())
	{
		return;   // Between possessions. Wait; the shot is not lost.
	}

	const ETraceTeam FromTeam = Carrier->GetTeam();
	const bool bGrazeShot = ((TurnoverReproGrazeShots + TurnoverReproDropShots) % 2) == 0;

	FVector LaunchPoint = FVector::ZeroVector;
	FVector LaunchVelocity = FVector::ZeroVector;

	if (bGrazeShot)
	{
		// THE SHOT THAT REPRODUCES THE BUG.
		//
		// A flat, fast throw that meets the top face at a few degrees - the shape a player produces
		// throwing over cover. The numbers are derived from the Core's own gravity rather than guessed,
		// so the shot still lands on the face if the weight model is retuned:
		//
		//   the Core is released HeightAboveFace above the face with NO vertical velocity, so it
		//   contacts after t = sqrt(2h/g) seconds and d = V*t uu of travel. Placing the launch point
		//   d - Extent/2 back from the probe point puts the contact half way along the face, which is
		//   the furthest possible from either edge.
		constexpr double HeightAboveFace = 40.0;
		constexpr double GrazeSpeed = 1400.0;

		const double GravityMagnitude = FMath::Abs(static_cast<double>(GetThrowGravityZ(World)));
		const double FallTime = (GravityMagnitude > 1.0)
			? FMath::Sqrt(2.0 * HeightAboveFace / GravityMagnitude) : 0.2;
		const double TravelToContact = GrazeSpeed * FallTime;

		const double Back = TravelToContact - FMath::Min(TurnoverReproTopExtent * 0.5, TravelToContact * 0.75);

		LaunchPoint = TurnoverReproTopPoint
			- TurnoverReproGrazeDirection * Back
			+ FVector(0.0, 0.0, TraceModeBTuning::CollisionRadius + HeightAboveFace);
		LaunchVelocity = TurnoverReproGrazeDirection * GrazeSpeed;
	}
	else
	{
		// THE CONTROL. A throw dropped straight onto open floor, well away from the cover, which spec
		// v6 §4.2 says must turn over. If this stops firing, the "fix" has broken the rule instead of
		// narrowing it, and the tally says so on the same line as the graze result.
		const FBox FieldBox = [World]() -> FBox
		{
			if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
			{
				return Arena->GetFieldBounds();
			}
			return FBox(ForceInit);
		}();

		const FVector Centre = (FieldBox.IsValid != 0) ? FieldBox.GetCenter() : FVector::ZeroVector;
		const double FloorZ = (FieldBox.IsValid != 0) ? FieldBox.Min.Z : 0.0;

		LaunchPoint = FVector(Centre.X, Centre.Y, FloorZ + 900.0);
		LaunchVelocity = FVector(0.0, 0.0, -1500.0);
	}

	// The launch point has to be in open air, or the shot tests whatever it started inside.
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreReproClear), /*bTraceComplex=*/false, this);
		Params.AddIgnoredActor(this);

		FHitResult ClearHit;
		if (World->SweepSingleByChannel(ClearHit, LaunchPoint, LaunchPoint, FQuat::Identity, ECC_WorldStatic,
			FCollisionShape::MakeSphere(TraceModeBTuning::CollisionRadius), Params))
		{
			++TurnoverReproSkipped;
			--TurnoverReproShotsLeft;
			TurnoverReproNextShotTime = Now + 0.5f;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBTurnoverRepro] shot SKIPPED: the launch point %s is inside %s."),
				*LaunchPoint.ToCompactString(), *GetNameSafe(ClearHit.GetActor()));
			return;
		}
	}

	if (!DebugLaunchLoose(LaunchPoint, LaunchVelocity, FromTeam, /*bAsThrow=*/true))
	{
		++TurnoverReproSkipped;
		--TurnoverReproShotsLeft;
		TurnoverReproNextShotTime = Now + 0.5f;
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBTurnoverRepro] shot SKIPPED: the launch was refused."));
		return;
	}

	(bGrazeShot ? TurnoverReproGrazeShots : TurnoverReproDropShots)++;
	--TurnoverReproShotsLeft;
	TurnoverReproNextShotTime = Now + 2.5f;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBTurnoverRepro] shot %d/%d: a %s throw %s from %s at %.0f uu/s (%d left)"),
		TurnoverReproGrazeShots + TurnoverReproDropShots,
		TurnoverReproGrazeShots + TurnoverReproDropShots + TurnoverReproShotsLeft + TurnoverReproSkipped,
		*TraceTeamName(FromTeam).ToString(),
		bGrazeShot ? TEXT("GRAZING the top of cover") : TEXT("DROPPED onto open floor"),
		*LaunchPoint.ToCompactString(), LaunchVelocity.Size(), TurnoverReproShotsLeft);
}

#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING

// =================================================================================================
// SPEC v31 §4 — PHOTOGRAPHING THE THREE STATES.
//
// `Trace.Core.ArtShots [DistanceUU]` on the listen host stages rest, flight and carried in front of
// the local camera and requests a screenshot of each. The frames are the deliverable; a log line
// saying "the nose points along the velocity" is not evidence that it does.
//
// It reaches every state through the SHIPPING functions (DebugLaunchLoose, GrantTo) and never touches
// PackMesh, so what is photographed is the real presentation path. The one thing it stages that a
// match would not is a SECOND BODY for the carried shot: the Core is bOwnerNoSee, deliberately hidden
// from the lens of whoever is holding it, so a player can never photograph their own.
// =================================================================================================

bool ATraceCore::DebugStageCoreArt(UWorld* World, int32 Which, float DistanceUU, FString& OutReport)
{
	ATraceCore* Core = ATraceCore::Get(World);
	if (Core == nullptr)
	{
		OutReport = TEXT("no Core in this world");
		return false;
	}
	if (!Core->HasAuthority())
	{
		OutReport = TEXT("this machine is not the server; stage on the listen host");
		return false;
	}
	if (!Core->IsModeB())
	{
		OutReport = TEXT("mode A: the Core is never loose, so only state 2 (carried) exists. Launch with ?mode=b.");
		if (Which != 2)
		{
			return false;
		}
	}

	APlayerController* PC = World->GetFirstPlayerController();
	ATraceCharacter* Local = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	if (!IsValid(Local))
	{
		OutReport = TEXT("no local pawn");
		return false;
	}

	// STAGE IT OUTSIDE THE PICKUP RADIUS, OR IT IS NOT THERE TO PHOTOGRAPH. Measured the hard way: at
	// 130 uu the first run's "at rest" frame came back showing the local player HOLDING the Core, with
	// the ball hidden from its own holder - the shipping first-contact poll had taken it on the frame
	// it landed. The rule is measured from the Core to the CAPSULE SURFACE, so the safe distance is the
	// radius plus the capsule plus a margin, asked of the same accessors the rule uses.
	const float CapsuleRadius = (Local->GetCapsuleComponent() != nullptr)
		? Local->GetCapsuleComponent()->GetScaledCapsuleRadius() : 42.f;
	const float SafeReach = TraceModeBTuning::PickupRadius() + CapsuleRadius + 80.f;

	float Reach = (DistanceUU > 1.f) ? DistanceUU : 420.f;
	if (Reach < SafeReach)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[CoreArt] %.0f uu is inside the %.0f uu first-contact radius - the local player would ")
			TEXT("simply pick the Core up. Staging at %.0f uu instead."),
			Reach, TraceModeBTuning::PickupRadius(), SafeReach);
		Reach = SafeReach;
	}

	const FVector Eye = Local->GetPawnViewLocation();
	const FRotator ViewRot = PC->GetControlRotation();
	const FVector Forward = FRotator(0.f, ViewRot.Yaw, 0.f).Vector();   // Along the ground, as §25's staging does.
	const FVector Right = FRotator(0.f, ViewRot.Yaw + 90.f, 0.f).Vector();

	FCollisionQueryParams DropParams(SCENE_QUERY_STAT(TraceCoreArtStage), /*bTraceComplex=*/false);
	DropParams.AddIgnoredActor(Local);
	DropParams.AddIgnoredActor(Core);

	// The floor under a point Reach ahead. Same reasoning as DebugStageTurnoverAtLocalCrosshair: a
	// point on the FLOOR is the only one that is both real and still there a second later.
	auto FloorAhead = [&](const FVector& Offset) -> FVector
	{
		const FVector Base = FVector(Local->GetActorLocation().X, Local->GetActorLocation().Y, Eye.Z) + Offset;
		FHitResult Floor;
		if (World->LineTraceSingleByChannel(Floor, Base + FVector(0.0, 0.0, 100.0),
			Base - FVector(0.0, 0.0, 4000.0), ECC_WorldStatic, DropParams))
		{
			return Floor.ImpactPoint + Floor.ImpactNormal * (TraceModeBVisibleOrbRadius + 2.0);
		}
		return Base;
	};

	const ETraceTeam LocalTeam = Local->GetTeam();

	switch (Which)
	{
	case 0:
	{
		// AT REST. Zero velocity onto the floor: the shipping rest probe settles it within a frame or
		// two, exactly as a spent throw does, and the art state follows from the replicated velocity.
		const FVector Where = FloorAhead(Forward * Reach);
		if (!Core->DebugLaunchLoose(Where, FVector::ZeroVector, LocalTeam, /*bAsThrow=*/false))
		{
			OutReport = TEXT("DebugLaunchLoose refused (state locked?)");
			return false;
		}
		PC->SetControlRotation((Where - Eye).Rotation());
		OutReport = FString::Printf(TEXT("REST staged at %s, %.0f uu ahead. Expect: standing on its point, turning at %.3f rev/s."),
			*Where.ToCompactString(), Reach, CVarCoreRestSpin.GetValueOnGameThread());
		return true;
	}

	case 1:
	{
		// IN FLIGHT, ACROSS THE VIEW. Perpendicular to the camera so the SILHOUETTE is what the frame
		// shows - a ball flying away from the lens is a ball whose nose you cannot see. The speed is a
		// real mid-arc speed rather than the launch speed, because a Core at full launch speed crosses
		// a 90-degree field of view in under a tenth of a second and no screenshot request is that
		// punctual. (That speed was 2236 uu/s when this was written, at the 3000 base; it is 2161.5
		// after Patch 28 §4 took the base to 2900. The 900 uu/s used below is deliberately neither —
		// it is a mid-arc speed, not a launch speed, and it is not meant to track the tuning.)
		const FVector From = FloorAhead(Forward * Reach - Right * (Reach * 0.9)) + FVector(0.0, 0.0, 160.0);
		const FVector Velocity = Right * 900.0 + FVector(0.0, 0.0, 240.0);
		if (!Core->DebugLaunchLoose(From, Velocity, LocalTeam, /*bAsThrow=*/true))
		{
			OutReport = TEXT("DebugLaunchLoose refused (state locked?)");
			return false;
		}
		PC->SetControlRotation(((From + Right * 260.0) - Eye).Rotation());
		OutReport = FString::Printf(TEXT("FLIGHT staged from %s at %.0f uu/s across the view. Expect: nose along the velocity, %.1f rev/s roll."),
			*From.ToCompactString(), Velocity.Size(), CVarCoreFlightSpin.GetValueOnGameThread());
		return true;
	}

	case 2:
	default:
	{
		// CARRIED, ON SOMEBODY ELSE. See the block comment: the holder never sees their own.
		// PREFER WHOEVER ALREADY HAS IT. Bots run, so a single stage-then-photograph is a race the bot
		// usually wins: measured, the carried frame came back with the bearer already out of shot.
		// Re-staging the SAME bearer just before each capture walks them back in front of the lens
		// without re-granting, so the Pickup clip is not restarted and the second frame really is the
		// carry pose rather than a second crack.
		ATraceCharacter* Bearer = nullptr;
		if (IsValid(Core->Carrier) && Core->Carrier != Local && Core->Carrier->IsAlive())
		{
			Bearer = Core->Carrier;
		}
		else
		{
			TArray<ATraceCharacter*> Characters;
			Core->GatherCharacters(Characters);
			for (ATraceCharacter* Candidate : Characters)
			{
				if (IsValid(Candidate) && Candidate != Local && Candidate->IsAlive())
				{
					Bearer = Candidate;
					break;
				}
			}
		}

		if (!IsValid(Bearer))
		{
			OutReport = TEXT("no second living pawn to carry it - run with bots, or join a client. ")
				TEXT("Granting it to the LOCAL player photographs something now (the carried ball is drawn ")
				TEXT("in the holder's hand and is no longer hidden from them), but only from behind at the ")
				TEXT("450 uu carry arm - use Trace.DebugTakeCore plus Trace.Core.CarryProbe for that view. ")
				TEXT("This harness stages the ball FACING the camera, which needs somebody else to hold it.");
			return false;
		}

		const FVector Where = FloorAhead(Forward * Reach);
		Bearer->SetActorLocation(Where + FVector(0.0, 0.0, 88.0), false, nullptr, ETeleportType::TeleportPhysics);
		if (Core->Carrier != Bearer)
		{
			Core->GrantTo(Bearer, ETraceCoreGrantReason::Debug);
		}

		// AIMED AT THE BALL ITSELF, WHICH IS NO LONGER THE SAME PLACE AS THE ACTOR. The Core actor
		// still rides OrbHeight above the capsule centre, but a CARRIED ball is now drawn down in the
		// holder's right hand (ArtRoot; see UpdateCarriedArtPlacement), so aiming at the actor would
		// point this harness at empty air above the head and photograph the one thing that is not
		// there. ArtRoot's world location is the drawn ball on every path, including the not-carried
		// ones where it sits exactly on the actor.
		//
		// Placed before it is aimed at. GrantTo above already ran ApplyAttachment, which places the
		// art on the same frame possession changes - this repeat is idempotent (it early-outs on an
		// unchanged offset) and is here so that this harness does not depend on the order of two
		// functions in a different part of the file to point its camera at the right place.
		Core->UpdateCarriedArtPlacement();
		const FVector AimAt = (Core->ArtRoot != nullptr)
			? Core->ArtRoot->GetComponentLocation()
			: Bearer->GetActorLocation() + FVector(0.0, 0.0, TraceCoreTuning::OrbHeight);
		PC->SetControlRotation((AimAt - Eye).Rotation());
		OutReport = FString::Printf(TEXT("CARRIED staged on %s, %.0f uu ahead. Expect: Pickup cracks the shell for %.2fs, then the Idle turntable."),
			*GetNameSafe(Bearer), Reach,
			(Core->ArtPickupAnim != nullptr) ? Core->ArtPickupAnim->GetPlayLength() : 0.f);
		return true;
	}
	}
}

const TCHAR* ATraceCore::DebugArtStateName(ETraceCoreArtState State)
{
	// ONE SPELLING OF THESE THREE WORDS, and it lives beside the enum's own consumer rather than in a
	// harness, because the harness now writes them into FILENAMES: a frame called "flight" is a claim
	// about ResolveCoreArtState()'s answer and the two must be the same three strings forever.
	switch (State)
	{
	case ETraceCoreArtState::Flight:  return TEXT("flight");
	case ETraceCoreArtState::Carried: return TEXT("carried");
	case ETraceCoreArtState::Rest:
	default:                          return TEXT("rest");
	}
}

// =================================================================================================
// SPEC v32 §3 — MEASURING THE GEOMETRY, RATHER THAN ASSERTING IT.
//
// "A verifier will measure the on-screen size." So this reads the numbers back OFF THE LIVE
// COMPONENTS, through UTraceFxShapes' inverse conversions - which that header says exist for exactly
// this ("a verifier that re-derives the radius it expects is only checking its own arithmetic").
// Nothing here recomputes what the code intended; it reports what the transforms on screen are, and
// prints the FX doc's figure beside each one so the two can be compared by eye.
//
// It also FAILS when it never saw an effect at all. A probe that prints "halo: 0 frames" and calls
// that a result is the §7b defect in a different costume.
// =================================================================================================

namespace TraceCoreFxProbe
{
	struct FProbe
	{
		TWeakObjectPtr<UWorld> World;
		double EndsAt = 0.0;
		int32 Frames = 0;

		int32 HaloFrames = 0;
		float HaloMinRadiusUU = TNumericLimits<float>::Max();
		float HaloMaxRadiusUU = -1.f;

		int32 TrailFrames = 0;
		int32 TrailSegmentsSeen = 0;
		float TrailMinLengthUU = TNumericLimits<float>::Max();
		float TrailMaxLengthUU = -1.f;
		float TrailHeadRadiusUU = -1.f;
		float TrailTailRadiusUU = -1.f;

		/** The range of opacity each effect was driven across. A constant here would fail §3's "peaking". */
		float HaloMinOpacity = TNumericLimits<float>::Max();
		float HaloMaxOpacity = -1.f;
		float TrailMinOpacity = TNumericLimits<float>::Max();
		float TrailMaxOpacity = -1.f;
	};

	void Execute(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}

		TSharedRef<FProbe> Probe = MakeShared<FProbe>();
		Probe->World = World;
		const double Seconds = (Args.Num() >= 1) ? FMath::Clamp(FCString::Atod(*Args[0]), 1.0, 120.0) : 12.0;
		Probe->EndsAt = World->GetTimeSeconds() + Seconds;

		UE_LOG(LogTraceGame, Display,
			TEXT("[CoreFx] SPEC v32 §3: watching the Core's FX geometry for %.0fs. Throw the Core and ")
			TEXT("let somebody pick it up (or run Trace.Core.ArtShots alongside this)."), Seconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Probe](float /*Delta*/) -> bool
		{
			UWorld* const TickWorld = Probe->World.Get();
			ATraceCore* const Core = IsValid(TickWorld) ? ATraceCore::Get(TickWorld) : nullptr;
			if (Core == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[CoreFx] no Core (or no world); aborting."));
				return false;
			}

			++Probe->Frames;

			// THE HALO. GetComponentScale, not the relative scale: the world scale is what the renderer
			// uses, so it is the only one that is a claim about what is on screen.
			if (Core->PickupHalo != nullptr && Core->PickupHalo->IsVisible())
			{
				const float RadiusUU = UTraceFxShapes::RadiusUUFromShapeScale(
					static_cast<float>(Core->PickupHalo->GetComponentScale().X));
				++Probe->HaloFrames;
				Probe->HaloMinRadiusUU = FMath::Min(Probe->HaloMinRadiusUU, RadiusUU);
				Probe->HaloMaxRadiusUU = FMath::Max(Probe->HaloMaxRadiusUU, RadiusUU);

				const float Opacity = Core->GetDebugPickupHaloOpacity();
				if (Opacity >= 0.f)
				{
					Probe->HaloMinOpacity = FMath::Min(Probe->HaloMinOpacity, Opacity);
					Probe->HaloMaxOpacity = FMath::Max(Probe->HaloMaxOpacity, Opacity);
				}
			}

			// THE TRAIL. Its length is the sum of the visible segments' own lengths, which is the taper
			// measured rather than the taper requested: if a segment failed to place, this is short.
			float TotalLengthUU = 0.f;
			int32 Visible = 0;
			float HeadRadiusUU = -1.f;
			float TailRadiusUU = -1.f;
			for (UStaticMeshComponent* Segment : Core->ThrownTrailSegments)
			{
				if (Segment == nullptr || !Segment->IsVisible())
				{
					continue;
				}
				const FVector Scale = Segment->GetComponentScale();
				TotalLengthUU += UTraceFxShapes::LengthUUFromShapeScale(static_cast<float>(Scale.Z));
				const float RadiusUU = UTraceFxShapes::RadiusUUFromShapeScale(static_cast<float>(Scale.X));
				if (Visible == 0)
				{
					HeadRadiusUU = RadiusUU;
				}
				TailRadiusUU = RadiusUU;
				++Visible;
			}

			if (Visible > 0)
			{
				++Probe->TrailFrames;
				Probe->TrailSegmentsSeen = FMath::Max(Probe->TrailSegmentsSeen, Visible);
				Probe->TrailMinLengthUU = FMath::Min(Probe->TrailMinLengthUU, TotalLengthUU);
				Probe->TrailMaxLengthUU = FMath::Max(Probe->TrailMaxLengthUU, TotalLengthUU);
				Probe->TrailHeadRadiusUU = HeadRadiusUU;
				Probe->TrailTailRadiusUU = TailRadiusUU;

				const float Opacity = Core->GetDebugThrownTrailOpacity();
				if (Opacity >= 0.f)
				{
					Probe->TrailMinOpacity = FMath::Min(Probe->TrailMinOpacity, Opacity);
					Probe->TrailMaxOpacity = FMath::Max(Probe->TrailMaxOpacity, Opacity);
				}
			}

			if (TickWorld->GetTimeSeconds() < Probe->EndsAt)
			{
				return true;
			}

			// ---- REPORT --------------------------------------------------------------------------
			const bool bHaloSeen = Probe->HaloFrames > 0;
			const bool bTrailSeen = Probe->TrailFrames > 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("[CoreFx] %d frames watched. Blends: halo %s, trail %s (the FX doc asks for ")
				TEXT("translucent; see ETraceFxBlend::Translucent for why additive is the faithful stand-in)."),
				Probe->Frames,
				UTraceFxShapes::BlendName(Core->GetDebugPickupHaloBlend()),
				UTraceFxShapes::BlendName(Core->GetDebugThrownTrailBlend()));

			if (bHaloSeen)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[CoreFx] PICKUP HALO: visible on %d frames, MEASURED radius %.2f -> %.2f uu, ")
					TEXT("MEASURED opacity %.2f -> %.2f (fades out, per the doc). ")
					TEXT("FX doc: r 0.20 m = %.1f uu, x0.6 -> x2.1, i.e. %.1f -> %.1f uu."),
					Probe->HaloFrames, Probe->HaloMinRadiusUU, Probe->HaloMaxRadiusUU,
					Probe->HaloMinOpacity, Probe->HaloMaxOpacity,
					TraceCoreArt::PickupHaloRadiusUU,
					TraceCoreArt::PickupHaloRadiusUU * TraceCoreArt::PickupHaloScaleStart,
					TraceCoreArt::PickupHaloRadiusUU * TraceCoreArt::PickupHaloScaleEnd);
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[CoreFx] PICKUP HALO: NEVER SEEN in %d frames. It fires on the possession edge, ")
					TEXT("so this window contained no pickup - or the halo is broken."), Probe->Frames);
			}

			if (bTrailSeen)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[CoreFx] THROWN TRAIL: visible on %d frames, %d/%d segments placed, MEASURED ")
					TEXT("length %.1f -> %.1f uu (apex knob %.0f uu), MEASURED opacity %.3f -> %.3f ")
					TEXT("(a RANGE is §3's \"peaking mid-flight\"; a single value would mean it is a ")
					TEXT("constant), MEASURED segment radii head %.2f uu ")
					TEXT("tail %.2f uu. FX doc: r 0.055 -> 0.012 m = %.1f -> %.1f uu, so the stacked ")
					TEXT("mid-point radii are %.2f and %.2f."),
					Probe->TrailFrames, Probe->TrailSegmentsSeen, Core->ThrownTrailSegments.Num(),
					Probe->TrailMinLengthUU, Probe->TrailMaxLengthUU,
					CVarCoreThrownTrailLength.GetValueOnGameThread(),
					Probe->TrailMinOpacity, Probe->TrailMaxOpacity,
					Probe->TrailHeadRadiusUU, Probe->TrailTailRadiusUU,
					TraceCoreArt::ThrownTrailHeadRadiusUU, TraceCoreArt::ThrownTrailTailRadiusUU,
					UTraceFxShapes::TaperSegmentRadiusUU(TraceCoreArt::ThrownTrailHeadRadiusUU,
						TraceCoreArt::ThrownTrailTailRadiusUU, 0, TraceCoreArt::ThrownTrailSegmentCount),
					UTraceFxShapes::TaperSegmentRadiusUU(TraceCoreArt::ThrownTrailHeadRadiusUU,
						TraceCoreArt::ThrownTrailTailRadiusUU,
						TraceCoreArt::ThrownTrailSegmentCount - 1, TraceCoreArt::ThrownTrailSegmentCount));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[CoreFx] THROWN TRAIL: NEVER SEEN in %d frames. It draws only while the Core is ")
					TEXT("LOOSE AND MOVING - so this window contained no throw, or the trail is broken."),
					Probe->Frames);
			}

			if (bHaloSeen && bTrailSeen)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[CoreFx] ===== PASS ===== both §3 effects were on screen and measured."));
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[CoreFx] ===== FAILED ===== at least one §3 effect never drew."));
			}

			return false;
		}), 0.f);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GTraceCoreFxProbeCmd(
	TEXT("Trace.Core.FxProbe"),
	TEXT("SPEC v32 §3. Watches the Core's two pieces of FX geometry for N seconds (default 12) and "
	     "reports their radii and lengths MEASURED off the live components, in uu, beside the FX doc's "
	     "own figures. Fails if either effect never drew. Pair it with Trace.Core.ArtShots, which "
	     "stages both a throw and a pickup."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&TraceCoreFxProbe::Execute));

/**
 * `Trace.Core.CarryProbe` - THE CARRIED BALL, PRINTED RATHER THAN ASSERTED.
 *
 * Deliberately not a pass/fail verifier. What is being fixed here is a PICTURE - "you cannot see the
 * ball being held" - and the only honest verdict on a picture is a screenshot. What a log CAN settle
 * is everything a screenshot cannot: which bone the right hand really resolved to ON WHICH RIG, or
 * whether the hip fallback quietly took over - the pair of names is the point, because a Mannequin
 * carrier and a Rocco carrier both used to print "resolved" and only one of them meant it -
 * whether the ACTOR is still at OrbHeight while the BALL is at hand height, where the ball
 * lands in pixels, and which pieces are owner-hidden. Run it with Trace.Core.CarryInHand at 1 and at
 * 0 and the two prints are the A/B, out of one binary.
 */
static FAutoConsoleCommandWithWorldAndArgs GTraceCoreCarryProbeCmd(
	TEXT("Trace.Core.CarryProbe"),
	TEXT("Prints where the carried Core is DRAWN against where its actor is, WHICH bone the holder's "
	     "right hand resolved to and on which body (`hand_r->RightHand1 on SK_Rocco`), where the "
	     "ball lands on this machine's screen in px, and "
	     "bOwnerNoSee on all four drawn pieces. Takes an optional DELAY in seconds, because the "
	     "answer changes during the 0.35 s pull-back: -TraceExec runs a whole command list on ONE "
	     "frame, so `Trace.DebugTakeCore 0 0 90|Trace.Core.CarryProbe 3` is how the settled carry "
	     "state gets printed from a single unattended run. Trace.Core.CarryInHand 0 restores the "
	     "pre-fix picture for the other arm."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		auto Print = [](UWorld* Where)
		{
			ATraceCore* const TheCore = (Where != nullptr) ? ATraceCore::Get(Where) : nullptr;
			if (TheCore == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[CarryProbe] no Core (or no world)."));
				return;
			}
			TheCore->DebugLogCarryState();
		};

		const double Delay = (Args.Num() >= 1) ? FMath::Clamp(FCString::Atod(*Args[0]), 0.0, 60.0) : 0.0;
		if (Delay <= 0.0)
		{
			Print(World);
			return;
		}

		// A DEADLINE ON THE WORLD'S OWN CLOCK, sampled each tick, rather than a countdown accumulated
		// per frame: this file's standing rule, and the reason is that a per-frame accumulator is what
		// shipped two bugs here already.
		TWeakObjectPtr<UWorld> Weak(World);
		const double DueAt = (World != nullptr) ? World->GetTimeSeconds() + Delay : 0.0;
		UE_LOG(LogTraceGame, Display, TEXT("[CarryProbe] will print in %.1fs."), Delay);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Weak, DueAt, Print](float /*Delta*/) -> bool
		{
			UWorld* const Live = Weak.Get();
			if (!IsValid(Live))
			{
				return false;
			}
			if (Live->GetTimeSeconds() < DueAt)
			{
				return true;
			}
			Print(Live);
			return false;
		}), 0.f);
	}));

// =================================================================================================
// SPEC v32 §7b — Trace.Core.ArtShots REPORTED SUCCESS WITHOUT WRITING FILES.
//
// The v31 verifier's finding, verbatim: it "reports `stage 0/1/2: ok` and `screenshot requested` for
// every state WHILE THE GAME IS STILL ON THE CHARACTER-SELECT SCREEN". A harness that cannot fail is
// worthless, and this one passed over its own failure in three separate ways at once. All three are
// fixed here, and each one has an arm that makes it fire:
//
//   1. IT DID NOT WAIT FOR GAMEPLAY. Two FTimerManager timers per beat, started the instant the
//      command was typed, firing at fixed offsets. A timer is not a state: the select screen is up
//      for as long as it takes a human to read five cards, the local pawn EXISTS behind it (this
//      project does not start players as spectators), and every precondition the old code actually
//      checked - "is there a Core", "is there a local pawn" - was already true. So it staged, and
//      photographed a menu. It now WAITS for the game to be live, on a ticker, and gives up loudly
//      if it never becomes live.
//
//      *** RED ARM: Trace.Core.ArtShots.RedArm 1 skips the wait, which is exactly the v31
//      behaviour. Run it on the select screen with the arm on and the harness stages into a menu;
//      run it there with the arm OFF and it refuses and FAILS. Two arms, two answers, so the gate
//      is measuring something. ***
//
//   2. IT NEVER LOOKED AT THE DISK. "screenshot requested" was printed by the line that made the
//      request, which can only ever mean "the request was made". FScreenshotRequest is asynchronous
//      and can decline outright - if a delegate is bound to UGameViewportClient::OnScreenshotCaptured
//      the engine fires that INSTEAD of writing a file, which is precisely the failure mode this
//      section is named after. Every request is now followed to the disk and the byte count printed.
//
//      *** RED ARM: Trace.Core.ArtShots.RedArm 2 does everything except press the shutter. The file
//      cannot appear, and the run must FAIL. ***
//
//   3. IT NAMED EACH FRAME BY THE STATE IT ASKED FOR. `TraceCoreArt_flight1_...png` was written by
//      a beat that had REQUESTED flight; whether the Core was in flight when the shutter fired was
//      never consulted. So a frame of a resting Core - or of a character-select screen - was filed
//      under "flight" and would have been read as evidence for it. The frame is now named by
//      ATraceCore::GetDebugArtState(), the SHIPPING state rule, sampled on the frame the request is
//      made and again on the frame after it (the capture can be serviced at the end of either), and
//      a disagreement between the two is stated in the filename rather than resolved by guessing.
//      A frame whose actual state is not the state the beat asked for FAILS THE RUN.
//
// It is still the same seven beats through the same shipping staging function; only the schedule,
// the verification and the naming changed.
// =================================================================================================

namespace TraceCoreArtShots
{
	/**
	 * SPEC v32 §7b. 0 = off. 1 = stage without waiting for gameplay (the v31 bug, reproduced).
	 * 2 = do everything except request the screenshot (so the on-disk check has something to catch).
	 *
	 * A CVar and not an argument because a red arm should be visible in `Trace.Core.ArtShots.RedArm`
	 * when somebody wonders why a run behaved oddly, and because arming it from -ExecCmds needs a
	 * value assignment rather than a positional parameter.
	 */
	TAutoConsoleVariable<int32> CVarArtShotsRedArm(
		TEXT("Trace.Core.ArtShots.RedArm"),
		0,
		TEXT("SPEC v32 §7b red arm. 0 (default): the fixed harness. 1: SKIP the wait-for-gameplay ")
		TEXT("gate, reproducing the v31 bug where the Core was staged and photographed on the ")
		TEXT("character-select screen. 2: run everything but never request the screenshot, so the ")
		TEXT("on-disk verification has a failure to catch. Both arms must make the run FAIL."),
		ECVF_Default);

	/** One capture: stage at @p StageAt, photograph at @p ShotAt, both seconds from GAMEPLAY GOING LIVE. */
	struct FBeat
	{
		float StageAt;
		float ShotAt;
		int32 Which;
		const TCHAR* Label;
	};

	/**
	 * REST first, FLIGHT second, CARRIED last, and the order is not arbitrary: each stage takes the
	 * Core AWAY from the previous one, so a sequence that ended on a loose Core would leave the match
	 * with the objective on the floor. Ending on CARRIED puts it back in somebody's hands.
	 *
	 * The flight state is photographed FOUR times, at four different ages of the arc, because a
	 * screenshot request is serviced at the end of a later frame and not necessarily the current one:
	 * a single request at a chosen instant is a guess, and four across the arc are a measurement.
	 *
	 * THE OFFSETS ARE NOW MEASURED FROM THE MOMENT GAMEPLAY GOES LIVE, not from the moment the
	 * command was typed. That is the §7b fix for defect 1: the same seven beats, hung off a state
	 * instead of off a stopwatch that started while a menu was up.
	 *
	 * *** AND EVERY FLIGHT BEAT NOW RE-STAGES, WHICH IS A DEFECT THE FIXED HARNESS FOUND IN ITSELF. ***
	 *
	 * The first fixed run reported "wanted flight, actual CARRIED" on beats 3 and 4 and failed. That
	 * was not a false alarm and not a naming bug: a BOT had caught the loose Core 0.39 s after it was
	 * launched, through the shipping catch zone, which is exactly what mode B's bots are for. One
	 * staged throw simply does not survive four shutters. Under the v31 harness those two frames were
	 * still filed as "flight1..4" and would have been read as evidence about a state they do not show.
	 *
	 * So each flight beat launches its own throw and photographs it a fixed age later - 0.10, 0.15,
	 * 0.22 and 0.30 s - which is the same trick, and the same reason, as the two carried beats below.
	 * DebugLaunchLoose is deterministic (identical From and identical velocity every time), so four
	 * ages of four identical arcs ARE four points along one arc, and unlike one arc they cannot be
	 * taken off the field between frames one and four.
	 */
	const FBeat Beats[] =
	{
		{ 0.10f, 1.20f, 0, TEXT("rest")    },
		{ 2.00f, 2.10f, 1, TEXT("flight1") },
		{ 2.30f, 2.45f, 1, TEXT("flight2") },
		{ 2.70f, 2.92f, 1, TEXT("flight3") },
		{ 3.20f, 3.50f, 1, TEXT("flight4") },
		// Both carried beats re-stage first, a tenth of a second before the shutter, for the reason in
		// DebugStageCoreArt's case 2: the bearer is a bot and bots do not stand still to be admired.
		{ 4.20f, 4.35f, 2, TEXT("carried_crack") },
		{ 5.40f, 5.55f, 2, TEXT("carried_idle")  },
	};

	constexpr int32 BeatCount = UE_ARRAY_COUNT(Beats);

	/** The art state a beat is ASKING for, as a name, so the comparison below is state-to-state. */
	const TCHAR* WantedStateName(int32 Which)
	{
		switch (Which)
		{
		case 0:  return ATraceCore::DebugArtStateName(ETraceCoreArtState::Rest);
		case 1:  return ATraceCore::DebugArtStateName(ETraceCoreArtState::Flight);
		default: return ATraceCore::DebugArtStateName(ETraceCoreArtState::Carried);
		}
	}

	/** Everything that became of one requested frame. Every field is printed in the summary. */
	struct FShot
	{
		bool bStaged = false;
		bool bStageRefused = false;
		bool bRequested = false;
		bool bResolved = false;      // the disk answered, one way or the other
		bool bOnDisk = false;
		bool bStateAgreed = false;   // actual == what the beat asked for

		FString RequestedPath;
		FString FinalPath;
		FString StateAtRequest;
		FString StateAfterFrame;
		double RequestedAt = 0.0;
		int64 Bytes = 0;
	};

	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		float Distance = 420.f;
		float ReadyTimeout = 30.f;
		int32 RedArm = 0;
		double StartedAt = 0.0;
		double ReadyAt = -1.0;
		double LastWaitLogAt = -1000.0;
		FString LastNotReadyReason;
		FString StampSuffix;
		FShot Shots[BeatCount];
	};

	/**
	 * ONE RUN AT A TIME, and it is a weak flag rather than a queue.
	 *
	 * Two overlapping runs would fight over the Core: the second run's REST stage launches the ball
	 * out of the bearer the first run's CARRIED stage just handed it to, and both would then report
	 * on frames the other one staged. Refusing is the only honest answer.
	 */
	bool bRunActive = false;

	/**
	 * *** THE §7b GATE: IS THIS ACTUALLY GAMEPLAY? ***
	 *
	 * Every one of these was true on the character-select screen except the ones marked, which is why
	 * the v31 harness sailed straight through. The reason string is returned so the log says WHICH
	 * condition is still unmet rather than "not ready", which is not something anybody can act on.
	 *
	 * @param bNeedSecondPawn  the CARRIED beats need a second living body; the Core is bOwnerNoSee and
	 *                         a player can never photograph their own. Checked here so the run does not
	 *                         start at all in a session that cannot finish it.
	 */
	bool IsGameplayLive(UWorld* World, bool bNeedSecondPawn, FString& OutWhyNot)
	{
		if (World == nullptr)
		{
			OutWhyNot = TEXT("no world");
			return false;
		}

		if (World->GetGameState() == nullptr)
		{
			OutWhyNot = TEXT("no GameState yet (the map is still coming up)");
			return false;
		}

		ATraceCore* const Core = ATraceCore::Get(World);
		if (Core == nullptr)
		{
			OutWhyNot = TEXT("no Core in this world");
			return false;
		}
		if (!Core->HasAuthority())
		{
			OutWhyNot = TEXT("this machine is not the server; stage on the listen host");
			return false;
		}

		APlayerController* const PC = World->GetFirstPlayerController();
		if (PC == nullptr)
		{
			OutWhyNot = TEXT("no local player controller");
			return false;
		}

		ATraceCharacter* const Local = Cast<ATraceCharacter>(PC->GetPawn());
		if (!IsValid(Local) || !Local->IsAlive())
		{
			OutWhyNot = TEXT("the local player has no living pawn");
			return false;
		}

		// *** THE ONE THE v31 RUN WAS SITTING IN. *** This project does not start players as
		// spectators, so a pawn EXISTS behind the select screen and every pawn test above passes while
		// a menu fills the frame. Nothing but the screen's own flag answers "is a menu up".
		ATracePlayerState* const LocalState = Cast<ATracePlayerState>(PC->PlayerState);
		if (LocalState == nullptr)
		{
			OutWhyNot = TEXT("the local player has no PlayerState yet");
			return false;
		}
		if (LocalState->IsCharacterSelectOpen())
		{
			OutWhyNot = TEXT("THE CHARACTER-SELECT SCREEN IS OPEN (this is the v31 failure)");
			return false;
		}
		if (LocalState->Team == ETraceTeam::None)
		{
			OutWhyNot = TEXT("the local player has not been given a team yet");
			return false;
		}

		// AND THE ONE THAT CATCHES THE FRAME BEFORE THE SCREEN OPENS. PollCharacterSelect runs at
		// 4 Hz and needs a team first, so for the first fraction of a second of a session the screen
		// is not open YET and the flag above is a false negative. "Locked in" is only false-negative
		// in the direction that costs a wait. Skipped entirely when characters are switched off for
		// the session (mode A, or the settings toggle), where nobody is ever locked in and requiring
		// it would hang forever.
		if (UTraceAbilityComponent::AreCharactersEnabled(World) && !LocalState->bCharacterLocked)
		{
			OutWhyNot = TEXT("the local player has not locked in a character yet");
			return false;
		}

		// THE MATCH HAS ACTUALLY KICKED OFF. Until ATraceGameMode calls KickoffTo() the Core belongs
		// to nobody and is parked at home - a real state, and not one any of these beats is about. It
		// is also the cheapest possible proof that the match loop is turning rather than initialising.
		if (!IsValid(Core->Carrier) && !Core->bLoose)
		{
			OutWhyNot = TEXT("the match has not kicked off (the Core has no holder and is not loose)");
			return false;
		}

		if (!Core->IsPackArtActive())
		{
			// Not fatal to a screenshot, but it IS fatal to the evidence: these frames exist to show
			// SK_TraceCore's three poses, and the fallback sphere has none of them.
			OutWhyNot = TEXT("the pack Core art did not resolve; these frames would photograph the "
			                 "fallback sphere (`git lfs pull` then Scripts/import-pack.sh)");
			return false;
		}

		if (bNeedSecondPawn)
		{
			TArray<ATraceCharacter*> Characters;
			Core->GatherCharacters(Characters);
			bool bFound = false;
			for (ATraceCharacter* Candidate : Characters)
			{
				if (IsValid(Candidate) && Candidate != Local && Candidate->IsAlive())
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				OutWhyNot = TEXT("no second living pawn to carry the Core - run with bots, or join a client");
				return false;
			}
		}

		OutWhyNot.Reset();
		return true;
	}

	/** The state the Core is in ON THIS FRAME, by the shipping rule. Never null; safe in a filename. */
	FString SampleState(UWorld* World)
	{
		const ATraceCore* const Core = ATraceCore::Get(World);
		return (Core != nullptr) ? FString(ATraceCore::DebugArtStateName(Core->GetDebugArtState()))
		                         : FString(TEXT("nocore"));
	}

	/** Requests one frame, named by the state the Core is ACTUALLY in. Fills in the FShot. */
	void Shoot(FRun& Run, int32 Index)
	{
		UWorld* const World = Run.World.Get();
		FShot& Shot = Run.Shots[Index];
		const FBeat& Beat = Beats[Index];

		Shot.StateAtRequest = SampleState(World);
		Shot.bStateAgreed = Shot.StateAtRequest.Equals(WantedStateName(Beat.Which));

		// THE ACTUAL STATE LEADS THE FILENAME AND THE REQUESTED LABEL FOLLOWS IT, because the first
		// token is what somebody sorting a directory of frames reads. The label is kept so a frame can
		// still be traced back to the beat that asked for it - "flight2" says which of the four passes
		// this was - but it can no longer be mistaken for a claim about what is in the picture.
		const FString FileName = FString::Printf(TEXT("TraceCoreArt_%s_asked-%s_%s.png"),
			*Shot.StateAtRequest, Beat.Label, *Run.StampSuffix);

		Shot.RequestedPath = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);
		Shot.RequestedAt = (World != nullptr) ? World->GetTimeSeconds() : 0.0;
		Shot.bRequested = true;

		if (Run.RedArm == 2)
		{
			// RED ARM 2. Everything but the shutter. The file cannot appear, so the on-disk check
			// below has a real failure to catch and the run must go red.
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CoreArt] RED ARM 2: shutter suppressed for '%s'. The on-disk check must now fail."),
				Beat.Label);
			return;
		}

		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Shot.RequestedPath));
		FScreenshotRequest::RequestScreenshot(Shot.RequestedPath, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);

		UE_LOG(LogTraceGame, Display,
			TEXT("[CoreArt] beat '%s' (wants %s): Core is actually %s%s. Screenshot requested -> %s"),
			Beat.Label, WantedStateName(Beat.Which), *Shot.StateAtRequest,
			Shot.bStateAgreed ? TEXT("") : TEXT("  *** MISMATCH ***"), *Shot.RequestedPath);
	}

	/** How long a requested frame is given to appear on disk before it is called missing. */
	constexpr double FileWaitSeconds = 4.0;

	/**
	 * Polls the disk for one requested frame.
	 *
	 * Also RENAMES it when the state moved between the request frame and the frame after it. The
	 * filename has to be chosen before the request, and the engine may service the capture at the end
	 * of either frame, so those two samples bracket the frame that was actually taken: if they agree
	 * the name is certain, and if they do not, saying so in the name is the only honest option. The
	 * alternative - keeping the earlier guess - is the v31 defect in miniature.
	 */
	void ResolveShot(FRun& Run, int32 Index, double Now)
	{
		FShot& Shot = Run.Shots[Index];
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		if (!PlatformFile.FileExists(*Shot.RequestedPath))
		{
			if (Now - Shot.RequestedAt >= FileWaitSeconds)
			{
				Shot.bResolved = true;
				Shot.bOnDisk = false;
				UE_LOG(LogTraceGame, Error,
					TEXT("[CoreArt] NO FILE. %.1fs after the request there is nothing at: %s"),
					FileWaitSeconds, *Shot.RequestedPath);
			}
			return;
		}

		Shot.bResolved = true;
		Shot.bOnDisk = true;
		Shot.Bytes = PlatformFile.FileSize(*Shot.RequestedPath);
		Shot.FinalPath = Shot.RequestedPath;

		if (!Shot.StateAfterFrame.IsEmpty() && !Shot.StateAfterFrame.Equals(Shot.StateAtRequest))
		{
			const FString Ambiguous = Shot.RequestedPath.Replace(
				*FString::Printf(TEXT("TraceCoreArt_%s_"), *Shot.StateAtRequest),
				*FString::Printf(TEXT("TraceCoreArt_%s-or-%s_"), *Shot.StateAtRequest, *Shot.StateAfterFrame));

			if (IFileManager::Get().Move(*Ambiguous, *Shot.RequestedPath))
			{
				Shot.FinalPath = Ambiguous;
			}

			// A WARNING and not an error: the frame is real and the file is on disk, and the harness
			// is telling the truth about not knowing which of two states it caught. A run is not
			// failed for it, because the four flight beats exist precisely to make one uncertain
			// frame survivable.
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CoreArt] the Core changed state across the capture window (%s -> %s); the frame "
				     "is named for both: %s"),
				*Shot.StateAtRequest, *Shot.StateAfterFrame, *Shot.FinalPath);
		}

		UE_LOG(LogTraceGame, Display, TEXT("[CoreArt] frame ON DISK (%lld bytes): %s"),
			Shot.Bytes, *Shot.FinalPath);
	}

	/** The verdict. Loud, and it says which of the four ways it failed. */
	void Report(FRun& Run)
	{
		int32 Requested = 0, OnDisk = 0, Mismatched = 0, Refused = 0;
		for (const FShot& Shot : Run.Shots)
		{
			Requested += Shot.bRequested ? 1 : 0;
			OnDisk += Shot.bOnDisk ? 1 : 0;
			Mismatched += (Shot.bRequested && !Shot.bStateAgreed) ? 1 : 0;
			Refused += Shot.bStageRefused ? 1 : 0;
		}

		const bool bNeverReady = Run.ReadyAt < 0.0;
		const bool bPass = !bNeverReady && Refused == 0 && Requested == BeatCount
			&& OnDisk == BeatCount && Mismatched == 0;

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[CoreArt] ===== PASS ===== %d/%d frames verified ON DISK, every one named for the "
				     "state the Core was actually in. Saved/Screenshots/TraceCoreArt_*_%s.png"),
				OnDisk, BeatCount, *Run.StampSuffix);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CoreArt] ===== FAILED ===== requested %d/%d, on disk %d/%d, stage refusals %d, "
				     "state mismatches %d%s%s"),
				Requested, BeatCount, OnDisk, BeatCount, Refused, Mismatched,
				bNeverReady ? TEXT(" | GAMEPLAY NEVER WENT LIVE: ") : TEXT(""),
				bNeverReady ? *Run.LastNotReadyReason : TEXT(""));
		}

		// The per-frame ledger, pass or fail, because a summary line is a claim and this is the
		// evidence for it. `ls` on the paths below is the check a reviewer can run themselves.
		for (int32 Index = 0; Index < BeatCount; ++Index)
		{
			const FShot& Shot = Run.Shots[Index];
			UE_LOG(LogTraceGame, Display,
				TEXT("[CoreArt]   beat %d '%s': wanted %-7s actual %-7s %s %s"),
				Index, Beats[Index].Label, WantedStateName(Beats[Index].Which),
				Shot.StateAtRequest.IsEmpty() ? TEXT("-") : *Shot.StateAtRequest,
				Shot.bStateAgreed ? TEXT("  ") : TEXT("!!"),
				Shot.bOnDisk ? *FString::Printf(TEXT("%lld bytes  %s"), Shot.Bytes, *Shot.FinalPath)
				             : TEXT("NO FILE"));
		}
	}

	void Execute(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}

		if (bRunActive)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CoreArt] a run is already in progress; refusing. Two runs would stage over each other."));
			return;
		}

		TSharedRef<FRun> State = MakeShared<FRun>();
		State->World = World;
		State->StartedAt = World->GetTimeSeconds();
		State->RedArm = CVarArtShotsRedArm.GetValueOnGameThread();
		State->StampSuffix = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

		if (Args.Num() >= 1)
		{
			State->Distance = FCString::Atof(*Args[0]);
		}
		if (Args.Num() >= 2)
		{
			State->ReadyTimeout = FMath::Max(0.f, FCString::Atof(*Args[1]));
		}

		bRunActive = true;

		UE_LOG(LogTraceGame, Display,
			TEXT("[CoreArt] SPEC v32 §7b: %d beats at %.0f uu. Waiting up to %.1fs for real gameplay "
			     "before staging anything.%s"),
			BeatCount, State->Distance, State->ReadyTimeout,
			(State->RedArm != 0)
				? *FString::Printf(TEXT("  *** RED ARM %d ENGAGED - THIS RUN IS EXPECTED TO FAIL. ***"), State->RedArm)
				: TEXT(""));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float /*Delta*/) -> bool
		{
			UWorld* const TickWorld = State->World.Get();
			if (!IsValid(TickWorld))
			{
				UE_LOG(LogTraceGame, Error, TEXT("[CoreArt] ===== FAILED ===== the world went away mid-run."));
				bRunActive = false;
				return false;
			}

			const double Now = TickWorld->GetTimeSeconds();

			// ---- PHASE 1: WAIT FOR GAMEPLAY ------------------------------------------------------
			if (State->ReadyAt < 0.0)
			{
				if (State->RedArm == 1)
				{
					// RED ARM 1. The v31 behaviour exactly: start staging the moment the command is
					// typed, whatever is on screen. Left as a single branch so the difference between
					// the arms is one condition and not two code paths.
					UE_LOG(LogTraceGame, Warning,
						TEXT("[CoreArt] RED ARM 1: skipping the wait-for-gameplay gate. This is the v31 "
						     "behaviour that photographed the character-select screen."));
					State->ReadyAt = Now;
				}
				else if (IsGameplayLive(TickWorld, /*bNeedSecondPawn=*/true, State->LastNotReadyReason))
				{
					State->ReadyAt = Now;
					UE_LOG(LogTraceGame, Display,
						TEXT("[CoreArt] gameplay is live after %.2fs; staging starts now."),
						Now - State->StartedAt);
				}
				else if (Now - State->StartedAt >= static_cast<double>(State->ReadyTimeout))
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[CoreArt] gave up after %.1fs waiting for gameplay. Last reason: %s"),
						Now - State->StartedAt, *State->LastNotReadyReason);
					Report(*State);
					bRunActive = false;
					return false;
				}
				else
				{
					// Throttled to once a second so a 30 s wait is 30 lines and not 1800. The latch is
					// on the RUN and not a function-local static: a static would be shared by every run
					// in the session, so a second run started inside a second of the first would be
					// silent about why it was waiting.
					if (Now - State->LastWaitLogAt >= 1.0)
					{
						State->LastWaitLogAt = Now;
						UE_LOG(LogTraceGame, Display, TEXT("[CoreArt] waiting for gameplay (%.0fs left): %s"),
							static_cast<double>(State->ReadyTimeout) - (Now - State->StartedAt),
							*State->LastNotReadyReason);
					}
					return true;
				}
			}

			const double Elapsed = Now - State->ReadyAt;

			// ---- PHASE 2: STAGE, SHOOT, AND FOLLOW EACH FRAME TO THE DISK ------------------------
			for (int32 Index = 0; Index < BeatCount; ++Index)
			{
				const FBeat& Beat = Beats[Index];
				FShot& Shot = State->Shots[Index];

				if (Beat.StageAt >= 0.f && !Shot.bStaged && Elapsed >= static_cast<double>(Beat.StageAt))
				{
					Shot.bStaged = true;
					FString StageReport;
					const bool bOk = ATraceCore::DebugStageCoreArt(TickWorld, Beat.Which, State->Distance, StageReport);
					Shot.bStageRefused = !bOk;
					// TWO UE_LOGs AND NOT ONE WITH A TERNARY VERBOSITY: UE_LOG needs the verbosity as a
					// literal token - it builds a type name out of it - so `bOk ? Display : Error` does
					// not compile. Spelling both out is also the only way a refusal reaches Error, which
					// is what makes a refused stage visible in a log somebody is grepping for failures.
					if (bOk)
					{
						UE_LOG(LogTraceGame, Display, TEXT("[CoreArt] stage %d for '%s': ok | %s"),
							Beat.Which, Beat.Label, *StageReport);
					}
					else
					{
						UE_LOG(LogTraceGame, Error, TEXT("[CoreArt] stage %d for '%s': REFUSED | %s"),
							Beat.Which, Beat.Label, *StageReport);
					}
				}

				if (!Shot.bRequested && Elapsed >= static_cast<double>(Beat.ShotAt))
				{
					Shoot(*State, Index);
					continue;   // The second state sample belongs to the NEXT tick, not this one.
				}

				if (Shot.bRequested && Shot.StateAfterFrame.IsEmpty())
				{
					// The frame after the request. See ResolveShot: these two samples bracket the
					// frame the engine actually captured.
					Shot.StateAfterFrame = SampleState(TickWorld);
				}

				if (Shot.bRequested && !Shot.bResolved)
				{
					ResolveShot(*State, Index, Now);
				}
			}

			// ---- PHASE 3: DONE WHEN EVERY FRAME HAS ANSWERED -------------------------------------
			bool bAllResolved = true;
			for (const FShot& Shot : State->Shots)
			{
				bAllResolved = bAllResolved && Shot.bResolved;
			}

			// A HARD DEADLINE AS WELL, because "every frame has answered" is a condition and a ticker
			// that waits on a condition which can never arrive is a leak that reports nothing at all.
			// The last beat's shutter, plus the disk wait, plus a couple of seconds of slack.
			const double Deadline = static_cast<double>(Beats[BeatCount - 1].ShotAt) + FileWaitSeconds + 3.0;
			if (!bAllResolved && Elapsed >= Deadline)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[CoreArt] deadline: %.1fs after gameplay went live, not every frame had answered."),
					Elapsed);
				Report(*State);
				bRunActive = false;
				return false;
			}

			if (bAllResolved)
			{
				Report(*State);
				bRunActive = false;
				return false;
			}

			return true;
		}), 0.f);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GTraceCoreArtShotsCmd(
	TEXT("Trace.Core.ArtShots"),
	TEXT("SPEC v32 §7b (was v31 §4). Listen host, mode B. WAITS for real gameplay - a living local "
	     "pawn, the character-select screen closed, a character locked in, the match kicked off - then "
	     "stages the Core at rest, in flight and carried by another pawn, photographs each, VERIFIES "
	     "the file landed on disk, and names every frame by the state the Core was actually in. Fails "
	     "loudly if any of that does not happen. Args: [distance uu = 420] [wait-for-gameplay "
	     "seconds = 30]. Red arms: Trace.Core.ArtShots.RedArm 1 / 2."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&TraceCoreArtShots::Execute));

#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING

// --- The reproduction ------------------------------------------------------------------------------

bool ATraceCore::RequestGoalRepro()
{
	if (!HasAuthority())
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[CoreAudit] Trace.Core.GoalRepro is a SERVER command - run it on the listen server, not the client."));
		return false;
	}

	if (bGoalReproArmed)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[CoreAudit] GoalRepro already staged; ignoring."));
		return false;
	}

	// A goal needs a carrier. If nobody has it (a kickoff window), take the first living player and
	// give it to them through the ordinary funnel.
	if (!IsValid(Carrier))
	{
		TArray<ATraceCharacter*> Characters;
		GatherCharacters(Characters);

		ATraceCharacter* Candidate = nullptr;
		for (ATraceCharacter* Character : Characters)
		{
			if (IsValid(Character) && Character->IsAlive())
			{
				Candidate = Character;
				break;
			}
		}

		if (Candidate == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[CoreAudit] GoalRepro: nobody alive to carry the Core."));
			return false;
		}

		bOutOfPlay = false;
		GrantTo(Candidate, ETraceCoreGrantReason::Debug);
	}

	if (!IsValid(Carrier))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[CoreAudit] GoalRepro: the grant did not take; aborting."));
		return false;
	}

	// STAGE ONE: put the holder as far from the centre pedestal as the field allows, which is what
	// makes this the longest reset in the game and the one the user reported. The carrier keeps their
	// Y and Z, so they stay on the floor and inside the arena.
	const UWorld* World = GetWorld();
	const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
	const FBox Field = (Arena != nullptr) ? Arena->GetFieldBounds() : FBox(ForceInit);

	const FVector CarrierNow = Carrier->GetActorLocation();
	FVector Staged = CarrierNow;
	if (Field.IsValid != 0)
	{
		// The end furthest from home, but 4000 uu inside the wall — SHORT OF THE ENDZONE ON PURPOSE.
		// ATraceArenaBuilder::EndzoneDepth is 2400 uu, so parking the carrier any deeper would trip a
		// REAL score the instant they arrived, and the reset would fire before the client had seen the
		// Core out there at all. Staging short means the fire below is the only reset in the window,
		// and it is the same KickoffTo a real goal produces.
		const FVector Home = GetHomeLocation();
		const double FarX = (FMath::Abs(Field.Max.X - Home.X) >= FMath::Abs(Home.X - Field.Min.X))
			? (Field.Max.X - 4000.0) : (Field.Min.X + 4000.0);
		Staged.X = FarX;
	}

	Carrier->SetActorLocation(Staged, false, nullptr, ETeleportType::TeleportPhysics);
	Carrier->ForceNetUpdate();

	GoalReproTeam = TraceOpposingTeam(Carrier->GetTeam());
	// Long enough that a 40 ms client has certainly rendered the Core out there before the reset -
	// the whole question is what that client does NEXT, and staging it too fast would let the two
	// events arrive in one bunch and hide the bug.
	GoalReproFireTime = GetServerTimeSeconds() + 1.5f;
	bGoalReproArmed = true;

	UE_LOG(LogTraceGame, Display,
		TEXT("[CoreAudit] GoalRepro staged: %s carried the Core to %s (%.0f uu from home). ")
		TEXT("Firing the post-goal KickoffTo(%s) in 1.5s."),
		*GetNameSafe(Carrier), *Staged.ToCompactString(),
		FVector::Dist(Staged, GetHomeLocation()), *TraceTeamName(GoalReproTeam).ToString());

	return true;
}

void ATraceCore::TickGoalRepro()
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// --- Auto-arming. SAME GATE AS THE SPEC v8 §0 MOMENTUM TEST, for the same reason. --------------
	//
	// The delay is a FLOOR and not the release condition; what actually releases a run is a REMOTE
	// CLIENT'S PAWN EXISTING. §10 is a client-side bug, so a reset fired while the second process is
	// still loading plugins measures the host and nothing else - which is precisely the failure spec
	// v8 §0 exists to stop, and it has bitten this file once already.
	const int32 RequestedRuns = CVarCoreGoalReproRuns.GetValueOnGameThread();
	if (RequestedRuns > 0 && GoalReproRunsDone < RequestedRuns && !bGoalReproArmed)
	{
		const float NowReal = static_cast<float>(World->GetTimeSeconds());
		const float Due = (GoalReproRunsDone == 0)
			? CVarCoreGoalReproDelay.GetValueOnGameThread()
			: GoalReproNextAutoTime;

		if (NowReal >= Due && HasRemoteClientPawn())
		{
			// Counted only on a run that actually STAGED. A failed attempt (nobody alive yet) must not
			// silently consume one of the runs the operator asked for.
			if (RequestGoalRepro())
			{
				++GoalReproRunsDone;
				GoalReproNextAutoTime = NowReal + FMath::Max(4.f, CVarCoreGoalReproInterval.GetValueOnGameThread());
				UE_LOG(LogTraceGame, Display, TEXT("[CoreAudit] GoalRepro run %d of %d staged."),
					GoalReproRunsDone, RequestedRuns);
			}
		}
	}

	if (!bGoalReproArmed || GetServerTimeSeconds() < GoalReproFireTime)
	{
		return;
	}

	bGoalReproArmed = false;

	UE_LOG(LogTraceGame, Display,
		TEXT("[CoreAudit] GoalRepro FIRING: Core at %s, %.0f uu from home. This is the exact call ")
		TEXT("ATraceGameMode::NotifyScored makes on a goal."),
		*GetActorLocation().ToCompactString(), FVector::Dist(GetActorLocation(), GetHomeLocation()));

	// STAGE TWO. Not a simulation of a goal's reset - it IS a goal's reset. NotifyScored's only
	// effect on this actor is this call.
	KickoffTo(GoalReproTeam);
}


// --- The audit -------------------------------------------------------------------------------------

void ATraceCore::TickTeleportAudit()
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (CVarCoreTeleportAudit.GetValueOnGameThread() == 0)
	{
		bAuditHasLast = false;
		bAuditWindowOpen = false;
		return;
	}

	// HOST / SERVER / CLIENT, printed on every line, because the entire point of §10 is that the host
	// and the client disagree and only one of them has the bug (spec v8 §0).
	const TCHAR* Machine = HasAuthority()
		? (World->GetNetMode() == NM_ListenServer ? TEXT("HOST") : TEXT("SERVER"))
		: TEXT("CLIENT");

	const FVector Now = GetActorLocation();
	const FVector Home = GetHomeLocation();
	const double HomeError = FVector::Dist(Now, Home);
	const bool bHeld = IsValid(Carrier);
	const float DeltaSeconds = World->GetDeltaSeconds();

	const double Step = bAuditHasLast ? FVector::Dist(Now, AuditLastLocation) : 0.0;

	// A single frame that moves the Core further than a player could be expected to. Logged wherever
	// it happens, in or out of a window: this is the line that says "it crossed the map in one frame".
	if (bAuditHasLast && Step > static_cast<double>(CVarCoreTeleportAuditJump.GetValueOnGameThread()))
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[CoreAudit] %s JUMP %.0f uu in one frame (%.3f s): %s -> %s | holder %s, loose %d, serial %u"),
			Machine, Step, DeltaSeconds,
			*AuditLastLocation.ToCompactString(), *Now.ToCompactString(),
			bHeld ? *GetNameSafe(Carrier) : TEXT("none"), bLoose ? 1 : 0,
			static_cast<uint32>(TeleportSerial));
	}

	// A possession that has just ENDED is the event §10 is about. Open a window and watch.
	if (bAuditHasLast && bAuditWasHeld && !bHeld && !bAuditWindowOpen)
	{
		bAuditWindowOpen = true;
		AuditWindowEndTime = static_cast<float>(World->GetTimeSeconds())
			+ FMath::Max(0.5f, CVarCoreTeleportAuditWindow.GetValueOnGameThread());
		AuditPathLength = 0.0;
		AuditMaxStep = 0.0;
		AuditWorstHomeError = 0.0;
		AuditAwayFromHomeSeconds = 0.f;
		AuditMovingFrames = 0;
		AuditFrames = 0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[CoreAudit] %s possession ended with the Core at %s (%.0f uu from home). Watching for %.1fs."),
			Machine, *Now.ToCompactString(), HomeError,
			FMath::Max(0.5f, CVarCoreTeleportAuditWindow.GetValueOnGameThread()));
	}

	if (bAuditWindowOpen)
	{
		++AuditFrames;
		AuditPathLength += Step;
		AuditMaxStep = FMath::Max(AuditMaxStep, Step);

		// 1 uu of movement in a frame is the difference between "it moved" and float noise. What this
		// counts is how MANY frames the travel was spread over, which is the whole SLIDE-versus-JUMP
		// question the spec's lead and the measured cause disagree about.
		if (Step > 1.0)
		{
			++AuditMovingFrames;
		}

		// Only meaningful while nobody is holding it: a Core riding a live holder is legitimately
		// nowhere near home.
		if (!bHeld && !bLoose)
		{
			AuditWorstHomeError = FMath::Max(AuditWorstHomeError, HomeError);
			if (HomeError > FMath::Sqrt(static_cast<double>(TraceCoreTuning::HomeToleranceSq)))
			{
				AuditAwayFromHomeSeconds += DeltaSeconds;
			}
		}

		if (static_cast<float>(World->GetTimeSeconds()) >= AuditWindowEndTime)
		{
			bAuditWindowOpen = false;

			// THE VERDICT, and the three cases it has to be able to tell apart:
			//   SNAP     one frame of travel, and the Core was never left far from where it belongs.
			//   SLIDE    the travel was spread over many frames - interpolation, the spec's lead.
			//   STRANDED it sat far from home for a measurable time and then crossed in one frame.
			const double HomeTolerance = FMath::Sqrt(static_cast<double>(TraceCoreTuning::HomeToleranceSq));
			const TCHAR* Verdict = TEXT("SNAP (clean)");
			if (AuditMovingFrames > 2 && AuditPathLength > 4.0 * AuditMaxStep)
			{
				Verdict = TEXT("SLIDE - travelled over many frames (interpolation)");
			}
			else if (AuditAwayFromHomeSeconds > 0.15f)
			{
				Verdict = TEXT("STRANDED then JUMPED - the reset was never applied here");
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[CoreAudit] %s reset summary: %d frames, %d moving, path %.0f uu, max step %.0f uu, ")
				TEXT("worst dist from home %.0f uu, held wrong place for %.2f s (tolerance %.0f uu) => %s"),
				Machine, AuditFrames, AuditMovingFrames, AuditPathLength, AuditMaxStep,
				AuditWorstHomeError, AuditAwayFromHomeSeconds, HomeTolerance, Verdict);
		}
	}

	AuditLastLocation = Now;
	bAuditHasLast = true;
	bAuditWasHeld = bHeld;
}

#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING

// =================================================================================================
// SPEC v25 §2 — THE RED ARM
//
// Four negative claims, and a negative claim is worthless until the positive one beside it has been
// watched to fire on the same pawns, in the same window, through the same functions:
//
//   1. A SAME-TEAM PLAYER MUST FAIL TO PULL      (green: the opposing player, standing there, passes)
//   2. A PLAYER WITH NO LINE OF SIGHT MUST FAIL  (green: the same player, same frame, unblocked)
//   3. RELEASING AT 0.29 s MUST FAIL             (green: the same player holding past 0.30 s wins it)
//   4. THE LOCKOUT MUST EXPIRE                   (red:   the locked-out player standing ON the Core
//                                                        is refused for the whole window)
//
// It drives the SHIPPING functions — CanPullNow, RequestPullInput, ServerTickTurnover, the pickup
// poll — and the only thing it fakes is the throw that would otherwise have to be arranged on cue.
// Armed from -ExecCmds as well as the console, because this project's testing policy forbids typing
// into a window.
//
// It needs BOTS to drive: a human's crosshair is theirs and this harness will not move it. On a
// bot-free session every step reports SKIP rather than passing on an empty room.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarModeBTurnoverVerify(
	TEXT("Trace.ModeB.TurnoverVerify"),
	0,
	TEXT("SPEC v25 §2. 1: run the turnover red arm once - same-team pull refused, no-line-of-sight ")
	TEXT("pull refused, a 0.29s release refused, a full hold completing and delivering at the thrown ")
	TEXT("speed, and the 5s lockout refusing then releasing the team that dropped it."),
	ECVF_Default);

namespace TraceCoreTurnoverVerify
{
	/** How long any one step may take before it is declared inconclusive rather than hanging the run. */
	constexpr float StepTimeoutSeconds = 6.0f;

	/** Times step 5 will re-arm if somebody legally takes the Core out from under it. */
	constexpr int32 LockoutRetries = 2;

	/**
	 * A bot on @p Team the harness may drive.
	 *
	 * BOTS ONLY, and deliberately: driving a human's control rotation from a test would be moving the
	 * player's own mouse, and the hover rule under test is precisely "where is this player looking".
	 */
	ATraceCharacter* FindDrivableBot(const TArray<ATraceCharacter*>& All, ETraceTeam Team)
	{
		for (ATraceCharacter* Candidate : All)
		{
			if (!IsValid(Candidate) || !Candidate->IsAlive() || Candidate->GetTeam() != Team)
			{
				continue;
			}

			const AController* Controller = Candidate->GetController();
			if (Controller == nullptr || Controller->IsPlayerController())
			{
				continue;
			}

			return Candidate;
		}

		return nullptr;
	}

	/**
	 * A place to park the Core that @p Puller can actually SEE.
	 *
	 * THIS IS NOT CONVENIENCE, IT IS WHAT MAKES THE GREEN ARMS MEAN ANYTHING. The first version of
	 * this harness dropped the Core at the locked-out bot's feet, wherever that bot happened to be on
	 * a 33600 uu pitch full of cover, and the green arm of steps 1 and 2 then failed with "no line of
	 * sight" — reporting a broken rule when what was broken was the test's own geometry. A red arm
	 * whose green half cannot fire proves nothing, so the point is CHOSEN: eight compass directions
	 * at arm's length, dropped onto whatever is underneath, and the first one the puller has a clear
	 * ray to wins.
	 *
	 * @return false when the puller can see none of the eight, which is reported as a SKIP.
	 */
	bool FindPullablePoint(const ATraceCharacter* Puller, double OrbRadius, FVector& OutPoint)
	{
		const UWorld* World = (Puller != nullptr) ? Puller->GetWorld() : nullptr;
		if (World == nullptr)
		{
			return false;
		}

		const FVector Eye = Puller->GetPawnViewLocation();
		const FVector Origin = Puller->GetActorLocation();
		// FAR ENOUGH THAT THE PULLER CANNOT SIMPLY WALK TO IT. At 600 uu a bot chasing the loose Core
		// arrived on foot inside the 0.3 s hold and step 4 measured the pickup radius instead of the
		// pull; at 1400 uu it needs ~2 s to cover ground the delivery crosses in 0.6.
		const double Reach = 1400.0;

		for (int32 Step = 0; Step < 8; ++Step)
		{
			const double Angle = static_cast<double>(Step) * (PI / 4.0);
			const FVector Offset(FMath::Cos(Angle) * Reach, FMath::Sin(Angle) * Reach, 0.0);

			FCollisionQueryParams DropParams(SCENE_QUERY_STAT(TraceCoreVerifyDrop), /*bTraceComplex=*/false);
			DropParams.AddIgnoredActor(Puller);

			FHitResult Ground;
			FVector Candidate = Origin + Offset;

			if (World->LineTraceSingleByChannel(Ground, Candidate + FVector(0.0, 0.0, 300.0),
				Candidate - FVector(0.0, 0.0, 2000.0), ECC_WorldStatic, DropParams))
			{
				Candidate = Ground.ImpactPoint + FVector(0.0, 0.0, OrbRadius + 2.0);
			}
			else
			{
				continue;   // Nothing underneath: a hole, or off the edge of the field.
			}

			FCollisionQueryParams LosParams(SCENE_QUERY_STAT(TraceCoreVerifyLos), /*bTraceComplex=*/false);
			LosParams.AddIgnoredActor(Puller);

			if (!World->LineTraceTestByChannel(Eye, Candidate, ECC_Visibility, LosParams))
			{
				OutPoint = Candidate;
				return true;
			}
		}

		return false;
	}
}

bool ATraceCore::DebugRegisterTurnover(ETraceTeam DroppingTeam, const FVector& Where)
{
	if (!HasAuthority() || !IsModeB() || DroppingTeam == ETraceTeam::None)
	{
		return false;
	}

	// Loose, as if DroppingTeam had thrown it, with no launch velocity: this harness is about the
	// WINDOW, and a flight would only add a landing the shipping rule has already been proven on.
	if (!DebugLaunchLoose(Where, FVector::ZeroVector, DroppingTeam, /*bAsThrow=*/true))
	{
		return false;
	}

	RegisterTurnover(DroppingTeam, Where, TEXT("armed by Trace.ModeB.TurnoverVerify"));
	return IsTurnoverActive();
}

#if !UE_BUILD_SHIPPING
bool ATraceCore::DebugStageTurnoverAtLocalCrosshair(UWorld* World, float DistanceUU, bool bLockLocalTeam,
	FString& OutReport)
{
	if (World == nullptr)
	{
		OutReport = TEXT("no world");
		return false;
	}

	ATraceCore* Core = ATraceCore::Get(World);
	if (Core == nullptr)
	{
		OutReport = TEXT("no Core in this world");
		return false;
	}
	if (!Core->HasAuthority())
	{
		OutReport = TEXT("this machine is not the server; stage the turnover on the listen host");
		return false;
	}
	if (!Core->IsModeB())
	{
		OutReport = TEXT("this match is not in goals mode; spec v25 §2 is goals mode only");
		return false;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	ATraceCharacter* Local = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	if (!IsValid(Local) || Local->GetTeam() == ETraceTeam::None)
	{
		OutReport = TEXT("no local pawn with a team");
		return false;
	}

	// THE AIM IS NOT TOUCHED HERE, BY ANYTHING. The Core goes on the FLOOR in front of the player,
	// which is where a thrown Core comes to rest; whether the player's crosshair happens to be on it
	// is then a real question for the shipping CanPullNow, and this fixture has recorded it answering
	// "not hovering the Core" more than once. (The demo below aims at the Core afterwards, in its own
	// clearly-logged step, which is the one thing a mouse does.)
	//
	// GROUND, NOT A VIEW RAY. Two earlier versions of this got the geometry wrong in opposite
	// directions and both were caught by the shipping rules rather than by me:
	//   * offset-then-drop put the Core metres below the ray, outside the 4-degree aim cone;
	//   * the view ray itself hit scenery the CORE's own downward probe does not see, so the Core was
	//     left "134 uu clear" of anything, RESUMED FLIGHT, fell, re-landed, fired a second genuine
	//     turnover that restarted the window, and cancelled a pull that had reached 0.777 of its hold.
	// A point on the floor is the only one that is both real and still there a second later.
	const FVector Eye = Local->GetPawnViewLocation();
	const FRotator ViewRot = PC->GetControlRotation();
	const FVector Forward = FRotator(0.f, ViewRot.Yaw, 0.f).Vector();   // Yaw only: along the ground.
	const float Reach = (DistanceUU > 1.f) ? DistanceUU : 450.f;

	FCollisionQueryParams DropParams(SCENE_QUERY_STAT(TraceCoreIntegStageDrop), /*bTraceComplex=*/false);
	DropParams.AddIgnoredActor(Local);
	DropParams.AddIgnoredActor(Core);

	FVector Where;

	if (DistanceUU <= 0.f)
	{
		// AT MY FEET, deliberately: the arm where this player's own team is locked out wants the pickup
		// poll offered to them on every tick, so the refusal that follows is a measured refusal rather
		// than an absence of opportunity.
		Where = Local->GetActorLocation();

		FHitResult Floor;
		if (World->LineTraceSingleByChannel(Floor, Where, Where - FVector(0.0, 0.0, 500.0),
			ECC_WorldStatic, DropParams))
		{
			Where = Floor.ImpactPoint + FVector(0.0, 0.0, TraceModeBVisibleOrbRadius + 2.0);
		}
	}
	else
	{
		// Walk out along the ground, not along the view, and take the floor under that point. If the
		// player is facing a wall the sweep shortens until it finds open floor, so the Core never ends
		// up inside geometry the pull would then have no line of sight to.
		FVector Base = FVector(Local->GetActorLocation().X, Local->GetActorLocation().Y, Eye.Z) + Forward * Reach;

		FHitResult Blocked;
		if (World->LineTraceSingleByChannel(Blocked, Eye, Base, ECC_WorldStatic, DropParams))
		{
			Base = Blocked.ImpactPoint - Forward * (TraceModeBVisibleOrbRadius * 3.0);
		}

		FHitResult Floor;
		if (World->LineTraceSingleByChannel(Floor, Base + FVector(0.0, 0.0, 100.0),
			Base - FVector(0.0, 0.0, 4000.0), ECC_WorldStatic, DropParams))
		{
			Where = Floor.ImpactPoint + Floor.ImpactNormal * (TraceModeBVisibleOrbRadius + 2.0);
		}
		else
		{
			Where = Local->GetActorLocation() + Forward * Reach;
		}
	}

	const ETraceTeam LocalTeam = Local->GetTeam();
	const ETraceTeam DroppingTeam = bLockLocalTeam ? LocalTeam : TraceOpposingTeam(LocalTeam);

	if (!Core->DebugRegisterTurnover(DroppingTeam, Where))
	{
		OutReport = TEXT("DebugRegisterTurnover refused (already held, or not loose-able right now)");
		return false;
	}

	const TCHAR* Reason = nullptr;
	const bool bLocalMayPull = Core->CanPullNow(Local, &Reason);

	OutReport = FString::Printf(
		TEXT("staged at %s, %.0f uu down the local player's own view. %s dropped it and is locked out ")
		TEXT("for %.2fs; %s may pull. The local pawn (%s, %s) %s — CanPullNow says \"%s\"."),
		*Where.ToCompactString(), Reach,
		*TraceTeamName(DroppingTeam).ToString(), Core->GetTurnoverSecondsRemaining(),
		*TraceTeamName(TraceOpposingTeam(DroppingTeam)).ToString(),
		*GetNameSafe(Local), *TraceTeamName(LocalTeam).ToString(),
		bLocalMayPull ? TEXT("MAY PULL") : TEXT("may NOT pull"),
		(Reason != nullptr) ? Reason : TEXT("legal"));

	return true;
}

namespace TraceCoreIntegStage
{
	/**
	 * `Trace.Integ.StageTurnover [DistanceUU] [1 = lock MY team out]`
	 *
	 * The integrator's camera rig for spec v25 §2/§3. Default (0) puts the turnover on the OTHER team,
	 * so this machine's player is the one who may pull and the ring is on screen to be photographed;
	 * `1` locks THIS player's team out, which is the arm where the ring must NOT appear and standing
	 * on the Core must do nothing for five seconds.
	 */
	static void Cmd(const TArray<FString>& Args, UWorld* World)
	{
		const float Distance = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 450.f;
		const bool bLockLocal = (Args.Num() > 1) && (FCString::Atoi(*Args[1]) != 0);

		FString Report;
		const bool bOk = ATraceCore::DebugStageTurnoverAtLocalCrosshair(World, Distance, bLockLocal, Report);

		if (bOk)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[v25Integ] StageTurnover: %s"), *Report);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] StageTurnover REFUSED: %s"), *Report);
		}
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdReg(
		TEXT("Trace.Integ.StageTurnover"),
		TEXT("SPEC v25 INTEGRATION. Stage a turnover on the ground the local player is already looking ")
		TEXT("at, so the window can be photographed. Args: [DistanceUU=450] [LockMyTeam=0]. It moves ")
		TEXT("the CORE, never the crosshair, and goes through the shipping RegisterTurnover()."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Cmd));

	// ---------------------------------------------------------------------------------------------
	// `Trace.Integ.TurnoverDemo` — THE WHOLE OF SPEC v25 §2 AS ONE SCRIPTED, PHOTOGRAPHED SEQUENCE.
	//
	// The §2 red arm proves the RULES on bots; this proves the PLAYER'S EXPERIENCE of them, on the
	// human-controlled pawn, in one run, with a screenshot at each claim:
	//
	//   A  the opposing team drops it -> the Core STAYS where it landed, beam recoloured and larger
	//   B  a ring appears around it for this player, empty
	//   C  holding the pull FILLS the ring, off the server's own number
	//   D  past 0.3 s the Core travels to this player and they hold it
	//   E  this player's OWN team drops it -> no ring, and standing on it does nothing
	//   F  five seconds later the same touch, through the same poll, hands it over
	//
	// Every verb is the shipping one. Nothing here writes a pull progress, a lockout, or a pickup.
	// ---------------------------------------------------------------------------------------------
	static void Shot(const TCHAR* Which)
	{
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"),
			FString::Printf(TEXT("v25integ_%s_%s.png"), Which,
				*FDateTime::Now().ToString(TEXT("%H%M%S"))));

		// bShowUI = true: the beam is world geometry but the RING is Canvas HUD, and a UI-less capture
		// photographs an arena with no evidence in it.
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[v25Integ] shot %s -> %s"), Which, *Path);
	}

	static void Demo(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] TurnoverDemo: no world."));
			return;
		}

		const float Distance = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 450.f;

		TWeakObjectPtr<UWorld> WeakWorld(World);
		// Every phase boundary is a GAME-TIME deadline, not a wall-clock one, because this machine has
		// run these captures at 0.1 fps under contention and a wall clock would step past the 0.30 s
		// hold between two frames without ever drawing the filling ring it is here to photograph.
		TSharedRef<int32> Phase = MakeShared<int32>(0);
		TSharedRef<double> PhaseStart = MakeShared<double>(World->GetTimeSeconds());
		TSharedRef<bool> ShotFilling = MakeShared<bool>(false);
		TSharedRef<double> LockoutOpened = MakeShared<double>(0.0);
		TSharedRef<int32> BeamRetries = MakeShared<int32>(0);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakWorld, Distance, Phase, PhaseStart, ShotFilling, LockoutOpened, BeamRetries](float) -> bool
			{
				UWorld* Live = WeakWorld.Get();
				if (Live == nullptr)
				{
					return false;
				}

				ATraceCore* Core = ATraceCore::Get(Live);
				APlayerController* PC = Live->GetFirstPlayerController();
				ATraceCharacter* Me = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
				if (Core == nullptr || !IsValid(Me))
				{
					// Between pawns (the select screen, a respawn). WAIT, and hold the settle clock at
					// zero so the first phase's delay is measured from the first frame this player
					// actually exists — armed from -TraceExec, this ticker starts before they do.
					*PhaseStart = Live->GetTimeSeconds();
					return true;
				}

				const double Now = Live->GetTimeSeconds();
				const double InPhase = Now - *PhaseStart;
				const auto Advance = [&Phase, &PhaseStart, Now](int32 Next)
				{
					*Phase = Next;
					*PhaseStart = Now;
				};

				const TCHAR* Reason = nullptr;
				const float Progress = Core->GetPullProgressFor(Me);
				const bool bMayPull = Core->CanPullNow(Me, &Reason);

				switch (*Phase)
				{
				case 0:
				{
					// Three seconds of live pawn before anything is staged: a spawn still has the camera
					// settling, and a turnover placed down a view that is mid-blend is placed nowhere.
					if (InPhase < 3.0)
					{
						break;
					}

					FString Report;
					if (!ATraceCore::DebugStageTurnoverAtLocalCrosshair(Live, Distance, /*bLockLocalTeam=*/false, Report))
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] A REFUSED: %s"), *Report);
						return false;
					}
					UE_LOG(LogTraceGame, Display,
						TEXT("[v25Integ] A — THE OPPOSING TEAM DROPPED IT. %s Beam scale x%.2f for the window."),
						*Report, ATraceCore::GetTurnoverBeamScale());
					Shot(TEXT("A_turnover_beam"));
					Advance(1);
					break;
				}

				case 1:
					// One frame of settle, then photograph the EMPTY ring before any button is pressed:
					// a ring that is only ever seen full proves nothing about where its number came from.
					// 0.8 s, not one frame: a staged Core still has to touch down and be judged by the
					// shipping landing rule, and photographing the ring before that is over photographs
					// a state the player never sees.
					if (InPhase >= 0.8)
					{
						// *** THE RED HALF, TAKEN BEFORE THE GREEN ONE AND FROM THE SAME WINDOW. ***
						// The Core is on the floor and NOTHING has touched this player's aim, so their
						// crosshair is wherever they left it. Whatever CanPullNow answers here, it
						// answers on its own.
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] B(red) — turnover open, aim UNTOUCHED: server progress %.3f ")
							TEXT("(-1 = no hold), this player %s (\"%s\"), %.2fs left."),
							Progress, bMayPull ? TEXT("MAY pull") : TEXT("may NOT pull"),
							(Reason != nullptr) ? Reason : TEXT("legal"),
							Core->GetTurnoverSecondsRemaining());

						// THE ONE THING A MOUSE DOES, done explicitly and logged as such: point the
						// camera at the Core. Nothing else is faked — the aim cone, the line of sight,
						// the 0.30 s clock and the race stay the server's, and the line above is this
						// same player being refused a moment earlier for want of exactly this.
						const FVector ToCore = Core->GetLooseLocation() - Me->GetPawnViewLocation();
						const FRotator Before = PC->GetControlRotation();
						const FRotator After = ToCore.Rotation();
						PC->SetControlRotation(After);

						const TCHAR* AimedReason = nullptr;
						const bool bAimedMayPull = Core->CanPullNow(Me, &AimedReason);

						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] B(green) — aimed at the Core (pitch %.1f -> %.1f, yaw %.1f -> ")
							TEXT("%.1f): this player %s (\"%s\"). Progress is still %.3f — looking at it ")
							TEXT("does not start a pull; the button does."),
							Before.Pitch, After.Pitch, Before.Yaw, After.Yaw,
							bAimedMayPull ? TEXT("MAY pull") : TEXT("may NOT pull"),
							(AimedReason != nullptr) ? AimedReason : TEXT("legal"),
							Core->GetPullProgressFor(Me));

						Shot(TEXT("B_ring_empty"));
						Advance(2);
					}
					break;

				case 2:
					// THE REAL BUTTON, re-asserted every frame exactly as a held mouse button is. The
					// server runs its own hover test, its own line of sight and its own 0.30 s clock.
					Core->RequestPullInput(true, Me);

					if (!*ShotFilling && Progress > 0.15f && Progress < 0.95f)
					{
						*ShotFilling = true;
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] C — the ring is FILLING: SERVER progress %.3f of the %.2fs hold."),
							Progress, ATraceCore::GetPullHoldSeconds());
						Shot(TEXT("C_ring_filling"));
					}

					if (Core->GetHolder() == Me || Core->GetPullWinner() == Me)
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] D — THE PULL COMPLETED for %s after %.2fs. Delivery speed %.0f uu/s ")
							TEXT("(= ATraceCore::GetThrowSpeed(), not a number of its own). Holding now: %d"),
							*GetNameSafe(Me), InPhase, ATraceCore::GetThrowSpeed(),
							Core->GetHolder() == Me ? 1 : 0);
						Shot(TEXT("D_pull_won"));
						Core->RequestPullInput(false, Me);
						Advance(3);
					}
					else if (InPhase > 8.0)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[v25Integ] C/D INCONCLUSIVE after %.1fs: progress %.3f, CanPullNow says \"%s\". ")
							TEXT("The player was probably not looking at the staged point."),
							InPhase, Progress, (Reason != nullptr) ? Reason : TEXT("legal"));
						Core->RequestPullInput(false, Me);
						Advance(3);
					}
					break;

				case 3:
					if (InPhase >= 1.5)
					{
						// THE OTHER HALF OF THE TABLE. Staged AT THIS PLAYER'S FEET so the pickup poll is
						// offered to them on every single tick of the window — the refusal below is a
						// measured refusal, not an absence of opportunity.
						FString Report;
						if (!ATraceCore::DebugStageTurnoverAtLocalCrosshair(Live, /*DistanceUU=*/0.f,
							/*bLockLocalTeam=*/true, Report))
						{
							UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] E REFUSED: %s"), *Report);
							return false;
						}
						*LockoutOpened = Now;
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] E — MY OWN TEAM DROPPED IT, and the Core is at my feet. %s"), *Report);
						Shot(TEXT("E_locked_out"));
						Advance(4);
					}
					break;

				case 4:
				{
					const bool bHolding = (Core->GetHolder() == Me);
					const float Left = Core->GetTurnoverSecondsRemaining();

					if (bHolding)
					{
						const double Held = Now - *LockoutOpened;
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] F — the locked-out player TOOK IT at t+%.2fs (lockout was %.2fs). ")
							TEXT("%s"), Held, ATraceCore::GetTurnoverLockoutSeconds(),
							(Held + 0.35 >= static_cast<double>(ATraceCore::GetTurnoverLockoutSeconds()))
								? TEXT("That is AFTER the window, which is the rule.")
								: TEXT("*** THAT IS INSIDE THE WINDOW — THE LOCKOUT LEAKED. ***"));
						Shot(TEXT("F_after_lockout"));
						Advance(5);
						break;
					}

					// One line per half second for the whole window: standing on it, refused, by name.
					if (FMath::Fmod(static_cast<float>(Now - *LockoutOpened), 0.5f) < 0.05f)
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] E+%.2fs standing on the Core: holding=%d, ring=%s, %.2fs left."),
							Now - *LockoutOpened, bHolding ? 1 : 0,
							bMayPull ? TEXT("SHOWN") : TEXT("hidden (correct: locked out)"), Left);
					}

					if (Now - *LockoutOpened > static_cast<double>(ATraceCore::GetTurnoverLockoutSeconds()) + 6.0)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[v25Integ] F INCONCLUSIVE: %.1fs after the lockout expired the player still ")
							TEXT("has not taken it — they are probably not standing close enough to it."),
							Now - *LockoutOpened - static_cast<double>(ATraceCore::GetTurnoverLockoutSeconds()));
						Advance(5);
					}
					break;
				}

				case 5:
				{
					// ---- THE BEAM A/B, arm 1: the shipped multiplier -------------------------------
					//
					// "LARGER" AND "THE OTHER TEAM'S COLOUR" ARE BOTH CLAIMS ABOUT THE NORMAL BEAM, so
					// neither can be photographed once. Arm 1 is a turnover beam at the shipped
					// CoreTurnoverBeamScale; arm 2 is the SAME turnover, at the SAME staged point, with
					// that one multiplier forced to 1.0 and nothing else touched. The pair isolates the
					// multiplier itself, which a "wait for the window to close" pair could not: this arena
					// is full of bots who may legally take the Core the moment it opens, and four
					// consecutive attempts at that version were stolen before the window ended.
					//
					// MY OWN TEAM drops it here, so the beam is the OPPOSING colour — the mirror of arm A,
					// which photographed it in my own colour when the opposition dropped it.
					if (InPhase >= 1.0)
					{
						if (IConsoleVariable* BeamCVar = CVarModeBTurnoverBeamScale.AsVariable())
						{
							// SetByCode outranks the ini, which is how TraceModeBTuning::Resolve decides
							// who wins — so this really does move the shipped multiplier for these frames.
							BeamCVar->Set(2.2f, ECVF_SetByCode);
						}

						FString Report;
						if (!ATraceCore::DebugStageTurnoverAtLocalCrosshair(Live, Distance,
							/*bLockLocalTeam=*/true, Report))
						{
							UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] G REFUSED: %s"), *Report);
							return false;
						}

						const FVector ToCore = Core->GetLooseLocation() - Me->GetPawnViewLocation();
						PC->SetControlRotation(ToCore.Rotation());

						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] G — BEAM A/B arm 1 of 2: window OPEN, beam x%.2f, colour = the ")
							TEXT("team that did NOT drop it. %s"), ATraceCore::GetTurnoverBeamScale(), *Report);
						Advance(6);
					}
					break;
				}

				case 6:
					// The screenshot is deferred one phase so the beam has a frame to be rebuilt at the
					// new width before it is photographed.
					if (InPhase >= 0.3)
					{
						Shot(TEXT("G_beam_turnover_x2.2"));
						Advance(7);
					}
					break;

				case 7:
					// ---- Arm 2: the identical staging, the multiplier forced to 1.0 ----------------
					if (InPhase >= 1.0)
					{
						if (IConsoleVariable* BeamCVar = CVarModeBTurnoverBeamScale.AsVariable())
						{
							// SetByCode outranks the ini, which is how TraceModeBTuning::Resolve decides
							// who wins — so this really does move the shipped multiplier for these frames.
							BeamCVar->Set(1.0f, ECVF_SetByCode);
						}

						FString Report;
						if (!ATraceCore::DebugStageTurnoverAtLocalCrosshair(Live, Distance,
							/*bLockLocalTeam=*/true, Report))
						{
							UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] H REFUSED: %s"), *Report);
							return false;
						}

						const FVector ToCore = Core->GetLooseLocation() - Me->GetPawnViewLocation();
						PC->SetControlRotation(ToCore.Rotation());

						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] H — BEAM A/B arm 2 of 2: the same turnover at the same point with ")
							TEXT("CoreTurnoverBeamScale forced to x%.2f. One number changed between these two ")
							TEXT("frames."), ATraceCore::GetTurnoverBeamScale());
						Advance(8);
					}
					break;

				case 8:
					if (InPhase >= 0.3)
					{
						Shot(TEXT("H_beam_turnover_x1.0"));
						Advance(9);
					}
					break;

				case 9:
					// Put the shipped value back so nothing after this run reads a harness number, then
					// stop. (*BeamRetries is kept only so the capture ends on a named phase.)
					if (InPhase >= 1.0)
					{
						if (IConsoleVariable* BeamCVar = CVarModeBTurnoverBeamScale.AsVariable())
						{
							// SetByCode outranks the ini, which is how TraceModeBTuning::Resolve decides
							// who wins — so this really does move the shipped multiplier for these frames.
							BeamCVar->Set(2.2f, ECVF_SetByCode);
						}
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] DONE. CoreTurnoverBeamScale restored to x%.2f. (%d)"),
							ATraceCore::GetTurnoverBeamScale(), *BeamRetries);
						return false;
					}
					break;

				default:
					return false;
				}

				return true;
			}), 0.f);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdDemoReg(
		TEXT("Trace.Integ.TurnoverDemo"),
		TEXT("SPEC v25 INTEGRATION. Runs the whole §2 table on the LOCAL player and photographs each ")
		TEXT("claim: turnover registered and the Core stays, beam recoloured/larger, ring empty, ring ")
		TEXT("filling off the server number, pull completing at the thrown speed, then the same player ")
		TEXT("locked out of their own team's drop for the full window and taking it after. Args: ")
		TEXT("[DistanceUU=450]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Demo));
}
#endif // !UE_BUILD_SHIPPING

bool ATraceCore::DriveAimAtLooseCore(ATraceCharacter* Puller)
{
	if (!IsValid(Puller) || !bLoose)
	{
		return false;
	}

	AController* Controller = Puller->GetController();
	if (Controller == nullptr || Controller->IsPlayerController())
	{
		return false;
	}

	const FVector ToCore = FVector(LooseLocation) - Puller->GetPawnViewLocation();
	if (ToCore.IsNearlyZero())
	{
		return false;
	}

	Controller->SetControlRotation(ToCore.Rotation());
	return true;
}

void ATraceCore::TickTurnoverVerify()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	if (!bTurnoverVerifyArmed)
	{
		if (bTurnoverVerifyDone || CVarModeBTurnoverVerify.GetValueOnGameThread() == 0)
		{
			return;   // One bool and one int compare in the steady state.
		}

		// A LATCH, not a re-read of the CVar. -ExecCmds arms at console priority and a code-priority
		// Set(0) is silently dropped, which is how a previous harness in this file re-fired 48 times -
		// and how the first version of THIS one ran three times in forty seconds.
		bTurnoverVerifyArmed = true;
		bTurnoverVerifySawWinner = false;
		TurnoverVerifyStep = 0;
		TurnoverVerifyPassCount = 0;
		TurnoverVerifyFailCount = 0;
		TurnoverVerifySkipCount = 0;
		TurnoverVerifyRetriesLeft = TraceCoreTurnoverVerify::LockoutRetries;
	}

	const float Now = GetServerTimeSeconds();

	const auto Pass = [this](const TCHAR* What)
	{
		++TurnoverVerifyPassCount;
		UE_LOG(LogTraceGame, Display, TEXT("[v25Turnover] PASS - %s"), What);
	};
	const auto Fail = [this](const TCHAR* What)
	{
		++TurnoverVerifyFailCount;
		UE_LOG(LogTraceGame, Error, TEXT("[v25Turnover] *** FAIL *** - %s"), What);
	};
	const auto Skip = [this](const TCHAR* What)
	{
		++TurnoverVerifySkipCount;
		UE_LOG(LogTraceGame, Warning, TEXT("[v25Turnover] SKIP - %s"), What);
	};

	// A step that stops making progress is reported as inconclusive rather than left to hang a run.
	if (TurnoverVerifyStep > 0 && TurnoverVerifyDeadline > 0.f && Now > TurnoverVerifyDeadline)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[v25Turnover] step %d timed out after %.1fs - inconclusive, moving on."),
			TurnoverVerifyStep, TraceCoreTurnoverVerify::StepTimeoutSeconds);
		++TurnoverVerifySkipCount;

		// A timeout in steps 1-4 skips to the LOCKOUT step, which is independent of them and is worth
		// measuring on its own; a timeout in step 5 goes to the report, because sending it back to
		// itself with a deadline already in the past is an infinite loop rather than a retry.
		TurnoverVerifyStep = (TurnoverVerifyStep >= 5) ? 6 : 5;
		TurnoverVerifyMark = -1.f;
		TurnoverVerifyDeadline = 0.f;
	}

	ATraceCharacter* Puller = TurnoverVerifyPuller.Get();
	ATraceCharacter* Locked = TurnoverVerifyLocked.Get();

	switch (TurnoverVerifyStep)
	{
	case 0:
	{
		if (!IsModeB())
		{
			Skip(TEXT("this match is not in goals mode; spec v25 §2 is goals mode only."));
			TurnoverVerifyStep = 5;
			break;
		}

		TArray<ATraceCharacter*> All;
		GatherCharacters(All);

		ATraceCharacter* Blue = TraceCoreTurnoverVerify::FindDrivableBot(All, ETraceTeam::Blue);
		ATraceCharacter* Orange = TraceCoreTurnoverVerify::FindDrivableBot(All, ETraceTeam::Orange);

		if (Blue == nullptr || Orange == nullptr)
		{
			Skip(TEXT("no drivable bot on one of the teams; the harness will not move a human's crosshair."));
			TurnoverVerifyStep = 5;
			break;
		}

		// ORANGE DROPS IT, BLUE PULLS. The Core is parked at Orange's own feet, which is what makes
		// step 4 a real test of the pickup lockout rather than an argument about it: the locked-out
		// player is standing inside the pickup radius for the whole window.
		TurnoverVerifyPuller = Blue;
		TurnoverVerifyLocked = Orange;

		// Placed where BLUE can see it, not where Orange happens to be standing: steps 1-3 all turn on
		// the puller having line of sight, and step 5 - the only one that needs the Core at the
		// locked-out player's feet - re-arms it there itself.
		FVector Where = FVector::ZeroVector;
		if (!TraceCoreTurnoverVerify::FindPullablePoint(Blue, TraceModeBVisibleOrbRadius, Where))
		{
			Skip(TEXT("the puller has clear sight of nowhere nearby; the green arms could not fire."));
			TurnoverVerifyStep = 5;
			break;
		}

		if (!DebugRegisterTurnover(ETraceTeam::Orange, Where))
		{
			Skip(TEXT("could not arm a turnover (the Core is locked, or the mode changed under us)."));
			TurnoverVerifyStep = 5;
			break;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[v25Turnover] armed: Orange dropped it at %s, Blue may pull. Lockout %.2fs, hold %.2fs, ")
			TEXT("delivery speed %.0f uu/s (= the thrown speed)."),
			*Where.ToCompactString(), GetTurnoverLockoutSeconds(), GetPullHoldSeconds(), GetThrowSpeed());

		TurnoverVerifyStep = 1;
		TurnoverVerifyMark = Now;
		TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
		break;
	}

	case 1:
	{
		// --- RED ARM 1: A SAME-TEAM PLAYER MUST FAIL TO PULL. ------------------------------------
		//
		// Orange dropped it, so Orange is locked out - and the test is run on a player who is NOT the
		// thrower, because spec v25 puts the lockout on the TEAM. Their aim is driven onto the Core and
		// their button is pressed, so every other condition in CanPullNow is satisfied and the only
		// thing that can refuse them is the team rule.
		if (!IsValid(Locked) || !IsValid(Puller) || !IsTurnoverActive())
		{
			Skip(TEXT("step 1: the window or a pawn went away before it could be measured."));
			TurnoverVerifyStep = 4;
			break;
		}

		DriveAimAtLooseCore(Locked);
		RequestPullInput(true, Locked);

		const TCHAR* Reason = TEXT("(none)");
		const bool bAllowed = CanPullNow(Locked, &Reason);

		// One frame later the state machine has run, so PullHolds is the authoritative answer to
		// "did a fill actually start" - CanPullNow alone would only prove the query agrees with itself.
		if (Now - TurnoverVerifyMark >= 0.05f)
		{
			const bool bHasHold = GetPullProgressFor(Locked) >= 0.f;

			if (!bAllowed && !bHasHold)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[v25Turnover] red arm 1: %s (Orange, the side that dropped it) was refused - \"%s\", ")
					TEXT("and no fill started."),
					*GetNameSafe(Locked), Reason);
				Pass(TEXT("a same-team player cannot pull."));
			}
			else
			{
				Fail(TEXT("a player on the team that DROPPED the Core was allowed to pull it."));
			}

			RequestPullInput(false, Locked);

			// GREEN ARM: the same instant, the same Core, the opposing player. Without this the red arm
			// above would also pass on a build where nobody can pull at all.
			DriveAimAtLooseCore(Puller);
			const TCHAR* GreenReason = TEXT("(none)");
			if (CanPullNow(Puller, &GreenReason))
			{
				Pass(TEXT("the OPPOSING player, on the same frame, is allowed to pull (green arm)."));
			}
			else
			{
				Fail(TEXT("the opposing player could not pull either - the red arm above proves nothing."));
				UE_LOG(LogTraceGame, Error, TEXT("[v25Turnover]   refusal was: %s"), GreenReason);
			}

			TurnoverVerifyStep = 2;
			TurnoverVerifyMark = Now;
			TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
		}
		break;
	}

	case 2:
	{
		// --- RED ARM 2: NO LINE OF SIGHT MUST FAIL. ----------------------------------------------
		//
		// Measured by moving the Core, for one query and with no tick in between, to a point 5000 uu
		// BELOW where it is lying - so the ray from the puller's eye crosses the arena floor slab and
		// is genuinely blocked by real world geometry. The alternative was to wait for a bot to
		// happen to stand behind a crate, which is not a test, and to hope the crate was the reason.
		if (!IsValid(Puller) || !IsTurnoverActive())
		{
			Skip(TEXT("step 2: the window or the puller went away before it could be measured."));
			TurnoverVerifyStep = 4;
			break;
		}

		DriveAimAtLooseCore(Puller);

		const TCHAR* ClearReason = TEXT("(none)");
		const bool bClear = CanPullNow(Puller, &ClearReason);

		const FVector Restore = LooseLocation;
		LooseLocation = Restore - FVector(0.0, 0.0, 5000.0);

		const TCHAR* BlockedReason = TEXT("(none)");
		const bool bBlocked = !CanPullNow(Puller, &BlockedReason);

		LooseLocation = Restore;

		if (bBlocked && FCString::Strstr(BlockedReason, TEXT("line of sight")) != nullptr)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[v25Turnover] red arm 2: with the floor slab between them the pull was refused - \"%s\"."),
				BlockedReason);
			Pass(TEXT("a player with no line of sight cannot pull."));
		}
		else
		{
			Fail(TEXT("a pull was allowed through solid geometry."));
		}

		if (bClear)
		{
			Pass(TEXT("the same player with a clear view CAN pull (green arm)."));
		}
		else
		{
			Fail(TEXT("the puller had no clear view either - the red arm above proves nothing."));
			UE_LOG(LogTraceGame, Error, TEXT("[v25Turnover]   refusal was: %s"), ClearReason);
		}

		TurnoverVerifyStep = 3;
		TurnoverVerifyMark = -1.f;   // "not pressed yet"
		TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
		break;
	}

	case 3:
	{
		// --- RED ARM 3: RELEASING AT 0.29 s MUST FAIL, AND HOLDING PAST 0.30 s MUST WIN. ---------
		if (!IsValid(Puller) || !IsTurnoverActive())
		{
			Skip(TEXT("step 3: the window or the puller went away before it could be measured."));
			TurnoverVerifyStep = 4;
			break;
		}

		DriveAimAtLooseCore(Puller);

		const float Hold = GetPullHoldSeconds();

		if (TurnoverVerifyMark < 0.f)
		{
			RequestPullInput(true, Puller);
			TurnoverVerifyMark = Now;
			break;
		}

		const float Held = Now - TurnoverVerifyMark;

		// Released on the last tick that is still SHORT of the hold time. Two frames of margin rather
		// than one, because the completion is decided by ServerTickTurnover LATER in this same frame -
		// so a release computed against a threshold this tick could otherwise be beaten by a
		// completion the previous tick had already earned. The measured hold is printed, so a harness
		// that overshoots says so instead of passing quietly.
		const float Margin = 2.f * FMath::Max(0.001f, static_cast<float>(World->GetDeltaSeconds()));

		if (Held + Margin >= Hold)
		{
			RequestPullInput(false, Puller);

			const bool bNoWinner = (PullWinner == nullptr);
			const bool bNoHold = (GetPullProgressFor(Puller) < 0.f);

			if (Held < Hold && bNoWinner)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[v25Turnover] red arm 3: released after %.3fs of a %.3fs hold - no pull completed, ")
					TEXT("and the fill was CANCELLED (progress now %s)."),
					Held, Hold, bNoHold ? TEXT("gone") : TEXT("STILL RUNNING"));
				Pass(TEXT("a release short of the hold time does not pull, and does not pause."));
			}
			else if (Held >= Hold)
			{
				Skip(TEXT("step 3: the harness overshot the hold time; the release was not short. "
					"Re-run with a slower tick or a longer CorePullHoldSeconds."));
			}
			else
			{
				Fail(TEXT("a release short of the hold time still completed a pull."));
			}

			TurnoverVerifyStep = 4;
			TurnoverVerifyMark = -1.f;
			TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
		}
		break;
	}

	case 4:
	{
		// --- GREEN ARM 3: A FULL HOLD COMPLETES AND DELIVERS AT THE THROWN SPEED. ----------------
		if (!IsValid(Puller))
		{
			Skip(TEXT("step 4: the puller went away."));
			TurnoverVerifyStep = 5;
			break;
		}

		if (TurnoverVerifyMark < 0.f)
		{
			if (!IsTurnoverActive())
			{
				// The window ran out while steps 1-3 were being measured. Re-arm it rather than
				// reporting a failure of a rule that was never given a chance.
				FVector Where = FVector::ZeroVector;
				if (!TraceCoreTurnoverVerify::FindPullablePoint(Puller, TraceModeBVisibleOrbRadius, Where)
					|| !DebugRegisterTurnover(ETraceTeam::Orange, Where))
				{
					Skip(TEXT("step 4: could not re-arm the window."));
					TurnoverVerifyStep = 5;
					break;
				}
			}

			bTurnoverVerifySawWinner = false;
			DriveAimAtLooseCore(Puller);
			RequestPullInput(true, Puller);
			TurnoverVerifyMark = Now;
			break;
		}

		DriveAimAtLooseCore(Puller);

		if (IsValid(Carrier) && Carrier == Puller)
		{
			// bTurnoverVerifySawWinner is what stops this passing on a puller who simply WALKED OVER
			// the Core: the ordinary pickup poll would hand it to them too, and the step would then be
			// reporting the pickup radius rather than the pull.
			if (bTurnoverVerifySawWinner)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[v25Turnover] green arm 3: %s held past %.2fs, the Core travelled to them at the full ")
					TEXT("thrown speed (%.0f uu/s) and they now hold it."),
					*GetNameSafe(Puller), GetPullHoldSeconds(), GetThrowSpeed());
				Pass(TEXT("a completed pull delivers the Core to the puller."));
			}
			else
			{
				Skip(TEXT("step 4: the puller reached the Core on foot before the hold completed; "
					"the delivery itself was not observed."));
			}

			RequestPullInput(false, Puller);
			TurnoverVerifyStep = 5;
			TurnoverVerifyMark = -1.f;
			TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
			break;
		}

		if (PullWinner == Puller)
		{
			bTurnoverVerifySawWinner = true;

			// In flight. The one number worth asserting here is the speed, because it is the one spec
			// v25 names and the one a re-implementation would get wrong.
			const double Speed = FVector(LooseVelocity).Size();
			const double Expected = static_cast<double>(GetThrowSpeed());

			if (FMath::Abs(Speed - Expected) <= 1.0)
			{
				UE_LOG(LogTraceGame, Verbose,
					TEXT("[v25Turnover] delivery in flight at %.0f uu/s (thrown speed %.0f)."), Speed, Expected);
			}
			else
			{
				Fail(TEXT("the pulled Core is not travelling at the Core's thrown speed."));
				UE_LOG(LogTraceGame, Error,
					TEXT("[v25Turnover]   measured %.0f uu/s, ATraceCore::GetThrowSpeed() says %.0f."),
					Speed, Expected);
				TurnoverVerifyStep = 5;
			}
		}
		break;
	}

	case 5:
	{
		// --- RED ARM 4: THE LOCKED-OUT TEAM CANNOT PICK IT UP, AND THE LOCKOUT MUST EXPIRE. ------
		//
		// The Core is pinned to the locked-out player's feet for the whole window, so the pickup poll
		// is being offered them on every single tick. It must refuse for the length of the window and
		// take it within a tick or two of the window closing. Both halves come out of the SAME poll -
		// there is no second code path for "after the lockout", which is the point of row 3 of the
		// table being row 1.
		if (!IsValid(Locked))
		{
			Skip(TEXT("step 5: the locked-out player went away."));
			TurnoverVerifyStep = 6;
			break;
		}

		if (TurnoverVerifyMark < 0.f)
		{
			double FeetDrop = 88.0;
			if (const UCapsuleComponent* Capsule = Locked->GetCapsuleComponent())
			{
				FeetDrop = Capsule->GetScaledCapsuleHalfHeight();
			}

			const FVector Where = Locked->GetActorLocation()
				- FVector(0.0, 0.0, FeetDrop - TraceModeBVisibleOrbRadius);

			if (!DebugRegisterTurnover(ETraceTeam::Orange, Where))
			{
				Skip(TEXT("step 5: could not arm the lockout window."));
				TurnoverVerifyStep = 6;
				break;
			}

			TurnoverVerifyMark = Now;
			TurnoverVerifyDeadline = Now + GetTurnoverLockoutSeconds()
				+ TraceCoreTurnoverVerify::StepTimeoutSeconds;
			break;
		}

		// Pinned to their feet every frame, so a wandering bot cannot quietly turn this into a test of
		// nothing. The Core is at rest and the turnover is already latched, so no landing re-fires.
		if (bLoose)
		{
			double FeetDrop = 88.0;
			if (const UCapsuleComponent* Capsule = Locked->GetCapsuleComponent())
			{
				FeetDrop = Capsule->GetScaledCapsuleHalfHeight();
			}

			LooseLocation = Locked->GetActorLocation() - FVector(0.0, 0.0, FeetDrop - TraceModeBVisibleOrbRadius);
			LooseVelocity = FVector::ZeroVector;
			bLooseAtRest = true;
		}

		const float Elapsed = Now - TurnoverVerifyMark;

		if (IsValid(Carrier))
		{
			if (Carrier == Locked && IsTurnoverActive())
			{
				Fail(TEXT("a player on the LOCKED-OUT team picked the Core up during the window."));
				TurnoverVerifyStep = 6;
			}
			else if (Carrier == Locked)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[v25Turnover] red arm 4: %s (Orange, locked out) stood on the Core for the whole ")
					TEXT("%.2fs window and could not take it, then took it %.2fs after it expired."),
					*GetNameSafe(Locked), GetTurnoverLockoutSeconds(),
					Elapsed - GetTurnoverLockoutSeconds());
				Pass(TEXT("the lockout refuses the dropping team, and expires."));
				TurnoverVerifyStep = 6;
			}
			else
			{
				// Legal: an opposing player is allowed to take it at any point in the window. It just
				// means this step measured nothing, so re-arm rather than report a result it did not get.
				if (TurnoverVerifyRetriesLeft-- > 0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[v25Turnover] step 5: %s took the Core legally; re-arming (%d retries left)."),
						*GetNameSafe(Carrier), TurnoverVerifyRetriesLeft);
					TurnoverVerifyMark = -1.f;
				}
				else
				{
					Skip(TEXT("step 5: the Core kept being taken legally by the opposing team."));
					TurnoverVerifyStep = 6;
				}
			}
			break;
		}

		if (Elapsed > GetTurnoverLockoutSeconds() + 1.0f)
		{
			Fail(TEXT("the lockout expired but the player standing on the Core still did not get it."));
			TurnoverVerifyStep = 6;
		}
		break;
	}

	default:
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[v25Turnover] ===== SPEC v25 §2 RED ARM: %d passed, %d FAILED, %d skipped ====="),
			TurnoverVerifyPassCount, TurnoverVerifyFailCount, TurnoverVerifySkipCount);

		if (TurnoverVerifyFailCount > 0)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[v25Turnover] the turnover rules above did not hold. Trace.ModeB.TurnoverPull 0 ")
				TEXT("restores the pre-v25 behaviour if a comparison is wanted."));
		}

		bTurnoverVerifyArmed = false;
		bTurnoverVerifyDone = true;   // See the field: the CVar cannot be written back down.
		TurnoverVerifyStep = -1;
		TurnoverVerifyPuller = nullptr;
		TurnoverVerifyLocked = nullptr;
		break;
	}
	}
}

#endif // !UE_BUILD_SHIPPING
