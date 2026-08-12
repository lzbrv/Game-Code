// =================================================================================================
// Trace — TraceRailgunVerify.cpp
//
// Console commands that report what the railgun viewmodel ACTUALLY BUILT, and that pin its fire
// animation so a screenshot can catch a pose which otherwise lasts about a third of a second.
//
//   Trace.Railgun.Probe          what rig is on screen, every part, every material slot
//   Trace.Railgun.Hold <alpha>   pin the fire pose (0 = discharge, 1 = rest) for 5 s
//   Trace.Railgun.Fire           play the discharge once, exactly as pulling the trigger does
//
// WHY THIS EXISTS. "The gun is in the game" is not something a log line can establish: the mesh can
// resolve and still be scaled wrong, hidden behind the hand, or wearing no material. Probe prints
// the numbers a screenshot cannot (which asset, which slot, what scale) and Hold makes the
// screenshot possible at all. Together they are the difference between believing the weapon works
// and having shown it.
//
// Dev-only; the whole file compiles out of Shipping.
// =================================================================================================
#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Core/TraceCharacter.h"
#include "Trace.h"
#include "Gameplay/TraceRailgunFireCurve.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"                 // FScreenshotRequest

// The namespace is named after the file on purpose. Anonymous namespaces are file-local in a
// standard build but NOT in UBT's unity/jumbo builds, where several .cpp files are concatenated into
// one translation unit and two same-named statics collide (MSVC C2084). This project has been broken
// that way once already; Scripts/check-jumbo-build-collisions.py now guards it, and the convention
// that keeps the guard quiet is this one.
namespace TraceRailgunVerifyLocal
{
	ATraceCharacter* FindLocalCharacter(UWorld* World)
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
				if (ATraceCharacter* Character = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					return Character;
				}
			}
		}
		return nullptr;
	}

	void ReportPart(const TCHAR* Label, UStaticMeshComponent* Part)
	{
		if (Part == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Railgun]   %s: NULL"), Label);
			return;
		}

		const UStaticMesh* Mesh = Part->GetStaticMesh();
		const FVector Loc = Part->GetRelativeLocation();
		const FRotator Rot = Part->GetRelativeRotation();
		const FVector Scale = Part->GetRelativeScale3D();

		UE_LOG(LogTraceGame, Warning,
			TEXT("[Railgun]   %s: mesh=%s loc=(%.2f, %.2f, %.2f) rot=(P%.2f Y%.2f R%.2f) scale=%.3f visible=%d"),
			Label,
			Mesh != nullptr ? *Mesh->GetName() : TEXT("NONE"),
			Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Rot.Yaw, Rot.Roll, Scale.X,
			Part->IsVisible() ? 1 : 0);

		const int32 NumSlots = Part->GetNumMaterials();
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			const UMaterialInterface* Material = Part->GetMaterial(Slot);
			const FName SlotName = (Mesh != nullptr && Mesh->GetStaticMaterials().IsValidIndex(Slot))
				? Mesh->GetStaticMaterials()[Slot].MaterialSlotName : NAME_None;
			UE_LOG(LogTraceGame, Warning, TEXT("[Railgun]       slot %d '%s' -> %s"),
				Slot, *SlotName.ToString(),
				Material != nullptr ? *Material->GetName() : TEXT("NONE"));
		}
	}

	FAutoConsoleCommandWithWorld CmdProbe(
		TEXT("Trace.Railgun.Probe"),
		TEXT("Reports the first-person weapon rig actually on screen: whether it is the imported "
		     "railgun or the fallback cube gun, every part's mesh, transform and material slots, and "
		     "where the muzzle sits in rig space."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			ATraceCharacter* Character = FindLocalCharacter(World);
			if (Character == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Railgun] No local ATraceCharacter yet - run this once a match has started."));
				return;
			}

			const bool bRailgun = Character->UsesRailgunViewModel();
			UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] ===================================="));
			UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] rig=%s parts=%d visible=%d"),
				bRailgun ? TEXT("RAILGUN (imported art)") : TEXT("FALLBACK (procedural cubes)"),
				Character->GetViewModelPartCount(),
				Character->IsViewModelVisible() ? 1 : 0);

			if (!bRailgun)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Railgun] The railgun art did not resolve, or -TraceNoRailgun was passed. "
					     "Content/Trace/Weapons must contain SM_Railgun_Body/RailL/RailR."));
				return;
			}

			UStaticMeshComponent* Body = nullptr;
			UStaticMeshComponent* Left = nullptr;
			UStaticMeshComponent* Right = nullptr;
			Character->DebugGetRailgunParts(Body, Left, Right);

			ReportPart(TEXT("body  "), Body);
			ReportPart(TEXT("railL "), Left);
			ReportPart(TEXT("railR "), Right);

			float LiveCyan = -1.f;
			float LiveAmber = -1.f;
			const bool bReadBack = Character->DebugGetRailgunEmissive(LiveCyan, LiveAmber);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Railgun] live EmissiveIntensity: cyan=%.3f amber=%.3f (readback %s)"),
				LiveCyan, LiveAmber,
				bReadBack ? TEXT("OK") : TEXT("FAILED - the parameter is not on the material"));

			UE_LOG(LogTraceGame, Warning,
				TEXT("[Railgun] curve: clip %.2fs, discharge at %.2fs, peak %.2fx cyan / %.2fx amber"),
				TraceRailgunFireCurve::ClipSeconds, TraceRailgunFireCurve::DischargeSeconds,
				TraceRailgunFireCurve::PeakCyan, TraceRailgunFireCurve::PeakAmber);
			UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] ===================================="));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdHold(
		TEXT("Trace.Railgun.Hold"),
		TEXT("Pins the fire pose for 5 seconds so a screenshot can catch it. "
		     "0 = the discharge frame (rails thrown fully apart, glow at peak), 1 = rest. "
		     "Trace.Railgun.Hold -1 releases."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
		{
			ATraceCharacter* Character = FindLocalCharacter(World);
			if (Character == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] No local ATraceCharacter yet."));
				return;
			}

			const float Alpha = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 0.f;
			const float Seconds = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : 5.f;
			Character->DebugHoldRailgunPhase(Alpha, Seconds);

			// What the pose SHOULD be, straight off the authored table, so the log says what to
			// expect before the next line says what actually landed on the material.
			const float ClipTime = TraceRailgunFireCurve::DischargeSeconds
				+ FMath::Clamp(Alpha, 0.f, 1.f)
					* (TraceRailgunFireCurve::ClipSeconds - TraceRailgunFireCurve::DischargeSeconds);
			float WantCyan = 1.f;
			float WantAmber = 1.f;
			TraceRailgunFireCurve::Sample(ClipTime, WantCyan, WantAmber);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Railgun] holding alpha=%.2f for %.1fs -> clip t=%.3fs, expect cyan=%.3f amber=%.3f"),
				Alpha, Seconds, ClipTime, WantCyan, WantAmber);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Railgun.Verify — the A/B that actually settles it.
	//
	// Reading the material back in the SAME FRAME the hold is set reports the pre-tick value and
	// looks like a failure, which is how the first attempt at this misled me. The animation is
	// applied by UpdateRailgunFire during Tick, so every read and every screenshot here is taken a
	// full step after the state that produced it.
	//
	// The two frames are captured seconds apart in a live match on purpose: comparing them by eye is
	// worthless when the arena's own lighting has moved between them, so the verdict below rests on
	// the material parameter and the rail geometry, which are exact.
	// ---------------------------------------------------------------------------------------------
	struct FVerifyState
	{
		int32 Step = 0;
		float RestCyan = -1.f;
		float RestAmber = -1.f;
		float RestRailY = 0.f;
		float FireCyan = -1.f;
		float FireAmber = -1.f;
		float FireRailY = 0.f;
		FTimerHandle Handle;
	};

	FVerifyState GVerify;

	void Shot(const TCHAR* Tag)
	{
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"),
			FString::Printf(TEXT("Railgun_%s_pid%d.png"), Tag, FPlatformProcess::GetCurrentProcessId()));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] screenshot -> %s"), *Path);
	}

	void VerifyStep(UWorld* World)
	{
		ATraceCharacter* Character = FindLocalCharacter(World);
		if (Character == nullptr || !Character->UsesRailgunViewModel())
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[Railgun] VERDICT: FAIL - no railgun rig to verify."));
			return;
		}

		UStaticMeshComponent* Body = nullptr;
		UStaticMeshComponent* Left = nullptr;
		UStaticMeshComponent* Right = nullptr;
		Character->DebugGetRailgunParts(Body, Left, Right);

		const auto RailSpread = [&]() -> float
		{
			return (Left != nullptr && Right != nullptr)
				? (Right->GetRelativeLocation().Y - Left->GetRelativeLocation().Y) : 0.f;
		};

		switch (GVerify.Step)
		{
		case 0:
			// Rest: make sure nothing is held or mid-shot, then let a tick settle it.
			Character->DebugHoldRailgunPhase(1.f, 1.5f);
			break;

		case 1:
			Character->DebugGetRailgunEmissive(GVerify.RestCyan, GVerify.RestAmber);
			GVerify.RestRailY = RailSpread();
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Railgun] REST      cyan=%.3f amber=%.3f railSpread=%.3f uu"),
				GVerify.RestCyan, GVerify.RestAmber, GVerify.RestRailY);
			Shot(TEXT("rest"));
			break;

		case 2:
			Character->DebugHoldRailgunPhase(0.f, 6.f);
			break;

		case 3:
			Character->DebugGetRailgunEmissive(GVerify.FireCyan, GVerify.FireAmber);
			GVerify.FireRailY = RailSpread();
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Railgun] DISCHARGE cyan=%.3f amber=%.3f railSpread=%.3f uu"),
				GVerify.FireCyan, GVerify.FireAmber, GVerify.FireRailY);
			Shot(TEXT("discharge"));
			break;

		default:
		{
			Character->DebugHoldRailgunPhase(-1.f, 0.f);

			// Expected values come from the authored table and the layout constants, not from what
			// was observed — a check that derives its expectation from the measurement always passes.
			const float WantCyan = TraceRailgunFireCurve::PeakCyan;
			const float WantAmber = TraceRailgunFireCurve::PeakAmber;
			const bool bGlow = FMath::IsNearlyEqual(GVerify.FireCyan, WantCyan, 0.05f)
				&& FMath::IsNearlyEqual(GVerify.FireAmber, WantAmber, 0.05f)
				&& FMath::IsNearlyEqual(GVerify.RestCyan, 1.f, 0.05f);
			const float Spread = GVerify.FireRailY - GVerify.RestRailY;
			const bool bRails = Spread > 0.5f;

			UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] ---- VERDICT ----"));
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Railgun] glow  %s: rest %.3f -> discharge %.3f (want 1.000 -> %.3f)"),
				bGlow ? TEXT("PASS") : TEXT("FAIL"), GVerify.RestCyan, GVerify.FireCyan, WantCyan);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Railgun] rails %s: spread %.3f -> %.3f uu (opened %.3f uu)"),
				bRails ? TEXT("PASS") : TEXT("FAIL"), GVerify.RestRailY, GVerify.FireRailY, Spread);
			UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] VERDICT: %s"),
				(bGlow && bRails) ? TEXT("PASS") : TEXT("FAIL"));
			return;
		}
		}

		++GVerify.Step;
		World->GetTimerManager().SetTimer(GVerify.Handle,
			FTimerDelegate::CreateStatic(&VerifyStep, World), 0.5f, /*bLoop=*/false);
	}

	FAutoConsoleCommandWithWorld CmdVerify(
		TEXT("Trace.Railgun.Verify"),
		TEXT("Proves the fire animation actually reaches the renderer: samples the live material and "
		     "the rail geometry at rest and at the discharge frame, photographs both, and ends in a "
		     "PASS/FAIL verdict against the authored curve."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (World == nullptr)
			{
				return;
			}
			GVerify = FVerifyState();
			VerifyStep(World);
		}));

	FAutoConsoleCommandWithWorld CmdFire(
		TEXT("Trace.Railgun.Fire"),
		TEXT("Plays the discharge animation once, exactly as firing does. Cosmetic only - no shot is "
		     "taken, nothing is sent to the server."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			ATraceCharacter* Character = FindLocalCharacter(World);
			if (Character == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] No local ATraceCharacter yet."));
				return;
			}
			Character->NotifyWeaponFired();
			UE_LOG(LogTraceGame, Warning, TEXT("[Railgun] fired (cosmetic)."));
		}));
}

#endif // !UE_BUILD_SHIPPING
