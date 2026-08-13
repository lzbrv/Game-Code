// =================================================================================================
// TraceVerifV19Probe.cpp — TEMPORARY VERIFICATION SCAFFOLDING (adversarial verifier, spec v19 pass).
//
// NOT PRODUCT CODE. Nothing in here is called by gameplay; it only reads. DELETE THIS FILE when the
// v19 verification pass is closed.
//
// It exists because the shipped harnesses for two of v19's claims do not exercise the code they
// claim to prove:
//   * Trace.Mortimer.Verify prints "Mortimer may hold 1.20s for x1.85" by RE-DERIVING the formula
//     from UTraceSettings. It never calls ATraceCore::GetThrowChargeScaleForHold, so it would print
//     the identical line on a build where the thrower argument was never wired in.
//   * Trace.Move.AuditV16 measures a dash but compares it to the GLOBAL expectation, so a
//     per-character scale reads as a FAIL rather than as a measurement.
//
// Every command here is compiled out of Shipping.
// =================================================================================================

#if !UE_BUILD_SHIPPING

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"

// Named after the file, per the Windows jumbo-build rule.
namespace TraceVerifV19Probe
{
	static ATraceCharacter* FindLocalPawn(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (APlayerController* PC = It->Get())
			{
				if (ATraceCharacter* Pawn = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					return Pawn;
				}
			}
		}
		return nullptr;
	}

	static ATraceCore* FindCore(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		TActorIterator<ATraceCore> It(World);
		return It ? *It : nullptr;
	}

	/**
	 * Trace.Verif.ThrowProbe [holdSeconds]
	 *
	 * Prints the charge scale THE REAL THROW WOULD USE, straight out of the shipped function with the
	 * local pawn as the thrower, alongside the same hold with a null thrower (= everybody else).
	 * Then stages an actual ATraceCore::ThrowFromHolder at that hold so the shipped
	 * "[ModeB] THROW ... charge x" line is produced by the real launch path.
	 */
	static void CmdThrowProbe(const TArray<FString>& Args, UWorld* World)
	{
		ATraceCharacter* Pawn = FindLocalPawn(World);
		ATraceCore* Core = FindCore(World);
		const float Hold = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 1.2f;

		if (Pawn == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[VERIFV19] ThrowProbe: no local pawn."));
			return;
		}

		const float Mine   = ATraceCore::GetThrowChargeScaleForHold(Hold, Pawn);
		const float Anyone = ATraceCore::GetThrowChargeScaleForHold(Hold, nullptr);
		const float MineAtFull   = ATraceCore::GetThrowChargeScaleForHold(0.6f, Pawn);
		const float AnyoneAtFull = ATraceCore::GetThrowChargeScaleForHold(0.6f, nullptr);
		const float InverseAt1   = ATraceCore::GetThrowHoldSecondsForScale(Mine);

		UE_LOG(LogTraceGame, Display,
			TEXT("[VERIFV19] THROWSCALE pawn=%s hold=%.2fs | THIS PAWN x%.4f | null-thrower x%.4f | ")
			TEXT("at 0.60s: this x%.4f vs null x%.4f | inverse(x%.4f) = %.3fs"),
			*GetNameSafe(Pawn), Hold, Mine, Anyone, MineAtFull, AnyoneAtFull, Mine, InverseAt1);

		// Sweep, so the SHAPE is visible and not just one point.
		for (int32 Step = 0; Step <= 8; ++Step)
		{
			const float T = 0.3f * static_cast<float>(Step);
			UE_LOG(LogTraceGame, Display,
				TEXT("[VERIFV19] THROWSWEEP hold=%.2fs -> this pawn x%.4f | everybody else x%.4f"),
				T,
				ATraceCore::GetThrowChargeScaleForHold(T, Pawn),
				ATraceCore::GetThrowChargeScaleForHold(T, nullptr));
		}

		if (Core != nullptr && Pawn->HasAuthority())
		{
			Core->GrantTo(Pawn, ETraceCoreGrantReason::Debug);
			const bool bThrown = Core->ThrowFromHolder(Pawn, Hold);
			UE_LOG(LogTraceGame, Display,
				TEXT("[VERIFV19] REAL THROW staged at hold=%.2fs, ThrowFromHolder returned %d — read the ")
				TEXT("shipped \"[ModeB] THROW ...\" line above/below for the charge it actually used."),
				Hold, bThrown ? 1 : 0);
		}
	}

	/**
	 * Trace.Verif.PawnProbe — the per-pawn numbers v19 §3 moves, read off the LIVE pawn rather than
	 * off the settings table: max health, dash speed/reach, dash charge ceiling, wall-jump retention.
	 */
	static void CmdPawnProbe(const TArray<FString>& /*Args*/, UWorld* World)
	{
		ATraceCharacter* Pawn = FindLocalPawn(World);
		if (Pawn == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[VERIFV19] PawnProbe: no local pawn."));
			return;
		}

		UTraceCharacterMovementComponent* Move = Pawn->GetTraceMovement();
		UTraceHealthComponent* HealthComp = Pawn->FindComponentByClass<UTraceHealthComponent>();
		const UTraceSettings& S = UTraceSettings::Get();

		// Health, replicated and public: a pawn is spawned at exactly its max, so the live float IS the
		// character's max health until something hits it.
		const float LiveHealth = (HealthComp != nullptr) ? HealthComp->Health : -1.f;
		const int32 Charges    = (Move != nullptr) ? Move->GetMaxDashCharges() : -1;

		UE_LOG(LogTraceGame, Display,
			TEXT("[VERIFV19] PAWNPROBE pawn=%s carrier=%d | health-at-spawn %.1f (global knob %.1f, ")
			TEXT("per-character override %.1f) | maxDashCharges %d | dashDistanceScale %.4f | ")
			TEXT("wallJumpScale %.4f (global retention knob %.4f) | throwHoldScale %.4f"),
			*GetNameSafe(Pawn), UTraceAbilityComponent::IsCarrier(Pawn) ? 1 : 0,
			LiveHealth, S.MaxHealth, TraceAbilityTraits::GetMaxHealthOverride(Pawn),
			Charges,
			TraceAbilityTraits::GetDashDistanceScale(Pawn),
			TraceAbilityTraits::GetWallJumpMomentumScale(Pawn), S.WallJumpSpeedRetention,
			TraceAbilityTraits::GetThrowChargeHoldScale(Pawn));

		// EVERY OTHER PAWN IN THE WORLD, so "hers alone" is a comparison and not an assertion.
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Other = *It;
			if (Other == nullptr || Other == Pawn)
			{
				continue;
			}
			UTraceCharacterMovementComponent* OtherMove = Other->GetTraceMovement();
			UTraceHealthComponent* OtherHealth = Other->FindComponentByClass<UTraceHealthComponent>();
			UE_LOG(LogTraceGame, Display,
				TEXT("[VERIFV19] PAWNPROBE other=%s | health %.1f | charges %d | dashScale %.4f | ")
				TEXT("wallJumpScale %.4f | throwHoldScale %.4f"),
				*GetNameSafe(Other),
				(OtherHealth != nullptr) ? OtherHealth->Health : -1.f,
				(OtherMove != nullptr) ? OtherMove->GetMaxDashCharges() : -1,
				TraceAbilityTraits::GetDashDistanceScale(Other),
				TraceAbilityTraits::GetWallJumpMomentumScale(Other),
				TraceAbilityTraits::GetThrowChargeHoldScale(Other));
		}
	}

	/** Trace.Verif.GrantCore — put the Core in the local pawn's hands, so posture rules can be probed. */
	static void CmdGrantCore(const TArray<FString>& /*Args*/, UWorld* World)
	{
		ATraceCharacter* Pawn = FindLocalPawn(World);
		ATraceCore* Core = FindCore(World);
		if (Pawn == nullptr || Core == nullptr || !Pawn->HasAuthority())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[VERIFV19] GrantCore: no pawn/core/authority."));
			return;
		}
		Core->GrantTo(Pawn, ETraceCoreGrantReason::Debug);
		UE_LOG(LogTraceGame, Display, TEXT("[VERIFV19] GrantCore: carrier=%d"),
			UTraceAbilityComponent::IsCarrier(Pawn) ? 1 : 0);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdThrowProbeReg(
		TEXT("Trace.Verif.ThrowProbe"),
		TEXT("TEMP verification scaffolding: real charge scale for the local pawn vs everybody else."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World) { CmdThrowProbe(Args, World); }));

	static FAutoConsoleCommandWithWorldAndArgs CmdPawnProbeReg(
		TEXT("Trace.Verif.PawnProbe"),
		TEXT("TEMP verification scaffolding: per-pawn health/dash/wall-jump numbers, live."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World) { CmdPawnProbe(Args, World); }));

	static FAutoConsoleCommandWithWorldAndArgs CmdGrantCoreReg(
		TEXT("Trace.Verif.GrantCore"),
		TEXT("TEMP verification scaffolding: grant the Core to the local pawn."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World) { CmdGrantCore(Args, World); }));
}

#endif // !UE_BUILD_SHIPPING
