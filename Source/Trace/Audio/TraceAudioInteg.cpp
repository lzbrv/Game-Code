// Trace — Trace.Audio.Integ: the nine sound events, driven through their REAL triggers (spec v26 §9,
// integration pass).
//
// =================================================================================================
// WHY THIS EXISTS, AND WHY Trace.Audio.Probe IS NOT ENOUGH
// =================================================================================================
// The audio pass shipped two harnesses and both are honest about what they measure:
//
//   Trace.Audio.Report   every event RESOLVES to a USoundBase.        (the bank is wired)
//   Trace.Audio.Probe    every event ALLOCATES a mixer source voice.  (sound reaches the mixer)
//
// Neither of them touches a call site. Both drive the audio system directly, so a trigger that was
// never wired — a Jump that nothing calls, a Headshot with no line in ClientNotifyHit — passes both
// with full marks. That is exactly the gap this pass had to close: five of the nine call sites lived
// in files the audio agent did not own and were reported rather than written.
//
// So this harness never calls TraceAudio:: at all. It presses REAL KEYS through Trace.SimInput and
// runs the REAL debug entry points that the shipping code paths hang off, then reads back
// UTraceAudioSubsystem::GetPlaysByEvent() — a per-event tally bumped inside PlayLocalNow/
// PlayWorldNow, i.e. after the side gate, the settings gate, the device test and the resolve. An
// entry in that map means "a real trigger handed this sound to the engine on this machine".
//
// WHAT DRIVES WHAT, and every one of them is the player's own path:
//
//   Dash        LeftShift through the controller route -> BeginDash                      (game-side)
//   Jump        SpaceBar  through the controller route -> DoJump                         (client)
//   WallJump    a real fall into a real wall, then SpaceBar -> TryWallJump               (client)
//   Bodyshot    control rotation onto an enemy's chest, then Fire -> ClientNotifyHit     (client)
//   Headshot    control rotation onto the head SPHERE the damage model itself defines    (client)
//   CoreTurnover  mouse 1 while carrying -> ThrowFromHolder -> AnnounceTurnoverSound    (game-side)
//   CorePickup  Trace.Verif.GrantCore -> ATraceCore::GrantTo -> OnRep_Carrier            (client)
//   Parry       RightMouseButton while carrying -> ServerTryBeginParry                   (game-side)
//   ButtonPress Escape, then Enter on a pause-menu row -> ActivateSelected               (client)
//
// ORDER IS LOAD-BEARING. The shots come BEFORE the Core is granted, because mouse 1 THROWS while
// carrying instead of firing (ATraceCharacter::DoFirePressed) — a fire step run while carrying would
// measure the throw and report a missing Bodyshot. The parry comes after the grant for the opposite
// reason: ServerTryBeginParry refuses a non-carrier, and a refused parry is correctly silent.
//
// =================================================================================================
// USING IT
// =================================================================================================
//     Trace.Audio.Integ            drive all nine and print the verdict
//
// Run it WITHOUT -nosound. With -nosound every event is refused at the device test and the verdict
// is 0 of 9 — which is the harness's own red arm and is worth taking once.
//
//     UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor"
//     "$UE" "$PWD/Trace.uproject" "/Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode" \
//         -game -windowed -ResX=1280 -ResY=720 \
//         -LogCmds="LogTraceGame Verbose" -TraceExec=Trace.Audio.Integ -TraceExecAt=8 -TraceExecOn=Match
//
// Scheduled on FTSTicker and not on the world timer, for the reason UI/TraceIntegrationWalk.cpp
// documents at length: the last steps press ESCAPE, which pauses the world, and a world timer in a
// paused world never fires again. Half of this walk is behind that pause.

#include "CoreMinimal.h"

#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "EngineUtils.h"                  // TActorIterator
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#include "Audio/TraceAudio.h"
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceHitZones.h"       // FTraceHitZoneModel — the head sphere, DERIVED not retyped
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"                        // LogTraceGame
#include "TraceTypes.h"

#if !UE_BUILD_SHIPPING

// Named after the file, never anonymous: this module is a unity build and
// Scripts/check-jumbo-build-collisions.py gates on exactly this.
namespace TraceAudioIntegFile
{
	/**
	 * The instant each step fires, in seconds after arming.
	 *
	 * TWO OF THESE NUMBERS ARE LOAD-BEARING AND BOTH WERE MEASURED THE HARD WAY, by a first run that
	 * reported two call sites missing when both were wired:
	 *
	 *   Warmup      the character-select screen is still up when a match begins and it SUPPRESSES
	 *               GAMEPLAY INPUT. The first run pressed W and LeftShift into that suppression and
	 *               reported "SILENT Dash" for a dash that never happened. The warm-up press closes
	 *               the screen; everything else waits until gameplay input is live.
	 *
	 *   WallJumpPress - WallSetup = 0.15 s. The pawn is perched one capsule-radius off the wall
	 *               moving into it at 700 uu/s, so it touches the face in about 0.07 s, but it is
	 *               also FALLING and reaches the floor in about a third of a second. The first run
	 *               waited 0.6 s, landed, and took an ordinary grounded jump — which played Jump,
	 *               not WallJump, and read as an unwired call site. The press has to land inside the
	 *               window between the wall contact and the ground.
	 */
	struct FSchedule
	{
		static constexpr float Warmup         =  0.30f;
		static constexpr float MoveStart      =  2.60f;
		static constexpr float Dash           =  3.70f;
		static constexpr float Jump           =  5.30f;
		static constexpr float FireBody       =  7.20f;
		static constexpr float FireHead       =  9.60f;
		static constexpr float WallSetup      = 11.60f;
		static constexpr float WallJumpPress  = 11.75f;
		// SPEC v28 §2 SWAPPED THESE THREE ROUND. The turnover sound is no longer staged onto the
		// ground; it is a real throw by a real carrier, so the Core has to be in the pawn's hands
		// first (Pickup) and the parry — which refuses a non-carrier — has to happen while it is still
		// there. The throw goes last and empties the hands, which nothing after it needs.
		static constexpr float Pickup         = 13.60f;
		static constexpr float Parry          = 15.60f;
		static constexpr float Turnover       = 17.00f;
		static constexpr float MenuOpen       = 18.80f;
		static constexpr float MenuTop        = 19.60f;
		static constexpr float MenuActivate   = 21.40f;
		static constexpr float Verdict        = 23.80f;
	};

	/** The local player's pawn, or null. */
	static ATraceCharacter* LocalPawn(UWorld* World)
	{
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		return (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	}

	/** Press a real key through the same injection path every other harness in this project uses. */
	static void Press(UWorld* World, const TCHAR* Key, float Hold, const TCHAR* Route)
	{
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		if (PC == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[AudioInteg] no local controller; '%s' not pressed."), Key);
			return;
		}
		PC->ConsoleCommand(FString::Printf(TEXT("Trace.SimInput %s %.2f %s"), Key, Hold, Route),
			/*bWriteToLog=*/false);
	}

	/** Run a real console command through the local controller, so "the local pawn" resolves. */
	static void Exec(UWorld* World, const FString& Command)
	{
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		if (PC != nullptr)
		{
			PC->ConsoleCommand(Command, /*bWriteToLog=*/true);
		}
		else if (GEngine != nullptr)
		{
			GEngine->Exec(World, *Command);
		}
	}

	/**
	 * Put a real enemy ON THE SHOT RAY the camera is already pointing down, so the fire press that
	 * follows it in the same tick connects.
	 *
	 * WHY NOT "aim at the bot and shoot". Two runs tried that and the reason it fails is worth
	 * recording. The shot ORIGIN and direction are CAMERA-derived on purpose (spec v26 §4 is emphatic
	 * that the hit-resolution ray must stay that way), and the camera follows control rotation on ITS
	 * own tick — so SetControlRotation followed by a fire press in the same frame resolves the shot
	 * down the PREVIOUS frame's ray. Splitting them by a frame trades that for a different miss: a
	 * bot walking at match speed leaves the ray during the gap, and at ~13 fps under load the gap is
	 * long. The headshot landed and the bodyshot missed on the same run for exactly this reason.
	 *
	 * So the fixture stops steering the camera and moves the TARGET onto the ray the camera reports
	 * right now. Nothing about the shot changes: the fire key, the server's lag-compensated resolver,
	 * the head/body classification and ClientNotifyHit are all the shipping path, and the zone that
	 * comes back is decided by the damage model, not by this function.
	 *
	 * AND IT HAS TO BE HELD THERE FOR A FEW FRAMES, which is the third thing this fixture had to
	 * learn. The server resolves a shot against UTraceLagCompensationComponent's RECORDED pose
	 * history, not against the pawn's live transform — that is the whole point of lag compensation —
	 * so a target teleported and shot in the same tick is resolved against the pose it had before the
	 * teleport, and the shot misses a target that is visibly on the crosshair. The caller re-places
	 * it every frame for a third of a second before pressing fire, which both fills the history and
	 * stops a walking bot from strolling back out of the ray.
	 *
	 * @param HitOffsetFromActor  the point on the target that must land on the ray, as an offset from
	 *                            its actor location. Zero for the chest; HeadCenter - ActorLocation
	 *                            for the head, so the head SPHERE is what the ray passes through.
	 */
	static ATraceCharacter* PlaceEnemyOnAimRay(UWorld* World, ATraceCharacter* Me,
		const FVector& HitOffsetFromActor, double Distance, const TCHAR* What, bool bLog)
	{
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		if (World == nullptr || Me == nullptr || PC == nullptr)
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

		// THE CAMERA'S OWN RAY, asked of the camera rather than rebuilt from control rotation - the
		// same view point the shot is resolved from.
		FVector ViewLocation = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

		const FVector Spot = ViewLocation + ViewRotation.Vector() * Distance;
		Nearest->TeleportTo(Spot - HitOffsetFromActor, (-ViewRotation.Vector()).Rotation(),
			/*bIsATest=*/false, /*bNoCheck=*/true);

		if (bLog)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[AudioInteg] FIXTURE MOVE (%s): %s held on the camera's own ray with its %s at %s, ")
				TEXT("%.0f uu out, until the server's lag-compensation history has recorded it there. The ")
				TEXT("SHOT is untouched - real fire key, real server resolve, real zone classification, ")
				TEXT("real ClientNotifyHit."),
				What, *GetNameSafe(Nearest), What, *Spot.ToCompactString(), Distance);
		}
		return Nearest;
	}

	/**
	 * The world-space centre of @p Target's HEAD SPHERE, asked of the damage model itself rather
	 * than typed as a fraction here. The standing rule: a value derived from a base must move when
	 * the base moves, and HeadCenterHeightFraction lives in UTraceDamageSettings where a designer
	 * can edit it. A hardcoded "capsule top minus 12 uu" here would silently start aiming at a
	 * shoulder the day that knob changed, and the harness would report a missing Headshot call site.
	 */
	static bool HeadPointOf(const ATraceCharacter* Target, FVector& OutHead)
	{
		const UCapsuleComponent* Capsule = (Target != nullptr) ? Target->GetCapsuleComponent() : nullptr;
		if (Capsule == nullptr)
		{
			return false;
		}

		const FTraceHitZoneModel Model = FTraceHitZoneModel::FromCapsule(
			Capsule->GetComponentLocation(),
			Capsule->GetScaledCapsuleHalfHeight(),
			Capsule->GetScaledCapsuleRadius(),
			Target->GetHitZonePostureScale());

		OutHead = Model.HeadCenter;
		return true;
	}

	/**
	 * Put the pawn in the air against a real wall, falling into it, so the NEXT jump press is a wall
	 * jump. Nothing here launches the pawn: HandleImpact opens the wall window when the capsule
	 * actually touches the face, and TryWallJump then does the whole of the work.
	 *
	 * @return  what happened, for the log — an empty string on success.
	 */
	static FString StageWallContact(UWorld* World)
	{
		ATraceCharacter* Me = LocalPawn(World);
		UTraceCharacterMovementComponent* Move =
			(Me != nullptr) ? Cast<UTraceCharacterMovementComponent>(Me->GetCharacterMovement()) : nullptr;
		const UCapsuleComponent* Capsule = (Me != nullptr) ? Me->GetCapsuleComponent() : nullptr;
		if (Me == nullptr || Move == nullptr || Capsule == nullptr)
		{
			return TEXT("no local pawn");
		}

		// Sweep for a wall in twelve directions around the pawn at chest height. Twelve rather than
		// four because the arena's walls are not axis-aligned everywhere, and a fixture that only
		// finds a wall on some spawns is a fixture that reports a missing call site on the others.
		const FVector From = Me->GetActorLocation();
		const double Reach = 3000.0;

		FVector BestPoint = FVector::ZeroVector;
		FVector BestNormal = FVector::ZeroVector;
		double BestDist = TNumericLimits<double>::Max();

		FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceAudioIntegWall), /*bTraceComplex=*/false);
		Params.AddIgnoredActor(Me);

		for (int32 Step = 0; Step < 12; ++Step)
		{
			const double Yaw = 360.0 * static_cast<double>(Step) / 12.0;
			const FVector Dir = FRotator(0.0, Yaw, 0.0).Vector();

			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, From, From + Dir * Reach, ECC_Visibility, Params))
			{
				continue;
			}

			// Near-vertical faces only. A ramp or the floor would take the press and produce an
			// ordinary jump, which is a different sound and would make this step lie.
			if (FMath::Abs(Hit.ImpactNormal.Z) > 0.25)
			{
				continue;
			}

			if (Hit.Distance < BestDist)
			{
				BestDist = Hit.Distance;
				BestPoint = Hit.ImpactPoint;
				BestNormal = Hit.ImpactNormal;
			}
		}

		if (BestNormal.IsNearlyZero())
		{
			return TEXT("no vertical wall within 3000 uu of the pawn");
		}

		// Stand off by the capsule's own radius plus a small gap, so the teleport does not start
		// interpenetrating the wall — again derived from the capsule rather than a typed number.
		const double StandOff = Capsule->GetScaledCapsuleRadius() + 12.0;
		const FVector Perch = BestPoint + BestNormal * StandOff + FVector(0.0, 0.0, 220.0);

		Me->TeleportTo(Perch, (-BestNormal).Rotation(), /*bIsATest=*/false, /*bNoCheck=*/true);
		Move->SetMovementMode(MOVE_Falling);

		// Into the face, and falling. The inward speed is what makes the contact a real impact rather
		// than a rest, which is what HandleImpact needs to open the window.
		Move->Velocity = (-BestNormal) * 700.0 + FVector(0.0, 0.0, -120.0);

		UE_LOG(LogTraceGame, Display,
			TEXT("[AudioInteg] wall staged: face at %s normal=%s, perched %.0f uu off it, falling into it."),
			*BestPoint.ToCompactString(), *BestNormal.ToCompactString(), StandOff);
		return FString();
	}

	/**
	 * Hold a bot on the shot ray for @p HoldSeconds, then press fire, then keep holding it for a beat
	 * so a re-resolve on a later frame still finds it. One helper for both shots; @p bHead only
	 * changes WHICH point on the target is put on the ray, and the zone that comes back is still the
	 * damage model's answer rather than this function's claim.
	 */
	static void DriveShot(UWorld* World, bool bHead, const TCHAR* Label)
	{
		if (World == nullptr)
		{
			return;
		}

		TSharedRef<double> Elapsed = MakeShared<double>(0.0);
		TSharedRef<bool> bFired = MakeShared<bool>(false);
		TSharedRef<bool> bLogged = MakeShared<bool>(false);
		const FString LabelCopy(Label);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[World, bHead, Elapsed, bFired, bLogged, LabelCopy](float Delta) -> bool
		{
			*Elapsed += Delta;

			ATraceCharacter* Me = LocalPawn(World);
			if (Me == nullptr)
			{
				return false;
			}

			// The head offset is re-measured every frame off whichever enemy is nearest, so a respawn
			// or a posture change between frames cannot leave a stale offset behind.
			FVector HitOffset = FVector::ZeroVector;
			if (bHead)
			{
				ATraceCharacter* Probe = nullptr;
				double BestDistSq = TNumericLimits<double>::Max();
				for (TActorIterator<ATraceCharacter> It(World); It; ++It)
				{
					ATraceCharacter* Other = *It;
					if (Other == nullptr || Other == Me || !Other->IsAlive() || Other->GetTeam() == Me->GetTeam())
					{
						continue;
					}
					const double DistSq = FVector::DistSquared(Me->GetActorLocation(), Other->GetActorLocation());
					if (DistSq < BestDistSq) { BestDistSq = DistSq; Probe = Other; }
				}

				FVector Head = FVector::ZeroVector;
				if (Probe != nullptr && HeadPointOf(Probe, Head))
				{
					HitOffset = Head - Probe->GetActorLocation();
				}
			}

			ATraceCharacter* Enemy = PlaceEnemyOnAimRay(World, Me, HitOffset, 1200.0,
				bHead ? TEXT("head") : TEXT("chest"), !*bLogged);
			if (Enemy == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[AudioInteg] %s SKIPPED - this world has no living enemy at all. The fixture ")
					TEXT("failed, not the call site."), *LabelCopy);
				return false;
			}
			*bLogged = true;

			if (!*bFired && *Elapsed >= 0.35)
			{
				*bFired = true;
				UE_LOG(LogTraceGame, Display, TEXT("[AudioInteg] %s - firing at %s."),
					*LabelCopy, *GetNameSafe(Enemy));
				Press(World, TEXT("LeftMouseButton"), 0.12f, TEXT("controller"));
			}

			return *Elapsed < 0.85;
		}), 0.f);
	}

	/** One row of the verdict. */
	struct FExpectation
	{
		FName Event;
		const TCHAR* Trigger;
	};

	static void Report(UWorld* World)
	{
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		if (Audio == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[AudioInteg] VERDICT: no audio subsystem. 0 of 9."));
			return;
		}

		const FExpectation Rows[] =
		{
			{ TraceSoundEvents::Dash,         TEXT("LeftShift -> BeginDash") },
			{ TraceSoundEvents::Jump,         TEXT("SpaceBar -> DoJump") },
			{ TraceSoundEvents::WallJump,     TEXT("SpaceBar on a wall -> TryWallJump") },
			{ TraceSoundEvents::Bodyshot,     TEXT("Fire at a chest -> ClientNotifyHit") },
			{ TraceSoundEvents::Headshot,     TEXT("Fire at a head -> ClientNotifyHit") },
			{ TraceSoundEvents::CoreTurnover, TEXT("mouse 1 while carrying -> ThrowFromHolder (v28 s2: the DROP)") },
			{ TraceSoundEvents::CorePickup,   TEXT("GrantCore -> OnRep_Carrier") },
			{ TraceSoundEvents::Parry,        TEXT("RightMouse while carrying -> ServerTryBeginParry") },
			{ TraceSoundEvents::ButtonPress,  TEXT("Enter on a menu row -> ActivateSelected") },
		};

		const TMap<FName, int32>& Plays = Audio->GetPlaysByEvent();
		int32 Heard = 0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[AudioInteg] ---- spec v26 §9: the nine events, driven through their REAL triggers ----"));

		for (const FExpectation& Row : Rows)
		{
			const int32* Count = Plays.Find(Row.Event);
			const int32 N = (Count != nullptr) ? *Count : 0;
			const bool bOk = (N > 0);
			Heard += bOk ? 1 : 0;

			// Display and Warning are separate calls: UE_LOG's verbosity is a token the macro pastes
			// into a compile-time category check, not a value a ternary can choose.
			if (bOk)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[AudioInteg]   HEARD   %-13s x%-3d side=%-6s  via %s"),
					*Row.Event.ToString(), N,
					TraceSoundEvents::SideName(TraceSoundEvents::SideOf(Row.Event)), Row.Trigger);
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[AudioInteg]   SILENT  %-13s x0    side=%-6s  via %s"),
					*Row.Event.ToString(),
					TraceSoundEvents::SideName(TraceSoundEvents::SideOf(Row.Event)), Row.Trigger);
			}
		}

		const FTraceAudioCounters& Tally = Audio->GetCounters();
		UE_LOG(LogTraceGame, Display,
			TEXT("[AudioInteg] counters: local %d  world %d  multicasts sent %d  "
			     "refused not-authority %d  refused not-local-player %d  missing %d  no-device %d"),
			Tally.LocalPlays, Tally.WorldPlays, Tally.MulticastsSent,
			Tally.RefusedNotAuthority, Tally.RefusedNotLocalPlayer, Tally.MissingSound, Tally.NoAudioDevice);

		if (Heard == UE_ARRAY_COUNT(Rows))
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[AudioInteg] VERDICT: PASS - %d of %d events fired from their own call site."),
				Heard, static_cast<int32>(UE_ARRAY_COUNT(Rows)));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[AudioInteg] VERDICT: FAIL - %d of %d events fired from their own call site."),
				Heard, static_cast<int32>(UE_ARRAY_COUNT(Rows)));
		}
	}

	/** Schedules @p Work at @p At seconds from now. One ticker per step: a failed step strands none. */
	static void At(float AtSeconds, TFunction<void()> Work)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Work](float) -> bool { Work(); return false; }), FMath::Max(0.01f, AtSeconds));
	}

	/** Roster slot the warm-up picks. 3 = MACE by default; see the warm-up step for why. */
	static int32 GCharacterSlot = 3;

	static void Start(UWorld* World, bool bResetTally, float BaseOffset, bool bWarmUp)
	{
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[AudioInteg] no world."));
			return;
		}

		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		if (Audio == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[AudioInteg] no audio subsystem in this world."));
			return;
		}

		// Measure THIS drive. Anything the match already played (a bot's dash, the menu click that
		// started the match) would otherwise be counted as evidence for a call site this walk never
		// exercised, which is the exact failure mode the harness exists to catch.
		if (bResetTally)
		{
			Audio->ResetPlaysByEvent();
		}

		// The game mode is in the banner because it decides whether two of the nine can pass at all.
		// ATracePracticeGameMode's Core RACK takes the Core straight back off anyone a debug grant
		// hands it to, so the parry — which refuses a non-carrier — cannot fire in the practice range
		// no matter how the call site is wired. Run this in a real match; the banner is what lets a
		// reader tell "the parry is unwired" from "the rack took the Core back".
		UE_LOG(LogTraceGame, Display,
			TEXT("[AudioInteg] === pass at t+%.1fs: driving the nine spec v26 §9 triggers. netmode=%d mode=%s ==="),
			BaseOffset, static_cast<int32>(World->GetNetMode()),
			*GetNameSafe(World->GetAuthGameMode()));

		TWeakObjectPtr<UWorld> WeakWorld(World);
		const auto W = [WeakWorld]() -> UWorld* { return WeakWorld.Get(); };

		// WARM-UP: close the character-select screen if it is still up. It suppresses gameplay input
		// entirely, and a walk that starts inside that suppression measures nothing - the first run
		// pressed W and LeftShift into that suppression and reported the Dash call site missing.
		//
		// MACE, AND THE CHOICE MATTERS FOR EXACTLY ONE STEP. Trace.Characters.Select goes through the
		// screen's own highlight-then-confirm path, so this is still the screen being used rather than
		// bypassed. The default highlight is ROCCO, and Rocco's passive is a SECOND JUMP that consumes
		// an airborne jump press before the movement component ever sees it (UTraceAbilitySetRocco::
		// OnJumpPressed) - measured: "[Rocco] second jump: Z -274 -> 260" instead of a wall jump. Mace
		// does not override OnJumpPressed, so the wall-jump step tests the wall jump.
		if (bWarmUp)
		{
		At(BaseOffset + FSchedule::Warmup, [W]()
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[AudioInteg] warm-up: picking roster slot %d through the select screen (default 3 = "
				     "MACE; Rocco's second jump would eat the wall-jump press) and handing gameplay "
				     "input back."), GCharacterSlot);
			Exec(W(), FString::Printf(TEXT("Trace.Characters.Select %d"), GCharacterSlot));
		});
		}

		At(BaseOffset + FSchedule::MoveStart, [W]()
		{
			UE_LOG(LogTraceGame, Display, TEXT("[AudioInteg] step 1/9: running, so the dash has somewhere to go."));
			Press(W(), TEXT("W"), 1.60f, TEXT("controller"));
		});

		At(BaseOffset + FSchedule::Dash, [W]()
		{
			UE_LOG(LogTraceGame, Display, TEXT("[AudioInteg] step 2/9: DASH (LeftShift)."));
			Press(W(), TEXT("LeftShift"), 0.10f, TEXT("controller"));
		});

		At(BaseOffset + FSchedule::Jump, [W]()
		{
			UE_LOG(LogTraceGame, Display, TEXT("[AudioInteg] step 3/9: JUMP (SpaceBar)."));
			Press(W(), TEXT("SpaceBar"), 0.10f, TEXT("controller"));
		});

		// The chest first, then the head. Both hold a real bot on the camera's existing ray for a
		// third of a second before the fire press, so the server's lag-compensation history has the
		// target where the shot is going. See PlaceEnemyOnAimRay for why all three of those clauses
		// are there and which run taught each one.
		At(BaseOffset + FSchedule::FireBody, [W]() { DriveShot(W(), /*bHead=*/false, TEXT("step 4/9: BODYSHOT")); });
		At(BaseOffset + FSchedule::FireHead, [W]() { DriveShot(W(), /*bHead=*/true,  TEXT("step 5/9: HEADSHOT")); });

		At(BaseOffset + FSchedule::WallSetup, [W]()
		{
			const FString Why = StageWallContact(W());
			if (!Why.IsEmpty())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[AudioInteg] step 6/9: WALLJUMP fixture failed: %s"), *Why);
				return;
			}
			UE_LOG(LogTraceGame, Display, TEXT("[AudioInteg] step 6/9: WALLJUMP - falling into a real wall."));
		});

		At(BaseOffset + FSchedule::WallJumpPress, [W]()
		{
			// PRESSED ON THE FRAME THE WALL WINDOW OPENS, not at a time guessed in advance. The first
			// two versions of this step pressed at a fixed delay and both missed: 0.6 s landed the pawn
			// first (an ordinary grounded Jump), and 0.15 s reached the wall but the press still found
			// no window. UTraceCharacterMovementComponent::IsWallJumpAvailable() is the movement
			// layer's OWN answer to "would a press count right now" — the same predicate spec v8 §7's
			// harness uses — so polling it removes the guess entirely.
			//
			// A press made when it returns false is not a wall jump and DoJump refuses it outright,
			// which is silent on purpose: neither Jump nor WallJump. That is why a missed window read
			// as an unwired call site rather than as a bad fixture, and why the timeout below prints
			// the whole state instead of just failing.
			TSharedRef<double> Elapsed = MakeShared<double>(0.0);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[W, Elapsed](float Delta) -> bool
			{
				*Elapsed += Delta;

				UWorld* Now = W();
				ATraceCharacter* Me = LocalPawn(Now);
				UTraceCharacterMovementComponent* Move =
					(Me != nullptr) ? Cast<UTraceCharacterMovementComponent>(Me->GetCharacterMovement()) : nullptr;
				if (Move == nullptr)
				{
					return false;
				}

				if (Move->IsWallJumpAvailable())
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[AudioInteg] wall window OPEN after %.3fs: normal=%s remaining=%.3fs "
						     "consecutive=%d. Pressing jump now."),
						*Elapsed, *Move->GetWallJumpNormal().ToCompactString(),
						Move->GetWallJumpWindowRemainingForAudit(), Move->GetWallJumpsSinceGround());
					Press(Now, TEXT("SpaceBar"), 0.10f, TEXT("controller"));
					return false;
				}

				if (*Elapsed > 2.0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[AudioInteg] WALLJUMP fixture timed out after %.2fs: falling=%d normal=%s "
						     "windowRemaining=%.3fs consecutive=%d velocity=%s. No window ever opened, so "
						     "the press was never made - this is the fixture, not the call site."),
						*Elapsed, Move->IsFalling() ? 1 : 0, *Move->GetWallJumpNormal().ToCompactString(),
						Move->GetWallJumpWindowRemainingForAudit(), Move->GetWallJumpsSinceGround(),
						*Move->Velocity.ToCompactString());
					return false;
				}

				return true;
			}), 0.f);
		});

		At(BaseOffset + FSchedule::Pickup, [W]()
		{
			UE_LOG(LogTraceGame, Display, TEXT("[AudioInteg] step 7/9: COREPICKUP (through GrantTo/OnRep_Carrier)."));
			Exec(W(), TEXT("Trace.Verif.GrantCore"));
		});

		At(BaseOffset + FSchedule::Parry, [W]()
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[AudioInteg] step 8/9: PARRY (right mouse, now that the pawn is carrying)."));
			Press(W(), TEXT("RightMouseButton"), 0.15f, TEXT("controller"));
		});

		// SPEC v28 §2 MOVED THIS STEP, AND MOVED IT LAST OF THE THREE ON PURPOSE.
		//
		// It used to be `Trace.Integ.StageTurnover`, which teleports a Core onto the floor and calls
		// RegisterTurnover — the LANDING. That is exactly the edge v28 §2 takes the sound off, so a
		// harness left pointing at it would report the fix as a missing call site.
		//
		// The new trigger is the owner's own: the carrier DROPS the Core. Mouse 1 while carrying is a
		// THROW (ATraceCharacter::DoFirePressed), so this is a real key, on the real player path, into
		// ThrowFromHolder -> AnnounceTurnoverSound.
		//
		// ORDER IS LOAD-BEARING, as it already was for the shots: the parry refuses a non-carrier, so
		// the throw has to come after it or the pawn would be empty-handed when the parry step fires.
		At(BaseOffset + FSchedule::Turnover, [W]()
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[AudioInteg] step 9/9: CORETURNOVER (mouse 1 while carrying -> the carrier DROPS ")
				TEXT("the Core -> AnnounceTurnoverSound, spec v28 §2)."));
			Press(W(), TEXT("LeftMouseButton"), 0.12f, TEXT("controller"));
		});

		At(BaseOffset + FSchedule::MenuOpen, [W]()
		{
			UE_LOG(LogTraceGame, Display, TEXT("[AudioInteg] step 9b: BUTTONPRESS - opening the pause menu."));
			Press(W(), TEXT("Escape"), 0.12f, TEXT("viewport"));
		});

		// WALK THE SELECTION TO THE TOP FIRST. MoveSelection clamps, so three Ups land on row 0 =
		// RESUME whatever the menu opened on. This is not tidiness: one run opened the pause menu with
		// the selection already on QUIT (the mouse happened to be over that row), the Enter below
		// activated it, and the process exited BEFORE the verdict could print. The ButtonPress had in
		// fact played — the evidence was destroyed by the step that produced it.
		At(BaseOffset + FSchedule::MenuTop,      [W]() { Press(W(), TEXT("Up"), 0.10f, TEXT("viewport")); });
		At(BaseOffset + FSchedule::MenuTop + 0.35f, [W]() { Press(W(), TEXT("Up"), 0.10f, TEXT("viewport")); });
		At(BaseOffset + FSchedule::MenuTop + 0.70f, [W]() { Press(W(), TEXT("Up"), 0.10f, TEXT("viewport")); });
		At(BaseOffset + FSchedule::MenuTop + 1.05f, [W]() { Press(W(), TEXT("Up"), 0.10f, TEXT("viewport")); });

		At(BaseOffset + FSchedule::MenuActivate, [W]()
		{
			// Enter on the pause root's FIRST row (RESUME). It activates through the same
			// FTraceOptionsMenu::ActivateSelected every settings row uses, and closing the menu again
			// is the tidy side effect rather than the point.
			UE_LOG(LogTraceGame, Display,
				TEXT("[AudioInteg] step 9b: activating the top pause row (RESUME)."));
			Press(W(), TEXT("Enter"), 0.12f, TEXT("viewport"));
		});

		if (!bWarmUp)   // the second pass, which is the one that reports
		{
			At(BaseOffset + FSchedule::Verdict, [W]() { Report(W()); });
		}
	}

	/**
	 * TWO PASSES, AND THE SECOND ONE IS NOT PADDING.
	 *
	 * This machine runs the harness at anywhere between 7 and 25 frames per second depending on what
	 * else is compiling, and several of these steps are only a frame or two wide — the wall-jump
	 * window is 0.087 s, a fire press has to land while the target is still held on the ray, and a
	 * jump press has to reach CheckJumpInput before the pawn lands. At 7 fps a single pass loses
	 * steps to the frame rate rather than to the code, and a run that reports SILENT for that reason
	 * is a harness lying about a call site.
	 *
	 * So the whole drive runs twice and the tally is NOT reset between them: a call site only has to
	 * fire once to be proven wired, and two passes make a frame-rate miss much less likely to be
	 * mistaken for an unwired trigger. If an event is silent in BOTH passes, that is a real finding.
	 */
	static void StartTwice(const TArray<FString>& Args, UWorld* World)
	{
		// Optional roster slot, so the walk can be run as somebody other than the default. It exists
		// for one reason: ROCCO (slot 1) intercepts the airborne jump press with his second jump, and
		// running the walk as him is how the "Rocco's second jump is completely silent" gap was found
		// and then shown fixed on the same harness.
		GCharacterSlot = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 10) : 3;

		Start(World, /*bResetTally=*/true, /*BaseOffset=*/0.f, /*bWarmUp=*/true);
		Start(World, /*bResetTally=*/false, /*BaseOffset=*/FSchedule::Verdict, /*bWarmUp=*/false);
	}

	static FAutoConsoleCommandWithWorldAndArgs GCmd(
		TEXT("Trace.Audio.Integ"),
		TEXT("SPEC v26 INTEGRATION. Drives all nine §9 sound events through their REAL triggers - real "
		     "key presses and the shipping debug entry points, never TraceAudio:: directly - then reports "
		     "which events actually reached the engine. Unlike Trace.Audio.Probe this can go red for a "
		     "single unwired CALL SITE. Run it WITHOUT -nosound; takes about 22 seconds."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StartTwice));
}

#endif // !UE_BUILD_SHIPPING
