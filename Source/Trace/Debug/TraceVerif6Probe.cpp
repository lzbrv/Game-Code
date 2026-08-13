// =================================================================================================
// TraceVerif6Probe.cpp — TEMPORARY VERIFICATION SCAFFOLDING (adversarial verifier #6, spec v19).
//
// NOT PRODUCT CODE. Nothing here is called by gameplay; it stages and it reads. DELETE when the v19
// verification pass closes.
//
// Why it exists: the shipped v19 harnesses either re-derive a formula from UTraceSettings instead of
// calling the shipped function (Trace.Mortimer.Verify) or compare a per-character dash against the
// GLOBAL expectation (Trace.Move.AuditV16), so neither can distinguish "Mortimer's scale is wired
// into GetDashSpeed" from "the knob exists and nothing reads it". This measures the pawn actually
// moving.
// =================================================================================================

#if !UE_BUILD_SHIPPING

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"

// Named after the file, per the Windows jumbo-build rule.
namespace TraceVerif6Probe
{
	static UWorld* AuthWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() != nullptr && Context.World()->IsGameWorld()
				&& Context.World()->GetAuthGameMode() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	static ATraceCharacter* HumanPawn(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = Cast<APlayerController>(It->Get());
			if (PC == nullptr)
			{
				continue;
			}
			ATraceCharacter* Pawn = Cast<ATraceCharacter>(PC->GetPawn());
			if (Pawn == nullptr)
			{
				continue;
			}
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Pawn);
			if (Comp != nullptr && Comp->IsBot())
			{
				continue;
			}
			return Pawn;
		}
		return nullptr;
	}

	/** Trace.V6.Become <charId 0..10> — the same ServerSetCharacter the game mode uses. */
	static void CmdBecome(const TArray<FString>& Args)
	{
		UWorld* World = AuthWorld();
		ATraceCharacter* Pawn = HumanPawn(World);
		const int32 Wanted = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 9;
		if (Pawn == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V6] Become: no human pawn."));
			return;
		}
		UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Pawn);
		if (Comp == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V6] Become: no ability component."));
			return;
		}
		Comp->ServerSetCharacter(static_cast<ETraceCharacterId>(Wanted));
		UE_LOG(LogTraceGame, Display, TEXT("[V6] Become(%d) -> pawn=%s is now %s (charactersEnabled=%d)"),
			Wanted, *GetNameSafe(Pawn), TraceCharacterIdToString(Comp->GetCharacterId()),
			UTraceAbilityComponent::AreCharactersEnabled(World) ? 1 : 0);
	}

	// ---------------------------------------------------------------------------------------------
	// THE REAL DASH MEASUREMENT. StartDash() is the exact call a key press makes; the pawn's own
	// world position is sampled every frame until it stops moving, so the number reported is the
	// ground the player actually covers and not DashSpeed x DashDuration read back out of the table.
	// ---------------------------------------------------------------------------------------------
	struct FDashRun
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;
		FVector Start = FVector::ZeroVector;
		float   Elapsed = 0.f;
		float   Peak = 0.f;
		float   PeakSpeed = 0.f;
		/** Displacement at the moment the dash's own clock runs out — the reach, without the carry. */
		float   AtDashEnd = -1.f;
		FString Who;
		float   Scale = 1.f;
		float   ConfiguredSpeed = 0.f;
	};

	static void CmdDashMeasure(const TArray<FString>& /*Args*/)
	{
		UWorld* World = AuthWorld();
		ATraceCharacter* Pawn = HumanPawn(World);
		UTraceCharacterMovementComponent* Move = (Pawn != nullptr) ? Pawn->GetTraceMovement() : nullptr;
		if (Pawn == nullptr || Move == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V6] DashMeasure: no pawn/movement."));
			return;
		}

		UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Pawn);

		TSharedPtr<FDashRun> Run = MakeShared<FDashRun>();
		Run->Pawn  = Pawn;
		Run->Start = Pawn->GetActorLocation();
		Run->Who   = (Comp != nullptr) ? FString(TraceCharacterIdToString(Comp->GetCharacterId())) : TEXT("<none>");
		// GetDashSpeed()/GetDashDuration() are protected on the component, so this scaffolding does NOT
		// read them — which is the better arrangement anyway: everything below is the pawn's own
		// position and velocity, so a knob that is declared and never read cannot fake a pass.
		Run->Scale = TraceAbilityTraits::GetDashDistanceScale(Pawn);
		Run->ConfiguredSpeed = UTraceSettings::Get().DashSpeed * Run->Scale;

		UE_LOG(LogTraceGame, Display,
			TEXT("[V6] DASHMEASURE begin: who=%s global DashSpeed knob %.1f uu/s x trait %.4f = %.1f uu/s "
			     "expected; global DashDuration %.3fs -> if the trait is wired, reach should be %.1f uu"),
			*Run->Who, UTraceSettings::Get().DashSpeed, Run->Scale, Run->ConfiguredSpeed,
			UTraceSettings::Get().DashDuration,
			Run->ConfiguredSpeed * UTraceSettings::Get().DashDuration);

		Move->StartDash();

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float Delta) -> bool
			{
				ATraceCharacter* P = Run->Pawn.Get();
				if (P == nullptr)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[V6] DASHMEASURE aborted: pawn gone."));
					return false;
				}
				Run->Elapsed += Delta;
				const float Travelled = static_cast<float>(
					FVector::Dist2D(P->GetActorLocation(), Run->Start));
				Run->Peak = FMath::Max(Run->Peak, Travelled);
				Run->PeakSpeed = FMath::Max(Run->PeakSpeed, static_cast<float>(P->GetVelocity().Size2D()));

				if (Run->AtDashEnd < 0.f && Run->Elapsed >= UTraceSettings::Get().DashDuration)
				{
					Run->AtDashEnd = Travelled;
				}

				if (Run->Elapsed < 0.75f)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[V6] DASHMEASURE result: who=%s | MEASURED reach at the dash's end (%.3fs) %.1f uu "
					     "| total displacement including the carry-out %.1f uu | peak planar speed %.1f uu/s "
					     "| expected speed %.1f uu/s (trait x%.4f) | global-only prediction %.1f uu"),
					*Run->Who, UTraceSettings::Get().DashDuration, Run->AtDashEnd, Run->Peak, Run->PeakSpeed,
					Run->ConfiguredSpeed, Run->Scale,
					UTraceSettings::Get().DashSpeed * UTraceSettings::Get().DashDuration);
				return false;
			}), 0.f);
	}

	/** Trace.V6.Posture — Mortimer's E gate, asked directly, in all four staged postures. */
	static void CmdPosture(const TArray<FString>& /*Args*/)
	{
		UWorld* World = AuthWorld();
		ATraceCharacter* Pawn = HumanPawn(World);
		UTraceAbilityComponent* Comp = (Pawn != nullptr) ? UTraceAbilityComponent::Get(Pawn) : nullptr;
		if (Pawn == nullptr || Comp == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V6] Posture: no pawn."));
			return;
		}
		ATraceCore* Core = ATraceCore::Get(World);
		UTraceCharacterMovementComponent* Move = Pawn->GetTraceMovement();

		auto Report = [&](const TCHAR* Stage)
		{
			FText Why;
			UTraceCharacterAbilitySet* Set = Comp->GetAbilitySet();
			const bool bCan = (Set != nullptr) ? Set->CanActivate(Why) : false;
			// And the REAL press, through the same door a key press uses, so a gate that only exists
			// in CanActivate() and is not consulted by TryActivate() shows up as a disagreement.
			const bool bFired = Comp->TryActivate();
			UE_LOG(LogTraceGame, Display,
				TEXT("[V6] POSTURE %s: character=%s carrier=%d grounded(engine)=%d | CanActivate=%d reason='%s' "
				     "| TryActivate (the real E press) FIRED=%d"),
				Stage, TraceCharacterIdToString(Comp->GetCharacterId()),
				UTraceAbilityComponent::IsCarrier(Pawn) ? 1 : 0,
				(Move != nullptr && Move->IsMovingOnGround()) ? 1 : 0,
				bCan ? 1 : 0, *Why.ToString(), bFired ? 1 : 0);
			// Clear the cooldown a successful press just started, so the next stage is not refused
			// for a reason that has nothing to do with posture.
			Comp->OnHalfTime();
		};

		// 1. no Core, on the ground.
		if (Core != nullptr)
		{
			for (TActorIterator<ATraceCharacter> It(World); It; ++It)
			{
				if (*It != nullptr && *It != Pawn && (*It)->IsAlive()) { Core->TryPickup(*It); break; }
			}
		}
		Report(TEXT("no-core / grounded"));

		// 2. Core, on the ground.
		if (Core != nullptr)
		{
			Core->TryPickup(Pawn);
		}
		Report(TEXT("CORE / grounded"));

		// 3. Core, airborne — AND NOT IN THE SAME FRAME.
		//
		// The gate is IsGroundedForAbilities(), which is IsMovingOnGround() OR a LedgeGroundGraceSeconds
		// (0.08 s) hysteresis window. A press issued on the frame of the teleport therefore lands
		// INSIDE the grace and is allowed — that is the harness reading its own staging, not the rule.
		// The first run of this probe made exactly that mistake, so the press is deferred well past
		// the grace and the airborne state is re-read at the moment of the press.
		if (Move != nullptr)
		{
			Pawn->SetActorLocation(Pawn->GetActorLocation() + FVector(0.f, 0.f, 500.f), false, nullptr,
				ETeleportType::TeleportPhysics);
			Move->SetMovementMode(MOVE_Falling);
			UE_LOG(LogTraceGame, Display,
				TEXT("[V6] POSTURE staging airborne: hoisted 500 uu, MOVE_Falling, grace %.3fs — press deferred."),
				UTraceSettings::Get().LedgeGroundGraceSeconds);
		}

		TWeakObjectPtr<ATraceCharacter> WeakPawn(Pawn);
		TWeakObjectPtr<UTraceAbilityComponent> WeakComp(Comp);
		TSharedPtr<float> Waited = MakeShared<float>(0.f);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakPawn, WeakComp, Waited](float Delta) -> bool
			{
				ATraceCharacter* P = WeakPawn.Get();
				UTraceAbilityComponent* C = WeakComp.Get();
				if (P == nullptr || C == nullptr)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[V6] POSTURE airborne: pawn gone."));
					return false;
				}
				*Waited += Delta;
				if (*Waited < 0.35f)
				{
					return true;
				}
				UTraceCharacterMovementComponent* M = P->GetTraceMovement();
				FText Why;
				UTraceCharacterAbilitySet* Set = C->GetAbilitySet();
				const bool bCan = (Set != nullptr) ? Set->CanActivate(Why) : false;
				const bool bFired = C->TryActivate();
				UE_LOG(LogTraceGame, Display,
					TEXT("[V6] POSTURE CORE / AIRBORNE (+%.2fs after the hoist): character=%s carrier=%d "
					     "grounded(engine)=%d groundedForAbilities=%d | CanActivate=%d reason='%s' "
					     "| TryActivate (the real E press) FIRED=%d"),
					*Waited, TraceCharacterIdToString(C->GetCharacterId()),
					UTraceAbilityComponent::IsCarrier(P) ? 1 : 0,
					(M != nullptr && M->IsMovingOnGround()) ? 1 : 0,
					(M != nullptr && M->IsGroundedForAbilities()) ? 1 : 0,
					bCan ? 1 : 0, *Why.ToString(), bFired ? 1 : 0);
				C->OnHalfTime();
				return false;
			}), 0.f);
	}

	/**
	 * Trace.V6.After <seconds> <console command with spaces>
	 *
	 * -ExecCmds fires at engine init, before the map has a pawn, and it splits on commas so a batch
	 * cannot sequence itself. This defers ONE command by wall time, so a battery can be written as
	 * several -ExecCmds entries with increasing delays and still run against a live pawn.
	 */
	static void CmdAfter(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V6] After: usage Trace.V6.After <seconds> <command...>"));
			return;
		}
		const float Delay = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 600.f);
		FString Command;
		for (int32 Index = 1; Index < Args.Num(); ++Index)
		{
			Command += (Index > 1) ? TEXT(" ") : TEXT("");
			Command += Args[Index];
		}
		UE_LOG(LogTraceGame, Display, TEXT("[V6] After: '%s' queued for +%.1fs."), *Command, Delay);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Command](float /*Delta*/) -> bool
			{
				UWorld* World = AuthWorld();
				UE_LOG(LogTraceGame, Display, TEXT("[V6] After: firing '%s' (world=%d)"),
					*Command, World != nullptr ? 1 : 0);
				GEngine->Exec(World, *Command);
				return false;
			}), Delay);
	}

	/**
	 * Trace.V6.RealThrow <holdSeconds>
	 *
	 * The other probe's ThrowFromHolder returned 0 because it fired on the same frame as the grant,
	 * inside the Core's own post-grant throw cooldown. This grants, waits, and then throws — so the
	 * number reported is produced by the SHIPPED server-side launch path and not by calling the
	 * charge formula on its own.
	 */
	static void CmdRealThrow(const TArray<FString>& Args)
	{
		UWorld* World = AuthWorld();
		ATraceCharacter* Pawn = HumanPawn(World);
		ATraceCore* Core = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
		const float Hold = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 1.2f;
		if (Pawn == nullptr || Core == nullptr || !Pawn->HasAuthority())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V6] RealThrow: no pawn/core/authority."));
			return;
		}
		Core->TryPickup(Pawn);
		UE_LOG(LogTraceGame, Display, TEXT("[V6] RealThrow: Core granted, waiting out the throw cooldown."));

		TWeakObjectPtr<ATraceCharacter> WeakPawn(Pawn);
		TWeakObjectPtr<ATraceCore> WeakCore(Core);
		TSharedPtr<float> Waited = MakeShared<float>(0.f);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakPawn, WeakCore, Waited, Hold](float Delta) -> bool
			{
				ATraceCharacter* P = WeakPawn.Get();
				ATraceCore* C = WeakCore.Get();
				if (P == nullptr || C == nullptr)
				{
					return false;
				}
				*Waited += Delta;
				if (*Waited < 2.0f)
				{
					return true;
				}
				const bool bThrown = C->ThrowFromHolder(P, Hold);
				UE_LOG(LogTraceGame, Display,
					TEXT("[V6] REALTHROW hold=%.2fs ThrowFromHolder=%d — the shipped \"[ModeB] THROW\" line "
					     "beside this one carries the charge the real launch used."),
					Hold, bThrown ? 1 : 0);
				return false;
			}), 0.f);
	}

	static FAutoConsoleCommand CmdRealThrowReg(
		TEXT("Trace.V6.RealThrow"),
		TEXT("TEMP scaffolding: a real server-side Core throw at a given hold."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdRealThrow));

	static FAutoConsoleCommand CmdAfterReg(
		TEXT("Trace.V6.After"),
		TEXT("TEMP scaffolding: run a console command after N seconds of wall time."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdAfter));

	static FAutoConsoleCommand CmdBecomeReg(
		TEXT("Trace.V6.Become"),
		TEXT("TEMP scaffolding: make the local human this character id."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdBecome));

	static FAutoConsoleCommand CmdDashMeasureReg(
		TEXT("Trace.V6.DashMeasure"),
		TEXT("TEMP scaffolding: StartDash() and measure the ground actually covered."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdDashMeasure));

	static FAutoConsoleCommand CmdPostureReg(
		TEXT("Trace.V6.Posture"),
		TEXT("TEMP scaffolding: ask the ability gate in three staged postures."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdPosture));
}

#endif // !UE_BUILD_SHIPPING
