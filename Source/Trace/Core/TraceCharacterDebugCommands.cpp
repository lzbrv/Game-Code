// Trace — ATraceCharacter's development console commands. See TraceCharacter.h for the pawn itself.
//
// WHAT IS IN HERE AND WHY IT IS NOT IN TraceCharacter.cpp. Fourteen `#if !UE_BUILD_SHIPPING`
// console commands — the view/aim probe, the anim probe, the body-mesh census, the pawn-watching
// camera, the viewmodel census, the crouch/take-core/aim/pass-target reproductions, the two gun
// commands, the two hands commands, and the out-of-bounds harness. They are two and a half thousand
// lines of measurement that never run in a shipped build, and they were half of what made
// TraceCharacter.cpp a ten-thousand-line file nobody could hold in their head. Moving them out
// changed nothing about them: this file is a verbatim relocation (RESTRUCTURE tranche D1) of the
// block that used to sit at the bottom of TraceCharacter.cpp, guards, banners and essays included.
//
// THE SEAM IS ATraceCharacter'S PUBLIC READ-BACK SURFACE, and that is deliberate. Nothing here is a
// member of the pawn, nothing here is a friend, and nothing here can reach into a private. Every
// command asks the same questions any other caller could ask — GetShownGun(), UsesPackHands(),
// GetHandsClipName(), IsCarrier() — which is exactly why these harnesses are worth anything: a
// probe that could see privates would be measuring the implementation instead of the behaviour.
// If a command here needs something the pawn does not expose, the fix is a new accessor on the
// pawn, not a friend declaration.
//
// The measured constants the commands grade against (the SMG's muzzle offset, the hands clips'
// emissive bands, the Mannequin's asset path) come from TraceCharacterInternal.h, which is the ONE
// copy the pawn is also built from. A harness carrying its own private copy of a number cannot fail.

#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterInternal.h"      // the measured layout/asset tables and this file's CVars

#include "Animation/AnimInstance.h"
#include "Camera/CameraActor.h"                // Trace.Characters.Watch films from a free camera
#include "Camera/CameraComponent.h"            // Trace.DebugViewProbe compares the CAMERA to the aim
#include "Components/CapsuleComponent.h"       // ... and the capsule half-height to the eye height
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"                 // FTSTicker: every command here runs over frames
#include "DrawDebugHelpers.h"                  // ... and labels its subject in the frame
#include "Engine/Engine.h"                     // GEngine: find whichever world is actually playing
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                       // TActorIterator (Trace.DebugAnimProbe)
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"               // FAutoConsoleCommand
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/PackageName.h"                  // DoesPackageExist (the character-art first-run check)

#include "Abilities/TraceAbilityComponent.h"    // Trace.Bounds.Verify: "the cooldown kept ticking"
#include "Core/TraceCharacterRoster.h"          // the per-character body mesh path and its yaw
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "GameFramework/SpringArmComponent.h"   // the third-person arm length, printed by the probe
#include "Movement/TraceCharacterMovementComponent.h"
#include "Settings/TraceUserSettings.h"        // spec v32 §7d: Trace.ViewModel.Equip reads the live binds
#include "Trace.h"
#include "World/TraceArenaBuilder.h"           // the field bounds Trace.Bounds.Verify shoves a pawn past

// =================================================================================================
// Debug console command
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace
{
	/** Shared by both debug commands below: whichever world is actually playing. */
	UWorld* FindDebugGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/** The local player's pawn, or null. */
	ATraceCharacter* FindDebugLocalCharacter(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (PC != nullptr && PC->IsLocalController())
			{
				if (ATraceCharacter* TraceChar = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					return TraceChar;
				}
			}
		}
		return nullptr;
	}

	/**
	 * Trace.DebugViewProbe [IntervalSeconds] [Samples]
	 *
	 * Prints, at Display so it survives an automated run's default verbosity, the two numbers that
	 * decide whether this feature works and that a screenshot cannot show:
	 *
	 *   eyeErr — distance from the CAMERA to GetPawnViewLocation(), the point the shot is built
	 *            from. In first person this must be 0: the camera IS the gun. In third person it is
	 *            the arm length, which is the whole point of third person and is fine, because a
	 *            carrier cannot fire.
	 *   aimErr — angle between the camera's forward vector and GetAimDirection(). This must be ~0
	 *            in BOTH modes, or the crosshair is lying about where the bullet goes.
	 *
	 * A screenshot proves the view changed; this proves the view did not take the aim with it.
	 */
	FAutoConsoleCommand CmdDebugViewProbe(
		TEXT("Trace.DebugViewProbe"),
		TEXT("Trace.DebugViewProbe [IntervalSeconds] [Samples] — log camera vs aim agreement in whichever view mode is active."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Interval = (Args.Num() > 0) ? FMath::Max(0.05f, FCString::Atof(*Args[0])) : 1.f;
			const int32 Samples = (Args.Num() > 1) ? FMath::Max(1, FCString::Atoi(*Args[1])) : 30;

			int32 Emitted = 0;
			double SinceLast = 0.0;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Emitted, SinceLast, Interval, Samples](float DeltaTime) mutable -> bool
				{
					SinceLast += DeltaTime;
					if (SinceLast < Interval)
					{
						return true;
					}
					SinceLast = 0.0;

					ATraceCharacter* TraceChar = FindDebugLocalCharacter(FindDebugGameWorld());
					if (TraceChar == nullptr || TraceChar->Camera == nullptr)
					{
						return (++Emitted < Samples);
					}

					const FVector CameraLoc = TraceChar->Camera->GetComponentLocation();
					const FVector CameraFwd = TraceChar->Camera->GetForwardVector();
					const FVector EyeLoc = TraceChar->GetPawnViewLocation();
					const FVector AimDir = TraceChar->GetAimDirection();

					const double EyeError = FVector::Dist(CameraLoc, EyeLoc);
					const double AimErrorDegrees = FMath::RadiansToDegrees(
						FMath::Acos(FMath::Clamp(FVector::DotProduct(CameraFwd, AimDir), -1.0, 1.0)));

					// The Core's own answer, printed next to the pawn's mirror of it. THE TWO MUST
					// AGREE. carrier=1 with coreHolder pointing at somebody else is precisely the
					// "stuck in third person" bug: the camera is a pure function of the mirror, so a
					// mirror that outlives the possession strands the view behind the player forever.
					// passHeld is here for the same reason — a latched mouse1 with nobody's finger on
					// it can start a pass on its own, and nothing else in the game shows it.
					const ATraceCore* ProbeCore = ATraceCore::Get(TraceChar->GetWorld());
					const ATraceCharacter* ProbeHolder = (ProbeCore != nullptr) ? ProbeCore->GetCarrier() : nullptr;

					// SPEC v26 §4 — the beam's start, as an ANGLE OFF THE CROSSHAIR, because that is
					// the thing the report was about and the thing a screenshot can be checked against:
					// "muzDeg=(19.2r, 11.6d)" says the tracer leaves 19.2 degrees right and 11.6 degrees
					// below the centre of the frame, which is where the barrel is drawn.
					//
					// RAW and DRAWN are both printed and they are DELIBERATELY DIFFERENT. Raw is where
					// the muzzle component sits; drawn is where the first-person re-projection puts it
					// on screen. If those two ever print the same numbers, the morph is not being
					// applied and the beam is back to leaving from beside the barrel — a green-looking
					// probe that cannot see the thing it is checking, which this project has been
					// bitten by before.
					auto ProbeMuzzleAngles = [TraceChar](const FVector& World, double& OutRight, double& OutDown)
					{
						const FVector Local = TraceChar->Camera->GetComponentTransform().InverseTransformPosition(World);
						OutRight = FMath::RadiansToDegrees(FMath::Atan2(Local.Y, FMath::Max(Local.X, 1.0)));
						OutDown = FMath::RadiansToDegrees(FMath::Atan2(-Local.Z, FMath::Max(Local.X, 1.0)));
					};

					FVector MuzzleRaw = FVector::ZeroVector;
					FVector MuzzleDrawn = FVector::ZeroVector;
					const bool bHasRaw = TraceChar->DebugGetViewModelMuzzleRaw(MuzzleRaw);
					const bool bHasDrawn = TraceChar->GetViewModelMuzzleViewPoint(MuzzleDrawn);

					double RawRight = 0.0, RawDown = 0.0, DrawnRight = 0.0, DrawnDown = 0.0;
					if (bHasRaw)   { ProbeMuzzleAngles(MuzzleRaw, RawRight, RawDown); }
					if (bHasDrawn) { ProbeMuzzleAngles(MuzzleDrawn, DrawnRight, DrawnDown); }

					// crouch / eye / viewmodel are logged alongside because they are the three things
					// that can silently break the aim guarantee or the new viewmodel and that a
					// screenshot cannot distinguish: a crouch that never engaged looks exactly like
					// one that did if the eye height is not printed, and a viewmodel hidden by a
					// visibility bug looks exactly like one that was never built.
					UE_LOG(LogTraceGame, Display,
						TEXT("[ViewProbe] mode=%s carrier=%d coreHolder=%s holderIsMe=%d passActive=%d passHeld=%d predicted=%d ")
						TEXT("blend=%.2f arm=%.1f eyeErr=%.2fuu aimErr=%.4fdeg ")
						TEXT("bodyHiddenFromOwner=%d ctrlYaw=%d orientToMove=%d ")
						TEXT("crouched=%d sliding=%d halfHeight=%.1f baseEye=%.1f vmParts=%d vmVisible=%d ")
						TEXT("muzRawDeg=(%.1fr,%.1fd) muzDrawnDeg=(%.1fr,%.1fd) muzDepth=%.1fuu"),
						TraceChar->GetViewBlendAlpha() < 0.5f ? TEXT("FIRST") : TEXT("THIRD"),
						TraceChar->IsCarrier() ? 1 : 0,
						*GetNameSafe(ProbeHolder),
						(ProbeHolder == TraceChar) ? 1 : 0,
						(ProbeCore != nullptr && ProbeCore->IsPassActive()) ? 1 : 0,
						(ProbeCore != nullptr && ProbeCore->IsPassInputHeld()) ? 1 : 0,
						(ProbeCore != nullptr && ProbeCore->IsPassLocallyPredicted()) ? 1 : 0,
						TraceChar->GetViewBlendAlpha(),
						TraceChar->SpringArm != nullptr ? TraceChar->SpringArm->TargetArmLength : -1.f,
						EyeError,
						AimErrorDegrees,
						(TraceChar->GetMesh() != nullptr && TraceChar->GetMesh()->bOwnerNoSee) ? 1 : 0,
						TraceChar->bUseControllerRotationYaw ? 1 : 0,
						(TraceChar->GetCharacterMovement() != nullptr && TraceChar->GetCharacterMovement()->bOrientRotationToMovement) ? 1 : 0,
						TraceChar->bIsCrouched ? 1 : 0,
						(TraceChar->GetTraceMovement() != nullptr && TraceChar->GetTraceMovement()->IsSliding()) ? 1 : 0,
						TraceChar->GetCapsuleComponent() != nullptr ? TraceChar->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : -1.f,
						TraceChar->BaseEyeHeight,
						TraceChar->GetViewModelPartCount(),
						TraceChar->IsViewModelVisible() ? 1 : 0,
						RawRight, RawDown, DrawnRight, DrawnDown,
						bHasDrawn ? FVector::Dist(CameraLoc, MuzzleDrawn) : -1.0);

					return (++Emitted < Samples);
				}),
				0.f);
		}));

	/**
	 * Trace.DebugAnimProbe [Seconds] [SampleInterval]
	 *
	 * "ARE THE CHARACTERS ACTUALLY ANIMATING?" — answered with a measurement instead of an opinion.
	 *
	 * A screenshot cannot tell a T-pose from a run cycle caught at its neutral frame, and it
	 * certainly cannot tell an idle loop from a run loop. Both of those failures have a specific,
	 * plausible cause in this project: the art is imported per developer (see the file header), so
	 * a machine can have SKM_Manny_Simple present and ABP_Unarmed absent, which renders a perfectly
	 * good-looking, perfectly motionless human.
	 *
	 * So this samples the actual POSE. It watches foot_l and hand_r in COMPONENT space — component,
	 * not world, so walking the actor across the pitch contributes nothing and only the animation
	 * can move them — and reports the travel of each. The pass/fail is unambiguous:
	 *
	 *     range ~0 uu  -> no anim instance, or a single static frame (T-pose / bind pose)
	 *     range >10 uu -> limbs are being posed; a Manny run cycle swings a foot ~60-90 uu
	 *
	 * It prefers whichever character is actually MOVING, because an idle pose legitimately barely
	 * moves the feet and would read as a failure. Display verbosity throughout: this is a diagnostic
	 * whose whole job is to be readable in a log somebody is already looking at.
	 */
	FAutoConsoleCommand CmdDebugAnimProbe(
		TEXT("Trace.DebugAnimProbe"),
		TEXT("Trace.DebugAnimProbe [Seconds] [SampleInterval] — measure whether the character anim blueprint is really posing limbs."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Seconds = (Args.Num() > 0) ? FMath::Max(0.5f, FCString::Atof(*Args[0])) : 6.f;
			const float Interval = (Args.Num() > 1) ? FMath::Max(0.02f, FCString::Atof(*Args[1])) : 0.1f;

			// Armed-line at Display, for the same reason every other diagnostic in this file has one:
			// a probe that prints nothing is indistinguishable from a probe that never ran, and this
			// one is deliberately silent until it has an answer.
			UE_LOG(LogTraceGame, Display, TEXT("[AnimProbe] armed: sampling for %.1fs every %.2fs."), Seconds, Interval);

			// Captured by value into the ticker; the lambda outlives this scope by design.
			double Elapsed = 0.0;
			double FirstSampleTime = -1.0;
			double LastSampleTime = -1.0e9;
			FBox FootBounds(ForceInit);
			FBox HandBounds(ForceInit);
			int32 Samples = 0;
			float FastestSpeed = 0.f;
			FString SubjectName;
			FString AnimClassName(TEXT("<none>"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[Elapsed, FirstSampleTime, LastSampleTime, FootBounds, HandBounds, Samples, FastestSpeed, SubjectName, AnimClassName, Seconds, Interval](float DeltaTime) mutable -> bool
				{
					Elapsed += DeltaTime;

					// EVERY-FRAME TICKER, sampled on our own clock. Registering with a non-zero ticker
					// period was measured NOT to fire in this project's -ExecCmds path at all (the probe
					// armed and then never ran once), while every other debug command here registers at
					// 0 and works. So: tick every frame, and decide here whether this frame is a sample.
					const bool bSampleThisFrame = (Elapsed - LastSampleTime) >= Interval;
					if (!bSampleThisFrame)
					{
						return true;
					}
					LastSampleTime = Elapsed;

					UWorld* World = FindDebugGameWorld();
					if (World == nullptr)
					{
						return (Elapsed < Seconds);
					}

					// Whoever is moving fastest right now. An idle Manny's feet are planted, so probing
					// a standing player would measure "no animation" on a perfectly healthy build.
					ATraceCharacter* Subject = nullptr;
					float BestSpeed = -1.f;
					for (TActorIterator<ATraceCharacter> It(World); It; ++It)
					{
						ATraceCharacter* Candidate = *It;
						if (Candidate == nullptr || !Candidate->IsAlive() || Candidate->GetMesh() == nullptr)
						{
							continue;
						}
						const float Speed = Candidate->GetVelocity().Size2D();
						if (Speed > BestSpeed)
						{
							BestSpeed = Speed;
							Subject = Candidate;
						}
					}

					if (Subject != nullptr)
					{
						USkeletalMeshComponent* MeshComp = Subject->GetMesh();
						FastestSpeed = FMath::Max(FastestSpeed, BestSpeed);
						SubjectName = Subject->GetName();

						const UAnimInstance* AnimInstance = MeshComp->GetAnimInstance();
						AnimClassName = (AnimInstance != nullptr) ? AnimInstance->GetClass()->GetName() : FString(TEXT("<none>"));

						// Component space: the actor's own travel across the pitch cannot contribute, so
						// anything this measures came from the animation and nothing else.
						//
						// THE BONE NAMES ARE RESOLVED, NOT SPELLED. A pawn's body depends on which
						// character is playing it and "foot_l"/"hand_r" are only the MANNEQUIN's names —
						// on Rocco's rig they are "LeftFoot1"/"RightHand1". Asking for a bone this rig
						// does not have returns the component origin, unmoving, i.e. this probe would
						// report "not animating" about a character that was.
						if (MeshComp->GetSkinnedAsset() != nullptr)
						{
							const FName FootBone = Subject->ResolveBodyBoneName(TEXT("foot_l"));
							const FName HandBone = Subject->ResolveBodyBoneName(TEXT("hand_r"));
							FootBounds += MeshComp->GetSocketTransform(FootBone, RTS_Component).GetLocation();
							HandBounds += MeshComp->GetSocketTransform(HandBone, RTS_Component).GetLocation();
							if (Samples == 0)
							{
								FirstSampleTime = Elapsed;
							}
							++Samples;
						}
					}

					// THE SAMPLING WINDOW STARTS AT THE FIRST CHARACTER, NOT AT THE COMMAND.
					//
					// Measured the hard way: -ExecCmds fires at engine init, the menu map and the level
					// travel then take 15-30s on a loaded machine, and a 14s window had closed before a
					// single pawn existed — the probe honestly reported "NOT ANIMATING" about an empty
					// world. A diagnostic that can produce a false failure is worse than none, so the
					// clock does not start until there is somebody to watch.
					if (Samples == 0)
					{
						if (Elapsed < (Seconds + 30.0))
						{
							return true;
						}

						// Absolute stop, and it SAYS SO. A probe that quietly gives up is how a run
						// gets read as "the feature is fine, nothing was reported".
						UE_LOG(LogTraceGame, Display,
							TEXT("[AnimProbe] gave up after %.0fs: no living character with a skeletal mesh ever ")
							TEXT("appeared. On a machine without the art import that is EXPECTED — the pawns are ")
							TEXT("fallback primitives; see the [CharacterArt] line above."), Elapsed);
						return false;
					}

					if (Elapsed < (FirstSampleTime + Seconds))
					{
						return true;
					}

					const FVector FootTravel = (Samples > 0) ? FootBounds.GetSize() : FVector::ZeroVector;
					const FVector HandTravel = (Samples > 0) ? HandBounds.GetSize() : FVector::ZeroVector;
					const float FootRange = static_cast<float>(FootTravel.GetMax());
					const float HandRange = static_cast<float>(HandTravel.GetMax());

					UE_LOG(LogTraceGame, Display,
						TEXT("[AnimProbe] subject=%s animClass=%s samples=%d peakSpeed=%.0fuu/s ")
						TEXT("foot_l travel=%.1fuu hand_r travel=%.1fuu -> %s"),
						SubjectName.IsEmpty() ? TEXT("<none>") : *SubjectName, *AnimClassName, Samples, FastestSpeed,
						FootRange, HandRange,
						(FootRange > 10.f || HandRange > 10.f)
							? TEXT("ANIMATING (limbs are being posed)")
							: TEXT("NOT ANIMATING (static pose — check the anim blueprint / the art import)"));

					return false;
				}),
				0.f);
		}));

	/**
	 * Trace.ViewModel.Census [NearbyRadiusUU]
	 *
	 * Names every primitive drawn at the local player's hands, and reports the number that decides
	 * how big each one is drawn.
	 *
	 * WHY THIS EXISTS. A first-person rig that looks wrong cannot be diagnosed from a screenshot: a
	 * flat slab across the frame is equally consistent with a mis-scaled hand, a leaked one-shot
	 * effect, and a shape authored in METRES against a component that wanted CENTIMETRES - and this
	 * project has now shipped all three. Every probe that already exists answers for exactly one rig
	 * (Trace.Hands.Probe for the hands, Trace.Knife.StreakProbe for the blade), so a part belonging
	 * to none of them is invisible to all of them, which is precisely the part you are hunting when
	 * the frame contains something nobody recognises.
	 *
	 * TWO PASSES, AND THE SECOND IS THE ONE THAT EARNS ITS KEEP. The first walks the pawn's own
	 * component tree. The second sweeps every OTHER actor in the world for primitives standing inside
	 * the viewmodel's depth range, because a leaked cosmetic actor - a tracer, a muzzle flash, a
	 * halo that never self-deleted - is not a child of ViewModelRoot and a pawn-only walk reports a
	 * perfectly healthy rig while the frame is full of debris.
	 *
	 * SIZE IS REPORTED AS THE DRAWN SIZE: the mesh's own bounds times the component's WORLD scale,
	 * as a full width rather than a half-extent, in uu. That is the number to hold against the FX
	 * doc, whose radii are all in metres - so the metres value is printed beside it on every line.
	 * Doing that conversion here, once, is the whole point: it is the arithmetic the defect hides in.
	 */
	/**
	 * Trace.Characters.BodyMesh
	 *
	 * "WHAT BODY IS EACH PAWN DRAWING, AND WHICH CHARACTER DOES ITS PLAYERSTATE SAY IT IS?"
	 *
	 * THE TWO HALVES ARE THE WHOLE POINT. Either one alone is useless: a log line saying SK_Rocco
	 * loaded proves nothing about the pawn a Rocco player is drawn as on YOUR machine, and a
	 * screenshot of a grey humanoid proves nothing about whose pawn it is. This prints them on the
	 * same line, per pawn, and compares them.
	 *
	 * *** RUN IT ON THE MACHINE THAT IS NOT THE ONE THAT SPAWNED THE PAWN. *** "Shows to others" is a
	 * statement about REMOTE pawns, and the bug this exists to catch — the PlayerState and its ability
	 * component arriving after the pawn, so a once-only apply reads "no character" — cannot happen on
	 * the server, where all three exist before BeginPlay. The ROLE column is therefore the first thing
	 * to read: a run whose every row says Authority has not tested the interesting case. It reports
	 * on every ATraceCharacter in the world, bots included, which is what makes a listen server a
	 * useful place to run it: a bot's pawn is remote-flavoured in every way that matters here.
	 *
	 * THREE VERDICTS PER PAWN, BECAUSE THERE ARE THREE WAYS TO BE WRONG. The MESH column asks whether
	 * the pawn is wearing the body its character calls for; the ANIM column asks whether it is running
	 * the anim class that goes WITH that body; the MATS column asks whether it is being drawn in that
	 * body's OWN materials. They fail independently, and the last two are the ones no screenshot can
	 * settle: a Rocco wearing SK_Rocco with no anim instance is the right shape, the right size and
	 * frozen solid, which in a still frame is indistinguishable from a Rocco standing in his idle —
	 * and a Rocco whose first two slots are still wearing the Mannequin's material is the right SHAPE
	 * and the right COLOUR, because the team tint is written into every MID whatever its parent.
	 *
	 * EACH VERDICT IS A COMPARISON, NOT A DESCRIPTION, and either can go RED:
	 *   FAIL   the pawn is drawing a body — or running an anim class — its character did not ask for.
	 *   SKIP   that asset is not on this machine (a fresh clone that has not run the import, or has
	 *          imported and not retargeted). Not a defect here — each has its own banner and its own
	 *          on-screen warning.
	 *   PASS   everything matches, and at least one pawn was checked.
	 *
	 * THREE RED ARMS, ONE PER COLUMN:
	 *   Trace.Characters.BodyMeshEventsOnly 1  restores the obvious, wrong implementation of the body
	 *     swap (hook the lifecycle events, apply once). Measured: with it on, a connecting client
	 *     reported "FAIL ... says=ROCCO applied=MANNEQUIN" and RESULT *** FAIL ***; with it off, the
	 *     same run reported PASS for all ten pawns including a role=Simulated Rocco.
	 *   Trace.Characters.BodyAnimIgnore 1      offers every pawn Epic's ABP_Unarmed, as this game did
	 *     before the retarget existed, which puts Rocco back in the bind pose this feature was
	 *     reported broken in.
	 *   Trace.Characters.BodyMeshKeepOverrides 1  stops the body swap from clearing the component's
	 *     material overrides, restoring the state where a pawn that has changed body draws some slots
	 *     in the previous body's materials.
	 * A harness whose red and green arms agree is not measuring its rule.
	 */
	FAutoConsoleCommand CmdCharactersBodyMesh(
		TEXT("Trace.Characters.BodyMesh"),
		TEXT("Trace, dev only. Per pawn: which character its PlayerState says it is, and which body it is "
		     "actually drawing. FAILS when the two disagree. Red arm: Trace.Characters.BodyMeshEventsOnly 1."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UWorld* const World = FindDebugGameWorld();
			if (World == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[BodyMesh] no game world."));
				return;
			}

			UE_LOG(LogTraceGame, Display, TEXT("[BodyMesh] ===== what every pawn is wearing ====="));
			// Every red arm's value goes in the header, so a log can never be read as green when one of
			// them was on — the arms are set from the command line and leave no other trace in the file.
			UE_LOG(LogTraceGame, Display,
				TEXT("[BodyMesh] roster source: %s   red arms: EventsOnly=%d AnimIgnore=%d KeepOverrides=%d"),
				TraceCharacterRoster::CurrentSourceName(),
				TraceCharacterBody::CVarBodyMeshEventsOnly.GetValueOnGameThread(),
				TraceCharacterBody::CVarBodyAnimIgnore.GetValueOnGameThread(),
				TraceCharacterBody::CVarBodyMeshKeepOverrides.GetValueOnGameThread());

			int32 Checked = 0;

			// *** HOW MANY PAWNS ACTUALLY EXERCISE THE FEATURE. *** Checked counts Mannequins too, so a
			// match in which nobody picked a character with a bespoke body would print PASS having proved
			// nothing about the thing under test - the most expensive kind of green, and the one this
			// file's own comment two screens down warns about while not actually guarding against it.
			int32 Bespoke = 0;
			int32 Failed = 0;
			int32 Skipped = 0;

			// The anim half is counted SEPARATELY from the mesh half. They fail independently and for
			// different reasons — a pawn can wear exactly the right body and be frozen in it — and a
			// single number would let one hide inside the other.
			int32 AnimChecked = 0;
			int32 AnimFailed = 0;
			int32 AnimSkipped = 0;

			// And the material half, counted separately again for the same reason: a pawn can wear the
			// right body, run the right anim class, and still be drawn in the PREVIOUS body's materials.
			int32 MatChecked = 0;
			int32 MatFailed = 0;

			for (TActorIterator<ATraceCharacter> It(World); It; ++It)
			{
				ATraceCharacter* const Pawn = *It;
				if (Pawn == nullptr || !IsValid(Pawn))
				{
					continue;
				}

				const USkeletalMeshComponent* const MeshComp = Pawn->GetMesh();
				const USkeletalMesh* const DrawnMesh = (MeshComp != nullptr) ? MeshComp->GetSkeletalMeshAsset() : nullptr;

				// WHAT THE PAWN'S OWN PLAYERSTATE SAYS — the replicated selection, asked the same way
				// the apply asks it, so the harness cannot pass by reading a different question.
				uint8 SelectedId = TraceCharacterRoster::NoneId;
				const ATracePlayerState* const TracePS = Pawn->GetPlayerState<ATracePlayerState>();
				if (TracePS != nullptr && TraceCharacterRoster::IsValidId(TracePS->GetSelectedCharacter()))
				{
					SelectedId = TracePS->GetSelectedCharacter();
				}

				// WHAT THAT CHARACTER CALLS FOR — resolved from the roster, i.e. from the assets when
				// the assets are being served and from the C++ table when they are not.
				FString WantedPath;
				FString WantedAnimPath;
				if (const TraceCharacterRoster::FTraceCharacterEntry* Row = TraceCharacterRoster::Find(SelectedId))
				{
					if (Row->BodyMeshPath != nullptr && Row->BodyMeshPath[0] != TEXT('\0'))
					{
						WantedPath = Row->BodyMeshPath;
						WantedAnimPath = (Row->BodyAnimClassPath != nullptr) ? Row->BodyAnimClassPath : TEXT("");
					}
				}

				const TCHAR* const RoleName =
					(Pawn->GetLocalRole() == ROLE_Authority)        ? TEXT("Authority")
					: (Pawn->GetLocalRole() == ROLE_AutonomousProxy) ? TEXT("Autonomous")
					: TEXT("Simulated");

				const FName HandBone = Pawn->ResolveBodyBoneName(TEXT("hand_r"));
				const UAnimInstance* const AnimInstance = (MeshComp != nullptr) ? MeshComp->GetAnimInstance() : nullptr;

				// The verdict for THIS pawn.
				const TCHAR* Verdict = TEXT("PASS");
				if (WantedPath.IsEmpty())
				{
					// The Mannequin is what this character calls for. Matching means the pawn is drawing
					// the mesh CharacterMeshAsset resolved to, which is what "no bespoke body" looks like.
					// COMPARE THE MESH, not just the latch. AppliedBodyCharacterId is written by the
					// apply itself and is KEPT EVEN WHEN THE APPLY FAILED (see the note on it), so a
					// test made only of that latch reduces to "the apply ran once and something is
					// drawn" and would pass on a pawn wearing the wrong body entirely.
					const FString DrawnManneqPath = (DrawnMesh != nullptr)
						? FSoftObjectPath(DrawnMesh).ToString() : FString();
					const bool bIsMannequin =
						DrawnManneqPath == FString(TraceCharacterAssets::MannequinMesh);

					if (DrawnMesh != nullptr && bIsMannequin
						&& Pawn->GetAppliedBodyCharacterId() == SelectedId)
					{
						Verdict = TEXT("PASS");
						++Checked;
					}
					else if (DrawnMesh == nullptr)
					{
						Verdict = TEXT("SKIP");   // art not imported at all; the banner covers it
						++Skipped;
					}
					else
					{
						Verdict = TEXT("FAIL");
						++Failed;
					}
				}
				else
				{
					const FString DrawnPath = (DrawnMesh != nullptr)
						? FSoftObjectPath(DrawnMesh).ToString() : FString(TEXT("<none>"));

					if (DrawnPath == WantedPath)
					{
						Verdict = TEXT("PASS");
						++Checked;
						++Bespoke;
					}
					else if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(WantedPath)))
					{
						// The import has not been run here. NOT a failure of the wiring, and saying so is
						// the difference between this harness being trusted and being ignored.
						Verdict = TEXT("SKIP");
						++Skipped;
					}
					else
					{
						Verdict = TEXT("FAIL");
						++Failed;
					}
				}

				// *** THE HALF A SCREENSHOT CANNOT CHECK. *** A pawn wearing the right mesh with no
				// anim instance is the right shape, the right size and completely frozen; that is the
				// state this whole retarget exists to end, and it looks identical to a healthy pawn in
				// any still frame. So the class on the component is compared to the class the roster
				// names, by path.
				const UClass* const DrawnAnimClass = (AnimInstance != nullptr) ? AnimInstance->GetClass() : nullptr;
				const FString DrawnAnimPath = (DrawnAnimClass != nullptr)
					? FSoftClassPath(DrawnAnimClass).ToString() : FString();

				const TCHAR* AnimVerdict = TEXT("-");
				if (!WantedAnimPath.IsEmpty())
				{
					if (DrawnAnimPath == WantedAnimPath)
					{
						AnimVerdict = TEXT("PASS");
						++AnimChecked;
					}
					else if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(WantedAnimPath)))
					{
						// Scripts/retarget-rocco.sh has not been run here. The banner and the on-screen
						// line already say so; this is not a defect in the wiring.
						AnimVerdict = TEXT("SKIP");
						++AnimSkipped;
					}
					else
					{
						AnimVerdict = TEXT("FAIL");
						++AnimFailed;
					}
				}

				// *** THE THIRD WAY TO BE WRONG, AND THE ONE THE MESH COLUMN CANNOT SEE. *** A material
				// override lives on the COMPONENT, not on the mesh, and SetSkeletalMeshAsset does not
				// clear it — so an override authored for the previous body wins, per slot index, over
				// the new asset's own material. A pawn can therefore be drawing exactly the right body
				// through the wrong materials: Rocco's first two sections in M_Mannequin, or a
				// Mannequin wearing Rocco's placeholder after a switch back. The team tint hides it
				// perfectly, since every MID is given the same colour whatever its parent, which is why
				// this has to be a comparison and not a screenshot. Each slot's material is walked back
				// through its MID parents and the root is required to be what the DRAWN asset names.
				const TCHAR* MatVerdict = TEXT("-");
				int32 StaleSlots = 0;
				if (MeshComp != nullptr && DrawnMesh != nullptr)
				{
					const TArray<FSkeletalMaterial>& AssetMaterials = DrawnMesh->GetMaterials();
					const int32 SlotCount = MeshComp->GetNumMaterials();
					for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
					{
						const UMaterialInterface* Root = MeshComp->GetMaterial(SlotIndex);
						// Bounded rather than while(true): a corrupt parent chain must not hang a debug
						// command. Nothing here nests deeper than MID -> instance -> instance -> material.
						for (int32 Hops = 0; Hops < 8; ++Hops)
						{
							const UMaterialInstanceDynamic* const AsMID = Cast<UMaterialInstanceDynamic>(Root);
							if (AsMID == nullptr) { break; }
							Root = AsMID->Parent;
						}

						const UMaterialInterface* const FromAsset = AssetMaterials.IsValidIndex(SlotIndex)
							? AssetMaterials[SlotIndex].MaterialInterface : nullptr;
						if (FromAsset != nullptr && Root != FromAsset)
						{
							++StaleSlots;
							UE_LOG(LogTraceGame, Error,
								TEXT("[BodyMesh]        slot %d draws %s but %s names %s"),
								SlotIndex, *GetNameSafe(Root), *GetNameSafe(DrawnMesh), *GetNameSafe(FromAsset));
						}
					}

					if (StaleSlots > 0) { MatVerdict = TEXT("FAIL"); ++MatFailed; }
					else                { MatVerdict = TEXT("PASS"); ++MatChecked; }
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[BodyMesh] %-4s %-22s role=%-10s local=%d  says=%-9s applied=%-9s"),
					Verdict, *GetNameSafe(Pawn), RoleName, Pawn->IsLocallyControlled() ? 1 : 0,
					*TraceCharacterRoster::NameFor(SelectedId),
					*TraceCharacterRoster::NameFor(Pawn->GetAppliedBodyCharacterId()));

				UE_LOG(LogTraceGame, Display,
					TEXT("[BodyMesh]        wants=%s"),
					WantedPath.IsEmpty() ? TEXT("(the Mannequin)") : *WantedPath);

				UE_LOG(LogTraceGame, Display,
					TEXT("[BodyMesh]        anim %-4s wants=%s"),
					AnimVerdict,
					WantedAnimPath.IsEmpty() ? TEXT("(ABP_Unarmed)") : *WantedAnimPath);

				UE_LOG(LogTraceGame, Display,
					TEXT("[BodyMesh]        draws=%s  yaw=%.1f  anim=%s  hand_r->%s  slots=%d  visible=%d  mats %s"),
					*GetNameSafe(DrawnMesh),
					(MeshComp != nullptr) ? MeshComp->GetRelativeRotation().Yaw : 0.f,
					(AnimInstance != nullptr) ? *GetNameSafe(AnimInstance->GetClass()) : TEXT("<none: bind pose>"),
					HandBone.IsNone() ? TEXT("<none>") : *HandBone.ToString(),
					(MeshComp != nullptr) ? MeshComp->GetNumMaterials() : 0,
					(MeshComp != nullptr && MeshComp->IsVisible()) ? 1 : 0,
					MatVerdict);
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[BodyMesh] anim classes: %d correct, %d wrong, %d not retargeted on this machine "
				     "(red arm Trace.Characters.BodyAnimIgnore = %d)."),
				AnimChecked, AnimFailed, AnimSkipped,
				TraceCharacterBody::CVarBodyAnimIgnore.GetValueOnGameThread());

			UE_LOG(LogTraceGame, Display,
				TEXT("[BodyMesh] materials: %d pawn(s) drawn entirely in their own body's materials, %d carrying "
				     "an override left behind by a previous body."),
				MatChecked, MatFailed);

			// A harness that reports PASS having looked at nothing is the most expensive kind of green.
			if (Failed > 0 || AnimFailed > 0 || MatFailed > 0)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[BodyMesh] RESULT: *** FAIL *** - %d pawn(s) are drawing a body their character did not ask "
					     "for, %d are not running the anim class it calls for, and %d are wearing another body's "
					     "materials (%d/%d correct, %d/%d skipped)."),
					Failed, AnimFailed, MatFailed, Checked, AnimChecked, Skipped, AnimSkipped);
			}
			else if (Checked == 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[BodyMesh] RESULT: NOT PROVEN - no pawn could be checked (%d skipped). Either no characters "
					     "have spawned yet or the art is not imported on this machine."), Skipped);
			}
			else if (Bespoke == 0)
			{
				// Every pawn matched, and every pawn was a Mannequin. That is a real result about the
				// FALLBACK - which since PIPELINE_DESIGN.md §9.1 means either nobody has picked yet or the
				// bodies are not imported on this machine - but it says nothing about the feature this
				// command exists to check, so it must not read as PASS.
				UE_LOG(LogTraceGame, Warning,
					TEXT("[BodyMesh] RESULT: NOT PROVEN - %d pawn(s) all match, but NONE of them wears a bespoke "
					     "body, so the swap itself was never exercised. Put somebody on a character that has one "
					     "(Trace.Characters.Select) and run this again."), Checked);
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[BodyMesh] RESULT: PASS - %d pawn(s) are drawing exactly the body their character calls for, "
					     "%d of them are running the anim class that goes with it, and %d are in that body's own "
					     "materials (%d/%d skipped)."),
					Checked, AnimChecked, MatChecked, Skipped, AnimSkipped);
			}
		}));

	/**
	 * Trace.Characters.Watch <CharacterId> [Seconds] [DistanceUU]
	 *
	 * *** THE CAMERA WORK, AND IT EXISTS BECAUSE THE DELIVERABLE IS A PICTURE. ***
	 *
	 * "Any player using Rocco shows to others with that model" is a claim about a pawn somebody ELSE
	 * is driving, and the only honest evidence for it is a frame of that pawn taken from a camera
	 * that does not own it. Getting one headlessly is the hard part of the whole task: the local
	 * player's own body is hidden from their own camera, so the subject has to be a bot or a second
	 * client; a bot is a moving target; and -TraceExec fires its whole list at ONE instant, so a
	 * single teleport is framing a pawn that has run off by the time the shutter opens.
	 *
	 * So this is a FOLLOW, not a pose. Every tick for @p Seconds it re-finds the subject and flies a
	 * free camera behind and off its shoulder, aimed at its chest — which keeps a running bot in
	 * frame for as long as -TraceAutoShotRepeat keeps firing. Two or three frames a second apart of
	 * the same bot are also the proof that it is ANIMATING and not sliding, which is the one thing a
	 * single screenshot can never settle.
	 *
	 * *** A FREE CAMERA, NOT A TELEPORTED PAWN, AND THAT IS NOT A DETAIL. *** The first version of
	 * this command moved the local pawn. That works on a listen server and silently does not work on
	 * a CLIENT, where the server owns the pawn's position and corrects every teleport within a frame
	 * or two — so the shutter opened on an empty corridor. A client is exactly where this evidence
	 * has to come from, because "what other players see" is a claim about a role=Simulated pawn. A
	 * view target is local, is never corrected, and leaves the match undisturbed.
	 *
	 * IT WATCHES A BODY, NOT A NAME. The subject is chosen by GetAppliedBodyCharacterId — what the
	 * pawn is actually WEARING — rather than by its PlayerState's selection, so a run where the body
	 * never got applied finds nothing and says so, instead of pointing the camera confidently at a
	 * Mannequin.
	 *
	 * Trace.Arena.Pose is the manual version of this and stays: it takes literal coordinates and is
	 * the right tool for photographing the ARENA, which does not move.
	 */
	struct FCharacterWatchState
	{
		uint8 CharacterId = 0;
		float Remaining = 0.f;
		float Distance = 260.f;
		int32 Frames = 0;
		int32 FramesWithSubject = 0;
		TWeakObjectPtr<ACameraActor> Camera;
	};

	FAutoConsoleCommand CmdCharactersWatch(
		TEXT("Trace.Characters.Watch"),
		TEXT("Trace, dev only. Trace.Characters.Watch <CharacterId> [Seconds] [DistanceUU] - point the local "
		     "view at the nearest pawn WEARING that character's body, so a headless run can photograph what "
		     "other players see. Pair with -TraceAutoShot/-TraceAutoShotRepeat."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FCharacterWatchState State;
			State.CharacterId = (Args.Num() > 0)
				? static_cast<uint8>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 255))
				: TraceCharacterRoster::FirstId;
			State.Remaining = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.5f, 120.f) : 12.f;
			State.Distance  = (Args.Num() > 2) ? FMath::Clamp(FCString::Atof(*Args[2]), 80.f, 2000.f) : 260.f;

			UE_LOG(LogTraceGame, Display,
				TEXT("[Watch] following the nearest pawn wearing %s for %.1fs at %.0fuu."),
				*TraceCharacterRoster::NameFor(State.CharacterId), State.Remaining, State.Distance);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					State.Remaining -= DeltaTime;
					const bool bLastFrame = (State.Remaining <= 0.f);
					++State.Frames;

					UWorld* const World = FindDebugGameWorld();
					APlayerController* Viewer = nullptr;
					if (World != nullptr)
					{
						for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
						{
							APlayerController* const PC = It->Get();
							if (PC != nullptr && PC->IsLocalController())
							{
								Viewer = PC;
								break;
							}
						}
					}
					if (World == nullptr || Viewer == nullptr)
					{
						return !bLastFrame;
					}

					// NEAREST TO THE VIEWER, so a ten-pawn match cannot pick one across the arena and
					// spend the whole capture looking at a wall — but A MOVING ONE FIRST.
					//
					// THAT PREFERENCE IS FRAMING, NOT SCORING, and the distinction matters. A locomotion
					// retarget can only be SEEN on a subject that is walking: the nearest Rocco spent one
					// whole capture idling against a wall, and thirty seconds of a standing figure says
					// nothing about whether it would move if it tried. Nothing here decides a verdict —
					// Trace.Characters.BodyMesh does that, over every pawn, moving or not.
					constexpr double MovingUUPerSecond = 100.0;
					const FVector From = Viewer->GetPawn() != nullptr
						? Viewer->GetPawn()->GetActorLocation() : Viewer->GetFocalLocation();
					ATraceCharacter* Subject = nullptr;
					double Best = TNumericLimits<double>::Max();
					bool bBestIsMoving = false;
					for (TActorIterator<ATraceCharacter> It(World); It; ++It)
					{
						ATraceCharacter* const Candidate = *It;
						if (Candidate == nullptr || !IsValid(Candidate) || Candidate == Viewer->GetPawn())
						{
							continue;
						}
						if (Candidate->GetAppliedBodyCharacterId() != State.CharacterId)
						{
							continue;
						}
						const bool bMoving = Candidate->GetVelocity().Size2D() > MovingUUPerSecond;
						if (bBestIsMoving && !bMoving)
						{
							continue;
						}
						const double Away = FVector::Dist(Candidate->GetActorLocation(), From);
						if (Subject == nullptr || (bMoving && !bBestIsMoving) || Away < Best)
						{
							Best = Away;
							bBestIsMoving = bMoving;
							Subject = Candidate;
						}
					}

					if (Subject == nullptr)
					{
						if (bLastFrame)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[Watch] no pawn is WEARING %s (checked %d frame(s)). Either nobody picked it "
								     "or the body was never applied — Trace.Characters.BodyMesh says which."),
								*TraceCharacterRoster::NameFor(State.CharacterId), State.Frames);
						}
					}
					else
					{
						++State.FramesWithSubject;

						// *** A FREE CAMERA, NOT A TELEPORTED PAWN, AND THIS IS THE WHOLE REASON THE
						// FIRST VERSION OF THIS COMMAND PHOTOGRAPHED EMPTY ROOMS. *** Moving the local
						// pawn works on a listen server and does NOT work on a client: the server owns
						// that pawn's position, so every teleport is corrected away within a frame or
						// two and the shutter opens on wherever the server thinks you are. And a client
						// is exactly where this evidence has to come from — "what OTHER players see" is
						// a claim about a role=Simulated pawn. A view target is purely local, is never
						// corrected, and disturbs the match not at all: nobody is shoved into a bot's
						// face to take its picture.
						ACameraActor* Camera = State.Camera.Get();
						if (Camera == nullptr)
						{
							FActorSpawnParameters Params;
							Params.ObjectFlags |= RF_Transient;
							Camera = World->SpawnActor<ACameraActor>(
								ACameraActor::StaticClass(), FTransform::Identity, Params);
							State.Camera = Camera;
						}

						// AHEAD OF THE SUBJECT AND OFF TO ONE SIDE, LOOKING BACK AT IT. Filming from
						// behind was the obvious choice and it is the wrong one: these bots strafe and
						// dash at 800-1150 uu/s, so a camera pinned to the subject's BACK is left
						// staring at empty floor the moment it changes direction, and the whole point of
						// the capture is a subject in motion. Placed down its line of travel instead, a
						// runner comes toward the lens and stays framed. Facing is the fallback for a
						// subject that is standing still, where there is no line of travel to use.
						const FVector Chest = Subject->GetActorLocation() + FVector(0.f, 0.f, 40.f);
						const FVector Travel = Subject->GetVelocity().GetSafeNormal2D();
						const FVector Along = Travel.IsNearlyZero()
							? Subject->GetActorForwardVector() * -1.f : Travel;
						const FVector Spot = Subject->GetActorLocation()
							+ Along * State.Distance
							+ FVector::CrossProduct(FVector::UpVector, Along) * (State.Distance * 0.45f)
							+ FVector(0.f, 0.f, 80.f);
						const FRotator Aim = (Chest - Spot).Rotation();

						if (Camera != nullptr)
						{
							Camera->SetActorLocationAndRotation(Spot, Aim);
							if (Viewer->GetViewTarget() != Camera)
							{
								Viewer->SetViewTargetWithBlend(Camera, 0.f);
							}
						}

						// *** THE FRAME HAS TO SAY WHICH PAWN IT IS. *** Ten pawns are on this map and
						// several are in shot at once; "the detailed one on the left" is somebody
						// squinting at a screenshot, which is the kind of evidence this project has been
						// burned by. So the subject is boxed and captioned IN THE PICTURE, with the mesh
						// and the anim class read off its own component.
						const USkeletalMeshComponent* const SubjectMesh = Subject->GetMesh();
						const UAnimInstance* const SubjectAnim =
							(SubjectMesh != nullptr) ? SubjectMesh->GetAnimInstance() : nullptr;
#if ENABLE_DRAW_DEBUG
						DrawDebugBox(World, Chest, FVector(40.f, 40.f, 90.f), FQuat::Identity,
							FColor::Green, /*bPersistent=*/false, /*LifeTime=*/0.f, /*DepthPriority=*/0,
							/*Thickness=*/2.f);
						DrawDebugString(World, FVector(0.f, 0.f, 130.f),
							FString::Printf(TEXT("%s  role=%s\nbody %s\nanim %s\n%.0f uu/s"),
								*GetNameSafe(Subject),
								(Subject->GetLocalRole() == ROLE_Authority) ? TEXT("Authority")
									: (Subject->GetLocalRole() == ROLE_AutonomousProxy) ? TEXT("Autonomous")
									: TEXT("Simulated"),
								*GetNameSafe((SubjectMesh != nullptr) ? SubjectMesh->GetSkeletalMeshAsset() : nullptr),
								(SubjectAnim != nullptr) ? *GetNameSafe(SubjectAnim->GetClass()) : TEXT("<none: bind pose>"),
								Subject->GetVelocity().Size2D()),
							Subject, FColor::Green, /*Duration=*/0.f, /*bDrawShadow=*/true, /*FontScale=*/1.4f);
#endif

						if (bLastFrame)
						{
							UE_LOG(LogTraceGame, Display,
								TEXT("[Watch] framed %s (role=%s) for %d of %d frame(s): body=%s anim=%s speed=%.0fuu/s"),
								*GetNameSafe(Subject),
								(Subject->GetLocalRole() == ROLE_Authority) ? TEXT("Authority")
									: (Subject->GetLocalRole() == ROLE_AutonomousProxy) ? TEXT("Autonomous")
									: TEXT("Simulated"),
								State.FramesWithSubject, State.Frames,
								*GetNameSafe((SubjectMesh != nullptr) ? SubjectMesh->GetSkeletalMeshAsset() : nullptr),
								(SubjectAnim != nullptr) ? *GetNameSafe(SubjectAnim->GetClass()) : TEXT("<none: bind pose>"),
								Subject->GetVelocity().Size2D());
						}
					}

					// PUT THE VIEW BACK. A capture that left the player watching a bot for the rest of
					// the match would be a debug command that breaks the game it is measuring.
					if (bLastFrame)
					{
						if (Viewer->GetPawn() != nullptr)
						{
							Viewer->SetViewTargetWithBlend(Viewer->GetPawn(), 0.f);
						}
						if (ACameraActor* const Camera = State.Camera.Get())
						{
							Camera->Destroy();
						}
					}
					return !bLastFrame;
				}),
				0.f);
		}));

	FAutoConsoleCommand CmdViewModelCensus(
		TEXT("Trace.ViewModel.Census"),
		TEXT("Trace.ViewModel.Census [NearbyRadiusUU] - name and measure every primitive drawn at the local player's hands."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const double NearbyRadius = (Args.Num() > 0)
				? FMath::Max(1.0, static_cast<double>(FCString::Atof(*Args[0])))
				: 400.0;

			UWorld* CensusWorld = FindDebugGameWorld();
			if (CensusWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Census] No playing world."));
				return;
			}

			ATraceCharacter* CensusPawn = nullptr;
			for (FConstPlayerControllerIterator It = CensusWorld->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* LocalPC = It->Get();
				if (LocalPC != nullptr && LocalPC->IsLocalController())
				{
					CensusPawn = Cast<ATraceCharacter>(LocalPC->GetPawn());
					if (CensusPawn != nullptr)
					{
						break;
					}
				}
			}

			if (CensusPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Census] No locally controlled ATraceCharacter yet."));
				return;
			}

			const FVector EyeWorld = CensusPawn->GetPawnViewLocation();

			// One formatter for both passes, so a pawn part and a piece of loose debris are described
			// in the same units and can be compared without translating between two report formats.
			auto DescribePrimitive = [](UPrimitiveComponent* Prim) -> FString
			{
				FVector LocalExtent = FVector::ZeroVector;
				FString AssetName = TEXT("<no-mesh>");

				if (const UStaticMeshComponent* AsStatic = Cast<UStaticMeshComponent>(Prim))
				{
					if (AsStatic->GetStaticMesh() != nullptr)
					{
						LocalExtent = AsStatic->GetStaticMesh()->GetBounds().BoxExtent;
						AssetName = AsStatic->GetStaticMesh()->GetName();
					}
				}
				else if (const USkeletalMeshComponent* AsSkeletal = Cast<USkeletalMeshComponent>(Prim))
				{
					if (AsSkeletal->GetSkeletalMeshAsset() != nullptr)
					{
						LocalExtent = AsSkeletal->GetSkeletalMeshAsset()->GetBounds().BoxExtent;
						AssetName = AsSkeletal->GetSkeletalMeshAsset()->GetName();
					}
				}

				const FVector CompScale = Prim->GetComponentScale();
				const FVector DrawnUU = LocalExtent * 2.0 * CompScale;   // full width, not half-extent
				const double LongestUU = DrawnUU.GetMax();

				// *** THE REFERENCE SIZE ABOVE CANNOT SEE A BROKEN POSE, AND THAT IS THE WHOLE
				// REASON THIS SECOND NUMBER EXISTS. *** GetBounds() on the ASSET answers with the
				// bind pose the mesh was imported at, so a skinned mesh whose bones have been
				// dragged apart - the classic symptom of a rig posed from the wrong skeleton, or
				// of a scale applied twice - reports a perfectly healthy 0.40 m while filling the
				// frame with smeared triangles. Prim->Bounds is what the renderer is actually
				// culling against THIS FRAME. When LIVE is much larger than the drawn size, the
				// pose is the defect and no amount of retuning the component's scale will help.
				const FVector LiveUU = Prim->Bounds.BoxExtent * 2.0;

				// The material on slot 0 is enough to tell a translucent FX shape from a lit prop,
				// which is the distinction that identifies an unrecognised slab at a glance.
				FString FirstMaterial = TEXT("<none>");
				if (Prim->GetNumMaterials() > 0)
				{
					if (const UMaterialInterface* SlotZero = Prim->GetMaterial(0))
					{
						FirstMaterial = SlotZero->GetName();
					}
				}

				return FString::Printf(
					TEXT("mesh=%-22s drawn=(%.1f, %.1f, %.1f)uu longest=%.1fuu (%.3f m) LIVE=(%.1f, %.1f, %.1f)uu scale=(%.3f, %.3f, %.3f) mats=%d mat0=%s"),
					*AssetName, DrawnUU.X, DrawnUU.Y, DrawnUU.Z, LongestUU, LongestUU / 100.0,
					LiveUU.X, LiveUU.Y, LiveUU.Z,
					CompScale.X, CompScale.Y, CompScale.Z, Prim->GetNumMaterials(), *FirstMaterial);
			};

			UE_LOG(LogTraceGame, Display,
				TEXT("[Census] ==== pawn %s, eye at %s, nearby radius %.0fuu ===="),
				*CensusPawn->GetName(), *EyeWorld.ToCompactString(), NearbyRadius);

			// --- Pass 1: everything the pawn itself owns ---------------------------------------
			TArray<UPrimitiveComponent*> PawnPrims;
			CensusPawn->GetComponents<UPrimitiveComponent>(PawnPrims);

			int32 VisibleCount = 0;
			for (UPrimitiveComponent* Prim : PawnPrims)
			{
				if (Prim == nullptr)
				{
					continue;
				}

				const bool bDrawn = Prim->IsVisible() && !Prim->bHiddenInGame;
				if (bDrawn)
				{
					++VisibleCount;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[Census]  %-22s %-3s fp=%d ownerOnly=%d ownerNoSee=%d dist=%6.1fuu  %s"),
					*Prim->GetName(),
					bDrawn ? TEXT("ON") : TEXT("off"),
					static_cast<int32>(Prim->FirstPersonPrimitiveType),
					Prim->bOnlyOwnerSee ? 1 : 0,
					Prim->bOwnerNoSee ? 1 : 0,
					FVector::Dist(Prim->GetComponentLocation(), EyeWorld),
					*DescribePrimitive(Prim));
			}

			UE_LOG(LogTraceGame, Display, TEXT("[Census]  -- %d component(s), %d drawn --"),
				PawnPrims.Num(), VisibleCount);

			// --- Pass 2: anything ELSE standing in the viewmodel's depth range -----------------
			//
			// This is the leaked-actor pass. Reported separately and named loudly, because a hit
			// here is never normal: nothing but the local pawn's own rig has any business being a
			// few tens of uu from the lens.
			int32 IntruderCount = 0;
			for (TActorIterator<AActor> ActorIt(CensusWorld); ActorIt; ++ActorIt)
			{
				AActor* Candidate = *ActorIt;
				if (Candidate == nullptr || Candidate == CensusPawn)
				{
					continue;
				}

				TArray<UPrimitiveComponent*> OtherPrims;
				Candidate->GetComponents<UPrimitiveComponent>(OtherPrims);
				for (UPrimitiveComponent* Prim : OtherPrims)
				{
					if (Prim == nullptr || !Prim->IsVisible() || Prim->bHiddenInGame)
					{
						continue;
					}

					const double DistUU = FVector::Dist(Prim->GetComponentLocation(), EyeWorld);
					if (DistUU > NearbyRadius)
					{
						continue;
					}

					++IntruderCount;
					UE_LOG(LogTraceGame, Warning,
						TEXT("[Census]  NEAR-CAMERA %s.%s dist=%.1fuu  %s"),
						*Candidate->GetName(), *Prim->GetName(), DistUU, *DescribePrimitive(Prim));
				}
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[Census] ==== %d non-pawn primitive(s) within %.0fuu of the lens ===="),
				IntruderCount, NearbyRadius);
		}));

	/**
	 * Trace.DebugCrouch [HoldSeconds] [DelaySeconds]
	 *
	 * Pulses the crouch/slide input on the local player, so the slide presentation can be
	 * photographed by an automated -TraceAutoShot run.
	 *
	 * It exists for the same reason Trace.DebugTakeCore does: the thing being verified is a VISUAL
	 * state, a screenshot harness has no hands, and the crouch bind belongs to the input layer,
	 * which is a different file and a different pass. It drives the two real entry points —
	 * ACharacter::Crouch() and UTraceCharacterMovementComponent::SetWantsToSlide() — so the slide
	 * begins through the same predicted path a key press uses, and the lean, the eye dip and the
	 * skid streak are driven by exactly the state the real thing produces. Only the key is skipped.
	 *
	 * IT PULSES RATHER THAN HOLDING, and that is not a detail. A ground slide needs a fresh PRESS
	 * (see UTraceCharacterMovementComponent's SlideBufferRemaining note), so holding the key gives
	 * you ONE slide of SlideDuration and then a walk. A harness taking a frame every second or two
	 * would then photograph a slide that had already ended and conclude the feature was dead —
	 * which is exactly the failure mode this project has already hit twice with suppressed logging.
	 * Pressing and releasing on a cycle guarantees a slide window overlaps a capture.
	 */
	FAutoConsoleCommand CmdDebugCrouch(
		TEXT("Trace.DebugCrouch"),
		TEXT("Trace.DebugCrouch [HoldSeconds] [DelaySeconds] — pulse crouch/slide on the local player so the slide pose can be captured."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Hold = (Args.Num() > 0) ? FMath::Max(0.1f, FCString::Atof(*Args[0])) : 4.f;
			const float Delay = (Args.Num() > 1) ? FMath::Max(0.f, FCString::Atof(*Args[1])) : 0.f;

			// Long enough to cover a whole slide window, short enough to fit several presses inside
			// a capture window.
			constexpr double PressSeconds = 1.1;
			constexpr double ReleaseSeconds = 0.5;

			double Elapsed = 0.0;
			bool bLogged = false;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Elapsed, bLogged, Hold, Delay](float DeltaTime) mutable -> bool
				{
					Elapsed += DeltaTime;
					if (Elapsed < Delay)
					{
						return true;
					}

					ATraceCharacter* TraceChar = FindDebugLocalCharacter(FindDebugGameWorld());
					if (TraceChar == nullptr)
					{
						// Keep waiting for a pawn until well past the hold window, then give up.
						return Elapsed < (Delay + Hold + 30.0);
					}

					UTraceCharacterMovementComponent* Movement = TraceChar->GetTraceMovement();

					// BOTH entry points: the movement component ORs its own intent flag with the
					// engine's bWantsToCrouch, and which of the two a real bind ends up using is the
					// input layer's business, not this command's.
					auto SetHeld = [TraceChar, Movement](bool bDown)
					{
						if (bDown) { TraceChar->Crouch(); } else { TraceChar->UnCrouch(); }
						if (Movement != nullptr) { Movement->SetWantsToSlide(bDown); }
					};

					if (Elapsed >= (Delay + Hold))
					{
						SetHeld(false);
						UE_LOG(LogTraceGame, Display,
							TEXT("[DebugCrouch] %s finished (sliding=%d baseEye=%.1f)."),
							*TraceChar->GetName(),
							(Movement != nullptr && Movement->IsSliding()) ? 1 : 0,
							TraceChar->BaseEyeHeight);
						return false;
					}

					if (!bLogged)
					{
						bLogged = true;
						UE_LOG(LogTraceGame, Display,
							TEXT("[DebugCrouch] %s pulsing crouch/slide for %.1fs (capsule halfHeight=%.1f baseEye=%.1f)."),
							*TraceChar->GetName(),
							Hold,
							TraceChar->GetCapsuleComponent() != nullptr ? TraceChar->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : -1.f,
							TraceChar->BaseEyeHeight);
					}

					// Square wave on the key, re-asserted every tick so a prediction correction that
					// clears the intent cannot silently end the test.
					const double Phase = FMath::Fmod(Elapsed - Delay, PressSeconds + ReleaseSeconds);
					SetHeld(Phase < PressSeconds);
					return true;
				}),
				0.f);
		}));

	/**
	 * Trace.DebugTakeCore [DelaySeconds] [TimeoutSeconds]
	 *
	 * Hands the Core to the local player, so the third-person carry view can be captured by an
	 * automated -TraceAutoShot run. Without this the only way to see it is for a human to walk to the
	 * centre of a 24000 uu arena and beat nine bots to the pickup, which is not a thing a screenshot
	 * harness can do — and "I could not photograph it" is how an unverified feature ships broken.
	 *
	 * It is not a cheat that bypasses the rules: it calls ATraceCore::TryPickup(), the same entry
	 * point the pickup sphere calls, so the Core really attaches, the trail really starts, the
	 * PlayerState really updates and bIsCarrier really replicates. Only the walking is skipped.
	 *
	 * HoldSeconds then passes the Core straight back out through ATraceCharacter::DoPass() — again
	 * the real path, the one RMB uses — so a single automated run covers the whole round trip:
	 * first person, pull back to third on pickup, push back in to first on release. Testing only
	 * the way in would leave the return blend unmeasured, and that is half the feature.
	 *
	 * It self-schedules: -ExecCmds fires long before the match map has a possessed pawn, so this
	 * retries on the core ticker until a living local pawn and a loose Core both exist, then stops.
	 */
	FAutoConsoleCommand CmdDebugTakeCore(
		TEXT("Trace.DebugTakeCore"),
		TEXT("Trace.DebugTakeCore [DelaySeconds] [HoldSeconds] [TimeoutSeconds] — give the local player the Core, then pass it away again."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 0.f;
			const float Hold = (Args.Num() > 1) ? FMath::Max(0.f, FCString::Atof(*Args[1])) : 0.f;
			const float Timeout = (Args.Num() > 2) ? FMath::Max(1.f, FCString::Atof(*Args[2])) : 60.f;

			// Captured by value into the ticker; the lambda outlives this scope by design.
			double ElapsedSeconds = 0.0;
			double CarriedSinceSeconds = -1.0;
			double PassPressedSeconds = -1.0;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[ElapsedSeconds, CarriedSinceSeconds, PassPressedSeconds, Delay, Hold, Timeout](float DeltaTime) mutable -> bool
				{
					ElapsedSeconds += DeltaTime;
					if (ElapsedSeconds < Delay)
					{
						return true;   // keep waiting
					}

					// ---- second leg: hand it back, so the return blend is exercised too ----------
					//
					// INTEGRATION FIX (spec §4). This used to be a single Carrier->DoPass() call, and
					// under the old model that was complete: DoPass() threw the Core along the aim and
					// it detached on the spot. The pass is now a HELD hover — DoPass() is only
					// DoPassPressed(), and the transfer needs the button to stay down for 0.5s with a
					// teammate under the crosshair. Pressing once and immediately dropping the ticker
					// left mouse1 LATCHED DOWN on the Core with nothing left alive to release it, so
					// the pawn sat with its shield suppressed and its trace invulnerable indefinitely
					// while this command logged that it had passed the Core away. It had not.
					//
					// So: press, then keep ticking until the Core actually leaves, and always send the
					// matching release. Whether it completes or times out, both halves of the risk beat
					// are restored, which is the state the camera blend is supposed to be measured in.
					if (CarriedSinceSeconds >= 0.0)
					{
						if ((ElapsedSeconds - CarriedSinceSeconds) < Hold)
						{
							return true;
						}

						ATraceCharacter* Carrier = FindDebugLocalCharacter(FindDebugGameWorld());
						if (Carrier == nullptr)
						{
							return false;
						}

						if (PassPressedSeconds < 0.0)
						{
							// Whether we still HAVE the Core decides how to read everything below. The
							// local pawn is re-resolved every tick, so by now it may be a different
							// actor entirely: getting trail-dashed while carrying kills you, hands the
							// Core to the dasher and respawns you as a fresh pawn. Without this the
							// "no longer carrying" that follows would be reported as a successful
							// transfer, which is how a harness talks itself into a false pass.
							if (!Carrier->IsCarrier())
							{
								UE_LOG(LogTraceGame, Display,
									TEXT("[DebugTakeCore] %s no longer has the Core after %.1fs (lost it while carrying); nothing to pass."),
									*Carrier->GetName(), Hold);
								return false;
							}

							// The same entry point mouse1 uses, so the server evaluates our real aim.
							Carrier->DoPassPressed();
							PassPressedSeconds = ElapsedSeconds;
							UE_LOG(LogTraceGame, Display,
								TEXT("[DebugTakeCore] %s began a pass after %.1fs carrying (shield down, trace invulnerable)."),
								*Carrier->GetName(), Hold);
							return true;
						}

						// Give the hover 20s to find a teammate — far more than the 0.5s the rule needs,
						// and the surplus is not slack. A pass that acquires and then CANCELS (a
						// receiver stepping behind cover, the crosshair drifting for a frame) spends
						// PassCooldownSeconds before the still-held button may acquire again, so a
						// window of a few seconds could only ever observe the first attempt. Watching
						// a held button recover from a cancel is the whole behaviour under test.
						const bool bStillCarrying = Carrier->IsCarrier();
						if (bStillCarrying && (ElapsedSeconds - PassPressedSeconds) < 20.0)
						{
							return true;
						}

						Carrier->DoPassReleased();

						UE_LOG(LogTraceGame, Display,
							TEXT("[DebugTakeCore] %s released the pass after %.1fs (carrier=%d, %s, view blend -> first person)."),
							*Carrier->GetName(), ElapsedSeconds - PassPressedSeconds, bStillCarrying ? 1 : 0,
							bStillCarrying ? TEXT("no receiver found - cancelled, shield restored") : TEXT("transfer completed"));
						return false;
					}

					if (ElapsedSeconds > (Delay + Timeout))
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[DebugTakeCore] Gave up after %.1fs: no living local pawn and loose Core."), Timeout);
						return false;
					}

					UWorld* World = FindDebugGameWorld();
					if (World == nullptr)
					{
						return true;
					}

					// Authority only. On a client the Core is server-owned and TryPickup would be a
					// no-op; say so rather than spinning silently for a minute.
					if (World->GetNetMode() == NM_Client)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[DebugTakeCore] Client build: the server owns the Core."));
						return false;
					}

					ATraceCore* TheCore = nullptr;
					if (const ATraceGameState* State = World->GetGameState<ATraceGameState>())
					{
						TheCore = State->Core;
					}
					if (TheCore == nullptr)
					{
						return true;
					}

					ATraceCharacter* TraceChar = FindDebugLocalCharacter(World);
					if (TraceChar == nullptr || !TraceChar->IsAlive())
					{
						return true;
					}

					if (TraceChar->IsCarrier())
					{
						UE_LOG(LogTraceGame, Display, TEXT("[DebugTakeCore] %s is already the carrier."), *TraceChar->GetName());
						CarriedSinceSeconds = ElapsedSeconds;
						return (Hold > 0.f);
					}

					TheCore->TryPickup(TraceChar);

					// TryPickup can legitimately refuse (someone else is carrying it, pickup lockout).
					// Report what actually happened rather than what was asked for, and keep retrying
					// if it did not take.
					if (!TraceChar->IsCarrier())
					{
						return true;
					}

					UE_LOG(LogTraceGame, Display,
						TEXT("[DebugTakeCore] %s is now carrying the Core (view blend -> third person)."),
						*TraceChar->GetName());

					CarriedSinceSeconds = ElapsedSeconds;
					return (Hold > 0.f);
				}),
				0.f);
		}));

	/**
	 * Trace.DebugAimAtTeammate [DelaySeconds] [DurationSeconds]
	 *
	 * Points the local player's view at their nearest living teammate and keeps it there.
	 *
	 * Same reason as Trace.DebugTakeCore, one step further along. The pass reticle only reaches its
	 * interesting state — brackets closed, team coloured, receiver named, hold ring filling — when a
	 * legal receiver is actually under the crosshair, and a headless harness cannot aim at a bot that
	 * is wandering a 24000 uu arena. Without this, the acquired state is the one state of the HUD
	 * that can never be photographed, which is exactly the sort of gap this project has shipped bugs
	 * through before.
	 *
	 * It moves the CONTROL ROTATION only, which is the same thing a mouse moves: the aim, the camera
	 * and the reticle all follow from it through the normal path, and ATraceCore evaluates the pass
	 * against the same rotation it would have got from a human. No pass rule is bypassed — if the
	 * teammate is behind a wall the pass is still refused and the reticle still shows no lock, which
	 * is the correct answer.
	 */
	FAutoConsoleCommand CmdDebugAimAtTeammate(
		TEXT("Trace.DebugAimAtTeammate"),
		TEXT("Trace.DebugAimAtTeammate [DelaySeconds] [DurationSeconds] — hold the local player's aim on their nearest teammate."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 0.f;
			const float Duration = (Args.Num() > 1) ? FMath::Max(0.5f, FCString::Atof(*Args[1])) : 20.f;

			double ElapsedSeconds = 0.0;
			// Weak, and re-logged whenever the choice changes: which teammate is reachable moves
			// around constantly in a live match, and "the harness stopped finding anyone" is the
			// single most useful thing this command can tell whoever is reading the log afterwards.
			TWeakObjectPtr<ATraceCharacter> LastAimedAt;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[ElapsedSeconds, LastAimedAt, Delay, Duration](float DeltaTime) mutable -> bool
				{
					ElapsedSeconds += DeltaTime;
					if (ElapsedSeconds < Delay)
					{
						return true;
					}
					if (ElapsedSeconds > (Delay + Duration))
					{
						return false;
					}

					UWorld* World = FindDebugGameWorld();
					ATraceCharacter* Me = FindDebugLocalCharacter(World);
					const ATraceGameState* State = (World != nullptr) ? World->GetGameState<ATraceGameState>() : nullptr;
					if (Me == nullptr || State == nullptr || !Me->IsAlive())
					{
						return true;
					}

					// Nearest living ally, found through the PlayerArray rather than an actor iterator:
					// every pawn in this match belongs to a player state, so this keeps the search to
					// the ten actors that could possibly be receivers.
					//
					// LINE OF SIGHT IS PART OF THE SEARCH, and it has to be. The first version simply
					// took the nearest ally, and the nearest ally in a real match is very often on a
					// platform behind a railing — so the harness pointed the crosshair at a teammate
					// the pass rules correctly refused, and photographed a reticle that was correctly
					// showing no lock. The Core's own LOS test (ATraceCore::IsLegalPassTarget) is
					// against world geometry only, so this mirrors it exactly.
					const FVector MyView = Me->GetPawnViewLocation();

					FCollisionObjectQueryParams ObjectParams;
					ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
					ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);

					ATraceCharacter* Nearest = nullptr;
					double NearestDistance = TNumericLimits<double>::Max();
					for (const APlayerState* PlayerState : State->PlayerArray)
					{
						ATraceCharacter* Candidate = (PlayerState != nullptr)
							? Cast<ATraceCharacter>(PlayerState->GetPawn())
							: nullptr;
						if (Candidate == nullptr || Candidate == Me || !Candidate->IsAlive())
						{
							continue;
						}
						// Same test ATraceCore::AreAllies makes, spelled out: that helper is file-local
						// to TraceCore.cpp and a second copy of the team rules is not worth a header.
						if (Me->GetTeam() == ETraceTeam::None || Candidate->GetTeam() != Me->GetTeam())
						{
							continue;
						}

						const double Distance = FVector::Dist(Me->GetActorLocation(), Candidate->GetActorLocation());
						if (Distance >= NearestDistance)
						{
							continue;
						}

						const FVector CandidateChest = Candidate->GetActorLocation() + FVector(0.0, 0.0, 40.0);
						FCollisionQueryParams QueryParams(FName(TEXT("TraceDebugAimLOS")), /*bTraceComplex=*/false);
						QueryParams.AddIgnoredActor(Me);
						QueryParams.AddIgnoredActor(Candidate);
						if (World->LineTraceTestByObjectType(MyView, CandidateChest, ObjectParams, QueryParams))
						{
							continue;   // something solid in the way: not a legal receiver, do not aim at it
						}

						NearestDistance = Distance;
						Nearest = Candidate;
					}

					AController* MyController = Me->GetController();
					if (Nearest != LastAimedAt.Get())
					{
						LastAimedAt = Nearest;
						UE_LOG(LogTraceGame, Display,
							TEXT("[DebugAimAtTeammate] %s is now looking at %s (%.0f uu, clear line of sight)."),
							*Me->GetName(),
							(Nearest != nullptr) ? *Nearest->GetName() : TEXT("<nobody reachable>"),
							(Nearest != nullptr) ? NearestDistance : 0.0);
					}

					if (Nearest == nullptr || MyController == nullptr)
					{
						return true;
					}

					// Chest height, which is the point ATraceCore aims a pass at.
					const FVector Chest = Nearest->GetActorLocation() + FVector(0.0, 0.0, 40.0);
					MyController->SetControlRotation((Chest - MyView).Rotation());
					return true;
				}),
				0.f);
		}));

	/**
	 * Trace.DebugPassTargets [IntervalSeconds] [Samples]
	 *
	 * For the local player, asks ATraceCore::IsLegalPassTarget about EVERY other character and prints
	 * which test each one failed.
	 *
	 * This exists because of a bug whose only symptom was a camera. The view mode is
	 * `!bIsCarrier` and nothing else, so a carrier who cannot get rid of the Core is a player stuck in
	 * third person — and "the camera is stuck" and "the pass never acquires anybody" produce exactly
	 * the same log line ("no receiver found") and exactly the same screenshot. The reasons come out of
	 * the real rule through an out-param rather than from a copy of it here, so this cannot drift away
	 * from what the game actually enforces.
	 *
	 * When the answer is "no line of sight" it re-runs the same ECC_Visibility trace and NAMES THE
	 * BLOCKER, because "something is in the way" across a 24000 uu arena is not a diagnosis.
	 */
	FAutoConsoleCommand CmdDebugPassTargets(
		TEXT("Trace.DebugPassTargets"),
		TEXT("Trace.DebugPassTargets [IntervalSeconds] [Samples] — log why each teammate is or is not a legal pass receiver."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Interval = (Args.Num() > 0) ? FMath::Max(0.1f, FCString::Atof(*Args[0])) : 1.f;
			const int32 Samples = (Args.Num() > 1) ? FMath::Max(1, FCString::Atoi(*Args[1])) : 30;

			int32 Emitted = 0;
			double SinceLast = 0.0;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Emitted, SinceLast, Interval, Samples](float DeltaTime) mutable -> bool
				{
					SinceLast += DeltaTime;
					if (SinceLast < Interval)
					{
						return true;
					}
					SinceLast = 0.0;

					UWorld* World = FindDebugGameWorld();
					ATraceCharacter* Me = FindDebugLocalCharacter(World);
					ATraceCore* TheCore = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
					if (Me == nullptr || TheCore == nullptr)
					{
						return (++Emitted < Samples);
					}

					TArray<ATraceCharacter*> Candidates;
					TheCore->GatherCharacters(Candidates);

					const FVector MyView = Me->GetPawnViewLocation();
					const FVector MyAim = Me->GetAimDirection();

					UE_LOG(LogTraceGame, Display,
						TEXT("[PassTargets] %s carrier=%d holder=%s candidates=%d best=%s"),
						*Me->GetName(), Me->IsCarrier() ? 1 : 0, *GetNameSafe(TheCore->GetCarrier()),
						Candidates.Num(), *GetNameSafe(TheCore->FindPassTargetFor(Me)));

					for (const ATraceCharacter* Candidate : Candidates)
					{
						if (Candidate == nullptr || Candidate == Me)
						{
							continue;
						}

						const TCHAR* Reason = TEXT("?");
						const bool bLegal = TheCore->IsLegalPassTarget(Me, Candidate, /*bRequireAim=*/true, &Reason);

						const FVector Chest = Candidate->GetActorLocation() + FVector(0.0, 0.0, 20.0);
						FVector ToTarget = Chest - MyView;
						const double Distance = ToTarget.Size();
						const double AngleDegrees = (Distance > 1.0)
							? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
								FVector::DotProduct(ToTarget / Distance, MyAim), -1.0, 1.0)))
							: 0.0;

						// Only the interesting ones: an enemy on the far side of the map failing "not
						// an ally" is noise, and ten players times thirty samples of it is a log nobody
						// reads.
						const bool bAlly = (Candidate->GetTeam() != ETraceTeam::None)
							&& (Candidate->GetTeam() == Me->GetTeam());
						if (!bAlly)
						{
							continue;
						}

						FString Blocker;
						if (FCString::Strcmp(Reason, TEXT("no line of sight")) == 0)
						{
							FCollisionQueryParams QueryParams(FName(TEXT("TraceDebugPassLOS")), /*bTraceComplex=*/false);
							QueryParams.AddIgnoredActor(TheCore);
							QueryParams.AddIgnoredActor(Me);
							QueryParams.AddIgnoredActor(Candidate);

							FHitResult Hit;
							if (World->LineTraceSingleByChannel(Hit, MyView, Chest, ECC_Visibility, QueryParams))
							{
								Blocker = FString::Printf(TEXT(" blockedBy=%s/%s profile=%s at %.0fuu"),
									*GetNameSafe(Hit.GetActor()),
									*GetNameSafe(Hit.GetComponent()),
									Hit.GetComponent() != nullptr
										? *Hit.GetComponent()->GetCollisionProfileName().ToString()
										: TEXT("?"),
									Hit.Distance);
							}
						}

						UE_LOG(LogTraceGame, Display,
							TEXT("[PassTargets]   ally %-18s legal=%d reason=%-24s dist=%.0fuu angle=%.1fdeg alive=%d%s"),
							*Candidate->GetName(), bLegal ? 1 : 0, Reason, Distance, AngleDegrees,
							Candidate->IsAlive() ? 1 : 0, *Blocker);
					}

					return (++Emitted < Samples);
				}),
				0.f);
		}));

	// =============================================================================================
	// Trace.ViewModel.Guns / Trace.ViewModel.Equip  —  SPEC v30 §2
	// =============================================================================================
	//
	// WHAT THEY ARE FOR. The complaint spec §2 opens with is that nothing on screen said which gun
	// was in hand. The fix is visual, so a screenshot is the primary evidence — but a screenshot
	// cannot tell you WHY the pistol is on screen when you pressed 3 (missing art? a refused swap? a
	// mesh that failed to attach?), and those three have different fixes. Guns prints the difference,
	// and Equip is what lets a headless run reach all three states to photograph them at all.
	//
	// *** DELIBERATELY NOT NAMED Trace.Smg.* ***. Core/TraceSmgVerify.cpp registers Trace.Smg.Probe
	// and Trace.Smg.Hold — the SMG's equivalents of Trace.Railgun.Probe/Hold that spec §5 asks for,
	// sitting where TraceRailgunVerify.cpp's live, which is the right place for them. Both of those
	// drive the accessors THIS file owns (GetShownGun, DebugGetSmgParts, DebugGetSmgEmissive,
	// DebugHoldSmgPhase), so the pawn keeps the state and the verify file keeps the commands. Two
	// registrations of one console name is a warning at best and a silently lost command at worst,
	// so these two take names that cannot collide and answer a narrower question: the RIG, not the
	// weapon.

	/** One SMG part: mesh, transform, and every material slot with what is actually on it. */
	void ReportViewModelSmgPart(const TCHAR* Label, UStaticMeshComponent* Part)
	{
		if (Part == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] %s : NOT BUILT"), Label);
			return;
		}

		const UStaticMesh* Mesh = Part->GetStaticMesh();
		const FVector Loc = Part->GetRelativeLocation();
		const FRotator Rot = Part->GetRelativeRotation();

		// bVisible AND bHiddenInGame, separately, because the whole selector rests on them being two
		// independent layers (see UpdateWeaponSelection). "drawn=0" with "visible=1" is the selector
		// having hidden this gun on purpose; "visible=0" is the rig being off screen entirely.
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] %s mesh=%-24s rel=(%.2f, %.2f, %.2f) pitch=%.2f scale=%.3f visible=%d hiddenInGame=%d drawn=%d"),
			Label, Mesh != nullptr ? *Mesh->GetName() : TEXT("NONE"),
			Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Part->GetRelativeScale3D().X,
			Part->GetVisibleFlag() ? 1 : 0, Part->bHiddenInGame ? 1 : 0, Part->IsVisible() ? 1 : 0);

		const int32 NumSlots = Part->GetNumMaterials();
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			const UMaterialInterface* Material = Part->GetMaterial(Slot);
			const FName SlotName = (Mesh != nullptr && Mesh->GetStaticMaterials().IsValidIndex(Slot))
				? Mesh->GetStaticMaterials()[Slot].MaterialSlotName : NAME_None;
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg]       slot %d '%s' -> %s"),
				Slot, *SlotName.ToString(),
				Material != nullptr ? *Material->GetName() : TEXT("NONE"));
		}
	}

	const TCHAR* ViewModelShownGunName(ATraceCharacter::EShownGun Gun)
	{
		switch (Gun)
		{
		case ATraceCharacter::EShownGun::None:   return TEXT("NEITHER (guns stowed)");
		case ATraceCharacter::EShownGun::Pistol: return TEXT("PISTOL");
		case ATraceCharacter::EShownGun::Smg:    return TEXT("SMG");
		default:                                 return TEXT("?");
		}
	}

	/**
	 * *** SPEC v32 §7d — WHICH WEAPON IS ON WHICH NUMBER KEY, ASKED RATHER THAN REMEMBERED. ***
	 *
	 * Trace.ViewModel.Equip shipped with the PRE-REVERT numbering hard-coded — "1 stows, 2 pistol,
	 * 3 SMG" — which was true under spec v29 §5 and stopped being true in Demo 26, when the binds
	 * moved to 1 = PISTOL, 2 = SMG, 3 = KNIFE. A harness that names the wrong state is worse than one
	 * that fails: every screenshot it took was correctly labelled with the wrong caption.
	 *
	 * The fix is not to re-type the new numbering, because that is the same defect with a later date
	 * on it. UTraceUserSettings IS the live bind table — it is what the player's keyboard actually
	 * goes through, migrations and rebinds included — so this asks it which of the three equip actions
	 * owns the number key in question, and only falls back to the shipped default when a player has
	 * rebound that key away from all three.
	 *
	 * That also makes the command follow a REBIND, which is the behaviour a harness wants: "slot 2"
	 * means "what the 2 key does on this machine", so a screenshot and the keyboard cannot disagree.
	 *
	 * @param OutHow  filled with how the answer was reached, so the log line can say so.
	 */
	ETraceEquippedWeapon ViewModelEquipSlotWeapon(int32 Slot, FString& OutHow)
	{
		// BUILT HERE AND NOT AT NAMESPACE SCOPE. EKeys' statics are constructed during module
		// startup, so an FKey living in a global would not reliably exist yet — the same trap
		// FTraceInputActionInfo::DefaultKey is a function pointer to avoid.
		const FKey Pressed = (Slot == 1) ? EKeys::One : ((Slot == 2) ? EKeys::Two : EKeys::Three);

		const UTraceUserSettings& Settings = UTraceUserSettings::Get();

		struct FEquipRow
		{
			ETraceInputAction Action;
			ETraceEquippedWeapon Weapon;
		};
		const FEquipRow Rows[] =
		{
			{ ETraceInputAction::EquipGun,   ETraceEquippedWeapon::Gun },
			{ ETraceInputAction::EquipSmg,   ETraceEquippedWeapon::Smg },
			{ ETraceInputAction::EquipKnife, ETraceEquippedWeapon::Knife },
		};

		for (const FEquipRow& Row : Rows)
		{
			// ActionUsesKey, not GetKey — spec v28 §3c gave every action up to two binds and a second
			// slot holding the number key is just as real a bind as the first.
			if (Settings.ActionUsesKey(Row.Action, Pressed))
			{
				OutHow = FString::Printf(TEXT("live bind table: %s -> %s"),
					*UTraceUserSettings::DescribeKey(Pressed),
					TraceInputActions::Info(Row.Action).DisplayName);
				return Row.Weapon;
			}
		}

		// NOBODY OWNS THAT KEY. A player may unbind or re-map it, and a dev command that then refused
		// to do anything would be a worse tool than one that says what it assumed. Demo 26's shipped
		// numbering, named as an assumption rather than presented as a fact.
		OutHow = FString::Printf(TEXT("no action is bound to %s; assuming Demo 26's shipped numbering"),
			*UTraceUserSettings::DescribeKey(Pressed));
		switch (Slot)
		{
		case 1:  return ETraceEquippedWeapon::Gun;
		case 2:  return ETraceEquippedWeapon::Smg;
		default: return ETraceEquippedWeapon::Knife;
		}
	}

	FAutoConsoleCommand CmdViewModelGuns(
		TEXT("Trace.ViewModel.Guns"),
		TEXT("Spec v30 §2. Reports which of the THREE weapon states is on screen and whether it agrees "
		     "with the replicated selector: whether the SMG rig was built or fell back, every SMG "
		     "part's mesh/transform/visibility layers/slots, the live EmissiveIntensity on both glowing "
		     "materials, and where the active muzzle marker is. Takes an optional DelaySeconds so a "
		     "single deferred-exec batch can sample it AFTER an equip or a held pose has landed."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			// DEFERRABLE FOR THE SAME REASON THE EQUIP IS, and it matters more here: the pose is
			// applied in Tick, so a probe run in the same frame as the command that changed it reports
			// the state from BEFORE the change. A delay of a frame or more is the difference between
			// measuring the feature and measuring the frame before it.
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 0.f;
			if (Delay > 0.f)
			{
				double Waited = 0.0;
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
					[Waited, Delay](float DeltaTime) mutable -> bool
				{
					Waited += DeltaTime;
					if (Waited < Delay)
					{
						return true;
					}
					if (GEngine != nullptr)
					{
						GEngine->Exec(FindDebugGameWorld(), TEXT("Trace.ViewModel.Guns"));
					}
					return false;
				}), 0.f);
				return;
			}

			ATraceCharacter* Character = FindDebugLocalCharacter(FindDebugGameWorld());
			if (Character == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] No local ATraceCharacter yet - run this once a match has started."));
				return;
			}

			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ===================================================="));
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] ON SCREEN: %s   |   smgRig=%s pistolRig=%s   rigVisible=%d parts=%d"),
				ViewModelShownGunName(Character->GetShownGun()),
				Character->UsesSmgViewModel() ? TEXT("IMPORTED ART") : TEXT("NOT BUILT"),
				Character->UsesRailgunViewModel() ? TEXT("RAILGUN") : TEXT("FALLBACK CUBES"),
				Character->IsViewModelVisible() ? 1 : 0,
				Character->GetViewModelPartCount());

			// The selector beside the picture. These two disagreeing is the ONE state worth shouting
			// about: the gun being simulated is not the gun being drawn, which is spec §2's complaint
			// restated. It is legal exactly once — the missing-art fallback — and the line says so.
			if (const UTraceWeaponComponent* W = Character->FindComponentByClass<UTraceWeaponComponent>())
			{
				const bool bSmgSelected = W->IsSmgEquipped();
				const bool bSmgDrawn = (Character->GetShownGun() == ATraceCharacter::EShownGun::Smg);
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] selector=%s clip=%d/%d reloading=%d (%.2fs left of %.2fs)  %s"),
					LexToString(W->GetEquippedWeapon()), W->GetClipAmmo(), W->GetClipSize(),
					W->IsReloading() ? 1 : 0, W->GetReloadRemaining(), W->GetReloadSeconds(),
					(bSmgSelected == bSmgDrawn)
						? TEXT("-- selector and picture agree")
						: (Character->UsesSmgViewModel()
							? TEXT("*** MISMATCH: the gun drawn is not the gun selected ***")
							: TEXT("-- SMG selected with no SMG art: the documented fallback")));
			}

			if (!Character->UsesSmgViewModel())
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] The SMG art did not resolve, or -TraceNoSmg/-TraceNoRailgun was passed. "
					     "Content/Trace/Weapons/Meshes must contain SM_RailgunSmg_Body/WallLeft/WallRight/Mag."));
				UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ===================================================="));
				return;
			}

			UStaticMeshComponent* Body = nullptr;
			UStaticMeshComponent* WallL = nullptr;
			UStaticMeshComponent* WallR = nullptr;
			UStaticMeshComponent* Mag = nullptr;
			Character->DebugGetSmgParts(Body, WallL, WallR, Mag);

			ReportViewModelSmgPart(TEXT("body  "), Body);
			ReportViewModelSmgPart(TEXT("wallL "), WallL);
			ReportViewModelSmgPart(TEXT("wallR "), WallR);
			ReportViewModelSmgPart(TEXT("mag   "), Mag);

			// The wall SPREAD rather than each wall's Y, because the spread is the thing §3 specifies
			// (±4.2 uu apart) and it is invariant under any recoil offset applied to both.
			if (WallL != nullptr && WallR != nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] wall spread = %.3f uu (rig), i.e. %.2f mesh-cm apart; magazine drop = %.3f uu"),
					WallR->GetRelativeLocation().Y - WallL->GetRelativeLocation().Y,
					(WallR->GetRelativeLocation().Y - WallL->GetRelativeLocation().Y) / TraceCharacterLayout::SmgScale,
					(Mag != nullptr && Body != nullptr) ? (Body->GetRelativeLocation().Z - Mag->GetRelativeLocation().Z) : 0.f);
			}

			float LiveCyan = -1.f;
			float LiveAmber = -1.f;
			const bool bReadBack = Character->DebugGetSmgEmissive(LiveCyan, LiveAmber);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] live EmissiveIntensity: cyan=%.3f (rest %.2f, peak %.2f) amber=%.3f (full %.2f, empty %.2f) -- readback %s"),
				LiveCyan, TraceCharacterLayout::SmgCyanRest, TraceCharacterLayout::SmgCyanPeak,
				LiveAmber, TraceCharacterLayout::SmgAmberFull, TraceCharacterLayout::SmgAmberEmpty,
				bReadBack ? TEXT("OK") : TEXT("FAILED - the parameter is not on the material"));

			FVector MuzzleRaw = FVector::ZeroVector;
			if (Character->DebugGetViewModelMuzzleRaw(MuzzleRaw))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] active muzzle marker (world) = (%.1f, %.1f, %.1f); mesh-local landmark (%.1f, %.1f, %.1f) cm"),
					MuzzleRaw.X, MuzzleRaw.Y, MuzzleRaw.Z,
					TraceCharacterLayout::SmgMuzzleLocal.X, TraceCharacterLayout::SmgMuzzleLocal.Y,
					TraceCharacterLayout::SmgMuzzleLocal.Z);
			}
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ===================================================="));
		}));

	/**
	 * Trace.ViewModel.Equip <1|2|3> [DelaySeconds] [TimeoutSeconds]
	 *
	 * IT SELF-SCHEDULES, and that is what makes photographing all three states possible at all. The
	 * deferred-exec harness fires ONE batch of commands at ONE time (see TraceAutoShot::
	 * ArmDeferredExec, "one timer, not one per command"), and at the moment that batch runs the match
	 * map has usually not produced a possessed pawn yet — the character-select screen is still open.
	 * A command that needed a pawn to already exist could therefore only ever capture whichever state
	 * happened to be selected, which is precisely one third of the evidence spec §2 asks for.
	 *
	 * So this waits for a living local pawn, then equips, and each invocation carries its OWN delay:
	 *
	 *     -TraceExec="Trace.ViewModel.Equip 1 0|Trace.ViewModel.Equip 2 4|Trace.ViewModel.Equip 3 8"
	 *     -TraceAutoShot=42 -TraceAutoShotRepeat=4
	 *
	 * ...photographs the pistol, the SMG and the knife in a single run.
	 *
	 * *** SPEC v32 §7d: THE NUMBERING IS NO LONGER WRITTEN DOWN HERE AT ALL. *** It used to say
	 * "1 stows, 2 pistol, 3 SMG", which was v29 §5's arrangement and was made wrong by Demo 26's
	 * revert to 1 = PISTOL, 2 = SMG, 3 = KNIFE — so this command spent a whole spec cycle putting the
	 * pawn in one state and labelling the screenshot with another. Re-typing the new numbering would
	 * be the identical defect with a later date on it, so the slot is now resolved against
	 * UTraceUserSettings' live bind table instead; see ViewModelEquipSlotWeapon.
	 */
	FAutoConsoleCommand CmdViewModelEquip(
		TEXT("Trace.ViewModel.Equip"),
		TEXT("Trace.ViewModel.Equip <1|2|3> [DelaySeconds] [TimeoutSeconds]. Dev only. Puts the local "
		     "player in the weapon state that NUMBER KEY is bound to, through the SHIPPED equip path, "
		     "waiting for a pawn to exist first. The slot is resolved against the live keybind table "
		     "(spec v32 §7d), so it follows a rebind and cannot go stale the way a hard-coded "
		     "numbering did; the shipped default is 1 pistol, 2 SMG, 3 knife."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const int32 Slot = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 1;
			const float Delay = (Args.Num() > 1) ? FMath::Max(0.f, FCString::Atof(*Args[1])) : 0.f;
			const float Timeout = (Args.Num() > 2) ? FMath::Max(1.f, FCString::Atof(*Args[2])) : 90.f;

			FString How;
			const ETraceEquippedWeapon Desired = ViewModelEquipSlotWeapon(Slot, How);

			double ElapsedSeconds = 0.0;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[ElapsedSeconds, Slot, Desired, Delay, Timeout, How](float DeltaTime) mutable -> bool
				{
					ElapsedSeconds += DeltaTime;
					if (ElapsedSeconds < Delay)
					{
						return true;
					}

					ATraceCharacter* Character = FindDebugLocalCharacter(FindDebugGameWorld());
					if (Character == nullptr || !Character->IsAlive())
					{
						if (ElapsedSeconds > Delay + Timeout)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[ViewModel.Equip] Gave up after %.1fs: no living local pawn."), Timeout);
							return false;
						}
						return true;
					}

					// THE SHIPPED VERB, not a write to the selector. A harness that set EquippedWeapon
					// directly would photograph a state no key can actually reach, which is how a
					// screenshot ends up proving something the game does not do.
					ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
					const bool bOk = TraceMelee::RequestEquip(Character, Desired, &Refusal);

					// A refusal is usually the 0.35s pullout from the PREVIOUS command in the same
					// batch still running, which is transient — so retry rather than report a failure
					// that would have succeeded a frame later.
					if (!bOk && Refusal == ETraceMeleeRefusal::Deploying && ElapsedSeconds < Delay + Timeout)
					{
						return true;
					}

					// Reports the rig drawn BEFORE this frame, and says so: UpdateWeaponSelection runs in
					// Tick, so the new selector has not reached the rig yet. Printing it as "now" was
					// off by one frame and read as the equip having failed. Ask Trace.ViewModel.Guns
					// (which takes a delay for exactly this reason) for the settled answer.
					UE_LOG(LogTraceGame, Display,
						TEXT("[ViewModel.Equip] slot %d (%s, via %s) -> %s (refusal=%d); rig drawn as of the previous frame: %s."),
						Slot, LexToString(Desired), *How, bOk ? TEXT("accepted") : TEXT("REFUSED"),
						static_cast<int32>(Refusal), ViewModelShownGunName(Character->GetShownGun()));
					return false;
				}),
				0.f);
		}));

	// =============================================================================================
	// SPEC v31 §6 — THE HANDS
	// =============================================================================================
	//
	// Two commands, and between them they answer the two questions a screenshot cannot.
	//
	//   Probe  says WHICH clip is playing, at what time, on which loadout, and whether the rig is the
	//          pack's or the procedural fallback. A photograph of a hand cannot tell you that the
	//          hand is playing Idle_Pistol rather than sitting in the reference pose because nothing
	//          ever started a clip — and those look nearly identical for the knife loadout, since the
	//          GLB's default node transforms ARE Idle_Knife's first frame.
	//   Hold   pins a pose. Without it, four of the sixteen action clips cannot be photographed at
	//          all: Shoot_{Pistol,Smg} is 0.1667 s, and the whole `core` loadout is third person by
	//          the rules of the game, so Idle_Core and Throw_Core are never on screen in normal play.

	ATraceCharacter::EHandsLoadout ParseHandsLoadout(const FString& Text)
	{
		if (Text.Equals(TEXT("knife"), ESearchCase::IgnoreCase)) { return ATraceCharacter::EHandsLoadout::Knife; }
		if (Text.Equals(TEXT("smg"), ESearchCase::IgnoreCase))   { return ATraceCharacter::EHandsLoadout::Smg; }
		if (Text.Equals(TEXT("core"), ESearchCase::IgnoreCase))  { return ATraceCharacter::EHandsLoadout::Core; }
		return ATraceCharacter::EHandsLoadout::Pistol;
	}

	ATraceCharacter::EHandsAction ParseHandsAction(const FString& Text)
	{
		if (Text.Equals(TEXT("draw"), ESearchCase::IgnoreCase))     { return ATraceCharacter::EHandsAction::Draw; }
		if (Text.Equals(TEXT("stab"), ESearchCase::IgnoreCase))     { return ATraceCharacter::EHandsAction::Stab; }
		if (Text.Equals(TEXT("inspect"), ESearchCase::IgnoreCase))  { return ATraceCharacter::EHandsAction::Inspect; }
		if (Text.Equals(TEXT("shoot"), ESearchCase::IgnoreCase))    { return ATraceCharacter::EHandsAction::Shoot; }
		if (Text.Equals(TEXT("reload"), ESearchCase::IgnoreCase))   { return ATraceCharacter::EHandsAction::Reload; }
		if (Text.Equals(TEXT("throw"), ESearchCase::IgnoreCase))    { return ATraceCharacter::EHandsAction::Throw; }
		if (Text.Equals(TEXT("jump"), ESearchCase::IgnoreCase))     { return ATraceCharacter::EHandsAction::Jump; }
		if (Text.Equals(TEXT("walljump"), ESearchCase::IgnoreCase)) { return ATraceCharacter::EHandsAction::Walljump; }
		return ATraceCharacter::EHandsAction::None;
	}

	FAutoConsoleCommand CmdHandsProbe(
		TEXT("Trace.Hands.Probe"),
		TEXT("Spec v31 §6. Reports the first-person hand rig: pack mesh or procedural fallback, the "
		     "loadout, the clip actually loaded on the component and its playhead, and where "
		     "wrist_right is in rig space. Optional DelaySeconds so one deferred-exec batch can sample "
		     "it after an equip or a hold has landed."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 0.f;

			double Elapsed = 0.0;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Elapsed, Delay](float DeltaTime) mutable -> bool
				{
					Elapsed += DeltaTime;
					if (Elapsed < Delay)
					{
						return true;
					}

					ATraceCharacter* Character = FindDebugLocalCharacter(FindDebugGameWorld());
					if (Character == nullptr)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[Hands] no local pawn."));
						return false;
					}

					FString Clip, Loadout;
					float Time = -1.f, Length = -1.f;
					const bool bPack = Character->DebugGetHandsState(Clip, Time, Length, Loadout);

					// The wrist in RIG space, which is the number every weapon offset is expressed
					// against. Printed because "the gun is in the wrong place" and "the hand is in the
					// wrong place" look identical in a screenshot and have different fixes.
					FString Wrist = TEXT("-");
					if (const USkeletalMeshComponent* Hands = Character->GetViewModelHandsMesh())
					{
						const FTransform W = Hands->GetSocketTransform(
							ATraceCharacter::GetWeaponAttachBoneName(), RTS_Component) * Hands->GetRelativeTransform();
						Wrist = FString::Printf(TEXT("(%.2f, %.2f, %.2f)"),
							W.GetLocation().X, W.GetLocation().Y, W.GetLocation().Z);
					}

					// *** [v32 §8] THE DELTA, WHICH IS THE NUMBER THE KNIFE DEFECT WAS INVISIBLE
					//     WITHOUT. ***
					//
					// wristRig above says WHERE the hand is; this says how far the hand has walked
					// from the base pose every prop's rig-space transform is authored against. It is
					// the multiplier the guns, the forearms and (since v32 §8) the blade are all
					// carried by, so a prop that is NOT riding it shows up here as a large number
					// beside a picture of something lying next to the fist instead of in it.
					//
					// Printed as a distance AND an angle because the angle is the half that does the
					// damage: 1.3 uu of translation is a few pixels, while the same wrist rotated 20
					// degrees swings a grip 7.8 uu away by nearly 3 uu. A translation-only readout
					// would have said "the hand barely moved" and been believed.
					//
					// It must read ~0.00 uu / ~0.0 deg on Idle_Pistol (that IS the base pose) and on
					// the cube fallback (no skeleton, so nothing can move) — two standing red arms
					// for the same line.
					const FTransform WristDelta = Character->GetViewModelWeaponDelta();
					const FString Delta = FString::Printf(TEXT("%.2fuu/%.1fdeg"),
						WristDelta.GetLocation().Size(),
						FMath::RadiansToDegrees(WristDelta.GetRotation().GetAngle()));

					// *** THE TWO NUMBERS §7a IS ABOUT, PRINTED SIDE BY SIDE AND PERMANENTLY. ***
					//
					// The stab clip used to be gated on the SHOOT LOCKOUT, which a knife swing never
					// touches; it sets the SWING COOLDOWN. Printing both is the standing red arm for
					// that fix: during a real swing the cooldown is non-zero and the lockout is flat
					// zero, so the predicate that used to live there is provably, not arguably,
					// incapable of firing. Anyone who ever wonders why the rule changed can read it
					// off one probe line instead of re-deriving the argument.
					// *** offHand IS PRINTED BECAUSE "OFF SCREEN" AND "NOT DRAWN" ARE THE SAME
					//     SCREENSHOT AND ONLY ONE OF THEM IS THIS FILE'S DOING. *** The framing block
					//     below already says the left wrist is at v = -1.14; it cannot say whether the
					//     palm hanging off it is still being rasterised into the bottom-left corner.
					//     It must read SHOWN on every Core clip and on all four walljumps, and HIDDEN
					//     on the one-handed idles, on both shoots AND ON BOTH RELOADS — that pairing
					//     is the standing check on TraceCharacterAssets::HandsClipShowsOffHand. The
					//     reloads are named because they used to be the exemption and the frame
					//     disagreed; the handL_top row in the framing block is what settles it now.
					UE_LOG(LogTraceGame, Display,
						TEXT("[Hands] rig=%s loadout=%s clip=%s t=%.4f/%.4fs wristRig=%s wristDelta=%s vmVisible=%d ")
						TEXT("offHand=%s shownGun=%s swingCooldown=%.4fs shootLockout=%.4fs"),
						bPack ? TEXT("PACK (SK_TraceHands)") : TEXT("PROCEDURAL CUBES (fallback)"),
						*Loadout, *Clip, Time, Length, *Wrist, *Delta,
						Character->IsViewModelVisible() ? 1 : 0,
						Character->DebugGetHandsOffHandHidden() ? TEXT("HIDDEN") : TEXT("shown"),
						ViewModelShownGunName(Character->GetShownGun()),
						TraceMelee::GetSwingCooldownRemaining(Character),
						TraceMelee::GetShootLockoutRemaining(Character));

					// [SPEC v32 §5] THE GLOW, ON ITS OWN LINE, AND IT NAMES THE DRIVER.
					//
					// Two brightness numbers that happen to move together are not evidence that the
					// gloves are on the weapon's curve; the SOURCE string is, because it says which
					// fact answered this frame. And the slot counts are here because "0 slots" and
					// "the driver is broken" produce the identical screenshot and have completely
					// different fixes — the SMG's magazine taught that one.
					float Cyan = -1.f, Amber = -1.f;
					int32 CyanSlots = 0, AmberSlots = 0;
					const bool bReadBack = Character->DebugGetHandsEmissive(Cyan, Amber, CyanSlots, AmberSlots);

					float Pulse = 0.f;
					const TCHAR* PulseSource = Character->DebugGetHandsPulse(Pulse);

					if (!bPack)
					{
						// The fallback has no named slots and never will. Said once per probe, not
						// once per frame — see UpdateHandsEmissive for why the driver itself is mute.
						UE_LOG(LogTraceGame, Display,
							TEXT("[Hands] emissive: n/a on PROCEDURAL CUBES (fallback) - that rig has ")
							TEXT("no circuit_cyan/core_amber slots to drive, which is a degrade and ")
							TEXT("not a fault."));
					}
					else
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[Hands] emissive: cyan=%.3f (idle %.2f-%.2f, peak %.2f) amber=%.3f ")
							TEXT("(idle %.2f-%.2f, peak %.2f) slots cyan=%d amber=%d pulse=%.3f from '%s' -- readback %s"),
							Cyan, TraceCharacterLayout::HandsCyanIdleLow, TraceCharacterLayout::HandsCyanIdleHigh,
							TraceCharacterLayout::HandsCyanPeak,
							Amber, TraceCharacterLayout::HandsAmberIdleLow, TraceCharacterLayout::HandsAmberIdleHigh,
							TraceCharacterLayout::HandsAmberPeak,
							CyanSlots, AmberSlots, Pulse, PulseSource,
							bReadBack ? TEXT("OK") : TEXT("FAILED - EmissiveIntensity is not on the material"));

						// *** WHAT THE GLOVES ARE ACTUALLY WEARING. *** A v31 verifier found every
						// pack mesh on WorldGridMaterial, the grey developer checkerboard, because
						// Interchange bound none of the MI_Pack_* instances it had just created. That
						// is fixed, and "fixed once" is not "still fixed" — a re-import undoes it
						// silently — so the live binding is printed rather than assumed. A MID is
						// reported by its PARENT, since that is the asset the question is about.
						if (const USkeletalMeshComponent* Hands = Character->GetViewModelHandsMesh())
						{
							const TArray<FName> Slots = Hands->GetMaterialSlotNames();
							FString Line;
							for (int32 Index = 0; Index < Slots.Num(); ++Index)
							{
								const UMaterialInterface* Bound = Hands->GetMaterial(Index);
								const UMaterialInstanceDynamic* AsMid = Cast<UMaterialInstanceDynamic>(Bound);
								const UMaterialInterface* Report =
									(AsMid != nullptr && AsMid->Parent != nullptr) ? AsMid->Parent.Get() : Bound;
								Line += FString::Printf(TEXT("  %s=%s%s"),
									*Slots[Index].ToString(),
									(Report != nullptr) ? *Report->GetName() : TEXT("NONE"),
									(AsMid != nullptr) ? TEXT(" (MID)") : TEXT(""));
							}
							UE_LOG(LogTraceGame, Display, TEXT("[Hands] live materials:%s"), *Line);
						}
					}

					// *** AND WHETHER ANY OF IT IS ACTUALLY IN THE FRAME. ***
					//
					// Printed for BOTH rigs, because the comparison is the point: the fallback's two
					// bands sit at v = -0.90 and photograph correctly, and the shipped pack rig had
					// its right band at -1.05 and its whole left arm past -1.22. Those are three
					// numbers that say "off the bottom of the screen" and a census that said
					// "drawn" — see DebugGetViewModelFraming for how long that cost.
					FString Framing;
					if (Character->DebugGetViewModelFraming(Framing))
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[Hands] framing (fraction of the half-frame through the ")
							TEXT("FIRST-PERSON lens; |v|<=1 is on screen, -1 is the bottom edge):%s"),
							*Framing);
					}
					return false;
				}),
				0.f);
		}));

	FAutoConsoleCommand CmdHandsHold(
		TEXT("Trace.Hands.Hold"),
		TEXT("Trace.Hands.Hold <knife|pistol|smg|core> <none|draw|stab|inspect|shoot|reload|throw|jump|walljump> "
		     "[Alpha] [HoldSeconds] [DelaySeconds]. Dev only. Pins one hand clip at Alpha of its length "
		     "and forces the rig visible, so a 0.1667 s recoil frame — or the Core cradle, which is "
		     "third person in real play — can be photographed. Alpha < 0 releases."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const ATraceCharacter::EHandsLoadout Loadout =
				ParseHandsLoadout(Args.Num() > 0 ? Args[0] : FString());
			const ATraceCharacter::EHandsAction Action =
				ParseHandsAction(Args.Num() > 1 ? Args[1] : FString());
			const float Alpha = (Args.Num() > 2) ? FCString::Atof(*Args[2]) : 0.f;
			const float Hold = (Args.Num() > 3) ? FMath::Max(0.f, FCString::Atof(*Args[3])) : 6.f;
			const float Delay = (Args.Num() > 4) ? FMath::Max(0.f, FCString::Atof(*Args[4])) : 0.f;

			double Elapsed = 0.0;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[Elapsed, Loadout, Action, Alpha, Hold, Delay](float DeltaTime) mutable -> bool
				{
					Elapsed += DeltaTime;
					if (Elapsed < Delay)
					{
						return true;
					}

					ATraceCharacter* Character = FindDebugLocalCharacter(FindDebugGameWorld());
					if (Character == nullptr)
					{
						// Retry: a deferred-exec batch usually fires before the first pawn exists.
						return Elapsed < Delay + 90.f;
					}

					Character->DebugHoldHandsClip(Loadout, Action, Alpha, Hold);

					FString Clip, Reported;
					float Time = -1.f, Length = -1.f;
					Character->DebugGetHandsState(Clip, Time, Length, Reported);
					UE_LOG(LogTraceGame, Display,
						TEXT("[Hands.Hold] loadout=%d action=%d alpha=%.3f for %.1fs (rig=%s)."),
						static_cast<int32>(Loadout), static_cast<int32>(Action), Alpha, Hold,
						Character->UsesPackHands() ? TEXT("pack") : TEXT("FALLBACK - nothing to hold"));
					return false;
				}),
				0.f);
		}));
}

#endif // !UE_BUILD_SHIPPING

// =================================================================================================
// SPEC v19 §4.1 — THE REPRODUCTION
//
// "If a player ever goes out of bounds of the arena, they should die and respawn."
//
// The rule is three lines of arithmetic, which is exactly the kind of rule that gets shipped broken:
// the arithmetic is trivially right and the CONSEQUENCES are the whole feature. So this asserts the
// consequences, not the arithmetic — a death, credited to nobody, with the cooldowns still running,
// and a pawn back inside the arena afterwards.
//
// WHY IT CAN GO RED, WHICH IS THE PART THAT MATTERS. Trace.Bounds.Enabled 0 removes the rule and
// nothing else, and the harness then reports FAIL with "still alive N seconds after being put
// outside". A harness whose only possible outcome is PASS proves nothing, and this project has
// already shipped one of those.
//
// It also asserts the two clauses that are easy to get wrong in the other direction and that a
// player would notice immediately:
//   * NOBODY IS CREDITED. Every other player's kill count must be unchanged. A world death that pays
//     out to whoever last shot at you is worse than no rule at all.
//   * THE COOLDOWN KEPT TICKING. Spec v19 §4.2's rule, restated by the user in the same breath, and
//     an out-of-bounds death is still a death, so it has to hold here too. Measured across the death
//     rather than argued from where the timer lives.
// =================================================================================================

#if !UE_BUILD_SHIPPING

// The five knobs are defined in TraceCharacter.cpp, beside the rule they govern, and declared in
// TraceCharacterInternal.h. Pulled in unqualified here so the harness below reads them exactly as it
// did when it and the rule shared one file - and, more to the point, so it reads THE SAME OBJECTS.
// A harness with its own copy of Trace.Bounds.Enabled could not go red when the rule is switched off.
using namespace TraceCharacterBounds;

namespace TraceCharacterBoundsVerify
{
	/** How long after the shove to wait for the rule to fire, in seconds. Grace + generous slack. */
	constexpr float DeathWindowSeconds = 3.0f;

	/** Cooldown parked on the victim before the shove, so "it kept ticking" is measurable at all. */
	constexpr float ParkedCooldownSeconds = 30.f;

	UWorld* PlayingWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr && Context.World()->GetAuthGameMode() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/** Every other player's kills, summed. The number that must not move. */
	int32 SumOtherKills(const UWorld* World, const APlayerState* Excluding)
	{
		int32 Total = 0;
		const AGameStateBase* GS = (World != nullptr) ? World->GetGameState() : nullptr;
		if (GS == nullptr)
		{
			return 0;
		}

		for (APlayerState* PS : GS->PlayerArray)
		{
			const ATracePlayerState* TracePS = Cast<ATracePlayerState>(PS);
			if (TracePS != nullptr && TracePS != Excluding)
			{
				Total += TracePS->Kills;
			}
		}
		return Total;
	}

	struct FRun
	{
		TWeakObjectPtr<ATraceCharacter> Victim;
		TWeakObjectPtr<ATracePlayerState> VictimState;
		TWeakObjectPtr<AController> VictimController;
		int32  DeathsAtStart = 0;
		int32  OtherKillsAtStart = 0;
		float  CooldownAtStart = 0.f;
		float  Elapsed = 0.f;
		bool   bDeathSeen = false;
		float  CooldownAtDeath = 0.f;
		int32  Failures = 0;
		int32  Stage = 0;   // 0 = waiting for the death, 1 = waiting for the respawn
	};

	void Report(FRun& Run, bool bPass, const FString& Detail)
	{
		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[BoundsVerify] PASS: %s"), *Detail);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[BoundsVerify] FAIL: %s"), *Detail);
		}
	}

	FAutoConsoleCommand CmdBoundsVerify(
		TEXT("Trace.Bounds.Verify"),
		TEXT("SPEC v19 §4.1. Parks a cooldown on a living player, teleports them outside the arena, and "
		     "asserts that they DIE, that nobody is credited with the kill, that the cooldown kept "
		     "ticking through the death, and that they respawn inside. Run it again with "
		     "Trace.Bounds.Enabled 0, which is the arm that must go RED."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UWorld* const World = PlayingWorld();
			if (World == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[BoundsVerify] INVALID: no authoritative world. NOT a pass - it could not run."));
				return;
			}

			const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
			const FBox Field = (Arena != nullptr) ? Arena->GetFieldBounds() : FBox(ForceInit);
			if (Field.IsValid == 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[BoundsVerify] INVALID: no arena, so there are no bounds to leave. NOT a pass."));
				return;
			}

			// A living pawn that is NOT holding the Core. The carrier would drag the possession rules
			// into a test about a boundary, and those are proven separately.
			ATraceCharacter* Victim = nullptr;
			for (TActorIterator<ATraceCharacter> It(World); It; ++It)
			{
				ATraceCharacter* const Candidate = *It;
				if (IsValid(Candidate) && Candidate->IsAlive() && !Candidate->IsCarrier()
					&& Candidate->GetController() != nullptr)
				{
					Victim = Candidate;
					break;
				}
			}

			if (Victim == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[BoundsVerify] INVALID: nobody alive to push out of the world. NOT a pass."));
				return;
			}

			TSharedRef<FRun> Run = MakeShared<FRun>();
			Run->Victim = Victim;
			Run->VictimState = Victim->GetPlayerState<ATracePlayerState>();
			Run->VictimController = Victim->GetController();
			Run->DeathsAtStart = Run->VictimState.IsValid() ? Run->VictimState->Deaths : 0;
			Run->OtherKillsAtStart = SumOtherKills(World, Run->VictimState.Get());

			// Park a cooldown so "it kept ticking" is a measurement rather than an assertion about
			// where a float lives. DebugSetActivatedCooldown works with no character assigned, which is
			// deliberate here: this harness must be runnable in the first seconds of a match, before
			// the bots have claimed characters.
			if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Run->VictimState.Get()))
			{
				Abilities->DebugSetActivatedCooldown(ParkedCooldownSeconds);
				Run->CooldownAtStart = Abilities->GetActivatedCooldownRemaining();
			}

			// Well outside, along +X, and far enough that no margin setting could call it inside.
			const FVector Outside(
				Field.Max.X + FMath::Max(4000.f, CVarBoundsMarginXY.GetValueOnAnyThread() * 2.f),
				Field.GetCenter().Y,
				Field.Min.Z + 400.0);

			UE_LOG(LogTraceGame, Display,
				TEXT("[BoundsVerify] ===== pushing %s from %s to %s (the arena runs X %.0f..%.0f, Y %.0f..%.0f, ")
				TEXT("Z %.0f..%.0f; margins XY %.0f, below %.0f, ceiling %.0f = %s; grace %.2fs) | rule = %s ====="),
				*Victim->GetName(), *Victim->GetActorLocation().ToCompactString(), *Outside.ToCompactString(),
				Field.Min.X, Field.Max.X, Field.Min.Y, Field.Max.Y, Field.Min.Z, Field.Max.Z,
				CVarBoundsMarginXY.GetValueOnAnyThread(), CVarBoundsMarginBelow.GetValueOnAnyThread(),
				CVarBoundsCeilingMargin.GetValueOnAnyThread(),
				(CVarBoundsCeilingMargin.GetValueOnAnyThread() > 0.f) ? TEXT("ON") : TEXT("OFF (Lily flies)"),
				CVarBoundsGraceSeconds.GetValueOnAnyThread(),
				(CVarBoundsEnabled.GetValueOnAnyThread() != 0)
					? TEXT("v19 §4.1 ON") : TEXT("OFF - THE RED ARM, ARMED"));

			Victim->SetActorLocation(Outside, false, nullptr, ETeleportType::TeleportPhysics);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Run](float DeltaTime) -> bool
				{
					Run->Elapsed += DeltaTime;

					UWorld* const TickWorld = PlayingWorld();
					if (TickWorld == nullptr || !Run->VictimState.IsValid())
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[BoundsVerify] INVALID: the world or the player went away mid-run. NOT a pass."));
						return false;
					}

					UTraceAbilityComponent* const Abilities =
						UTraceAbilityComponent::Get(Run->VictimState.Get());
					const float CooldownNow =
						(Abilities != nullptr) ? Abilities->GetActivatedCooldownRemaining() : 0.f;

					// --- Stage 0: did the rule kill them? -------------------------------------------
					if (Run->Stage == 0)
					{
						const bool bDiedByCount = (Run->VictimState->Deaths > Run->DeathsAtStart);
						const bool bDiedByPawn = Run->Victim.IsValid() && !Run->Victim->IsAlive();

						if (bDiedByCount || bDiedByPawn)
						{
							Run->bDeathSeen = true;
							Run->CooldownAtDeath = CooldownNow;
							Run->Stage = 1;

							const int32 OtherKillsNow = SumOtherKills(TickWorld, Run->VictimState.Get());
							if (OtherKillsNow != Run->OtherKillsAtStart)
							{
								++Run->Failures;
								Report(*Run, false, FString::Printf(
									TEXT("somebody was CREDITED with the out-of-bounds death (other players' "
									     "kills %d -> %d). It must be creditable to nobody."),
									Run->OtherKillsAtStart, OtherKillsNow));
							}

							// The cooldown must be RUNNING, and must be LOWER than it was: a reset would
							// snap it back to 30, and a stopped clock would hold it exactly.
							if (Abilities == nullptr)
							{
								++Run->Failures;
								Report(*Run, false, TEXT("the victim has no ability component, so the cooldown "
								                         "clause could not be measured. NOT a pass."));
							}
							else if (CooldownNow <= 0.f)
							{
								++Run->Failures;
								Report(*Run, false, FString::Printf(
									TEXT("the cooldown was CLEARED by the death (%.2fs -> %.2fs). Spec v19 §4.2: "
									     "cooldowns keep ticking down through a death."),
									Run->CooldownAtStart, CooldownNow));
							}
							else if (CooldownNow >= Run->CooldownAtStart)
							{
								++Run->Failures;
								Report(*Run, false, FString::Printf(
									TEXT("the cooldown did not TICK across the death (%.2fs -> %.2fs). It must "
									     "keep counting down, not freeze or restart."),
									Run->CooldownAtStart, CooldownNow));
							}

							return true;   // now wait for the respawn
						}

						if (Run->Elapsed >= DeathWindowSeconds)
						{
							Report(*Run, false, FString::Printf(
								TEXT("%s was still ALIVE %.1fs after being teleported outside the arena. Spec "
								     "v19 §4.1 says out of bounds is a death. Rule = %s."),
								*GetNameSafe(Run->Victim.Get()), Run->Elapsed,
								(CVarBoundsEnabled.GetValueOnAnyThread() != 0)
									? TEXT("ON - THIS IS A REAL FAILURE")
									: TEXT("OFF - this is the RED ARM reproducing correctly")));
							return false;
						}

						return true;
					}

					// --- Stage 1: did they come back, inside? ---------------------------------------
					const APawn* const FreshPawn = Run->VictimController.IsValid()
						? Run->VictimController->GetPawn() : nullptr;
					const ATraceCharacter* const FreshCharacter = Cast<ATraceCharacter>(FreshPawn);

					if (FreshCharacter != nullptr && FreshCharacter->IsAlive())
					{
						FString Unused;
						const bool bInside = !ATraceCharacter::IsLocationOutOfArenaBounds(
							TickWorld, FreshCharacter->GetActorLocation(), Unused);

						if (!bInside)
						{
							++Run->Failures;
							Report(*Run, false, FString::Printf(
								TEXT("they respawned at %s, which is STILL out of bounds - so the rule would "
								     "kill them again in a loop."),
								*FreshCharacter->GetActorLocation().ToCompactString()));
						}

						Report(*Run, Run->Failures == 0, FString::Printf(
							TEXT("%s went out of bounds, DIED (deaths %d -> %d, cause is not creditable to any "
							     "enemy - their kills stayed at %d), their E cooldown kept ticking straight "
							     "through it (%.2fs at the push -> %.2fs at the death -> %.2fs now, never "
							     "reset), and they respawned INSIDE at %s. %d sub-check(s) failed."),
							*GetNameSafe(FreshCharacter), Run->DeathsAtStart, Run->VictimState->Deaths,
							Run->OtherKillsAtStart, Run->CooldownAtStart, Run->CooldownAtDeath, CooldownNow,
							*FreshCharacter->GetActorLocation().ToCompactString(), Run->Failures));
						return false;
					}

					if (Run->Elapsed >= DeathWindowSeconds + 15.f)
					{
						Report(*Run, false, FString::Printf(
							TEXT("%s died out of bounds but never respawned within %.0fs. 'Die AND respawn' is "
							     "one requirement, not two."),
							*GetNameSafe(Run->Victim.Get()), Run->Elapsed));
						return false;
					}

					return true;
				}),
				0.f);
		}));
}

#endif // !UE_BUILD_SHIPPING

