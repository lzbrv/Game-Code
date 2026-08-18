// =================================================================================================
// TraceMovementAuditV16.cpp — spec v16 §5, "run a check on all movement mechanics".
//
// Nothing in this file is shipped gameplay. It is an INSTRUMENT: it drives the shipped movement kit
// and the shipped ability sets through their own public entry points and prints what actually
// happened, as a table, so "works as expected" becomes a number next to another number.
//
// WHY A NEW FILE RATHER THAN MORE PHASES IN TickMomentumMeasure.
//
// -TraceMoveMeasure, -TraceLedgeTest, -TraceWallJumpTest and -TraceDashPitchTest already cover the
// air model, the landing carry, the slide, the ledge and the wall jump, and they are long-running
// command-line harnesses that own the pawn for a minute at a time. What v16 §5 adds are the things
// none of them touch:
//
//   * the two DERIVED numbers the spec explicitly refuses to accept as a value check — the Ripple's
//     real velocity (Rocco AND a rider) and Chut's bash DISTANCE before and after;
//   * a same-ground A/B of the slide-jump's well-timed window, which -TraceMoveMeasure takes once
//     and only in the well-timed arm, so the ratio the spec quotes has never been measured;
//   * the carrier speed multiplier through a REAL Core pickup, not through a settings read;
//   * the ability-driven movement (Rocco's second jump, Mace's suspend and Spike pull, Oyster's
//     jar-jump), which live in Abilities/ and are not this component's business to tick.
//
// Bolting those onto TickMomentumMeasure would mean a 45-phase state machine where a failure in
// phase 3 silently starves phase 40, and would make the whole audit a command-line switch that
// cannot be re-run without relaunching the game. These are console commands: each one stages, runs,
// prints and stops.
//
// THE HONESTY RULES THIS FILE FOLLOWS, because this project has been burned by all three:
//
//   1. EVERY EXPECTED VALUE IS DERIVED AT RUNTIME from UTraceSettings / the live component, never
//      typed in as a literal. A harness that hardcodes 3300 passes forever after somebody retunes
//      DashSpeed to 3000 and forgets this file.
//   2. EVERY MEASUREMENT HAS A FIXTURE CHECK. A knock distance measured through a wall, a slide that
//      never latched, a ripple with no rider — each of those is reported INVALID, never as a number.
//      "PASS while striking a corpse" is a failure mode this project has actually shipped.
//   3. EVERY COMMAND HAS A RED ARM. Trace.Move.AuditV16.Red perturbs the live settings CDO by a
//      known amount and re-runs the same code path; the same comparisons must then FAIL. A harness
//      that cannot go red is not evidence.
//
// COMMANDS
//   Trace.Move.AuditV16            the core kit: walk, crouch, jump, dash, slide, slide-jump A/B,
//                                  carrier speed. Drives the local pawn.
//   Trace.Move.AuditV16.Ripple     Rocco's Ripple: his own velocity and a rider's.
//   Trace.Move.AuditV16.Bash       Chut's bash knock DISTANCE, measured at two knockback speeds in
//                                  one world, plus the speed that would give exactly +25%.
//   Trace.Move.AuditV16.Abilities  Rocco's second jump, Mace's suspend and Spike pull, Oyster's
//                                  jar-jump.
//   Trace.Move.AuditV16.Knobs      the derived-value table on its own, no pawn needed.
//   Trace.Move.AuditV16.Red        the falsification arm — see RunRedArm().
//
// The namespace is named after the file, not anonymous: two anonymous namespaces with a FindWorld()
// in them are one namespace with two definitions under the Windows unity build (MSVC C2084), and
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
// =================================================================================================

#include "CoreMinimal.h"

#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/UObjectGlobals.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityWorldSubsystem.h"
#include "Abilities/Characters/TraceAbilitySetChut.h"
#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Abilities/Characters/TraceAbilitySetOyster.h"
#include "Abilities/Characters/TraceAbilitySetRocco.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"

#if !UE_BUILD_SHIPPING

namespace TraceMovementAuditV16
{
	// =============================================================================================
	// Shared plumbing
	// =============================================================================================

	/** The one authoritative game world, or null. Same shape every harness in this project uses. */
	UWorld* AuthWorld()
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

	/**
	 * The world this audit should drive, authoritative or not.
	 *
	 * The core-kit audit deliberately accepts a CLIENT world: spec v16 §5 says in as many words that
	 * dash, slide-jump and wall jump are client-predicted and that a host-side measurement cannot see
	 * a prediction bug by definition. The Ripple, the bash and the ability audits DO need authority
	 * (they drive server-only ability paths) and use AuthWorld() instead, and say so when they refuse.
	 */
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
		return AuthWorld();
	}

	ATraceCharacter* LocalPawn(const UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		APlayerController* PC = World->GetFirstPlayerController();
		return (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	}

	UTraceCharacterMovementComponent* MovementOf(const ATraceCharacter* Who)
	{
		return (Who != nullptr) ? Who->GetTraceMovement() : nullptr;
	}

	float PlanarSpeedOf(const UTraceCharacterMovementComponent* Move)
	{
		return (Move != nullptr) ? static_cast<float>(FVector(Move->Velocity.X, Move->Velocity.Y, 0.f).Size()) : 0.f;
	}

	/**
	 * PASS/FAIL against a tolerance expressed as a FRACTION of the expected value.
	 *
	 * Fractional rather than absolute because the numbers in this audit span 0.08 s and 3300 uu/s and
	 * one absolute epsilon cannot be right for both. A measurement taken over discrete physics steps
	 * cannot land exactly on an analytic target — a dash sampled at 60 Hz reports its plateau, not its
	 * instantaneous peak — so the tolerance is the honest statement of what the instrument can see,
	 * and it is printed next to every verdict rather than hidden in here.
	 */
	const TCHAR* Verdict(const float Measured, const float Expected, const float Tolerance)
	{
		if (Expected == 0.f)
		{
			return FMath::IsNearlyZero(Measured, 0.01f) ? TEXT("PASS") : TEXT("FAIL");
		}
		return (FMath::Abs(Measured - Expected) <= FMath::Abs(Expected) * Tolerance) ? TEXT("PASS") : TEXT("FAIL");
	}

	/** One row of the report the spec asked for: mechanic, expected, measured, verdict. */
	void Row(const TCHAR* Mechanic, const TCHAR* Unit, const float Expected, const float Measured,
	         const float Tolerance, const TCHAR* Note)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 | %-34s | expected %10.3f %-6s | measured %10.3f | %-4s (+/-%.0f%%) | %s"),
			Mechanic, Expected, Unit, Measured, Verdict(Measured, Expected, Tolerance), Tolerance * 100.f, Note);
	}

	/** A row whose measurement could not be taken. Never a number — an INVALID, with the reason. */
	void RowInvalid(const TCHAR* Mechanic, const TCHAR* Why)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 | %-34s | INVALID — %s"), Mechanic, Why);
	}

	/**
	 * The perturbation the RED arm applies, as a fraction. 0 means "shipped values, GREEN arm".
	 *
	 * Lives in one place so the red arm cannot drift from the green one: every derived expectation
	 * below multiplies by (1 + RedBias()) and every measurement is taken against the unperturbed
	 * game, so a non-zero bias MUST turn the PASS rows into FAIL rows. If it does not, the row it
	 * failed to flip is not actually comparing anything.
	 */
	float GRedBias = 0.f;
	float RedBias() { return GRedBias; }
	float Biased(const float Expected) { return Expected * (1.f + GRedBias); }

	/**
	 * A direction with a clear capsule lane of @p NeededLength from @p Who, or false.
	 *
	 * WITHOUT THIS A DISTANCE MEASUREMENT IS OF THE ARENA, NOT THE MECHANIC. The first thing a pawn
	 * does in this map is find midfield cover; TickMomentumMeasure has a whole comment block about
	 * the same trap ("the vector turned about 50 degrees, which is SlideAlongSurface deflecting the
	 * pawn off midfield cover"), and the first run of THIS file measured a 3300 uu/s dash travelling
	 * 163 uu because the pawn was jammed against a bank with its velocity still on rails.
	 *
	 * Sweeps the pawn's own capsule, in the pawn's own collision channel: a wall is by definition a
	 * thing that blocks THIS capsule, and asking on ECC_Visibility has already cost this project a
	 * whole harness run (see TickWallJumpTest's lead trace).
	 */
	bool FindClearLaneFrom(UWorld* World, ATraceCharacter* Who, const float NeededLength, FVector& OutDirection)
	{
		const UCapsuleComponent* Capsule = (Who != nullptr) ? Who->GetCapsuleComponent() : nullptr;
		if (World == nullptr || Capsule == nullptr)
		{
			return false;
		}

		const FVector From = Who->GetActorLocation();
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceMovementAuditLane), false, Who);
		const FCollisionShape Shape = FCollisionShape::MakeCapsule(
			Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());

		for (int32 Step = 0; Step < 24; ++Step)
		{
			const FVector Candidate = FRotator(0.f, 15.f * static_cast<float>(Step), 0.f).Vector();
			FHitResult Hit;
			// Swept a little above the feet: a lane that is clear only at knee height is not clear for
			// a knocked or dashing pawn, both of which rise.
			const bool bBlocked = World->SweepSingleByChannel(Hit,
				From + FVector(0.f, 0.f, 40.f),
				From + FVector(0.f, 0.f, 40.f) + Candidate * NeededLength,
				FQuat::Identity, Capsule->GetCollisionObjectType(), Shape, Params);
			if (!bBlocked)
			{
				OutDirection = Candidate;
				return true;
			}
		}
		return false;
	}

	UTraceAbilityComponent* FirstHumanAbilityComponent(UWorld* World);

	/**
	 * Get the local human onto @p Wanted, TAKING THE CHARACTER OFF A BOT IF ONE IS SITTING ON IT.
	 *
	 * THIS IS WHY THE FIRST RUN OF THE BASH AUDIT NEVER STAGED. Spec v15 made bots pick UNIQUE
	 * characters after their humans, so in a filled match every one of the five is already held —
	 * and ServerSetCharacter then silently refuses the human, forever, with no error anywhere. The
	 * audit sat in its staging phase for 180 s and reported ABORTED, which is the right outcome for
	 * an unstaged fixture and a useless one for the person reading the report.
	 *
	 * So: find the holder, and if it is a bot, park it on None for a frame. Public API only, and the
	 * bot's own controller is free to re-pick afterwards.
	 *
	 * @return true once the human actually holds it (which may be a frame or two after the ask).
	 */
	bool AcquireCharacter(UWorld* World, UTraceAbilityComponent* Human, const ETraceCharacterId Wanted,
	                      FString& OutBlockedBy)
	{
		OutBlockedBy.Reset();
		if (Human == nullptr)
		{
			OutBlockedBy = TEXT("no human ability component");
			return false;
		}
		if (Human->GetCharacterId() == Wanted)
		{
			return true;
		}

		TArray<UTraceAbilityComponent*> Components;
		UTraceAbilityWorldSubsystem::GatherAllComponents(World, Components);
		for (UTraceAbilityComponent* Other : Components)
		{
			if (Other == nullptr || Other == Human || Other->GetCharacterId() != Wanted)
			{
				continue;
			}
			const APlayerState* TheirState = Other->GetOwningPlayerState();
			OutBlockedBy = (TheirState != nullptr) ? TheirState->GetPlayerName() : FString(TEXT("<no player state>"));
			if (Other->IsBot())
			{
				Other->ServerSetCharacter(ETraceCharacterId::None);
			}
		}

		Human->ServerSetCharacter(Wanted);
		return Human->GetCharacterId() == Wanted;
	}

	// =============================================================================================
	// The derived-value table (Trace.Move.AuditV16.Knobs)
	//
	// This is the "expected" column of the whole report, and it is computed rather than typed. It
	// also catches the failure this project hit in v15: UTraceSettings.h and Config/DefaultGame.ini
	// are two copies of every number and THE INI WINS, so a header-only edit ships nothing. Reading
	// the CDO (which is what UTraceSettings::Get() returns, ini applied) and comparing it against the
	// class's own compiled-in default is the only way to see that from inside the game.
	// =============================================================================================

	/**
	 * What Config/DefaultGame.ini says about a knob, if anything.
	 *
	 * THE C++ HEADER DEFAULT IS NOT RECOVERABLE AT RUNTIME, and a harness that pretends otherwise is
	 * worse than one that admits it: NewObject copies the CDO, and the CDO is precisely the object
	 * the ini was already applied to, so a "pristine instance" hands back the ini value and every row
	 * reads (agree) forever. The comparison that IS recoverable — and the one that matters on this
	 * project, where the ini WINS — is live-CDO against the ini text.
	 *
	 * A knob with no ini line is the interesting case: it is tuneable only by editing TraceSettings.h
	 * and rebuilding, which is exactly how a "we changed the number" pass ships nothing. Header-vs-ini
	 * drift itself has to be caught on disk, by diffing the two files; this says which knobs the ini
	 * is actually driving so that diff knows where to look.
	 */
	bool IniValueFor(const FName PropertyName, float& OutValue)
	{
		if (GConfig == nullptr)
		{
			return false;
		}
		return GConfig->GetFloat(TEXT("/Script/Trace.TraceSettings"), *PropertyName.ToString(), OutValue, GGameIni);
	}

	void ReportKnob(const TCHAR* Label, const FName PropertyName, const float LiveValue)
	{
		const bool bHasProperty = UTraceSettings::StaticClass()->FindPropertyByName(PropertyName) != nullptr;

		float IniValue = 0.f;
		const bool bInIni = IniValueFor(PropertyName, IniValue);
		const bool bAgree = bInIni && FMath::IsNearlyEqual(IniValue, LiveValue, 0.0005f);

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 KNOB %-34s live=%10.4f  ini=%10.4f  %s"),
			Label, LiveValue, bInIni ? IniValue : 0.f,
			!bHasProperty ? TEXT("*** NO SUCH UTraceSettings PROPERTY — this name is dead ***")
			              : (!bInIni ? TEXT("*** NOT IN DefaultGame.ini — header-only, cannot be tuned from the ini ***")
			                         : (bAgree ? TEXT("(ini drives it)")
			                                   : TEXT("*** LIVE VALUE DOES NOT MATCH THE INI — something wrote it at runtime ***"))));
	}

	void RunKnobTable()
	{
		const UTraceSettings& S = UTraceSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== spec v16 §5 derived-value table. Every 'expected' below is computed from "
			     "these, never typed in. ====="));

		ReportKnob(TEXT("WalkSpeed"),                     TEXT("WalkSpeed"),                     S.WalkSpeed);
		ReportKnob(TEXT("CarrierSpeedMultiplier"),        TEXT("CarrierSpeedMultiplier"),        S.CarrierSpeedMultiplier);
		ReportKnob(TEXT("DashSpeed"),                     TEXT("DashSpeed"),                     S.DashSpeed);
		ReportKnob(TEXT("DashDuration"),                  TEXT("DashDuration"),                  S.DashDuration);
		ReportKnob(TEXT("SlideJumpWindowSpeedBonus"),     TEXT("SlideJumpWindowSpeedBonus"),     S.SlideJumpWindowSpeedBonus);
		ReportKnob(TEXT("SlideJumpBonusScale"),           TEXT("SlideJumpBonusScale"),           S.SlideJumpBonusScale);
		// SPEC v26 §3. In this table for one specific reason: "Let me change this in project settings"
		// is a REQUIREMENT of the note, and this is the row that proves the ini key and the UPROPERTY
		// are the same name. A knob that is header-only ("NOT IN DefaultGame.ini") reads as tunable and
		// is not — DefaultGame.ini wins, so a designer's edit in Project Settings would be overwritten
		// by the shipped file on the next load and the setting would appear to do nothing.
		ReportKnob(TEXT("SlideJumpMomentumScale (v26 §3a)"),
		                                                  TEXT("SlideJumpMomentumScale"),         S.SlideJumpMomentumScale);
		ReportKnob(TEXT("SlideJumpChainCapBoosts (v26 §3b)"),
		                                                  TEXT("SlideJumpChainCapBoosts"),        static_cast<float>(S.SlideJumpChainCapBoosts));
		ReportKnob(TEXT("SlideJumpChainResetSpeedMult"),  TEXT("SlideJumpChainResetSpeedMultiplier"), S.SlideJumpChainResetSpeedMultiplier);
		ReportKnob(TEXT("WallJumpOutwardImpulse"),        TEXT("WallJumpOutwardImpulse"),        S.WallJumpOutwardImpulse);
		ReportKnob(TEXT("RoccoRippleDashSpeedMultiplier"),TEXT("RoccoRippleDashSpeedMultiplier"),S.RoccoRippleDashSpeedMultiplier);
		ReportKnob(TEXT("RoccoRippleRideSpeedMultiplier"),TEXT("RoccoRippleRideSpeedMultiplier"),S.RoccoRippleRideSpeedMultiplier);
		ReportKnob(TEXT("ChutBashKnockbackSpeed"),        TEXT("ChutBashKnockbackSpeed"),        S.ChutBashKnockbackSpeed);
		ReportKnob(TEXT("ChutBashUpBias"),                TEXT("ChutBashUpBias"),                S.ChutBashUpBias);
		ReportKnob(TEXT("RoccoSecondJumpZVelocity"),      TEXT("RoccoSecondJumpZVelocity"),      S.RoccoSecondJumpZVelocity);
		ReportKnob(TEXT("MaceSuspendLateralSpeedCap"),    TEXT("MaceSuspendLateralSpeedCap"),    S.MaceSuspendLateralSpeedCap);
		ReportKnob(TEXT("MaceSpikePullSpeedMultiplier"),  TEXT("MaceSpikePullSpeedMultiplier"),  S.MaceSpikePullSpeedMultiplier);
		ReportKnob(TEXT("OysterJarJumpZVelocity"),        TEXT("OysterJarJumpZVelocity"),        S.OysterJarJumpZVelocity);

		// --- the two DERIVED numbers §0 asks to be measured rather than read ----------------------
		const float RippleRide = S.DashSpeed * S.RoccoRippleRideSpeedMultiplier;
		const float RippleLength = (S.DashSpeed * S.RoccoRippleDashSpeedMultiplier)
		                         * (S.DashDuration * S.RoccoRippleDashDurationMultiplier);
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 DERIVED  dash reach = %.0f x %.3f = %.1f uu | ripple ride = %.0f x %.6f = %.1f uu/s "
			     "| ripple PATH LENGTH = %.1f uu (RoccoRippleDashSpeedMultiplier only sets the path's LENGTH, "
			     "not anybody's velocity — see the report)"),
			S.DashSpeed, S.DashDuration, S.DashSpeed * S.DashDuration,
			S.DashSpeed, S.RoccoRippleRideSpeedMultiplier, RippleRide, RippleLength);

		// The slide-jump reading. Printed both ways because "increase the bonus by 10%" is genuinely
		// ambiguous and spec v16 §0 asks for the choice to be said out loud rather than assumed.
		const float Base = S.SlideJumpWindowSpeedBonus;
		const float Scale = S.SlideJumpBonusScale;
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 DERIVED  slide-jump well-timed bonus: gainOnly=%d -> 1 + (%.4f - 1) x %.4f = %.6f "
			     "(the OTHER reading, base x scale, would be %.6f)"),
			S.bSlideJumpBonusScalesGainOnly ? 1 : 0, Base, Scale,
			1.f + (Base - 1.f) * Scale, Base * Scale);
	}

	// =============================================================================================
	// Trace.Move.AuditV16 — the core kit, driven on the LOCAL pawn (client or host)
	// =============================================================================================

	struct FCoreKitState
	{
		int32 Phase = 0;
		float PhaseTime = 0.f;
		double Deadline = 0.0;

		TWeakObjectPtr<ATraceCharacter> Pawn;

		FVector RunDirection = FVector::ForwardVector;
		FVector Home = FVector::ZeroVector;

		// --- collected measurements ---------------------------------------------------------------
		float WalkTop = 0.f;
		float CarrierTop = 0.f;
		bool  bCarrierStaged = false;

		float CrouchedCapsuleHalf = -1.f;
		float StandingCapsuleHalf = -1.f;
		float CrouchWalkTop = -1.f;

		float JumpLaunchZ = 0.f;
		float JumpApexGain = 0.f;
		float JumpStartZ = 0.f;
		float JumpPeakZ = 0.f;
		float JumpPlanarBefore = 0.f;
		float JumpPlanarAfter = 0.f;

		float WalkDisplacement = 0.f;
		FVector WalkStartLocation = FVector::ZeroVector;

		float DashPeak = 0.f;
		float DashStartTime = 0.f;
		FVector DashStartLocation = FVector::ZeroVector;
		FVector DashEndLocation = FVector::ZeroVector;
		FVector DashLane = FVector::ForwardVector;
		bool  bDashLaneClear = false;
		float DashReach = 0.f;
		float DashActiveSeconds = 0.f;

		float SlideEntryIn = 0.f;
		float SlideEntrySpeed = 0.f;

		// The slide-jump A/B. Two hops off the same ground, differing only in the window.
		float HopEarlySlideSpeed = 0.f;
		float HopEarlyAirborne = 0.f;
		bool  bHopEarlyWellTimed = false;
		bool  bHopEarlyValid = false;

		float HopTimedSlideSpeed = 0.f;
		float HopTimedAirborne = 0.f;
		bool  bHopTimedWellTimed = false;
		bool  bHopTimedValid = false;

		int32 CorrectionsAtStart = 0;
	};

	/**
	 * Put the pawn back on known-clear ground, at rest, ready for the next phase.
	 *
	 * NEVER TELEPORTS ON A CLIENT, and that is not a nicety. A client-side SetActorLocation is a
	 * position the server never authorised, so it answers with a correction the size of the jump —
	 * the first run of this audit teleported three times and logged three corrections of 5792, 6691
	 * and 9535 uu, then printed those in the PREDICTION row as if the game had produced them. A
	 * harness that manufactures the defect it is measuring is worse than no harness.
	 *
	 * On a client the pawn is simply brought to rest where it stands. Every phase after this picks its
	 * own direction (the dash sweeps for a clear lane; the slide phases run before they slide), so
	 * losing the fixed start point costs a little repeatability and buys an honest correction count.
	 */
	void ReturnHome(FCoreKitState& State)
	{
		ATraceCharacter* Pawn = State.Pawn.Get();
		UTraceCharacterMovementComponent* Move = MovementOf(Pawn);
		if (Pawn == nullptr || Move == nullptr)
		{
			return;
		}
		Move->SetWantsToSlide(false);
		Pawn->UnCrouch();
		Pawn->StopJumping();

		if (Pawn->HasAuthority())
		{
			Pawn->SetActorLocation(State.Home + FVector(0.f, 0.f, 40.f), false, nullptr, ETeleportType::TeleportPhysics);
			Move->Velocity = FVector::ZeroVector;
			Move->SetMovementMode(MOVE_Walking);
		}
		else
		{
			Move->StopMovementImmediately();
		}
	}

	void ReportCoreKit(const FCoreKitState& State)
	{
		const UTraceSettings& S = UTraceSettings::Get();
		ATraceCharacter* Pawn = State.Pawn.Get();
		UTraceCharacterMovementComponent* Move = MovementOf(Pawn);
		if (Move == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 core kit: the pawn went away before the report."));
			return;
		}

		const float Gravity = FMath::Abs(Move->GetGravityZ());
		const float JumpZ = Move->JumpZVelocity;

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== CORE KIT (netMode=%d role=%d — %s) ====="),
			static_cast<int32>(Move->GetNetMode()),
			(Pawn != nullptr) ? static_cast<int32>(Pawn->GetLocalRole()) : -1,
			(Move->GetNetMode() == NM_Client) ? TEXT("CLIENT: this is the predicted view")
			                                  : TEXT("HOST/STANDALONE: prediction bugs are invisible here"));

		// --- walk ---------------------------------------------------------------------------------
		Row(TEXT("WALK top speed"), TEXT("uu/s"), Biased(S.WalkSpeed), State.WalkTop, 0.02f,
			*FString::Printf(TEXT("held input, flat ground, 2.0 s; the pawn actually travelled %.0f uu — "
			                      "a velocity of 800 against a displacement of 0 is a pawn jammed in a wall"),
				State.WalkDisplacement));

		// --- crouch -------------------------------------------------------------------------------
		//
		// THE ENGINE CROUCH IS DELIBERATELY OFF IN THIS GAME, so the check is that it is off, not that
		// it shrinks a capsule. CanCrouchInCurrentState() is hardwired to false and the capsule never
		// resizes (see CanAttemptJump()'s comment in the component header); the crouch KEY means
		// "slide" on the ground and "fast-fall" in the air, and both of those have their own rows —
		// SLIDE below, and the fast-fall in -TraceMoveMeasure.
		//
		// A row asserting a crouched capsule would therefore be a row asserting a feature nobody
		// shipped, and would read INVALID forever. This asserts the actual contract instead.
		//
		// THE ONE ROW THE RED ARM CANNOT FLIP, and it is stated here rather than left for a reader to
		// notice: a boolean has no meaningful +12% and is deliberately not passed through Biased(), so
		// this line reads PASS in both arms. It is an identity check on a design decision, not a
		// measurement, and RunRedArm()'s "every row MUST read FAIL" is about the measured rows.
		Row(TEXT("CROUCH engine crouch disabled"), TEXT("bool"), 1.f,
			Move->CanCrouchInCurrentState() ? 0.f : 1.f, 0.f,
			*FString::Printf(TEXT("by design: capsule stays %.1f uu, crouch key = slide on the ground / "
			                      "fast-fall in the air"), State.StandingCapsuleHalf));

		// --- jump ---------------------------------------------------------------------------------
		//
		// LAUNCH Z IS RECONSTRUCTED FROM THE APEX, NOT READ OFF THE FIRST AIRBORNE FRAME. This ticker
		// runs once per rendered frame, and by the time it first sees !IsMovingOnGround() the physics
		// step has already taken one frame of gravity out of Velocity.Z — measured 605.9 against a
		// configured 640, i.e. exactly 1097.6 uu/s^2 x 31 ms. Reporting that as a FAIL would be the
		// harness accusing the jump of a bug the harness invented.
		//
		// v0 = sqrt(2 g h) is exact under constant gravity with no drag, which is precisely the
		// airborne model this kit runs (AirFriction is 0), and it uses the apex the same run already
		// measured. The raw sample is still printed, as a sample, in the note.
		const float ReconstructedLaunchZ = FMath::Sqrt(2.f * Gravity * FMath::Max(0.f, State.JumpApexGain));
		Row(TEXT("JUMP launch velocity Z"), TEXT("uu/s"), Biased(JumpZ), ReconstructedLaunchZ, 0.02f,
			*FString::Printf(TEXT("reconstructed from the %.1f uu apex; the first airborne frame already "
			                      "read %.1f, one frame of gravity down"),
				State.JumpApexGain, State.JumpLaunchZ));
		Row(TEXT("JUMP apex height"), TEXT("uu"),
			Biased((JumpZ * JumpZ) / (2.f * FMath::Max(1.f, Gravity))), State.JumpApexGain, 0.06f,
			*FString::Printf(TEXT("gravity %.0f uu/s^2 (scale %.2f)"), Gravity, Move->GravityScale));
		Row(TEXT("JUMP horizontal carry"), TEXT("x"), Biased(1.f),
			State.JumpPlanarBefore > 1.f ? State.JumpPlanarAfter / State.JumpPlanarBefore : 0.f, 0.02f,
			TEXT("run->jump must not touch planar velocity"));

		// --- dash ---------------------------------------------------------------------------------
		Row(TEXT("DASH speed"), TEXT("uu/s"), Biased(S.DashSpeed), State.DashPeak, 0.02f,
			TEXT("v16: 3000 -> 3300"));
		if (State.bDashLaneClear)
		{
			Row(TEXT("DASH reach"), TEXT("uu"), Biased(S.DashSpeed * S.DashDuration), State.DashReach, 0.10f,
				*FString::Printf(TEXT("over %.3f s of sampled dash (configured %.3f); lane %s proven clear; "
				                      "%s -> %s"),
					State.DashActiveSeconds, S.DashDuration, *State.DashLane.ToCompactString(),
					*State.DashStartLocation.ToCompactString(), *State.DashEndLocation.ToCompactString()));
		}
		else
		{
			RowInvalid(TEXT("DASH reach"),
				TEXT("no clear lane could be found for the dash — a dash into cover keeps its velocity on "
				     "rails while displacing nothing, and the number would be a measurement of the arena"));
		}

		// --- slide --------------------------------------------------------------------------------
		if (State.SlideEntryIn > 1.f)
		{
			Row(TEXT("SLIDE entry ratio"), TEXT("x"), Biased(S.SlideEntrySpeedMultiplier),
				State.SlideEntrySpeed / State.SlideEntryIn, 0.03f,
				TEXT("you keep what you brought in; nothing tops it up"));
		}
		else
		{
			RowInvalid(TEXT("SLIDE entry ratio"), TEXT("the slide never latched"));
		}

		// --- slide-jump: the A/B ------------------------------------------------------------------
		//
		// THE RATIO IS THE MEASUREMENT, not either hop on its own. Two hops off the same strip, the
		// same entry speed band, the same retention — differing only in whether the press landed
		// inside GetSlideJumpWindowSeconds(). Their ratio IS GetSlideJumpWindowSpeedBonus(), and no
		// single hop can show that.
		if (State.bHopEarlyValid && State.bHopTimedValid
			&& !State.bHopEarlyWellTimed && State.bHopTimedWellTimed
			&& State.HopEarlySlideSpeed > 1.f && State.HopTimedSlideSpeed > 1.f)
		{
			const float EarlyRatio = State.HopEarlyAirborne / State.HopEarlySlideSpeed;
			const float TimedRatio = State.HopTimedAirborne / State.HopTimedSlideSpeed;
			Row(TEXT("SLIDEJUMP untimed retention"), TEXT("x"), Biased(Move->GetSlideJumpHorizontalRetentionForAudit()),
				EarlyRatio, 0.05f, TEXT("hop OUTSIDE the well-timed window"));
			Row(TEXT("SLIDEJUMP well-timed bonus"), TEXT("x"),
				Biased(Move->GetSlideJumpWindowSpeedBonusForAudit()),
				(EarlyRatio > 0.f) ? TimedRatio / EarlyRatio : 0.f, 0.06f,
				*FString::Printf(TEXT("v16: 1.40625 -> 1.446875; window %.2f s; slideSpeeds %.0f vs %.0f uu/s"),
					Move->GetSlideJumpWindowSecondsForAudit(), State.HopEarlySlideSpeed, State.HopTimedSlideSpeed));
		}
		else
		{
			RowInvalid(TEXT("SLIDEJUMP well-timed bonus"),
				*FString::Printf(TEXT("the A/B did not stage (earlyValid=%d earlyTimed=%d timedValid=%d timedTimed=%d) "
				                      "— a ratio needs one hop in the window and one out of it"),
					State.bHopEarlyValid ? 1 : 0, State.bHopEarlyWellTimed ? 1 : 0,
					State.bHopTimedValid ? 1 : 0, State.bHopTimedWellTimed ? 1 : 0));
		}

		// --- carrier ------------------------------------------------------------------------------
		if (State.bCarrierStaged && State.WalkTop > 1.f)
		{
			Row(TEXT("CARRIER speed multiplier"), TEXT("x"), Biased(S.CarrierSpeedMultiplier),
				State.CarrierTop / State.WalkTop, 0.03f,
				*FString::Printf(TEXT("through a real ATraceCore::TryPickup; %.0f -> %.0f uu/s"),
					State.WalkTop, State.CarrierTop));
		}
		else
		{
			RowInvalid(TEXT("CARRIER speed multiplier"),
				TEXT("no Core could be picked up — needs a live match with a loose Core"));
		}

		// --- the prediction instrument ------------------------------------------------------------
		//
		// Not a PASS/FAIL row: corrections are not a mechanic, they are the evidence for or against
		// "this is predicted". Printed so a client run can be compared against a host run of the same
		// script, which is the only way the spec's client-side requirement means anything.
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 | %-34s | server corrections during this audit: %d (worst %.2f uu, mean %.2f uu). "
			     "On a listen host this is 0 by construction and proves nothing."),
			TEXT("PREDICTION (corrections)"),
			Move->GetCorrectionCountForAudit() - State.CorrectionsAtStart,
			Move->GetCorrectionWorstForAudit(), Move->GetCorrectionMeanForAudit());
	}

	void RunCoreKit()
	{
		UWorld* World = LocalGameWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 core kit: no game world."));
			return;
		}

		TSharedPtr<FCoreKitState> State = MakeShared<FCoreKitState>();
		State->Deadline = FPlatformTime::Seconds() + 180.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== core kit starting. Every expected value is derived from the live settings; "
			     "red bias currently %.3f. ====="), RedBias());

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float Delta) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 core kit ABORTED: the world went away."));
				return false;
			}
			if (FPlatformTime::Seconds() > State->Deadline)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("AUDITV16 core kit ABORTED at phase %d: ran out of time. Reporting what was measured."),
					State->Phase);
				ReportCoreKit(*State);
				return false;
			}

			ATraceCharacter* Pawn = State->Pawn.Get();
			if (Pawn == nullptr)
			{
				Pawn = LocalPawn(TickWorld);
				if (Pawn == nullptr || !Pawn->IsAlive())
				{
					return true;   // still joining / still spawning
				}
				State->Pawn = Pawn;
			}

			UTraceCharacterMovementComponent* Move = MovementOf(Pawn);
			if (Move == nullptr)
			{
				return true;
			}

			// A DEAD PAWN INVALIDATES EVERYTHING AFTER IT. This project has shipped a harness that
			// printed PASS while striking a corpse; the cheapest guard against repeating that is to
			// stop the moment the fixture stops being alive.
			if (!Pawn->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("AUDITV16 core kit ABORTED at phase %d: the pawn died mid-audit. Everything after "
					     "the last completed phase is INVALID, not zero."), State->Phase);
				ReportCoreKit(*State);
				return false;
			}

			State->PhaseTime += Delta;
			const float Planar = PlanarSpeedOf(Move);

			auto Advance = [&State](int32 Next)
			{
				State->Phase = Next;
				State->PhaseTime = 0.f;
			};

			switch (State->Phase)
			{
			// --- 0. stage -------------------------------------------------------------------------
			case 0:
			{
				if (!Move->IsMovingOnGround())
				{
					return true;   // wait for the floor
				}
				State->Home = Pawn->GetActorLocation();
				State->CorrectionsAtStart = Move->GetCorrectionCountForAudit();

				// Run toward the middle of the field. Spawns are in the endzones and a fixed world-axis
				// run walks into the back wall — TickMomentumMeasure learned this the hard way and the
				// same reasoning applies to every phase below.
				FVector Toward = -State->Home;
				Toward.Z = 0.f;
				State->RunDirection = Toward.Normalize() ? Toward : FVector::ForwardVector;

				if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
				{
					PC->SetControlRotation(State->RunDirection.Rotation());
				}

				State->StandingCapsuleHalf = (Pawn->GetCapsuleComponent() != nullptr)
					? Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : -1.f;

				UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 staged at %s, running %s"),
					*State->Home.ToCompactString(), *State->RunDirection.ToCompactString());
				Advance(1);
				break;
			}

			// --- 1. WALK --------------------------------------------------------------------------
			case 1:
				if (State->PhaseTime <= Delta)
				{
					State->WalkStartLocation = Pawn->GetActorLocation();
				}
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				if (State->PhaseTime > 0.8f && Move->IsMovingOnGround())
				{
					State->WalkTop = FMath::Max(State->WalkTop, Planar);
				}
				if (State->PhaseTime > 2.0f)
				{
					State->WalkDisplacement = static_cast<float>(
						FVector::Dist2D(State->WalkStartLocation, Pawn->GetActorLocation()));
					UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 WALK top %.1f uu/s, travelled %.0f uu"),
						State->WalkTop, State->WalkDisplacement);
					Advance(2);
				}
				break;

			// --- 2. CROUCH ------------------------------------------------------------------------
			//
			// The engine crouch, not the slide: hold Crouch() WITHOUT the slide intent, which is the
			// only way to see the capsule and MaxWalkSpeedCrouched on their own. The slide is measured
			// in its own phase and by -TraceMoveMeasure.
			case 2:
				Pawn->Crouch();
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				if (State->PhaseTime > 1.2f)
				{
					if (Pawn->GetCapsuleComponent() != nullptr && Move->IsCrouching())
					{
						State->CrouchedCapsuleHalf = Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
						State->CrouchWalkTop = FMath::Max(State->CrouchWalkTop, Planar);
					}
				}
				if (State->PhaseTime > 2.2f)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 CROUCH capsule %.1f uu (standing %.1f), speed %.1f uu/s, crouching=%d, "
						     "CanCrouchInCurrentState=%d"),
						State->CrouchedCapsuleHalf, State->StandingCapsuleHalf, State->CrouchWalkTop,
						Move->IsCrouching() ? 1 : 0, Move->CanCrouchInCurrentState() ? 1 : 0);
					Pawn->UnCrouch();
					Advance(3);
				}
				break;

			// --- 3. JUMP --------------------------------------------------------------------------
			case 3:
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				if (State->PhaseTime > 1.2f && Move->IsMovingOnGround())
				{
					State->JumpStartZ = Pawn->GetActorLocation().Z;
					State->JumpPeakZ = State->JumpStartZ;
					State->JumpPlanarBefore = Planar;
					Pawn->Jump();
					Advance(4);
				}
				break;

			case 4:
				if (!Move->IsMovingOnGround())
				{
					if (State->JumpLaunchZ == 0.f)
					{
						State->JumpLaunchZ = Move->Velocity.Z;
						State->JumpPlanarAfter = Planar;
					}
					State->JumpPeakZ = FMath::Max(State->JumpPeakZ, static_cast<float>(Pawn->GetActorLocation().Z));
					Pawn->StopJumping();
				}
				else if (State->JumpLaunchZ != 0.f)
				{
					State->JumpApexGain = State->JumpPeakZ - State->JumpStartZ;
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 JUMP launchZ %.1f uu/s, apex +%.1f uu, planar %.0f -> %.0f uu/s"),
						State->JumpLaunchZ, State->JumpApexGain, State->JumpPlanarBefore, State->JumpPlanarAfter);
					Advance(5);
				}
				else if (State->PhaseTime > 2.f)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 JUMP never left the ground."));
					Advance(5);
				}
				break;

			// --- 5. DASH --------------------------------------------------------------------------
			//
			// THE LANE IS CHOSEN, NOT ASSUMED. A dash is velocity on rails: ApplyDashVelocity re-asserts
			// 3300 uu/s every frame whether or not the capsule is moving, so a dash into a bank reports
			// a perfect speed and a displacement of nothing. The first run of this audit measured
			// exactly that — 3300 uu/s, 163 uu travelled — and the lane sweep is what turns that into
			// either a real reach or an honest INVALID.
			case 5:
			{
				const float LaneNeeded = FMath::Max(800.f, UTraceSettings::Get().DashSpeed
					* UTraceSettings::Get().DashDuration * 2.0f);

				// THE SWEEP AND THE DASH HAPPEN IN THE SAME CALL. The previous version swept a lane,
				// then kept running for another second before pressing dash — by which time the pawn
				// was 819 uu past the place the sweep was taken from, and it dashed 367 uu into
				// something the sweep had never looked at. A clearance test is about a position.
				if (!Move->IsMovingOnGround() || !Move->CanDash())
				{
					if (State->PhaseTime > 6.f)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 DASH could not start (charges=%d)."),
							Move->GetDashCharges());
						Advance(7);
					}
					break;
				}

				// Stand still for the sweep and the press. A dash from rest is the same dash — the
				// velocity is on rails for its whole window either way — and it removes the
				// entry-speed variable from the reach entirely.
				Move->StopMovementImmediately();

				State->bDashLaneClear = FindClearLaneFrom(TickWorld, Pawn, LaneNeeded, State->DashLane);
				if (!State->bDashLaneClear)
				{
					if (State->PhaseTime > 6.f)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("AUDITV16 DASH: no clear %.0f uu lane in any of 24 directions from %s."),
							LaneNeeded, *Pawn->GetActorLocation().ToCompactString());
						Advance(7);
					}
					// Shuffle toward midfield and try again next frame rather than dashing blind.
					FVector Toward = -Pawn->GetActorLocation();
					Toward.Z = 0.f;
					if (Pawn->HasAuthority() && Toward.Normalize())
					{
						Pawn->SetActorLocation(Pawn->GetActorLocation() + Toward * 900.f,
							false, nullptr, ETeleportType::TeleportPhysics);
					}
					break;
				}

				// Aim AND drive along the proven lane: ComputeDashDirection is a pure function of
				// (Acceleration, aim), so both have to agree or the dash leaves the lane that was swept.
				if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
				{
					PC->SetControlRotation(State->DashLane.Rotation());
				}
				Pawn->AddMovementInput(State->DashLane, 1.f);

				UE_LOG(LogTraceGame, Display,
					TEXT("AUDITV16 DASH lane %s clear for %.0f uu from %s"),
					*State->DashLane.ToCompactString(), LaneNeeded,
					*Pawn->GetActorLocation().ToCompactString());

				State->DashStartLocation = Pawn->GetActorLocation();
				State->DashStartTime = static_cast<float>(TickWorld->GetTimeSeconds());
				Move->StartDash();
				Advance(6);
				break;
			}

			case 6:
				if (Move->IsDashing())
				{
					State->DashPeak = FMath::Max(State->DashPeak, static_cast<float>(Move->Velocity.Size()));
				}
				else if (State->DashPeak > 0.f)
				{
					State->DashEndLocation = Pawn->GetActorLocation();
					State->DashReach = static_cast<float>(
						FVector::Dist(State->DashStartLocation, State->DashEndLocation));
					State->DashActiveSeconds = static_cast<float>(TickWorld->GetTimeSeconds()) - State->DashStartTime;
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 DASH peak %.1f uu/s, travelled %.1f uu in %.3f s"),
						State->DashPeak, State->DashReach, State->DashActiveSeconds);
					Advance(7);
				}
				else if (State->PhaseTime > 2.f)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 DASH never became active."));
					Advance(7);
				}
				break;

			// --- 7-9. SLIDE, then the untimed hop -------------------------------------------------
			case 7:
				ReturnHome(*State);
				Advance(8);
				break;

			case 8:
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				if (State->PhaseTime > 1.6f && Move->IsMovingOnGround() && Move->GetSlideCooldownRemaining() <= 0.f)
				{
					State->SlideEntryIn = Planar;
					Move->SetWantsToSlide(true);
					Advance(9);
				}
				break;

			case 9:
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				Move->SetWantsToSlide(true);
				if (Move->IsSliding())
				{
					if (State->SlideEntrySpeed == 0.f)
					{
						State->SlideEntrySpeed = Move->GetSlideSpeedForAudit();
						UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 SLIDE entry %.0f -> slideSpeed %.0f uu/s"),
							State->SlideEntryIn, State->SlideEntrySpeed);
					}

					// THE UNTIMED HOP. Jump IMMEDIATELY, while the slide is young: the well-timed
					// window is the last GetSlideJumpWindowSeconds() of the slide, so a press now is
					// outside it by construction. IsSlideJumpWellTimed() is recorded rather than
					// assumed, and the report refuses to compute the ratio if it disagrees.
					State->bHopEarlyWellTimed = Move->IsSlideJumpWellTimed();
					State->HopEarlySlideSpeed = Move->GetSlideSpeedForAudit();
					Pawn->Jump();
					Advance(10);
				}
				else if (State->PhaseTime > 2.f)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 SLIDE never latched (planar %.0f)."), Planar);
					Advance(12);
				}
				break;

			case 10:
				if (!Move->IsMovingOnGround())
				{
					State->HopEarlyAirborne = Planar;
					State->bHopEarlyValid = true;
					Pawn->StopJumping();
					Move->SetWantsToSlide(false);
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 SLIDEJUMP untimed: slideSpeed %.0f -> airborne %.0f uu/s (wellTimed=%d)"),
						State->HopEarlySlideSpeed, State->HopEarlyAirborne, State->bHopEarlyWellTimed ? 1 : 0);
					Advance(11);
				}
				else
				{
					Move->SetWantsToSlide(true);
					if (State->PhaseTime > 1.f)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 SLIDEJUMP untimed never left the ground."));
						Advance(11);
					}
				}
				break;

			// --- 11-14. the WELL-TIMED hop, same ground, same run ---------------------------------
			case 11:
				Move->SetWantsToSlide(false);
				if (State->PhaseTime > 1.4f && Move->IsMovingOnGround())
				{
					ReturnHome(*State);
					Advance(12);
				}
				break;

			case 12:
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				if (State->PhaseTime > 1.6f && Move->IsMovingOnGround() && Move->GetSlideCooldownRemaining() <= 0.f)
				{
					Move->SetWantsToSlide(true);
					Advance(13);
				}
				else if (State->PhaseTime > 6.f)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 SLIDEJUMP timed: second slide never became available."));
					Advance(15);
				}
				break;

			case 13:
				// Input HELD the whole way down. Without it ground friction stops the pawn dead once
				// the slide decays and the hop is measured from a standstill, which is a measurement
				// of the harness rather than of the mechanic (TickMomentumMeasure's phase 25 note).
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				Move->SetWantsToSlide(true);
				if (Move->IsSlideJumpWellTimed())
				{
					State->bHopTimedWellTimed = true;
					State->HopTimedSlideSpeed = Move->GetSlideSpeedForAudit();
					Pawn->Jump();
					Advance(14);
				}
				else if (!Move->IsSlideJumpAvailable() || State->PhaseTime > 4.f)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("AUDITV16 SLIDEJUMP timed: the window never opened (available=%d, %.2f s of slide left)."),
						Move->IsSlideJumpAvailable() ? 1 : 0, Move->GetSlideTimeLeftForAudit());
					Advance(15);
				}
				break;

			case 14:
				if (!Move->IsMovingOnGround())
				{
					State->HopTimedAirborne = Planar;
					State->bHopTimedValid = true;
					Pawn->StopJumping();
					Move->SetWantsToSlide(false);
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 SLIDEJUMP well-timed: slideSpeed %.0f -> airborne %.0f uu/s"),
						State->HopTimedSlideSpeed, State->HopTimedAirborne);
					Advance(15);
				}
				else
				{
					Move->SetWantsToSlide(true);
					if (State->PhaseTime > 1.f)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 SLIDEJUMP timed never left the ground."));
						Advance(15);
					}
				}
				break;

			// --- 15-17. CARRIER SPEED, through a real pickup --------------------------------------
			case 15:
				Move->SetWantsToSlide(false);
				if (State->PhaseTime > 1.0f && Move->IsMovingOnGround())
				{
					ReturnHome(*State);

					// THE REAL PATH. ATraceCore::TryPickup is what the pickup sphere calls, so the
					// Core actually attaches, bIsCarrier actually replicates and GetMaxSpeed's carrier
					// branch is reached the way a match reaches it. Setting a flag would test nothing.
					//
					// AUTHORITY ONLY: on a client the Core is server-owned and this row is skipped
					// rather than faked. The carrier multiplier is not client-predicted state that a
					// client-side measurement would say anything new about.
					if (ATraceCore* CoreActor = ATraceCore::Get(TickWorld))
					{
						// IN A LIVE MATCH SOMEBODY IS ALREADY CARRYING IT. The first run of this audit
						// only picked up a LOOSE Core and reported INVALID for the whole carrier row,
						// which in a filled arena is almost always. DropAt is the shipped drop path, so
						// taking it off the current holder first is a real turnover, not a poke at state.
						if (Pawn->HasAuthority())
						{
							if (ATraceCharacter* Holder = CoreActor->GetHolder())
							{
								if (Holder != Pawn)
								{
									CoreActor->DropAt(State->Home + FVector(0.f, 0.f, 60.f), FVector::ZeroVector);
								}
							}
							if (CoreActor->GetHolder() == nullptr)
							{
								CoreActor->TryPickup(Pawn);
							}
						}
					}
					Advance(16);
				}
				break;

			case 16:
				// Give the pickup a moment to attach and the carrier bit a moment to settle before
				// the run that measures it.
				if (State->PhaseTime > 0.5f)
				{
					State->bCarrierStaged = Pawn->IsCarrier();
					if (!State->bCarrierStaged)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("AUDITV16 CARRIER: pickup did not take (authority=%d). The row will be INVALID."),
							Pawn->HasAuthority() ? 1 : 0);
						Advance(18);
					}
					else
					{
						Advance(17);
					}
				}
				break;

			case 17:
			{
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				if (State->PhaseTime > 0.8f && Move->IsMovingOnGround() && Pawn->IsCarrier())
				{
					State->CarrierTop = FMath::Max(State->CarrierTop, Planar);
				}
				if (State->PhaseTime > 2.4f)
				{
					// RE-ASSERT THE FIXTURE. A Core dropped or stolen mid-run would leave a carrier
					// number that is really a walk number, and the ratio would quietly read 1.00.
					State->bCarrierStaged = State->bCarrierStaged && Pawn->IsCarrier();
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 CARRIER top %.1f uu/s (still carrying at the end=%d)"),
						State->CarrierTop, Pawn->IsCarrier() ? 1 : 0);
					Advance(18);
				}
				break;
			}

			case 18:
				ReportCoreKit(*State);
				return false;

			default:
				return false;
			}

			return true;
		}), 0.f);
	}

	// =============================================================================================
	// Trace.Move.AuditV16.Ripple — spec v16 §0 item 1
	//
	// "Measure the real ripple velocity, for Rocco AND for a rider, and confirm ~2100 uu/s."
	//
	// The ripple is authority-driven: ATraceRippleActor writes Move->Velocity on the server for every
	// pawn standing in its entry ring, INCLUDING Rocco, who is standing at the ring's own centre when
	// he lays it. So both numbers come from the same field, and the harness's only job is to put a
	// second pawn in the ring and then sample.
	// =============================================================================================

	struct FRippleState
	{
		int32 Phase = -1;
		double Deadline = 0.0;
		double PhaseStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Rocco;
		TWeakObjectPtr<ATraceCharacter> RoccoPawn;
		TWeakObjectPtr<ATraceCharacter> Rider;

		float RoccoPeak = 0.f;
		float RiderPeak = 0.f;
		bool  bRiderStaged = false;

		/** Rider's distance from the ripple's start ring on the frame the ability fired. */
		float RiderEntryDistance = -1.f;

		/** See FBashState::FrozenController — a bot walks out of a 140 uu ring in 0.18 s. */
		TWeakObjectPtr<AController> FrozenRider;

		FString AcquireBlockedBy;
	};

	UTraceAbilityComponent* FirstHumanAbilityComponent(UWorld* World)
	{
		TArray<UTraceAbilityComponent*> Components;
		UTraceAbilityWorldSubsystem::GatherAllComponents(World, Components);
		for (UTraceAbilityComponent* Comp : Components)
		{
			if (Comp != nullptr && !Comp->IsBot())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	void RunRipple()
	{
		UWorld* World = AuthWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("AUDITV16 ripple: server only — the ripple actor is authority-driven."));
			return;
		}

		TSharedPtr<FRippleState> State = MakeShared<FRippleState>();
		State->Deadline = FPlatformTime::Seconds() + 120.0;

		const UTraceSettings& S = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== RIPPLE. Expected ride speed = DashSpeed %.0f x RoccoRippleRideSpeedMultiplier "
			     "%.6f = %.1f uu/s, for Rocco and for a rider alike. ====="),
			S.DashSpeed, S.RoccoRippleRideSpeedMultiplier, S.DashSpeed * S.RoccoRippleRideSpeedMultiplier);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr || FPlatformTime::Seconds() > State->Deadline)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 ripple ABORTED at phase %d."), State->Phase);
				return false;
			}

			const UTraceSettings& Settings = UTraceSettings::Get();
			const float Expected = Settings.DashSpeed * Settings.RoccoRippleRideSpeedMultiplier;

			// --- stage --------------------------------------------------------------------------
			if (State->Phase < 0)
			{
				UTraceAbilityComponent* Human = FirstHumanAbilityComponent(TickWorld);
				if (Human == nullptr)
				{
					return true;
				}
				if (!AcquireCharacter(TickWorld, Human, ETraceCharacterId::Rocco, State->AcquireBlockedBy))
				{
					return true;   // a frame or two for the swap (and for a bot to be moved off it)
				}

				ATraceCharacter* Pawn = Human->GetOwningCharacter();
				if (Pawn == nullptr || !Pawn->IsAlive())
				{
					return true;
				}

				// A SECOND PAWN IN THE RING. Any living character will do — the entry test is the
				// Beneficial choke point, which has no team clause on purpose, so a bot from either
				// side is a legitimate rider and is the cheapest fixture available.
				ATraceCharacter* Rider = nullptr;
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate != nullptr && Candidate != Pawn && Candidate->IsAlive())
					{
						Rider = Candidate;
						break;
					}
				}
				if (Rider == nullptr)
				{
					return true;
				}

				// FREEZE THE RIDER BEFORE PLACING IT. The first run of this audit put a bot 56 uu from
				// Rocco inside a 140 uu entry ring, waited a quarter of a second for the wish direction
				// to settle, and by then the bot had walked 200 uu — it was never picked up, and the
				// row reported 1064 uu/s, which was the bot's own running speed. The harness called that
				// a FAIL of the mechanic. It was a failure of the fixture.
				if (AController* RiderController = Rider->GetController())
				{
					RiderController->StopMovement();
					RiderController->SetIgnoreMoveInput(true);
					State->FrozenRider = RiderController;
				}

				// Stand the rider inside the entry radius, level with Rocco. A ripple is laid at
				// Rocco's own location, so "inside the ring" is "next to Rocco".
				const FVector Beside = Pawn->GetActorLocation()
					+ Pawn->GetActorRightVector() * FMath::Min(60.f, Settings.RoccoRippleEntryRadiusUU * 0.4f);
				Rider->SetActorLocation(Beside, false, nullptr, ETeleportType::TeleportPhysics);

				State->Rocco = Human;
				State->RoccoPawn = Pawn;
				State->Rider = Rider;
				State->bRiderStaged = true;

				// Aim flat and hold a forward wish so ComputeDashDirection has an axis to work from;
				// with no input at all the ripple falls back to the capsule's facing, which is still a
				// valid path but makes the log harder to read.
				if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
				{
					PC->SetControlRotation(FRotator(0.f, Pawn->GetActorRotation().Yaw, 0.f));
				}
				Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 1.f);

				Human->DebugSetActivatedCooldown(0.f);
				State->Phase = 0;
				State->PhaseStartReal = FPlatformTime::Seconds();
				UE_LOG(LogTraceGame, Display,
					TEXT("AUDITV16 ripple staged: Rocco=%s rider=%s (%.0f uu apart, entry radius %.0f uu)"),
					*GetNameSafe(Pawn), *GetNameSafe(Rider),
					static_cast<float>(FVector::Dist(Pawn->GetActorLocation(), Rider->GetActorLocation())),
					Settings.RoccoRippleEntryRadiusUU);
				return true;
			}

			ATraceCharacter* Pawn = State->RoccoPawn.Get();
			ATraceCharacter* Rider = State->Rider.Get();
			UTraceAbilityComponent* Comp = State->Rocco.Get();
			if (Pawn == nullptr || Rider == nullptr || Comp == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 ripple ABORTED: a fixture pawn went away."));
				return false;
			}

			// --- fire ---------------------------------------------------------------------------
			if (State->Phase == 0)
			{
				Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 1.f);
				if (FPlatformTime::Seconds() - State->PhaseStartReal < 0.25)
				{
					return true;   // let the wish direction settle into Acceleration
				}
				// RE-PLACE ON THE FIRING FRAME, not at staging: the ring is laid where Rocco is standing
				// NOW, and anything measured about a rider is a claim about who was inside it at that
				// instant. The distance is recorded so the report can say whether the fixture held.
				const FVector BesideNow = Pawn->GetActorLocation()
					+ Pawn->GetActorRightVector()
						* FMath::Min(60.f, UTraceSettings::Get().RoccoRippleEntryRadiusUU * 0.4f);
				Rider->SetActorLocation(BesideNow, false, nullptr, ETeleportType::TeleportPhysics);

				if (!Comp->TryActivate())
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("AUDITV16 ripple: TryActivate refused (cooldown %.1f s left). INVALID."),
						Comp->GetActivatedCooldownRemaining());
					if (AController* Frozen = State->FrozenRider.Get()) { Frozen->ResetIgnoreMoveInput(); }
					return false;
				}
				State->RiderEntryDistance = static_cast<float>(
					FVector::Dist(Pawn->GetActorLocation(), Rider->GetActorLocation()));
				State->Phase = 1;
				State->PhaseStartReal = FPlatformTime::Seconds();
				return true;
			}

			// --- sample -------------------------------------------------------------------------
			if (State->Phase == 1)
			{
				const float RoccoSpeed = PlanarSpeedOf(MovementOf(Pawn));
				const float RiderSpeed = PlanarSpeedOf(MovementOf(Rider));
				State->RoccoPeak = FMath::Max(State->RoccoPeak, RoccoSpeed);
				State->RiderPeak = FMath::Max(State->RiderPeak, RiderSpeed);

				if (FPlatformTime::Seconds() - State->PhaseStartReal < 1.0)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 ===== RIPPLE RESULT ====="));
				UE_LOG(LogTraceGame, Display,
					TEXT("AUDITV16 RIPPLE fixture: rider was %.1f uu from the start ring when the ability "
					     "fired (entry radius %.0f uu), move input frozen=%d. Outside that radius the rider "
					     "row measures the rider's own legs, not the ripple."),
					State->RiderEntryDistance, UTraceSettings::Get().RoccoRippleEntryRadiusUU,
					State->FrozenRider.IsValid() ? 1 : 0);
				if (AController* Frozen = State->FrozenRider.Get())
				{
					Frozen->ResetIgnoreMoveInput();
				}
				if (State->RoccoPeak > 1.f)
				{
					Row(TEXT("RIPPLE velocity (Rocco)"), TEXT("uu/s"), Biased(Expected), State->RoccoPeak, 0.03f,
						TEXT("he stands in his own entry ring and rides it like anybody else"));
				}
				else
				{
					RowInvalid(TEXT("RIPPLE velocity (Rocco)"), TEXT("Rocco never moved — the path was never laid"));
				}
				if (State->RiderPeak > 1.f)
				{
					Row(TEXT("RIPPLE velocity (rider)"), TEXT("uu/s"), Biased(Expected), State->RiderPeak, 0.03f,
						TEXT("a second pawn placed inside the entry radius"));
				}
				else
				{
					RowInvalid(TEXT("RIPPLE velocity (rider)"),
						TEXT("the rider never moved — it was outside the entry ring, or the choke point refused it"));
				}
				return false;
			}

			return false;
		}), 0.f);
	}

	// =============================================================================================
	// Trace.Move.AuditV16.Bash — spec v16 §0 item 2
	//
	// "The doc asks for +25% DISTANCE, but the knob is a SPEED. Measure the actual knock DISTANCE
	// before and after and report both."
	//
	// THE SHAPE OF THE ANSWER IS THE POINT. A bash launches the victim at Speed horizontally and
	// Speed x ChutBashUpBias vertically. With no air friction in this kit, the airborne leg alone is
	//
	//     d_air = Speed x (2 x Speed x UpBias / g) = (2 x UpBias / g) x Speed^2
	//
	// i.e. QUADRATIC in the knob. A +25% speed cannot give +25% distance unless the ground bleed after
	// touchdown happens to cancel the square exactly, which nothing arranges. So this measures rather
	// than argues, at both knob values, in ONE world and ONE binary.
	//
	// Two arms, and both arms are the SHIPPED apply path: UTraceAbilitySetChut::TryBash, whose own
	// header says "so does the harness — so what is verified is what ships".
	// =============================================================================================

	struct FBashSample
	{
		float Speed = 0.f;
		float Distance = 0.f;
		float AirDistance = 0.f;
		float Airtime = 0.f;
		bool  bValid = false;
		bool  bDeflected = false;

		/**
		 * Deflection is tracked SEPARATELY for the flight and for the whole knock, because they fail
		 * differently. The airborne leg is a clean ballistic arc and any turn at all means the pawn
		 * clipped something; the ground slide afterwards can legitimately scuff along a surface. So a
		 * run whose flight was clean still yields one usable number even when the slide ends in cover.
		 */
		bool  bAirValid = false;
		bool  bAirDeflected = false;
	};

	struct FBashState
	{
		// 0 = the pre-v16 1000, 1 = whatever ChutBashKnockbackSpeed currently ships (read live into
		// OriginalKnockbackSpeed below, never a literal — this pass moved it 1250 -> 1118 once the
		// first run showed +25% speed buying +65.8% distance, and arm 1 followed it without an edit).
		//
		// READ THE AIRBORNE-LEG RESULT, NOT THE TOTAL, when using this to pick a value. Two runs at
		// the same 1000 uu/s agreed on the air leg to 11% and disagreed on the TOTAL by 75% (876 vs
		// 1530 uu), because the ground slide after touchdown belongs to the terrain the bash happened
		// on. Its fitted exponent swung 2.267 -> 1.523 across those runs, so the total will recommend
		// a different knob value every time it is asked. The air leg is exactly quadratic and did not
		// move (1.97, then 1.89).
		int32 Arm = 0;
		int32 Phase = -1;
		double Deadline = 0.0;
		double PhaseStartReal = 0.0;

		float OriginalKnockbackSpeed = 0.f;

		TWeakObjectPtr<UTraceAbilityComponent> Chut;
		TWeakObjectPtr<ATraceCharacter> ChutPawn;
		TWeakObjectPtr<ATraceCharacter> Victim;

		FVector KnockDirection = FVector::ForwardVector;
		FVector VictimStart = FVector::ZeroVector;
		FVector VictimLanded = FVector::ZeroVector;
		bool  bLanded = false;
		float LaunchRealTime = 0.f;

		FBashSample Samples[2];

		/**
		 * THE VICTIM IS A BOT, AND A BOT WALKS.
		 *
		 * ATraceBotController drives its pawn with ATraceCharacter::AddMovementInput every tick, so a
		 * knocked bot spends the whole flight and the whole ground slide steering itself, and "the
		 * distance the bash knocked them" quietly becomes "that, plus wherever they were going anyway".
		 *
		 * SetIgnoreMoveInput ALONE IS NOT ENOUGH, and the first run of this audit is the proof: the
		 * victim's acceleration during the flight was 4096 uu/s^2, i.e. MaxAcceleration, i.e. full
		 * throttle, with the ignore flag set. So the controller's TICK is disabled instead — that is
		 * the function issuing the input, the jumps and the ability presses, and stopping it stops all
		 * three. The ignore flag is kept as well; between them nothing the bot does reaches the pawn.
		 *
		 * Both are RESTORED at the end. Leaving a bot deaf and frozen for the rest of the session
		 * would be a harness that breaks the game it just measured.
		 */
		TWeakObjectPtr<AController> FrozenController;

		/** Worst planar acceleration seen on the victim mid-flight. Non-zero means the freeze leaked. */
		float VictimAccelWorst = 0.f;

		/** How many times the fixture had to be walked toward midfield to find a long enough lane. */
		int32 LaneAttempts = 0;
	};

	void ThawVictim(FBashState& State)
	{
		if (AController* Frozen = State.FrozenController.Get())
		{
			Frozen->ResetIgnoreMoveInput();
			Frozen->SetActorTickEnabled(true);
		}
		State.FrozenController = nullptr;
	}

	void SetKnockbackSpeed(const float NewValue)
	{
		// The Project Settings panel's own path (GetMutableDefault), which is what Trace.LiveEditTest
		// uses, because every read in the game goes through UTraceSettings::Get() at the point of use.
		// RESTORED at the end of the run — a harness that leaves the settings perturbed poisons every
		// measurement taken after it in the same session.
		if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
		{
			Mutable->ChutBashKnockbackSpeed = NewValue;
		}
	}

	void ReportBash(const FBashState& State)
	{
		const FBashSample& Before = State.Samples[0];
		const FBashSample& After = State.Samples[1];

		UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 ===== CHUT'S BASH — DISTANCE, not speed ====="));

		auto Print = [](const TCHAR* Label, const FBashSample& Sample)
		{
			if (!Sample.bValid && !Sample.bAirValid)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("AUDITV16 BASH %-8s INVALID (speed %.0f uu/s, deflected=%d) — no distance to report."),
					Label, Sample.Speed, Sample.bDeflected ? 1 : 0);
				return;
			}
			UE_LOG(LogTraceGame, Display,
				TEXT("AUDITV16 BASH %-8s knock speed %7.1f uu/s -> total distance %8.1f uu (valid=%d) "
				     "| airborne leg %7.1f uu over %.3f s (valid=%d), then the ground bleed"),
				Label, Sample.Speed, Sample.Distance, Sample.bValid ? 1 : 0,
				Sample.AirDistance, Sample.Airtime, Sample.bAirValid ? 1 : 0);
		};

		Print(TEXT("BEFORE"), Before);
		Print(TEXT("AFTER"), After);

		// THE FIXTURE, STATED OUT LOUD. If the victim was still steering itself the distances are a
		// measurement of a bot's pathing as much as of the bash, and the ratio below means nothing.
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 BASH fixture: victim's worst self-steering acceleration during flight %.1f uu/s^2 "
			     "(0 means the move-input freeze held; anything else means the ratio is contaminated)."),
			State.VictimAccelWorst);

		const float SpeedRatio = After.Speed / FMath::Max(1.f, Before.Speed);

		// ONE HELPER, TWO LEGS. The airborne arc and the whole knock are reported the same way and
		// solved the same way, so a run that only produced a clean flight still answers the question.
		auto ReportLeg = [SpeedRatio, &Before](const TCHAR* Leg, const bool bUsable,
		                                       const float BeforeDist, const float AfterDist)
		{
			if (!bUsable || BeforeDist <= 1.f)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("AUDITV16 BASH %s: no ratio — an arm is INVALID. Treat this leg as no evidence."), Leg);
				return;
			}

			const float DistanceRatio = AfterDist / BeforeDist;
			UE_LOG(LogTraceGame, Display,
				TEXT("AUDITV16 BASH RESULT (%s): %.1f uu -> %.1f uu. Speed x%.4f (%+.1f%%) produced "
				     "distance x%.4f (%+.1f%%). The doc asked for +25%% DISTANCE."),
				Leg, BeforeDist, AfterDist, SpeedRatio, (SpeedRatio - 1.f) * 100.f,
				DistanceRatio, (DistanceRatio - 1.f) * 100.f);

			// --- what value WOULD give +25%? ------------------------------------------------------
			//
			// Two estimates, printed together on purpose. The exponent fit is the honest one: it uses
			// only the two measurements and assumes no model. The quadratic solve is what the airborne
			// leg predicts analytically (d_air = (2 x UpBias / g) x Speed^2), and printing both shows
			// whether the ground bleed bends the curve away from a pure square.
			const float Exponent = (SpeedRatio > 1.0001f && DistanceRatio > 0.f)
				? FMath::Loge(DistanceRatio) / FMath::Loge(SpeedRatio)
				: 0.f;
			const float FromFit = (Exponent > 0.01f)
				? Before.Speed * FMath::Pow(1.25f, 1.f / Exponent)
				: 0.f;
			const float FromSquare = Before.Speed * FMath::Sqrt(1.25f);

			UE_LOG(LogTraceGame, Display,
				TEXT("AUDITV16 BASH (%s): distance scales as speed^%.3f here. For +25%% DISTANCE off the old "
				     "%.0f uu/s, ChutBashKnockbackSpeed would need to be ~%.0f uu/s (measured-exponent fit) / "
				     "~%.0f uu/s (pure-quadratic airborne model). REPORTED ONLY — TraceSettings.h is not this "
				     "harness's to edit."),
				Leg, Exponent, Before.Speed, FromFit, FromSquare);
		};

		ReportLeg(TEXT("AIRBORNE LEG"), Before.bAirValid && After.bAirValid, Before.AirDistance, After.AirDistance);
		ReportLeg(TEXT("TOTAL KNOCK"), Before.bValid && After.bValid, Before.Distance, After.Distance);
	}

	void RunBash()
	{
		UWorld* World = AuthWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 bash: server only — TryBash is authority-gated."));
			return;
		}

		TSharedPtr<FBashState> State = MakeShared<FBashState>();
		State->Deadline = FPlatformTime::Seconds() + 180.0;
		State->PhaseStartReal = FPlatformTime::Seconds();
		State->OriginalKnockbackSpeed = UTraceSettings::Get().ChutBashKnockbackSpeed;

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== BASH. Two arms in one world: the OLD 1000 uu/s and the shipped %.0f uu/s. "
			     "Both go through the shipped UTraceAbilitySetChut::TryBash. ====="),
			State->OriginalKnockbackSpeed);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr || FPlatformTime::Seconds() > State->Deadline)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 bash ABORTED at arm %d phase %d."),
					State->Arm, State->Phase);
				SetKnockbackSpeed(State->OriginalKnockbackSpeed);
				ThawVictim(*State);
				ReportBash(*State);
				return false;
			}

			// --- stage ----------------------------------------------------------------------------
			if (State->Phase < 0)
			{
				UTraceAbilityComponent* Human = FirstHumanAbilityComponent(TickWorld);
				if (Human == nullptr)
				{
					return true;
				}
				FString BlockedBy;
				if (!AcquireCharacter(TickWorld, Human, ETraceCharacterId::Chut, BlockedBy))
				{
					// NAME THE OBSTACLE. Silence here is what turned the first run into a 180 s stall
					// and an "ABORTED at phase -1" that told the reader nothing.
					if (FPlatformTime::Seconds() > State->PhaseStartReal + 15.0)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("AUDITV16 bash: could not put the human on Chut after 15 s (currently %s, "
							     "blocked by %s). INVALID."),
							TraceCharacterIdToString(Human->GetCharacterId()),
							BlockedBy.IsEmpty() ? TEXT("nothing — the roster simply refused") : *BlockedBy);
						SetKnockbackSpeed(State->OriginalKnockbackSpeed);
						ThawVictim(*State);
						ReportBash(*State);
						return false;
					}
					return true;
				}

				UTraceAbilitySetChut* ChutSet = Human->GetAbilitySetAs<UTraceAbilitySetChut>();
				ATraceCharacter* ChutPawn = Human->GetOwningCharacter();
				if (ChutSet == nullptr || ChutPawn == nullptr || !ChutPawn->IsAlive())
				{
					return true;
				}

				// A VICTIM THE CHOKE POINT WILL ACCEPT: living, not this pawn, and not the carrier.
				// Refusing to fall back to "any pawn" matters — a bash refused by the §4 rule reads
				// as a distance of zero, which would look like a broken knockback.
				ATraceCharacter* Victim = nullptr;
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || Candidate == ChutPawn || !Candidate->IsAlive())
					{
						continue;
					}
					if (Candidate->IsCarrier())
					{
						continue;   // §6: the carrier is immune, and that is a different test
					}
					if (!UTraceAbilityComponent::CanAffect(ChutPawn, Candidate, ETraceAbilityEffect::Control))
					{
						continue;
					}
					Victim = Candidate;
					break;
				}
				if (Victim == nullptr)
				{
					return true;
				}

				State->Chut = Human;
				State->ChutPawn = ChutPawn;
				State->Victim = Victim;
				State->Phase = 0;

				if (AController* VictimController = Victim->GetController())
				{
					VictimController->StopMovement();
					VictimController->SetIgnoreMoveInput(true);
					VictimController->SetActorTickEnabled(false);
					State->FrozenController = VictimController;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("AUDITV16 bash staged: Chut=%s victim=%s (victim move input frozen=%d)"),
					*GetNameSafe(ChutPawn), *GetNameSafe(Victim), State->FrozenController.IsValid() ? 1 : 0);
				return true;
			}

			ATraceCharacter* ChutPawn = State->ChutPawn.Get();
			ATraceCharacter* Victim = State->Victim.Get();
			UTraceAbilityComponent* Comp = State->Chut.Get();
			UTraceAbilitySetChut* ChutSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetChut>() : nullptr;
			if (ChutPawn == nullptr || Victim == nullptr || ChutSet == nullptr || !Victim->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("AUDITV16 bash ABORTED: a fixture pawn went away or died. Both arms INVALID."));
				SetKnockbackSpeed(State->OriginalKnockbackSpeed);
				ThawVictim(*State);
				ReportBash(*State);
				return false;
			}

			UTraceCharacterMovementComponent* VictimMove = MovementOf(Victim);
			if (VictimMove == nullptr)
			{
				return false;
			}

			// --- phase 0: place the victim on a clear lane and knock -------------------------------
			if (State->Phase == 0)
			{
				const UTraceSettings& Settings = UTraceSettings::Get();
				const float Speed = (State->Arm == 0) ? 1000.f : State->OriginalKnockbackSpeed;
				SetKnockbackSpeed(Speed);

				// Both arms are knocked from the SAME place along the SAME lane. A lane chosen per arm
				// would make the two distances measurements of two different bits of floor.
				if (State->Arm == 0)
				{
					// THE LANE MUST COVER THE WHOLE KNOCK, not just the airborne leg. The first run
					// sized it for the flight (1708 uu) and then measured a TOTAL of 2425 uu, so the
					// pawn finished its ground slide inside cover and the sample came back deflected.
					// The airborne leg is (2 x UpBias / g) x Speed^2; the ground slide that follows is
					// comparable again, so budget several times over and say so.
					const float Faster = FMath::Max(1000.f, State->OriginalKnockbackSpeed);
					const float Gravity = FMath::Max(1.f, FMath::Abs(VictimMove->GetGravityZ()));
					const float PredictedAir = (2.f * FMath::Max(0.f, Settings.ChutBashUpBias) / Gravity)
						* Faster * Faster;
					const float NeededLane = FMath::Max(3000.f, PredictedAir * 4.0f);

					State->VictimStart = Victim->GetActorLocation();
					if (!FindClearLaneFrom(TickWorld, Victim, NeededLane, State->KnockDirection))
					{
						// Walk the fixture toward the middle of the field and try again rather than
						// giving up: a spawn corner has no 3000 uu lane in any direction, and that is a
						// fact about where the pawn happened to be standing, not about the bash.
						++State->LaneAttempts;
						if (State->LaneAttempts > 8)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("AUDITV16 bash: no clear %.0f uu lane anywhere after 8 relocations "
								     "(last tried from %s). Both arms INVALID rather than measured "
								     "through a wall."),
								NeededLane, *State->VictimStart.ToCompactString());
							SetKnockbackSpeed(State->OriginalKnockbackSpeed);
							ThawVictim(*State);
							ReportBash(*State);
							return false;
						}
						FVector TowardCentre = -State->VictimStart;
						TowardCentre.Z = 0.f;
						if (!TowardCentre.Normalize())
						{
							TowardCentre = FVector::ForwardVector;
						}
						Victim->SetActorLocation(State->VictimStart + TowardCentre * 1800.f,
							false, nullptr, ETeleportType::TeleportPhysics);
						return true;
					}
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 bash lane: %s, clear for %.0f uu (after %d relocation(s))"),
						*State->KnockDirection.ToCompactString(), NeededLane, State->LaneAttempts);
				}
				else
				{
					Victim->SetActorLocation(State->VictimStart, false, nullptr, ETeleportType::TeleportPhysics);
				}

				VictimMove->Velocity = FVector::ZeroVector;
				VictimMove->SetMovementMode(MOVE_Walking);

				// Stand Chut just behind the victim so the CURRENT-location reach test in TryBash is a
				// real one rather than an override. DashProgress 1.0 is the end of the dash, which is
				// the only window §6 allows.
				ChutPawn->SetActorLocation(
					State->VictimStart - State->KnockDirection * (Settings.ChutBashRadiusUU * 0.5f),
					false, nullptr, ETeleportType::TeleportPhysics);

				State->Phase = 1;
				State->PhaseStartReal = FPlatformTime::Seconds();
				return true;
			}

			// --- phase 1: fire ---------------------------------------------------------------------
			if (State->Phase == 1)
			{
				if (FPlatformTime::Seconds() - State->PhaseStartReal < 0.25)
				{
					return true;   // let both teleports settle onto the floor
				}

				State->VictimStart = Victim->GetActorLocation();

				// ONE BASH PER VICTIM PER DASH is a shipped rule (BashedThisDash), and it refused the
				// SECOND arm outright the first time this ran — reported, correctly, as INVALID rather
				// than as a knock of zero. OnDashStarted is the shipped edge that clears the set; a new
				// arm is a new dash, so opening one is the honest way to re-arm rather than reaching
				// into the ability's state.
				ChutSet->OnDashStarted(State->KnockDirection);

				const bool bLanded = ChutSet->TryBash(Victim, 1.f, State->KnockDirection, 0.f);
				if (!bLanded)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("AUDITV16 bash arm %d: TryBash REFUSED the victim. INVALID — a refused bash is "
						     "not a short one."), State->Arm);
					State->Samples[State->Arm].Speed = UTraceSettings::Get().ChutBashKnockbackSpeed;
					State->Samples[State->Arm].bValid = false;
					State->Phase = 3;
					return true;
				}

				State->Samples[State->Arm].Speed = UTraceSettings::Get().ChutBashKnockbackSpeed;
				State->bLanded = false;
				State->LaunchRealTime = static_cast<float>(TickWorld->GetTimeSeconds());
				State->Phase = 2;
				State->PhaseStartReal = FPlatformTime::Seconds();
				return true;
			}

			// --- phase 2: follow the victim until it stops -----------------------------------------
			if (State->Phase == 2)
			{
				FBashSample& Sample = State->Samples[State->Arm];
				const FVector Here = Victim->GetActorLocation();
				const FVector Travel = FVector(Here.X - State->VictimStart.X, Here.Y - State->VictimStart.Y, 0.f);

				// DEFLECTION GUARD. The knock is a straight line by construction, so a travel vector
				// that has rotated away from the launch direction means the pawn hit something and the
				// distance is no longer a measurement of the bash.
				if (Travel.SizeSquared() > 100.f * 100.f)
				{
					const float Cos = static_cast<float>(FVector::DotProduct(Travel.GetSafeNormal(), State->KnockDirection));
					if (Cos < 0.985f)   // ~10 degrees
					{
						Sample.bDeflected = true;
						if (!State->bLanded)
						{
							Sample.bAirDeflected = true;
						}
					}
				}

				State->VictimAccelWorst = FMath::Max(State->VictimAccelWorst,
					static_cast<float>(VictimMove->GetCurrentAcceleration().Size2D()));

				if (!State->bLanded && VictimMove->IsMovingOnGround()
					&& FPlatformTime::Seconds() - State->PhaseStartReal > 0.1)
				{
					State->bLanded = true;
					State->VictimLanded = Here;
					Sample.AirDistance = static_cast<float>(Travel.Size());
					Sample.Airtime = static_cast<float>(TickWorld->GetTimeSeconds()) - State->LaunchRealTime;
					Sample.bAirValid = !Sample.bAirDeflected && Sample.AirDistance > 1.f;
				}

				const bool bStopped = State->bLanded && PlanarSpeedOf(VictimMove) < 20.f;
				if (bStopped || FPlatformTime::Seconds() - State->PhaseStartReal > 6.0)
				{
					Sample.Distance = static_cast<float>(Travel.Size());
					Sample.bValid = !Sample.bDeflected && Sample.Distance > 1.f;
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 bash arm %d: %.0f uu/s -> %.1f uu total (air leg %.1f uu / %.3f s), "
						     "deflected=%d"),
						State->Arm, Sample.Speed, Sample.Distance, Sample.AirDistance, Sample.Airtime,
						Sample.bDeflected ? 1 : 0);
					State->Phase = 3;
				}
				return true;
			}

			// --- phase 3: next arm, or report -------------------------------------------------------
			if (State->Phase == 3)
			{
				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Phase = 0;
					return true;
				}
				SetKnockbackSpeed(State->OriginalKnockbackSpeed);
				ThawVictim(*State);
				ReportBash(*State);
				return false;
			}

			return false;
		}), 0.f);
	}

	// =============================================================================================
	// Trace.Move.AuditV16.Abilities — the ability-driven movement
	//
	// Rocco's second jump, Mace's suspend, Mace's Spike pull and Oyster's jar-jump. Each is a
	// different owner's code, so this measures rather than asserts internals: press the shipped entry
	// point, watch Velocity.
	// =============================================================================================

	struct FAbilityMoveState
	{
		int32 Stage = 0;           // 0 Rocco, 1 Mace suspend, 2 Oyster jar-jump
		int32 Phase = -1;
		double Deadline = 0.0;
		double PhaseStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Comp;
		TWeakObjectPtr<ATraceCharacter> Pawn;

		float RoccoZBefore = 0.f;
		float RoccoZAfter = 0.f;
		bool  bRoccoValid = false;

		float SuspendZWorst = 0.f;
		float SuspendLateralWorst = 0.f;
		float SuspendSeconds = 0.f;
		float SuspendStartZ = 0.f;
		float SuspendDrop = 0.f;
		float SuspendFreeFallDrop = 0.f;
		bool  bSuspendValid = false;

		float JarJumpZ = 0.f;
		bool  bJarJumpValid = false;
	};

	void ReportAbilityMoves(const FAbilityMoveState& State)
	{
		const UTraceSettings& S = UTraceSettings::Get();

		UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 ===== ABILITY-DRIVEN MOVEMENT ====="));

		if (State.bRoccoValid)
		{
			Row(TEXT("ROCCO second jump Z"), TEXT("uu/s"), Biased(S.RoccoSecondJumpZVelocity), State.RoccoZAfter, 0.05f,
				*FString::Printf(TEXT("mid-air, Z was %.1f uu/s before the press"), State.RoccoZBefore));
		}
		else
		{
			RowInvalid(TEXT("ROCCO second jump Z"), TEXT("the second jump never fired"));
		}

		if (State.bSuspendValid)
		{
			// THE ASSERTION SPEC v16 §5 SAYS IS MISSING, and it is ALTITUDE, not Velocity.Z.
			//
			// ApplySuspend re-zeroes Velocity.Z once per component tick while PhysFalling integrates
			// gravity on every sub-step in between, so an instantaneous |Vz| sample always catches
			// some fraction of a frame's fall — the first run of this audit read 141 uu/s and called
			// it a FAIL. What "gravity does not affect her" actually claims is that she does not
			// DESCEND, and net drop over the whole window is exactly that claim, measured once,
			// immune to where in the frame the sample lands.
			//
			// The comparison is against free fall over the same window, printed alongside, because
			// "she dropped 12 uu" only means something next to "she would have dropped 857".
			// TOLERANCE AGAINST FREE FALL, not against zero. ApplySuspend re-zeroes Velocity.Z once per
			// component tick and PhysFalling integrates gravity on every sub-step in between, so a
			// small residual sink is structural, not a bug — demanding an exact 0 would fail this row
			// forever and teach the reader to ignore it. 5% of free fall over the same window is the
			// bar: anything under that is "gravity does not affect her" in any sense a player can see.
			Row(TEXT("MACE suspend altitude drop"), TEXT("uu"),
				Biased(State.SuspendFreeFallDrop * 0.05f), State.SuspendDrop, 1.0f,
				*FString::Printf(TEXT("over %.2f s of held suspend (cap %.2f s); free fall over the same "
				                      "window would be %.0f uu. Worst instantaneous |Vz| %.1f uu/s is one "
				                      "frame of gravity between re-zeroes, not a fall."),
					State.SuspendSeconds, S.MaceSuspendMaxSeconds, State.SuspendFreeFallDrop,
					State.SuspendZWorst));

			// A CAP IS A CEILING, NOT A TARGET. The previous row demanded the measurement EQUAL 550,
			// which fails whenever the pawn is slower than the cap — i.e. whenever the cap is doing
			// its job. This asserts the ceiling and separately asserts that the fixture went fast
			// enough to press against it, so "0 uu/s, PASS" is impossible.
			const bool bUnderCap = State.SuspendLateralWorst <= S.MaceSuspendLateralSpeedCap * (1.f + RedBias()) + 1.f;
			const bool bExercised = State.SuspendLateralWorst > S.MaceSuspendLateralSpeedCap * 0.9f;
			if (bExercised)
			{
				Row(TEXT("MACE suspend lateral cap"), TEXT("uu/s"), Biased(S.MaceSuspendLateralSpeedCap),
					bUnderCap ? Biased(S.MaceSuspendLateralSpeedCap) : State.SuspendLateralWorst, 0.001f,
					*FString::Printf(TEXT("worst planar speed while suspended was %.1f uu/s; the row asserts "
					                      "'<= the cap', with full lateral input held to press against it"),
						State.SuspendLateralWorst));
			}
			else
			{
				RowInvalid(TEXT("MACE suspend lateral cap"),
					*FString::Printf(TEXT("the fixture only reached %.1f uu/s against a %.0f uu/s cap — the "
					                      "ceiling was never pressed, so 'under the cap' proves nothing"),
						State.SuspendLateralWorst, S.MaceSuspendLateralSpeedCap));
			}
		}
		else
		{
			RowInvalid(TEXT("MACE suspend"), TEXT("suspend never engaged — needs Mace, airborne, V held"));
		}

		if (State.bJarJumpValid)
		{
			Row(TEXT("OYSTER jar-jump launch Z"), TEXT("uu/s"), Biased(S.OysterJarJumpZVelocity), State.JarJumpZ, 0.05f,
				TEXT("a jar broken under Oyster's own feet"));
		}
		else
		{
			RowInvalid(TEXT("OYSTER jar-jump launch Z"), TEXT("no jar-jump was staged"));
		}

		// Mace's Spike is a long-range projectile with its own consistency harness
		// (Trace.Mace.RemotePullTest, 1443 lines of it). Duplicating it here would be a second
		// opinion about the same rule, which is how this project got two carrier tests that could
		// disagree. Say where the answer lives instead of half-measuring it.
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 | %-34s | not measured here on purpose — Trace.Mace.Verify and "
			     "Trace.Mace.RemotePullTest own this rule (travel %.0f uu/s, range %.0f uu, pull x%.2f)."),
			TEXT("MACE Spike pull"), S.MaceSpikeTravelSpeed, S.MaceSpikeRangeUU, S.MaceSpikePullSpeedMultiplier);
	}

	void RunAbilityMoves()
	{
		UWorld* World = AuthWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 ability moves: server only."));
			return;
		}

		TSharedPtr<FAbilityMoveState> State = MakeShared<FAbilityMoveState>();
		State->Deadline = FPlatformTime::Seconds() + 180.0;
		State->PhaseStartReal = FPlatformTime::Seconds();

		UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 ===== ability-driven movement starting. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr || FPlatformTime::Seconds() > State->Deadline)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 ability moves ABORTED at stage %d phase %d."),
					State->Stage, State->Phase);
				ReportAbilityMoves(*State);
				return false;
			}

			static const ETraceCharacterId Wanted[3] =
				{ ETraceCharacterId::Rocco, ETraceCharacterId::Mace, ETraceCharacterId::Oyster };

			UTraceAbilityComponent* Comp = FirstHumanAbilityComponent(TickWorld);
			if (Comp == nullptr)
			{
				return true;
			}

			// --- swap to the character this stage needs ---------------------------------------------
			if (State->Phase < 0)
			{
				FString BlockedBy;
				if (!AcquireCharacter(TickWorld, Comp, Wanted[State->Stage], BlockedBy))
				{
					if (FPlatformTime::Seconds() > State->PhaseStartReal + 15.0)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("AUDITV16 ability moves: could not put the human on %s after 15 s "
							     "(currently %s, blocked by %s). Skipping this stage."),
							TraceCharacterIdToString(Wanted[State->Stage]),
							TraceCharacterIdToString(Comp->GetCharacterId()),
							BlockedBy.IsEmpty() ? TEXT("nothing — the roster simply refused") : *BlockedBy);
						++State->Stage;
						State->PhaseStartReal = FPlatformTime::Seconds();
						if (State->Stage > 2)
						{
							ReportAbilityMoves(*State);
							return false;
						}
					}
					return true;
				}
				ATraceCharacter* Pawn = Comp->GetOwningCharacter();
				if (Pawn == nullptr || !Pawn->IsAlive() || MovementOf(Pawn) == nullptr)
				{
					return true;
				}
				State->Comp = Comp;
				State->Pawn = Pawn;
				State->Phase = 0;
				State->PhaseStartReal = FPlatformTime::Seconds();
				return true;
			}

			ATraceCharacter* Pawn = State->Pawn.Get();
			UTraceCharacterMovementComponent* Move = MovementOf(Pawn);
			if (Pawn == nullptr || Move == nullptr || !Pawn->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 ability moves ABORTED: pawn gone or dead."));
				ReportAbilityMoves(*State);
				return false;
			}

			const double Elapsed = FPlatformTime::Seconds() - State->PhaseStartReal;

			auto NextStage = [&State]()
			{
				++State->Stage;
				State->Phase = -1;
				State->PhaseStartReal = FPlatformTime::Seconds();
			};

			// =========================================================================================
			// Stage 0 — Rocco's second jump
			// =========================================================================================
			if (State->Stage == 0)
			{
				if (State->Phase == 0)
				{
					// Get airborne with an ordinary jump: the ability declines on the ground on
					// purpose ("the normal jump owns the key"), so a ground press would measure the
					// wrong thing entirely.
					if (Move->IsMovingOnGround())
					{
						Pawn->Jump();
						return true;
					}
					Pawn->StopJumping();

					// WAIT UNTIL THE FLOOR IS OBSERVABLE. The second jump is
					// Velocity.Z = Max(Velocity.Z, RoccoSecondJumpZVelocity) — a FLOOR, deliberately,
					// so a Rocco still rising from his first jump is not SLOWED by pressing again. The
					// first run of this audit pressed at Vz = 278.6 against a 260 floor, watched Z not
					// change, and reported "the second jump never fired". It fired and correctly did
					// nothing. Pressing below the floor is the only press that can show the floor.
					if (Move->Velocity.Z > UTraceSettings::Get().RoccoSecondJumpZVelocity * 0.9f
						&& Elapsed < 3.0)
					{
						return true;
					}
					// SAMPLED IN THE SAME CALL AS THE PRESS. The ability writes Move->Velocity
					// synchronously, and nothing else runs between these two lines — whereas waiting
					// for the next ticker call costs one physics step of gravity and turns a correct
					// 260.0 into a 240.5 "FAIL", which is what the first run of this audit reported.
					State->RoccoZBefore = Move->Velocity.Z;
					Comp->HandleJumpPressed();
					State->RoccoZAfter = Move->Velocity.Z;
					State->Phase = 1;
					State->PhaseStartReal = FPlatformTime::Seconds();
					return true;
				}
				if (State->Phase == 1)
				{
					State->bRoccoValid = !FMath::IsNearlyEqual(State->RoccoZAfter, State->RoccoZBefore, 1.f);
					UE_LOG(LogTraceGame, Display,
						TEXT("AUDITV16 ROCCO second jump: Vz %.1f -> %.1f uu/s"),
						State->RoccoZBefore, State->RoccoZAfter);
					NextStage();
					return true;
				}
			}

			// =========================================================================================
			// Stage 1 — Mace's suspend. THE ASSERTION THE SPEC SAYS IS MISSING.
			// =========================================================================================
			if (State->Stage == 1)
			{
				if (State->Phase == 0)
				{
					// RUN INTO THE JUMP. The lateral cap is 550 uu/s and AirMaxWishSpeed is 160, so a
					// Mace who jumps from a standstill can never reach the ceiling under her own air
					// control — the first run of this audit topped out at exactly 160 uu/s and the cap
					// row was untestable. Carrying 800 uu/s of ground speed into the jump puts her
					// over the cap on the first suspended frame, which is the case the clamp is for.
					Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 1.f);
					if (Move->IsMovingOnGround())
					{
						if (PlanarSpeedOf(Move) > Move->GetMaxSpeed() * 0.95f || Elapsed > 3.0)
						{
							Pawn->Jump();
						}
						return true;
					}
					Pawn->StopJumping();
					if (Elapsed < 0.30)
					{
						return true;
					}
					Comp->HandleSecondaryPressed();
					State->Phase = 1;
					State->PhaseStartReal = FPlatformTime::Seconds();
					State->SuspendZWorst = 0.f;
					State->SuspendLateralWorst = 0.f;
					State->SuspendStartZ = static_cast<float>(Pawn->GetActorLocation().Z);
					return true;
				}
				if (State->Phase == 1)
				{
					// The held-input contract: the ability stops the moment the button is released,
					// so re-assert it every frame exactly as the input layer does.
					const UTraceAbilitySetMace* MaceSet = Comp->GetAbilitySetAs<UTraceAbilitySetMace>();
					const bool bSuspending = (MaceSet != nullptr) && MaceSet->GetSuspendRemaining() > 0.f;
					if (bSuspending)
					{
						// FULL LATERAL INPUT, HELD. "She may move laterally capped at 550 uu/s" is a
						// claim about a ceiling, and a ceiling nobody pushes against is untested.
						Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 1.f);

						State->bSuspendValid = true;
						State->SuspendSeconds = static_cast<float>(Elapsed);
						State->SuspendZWorst = FMath::Max(State->SuspendZWorst, FMath::Abs(Move->Velocity.Z));
						State->SuspendLateralWorst = FMath::Max(State->SuspendLateralWorst, PlanarSpeedOf(Move));
						State->SuspendDrop = State->SuspendStartZ - static_cast<float>(Pawn->GetActorLocation().Z);
						State->SuspendFreeFallDrop = 0.5f * FMath::Abs(Move->GetGravityZ())
							* State->SuspendSeconds * State->SuspendSeconds;
					}
					if (Elapsed > static_cast<double>(UTraceSettings::Get().MaceSuspendMaxSeconds) + 0.3
						|| (State->bSuspendValid && !bSuspending))
					{
						Comp->HandleSecondaryReleased();
						UE_LOG(LogTraceGame, Display,
							TEXT("AUDITV16 MACE suspend: held %.2f s, worst |Vz| %.2f uu/s, worst planar %.1f uu/s"),
							State->SuspendSeconds, State->SuspendZWorst, State->SuspendLateralWorst);
						NextStage();
					}
					return true;
				}
			}

			// =========================================================================================
			// Stage 2 — Oyster's jar-jump
			// =========================================================================================
			if (State->Stage == 2)
			{
				UTraceAbilitySetOyster* OysterSet = Comp->GetAbilitySetAs<UTraceAbilitySetOyster>();
				if (OysterSet == nullptr)
				{
					RowInvalid(TEXT("OYSTER jar-jump"), TEXT("the Oyster ability set never built"));
					ReportAbilityMoves(*State);
					return false;
				}
				if (State->Phase == 0)
				{
					if (!Move->IsMovingOnGround())
					{
						return true;
					}
					// A JAR-JUMP NEEDS A JAR. The first run of this audit called DebugTryJarJump on an
					// Oyster who had thrown nothing and reported "kept refusing (no jars?)" for five
					// seconds — the refusal was correct and the fixture was the thing missing.
					// DebugSpawnJarAt is the set's own debug spawn, so the jar is a real jar.
					if (OysterSet->GetLiveJarCount() <= 0)
					{
						OysterSet->DebugSpawnJarAt(Pawn->GetActorLocation(), false);
						return true;
					}

					// DebugTryJarJump breaks a jar under Oyster's own feet through DoJarJump, the same
					// entry the live jar-break path uses. Sampled in the SAME call for the reason
					// Rocco's second jump is: one ticker call later is one physics step of gravity.
					if (!OysterSet->DebugTryJarJump())
					{
						if (Elapsed > 8.0)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("AUDITV16 OYSTER jar-jump: DebugTryJarJump kept refusing with %d live jar(s)."),
								OysterSet->GetLiveJarCount());
							ReportAbilityMoves(*State);
							return false;
						}
						return true;
					}
					State->JarJumpZ = Move->Velocity.Z;
					State->Phase = 1;
					State->PhaseStartReal = FPlatformTime::Seconds();
					return true;
				}
				if (State->Phase == 1)
				{
					State->bJarJumpValid = State->JarJumpZ > 1.f;
					UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 OYSTER jar-jump: Vz %.1f uu/s"), State->JarJumpZ);
					ReportAbilityMoves(*State);
					return false;
				}
			}

			return true;
		}), 0.f);
	}

	// =============================================================================================
	// Trace.Move.AuditV16.Red — THE FALSIFICATION ARM
	//
	// Spec v16's standing rule: a harness that cannot go red is not evidence. This biases every
	// EXPECTED value by +12% while leaving the game completely untouched, then runs the identical
	// core-kit script. Every row that is genuinely comparing a measurement against a derived
	// expectation MUST flip to FAIL; any row that stays PASS is a row that is not actually testing
	// anything, and is a defect in this file rather than in the movement kit.
	//
	// 12% because it is comfortably outside every tolerance used above (the loosest is 10%) and
	// comfortably inside the range where the measurements stay physically sane.
	// =============================================================================================

	void RunRedArm()
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== RED ARM. Expectations biased by +12%%; the game is NOT modified. Every "
			     "MEASURED row below MUST read FAIL. A PASS on a measured row is a row that compares "
			     "nothing. The one exception, stated so nobody has to guess: 'CROUCH engine crouch "
			     "disabled' is a boolean identity check that no bias can move, and it stays PASS. ====="));
		GRedBias = 0.12f;
		RunCoreKit();

		// Cleared on a delay rather than immediately: the core kit is asynchronous, so clearing here
		// would restore the green expectations before the first row is ever printed.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
		{
			GRedBias = 0.f;
			UE_LOG(LogTraceGame, Display, TEXT("AUDITV16 red arm: bias cleared, expectations back to shipped."));
			return false;
		}), 200.f);
	}

	// =============================================================================================
	// SPEC v26 §3 — FOUR CHAINED SLIDE-JUMPS, MEASURED
	//
	// v26 §3, verbatim: "Reduce slide jump momentum boost by 20%. Add a ceiling to slide jump
	// momentum boosts, so that you can't chain them over and over to go faster and faster. Right now,
	// if you do three slide jump boosts in a row you can zip down the whole field. For now, lets cap
	// it at what the momentum is after you do two consecutive slide boosts."
	//
	// WHY THIS CANNOT BE A CONFIG READ, WHICH IS WHY IT IS A WHOLE NEW HARNESS. Every other number in
	// §3 is arithmetic and the V26TUNING lines in the movement component already print it. The number
	// the owner's complaint is ABOUT is not: "if you do three slide jump boosts in a row you can zip
	// down the whole field" is a claim about the speed reached at the END of a real chain, and that
	// depends on how far each slide decayed before its hop, how much of the landing survived the
	// ground friction, and whether the chain was still alive when the next slide started. No
	// expression on any settings page knows those. So this drives a live pawn through four ACTUAL
	// chained slide-jumps and prints the launch speed of each.
	//
	// THE RED ARM IS Trace.V26LegacySlideJump 1, and it is the point of the exercise. It reverts the
	// -20% and turns the ceiling off in the SAME BINARY, so "before" and "after" are the same four
	// phases on the same lane of the same arena rather than two builds an afternoon apart.
	//
	//     Trace.V26LegacySlideJump 1 ; Trace.Move.AuditV16.SlideChain     <- boosts 3 and 4 climb
	//     Trace.V26LegacySlideJump 0 ; Trace.Move.AuditV16.SlideChain     <- boost 3 pinned to 2
	//
	// If the two arms print the same four numbers, the harness is not measuring its rule and the run
	// is worth nothing — say so rather than reporting the green arm on its own.
	//
	// THE FIXTURE CHECKS, because a chain is easy to measure wrong:
	//   * every hop must be WELL TIMED. A mistimed hop has no boost at all (retention is 1.0), so a
	//     chain containing one is measuring the player, not the mechanic. Recorded, never assumed.
	//   * the chain must still be ALIVE at each press. If the pawn gave the momentum back between
	//     hops — walked into cover, ran out of lane — the next hop is a FIRST hop and the row is a
	//     measurement of a fresh chain wearing hop 3's label. The component's own chain counter is
	//     read at the press and printed, so this is visible rather than assumed.
	//   * the lane is swept before the run starts. Four chained hops cover thousands of uu, and a
	//     pawn that puts its face into midfield cover on hop 2 reports a beautifully capped hop 3.
	// =============================================================================================

	struct FSlideChainState
	{
		static constexpr int32 MaxHops = 4;

		int32 Phase = 0;
		float PhaseTime = 0.f;
		double Deadline = 0.0;

		TWeakObjectPtr<ATraceCharacter> Pawn;
		FVector RunDirection = FVector::ForwardVector;
		FVector Home = FVector::ZeroVector;
		float LaneLength = 0.f;

		int32 HopIndex = 0;   // hops completed so far; also the index being filled

		/** Slide speed at the instant the jump was pressed — what the launch is a multiple OF. */
		float Carry[MaxHops] = {};
		/** Planar speed on the first airborne frame. THE HEADLINE NUMBER. */
		float Launch[MaxHops] = {};
		/** Highest planar speed reached during the airborne arc, for context. */
		float AirTop[MaxHops] = {};
		/** The component's own chain counter, read at the press (0-based: 0 means "this is hop 1"). */
		int32 ChainAtPress[MaxHops] = {};
		/** The chain's measured ceiling after the hop, or 0 if none is recorded yet. */
		float Ceiling[MaxHops] = {};
		bool  bWellTimed[MaxHops] = {};
		bool  bValid[MaxHops] = {};

		float RunUpTop = 0.f;
		int32 ChainBreaks = 0;
	};

	void ReportSlideChain(const FSlideChainState& State)
	{
		ATraceCharacter* Pawn = State.Pawn.Get();
		UTraceCharacterMovementComponent* Move = MovementOf(Pawn);
		if (Move == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 slide chain: the pawn went away before the report."));
			return;
		}

		const UTraceSettings& S = UTraceSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== SPEC v26 §3: FOUR CHAINED SLIDE-JUMPS (netMode=%d role=%d) ====="),
			static_cast<int32>(Move->GetNetMode()),
			(Pawn != nullptr) ? static_cast<int32>(Pawn->GetLocalRole()) : -1);

		// The ARM, printed first and derived from the live component rather than from the cvar, so a
		// run whose arm did not take cannot be read as the arm it was asked for. The well-timed
		// multiplier IS the arm: 1.446875 is pre-v26, 1.3575 is shipped.
		const float Multiplier = Move->GetSlideJumpWindowSpeedBonusForAudit()
			* Move->GetSlideJumpHorizontalRetentionForAudit();
		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 arm: well-timed multiplier %.5f  (base %.5f x scale %.3f, then v26 gain scale "
			     "%.3f)   ceiling %s, cap after %d boost(s), chain reset at %5.0f uu/s"),
			Multiplier, S.SlideJumpWindowSpeedBonus, S.SlideJumpBonusScale, S.SlideJumpMomentumScale,
			Move->IsSlideJumpChainCapEnabledForAudit() ? TEXT("ON") : TEXT("OFF"),
			Move->GetSlideJumpChainCapBoostsForAudit(), Move->GetSlideJumpChainResetSpeedForAudit());

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 run-up top speed %.0f uu/s over a %.0f uu swept lane; %d chain break(s) between hops."),
			State.RunUpTop, State.LaneLength, State.ChainBreaks);

		float PreviousLaunch = 0.f;
		for (int32 Hop = 0; Hop < FSlideChainState::MaxHops; ++Hop)
		{
			if (!State.bValid[Hop])
			{
				RowInvalid(*FString::Printf(TEXT("SLIDEJUMP chain hop %d"), Hop + 1),
					TEXT("the hop was never taken — see the warnings above for which phase gave up"));
				continue;
			}

			// The chain counter read AT THE PRESS is the fixture check. It must equal the number of
			// hops already taken; anything less means the chain reset and this row is a fresh chain
			// mislabelled. With the ceiling OFF there is no chain to keep, so the check is not run
			// rather than run and failed — see the note where ChainBreaks is counted.
			const bool bChainIntact = !Move->IsSlideJumpChainCapEnabledForAudit()
				|| (State.ChainAtPress[Hop] == Hop);
			const float Ratio = (State.Carry[Hop] > 1.f) ? (State.Launch[Hop] / State.Carry[Hop]) : 0.f;

			UE_LOG(LogTraceGame, Display,
				TEXT("AUDITV16 | hop %d | carry %7.1f -> LAUNCH %7.1f uu/s (x%.4f) | air top %7.1f | "
				     "chain depth at press %d %s | ceiling after %7.1f | %s%s"),
				Hop + 1, State.Carry[Hop], State.Launch[Hop], Ratio, State.AirTop[Hop],
				State.ChainAtPress[Hop],
				!Move->IsSlideJumpChainCapEnabledForAudit() ? TEXT("(n/a, ceiling off)")
					: (bChainIntact ? TEXT("(intact)") : TEXT("(*** BROKEN ***)")),
				State.Ceiling[Hop],
				State.bWellTimed[Hop] ? TEXT("well timed") : TEXT("*** MISTIMED - row is not comparable ***"),
				(PreviousLaunch > 0.f)
					? *FString::Printf(TEXT(" | vs previous hop %+.1f uu/s (%+.1f%%)"),
						State.Launch[Hop] - PreviousLaunch,
						100.f * (State.Launch[Hop] - PreviousLaunch) / FMath::Max(1.f, PreviousLaunch))
					: TEXT(""));

			PreviousLaunch = State.Launch[Hop];
		}

		// --- THE ONE VERDICT THE NOTE ASKS FOR --------------------------------------------------
		//
		// "if you do three slide jump boosts in a row you can zip down the whole field [...] cap it at
		// what the momentum is after you do two consecutive slide boosts."
		//
		// So: hop 3 must not be faster than hop 2. Expressed as a comparison of two MEASURED numbers
		// rather than against a derived expectation, because that is exactly what the sentence says
		// and because both sides of it come off the same run.
		const bool bHaveThree = State.bValid[1] && State.bValid[2];
		if (!bHaveThree)
		{
			RowInvalid(TEXT("v26 §3b third hop <= second"),
				TEXT("fewer than three hops completed — nothing to compare"));
		}
		else
		{
			// A percent of slack, not zero: the launch is sampled on the first airborne frame, so at
			// 60 Hz two hops of an identical chain can differ by the fraction of a frame the physics
			// step landed on.
			const float Slack = 0.01f;
			const bool bCapped = State.Launch[2] <= State.Launch[1] * (1.f + Slack);
			UE_LOG(LogTraceGame, Display,
				TEXT("AUDITV16 | %-34s | hop2 %8.1f  hop3 %8.1f uu/s | %-4s | %s"),
				TEXT("v26 §3b third hop <= second"), State.Launch[1], State.Launch[2],
				bCapped ? TEXT("PASS") : TEXT("FAIL"),
				Move->IsSlideJumpChainCapEnabledForAudit()
					? TEXT("ceiling ON — a FAIL here is the ceiling not working")
					: TEXT("ceiling OFF (RED arm) — a PASS here means the harness measured nothing"));
		}

		if (State.bValid[1] && State.bValid[3])
		{
			const float Slack = 0.01f;
			const bool bCapped = State.Launch[3] <= State.Launch[1] * (1.f + Slack);
			UE_LOG(LogTraceGame, Display,
				TEXT("AUDITV16 | %-34s | hop2 %8.1f  hop4 %8.1f uu/s | %-4s |"),
				TEXT("v26 §3b fourth hop <= second"), State.Launch[1], State.Launch[3],
				bCapped ? TEXT("PASS") : TEXT("FAIL"));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== slide chain done. Run the other arm (Trace.V26LegacySlideJump) and diff "
			     "these four numbers; identical arms mean the harness is not measuring its rule. ====="));
	}

	void RunSlideChain()
	{
		UWorld* World = LocalGameWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 slide chain: no game world."));
			return;
		}

		TSharedPtr<FSlideChainState> State = MakeShared<FSlideChainState>();
		State->Deadline = FPlatformTime::Seconds() + 120.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("AUDITV16 ===== slide chain starting: four consecutive well-timed slide-jumps, no "
			     "teleports, no velocity writes. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float Delta) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("AUDITV16 slide chain ABORTED: the world went away."));
				return false;
			}
			if (FPlatformTime::Seconds() > State->Deadline)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("AUDITV16 slide chain ABORTED at phase %d, hop %d: ran out of time. Reporting "
					     "what was measured."), State->Phase, State->HopIndex + 1);
				ReportSlideChain(*State);
				return false;
			}

			ATraceCharacter* Pawn = State->Pawn.Get();
			if (Pawn == nullptr)
			{
				Pawn = LocalPawn(TickWorld);
				if (Pawn == nullptr || !Pawn->IsAlive())
				{
					return true;
				}
				State->Pawn = Pawn;
			}

			UTraceCharacterMovementComponent* Move = MovementOf(Pawn);
			if (Move == nullptr)
			{
				return true;
			}

			if (!Pawn->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("AUDITV16 slide chain ABORTED: the pawn died mid-run. Everything after the last "
					     "completed hop is INVALID, not zero."));
				ReportSlideChain(*State);
				return false;
			}

			State->PhaseTime += Delta;
			const float Planar = PlanarSpeedOf(Move);
			const int32 Hop = State->HopIndex;

			auto Advance = [&State](int32 Next)
			{
				State->Phase = Next;
				State->PhaseTime = 0.f;
			};

			switch (State->Phase)
			{
			// --- 0. stage, and SWEEP THE LANE ------------------------------------------------------
			//
			// Four chained hops cover thousands of uu. A pawn that puts its face into midfield cover on
			// hop 2 reports a beautifully capped hop 3 that is really a measurement of a wall, which is
			// the exact failure mode FindClearLaneFrom exists for — so the longest clear lane is found
			// FIRST and the whole chain runs down it.
			case 0:
			{
				if (!Move->IsMovingOnGround())
				{
					return true;
				}

				State->Home = Pawn->GetActorLocation();

				const float Wanted[] = { 16000.f, 12000.f, 9000.f, 6000.f, 4000.f };
				for (const float Length : Wanted)
				{
					FVector Lane;
					if (FindClearLaneFrom(TickWorld, Pawn, Length, Lane))
					{
						State->RunDirection = Lane;
						State->LaneLength = Length;
						break;
					}
				}

				if (State->LaneLength <= 0.f)
				{
					// Shuffle toward midfield and try again — the same recovery the dash phase uses.
					if (State->PhaseTime > 8.f)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("AUDITV16 slide chain: no clear lane of even 4000 uu from %s. A chain "
							     "cannot be measured here."), *State->Home.ToCompactString());
						ReportSlideChain(*State);
						return false;
					}
					FVector Toward = -Pawn->GetActorLocation();
					Toward.Z = 0.f;
					if (Pawn->HasAuthority() && Toward.Normalize())
					{
						Pawn->SetActorLocation(Pawn->GetActorLocation() + Toward * 1200.f,
							false, nullptr, ETeleportType::TeleportPhysics);
					}
					return true;
				}

				if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
				{
					PC->SetControlRotation(State->RunDirection.Rotation());
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("AUDITV16 slide chain staged at %s, running %s down a %.0f uu clear lane."),
					*State->Home.ToCompactString(), *State->RunDirection.ToCompactString(),
					State->LaneLength);
				Advance(1);
				break;
			}

			// --- 1. arm the next slide -------------------------------------------------------------
			//
			// Held input the whole way, for the reason the core kit's phase 13 gives: without it ground
			// friction stops the pawn dead and the hop is measured from a standstill, which measures the
			// harness rather than the mechanic.
			//
			// The slide intent is released for the WHOLE airborne arc in phase 3, so the press here is
			// always a fresh EDGE (bSlideHeldLastMove). Re-asserting a still-held intent would never
			// start a second slide and the chain would silently be one hop long.
			//
			// *** THE FIRST HOP MUST START FROM A FULL RUN AND THE REST MUST NOT WAIT AT ALL. ***
			// Both halves of that were learned from a run:
			//
			//   HOP 1. CanStartSlide only needs SlideEntrySpeedFraction x WalkSpeed (440 uu/s), and the
			//     first version slid the moment it crossed that. The slide then decayed to 442 and
			//     launched at 600 — BELOW the pawn's own 800 uu/s ground speed — so the chain was
			//     correctly declared spent on the very next landing and hop 2 was a FIRST hop wearing
			//     hop 2's label. The whole four-hop run measured two chains of two. Requiring a real
			//     run-up is not cosmetic: a chain that starts below walking pace has nothing to
			//     compound and is not the thing §3 is about.
			//
			//   HOPS 2-4. Every frame spent on the ground before the next slide is a frame of ground
			//     friction eating the carry, which is precisely how a chain ends. So these press crouch
			//     on the first frame the slide is legal — no settle time, no speed gate beyond the one
			//     CanStartSlide itself applies. That is also what a player chaining hops actually does.
			case 1:
			{
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				State->RunUpTop = FMath::Max(State->RunUpTop, Planar);

				const float EntryMinimum = FMath::Max(1.f, UTraceSettings::Get().WalkSpeed)
					* UTraceSettings::Get().SlideEntrySpeedFraction;

				// 0.98 rather than 1.0: ground acceleration approaches the ceiling asymptotically and
				// the last hundredth never arrives. 1.6 s is the same settle time the core-kit audit's
				// slide phases use, and it is what makes the two harnesses' entry speeds comparable.
				const bool bRunUpDone = (Hop > 0)
					|| (State->PhaseTime > 1.6f && Planar >= 0.98f * FMath::Max(1.f, Move->GetMaxSpeed()));

				if (Move->IsMovingOnGround()
					&& Move->GetSlideCooldownRemaining() <= 0.f
					&& Planar > EntryMinimum
					&& bRunUpDone)
				{
					Move->SetWantsToSlide(true);
					Advance(2);
					break;
				}

				if (State->PhaseTime > 6.f)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("AUDITV16 slide chain: hop %d never got a slide started (grounded=%d, "
						     "cooldown %.2fs, planar %.0f, entry minimum %.0f). Reporting what was measured."),
						Hop + 1, Move->IsMovingOnGround() ? 1 : 0, Move->GetSlideCooldownRemaining(),
						Planar, EntryMinimum);
					ReportSlideChain(*State);
					return false;
				}
				break;
			}

			// --- 2. sliding: wait for the window, then hop -----------------------------------------
			case 2:
				Pawn->AddMovementInput(State->RunDirection, 1.f);
				Move->SetWantsToSlide(true);

				if (Move->IsSlideJumpWellTimed())
				{
					// Recorded, never assumed: the chain depth AT THE PRESS is the fixture check that
					// says whether this really is hop N of one chain or hop 1 of a new one.
					State->bWellTimed[Hop] = true;
					State->Carry[Hop] = Move->GetSlideSpeedForAudit();
					State->ChainAtPress[Hop] = Move->GetSlideJumpChainBoostsForAudit();

					// ONLY MEANINGFUL WITH THE CEILING ON. With it off the component does not keep a
					// chain at all — DoJump zeroes the counter on every hop — so counting "breaks" in
					// the RED arm would report a fixture failure that is really the arm doing its job,
					// and a reader would discount the very numbers the arm exists to produce.
					if (Move->IsSlideJumpChainCapEnabledForAudit() && State->ChainAtPress[Hop] != Hop)
					{
						++State->ChainBreaks;
					}
					Pawn->Jump();
					Advance(3);
					break;
				}

				if (!Move->IsSlideJumpAvailable() || State->PhaseTime > 4.f)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("AUDITV16 slide chain: hop %d's window never opened (available=%d, %.2fs of "
						     "slide left)."),
						Hop + 1, Move->IsSlideJumpAvailable() ? 1 : 0, Move->GetSlideTimeLeftForAudit());
					ReportSlideChain(*State);
					return false;
				}
				break;

			// --- 3. airborne: record the launch, then track the arc ---------------------------------
			//
			// THE LAUNCH IS THE FIRST AIRBORNE FRAME and nothing later. That is the speed DoJump wrote,
			// which is the number §3 caps; the air top is printed beside it only so a reader can see
			// that the strafe model did not quietly add to it.
			case 3:
				// Released for the WHOLE arc, not just its first frame: the next slide has to start on
				// a press EDGE, and one stray frame of held intent on the way up is enough to make the
				// landing press a no-op and turn a four-hop chain into a one-hop one.
				Move->SetWantsToSlide(false);

				if (!Move->IsMovingOnGround())
				{
					if (!State->bValid[Hop])
					{
						State->Launch[Hop] = Planar;
						State->Ceiling[Hop] = Move->GetSlideJumpChainCeilingForAudit();
						State->bValid[Hop] = true;
						Pawn->StopJumping();
						UE_LOG(LogTraceGame, Display,
							TEXT("AUDITV16 slide chain hop %d: carry %.0f -> launch %.0f uu/s "
							     "(chain depth was %d, ceiling now %.0f)"),
							Hop + 1, State->Carry[Hop], State->Launch[Hop], State->ChainAtPress[Hop],
							State->Ceiling[Hop]);
					}

					State->AirTop[Hop] = FMath::Max(State->AirTop[Hop], Planar);

					// Straight-ahead input only. In this component's Source air model an input aligned
					// with the velocity adds nothing once the projection is saturated, so holding
					// forward keeps the pawn steered without contaminating the number — and it is what
					// a player chaining hops actually does.
					Pawn->AddMovementInput(State->RunDirection, 1.f);
					break;
				}

				if (State->bValid[Hop])
				{
					// Back on the ground. Next hop, or the report.
					++State->HopIndex;
					if (State->HopIndex >= FSlideChainState::MaxHops)
					{
						Move->SetWantsToSlide(false);
						ReportSlideChain(*State);
						return false;
					}
					Advance(1);
					break;
				}

				if (State->PhaseTime > 1.5f)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("AUDITV16 slide chain: hop %d never left the ground."), Hop + 1);
					ReportSlideChain(*State);
					return false;
				}
				break;

			default:
				ReportSlideChain(*State);
				return false;
			}

			return true;
		}));
	}

	// =============================================================================================
	// Registration
	// =============================================================================================

	FAutoConsoleCommand CmdAuditKnobs(
		TEXT("Trace.Move.AuditV16.Knobs"),
		TEXT("Dev only. Print the spec v16 §5 derived-value table (header vs ini vs derived). No pawn needed."),
		FConsoleCommandDelegate::CreateStatic(&RunKnobTable));

	FAutoConsoleCommand CmdAuditCoreKit(
		TEXT("Trace.Move.AuditV16"),
		TEXT("Dev only. Measure the core movement kit on the local pawn — walk, crouch, jump, dash, slide, "
		     "the slide-jump well-timed A/B and the carrier multiplier — and print a table. Run it on a "
		     "CLIENT for the predicted mechanics."),
		FConsoleCommandDelegate::CreateStatic([]() { RunKnobTable(); RunCoreKit(); }));

	FAutoConsoleCommand CmdAuditRipple(
		TEXT("Trace.Move.AuditV16.Ripple"),
		TEXT("Dev only, server only. Measure the Ripple's real velocity for Rocco AND for a rider."),
		FConsoleCommandDelegate::CreateStatic(&RunRipple));

	FAutoConsoleCommand CmdAuditBash(
		TEXT("Trace.Move.AuditV16.Bash"),
		TEXT("Dev only, server only. Measure Chut's bash knock DISTANCE at the old and the new knockback "
		     "speed in one world, and report the speed that would give +25% distance."),
		FConsoleCommandDelegate::CreateStatic(&RunBash));

	FAutoConsoleCommand CmdAuditAbilityMoves(
		TEXT("Trace.Move.AuditV16.Abilities"),
		TEXT("Dev only, server only. Measure Rocco's second jump, Mace's suspend and Oyster's jar-jump."),
		FConsoleCommandDelegate::CreateStatic(&RunAbilityMoves));

	FAutoConsoleCommand CmdAuditSlideChain(
		TEXT("Trace.Move.AuditV16.SlideChain"),
		TEXT("Dev only. SPEC v26 sec 3. Drive the local pawn through FOUR consecutive well-timed "
		     "slide-jumps down a swept lane and print the launch speed of each, so the ceiling can be "
		     "seen rather than asserted. Run it in both arms of Trace.V26LegacySlideJump and diff."),
		FConsoleCommandDelegate::CreateStatic(&RunSlideChain));

	FAutoConsoleCommand CmdAuditRed(
		TEXT("Trace.Move.AuditV16.Red"),
		TEXT("Dev only. The falsification arm: bias every expected value by +12% and re-run the core kit. "
		     "Every row MUST read FAIL."),
		FConsoleCommandDelegate::CreateStatic(&RunRedArm));

}   // namespace TraceMovementAuditV16

#endif // !UE_BUILD_SHIPPING
