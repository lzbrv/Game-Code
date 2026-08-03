// Trace — synthetic input harness. See TraceInputHarness.h for what this is and why.

#include "Debug/TraceInputHarness.h"

// The harness is a test instrument, not a feature. Compiling it out of Shipping keeps a console
// command that can move the local pawn and pull the trigger out of a build players can run.
#if !UE_BUILD_SHIPPING

/**
 * Set to 1 ONLY after adding "Slate", "SlateCore" and "ApplicationCore" to
 * PublicDependencyModuleNames in Trace.Build.cs. Without them the Slate path below compiles and
 * then fails to link — FSlateApplication::CurrentApplication, ProcessKeyDownEvent,
 * ProcessMouseButtonDownEvent, the FKeyEvent constructor and the FInputEvent vtable are all
 * exported by modules this one does not link against. See the header for what the extra level
 * buys you.
 */
#define TRACE_HARNESS_WITH_SLATE 0

#include "CollisionQueryParams.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                          // TActorIterator
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/CoreMiscDefines.h"                                 // FInputDeviceId
#include "UnrealClient.h"                                         // FViewport

#if TRACE_HARNESS_WITH_SLATE
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#endif

#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"                                   // hand the Core off before the fire test
#include "Gameplay/TraceHealthComponent.h"                        // killing ourselves to test respawn
#include "Gameplay/TraceTracer.h"                                 // counted as proof a shot was emitted
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"

namespace TraceInputHarness
{
	// -----------------------------------------------------------------------------------------
	// Plumbing
	// -----------------------------------------------------------------------------------------

	/** Where in the input chain the synthetic event is injected. */
	enum class EInjectPath : uint8
	{
		/**
		 * UGameViewportClient::InputKey — exactly what FSceneViewport::OnKeyDown calls once Slate
		 * has routed a real keystroke to the game viewport widget. This is the default.
		 */
		Viewport,

		/**
		 * APlayerController::InputKey — one level lower. Skips the viewport's fullscreen toggle,
		 * console, IgnoreInput() gate and its input-device-to-local-player lookup, so a run that
		 * fails on "viewport" and passes on "controller" has localised the fault to the viewport.
		 */
		Controller,

		/** FSlateApplication. Only exists when TRACE_HARNESS_WITH_SLATE is on. */
		Slate,
	};

	EInjectPath ParsePath(const FString& Token)
	{
		if (Token.Equals(TEXT("controller"), ESearchCase::IgnoreCase)) { return EInjectPath::Controller; }
#if TRACE_HARNESS_WITH_SLATE
		if (Token.Equals(TEXT("slate"), ESearchCase::IgnoreCase))      { return EInjectPath::Slate; }
#else
		if (Token.Equals(TEXT("slate"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("SIMINPUT: the Slate path is compiled out (TRACE_HARNESS_WITH_SLATE=0 — the Trace module ")
				TEXT("does not link Slate/SlateCore). Falling back to the viewport path."));
		}
#endif
		return EInjectPath::Viewport;
	}

	const TCHAR* PathName(EInjectPath Path)
	{
		switch (Path)
		{
		case EInjectPath::Controller: return TEXT("controller");
		case EInjectPath::Slate:      return TEXT("slate");
		default:                      return TEXT("viewport");
		}
	}

	/** Friendly names, because "LMB" is what everybody writes and EKeys has no such alias. */
	FKey ResolveKey(const FString& Token)
	{
		if (Token.Equals(TEXT("LMB"), ESearchCase::IgnoreCase))   { return EKeys::LeftMouseButton; }
		if (Token.Equals(TEXT("RMB"), ESearchCase::IgnoreCase))   { return EKeys::RightMouseButton; }
		if (Token.Equals(TEXT("MMB"), ESearchCase::IgnoreCase))   { return EKeys::MiddleMouseButton; }
		if (Token.Equals(TEXT("Space"), ESearchCase::IgnoreCase)) { return EKeys::SpaceBar; }
		if (Token.Equals(TEXT("Shift"), ESearchCase::IgnoreCase)) { return EKeys::LeftShift; }

		// FKey is just a name; an unknown one constructs fine and fails IsValid().
		return FKey(*Token);
	}

	FViewport* GameViewport()
	{
		return (GEngine != nullptr && GEngine->GameViewport != nullptr) ? GEngine->GameViewport->Viewport : nullptr;
	}

	// -----------------------------------------------------------------------------------------
	// World lookups
	// -----------------------------------------------------------------------------------------

	UWorld* FindGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE) && Context.World() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	ATracePlayerController* FindLocalController()
	{
		UWorld* World = FindGameWorld();
		if (World == nullptr)
		{
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (ATracePlayerController* PC = Cast<ATracePlayerController>(It->Get()))
			{
				if (PC->IsLocalController())
				{
					return PC;
				}
			}
		}
		return nullptr;
	}

	/**
	 * UGameViewportClient::InputKey ends in
	 * GEngine->GetLocalPlayerFromInputDevice(this, EventArgs.InputDevice), so an event carrying a
	 * device id that maps to no local player is silently dropped — it would look exactly like
	 * broken input. Rather than hardcode "device 0", find the id that really does resolve to a
	 * local player and use that; keyboard and mouse are the same device to this API.
	 */
	FInputDeviceId ResolveInputDevice()
	{
		if (GEngine != nullptr && GEngine->GameViewport != nullptr)
		{
			for (int32 InternalId = 0; InternalId < 8; ++InternalId)
			{
				const FInputDeviceId Candidate = FInputDeviceId::CreateFromInternalId(InternalId);
				if (GEngine->GetLocalPlayerFromInputDevice(GEngine->GameViewport, Candidate) != nullptr)
				{
					return Candidate;
				}
			}
		}

		// Desktop platforms map the keyboard/mouse to internal id 0; this is the honest default.
		return FInputDeviceId::CreateFromInternalId(0);
	}

	// -----------------------------------------------------------------------------------------
	// Injection
	// -----------------------------------------------------------------------------------------

	bool InjectKey(const FKey& Key, bool bPressed, EInjectPath Path)
	{
		if (!Key.IsValid())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("SIMINPUT: '%s' is not a key this build knows about."), *Key.ToString());
			return false;
		}

#if TRACE_HARNESS_WITH_SLATE
		if (Path == EInjectPath::Slate && FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();

			if (GEngine != nullptr && GEngine->GameViewport != nullptr)
			{
				// Slate refuses to route a key event to a widget with no focus, and an automated
				// run very often has no OS window focus at all. Forcing focus is the difference
				// between "input is broken" and "this process was in the background".
				if (TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget())
				{
					Slate.SetAllUserFocus(ViewportWidget, EFocusCause::SetDirectly);
				}
			}

			if (Key.IsMouseButton())
			{
				const FVector2D Cursor = Slate.GetCursorPos();
				TSet<FKey> PressedButtons;
				if (bPressed)
				{
					PressedButtons.Add(Key);
				}

				const FPointerEvent MouseEvent(
					ResolveInputDevice(), /*PointerIndex*/ 0, Cursor, Cursor,
					PressedButtons, Key, /*WheelDelta*/ 0.f, FModifierKeysState());

				return bPressed
					? Slate.ProcessMouseButtonDownEvent(nullptr, MouseEvent)
					: Slate.ProcessMouseButtonUpEvent(MouseEvent);
			}

			const FKeyEvent KeyEvent(
				Key, FModifierKeysState(), ResolveInputDevice(),
				/*bIsRepeat*/ false, /*CharacterCode*/ 0, /*KeyCode*/ 0);

			return bPressed ? Slate.ProcessKeyDownEvent(KeyEvent) : Slate.ProcessKeyUpEvent(KeyEvent);
		}
#endif

		const FInputKeyEventArgs Args(
			GameViewport(),
			ResolveInputDevice(),
			Key,
			bPressed ? IE_Pressed : IE_Released,
			/*AmountDepressed*/ bPressed ? 1.f : 0.f,
			/*bIsTouchEvent*/ false,
			FPlatformTime::Cycles64());

		if (Path == EInjectPath::Controller)
		{
			APlayerController* PC = FindLocalController();
			return (PC != nullptr) && PC->InputKey(Args);
		}

		return (GEngine != nullptr && GEngine->GameViewport != nullptr) && GEngine->GameViewport->InputKey(Args);
	}

	bool InjectAxis(const FKey& Key, float Delta, EInjectPath Path)
	{
#if TRACE_HARNESS_WITH_SLATE
		if (Path == EInjectPath::Slate && FSlateApplication::IsInitialized())
		{
			// OnRawMouseMove is what the platform layer calls while the viewport holds the mouse —
			// the very entry point a physical mouse uses in this game's capture mode.
			const int32 DX = (Key == EKeys::MouseX) ? FMath::RoundToInt(Delta) : 0;
			const int32 DY = (Key == EKeys::MouseY) ? FMath::RoundToInt(Delta) : 0;
			FSlateApplication::Get().OnRawMouseMove(DX, DY);
			return true;
		}
#endif

		// NumSamples must be >= 1: UPlayerInput::InputAxis accumulates
		// (Delta, NumSamples) pairs and a zero sample count makes the axis read as untouched.
		const FInputKeyEventArgs Args(
			GameViewport(),
			ResolveInputDevice(),
			Key,
			Delta,
			/*DeltaTime*/ 1.f / 60.f,
			/*NumSamples*/ 1,
			FPlatformTime::Cycles64());

		if (Path == EInjectPath::Controller)
		{
			// APlayerController has no InputAxis of its own in 5.8: axis samples are ordinary
			// FInputKeyEventArgs carrying IE_Axis (which the constructor above set), and
			// UGameViewportClient::InputAxis itself ends in PlayerController->InputKey.
			APlayerController* PC = FindLocalController();
			return (PC != nullptr) && PC->InputKey(Args);
		}

		return (GEngine != nullptr && GEngine->GameViewport != nullptr) && GEngine->GameViewport->InputAxis(Args);
	}

	/** Nearest living character on a different team, so a synthetic shot has something to hit. */
	ATraceCharacter* FindNearestEnemy(const ATraceCharacter* Self)
	{
		UWorld* World = (Self != nullptr) ? Self->GetWorld() : nullptr;
		if (World == nullptr)
		{
			return nullptr;
		}

		const ETraceTeam MyTeam = Self->GetTeam();
		ATraceCharacter* Best = nullptr;
		float BestDistSq = TNumericLimits<float>::Max();

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Other = *It;
			if (Other == nullptr || Other == Self || !Other->IsAlive())
			{
				continue;
			}
			// Requiring two real, different teams matters: before teams replicate everyone reads as
			// None, and aiming at a team-mate would produce a burst the server correctly ignores —
			// which would then look like a broken fire path.
			if (MyTeam == ETraceTeam::None || Other->GetTeam() == ETraceTeam::None || Other->GetTeam() == MyTeam)
			{
				continue;
			}

			const float DistSq = FVector::DistSquared(Other->GetActorLocation(), Self->GetActorLocation());
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = Other;
			}
		}

		return Best;
	}

	/**
	 * Any living team-mate of @p Self other than @p Self, for handing the Core off before the fire
	 * test. Nearest is not required — this only has to be a legal holder.
	 */
	ATraceCharacter* FindTeammateOtherThan(const ATraceCharacter* Self)
	{
		UWorld* World = (Self != nullptr) ? Self->GetWorld() : nullptr;
		if (World == nullptr)
		{
			return nullptr;
		}

		const ETraceTeam MyTeam = Self->GetTeam();
		if (MyTeam == ETraceTeam::None)
		{
			return nullptr;
		}

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Other = *It;
			if (Other != nullptr && Other != Self && Other->IsAlive() && Other->GetTeam() == MyTeam)
			{
				return Other;
			}
		}

		return nullptr;
	}

	/**
	 * Every live tracer actor in the world, by unique id.
	 *
	 * UTraceWeaponComponent::FireOnce spawns one locally the instant a shot leaves the muzzle, so
	 * accumulating these across a burst counts shots that were actually emitted — evidence that is
	 * independent of whether any of them connected. Tracers live 0.08s, hence the per-tick sampling
	 * and the set: a single sample would see at most one or two.
	 */
	void CollectTracerIds(UWorld* World, TSet<uint32>& InOutIds)
	{
		if (World == nullptr)
		{
			return;
		}

		for (TActorIterator<ATraceTracer> It(World); It; ++It)
		{
			if (*It != nullptr)
			{
				InOutIds.Add(It->GetUniqueID());
			}
		}
	}

	// -----------------------------------------------------------------------------------------
	// Trace.SimInput — press, hold, release
	// -----------------------------------------------------------------------------------------

	void HoldKey(const FKey& Key, float Seconds, EInjectPath Path)
	{
		const bool bDown = InjectKey(Key, /*bPressed=*/true, Path);
		UE_LOG(LogTraceGame, Display, TEXT("SIMINPUT: %s DOWN via %s (handled=%d), releasing in %.2fs"),
			*Key.ToString(), PathName(Path), bDown ? 1 : 0, Seconds);

		const FKey KeyCopy = Key;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[KeyCopy, Path](float) -> bool
			{
				const bool bUp = InjectKey(KeyCopy, /*bPressed=*/false, Path);
				UE_LOG(LogTraceGame, Display, TEXT("SIMINPUT: %s UP via %s (handled=%d)"),
					*KeyCopy.ToString(), PathName(Path), bUp ? 1 : 0);
				return false;   // one-shot
			}),
			FMath::Max(0.02f, Seconds));
	}

	// -----------------------------------------------------------------------------------------
	// Trace.InputSelfTest
	// -----------------------------------------------------------------------------------------

	/**
	 * A scripted press/hold/release sequence that asserts on the *effects* — events counted inside
	 * the Enhanced Input delegates, metres travelled, hit confirmations sent back by the server —
	 * rather than on the fact that a function was called. Runs on the core ticker so it is
	 * independent of match state and survives the pawn being destroyed underneath it.
	 */
	class FSelfTest
	{
	public:
		static void Start(float SettleSeconds, EInjectPath Path)
		{
			if (Instance != nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("SELFTEST: already running."));
				return;
			}

			Instance = new FSelfTest(SettleSeconds, Path);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(Instance, &FSelfTest::Tick), 0.f);

			UE_LOG(LogTraceGame, Display,
				TEXT("SELFTEST: armed. Waiting for a living local pawn, then settling %.1fs before the first input. Path=%s"),
				SettleSeconds, PathName(Path));
		}

	private:
		enum class EPhase : uint8
		{
			WaitForPawn,
			Settle,
			MoveHold,
			MoveReport,
			LookSweep,
			LookReport,
			Aim,
			FireHold,
			FireReport,
			KillSelf,
			AwaitRespawn,
			Done,
		};

		FSelfTest(float InSettleSeconds, EInjectPath InPath)
			: SettleSeconds(FMath::Max(0.5f, InSettleSeconds))
			, Path(InPath)
		{
		}

		bool Tick(float DeltaTime)
		{
			Elapsed += DeltaTime;
			PhaseElapsed += DeltaTime;

			ATracePlayerController* PC = FindLocalController();
			if (PC == nullptr)
			{
				if (Elapsed > MaxWaitSeconds)
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("SELFTEST: FAIL — no local ATracePlayerController after %.0fs. Nothing to test."), Elapsed);
					return Finish();
				}
				return true;
			}

			switch (Phase)
			{
			case EPhase::WaitForPawn:   return TickWaitForPawn(PC);
			case EPhase::Settle:        return TickSettle(PC);
			case EPhase::MoveHold:      return TickMoveHold(PC);
			case EPhase::MoveReport:    return TickMoveReport(PC);
			case EPhase::LookSweep:     return TickLookSweep(PC);
			case EPhase::LookReport:    return TickLookReport(PC);
			case EPhase::Aim:           return TickAim(PC);
			case EPhase::FireHold:      return TickFireHold(PC);
			case EPhase::FireReport:    return TickFireReport(PC);
			case EPhase::KillSelf:      return TickKillSelf(PC);
			case EPhase::AwaitRespawn:  return TickAwaitRespawn(PC);
			default:                    return Finish();
			}
		}

		void Advance(EPhase Next)
		{
			Phase = Next;
			PhaseElapsed = 0.f;
		}

		bool Finish()
		{
			UE_LOG(LogTraceGame, Display, TEXT("SELFTEST: ==== END (%d passed, %d failed) ===="), PassCount, FailCount);

			// The ticker delegate we are running inside holds a raw pointer to this object, so the
			// delete has to happen after we have returned false and been unregistered.
			FSelfTest* Dying = Instance;
			Instance = nullptr;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Dying](float) -> bool
			{
				delete Dying;
				return false;
			}), 0.f);

			return false;
		}

		/** "" on the first pass, " [after respawn]" on the second — see TickKillSelf. */
		const TCHAR* PassLabel() const
		{
			return bSecondPass ? TEXT(" [after respawn]") : TEXT("");
		}

		void Check(bool bCondition, const FString& Message)
		{
			if (bCondition)
			{
				++PassCount;
				UE_LOG(LogTraceGame, Display, TEXT("SELFTEST: PASS  %s%s"), *Message, PassLabel());
			}
			else
			{
				++FailCount;
				UE_LOG(LogTraceGame, Error, TEXT("SELFTEST: FAIL  %s%s"), *Message, PassLabel());
			}
		}

		// -- phases ---------------------------------------------------------------------------

		bool TickWaitForPawn(ATracePlayerController* PC)
		{
			const ATraceCharacter* Character = PC->GetTraceCharacter();
			if (Character == nullptr || !Character->IsAlive())
			{
				if (Elapsed > MaxWaitSeconds)
				{
					UE_LOG(LogTraceGame, Error, TEXT("SELFTEST: FAIL — no living local pawn after %.0fs."), Elapsed);
					return Finish();
				}
				return true;
			}

			UE_LOG(LogTraceGame, Display, TEXT("SELFTEST: ==== BEGIN ==== pawn=%s at t=%.1fs"),
				*Character->GetName(), Elapsed);
			PC->LogInputDiagnostics(TEXT("selftest-start"));

			// Static checks first: if any of these fail, every later failure is a consequence of it
			// rather than an independent finding.
			Check(PC->IsLocalController(), TEXT("controller is local"));
			Check(PC->GetLocalPlayer() != nullptr, TEXT("controller has a ULocalPlayer"));
			Check(Character->IsLocallyControlled(), TEXT("pawn is locally controlled"));
			Check(GEngine != nullptr && GEngine->GameViewport != nullptr && !GEngine->GameViewport->IgnoreInput(),
				TEXT("game viewport exists and is not ignoring input"));
			Check(GEngine != nullptr && GEngine->GameViewport != nullptr &&
					GEngine->GetLocalPlayerFromInputDevice(GEngine->GameViewport, ResolveInputDevice()) != nullptr,
				TEXT("an input device resolves to a local player (viewport can route key events)"));

			Advance(EPhase::Settle);
			return true;
		}

		bool TickSettle(ATracePlayerController* PC)
		{
			// ApplyGameInputMode suppresses Look for half a second after every possession, and the
			// match spends its first seconds in warm-up. Firing into that would test the harness's
			// patience rather than the input path.
			if (PhaseElapsed < SettleSeconds)
			{
				return true;
			}

			const ATraceCharacter* Character = PC->GetTraceCharacter();
			if (Character == nullptr)
			{
				Advance(EPhase::WaitForPawn);
				return true;
			}

			MoveStartLocation = Character->GetActorLocation();
			MoveStartEventCount = PC->DebugMoveEventCount;

			UE_LOG(LogTraceGame, Display, TEXT("SELFTEST: pressing W. Location before = %s"),
				*MoveStartLocation.ToCompactString());

			InjectKey(EKeys::W, /*bPressed=*/true, Path);
			Advance(EPhase::MoveHold);
			return true;
		}

		bool TickMoveHold(ATracePlayerController* PC)
		{
			if (PhaseElapsed < MoveHoldSeconds)
			{
				return true;
			}

			InjectKey(EKeys::W, /*bPressed=*/false, Path);
			Advance(EPhase::MoveReport);
			return true;
		}

		bool TickMoveReport(ATracePlayerController* PC)
		{
			// A little slack so the release is processed before the counters are read.
			if (PhaseElapsed < 0.2f)
			{
				return true;
			}

			const ATraceCharacter* Character = PC->GetTraceCharacter();
			const FVector EndLocation = (Character != nullptr) ? Character->GetActorLocation() : MoveStartLocation;
			const float Moved = FVector::Dist2D(EndLocation, MoveStartLocation);
			const int32 MoveEvents = PC->DebugMoveEventCount - MoveStartEventCount;

			UE_LOG(LogTraceGame, Display,
				TEXT("SELFTEST: W held %.1fs -> %d Move events, last value (%.2f, %.2f), moved %.1f uu (%s -> %s)"),
				MoveHoldSeconds, MoveEvents, PC->DebugLastMoveValue.X, PC->DebugLastMoveValue.Y,
				Moved, *MoveStartLocation.ToCompactString(), *EndLocation.ToCompactString());

			Check(MoveEvents > 0, FString::Printf(TEXT("synthetic W reached the Move binding (%d events)"), MoveEvents));
			Check(FMath::Abs(PC->DebugLastMoveValue.Y) > 0.5f,
				FString::Printf(TEXT("Move delivered a non-zero forward axis (Y=%.2f)"), PC->DebugLastMoveValue.Y));
			Check(Moved > MinExpectedMoveUU,
				FString::Printf(TEXT("pawn actually moved (%.1f uu, need > %.0f)"), Moved, MinExpectedMoveUU));

			LookStartYaw = PC->GetControlRotation().Yaw;
			LookStartEventCount = PC->DebugLookEventCount;
			Advance(EPhase::LookSweep);
			return true;
		}

		bool TickLookSweep(ATracePlayerController* PC)
		{
			// Many small samples rather than one big one: OnLookInput drops any single event above
			// a per-frame rate budget, precisely because a mouse-capture warp arrives as one huge
			// jump. A synthetic sweep must look like a hand, not like a warp.
			if (LookSamplesSent < LookSampleCount)
			{
				InjectAxis(EKeys::MouseX, LookSampleDelta, Path);
				++LookSamplesSent;
				return true;
			}

			Advance(EPhase::LookReport);
			return true;
		}

		bool TickLookReport(ATracePlayerController* PC)
		{
			if (PhaseElapsed < 0.2f)
			{
				return true;
			}

			const int32 LookEvents = PC->DebugLookEventCount - LookStartEventCount;
			const float YawDelta = FMath::Abs(FMath::FindDeltaAngleDegrees(LookStartYaw, PC->GetControlRotation().Yaw));

			UE_LOG(LogTraceGame, Display,
				TEXT("SELFTEST: %d synthetic MouseX samples of %+.0f -> %d Look events, yaw moved %.1f deg"),
				LookSamplesSent, LookSampleDelta, LookEvents, YawDelta);

			Check(LookEvents > 0, FString::Printf(TEXT("synthetic mouse reached the Look binding (%d events)"), LookEvents));
			Check(YawDelta > 1.f, FString::Printf(TEXT("control rotation yaw changed (%.1f deg)"), YawDelta));

			Advance(EPhase::Aim);
			return true;
		}

		bool TickAim(ATracePlayerController* PC)
		{
			ATraceCharacter* Character = PC->GetTraceCharacter();
			if (Character == nullptr || !Character->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("SELFTEST: pawn died before the fire test; waiting for a respawn."));
				Advance(EPhase::WaitForPawn);
				return true;
			}

			// Set the shot up so that a miss means something. This is the ONLY place the harness
			// cheats, and it cheats on where the target stands — the trigger itself still travels
			// the whole input chain, and the shot is resolved by the ordinary server code.
			//
			// Two earlier attempts and why they were wrong:
			//
			//  1. Just aim at the nearest enemy. On the enlarged arena that enemy was 14786 uu away
			//     against a HitscanRange of 15000, and the shipped 0.6 deg of spread is a +/-155 uu
			//     cone at that range — against a 34 uu capsule, through whatever cover is between.
			//     Every shot missed and the test could not tell "the fire path is broken" from "you
			//     were shooting at the horizon".
			//
			//  2. Teleport OUR pawn next to the enemy. It worked (1201 uu) and then the frame rate
			//     collapsed to ~4 fps — the pawn had been dropped into somewhere it should not be
			//     and spent the test depenetrating. Measured: 13 frames in 3.5s, and the tracers
			//     (0.08s lifetime) expired between samples, so the run reported zero shots and
			//     looked like a broken weapon. That is exactly the false conclusion to avoid.
			//
			// So: leave our pawn exactly where the game put it, and bring a target to a spot the
			// engine has just told us is reachable by a straight line from our own muzzle.
			if (ATraceCharacter* Target = FindNearestEnemy(Character))
			{
				const FVector Muzzle = Character->GetMuzzleLocation();
				const FVector AimDir = Character->GetAimDirection().GetSafeNormal();

				// Trace on Visibility for the same reason the weapon does not: the Pawn profile
				// ignores that channel, so this finds walls and pillars and nothing else. Whatever
				// distance comes back is guaranteed clear of geometry.
				float ClearDistance = FireTestRangeUU;
				if (UWorld* World = Character->GetWorld())
				{
					FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceInputHarnessLOS), /*bTraceComplex=*/false);
					Params.AddIgnoredActor(Character);

					FHitResult Hit;
					if (World->LineTraceSingleByChannel(
							Hit, Muzzle, Muzzle + AimDir * FireTestRangeUU, ECC_Visibility, Params))
					{
						ClearDistance = FMath::Max(FireTestMinRangeUU, Hit.Distance - FireTestClearanceUU);
					}
				}

				const FVector Spot = Muzzle + AimDir * ClearDistance;
				const bool bMoved = Target->SetActorLocation(
					Spot, /*bSweep=*/false, /*OutSweepHitResult=*/nullptr, ETeleportType::TeleportPhysics);

				const FVector Eye = Character->GetPawnViewLocation();
				PC->SetControlRotation((Target->GetActorLocation() - Eye).Rotation());

				UE_LOG(LogTraceGame, Display,
					TEXT("SELFTEST: placed %s %.0f uu down a clear line from our muzzle (ok=%d); aiming at it."),
					*Target->GetName(), ClearDistance, bMoved ? 1 : 0);
				bHadTarget = true;
				FireTarget = Target;
			}
			else
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("SELFTEST: no living enemy found — the fire test can still prove the binding fires, ")
					TEXT("but not that a shot resolved on a victim."));
			}

			// SPEC §4 MADE MOUSE1 CONTEXTUAL: while carrying the Core it passes and the gun is silent
			// BY DESIGN. The kickoff can legitimately hand the Core to the local pawn, in which case
			// the fire test would report zero tracers and read exactly like a broken weapon — the
			// false conclusion this harness exists to prevent. So take the Core off ourselves first.
			// Authority-only, single player; the Core is never destroyed, only handed to a teammate.
			if (Character->IsCarrier() && Character->HasAuthority())
			{
				if (ATraceCore* TheCore = ATraceCore::Get(Character->GetWorld()))
				{
					ATraceCharacter* Receiver = FindTeammateOtherThan(Character);
					UE_LOG(LogTraceGame, Display,
						TEXT("SELFTEST: we are the Core holder, so mouse1 would PASS, not fire. Handing the Core to %s first."),
						*GetNameSafe(Receiver));

					if (Receiver != nullptr)
					{
						TheCore->GrantTo(Receiver, ETraceCoreGrantReason::Debug);
					}
					else
					{
						// Nobody to hand it to. Park it: KickoffTo(None) is "out of play", which is
						// holderless and therefore leaves our gun live.
						TheCore->KickoffTo(ETraceTeam::None);
					}
				}
			}

			FireStartCount = PC->DebugFireStartedCount;
			HitStartCount = PC->DebugHitConfirmCount;
			TracerIds.Reset();

			UE_LOG(LogTraceGame, Display, TEXT("SELFTEST: pressing LMB (carrier=%d)."), Character->IsCarrier() ? 1 : 0);
			InjectKey(EKeys::LeftMouseButton, /*bPressed=*/true, Path);

			Advance(EPhase::FireHold);
			return true;
		}

		bool TickFireHold(ATracePlayerController* PC)
		{
			// Sampled every tick, not once: a tracer lives 0.08s, so anything less frequent would
			// miss most of the burst.
			CollectTracerIds(PC->GetWorld(), TracerIds);

			if (PhaseElapsed < FireHoldSeconds)
			{
				// Keep tracking the target we placed. Using the cached pointer rather than
				// re-running FindNearestEnemy keeps a TActorIterator over the whole (now large)
				// arena out of the per-frame path, and stops the aim snapping to a different bot
				// halfway through the burst.
				ATraceCharacter* Character = PC->GetTraceCharacter();
				ATraceCharacter* Target = FireTarget.Get();
				if (Character != nullptr && Target != nullptr && Target->IsAlive())
				{
					const FVector Eye = Character->GetPawnViewLocation();
					PC->SetControlRotation((Target->GetActorLocation() - Eye).Rotation());
				}
				return true;
			}

			InjectKey(EKeys::LeftMouseButton, /*bPressed=*/false, Path);
			Advance(EPhase::FireReport);
			return true;
		}

		bool TickFireReport(ATracePlayerController* PC)
		{
			if (PhaseElapsed < 0.5f)
			{
				return true;
			}

			CollectTracerIds(PC->GetWorld(), TracerIds);

			const int32 Fires = PC->DebugFireStartedCount - FireStartCount;
			const int32 Hits = PC->DebugHitConfirmCount - HitStartCount;
			const int32 Shots = TracerIds.Num();

			UE_LOG(LogTraceGame, Display,
				TEXT("SELFTEST: LMB held %.1fs -> %d Fire-pressed events, %d tracers emitted, %d server hit confirmations"),
				FireHoldSeconds, Fires, Shots, Hits);

			// One Started event is correct and expected — the binding is on press, and the weapon
			// component's own tick drives the rest of the burst off the held trigger.
			Check(Fires > 0, FString::Printf(TEXT("synthetic LMB reached the Fire binding (%d events)"), Fires));

			// A tracer is spawned by UTraceWeaponComponent::FireOnce, i.e. only after CanFire()
			// passed and immediately before ServerFire goes out. Several of them prove the held
			// trigger kept firing, not just that one press was delivered.
			Check(Shots > 1, FString::Printf(
				TEXT("held trigger produced repeated shots (%d tracers over %.1fs at a %.2fs fire interval)"),
				Shots, FireHoldSeconds, UTraceSettings::Get().FireInterval));

			if (bHadTarget)
			{
				Check(Hits > 0, FString::Printf(
					TEXT("server resolved a shot and confirmed a hit (%d) — the whole fire path works"), Hits));
			}

			if (!bSecondPass)
			{
				Advance(EPhase::KillSelf);
				return true;
			}

			PC->LogInputDiagnostics(TEXT("selftest-end"));
			Advance(EPhase::Done);
			return Finish();
		}

		/**
		 * The single most suspicious thing about this input setup, and the one a passing first pass
		 * says nothing about.
		 *
		 * Respawn destroys the pawn and re-possesses a brand new one. The action bindings live on
		 * the CONTROLLER's UEnhancedInputComponent and the mapping context lives on the
		 * ULocalPlayer's subsystem, so in principle neither is disturbed — but "in principle" is
		 * exactly the reasoning that produced a shipped build the user could not shoot in. If
		 * bindings did die with the pawn, input would work until the first death and then stop,
		 * which matches the reported symptom precisely.
		 *
		 * So kill the pawn deliberately and run the whole sequence again on the replacement.
		 */
		bool TickKillSelf(ATracePlayerController* PC)
		{
			ATraceCharacter* Character = PC->GetTraceCharacter();
			if (Character == nullptr)
			{
				// Already gone — the bots got there first, which serves just as well.
				PreDeathPawn = nullptr;
				Advance(EPhase::AwaitRespawn);
				return true;
			}

			PreDeathPawn = Character;

			if (UTraceHealthComponent* Health = Character->FindComponentByClass<UTraceHealthComponent>())
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("SELFTEST: killing %s on purpose to test that input survives re-possession."),
					*Character->GetName());

				// Kill(), not ApplyDamage(): it ignores invulnerability, so this works even if we
				// happen to be holding the Core when the test reaches this point.
				Health->Kill(/*Instigator=*/nullptr, FName(TEXT("Fell")));
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("SELFTEST: no health component; cannot test respawn."));
				Advance(EPhase::Done);
				return Finish();
			}

			Advance(EPhase::AwaitRespawn);
			return true;
		}

		bool TickAwaitRespawn(ATracePlayerController* PC)
		{
			ATraceCharacter* Character = PC->GetTraceCharacter();
			const bool bFreshPawn =
				Character != nullptr && Character->IsAlive() && Character != PreDeathPawn.Get();

			if (!bFreshPawn)
			{
				if (PhaseElapsed > RespawnWaitSeconds)
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("SELFTEST: FAIL — no replacement pawn within %.0fs of dying; cannot test re-possession."),
						RespawnWaitSeconds);
					++FailCount;
					Advance(EPhase::Done);
					return Finish();
				}
				return true;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("SELFTEST: respawned as %s after %.1fs. Re-running the whole sequence on the new pawn."),
				*Character->GetName(), PhaseElapsed);

			bSecondPass = true;
			LookSamplesSent = 0;
			FireTarget = nullptr;
			bHadTarget = false;

			PC->LogInputDiagnostics(TEXT("after-respawn"));

			// Short settle: ApplyGameInputMode has just re-armed the look-suppression window, and
			// re-running with it still open would drop the Look samples for reasons that have
			// nothing to do with binding survival.
			SettleSeconds = PostRespawnSettleSeconds;
			Advance(EPhase::Settle);
			return true;
		}

		// -- tuning ---------------------------------------------------------------------------

		static constexpr float MaxWaitSeconds = 90.f;
		static constexpr float MoveHoldSeconds = 1.2f;
		static constexpr float FireHoldSeconds = 3.0f;
		/** Walk speed is several hundred uu/s, so a second of W is hundreds of uu. 50 is a floor. */
		static constexpr float MinExpectedMoveUU = 50.f;
		static constexpr int32 LookSampleCount = 20;
		static constexpr float LookSampleDelta = 6.f;
		/** How far down the aim ray the target is placed when nothing is in the way. */
		static constexpr float FireTestRangeUU = 1200.f;
		/** Never place a target closer than this — a muzzle-contact shot tests nothing useful. */
		static constexpr float FireTestMinRangeUU = 400.f;
		/** Backed off from any wall we hit, so the target is not half-buried in it. */
		static constexpr float FireTestClearanceUU = 150.f;
		/** Generous next to any sane RespawnDelay, so a slow respawn is not read as a failure. */
		static constexpr float RespawnWaitSeconds = 30.f;
		static constexpr float PostRespawnSettleSeconds = 1.5f;

		// -- state ----------------------------------------------------------------------------

		static FSelfTest* Instance;

		float SettleSeconds = 3.f;
		EInjectPath Path = EInjectPath::Viewport;

		EPhase Phase = EPhase::WaitForPawn;
		float Elapsed = 0.f;
		float PhaseElapsed = 0.f;

		FVector MoveStartLocation = FVector::ZeroVector;
		int32 MoveStartEventCount = 0;

		float LookStartYaw = 0.f;
		int32 LookStartEventCount = 0;
		int32 LookSamplesSent = 0;

		int32 FireStartCount = 0;
		int32 HitStartCount = 0;
		bool bHadTarget = false;
		bool bSecondPass = false;
		TWeakObjectPtr<ATraceCharacter> PreDeathPawn;
		TWeakObjectPtr<ATraceCharacter> FireTarget;
		/** Unique ids of every tracer seen during the burst; see CollectTracerIds. */
		TSet<uint32> TracerIds;

		int32 PassCount = 0;
		int32 FailCount = 0;
	};

	FSelfTest* FSelfTest::Instance = nullptr;

	// -----------------------------------------------------------------------------------------
	// Console commands
	// -----------------------------------------------------------------------------------------

	FAutoConsoleCommand CmdInputDiag(
		TEXT("Trace.InputDiag"),
		TEXT("Dump the local player's Enhanced Input setup: PlayerInput class, bindings, mapping context, viewport capture."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ATracePlayerController* PC = FindLocalController();
			if (PC == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("INPUTDIAG: no local ATracePlayerController in any game world."));
				return;
			}

			PC->LogInputDiagnostics(TEXT("console"));

			if (GEngine != nullptr && GEngine->GameViewport != nullptr)
			{
				const FInputDeviceId Device = ResolveInputDevice();
				UE_LOG(LogTraceGame, Display,
					TEXT("INPUTDIAG [console] viewport captureMode=%d ignoreInput=%d inputDevice=%d resolvesToLocalPlayer=%d"),
					static_cast<int32>(GEngine->GameViewport->GetMouseCaptureMode()),
					GEngine->GameViewport->IgnoreInput() ? 1 : 0,
					Device.GetId(),
					(GEngine->GetLocalPlayerFromInputDevice(GEngine->GameViewport, Device) != nullptr) ? 1 : 0);
			}
		}));

	FAutoConsoleCommand CmdSimInput(
		TEXT("Trace.SimInput"),
		TEXT("Trace.SimInput <Key> [HoldSeconds] [viewport|controller] — inject a key/mouse press through the real input pipeline."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("SIMINPUT: usage  Trace.SimInput <Key> [HoldSeconds] [viewport|controller]   e.g. Trace.SimInput W 1.0"));
				return;
			}

			const FKey Key = ResolveKey(Args[0]);
			const float Seconds = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : 0.5f;
			const EInjectPath Path = (Args.Num() > 2) ? ParsePath(Args[2]) : EInjectPath::Viewport;

			HoldKey(Key, Seconds, Path);
		}));

	FAutoConsoleCommand CmdSimAxis(
		TEXT("Trace.SimAxis"),
		TEXT("Trace.SimAxis <MouseX|MouseY> <Delta> [viewport|controller] — inject one analog mouse sample."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("SIMAXIS: usage  Trace.SimAxis <MouseX|MouseY> <Delta> [viewport|controller]"));
				return;
			}

			const FKey Key = ResolveKey(Args[0]);
			const float Delta = FCString::Atof(*Args[1]);
			const EInjectPath Path = (Args.Num() > 2) ? ParsePath(Args[2]) : EInjectPath::Viewport;

			const bool bHandled = InjectAxis(Key, Delta, Path);
			UE_LOG(LogTraceGame, Display, TEXT("SIMAXIS: %s %+.1f via %s (handled=%d)"),
				*Key.ToString(), Delta, PathName(Path), bHandled ? 1 : 0);
		}));

	FAutoConsoleCommand CmdSelfTest(
		TEXT("Trace.InputSelfTest"),
		TEXT("Trace.InputSelfTest [SettleSeconds] [viewport|controller] — scripted proof that synthetic input moves and fires the pawn."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Settle = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 4.f;
			const EInjectPath Path = (Args.Num() > 1) ? ParsePath(Args[1]) : EInjectPath::Viewport;

			// The proof is worthless if the per-event logging is off, so turn it on here rather
			// than relying on whoever launched the run to have remembered.
			if (IConsoleVariable* LogInput = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.LogInput")))
			{
				LogInput->Set(1, ECVF_SetByConsole);
			}

			FSelfTest::Start(Settle, Path);
		}));
}

#endif // !UE_BUILD_SHIPPING
