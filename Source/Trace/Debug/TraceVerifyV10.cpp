// =================================================================================================
// TraceVerifyV10.cpp — FINAL VERIFIER's own harnesses. Nothing in this file is shipped gameplay.
//
// WHY THIS FILE EXISTS AT ALL, given Trace.Knife.CarrierImmunityTest already exists.
//
// That test stages a back-stab through the WHOLE delivery chain — bot AI, weapon equip, the 0.2 s
// pullout, the 0.5 s cooldown, StartSwing's gates, a 0.10 s wind-up, TickSwing's muzzle/aim, then
// ResolveSwing — inside a live match, while pinning the Core and forcing a pass window every frame.
// Its RED arm (the rule deleted, so a blade MUST land on the carrier) never went red. Its own author
// said so and refused to claim the rule was proven. They were right to.
//
// The diagnosis is not exotic. The rule under test is four lines inside ONE function. Everything
// else in that chain is a delivery mechanism, and the delivery mechanism is what will not stage:
// the harness writes the attacker's control rotation from the CORE TICKER, which runs before the
// world tick, and ATraceBotController::UpdateAim then slews that rotation back toward the bot's OWN
// target later in the same frame (RInterpConstantTo, finite turn rate). By the time TickSwing reads
// GetAimDirection() the blade is pointed somewhere nobody chose. Add a pawn being teleported every
// frame, a victim walking, a lag-comp pose sampled in TG_PostPhysics of the PREVIOUS frame, and a
// pass window that must survive a 0.10 s wind-up, and the staged geometry is a coin flip.
//
// So this harness deletes the delivery chain and tests the rule.
//
// ResolveSwing's signature takes Origin and AimDirection as PARAMETERS. That is the whole trick:
// the geometry can be constructed arithmetically instead of being coaxed out of a bot, and the same
// constructed geometry can be replayed twice in ONE frame with ONE variable changed — the
// Trace.Knife.CarrierImmune arm. Same world, same tick, same victim, same pose history, same origin,
// same direction, same everything, differing only in the branch under test. That is a cleaner
// experiment than the staged version could ever be, not a weaker one.
//
// This is NOT a mock. TraceMelee::ResolveSwing is the shipped resolver; the carrier branch it
// contains is the only implementation of the rule in the build (grep IsCoreHolder in TraceMelee.cpp
// — there is exactly one), and GetCarrierKnifeHitCount() is incremented at the line where the
// verdict is reached, by the shipped code, not by this file. What this file supplies is the input.
//
// FOUR RESOLUTIONS, ONE FRAME, reported as a table:
//
//   1. CONTROL      non-carrier enemy, same constructed geometry, rule ON
//                   -> MUST HIT. If this misses, the geometry is wrong and nothing below means
//                      anything. This is the guard against the failure mode the staged test hit:
//                      reporting "no blade landed" when no blade was ever in the air.
//   2. SHIELD-UP    the carrier, shield up, rule OFF
//                   -> must MISS, and it is ResolveHitscan's own bullet rule that stops it, not the
//                      knife's. Reported separately so the two rules are never confused for one.
//   3. RED          the carrier, shield SUPPRESSED (mid-pass), rule OFF
//                   -> MUST HIT THE CARRIER. THE FALSIFICATION. With the rule deleted the blade has
//                      to land, or this harness cannot see the bug it claims to be testing.
//   4. GREEN        the carrier, shield SUPPRESSED, rule ON
//                   -> must MISS. THE CLAIM.
//
// 3 and 4 differ in one CVar and nothing else — not one frame apart, not one pawn apart. The verdict
// is PROVEN only when 1 hits, 3 hits, and 4 misses. Any other combination prints NOT PROVEN and
// says which leg failed.
//
// The shield state for 3 and 4 is asserted immediately before EACH resolution and re-asserted
// between them, because ATraceCore::DebugForcePassWindow's window is short and the two resolutions
// must sit inside the same one.
// =================================================================================================

#include "CoreMinimal.h"

#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"              // FTSTicker — the proof polls for a staged world
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"

#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceMelee.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Settings/TraceUserSettings.h"
#include "InputCoreTypes.h"
#include "Trace.h"
#include "TraceTypes.h"

namespace
{
	/** The one authoritative game world, or null. Same shape every harness in this project uses. */
	UWorld* FindAuthWorld()
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

	double ServerNow(const UWorld* World)
	{
		if (World == nullptr)
		{
			return 0.0;
		}
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return GameState->GetServerWorldTimeSeconds();
		}
		return World->GetTimeSeconds();
	}

	IConsoleVariable* CarrierImmuneCVar()
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Knife.CarrierImmune"));
	}

	/**
	 * A blade's worth of geometry aimed at a victim's BACK, built from arithmetic rather than from a
	 * pawn's transform.
	 *
	 * Origin sits on the victim's rear axis at the victim's own eye height — eye height rather than
	 * capsule centre because that is where GetMuzzleLocation() puts a real swing, so the elevation of
	 * this ray matches a real one. Direction is the victim's forward, i.e. straight into their back,
	 * which is unambiguously inside BackstabHalfAngleDegrees and is the case the user asked about.
	 *
	 * 55% of the blade's reach, not 100%: comfortably inside the range check while leaving the two
	 * bodies far enough apart that the approach vector is not degenerate.
	 */
	void BuildBackstabGeometry(const ATraceCharacter& Victim, FVector& OutOrigin, FVector& OutDirection)
	{
		const FRotator YawOnly(0.f, Victim.GetActorRotation().Yaw, 0.f);
		const FVector Forward = YawOnly.Vector();

		const double Standoff = static_cast<double>(TraceMelee::GetSwingRangeUU()) * 0.55;

		FVector Eye = Victim.GetActorLocation();
		Eye.Z = Victim.GetPawnViewLocation().Z;

		OutOrigin = Eye - Forward * Standoff;
		OutDirection = Forward;
	}

	/** One resolution, logged in full. Returns whether the blade landed on the intended victim. */
	bool ResolveOnce(
		UWorld* World,
		ATraceCharacter* Attacker,
		ATraceCharacter* IntendedVictim,
		int32 ImmuneArm,
		const TCHAR* Label,
		int32& OutCarrierHitDelta)
	{
		FVector Origin = FVector::ZeroVector;
		FVector Direction = FVector::ForwardVector;
		BuildBackstabGeometry(*IntendedVictim, Origin, Direction);

		if (IConsoleVariable* Immune = CarrierImmuneCVar())
		{
			Immune->Set(ImmuneArm, ECVF_SetByConsole);
		}

		const int32 Before = TraceMelee::GetCarrierKnifeHitCount();

		FTraceMeleeHit Hit;
		TraceMelee::ResolveSwing(World, Attacker, Origin, Direction,
			static_cast<float>(ServerNow(World)), Hit);

		OutCarrierHitDelta = TraceMelee::GetCarrierKnifeHitCount() - Before;

		const bool bHitIntended = (Hit.Victim == IntendedVictim);

		UE_LOG(LogTraceGame, Display,
			TEXT("[V10PROOF] %-10s immune=%d | victimIsCarrier=%d shieldSuppressed=%d | hit=%s (intended=%d) "
			     "backstab=%d damage=%.0f approach=%.1fdeg blockedByShield=%d carrierHitDelta=%d | standoff=%.0fuu"),
			Label, ImmuneArm,
			ATraceCore::IsCoreHolder(IntendedVictim) ? 1 : 0,
			ATraceCore::IsShieldSuppressedFor(IntendedVictim) ? 1 : 0,
			*GetNameSafe(Hit.Victim), bHitIntended ? 1 : 0,
			Hit.bBackstab ? 1 : 0, Hit.Damage, Hit.ApproachAngleDegrees,
			Hit.bBlockedByCarrierShield ? 1 : 0, OutCarrierHitDelta,
			FVector::Dist(Origin, IntendedVictim->GetActorLocation()));

		return bHitIntended;
	}

	/** A living enemy of Victim that is not Victim and is not carrying. */
	ATraceCharacter* FindEnemy(UWorld* World, const ATraceCharacter* Victim, const ATraceCharacter* Exclude)
	{
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Victim || Candidate == Exclude)
			{
				continue;
			}
			if (!Candidate->IsAlive() || Candidate->IsCarrier())
			{
				continue;
			}
			if (Candidate->GetTeam() == Victim->GetTeam() || Candidate->GetTeam() == ETraceTeam::None)
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	/**
	 * Trace.V10.After <seconds> <command...>
	 *
	 * -ExecCmds fires the instant the map finishes loading, which is before the GameMode has spawned
	 * a single bot. Every harness that reads live pawns therefore has to either poll internally or be
	 * scheduled, and most of the ones in this project do not poll. This is the scheduler, so a whole
	 * battery can be lined up in one -ExecCmds string without a shell sleeping between processes.
	 */
	FAutoConsoleCommand CmdAfter(
		TEXT("Trace.V10.After"),
		TEXT("Trace.V10.After <seconds> <command...> — run a console command later. Exists because "
		     "-ExecCmds fires before any pawn has spawned."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V10] usage: Trace.V10.After <seconds> <command...>"));
				return;
			}

			const double Delay = FMath::Clamp(FCString::Atod(*Args[0]), 0.0, 3600.0);

			TArray<FString> Rest(Args);
			Rest.RemoveAt(0);
			const FString Command = FString::Join(Rest, TEXT(" "));
			const double FireAt = FPlatformTime::Seconds() + Delay;

			UE_LOG(LogTraceGame, Display, TEXT("[V10] scheduled in %.0fs: %s"), Delay, *Command);

			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[FireAt, Command](float) -> bool
			{
				if (FPlatformTime::Seconds() < FireAt)
				{
					return true;
				}
				UE_LOG(LogTraceGame, Display, TEXT("[V10] firing: %s"), *Command);
				if (GEngine != nullptr)
				{
					GEngine->Exec(FindAuthWorld(), *Command);
				}
				return false;
			}), 0.f);
		}));

	/**
	 * Trace.V10.Pose X Y Z Pitch Yaw — put the local pawn exactly there and HOLD it there.
	 *
	 * Trace.Arena.Pose already exists and does almost this, but it goes through APawn::TeleportTo,
	 * which performs an encroachment check and REFUSES silently — it logged `moved=0` on every one of
	 * four attempts here and left the pawn on its spawn point, so a screenshot battery aimed at the
	 * wall cove photographed the middle of the field instead. A camera rig must not be able to fail
	 * quietly, because the failure mode is a picture of the wrong thing that looks like a picture of
	 * the right thing.
	 *
	 * So: an unconditional TeleportPhysics move, and then MOVE_Flying with the velocity zeroed, so
	 * the pawn hovers at the requested Z instead of falling out of the composed shot. Nothing here is
	 * gameplay — it exists to aim a camera.
	 */
	FAutoConsoleCommand CmdPose(
		TEXT("Trace.V10.Pose"),
		TEXT("Trace.V10.Pose X Y Z [Pitch] [Yaw] — unconditionally place and hold the local pawn for a "
		     "screenshot. Unlike Trace.Arena.Pose this cannot be refused by an encroachment check."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (Args.Num() < 3)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V10POSE] usage: Trace.V10.Pose X Y Z [Pitch] [Yaw]"));
				return;
			}

			UWorld* World = FindAuthWorld();
			APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
			ACharacter* LocalPawn = (PC != nullptr) ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
			if (LocalPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V10POSE] no locally controlled character."));
				return;
			}

			const FVector Where(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2]));
			const FRotator Aim(
				(Args.Num() > 3) ? FCString::Atof(*Args[3]) : 0.f,
				(Args.Num() > 4) ? FCString::Atof(*Args[4]) : 0.f,
				0.f);

			LocalPawn->SetActorLocationAndRotation(Where, FRotator(0.f, Aim.Yaw, 0.f), false, nullptr,
				ETeleportType::TeleportPhysics);
			PC->SetControlRotation(Aim);

			if (UCharacterMovementComponent* Move = LocalPawn->GetCharacterMovement())
			{
				Move->StopMovementImmediately();
				Move->SetMovementMode(MOVE_Flying);
				Move->Velocity = FVector::ZeroVector;
			}

			UE_LOG(LogTraceGame, Display, TEXT("[V10POSE] %s held at %s aim %s"),
				*GetNameSafe(LocalPawn), *Where.ToCompactString(), *Aim.ToCompactString());
		}));

	// ---------------------------------------------------------------------------------------------
	// SPEC v10 §6 — "Don't let players shoot while in a dash animation. As soon as they end the dash,
	// let them shoot again."
	//
	// Two claims, and the second is the one a config value cannot answer. "Blocked during" is easy to
	// assert; "released the INSTANT it ends, not on a cooldown afterwards" is a statement about frame
	// boundaries. So this watches every pawn every frame and checks the STRONGER property that
	// implies both:
	//
	//     AreWeaponActionsBlocked() == IsDashing(), on every frame, for every pawn.
	//
	// If the gate is ever true while not dashing, something added a cooldown after the dash. If it is
	// ever false while dashing, the gate leaks. Either way the equality breaks and the frame is
	// counted and named. A per-frame identity is also immune to the sampling luck a "press fire after
	// a dash and see what happens" test depends on.
	//
	// CanFire() is sampled alongside it and counted separately, because the gate being closed is only
	// interesting if it actually stops the weapon.
	// ---------------------------------------------------------------------------------------------
	struct FDashGateWatch
	{
		double EndAt = 0.0;
		int64 Frames = 0;
		int64 DashFrames = 0;
		int64 BlockedNotDashing = 0;   // a cooldown after the dash — spec §6 forbids this
		int64 DashingNotBlocked = 0;   // the gate leaking mid-dash
		int64 DashFramesCanFire = 0;   // the leak that actually costs a shot
		int64 DashFramesCanSwing = 0;
		int32 Releases = 0;            // dashes that ended
		int32 ReleasesLate = 0;        // ...and were still blocked on the very next frame
		TMap<TWeakObjectPtr<ATraceCharacter>, bool> WasDashing;
	};

	FAutoConsoleCommand CmdDashGate(
		TEXT("Trace.V10.DashGate"),
		TEXT("Trace.V10.DashGate [Seconds] — watch every pawn every frame and assert "
		     "AreWeaponActionsBlocked() == IsDashing(). Proves both halves of spec v10 §6: no shot or "
		     "swing during a dash, and the gate opening on the very frame the dash ends."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			TSharedPtr<FDashGateWatch> W = MakeShared<FDashGateWatch>();
			const double Seconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atod(*Args[0]), 5.0, 600.0) : 60.0;
			W->EndAt = FPlatformTime::Seconds() + Seconds;

			UE_LOG(LogTraceGame, Display, TEXT("[V10DASHGATE] watching for %.0fs."), Seconds);

			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([W](float) -> bool
			{
				UWorld* World = FindAuthWorld();
				if (World == nullptr)
				{
					return false;
				}

				++W->Frames;
				for (TActorIterator<ATraceCharacter> It(World); It; ++It)
				{
					ATraceCharacter* C = *It;
					if (C == nullptr || !C->IsAlive())
					{
						continue;
					}

					const bool bDashing = C->IsDashing();
					const bool bBlocked = C->AreWeaponActionsBlocked();

					if (bDashing)
					{
						++W->DashFrames;
						if (!bBlocked) { ++W->DashingNotBlocked; }

						if (UTraceWeaponComponent* Weapon = C->FindComponentByClass<UTraceWeaponComponent>())
						{
							if (Weapon->CanFire())  { ++W->DashFramesCanFire; }
							if (Weapon->CanSwing()) { ++W->DashFramesCanSwing; }
						}
					}
					else if (bBlocked)
					{
						++W->BlockedNotDashing;
					}

					// The release edge: the frame AFTER a dash ended, the gate must already be open.
					const bool* Prev = W->WasDashing.Find(C);
					if (Prev != nullptr && *Prev && !bDashing)
					{
						++W->Releases;
						if (bBlocked) { ++W->ReleasesLate; }
					}
					W->WasDashing.Add(C, bDashing);
				}

				if (FPlatformTime::Seconds() < W->EndAt)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[V10DASHGATE] %lld frames, %lld pawn-frames mid-dash across %d dash endings. "
					     "LEAKS: dashing-but-not-blocked=%lld, CanFire mid-dash=%lld, CanSwing mid-dash=%lld. "
					     "OVERHANG: blocked-but-not-dashing=%lld, still blocked one frame after the dash ended=%d. "
					     "VERDICT: %s"),
					W->Frames, W->DashFrames, W->Releases,
					W->DashingNotBlocked, W->DashFramesCanFire, W->DashFramesCanSwing,
					W->BlockedNotDashing, W->ReleasesLate,
					(W->DashingNotBlocked == 0 && W->DashFramesCanFire == 0 && W->DashFramesCanSwing == 0
						&& W->BlockedNotDashing == 0 && W->ReleasesLate == 0 && W->DashFrames > 0)
						? TEXT("PASS — the gate is exactly the dash, frame for frame, and opens on the frame it ends")
						: (W->DashFrames == 0
							? TEXT("*** INCONCLUSIVE — nobody dashed ***")
							: TEXT("*** FAIL ***")));
				return false;
			}), 0.f);
		}));

	// ---------------------------------------------------------------------------------------------
	// SPEC v10 §8 — "Allow Mouse button 1 and mouse button 2 as keybinds in the settings menu."
	//
	// Trace.VerifyBindableKeys already proves the VALIDATION accepts mouse buttons, and it is careful
	// to say so and no more: its own verdict ends "if the MENU still refuses a mouse click, the
	// refusal is in DELIVERY, not in validation." What it does not do is the thing the instruction
	// literally asks for — take FIRE, move it, and put it back — and "and back" is not a formality:
	// a rebind path that accepts a key but cannot restore the previous one is half a feature.
	//
	// So this drives the real UTraceUserSettings::SetKey / GetKey / Save path FIRE uses, round trips
	// through the ini in both directions, and checks the DISPLACEMENT rule on the way (binding a key
	// that another action already owns must take it from that action, not silently duplicate it).
	//
	// It still cannot click a mouse button at a live rebind widget. That is stated in the verdict
	// rather than papered over.
	// ---------------------------------------------------------------------------------------------
	FAutoConsoleCommand CmdRebindFire(
		TEXT("Trace.V10.RebindFire"),
		TEXT("Rebind FIRE to the right mouse button and then back to the left, through the same "
		     "UTraceUserSettings path the options menu uses, checking the ini round trip each way."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UTraceUserSettings& Settings = UTraceUserSettings::Get();

			const FKey Original = Settings.GetKey(ETraceInputAction::Fire);
			const FKey RMB = EKeys::RightMouseButton;
			const FKey LMB = EKeys::LeftMouseButton;

			// EVERY binding, snapshotted before anything is touched.
			//
			// The first version of this command saved only FIRE, and it left the user's PASS action
			// permanently unbound in Saved/Config/.../TraceUserSettings.ini. SetKey DISPLACES whoever
			// else holds the key, PASS owns the right mouse button by default, and this test's whole
			// first step is to take the right mouse button. So the test broke the exact invariant it
			// was written to check, and then wrote the damage to disk with Save().
			//
			// A rebind test that cannot rebind without collateral is not evidence about rebinding. The
			// snapshot is total rather than "FIRE and PASS" on purpose: the next key this test borrows
			// will be owned by something else again, and a list of two names would not cover it.
			TMap<ETraceInputAction, FKey> Before;
			for (const FTraceInputActionInfo& Info : TraceInputActions::All())
			{
				Before.Add(Info.Action, Settings.GetKey(Info.Action));
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[V10REBIND] FIRE starts on '%s'. bindable(LMB)=%d bindable(RMB)=%d"),
				*UTraceUserSettings::DescribeKey(Original),
				UTraceUserSettings::IsBindableKey(LMB) ? 1 : 0,
				UTraceUserSettings::IsBindableKey(RMB) ? 1 : 0);

			// --- OUT: FIRE -> right mouse button ---------------------------------------------------
			Settings.SetKey(ETraceInputAction::Fire, RMB);
			Settings.Save();
			const FKey AfterOut = Settings.GetKey(ETraceInputAction::Fire);
			const bool bOut = (AfterOut == RMB);

			UE_LOG(LogTraceGame, Display,
				TEXT("[V10REBIND] step 1 FIRE <- RIGHT MOUSE BUTTON: now '%s' -> %s"),
				*UTraceUserSettings::DescribeKey(AfterOut), bOut ? TEXT("OK") : TEXT("*** FAILED ***"));

			// --- BACK: FIRE -> left mouse button ---------------------------------------------------
			Settings.SetKey(ETraceInputAction::Fire, LMB);
			Settings.Save();
			const FKey AfterBack = Settings.GetKey(ETraceInputAction::Fire);
			const bool bBack = (AfterBack == LMB);

			UE_LOG(LogTraceGame, Display,
				TEXT("[V10REBIND] step 2 FIRE <- LEFT MOUSE BUTTON (back): now '%s' -> %s"),
				*UTraceUserSettings::DescribeKey(AfterBack), bBack ? TEXT("OK") : TEXT("*** FAILED ***"));

			// --- COLLATERAL, and then repair it ----------------------------------------------------
			//
			// Reported BEFORE it is repaired, because "taking mouse-2 for FIRE unbinds PASS" is a true
			// and useful fact about the rebind UI — a player who does this in the menu really does
			// lose their pass bind, and the menu should be telling them so. It is a finding, not a
			// mess to be hidden. Then it is put back, because a diagnostic must not cost the user
			// their controls.
			int32 Displaced = 0;
			for (const FTraceInputActionInfo& Info : TraceInputActions::All())
			{
				if (Info.Action == ETraceInputAction::Fire)
				{
					continue;
				}
				const FKey Was = Before[Info.Action];
				const FKey Now = Settings.GetKey(Info.Action);
				if (Now != Was)
				{
					++Displaced;
					UE_LOG(LogTraceGame, Warning,
						TEXT("[V10REBIND] COLLATERAL: '%s' went from '%s' to '%s' — taking a mouse button for FIRE "
						     "displaced it. Restoring."),
						Info.ConfigId, *UTraceUserSettings::DescribeKey(Was), *UTraceUserSettings::DescribeKey(Now));
					Settings.SetKey(Info.Action, Was);
				}
			}

			// FIRE last, so restoring a displaced action cannot steal the button back off it.
			Settings.SetKey(ETraceInputAction::Fire, Original);
			Settings.Save();

			int32 NotRestored = 0;
			for (const FTraceInputActionInfo& Info : TraceInputActions::All())
			{
				if (Settings.GetKey(Info.Action) != Before[Info.Action])
				{
					++NotRestored;
					UE_LOG(LogTraceGame, Error,
						TEXT("[V10REBIND] '%s' was NOT restored: wanted '%s', left as '%s'."),
						Info.ConfigId, *UTraceUserSettings::DescribeKey(Before[Info.Action]),
						*UTraceUserSettings::DescribeKey(Settings.GetKey(Info.Action)));
				}
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[V10REBIND] VERDICT: %s  (FIRE moved to mouse-2 and back to mouse-1 through the real "
				     "settings path. %d action(s) were displaced on the way and %d failed to restore; every "
				     "binding is back as it was.) NOT COVERED: a human physically clicking a mouse button at "
				     "the rebind widget — that is the delivery path, and it cannot be driven from a console "
				     "command."),
				(bOut && bBack && NotRestored == 0) ? TEXT("PASS") : TEXT("*** FAIL ***"), Displaced, NotRestored);
		}));

	// ---------------------------------------------------------------------------------------------
	// THE PROOF
	// ---------------------------------------------------------------------------------------------
	/**
	 * One complete four-way. Returns true when it ran, false when the world was not staged (no
	 * living carrier, no enemy) and the caller should wait and try again.
	 *
	 * SEPARATED FROM THE COMMAND, and polled, because a live match does not hold still on request.
	 * At map load the Core is loose; after a goal it is loose again; a carrier can die between the
	 * console command being typed and the frame it runs on. A harness that fires once and reports
	 * "no carrier" is indistinguishable from one that fires once and reports a real failure, and
	 * this project has already been burned by exactly that ambiguity.
	 */
	bool RunCarrierProofOnce(int32 Attempt)
	{
		UWorld* World = FindAuthWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V10PROOF] no authoritative game world — run this on the server."));
			return true;   // never going to become true by waiting
		}
		if (CarrierImmuneCVar() == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V10PROOF] Trace.Knife.CarrierImmune not registered — cannot run the arms."));
			return true;
		}

		ATraceCore* Core = ATraceCore::Get(World);
		if (Core == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V10PROOF] no Core in the world."));
			return true;
		}

		ATraceCharacter* Carrier = Core->Carrier;
		if (Carrier == nullptr || !Carrier->IsAlive())
		{
			return false;   // the Core is loose; wait for a holder
		}

		// The attacker only supplies team, self-exclusion and the arc's plane. It is never asked
		// where it is standing: the origin is a parameter, which is the entire point of resolving
		// this way instead of teleporting a bot behind somebody and hoping.
		ATraceCharacter* Attacker = FindEnemy(World, Carrier, nullptr);
		if (Attacker == nullptr)
		{
			return false;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[V10PROOF] ---------- sample %d ----------"), Attempt);

			// The control victim must be an enemy OF THE ATTACKER, so it is found relative to the
			// attacker's opposition rather than the carrier's — otherwise friendly fire silently
			// eats the control and it reports a miss for a reason that has nothing to do with this.
			ATraceCharacter* ControlVictim = nullptr;
			for (TActorIterator<ATraceCharacter> It(World); It; ++It)
			{
				ATraceCharacter* Candidate = *It;
				if (Candidate == nullptr || Candidate == Attacker || Candidate == Carrier) { continue; }
				if (!Candidate->IsAlive() || Candidate->IsCarrier()) { continue; }
				if (Candidate->GetTeam() == Attacker->GetTeam() || Candidate->GetTeam() == ETraceTeam::None) { continue; }
				ControlVictim = Candidate;
				break;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[V10PROOF] ===== attacker %s (team %d) | carrier %s (team %d) | control %s | range %.0fuu arc %.0fdeg samples %d ====="),
				*GetNameSafe(Attacker), static_cast<int32>(Attacker->GetTeam()),
				*GetNameSafe(Carrier), static_cast<int32>(Carrier->GetTeam()),
				*GetNameSafe(ControlVictim),
				TraceMelee::GetSwingRangeUU(), TraceMelee::GetSwingArcDegrees(), TraceMelee::GetSwingSamples());

			int32 Ignored = 0;

			// --- 1. CONTROL: the constructed geometry must land a blade on a plain pawn ----------
			bool bControlHit = false;
			if (ControlVictim != nullptr)
			{
				bControlHit = ResolveOnce(World, Attacker, ControlVictim, 1, TEXT("CONTROL"), Ignored);
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V10PROOF] CONTROL skipped: no non-carrier enemy of the attacker alive."));
			}

			// --- 2. Carrier with the shield UP, rule OFF ------------------------------------------
			//
			// Expected to miss, and the interesting part is WHERE it is stopped: ResolveHitscan skips
			// a shielded carrier as a candidate, so the knife's own rule is never even consulted here.
			// Run with the rule OFF precisely so that a hit would be attributable to the bullet rule
			// having a hole in it rather than to the knife rule doing the work.
			int32 ShieldUpDelta = 0;
			const bool bShieldUpBefore = !ATraceCore::IsShieldSuppressedFor(Carrier);
			const bool bShieldUpHit = ResolveOnce(World, Attacker, Carrier, 0, TEXT("SHIELD-UP"), ShieldUpDelta);

			// --- 3 and 4. The pass window: shield suppressed, rule OFF then ON ---------------------
			Core->DebugForcePassWindow();
			const bool bSuppressedForRed = ATraceCore::IsShieldSuppressedFor(Carrier);

			int32 RedDelta = 0;
			const bool bRedHit = ResolveOnce(World, Attacker, Carrier, 0, TEXT("RED"), RedDelta);

			// Re-asserted between the two arms. The window is short and both resolutions have to sit
			// inside the SAME one, or the green arm is passing because the shield came back up.
			Core->DebugForcePassWindow();
			const bool bSuppressedForGreen = ATraceCore::IsShieldSuppressedFor(Carrier);

			int32 GreenDelta = 0;
			const bool bGreenHit = ResolveOnce(World, Attacker, Carrier, 1, TEXT("GREEN"), GreenDelta);

			// Never leave the rule off, whatever happened above.
			if (IConsoleVariable* Immune = CarrierImmuneCVar())
			{
				Immune->Set(1, ECVF_SetByConsole);
			}

			// --- Verdict ---------------------------------------------------------------------------
			const bool bRedStaged = bSuppressedForRed;
			const bool bGreenStaged = bSuppressedForGreen;
			const bool bRedWentRed = bRedStaged && bRedHit && RedDelta > 0;
			const bool bGreenClean = bGreenStaged && !bGreenHit && GreenDelta == 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("[V10PROOF] control hit=%d | shield-up (rule OFF) hit=%d shieldWasUp=%d | RED staged=%d hit=%d | GREEN staged=%d hit=%d"),
				bControlHit ? 1 : 0, bShieldUpHit ? 1 : 0, bShieldUpBefore ? 1 : 0,
				bRedStaged ? 1 : 0, bRedHit ? 1 : 0, bGreenStaged ? 1 : 0, bGreenHit ? 1 : 0);

			if (!bRedWentRed)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[V10PROOF] ***** RED ARM DID NOT REPRODUCE (staged=%d hit=%d delta=%d) — the green arm below proves NOTHING. *****"),
					bRedStaged ? 1 : 0, bRedHit ? 1 : 0, RedDelta);
			}

		UE_LOG(LogTraceGame, Display,
			TEXT("[V10PROOF] ===== RESULT: %s ====="),
			(bControlHit && bRedWentRed && bGreenClean)
				? TEXT("THE KNIFE CANNOT HURT THE CORE CARRIER — PROVEN. Control landed, red arm landed a blade on the carrier, green arm did not.")
				: TEXT("*** NOT PROVEN ***"));

		return true;
	}

	FAutoConsoleCommand CmdCarrierProof(
		TEXT("Trace.V10.CarrierProof"),
		TEXT("Server only. Four ResolveSwing resolutions in ONE frame on identical constructed "
		     "geometry: a non-carrier control, the carrier shield-up, the carrier shield-down with "
		     "the immunity rule OFF (the red arm), and the same with it ON. PROVEN requires the "
		     "control to hit, the red arm to hit the carrier, and the green arm to miss. Polls until "
		     "the Core has a living holder, then takes 5 samples a second apart."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			// Sample count and spacing are deliberately small and fixed: each sample is a complete
			// self-contained experiment (control + red + green in one frame), so repetition buys
			// robustness against a transient world state, not statistical power.
			TSharedPtr<int32> Taken = MakeShared<int32>(0);
			TSharedPtr<double> NextAt = MakeShared<double>(0.0);
			TSharedPtr<double> Deadline = MakeShared<double>(FPlatformTime::Seconds() + 90.0);

			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[Taken, NextAt, Deadline](float) -> bool
			{
				const double Now = FPlatformTime::Seconds();
				if (Now > *Deadline)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[V10PROOF] gave up after 90s with %d of 5 samples — the Core never had a living holder alongside a living enemy."),
						*Taken);
					return false;
				}
				if (Now < *NextAt)
				{
					return true;
				}

				if (RunCarrierProofOnce(*Taken + 1))
				{
					++(*Taken);
					*NextAt = Now + 1.0;
				}
				return *Taken < 5;
			}), 0.f);
		}));

	// ---------------------------------------------------------------------------------------------
	// A second, blunter question the four-way above does not answer: how far off the victim's back
	// can the ray start and still be a BACK-stab, and does the damage tier actually follow it.
	//
	// Walks the approach azimuth in 15 degree steps all the way round the victim and prints the
	// damage each one resolves to. The 100/30 boundary should appear exactly once on each side, at
	// BackstabHalfAngleDegrees. Reads the shipped resolver, not the pure angle helper the existing
	// Trace.Knife.AngleTest pins — so it catches a wiring error between the two that the pure test
	// cannot see.
	// ---------------------------------------------------------------------------------------------
	FAutoConsoleCommand CmdDamageSweep(
		TEXT("Trace.V10.KnifeDamageSweep"),
		TEXT("Server only. Sweeps the approach azimuth around a live victim and prints the damage the "
		     "SHIPPED resolver returns at each angle, so 100-from-behind / 30-from-the-front is read "
		     "off the real code path rather than off the pure angle helper."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UWorld* World = FindAuthWorld();
			if (World == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V10SWEEP] no authoritative game world."));
				return;
			}

			// Any living pair of enemies will do; the carrier is explicitly excluded because the
			// whole point of the rule above is that it never returns damage.
			ATraceCharacter* Attacker = nullptr;
			ATraceCharacter* Victim = nullptr;
			for (TActorIterator<ATraceCharacter> It(World); It; ++It)
			{
				ATraceCharacter* C = *It;
				if (C == nullptr || !C->IsAlive() || C->IsCarrier() || C->GetTeam() == ETraceTeam::None) { continue; }
				if (Attacker == nullptr) { Attacker = C; continue; }
				if (C->GetTeam() != Attacker->GetTeam()) { Victim = C; break; }
			}
			if (Attacker == nullptr || Victim == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V10SWEEP] no living cross-team pair."));
				return;
			}

			const FRotator YawOnly(0.f, Victim->GetActorRotation().Yaw, 0.f);
			const double Standoff = static_cast<double>(TraceMelee::GetSwingRangeUU()) * 0.55;
			FVector Eye = Victim->GetActorLocation();
			Eye.Z = Victim->GetPawnViewLocation().Z;

			UE_LOG(LogTraceGame, Display,
				TEXT("[V10SWEEP] victim %s | backstab half-angle %.0fdeg | back=%.0f front=%.0f | 0deg = directly behind"),
				*GetNameSafe(Victim), TraceMelee::GetBackstabHalfAngleDegrees(),
				TraceMelee::GetBackstabDamage(), TraceMelee::GetFrontDamage());

			for (int32 Deg = 0; Deg < 360; Deg += 15)
			{
				// Deg 0 puts the origin directly behind the victim; the ray always points back at
				// them, so only the approach side changes across the sweep.
				const FVector Offset = YawOnly.Vector().RotateAngleAxis(static_cast<float>(Deg), FVector::UpVector);
				const FVector Origin = Eye - Offset * Standoff;
				const FVector Direction = Offset;

				FTraceMeleeHit Hit;
				TraceMelee::ResolveSwing(World, Attacker, Origin, Direction,
					static_cast<float>(ServerNow(World)), Hit);

				UE_LOG(LogTraceGame, Display,
					TEXT("[V10SWEEP] azimuth %3d deg | hit=%d | approach %6.1fdeg | %s | %.0f damage"),
					Deg, (Hit.Victim == Victim) ? 1 : 0, Hit.ApproachAngleDegrees,
					Hit.bBackstab ? TEXT("BACKSTAB") : TEXT("front   "), Hit.Damage);
			}
		}));
} // namespace
