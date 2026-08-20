// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — Trace.Audio.V29Integ: Goal, Kill and RoccoRipple through their REAL triggers (spec v29 §1f)
// ===================================================================================================
//
// §1f gave three WAVs with no stated trigger. Choosing a trigger is a judgement (the reasoning is on
// each row in Audio/TraceSoundEvents.cpp); WIRING it is a fact, and this project has shipped a
// "wired" hook that never fired more than once. Trace.Audio.Report would say all three resolve to a
// sound and Trace.Audio.Probe would say all three reach the mixer, and BOTH would pass on a build
// where nothing in the game ever called them — which is exactly the gap Trace.Audio.Integ was written
// for in v26. This is the same idea for the three new ones.
//
// So this harness never calls TraceAudio:: itself. It drives the shipping paths:
//
//   RoccoRipple   pick Rocco, then activate the ability -> UTraceAbilitySetRocco::ActivateAbility
//   Kill          bring a real enemy to 5 HP, put it on the camera's own ray, fire the real gun ->
//                 ServerFire -> the lag-compensated resolver -> ClientNotifyHit(bKilled=true)
//   Goal          ATraceGameMode::NotifyScored, which is the function both endzone detectors call
//
// ...and then reads UTraceAudioSubsystem::GetPlaysByEvent() back. That map is bumped inside
// PlayLocalNow/PlayWorldNow, i.e. AFTER the side gate, the settings gate, the device test and the
// resolve, so a row that moved means a real trigger handed a real sound to the engine.
//
// TWO OF THE THREE CAN LEGITIMATELY BE SKIPPED, AND THE HARNESS SAYS SO RATHER THAN FAILING:
//
//   * GOAL is deliberately inside NotifyScored's `bCounts` branch — a warm-up carry, a half-time
//     carry and a post-whistle carry all run that function and reset the field, and none of them is
//     a goal. In a match that is not InProgress there is no goal to make a sound for, so a run in
//     that state reports SKIPPED with the match state printed. Calling it a failure would train the
//     next reader to ignore the harness.
//   * KILL needs a living enemy. The practice range has targets; an empty arena has nobody to kill.
//
// RUN IT WITHOUT -nosound, on the SERVER, one harness per -TraceExec batch.
// ===================================================================================================

#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                  // TActorIterator
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Trace.h"
#include "TraceSettings.h"   // MaxHealth, so the 5 HP fixture is relative to the knob
#include "TraceTypes.h"

#if !UE_BUILD_SHIPPING

// Named after the file. See Scripts/check-jumbo-build-collisions.py.
namespace TraceAudioV29IntegFile
{
	/**
	 * The instant each step fires, in seconds after arming.
	 *
	 * *** THE KILL COMES BEFORE THE RIPPLE, AND THE ORDER IS LOAD-BEARING. *** The first version ran
	 * the Ripple first and the log showed why that was wrong: "[Ripple] TraceCharacter_0 entered —
	 * propelled at 2100 uu/s". Rocco's own ripple picks the caster up and flings them down the path,
	 * so the shooting step was firing from a pawn moving at 2100 uu/s at a target being re-placed
	 * relative to it every frame. Shoot first, standing still; ride afterwards, when nothing else
	 * depends on where the shooter is.
	 */
	struct FSchedule
	{
		static constexpr double Arm          =  0.20;
		static constexpr double StageEnemy   =  0.60;   // re-run every tick until Fire
		static constexpr double Fire         =  1.60;   // a full second of holding still first
		static constexpr double PickRocco    =  4.00;
		static constexpr double Ripple       =  5.00;
		static constexpr double Score        =  6.20;
		static constexpr double Verdict      =  7.60;
	};

	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		double StartedReal = 0.0;
		int32 Stage = 0;
		bool bTriggerDown = false;
		bool bEnemyFound = false;
		bool bMatchCounted = false;
		FString MatchStateLabel;
		TWeakObjectPtr<ATraceCharacter> Enemy;
	};

	static TSharedPtr<FRun> GRun;

	static UWorld* AuthoritativeWorld()
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

	static ATraceCharacter* LocalPawn(UWorld* World)
	{
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		return (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	}

	static int32 PlaysOf(UWorld* World, FName Event)
	{
		if (const UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World))
		{
			if (const int32* Found = Audio->GetPlaysByEvent().Find(Event))
			{
				return *Found;
			}
		}
		return 0;
	}

	/**
	 * Holds the nearest living enemy on the camera's OWN ray, at 5 HP.
	 *
	 * The fixture moves the TARGET rather than steering the camera, and it re-places it every frame
	 * for a few frames before the shot — both for the reasons Audio/TraceAudioInteg.cpp documents at
	 * length: the shot ray is camera-derived and the camera follows control rotation on its own tick,
	 * and the server resolves against the lag-compensation history rather than the live pose. Nothing
	 * about the SHOT is faked: real trigger, real server resolve, real ClientNotifyHit.
	 *
	 * The 5 HP is what makes the next bullet a KILL rather than a hit, which is the event under test.
	 */
	static ATraceCharacter* StageEnemy(UWorld* World, ATraceCharacter* Me)
	{
		if (World == nullptr || Me == nullptr)
		{
			return nullptr;
		}

		ATraceCharacter* Nearest = nullptr;
		double BestDistSq = TNumericLimits<double>::Max();
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Other = *It;
			if (Other == nullptr || Other == Me || !Other->IsAlive() || Other->GetTeam() == Me->GetTeam())
			{
				continue;
			}
			const double DistSq = FVector::DistSquared(Me->GetActorLocation(), Other->GetActorLocation());
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Nearest = Other;
			}
		}
		if (Nearest == nullptr)
		{
			return nullptr;
		}

		// =========================================================================================
		// *** MOVE THE TARGET TO THE FLOOR, THEN AIM AT IT. NOT THE OTHER WAY ROUND. ***
		//
		// The first two versions of this fixture teleported the target ONTO the shot ray and fired
		// down it, and both missed every round. The measurement that settled it is the line this
		// function's caller now prints: "584.1 uu along the ray and 66.0 uu OFF it (capsule radius
		// 34.0)". A WALKING character does not stay where a teleport puts it — the movement component
		// snaps it back down to the floor on the very next update — so the target was reliably parked
		// 66 uu below a horizontal ray fired from a muzzle at eye height, i.e. two capsule radii wide
		// of a capsule. The harness then reported the Kill call site as unwired when nothing was wrong
		// with it, which is precisely the "harness that lied" this project keeps a house rule about.
		//
		// So the target is put somewhere it can actually STAND — level with the shooter, 600 uu
		// ahead — and the SHOOTER is aimed at it. Aiming is safe to do in the same frame here in a way
		// that it was not for the v26 walk: UTraceWeaponComponent builds its ray from
		// ATraceCharacter::GetMuzzleLocation() and GetAimDirection(), which are arithmetic on the
		// actor transform and the control rotation rather than on the camera's interpolated pose. And
		// the caller re-runs this EVERY frame of the burst, so even a one-frame lag converges long
		// before the second round.
		// =========================================================================================
		const FVector Forward = Me->GetActorForwardVector().GetSafeNormal2D();
		if (Forward.IsNearlyZero())
		{
			return nullptr;
		}

		FVector Spot = Me->GetActorLocation() + Forward * 600.0;
		Spot.Z = Me->GetActorLocation().Z;   // the same floor, so the snap-down has nothing to do
		Nearest->TeleportTo(Spot, (-Forward).Rotation(), /*bIsATest=*/false, /*bNoCheck=*/true);

		// AIM AT THE CAPSULE CENTRE, from the muzzle the bullet leaves. A body shot is all this needs
		// — the zone does not matter, only that a bullet lands and that it is the last one.
		if (APlayerController* Shooter = Cast<APlayerController>(Me->GetController()))
		{
			const FVector ToCentre = Nearest->GetActorLocation() - Me->GetMuzzleLocation();
			if (!ToCentre.IsNearlyZero())
			{
				Shooter->SetControlRotation(ToCentre.Rotation());
			}
		}

		if (UTraceHealthComponent* Health = Nearest->Health)
		{
			// GetHealthPercent() x the MaxHealth knob, rather than a hardcoded 100: the health bar and
			// the knob are the two things a designer can move, and re-typing either here would make
			// this fixture leave the target on the wrong number the day one of them changed.
			const float Current = Health->GetHealthPercent() * FMath::Max(1.f, UTraceSettings::Get().MaxHealth);
			const float Excess = Current - 5.f;
			if (Excess > 0.f)
			{
				// A NULL instigator, deliberately: this must not look like a kill by the player, or the
				// bullet that follows would not be the thing that killed them.
				Health->ApplyDamage(Excess, nullptr, FName(TEXT("Trace.Audio.V29Integ")));
			}
		}
		return Nearest;
	}

	static void Report(FRun& Run)
	{
		UWorld* World = Run.World.Get();

		struct FRow
		{
			FName Event;
			const TCHAR* Trigger;
			bool bRequired;
			const TCHAR* SkipReason;
		};

		const bool bHaveEnemy = Run.bEnemyFound;
		const FRow Rows[] =
		{
			{ TraceSoundEvents::RoccoRipple,
			  TEXT("pick Rocco, activate -> UTraceAbilitySetRocco::ActivateAbility"), true, TEXT("") },
			{ TraceSoundEvents::Kill,
			  TEXT("a real bullet finishes a real enemy -> ClientNotifyHit(bKilled)"), bHaveEnemy,
			  TEXT("this world had no living enemy to kill") },
			{ TraceSoundEvents::Goal,
			  TEXT("ATraceGameMode::NotifyScored -> the score moves"), Run.bMatchCounted,
			  TEXT("the match is not InProgress, so NotifyScored counts nothing and there is no goal") },
		};

		// The hit-marker sounds are the witness for the Kill step: they are client-side, the local
		// player is the shooter, and one of them plays for EVERY bullet that lands. Zero of them means
		// the burst missed (a fixture problem); some of them with no Kill means the target survived.
		UE_LOG(LogTraceGame, Display,
			TEXT("[V29Integ] bullets that LANDED this run: Bodyshot x%d, Headshot x%d. Target %s alive=%d."),
			PlaysOf(World, TraceSoundEvents::Bodyshot), PlaysOf(World, TraceSoundEvents::Headshot),
			*GetNameSafe(Run.Enemy.Get()),
			(Run.Enemy.IsValid() && Run.Enemy->IsAlive()) ? 1 : 0);

		UE_LOG(LogTraceGame, Display,
			TEXT("[V29Integ] ---- spec v29 §1f: the three inferred triggers, driven for real ----"));
		UE_LOG(LogTraceGame, Display, TEXT("[V29Integ] match state: %s"), *Run.MatchStateLabel);

		int32 Heard = 0;
		int32 Wanted = 0;
		for (const FRow& Row : Rows)
		{
			const int32 Count = PlaysOf(World, Row.Event);
			const TCHAR* Side = TraceSoundEvents::SideName(TraceSoundEvents::SideOf(Row.Event));

			if (!Row.bRequired)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V29Integ]   SKIPPED %-12s x%-3d side=%-11s  %s"),
					*Row.Event.ToString(), Count, Side, Row.SkipReason);
				continue;
			}

			++Wanted;
			if (Count > 0)
			{
				++Heard;
				UE_LOG(LogTraceGame, Display, TEXT("[V29Integ]   HEARD   %-12s x%-3d side=%-11s  via %s"),
					*Row.Event.ToString(), Count, Side, Row.Trigger);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[V29Integ]   SILENT  %-12s x0   side=%-11s  via %s"),
					*Row.Event.ToString(), Side, Row.Trigger);
			}
		}

		// TWO CALLS, NOT A TERNARY VERBOSITY: UE_LOG pastes its second argument onto `ELogVerbosity::`.
#define TRACE_V29INTEG_VERDICT_TEXT \
	TEXT("TRACE V29 INTEG VERDICT: %s - %d of %d drivable §1f events fired from their own call site.")
#define TRACE_V29INTEG_VERDICT_ARGS \
	(Heard == Wanted && Wanted > 0 ? TEXT("PASS") : TEXT("FAIL")), Heard, Wanted

		if (Heard == Wanted && Wanted > 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_V29INTEG_VERDICT_TEXT, TRACE_V29INTEG_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_V29INTEG_VERDICT_TEXT, TRACE_V29INTEG_VERDICT_ARGS);
		}

#undef TRACE_V29INTEG_VERDICT_ARGS
#undef TRACE_V29INTEG_VERDICT_TEXT

		GRun.Reset();
	}

	static bool Tick(float /*Delta*/)
	{
		TSharedPtr<FRun> Run = GRun;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* World = Run->World.Get();
		ATraceCharacter* Me = LocalPawn(World);
		if (World == nullptr)
		{
			GRun.Reset();
			return false;
		}

		const double Elapsed = FPlatformTime::Seconds() - Run->StartedReal;

		if (Run->Stage == 0 && Elapsed >= FSchedule::Arm)
		{
			// Measure THIS drive. Anything the match already played would otherwise be counted as
			// evidence for a call site this walk never exercised.
			if (UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World))
			{
				Audio->ResetPlaysByEvent();
			}
			++Run->Stage;
			return true;
		}

		if (Run->Stage == 1)
		{
			// Held on the ray every frame until the shot — see StageEnemy. A full second of it, so the
			// server's lag-compensation history is full of the target STANDING WHERE IT IS NOW rather
			// than where it was before the fixture moved it.
			if (Elapsed >= FSchedule::StageEnemy && Elapsed < FSchedule::Fire)
			{
				if (ATraceCharacter* Enemy = StageEnemy(World, Me))
				{
					Run->Enemy = Enemy;
					Run->bEnemyFound = true;
				}
				return true;
			}
			if (Elapsed >= FSchedule::Fire)
			{
				if (Run->bEnemyFound && Me != nullptr)
				{
					// EVIDENCE, NOT HOPE: the perpendicular distance from the target's capsule centre to
					// the gun's own ray, printed beside the capsule radius. If a burst misses, this line
					// says whether the fixture put the target in the wrong place or whether something
					// downstream ate the shot — which is the difference between fixing the harness and
					// fixing the game.
					ATraceCharacter* Target = Run->Enemy.Get();
					const FVector Origin = Me->GetMuzzleLocation();
					const FVector Direction = Me->GetAimDirection().GetSafeNormal();
					const FVector ToTarget = Target->GetActorLocation() - Origin;
					const double Along = FVector::DotProduct(ToTarget, Direction);
					const double OffAxis = (ToTarget - Direction * Along).Size();
					const float Radius = (Target->GetCapsuleComponent() != nullptr)
						? Target->GetCapsuleComponent()->GetScaledCapsuleRadius() : 0.f;

					UE_LOG(LogTraceGame, Display,
						TEXT("[V29Integ] Kill: %s staged. muzzle %s, aim %s, target %s -> %.1f uu along "
						     "the ray and %.1f uu OFF it (capsule radius %.1f). health %.0f%%, alive=%d."),
						*GetNameSafe(Target), *Origin.ToCompactString(), *Direction.ToCompactString(),
						*Target->GetActorLocation().ToCompactString(), Along, OffAxis, Radius,
						100.f * ((Target->Health != nullptr) ? Target->Health->GetHealthPercent() : 0.f),
						Target->IsAlive() ? 1 : 0);
				}
				else
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[V29Integ] Kill: no living enemy in this world. The FIXTURE failed, not "
						     "the call site."));
				}
				++Run->Stage;
				return true;
			}
			return true;
		}

		if (Run->Stage == 2)
		{
			// Pulse the trigger. Pulsed, not held, so this works whether or not spec v29 §2b has made
			// the pistol semi-automatic.
			if (Elapsed < FSchedule::PickRocco)
			{
				// *** THE TARGET IS RE-PLACED WHILE THE SHOOTING HAPPENS, NOT ONLY BEFORE IT. *** The
				// practice range's targets and a real match's bots both move on their own, and a burst
				// fired at where a target used to be lands nowhere. The SHOT is still entirely the
				// shipping path.
				StageEnemy(World, Me);

				if (UTraceWeaponComponent* Weapon = (Me != nullptr) ? Me->Weapon : nullptr)
				{
					if (Run->bTriggerDown) { Weapon->StopFire(); } else { Weapon->StartFire(); }
					Run->bTriggerDown = !Run->bTriggerDown;
				}
				return true;
			}

			if (UTraceWeaponComponent* Weapon = (Me != nullptr) ? Me->Weapon : nullptr)
			{
				Weapon->StopFire();
			}
			++Run->Stage;
			return true;
		}

		if (Run->Stage == 3 && Elapsed >= FSchedule::PickRocco)
		{
			if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Me))
			{
				Abilities->ServerSetCharacter(ETraceCharacterId::Rocco);
				Abilities->DebugSetActivatedCooldown(0.f);
				UE_LOG(LogTraceGame, Display, TEXT("[V29Integ] picked Rocco (roster says %s)."),
					TraceCharacterIdToString(Abilities->GetCharacterId()));
			}
			++Run->Stage;
			return true;
		}

		if (Run->Stage == 4 && Elapsed >= FSchedule::Ripple)
		{
			if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Me))
			{
				Abilities->DebugSetActivatedCooldown(0.f);
				const bool bFired = Abilities->TryActivate();
				UE_LOG(LogTraceGame, Display,
					TEXT("[V29Integ] Ripple: TryActivate() returned %d. The sound belongs to the "
					     "authority's own ActivateAbility, one line after the ripple actor spawns."),
					bFired ? 1 : 0);
			}
			++Run->Stage;
			return true;
		}

		if (Run->Stage == 5 && Elapsed >= FSchedule::Score)
		{
			// GOAL, through the function both endzone detectors call.
			ATraceGameMode* Mode = World->GetAuthGameMode<ATraceGameMode>();
			ATraceGameState* State = World->GetGameState<ATraceGameState>();
			if (Mode != nullptr && Me != nullptr)
			{
				Run->bMatchCounted = (State != nullptr)
					&& State->TraceMatchState == ETraceMatchState::InProgress && !State->IsHalfTimeBreak();
				Run->MatchStateLabel = (State != nullptr)
					? FString::Printf(TEXT("%d (InProgress=%d, halfTime=%d)"),
						static_cast<int32>(State->TraceMatchState),
						State->TraceMatchState == ETraceMatchState::InProgress ? 1 : 0,
						State->IsHalfTimeBreak() ? 1 : 0)
					: FString(TEXT("no game state"));

				UE_LOG(LogTraceGame, Display,
					TEXT("[V29Integ] Goal: calling NotifyScored(%s). match state %s"),
					*TraceTeamName(Me->GetTeam()).ToString(), *Run->MatchStateLabel);
				Mode->NotifyScored(Me->GetTeam());
			}
			++Run->Stage;
			return true;
		}

		if (Run->Stage == 6 && Elapsed >= FSchedule::Verdict)
		{
			Report(*Run);
			return false;
		}

		return true;
	}

	static void V29Integ()
	{
		UWorld* World = AuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[V29Integ] no authoritative game world. Two of the three are game-side sounds the "
				     "server multicasts, so this must run on the server."));
			return;
		}
		if (GRun.IsValid())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V29Integ] a run is already in progress."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Audio.V29Integ (spec v29 s1f) ================"));
		if (World->GetAudioDeviceRaw() == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[V29Integ] NO AUDIO DEVICE (-nosound, or a dedicated server). Every row will read "
				     "0 and the verdict will FAIL - that is this harness's own red arm, not a statement "
				     "about the call sites."));
		}

		TSharedPtr<FRun> Run = MakeShared<FRun>();
		Run->World = World;
		Run->StartedReal = FPlatformTime::Seconds();
		Run->MatchStateLabel = TEXT("(not read yet)");
		GRun = Run;

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.f);
	}

	FAutoConsoleCommand CmdV29Integ(
		TEXT("Trace.Audio.V29Integ"),
		TEXT("Spec v29 s1f. Drives Goal, Kill and RoccoRipple through their REAL triggers - Rocco's ")
		TEXT("own ActivateAbility, a real bullet finishing a real enemy, and ATraceGameMode::")
		TEXT("NotifyScored - then reads the per-event play tally back. Server only, without -nosound."),
		FConsoleCommandDelegate::CreateStatic(&V29Integ));
}

#endif // !UE_BUILD_SHIPPING
