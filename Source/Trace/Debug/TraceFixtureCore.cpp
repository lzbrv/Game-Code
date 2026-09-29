// Trace — putting the Core back where a test found it, and keeping it off the local player while a
// test needs his hands. See TraceFixtureCore.h.

#include "Debug/TraceFixtureCore.h"

#if !UE_BUILD_SHIPPING

#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Trace.h"
#include "TraceTypes.h"

namespace TraceFixtureCore
{
	// The one pawn a harness is keeping clear of the Core, and how many holds are on it. A single
	// slot: the only pawn any harness keeps clear is the local player, and two harnesses running at
	// once would both be asking for him. Named, not anonymous — see TraceRailgunVerify.cpp for why.
	namespace KeptClearSlot
	{
		TWeakObjectPtr<ATraceCharacter> GKeptPawn;
		int32 GKeptHolds = 0;
		double GKeptUntilRealTime = 0.0;
	}

	FCoreHolding CaptureCoreHolding(const UWorld* World)
	{
		FCoreHolding Captured;
		const ATraceCore* const TheCore = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
		if (TheCore == nullptr)
		{
			return Captured;
		}

		Captured.bCaptured = true;
		ATraceCharacter* const HeldBy = TheCore->GetCarrier();
		Captured.bWasHeld = (HeldBy != nullptr);
		Captured.Holder = HeldBy;
		Captured.HolderName = GetNameSafe(HeldBy);
		return Captured;
	}

	FString RestoreCoreHolding(UWorld* World, const FCoreHolding& Captured)
	{
		ATraceCore* const TheCore = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
		if (!Captured.bCaptured || TheCore == nullptr || !TheCore->HasAuthority())
		{
			return TEXT("the Core was not restored (there was no Core to capture, or this is not the server)");
		}

		ATraceCharacter* const HeldNow = TheCore->GetCarrier();
		ATraceCharacter* const HeldBefore = Captured.Holder.Get();

		// THE HOLDER GETS IT BACK, if there was one and they are still standing.
		if (Captured.bWasHeld && HeldBefore != nullptr && HeldBefore->IsAlive())
		{
			if (HeldNow != HeldBefore)
			{
				const FString WasOn = (HeldNow != nullptr)
					? FString::Printf(TEXT("it was on %s"), *GetNameSafe(HeldNow))
					: FString(TEXT("it was loose"));
				TheCore->GrantTo(HeldBefore, ETraceCoreGrantReason::Debug);
				return FString::Printf(TEXT("the Core went back to %s, who held it before the test (%s)"),
					*GetNameSafe(HeldBefore), *WasOn);
			}
			return FString::Printf(TEXT("the Core is still with %s, who held it before the test"),
				*GetNameSafe(HeldBefore));
		}

		// NOBODY HELD IT (or its holder has died since), so nobody should hold it now.
		if (HeldNow == nullptr)
		{
			return Captured.bWasHeld
				? FString::Printf(TEXT("the Core is held by nobody; %s, who held it before, is gone"), *Captured.HolderName)
				: FString(TEXT("the Core is held by nobody, as the test found it"));
		}

		const FString TakenFrom = GetNameSafe(HeldNow);
		const FString Before = Captured.bWasHeld
			? FString::Printf(TEXT("%s, who held it before, is gone"), *Captured.HolderName)
			: FString(TEXT("nobody held it before the test"));

		TheCore->KickoffContested(ATraceCore::GetHalfStartCoreSurface(World), /*LockedOutTeam=*/ETraceTeam::None,
			/*FavouredTeam=*/ETraceTeam::None, TEXT("a test fixture putting the Core back"));
		return FString::Printf(TEXT("took the Core off %s and put it on the half-start spot, held by nobody (%s)"),
			*TakenFrom, *Before);
	}

	// ---------------------------------------------------------------------------------------------
	// Keeping a pawn clear of the Core
	// ---------------------------------------------------------------------------------------------

	void KeepClearOfCore(ATraceCharacter* Pawn)
	{
		if (Pawn == nullptr)
		{
			return;
		}

		ATraceCharacter* const AlreadyKept = KeptClearSlot::GKeptPawn.Get();
		if (KeptClearSlot::GKeptHolds > 0 && AlreadyKept != nullptr && AlreadyKept != Pawn)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[FixtureCore] %s was being kept clear of the Core by another harness; %s is kept clear now "
				     "instead. Two harnesses that need different pawns' hands should not run at once."),
				*GetNameSafe(AlreadyKept), *GetNameSafe(Pawn));
			KeptClearSlot::GKeptHolds = 0;
		}

		KeptClearSlot::GKeptPawn = Pawn;
		++KeptClearSlot::GKeptHolds;
		KeptClearSlot::GKeptUntilRealTime = FPlatformTime::Seconds() + KeepClearSafetySeconds;
	}

	void StopKeepingClearOfCore(const ATraceCharacter* Pawn)
	{
		if (KeptClearSlot::GKeptHolds <= 0)
		{
			return;
		}

		// A hold on somebody else is not this caller's to release. A kept pawn that has since been
		// destroyed is anybody's: nothing can be kept off a pawn that no longer exists.
		const ATraceCharacter* const Kept = KeptClearSlot::GKeptPawn.Get();
		if (Pawn != nullptr && Kept != nullptr && Kept != Pawn)
		{
			return;
		}

		if (--KeptClearSlot::GKeptHolds <= 0)
		{
			KeptClearSlot::GKeptHolds = 0;
			KeptClearSlot::GKeptPawn.Reset();
		}
	}

	bool IsKeptClearOfCore(const ATraceCharacter* Character)
	{
		if (KeptClearSlot::GKeptHolds <= 0 || Character == nullptr)
		{
			return false;
		}

		// THE SAFETY LAPSE. Every harness that takes a hold releases it on every exit, but a harness
		// whose world is torn down mid-run never reaches its exit — and a player who can never touch
		// the Core again for the rest of the session is a far worse bug than the one this fixes.
		if (FPlatformTime::Seconds() > KeptClearSlot::GKeptUntilRealTime)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[FixtureCore] %s had been kept clear of the Core for %.0f s with the hold never released; "
				     "the hold has lapsed and they can take the Core again. A harness exited without calling "
				     "EndClearOfCore."),
				*GetNameSafe(KeptClearSlot::GKeptPawn.Get()), KeepClearSafetySeconds);
			KeptClearSlot::GKeptHolds = 0;
			KeptClearSlot::GKeptPawn.Reset();
			return false;
		}

		return KeptClearSlot::GKeptPawn.Get() == Character;
	}

	FString BeginClearOfCore(UWorld* World, ATraceCharacter* Pawn, FClearOfCore& OutClear)
	{
		OutClear = FClearOfCore();
		OutClear.bBegun = true;
		OutClear.ClearWorld = World;
		OutClear.ClearPawn = Pawn;
		OutClear.Before = CaptureCoreHolding(World);

		ATraceCore* const TheCore = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
		if (Pawn == nullptr || TheCore == nullptr || !TheCore->HasAuthority())
		{
			return TEXT("nothing done: there is no pawn, no Core, or this is not the server");
		}

		// Kept clear FIRST, so the pickup poll on the very next tick cannot hand it straight back —
		// the half-start spot is where the local player spawns in some layouts.
		KeepClearOfCore(Pawn);
		OutClear.bKeeping = true;

		if (TheCore->GetCarrier() != Pawn)
		{
			const FString Where = OutClear.Before.bWasHeld
				? FString::Printf(TEXT("%s has it"), *OutClear.Before.HolderName)
				: FString(TEXT("nobody has it"));
			return FString::Printf(TEXT("%s is not carrying (%s); kept clear of the Core until the test ends"),
				*GetNameSafe(Pawn), *Where);
		}

		TheCore->KickoffContested(ATraceCore::GetHalfStartCoreSurface(World), /*LockedOutTeam=*/ETraceTeam::None,
			/*FavouredTeam=*/ETraceTeam::None, TEXT("a test fixture freeing the local player's hands"));
		OutClear.bTookItOff = true;
		return FString::Printf(TEXT("took the Core off %s and put it on the half-start spot, held by nobody; "
		                            "kept clear of it until the test ends"),
			*GetNameSafe(Pawn));
	}

	FString EndClearOfCore(FClearOfCore& InOutClear)
	{
		if (!InOutClear.bBegun)
		{
			return TEXT("nothing to put back: this test never freed anybody's hands");
		}
		if (InOutClear.bEnded)
		{
			return TEXT("already put back");
		}
		InOutClear.bEnded = true;

		ATraceCharacter* const Kept = InOutClear.ClearPawn.Get();
		if (InOutClear.bKeeping)
		{
			StopKeepingClearOfCore(Kept);
			InOutClear.bKeeping = false;
		}

		if (!InOutClear.bTookItOff)
		{
			return FString::Printf(TEXT("%s is no longer kept clear; the Core was not touched (they were not "
			                            "carrying), so it stays where play has it"),
				*GetNameSafe(Kept));
		}

		return FString::Printf(TEXT("%s is no longer kept clear; %s"),
			*GetNameSafe(Kept), *RestoreCoreHolding(InOutClear.ClearWorld.Get(), InOutClear.Before));
	}
}

// =================================================================================================
// Trace.FixtureCore.KeepClearTest — the switch under BeginClearOfCore, proved against a control.
//
// It parks the Core, loose and at rest, AT THE LOCAL PLAYER'S FEET — a placement the pickup poll
// cannot refuse him — twice, a second each:
//
//   CONTROL  nobody kept clear. He must be carrying it by the end of the second, or the staging
//            never reached the pickup poll and the next arm proves nothing (verdict INVALID).
//   KEPT     KeepClearOfCore(him) first. He must NOT be carrying it, and the Core must still be lying
//            where it was put — so "he did not take it" is the switch's doing and not the ball having
//            gone somewhere else (INCONCLUSIVE if a bot got there first).
//
// Then the Core goes on the half-start spot while he is still kept clear (released at his feet, he
// would pick it up on the next tick), the hold is released, and possession goes back to whoever had
// it before the test. Authority only: run it in a standalone or listen-server match.
// =================================================================================================

namespace TraceFixtureCoreKeepClearTest
{
	/** Real seconds each arm leaves the Core at his feet before it looks: dozens of pickup polls. */
	constexpr double ArmSeconds = 1.0;

	/** How long it waits for a living local pawn and a Core before it gives up with INVALID. */
	constexpr double WaitForPawnSeconds = 45.0;

	/** "Still lying where it was put": a resting Core does not move, so this is generous. */
	constexpr double LayTolerance = 10.0;

	struct FRun
	{
		int32 Step = 0;
		double NextAt = 0.0;
		double GiveUpAt = 0.0;

		TWeakObjectPtr<UWorld> RunWorld;
		TWeakObjectPtr<ATraceCharacter> Subject;
		TraceFixtureCore::FCoreHolding BeforeTest;
		FVector ParkedAt = FVector::ZeroVector;
		bool bKeptHeld = false;

		bool bControlTook = false;
		FString ControlHolder;
	};

	UWorld* FindServerWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetNetMode() != NM_Client)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	ATraceCharacter* FindLocalPawn(UWorld* InWorld)
	{
		if (InWorld == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = InWorld->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* Controller = It->Get();
			if (Controller != nullptr && Controller->IsLocalController())
			{
				return Cast<ATraceCharacter>(Controller->GetPawn());
			}
		}
		return nullptr;
	}

	/** The floor under his capsule: the surface KickoffContested wants, which adds the ball's radius. */
	FVector FeetOf(const ATraceCharacter* Standing)
	{
		const UCapsuleComponent* Capsule = Standing->GetCapsuleComponent();
		const double HalfHeight = (Capsule != nullptr) ? static_cast<double>(Capsule->GetScaledCapsuleHalfHeight()) : 0.0;
		return Standing->GetActorLocation() - FVector(0.0, 0.0, HalfHeight);
	}

	/** Parks the Core at rest on @p Surface. Returns where it came to rest. */
	FVector ParkCore(ATraceCore* TheCore, const FVector& Surface, const TCHAR* Why)
	{
		TheCore->KickoffContested(Surface, ETraceTeam::None, ETraceTeam::None, Why);
		return TheCore->GetActorLocation();
	}

	/** Every exit after the Core was first moved: off his feet, hold released, possession put back. */
	void CleanUp(FRun& Ran, UWorld* InWorld, ATraceCore* TheCore)
	{
		if (TheCore != nullptr && InWorld != nullptr)
		{
			// While he is still kept clear: released with the Core at his feet, he would take it on the
			// next tick and the restore below would be describing a state that no longer exists.
			ParkCore(TheCore, ATraceCore::GetHalfStartCoreSurface(InWorld),
				TEXT("Trace.FixtureCore.KeepClearTest, clearing up"));
		}
		if (Ran.bKeptHeld)
		{
			TraceFixtureCore::StopKeepingClearOfCore(Ran.Subject.Get());
			Ran.bKeptHeld = false;
		}
		UE_LOG(LogTraceGame, Display, TEXT("[KeepClear] restored: %s."),
			*TraceFixtureCore::RestoreCoreHolding(InWorld, Ran.BeforeTest));
	}

	void Run()
	{
		TSharedRef<FRun> State = MakeShared<FRun>();
		State->GiveUpAt = FPlatformTime::Seconds() + WaitForPawnSeconds;

		UE_LOG(LogTraceGame, Display,
			TEXT("[KeepClear] ===== a harness that needs the local player's hands keeps the Core off him "
			     "(TraceFixtureCore). CONTROL: a Core parked at his feet must be his within %.1f s. KEPT: kept "
			     "clear, the same Core must lie there for %.1f s and not be. ====="),
			ArmSeconds, ArmSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State](float) -> bool
		{
			const double Now = FPlatformTime::Seconds();
			if (Now < State->NextAt)
			{
				return true;
			}

			UWorld* TickWorld = (State->Step == 0) ? FindServerWorld() : State->RunWorld.Get();
			ATraceCore* TheCore = (TickWorld != nullptr) ? ATraceCore::Get(TickWorld) : nullptr;
			ATraceCharacter* Local = (State->Step == 0) ? FindLocalPawn(TickWorld) : State->Subject.Get();

			if (State->Step == 0)
			{
				if (TheCore == nullptr || !TheCore->HasAuthority() || Local == nullptr || !Local->IsAlive())
				{
					if (Now > State->GiveUpAt)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[KeepClear] VERDICT: INVALID — after %.0f s there is still no Core on a server "
							     "and a living local pawn to park it on. Nothing was moved."),
							WaitForPawnSeconds);
						return false;
					}
					return true;
				}

				if (TraceFixtureCore::IsKeptClearOfCore(Local))
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[KeepClear] VERDICT: INVALID — another harness is keeping %s clear of the Core right "
						     "now, so the control arm cannot run. Run this on its own. Nothing was moved."),
						*GetNameSafe(Local));
					return false;
				}

				State->RunWorld = TickWorld;
				State->Subject = Local;
				State->BeforeTest = TraceFixtureCore::CaptureCoreHolding(TickWorld);

				State->ParkedAt = ParkCore(TheCore, FeetOf(Local),
					TEXT("Trace.FixtureCore.KeepClearTest, control arm: at the local player's feet"));
				UE_LOG(LogTraceGame, Display,
					TEXT("[KeepClear] CONTROL: the Core is at rest at %s, at %s's feet, and nobody is kept clear."),
					*State->ParkedAt.ToCompactString(), *GetNameSafe(Local));

				State->Step = 1;
				State->NextAt = Now + ArmSeconds;
				return true;
			}

			// From here on the Core has been moved, so every exit goes through CleanUp.
			if (TickWorld == nullptr || TheCore == nullptr || Local == nullptr || !Local->IsAlive())
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[KeepClear] VERDICT: INVALID — the world, the Core or the local pawn went away "
					     "mid-test (a bot may have killed him)."));
				CleanUp(*State, TickWorld, TheCore);
				return false;
			}

			if (State->Step == 1)
			{
				State->bControlTook = (TheCore->GetCarrier() == Local);
				State->ControlHolder = GetNameSafe(TheCore->GetCarrier());
				UE_LOG(LogTraceGame, Display, TEXT("[KeepClear] CONTROL after %.1f s: %s is carrying=%d (holder %s)."),
					ArmSeconds, *GetNameSafe(Local), State->bControlTook ? 1 : 0, *State->ControlHolder);

				TraceFixtureCore::KeepClearOfCore(Local);
				State->bKeptHeld = true;

				// KickoffContested takes it off him first (he should be holding it after the control).
				State->ParkedAt = ParkCore(TheCore, FeetOf(Local),
					TEXT("Trace.FixtureCore.KeepClearTest, kept arm: at the local player's feet"));
				UE_LOG(LogTraceGame, Display,
					TEXT("[KeepClear] KEPT: %s is kept clear; the Core is at rest at %s, at his feet."),
					*GetNameSafe(Local), *State->ParkedAt.ToCompactString());

				State->Step = 2;
				State->NextAt = Now + ArmSeconds;
				return true;
			}

			const ATraceCharacter* const HeldBy = TheCore->GetCarrier();
			const bool bKeptTook = (HeldBy == Local);
			const double Moved = FVector::Dist(TheCore->GetActorLocation(), State->ParkedAt);
			const bool bLayThere = TheCore->IsLoose() && HeldBy == nullptr && Moved <= LayTolerance;
			UE_LOG(LogTraceGame, Display,
				TEXT("[KeepClear] KEPT after %.1f s: %s is carrying=%d | the Core is loose=%d, holder %s, %.1f uu "
				     "from where it was put."),
				ArmSeconds, *GetNameSafe(Local), bKeptTook ? 1 : 0, TheCore->IsLoose() ? 1 : 0,
				*GetNameSafe(HeldBy), Moved);

			CleanUp(*State, TickWorld, TheCore);

			if (!State->bControlTook)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[KeepClear] VERDICT: INVALID — the CONTROL failed: a Core parked at his feet with nobody "
					     "kept clear was not his (holder %s). The staging never reached the pickup poll, so the "
					     "kept arm proves nothing."),
					*State->ControlHolder);
			}
			else if (bKeptTook)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[KeepClear] VERDICT: *** FAIL *** — kept clear, he still took the Core at his feet. "
					     "ATraceCore::GatherCharacters is not leaving him out."));
			}
			else if (!bLayThere)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[KeepClear] VERDICT: INCONCLUSIVE — he did not take it, but the Core did not stay where "
					     "it was put (holder %s, moved %.1f uu), so that is not proof of the switch. Run it again."),
					*GetNameSafe(HeldBy), Moved);
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[KeepClear] VERDICT: PASS — with nobody kept clear he took a Core parked at his feet; "
					     "kept clear, the same Core lay at his feet for %.1f s and he did not."),
					ArmSeconds);
			}
			return false;
		}));
	}

	FAutoConsoleCommand CmdKeepClearTest(
		TEXT("Trace.FixtureCore.KeepClearTest"),
		TEXT("Dev only. Proves the switch harnesses use to keep the Core off the local player while they "
		     "drive his weapon: parks the Core at his feet with nobody kept clear (he must take it), then "
		     "kept clear (he must not). Puts the Core back as it found it."),
		FConsoleCommandDelegate::CreateStatic(&Run));
}

#endif // !UE_BUILD_SHIPPING
