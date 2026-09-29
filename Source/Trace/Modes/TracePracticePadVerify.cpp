// Trace — Trace.Practice.PadLabels: can the player READ the range's pads from where the range puts them?
//
// ===================================================================================================
// THE BUG THIS IS ABOUT
// ===================================================================================================
//
// The INFINITE ABILITIES and CHANGE CHARACTER pads sit 450 uu in front of the spawn line, and their
// labels were UTextRenderComponents spawned with the pad's zero rotation. An engine text render faces
// +X and reads correctly only from its +X side; the spawn line looks down +X. So from where the range
// puts the player, both labels read MIRRORED — in the engine's default font, with lower-case "walk on
// to..." notes, and "CHANGE CHARACTER" on a pad that now opens the loadout page.
//
// The labels are now kit plates the HUD draws over each pad (ATraceHUD::DrawPracticePadLabels). This
// harness stands the player on the range's own spawn line, AIMS at each pad in turn, and asks what is
// actually on screen, from the HUD's draw record and from the world. The CORE RACK sits on the centre
// platform, 900 uu BEHIND the spawn line and out of its sight, so it is aimed at twice: from the spawn
// line (where the structure over the spawn hides it, and its plate must not show through that) and
// from the platform itself, where a player walking up to it stands.
//
//   1. the pad's label is drawn where the player is looking, whole (not faded), on an OPAQUE plate —
//      or, where world geometry is in the way (the harness traces that itself), not drawn through it;
//   2. every label the player can see reads LEFT TO RIGHT from there. A world-space text render on a
//      pad is projected, both ends, through the player's own view: if its end lands left of its start,
//      it is mirrored. (This is the check the old pads fail. The HUD's plates read left to right by
//      construction, and the check says which kind it measured.)
//   3. the words are the pad's own line (ATracePracticePad::LabelFor), one line, no lower case, short;
//      and the loadout pad does not talk about characters.
//   4. switching INFINITE ABILITIES on puts the ON line on an amber-ringed plate, and off again.
//
// Headless recipe (the range is the arena with the practice game mode):
//   <Arena_Baked>?game=/Script/Trace.TracePracticeGameMode  -TraceExecAt=12 -TraceExec="Trace.Loadout.Press lock"
//   -TraceExec2At=16 -TraceExec2="Trace.Practice.PadLabels"
// It waits up to 20 s for no overlay to be up (the loadout page opens first on the range).

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Components/TextRenderComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                  // TActorIterator
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"

#include "Modes/TracePracticeActors.h"
#include "Modes/TracePracticeRange.h"
#include "Trace.h"                        // LogTraceGame
#include "UI/TraceHUD.h"

// Named after the file, not anonymous: Scripts/check-jumbo-build-collisions.py.
namespace TracePracticePadVerify
{
	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		int32 Step = 0;
		double StepStart = 0.0;
		double Deadline = 0.0;
		int32 Passes = 0;
		int32 Failures = 0;

		/** One look: which pad, and whether the player stands on the spawn line or beside that pad. */
		struct FView
		{
			TWeakObjectPtr<ATracePracticePad> Pad;
			bool bFromSpawnLine = true;
		};

		/** The toggle pads from the spawn line, then the rack from the spawn line and from its platform. */
		TArray<FView> Views;
		int32 ViewIndex = 0;
		TWeakObjectPtr<ATracePracticePad> InfinitePad;
	};

	static void Report(FRun& Run, bool bPass, const FString& Claim, const FString& Detail)
	{
		(bPass ? Run.Passes : Run.Failures) += 1;
		UE_LOG(LogTraceGame, Display, TEXT("[PadLabels]   %-4s %s  %s"), bPass ? TEXT("ok") : TEXT("FAIL"), *Claim, *Detail);
	}

	static void GoTo(FRun& Run, int32 Step)
	{
		Run.Step = Step;
		Run.StepStart = FPlatformTime::Seconds();
	}

	static ATraceHUD* LocalHud(UWorld* WorldPtr)
	{
		APlayerController* const LocalPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		return (LocalPC != nullptr) ? Cast<ATraceHUD>(LocalPC->GetHUD()) : nullptr;
	}

	static const ATraceHUD::FHudKitRecord::FPadLabel* FindDrawn(const ATraceHUD& Hud, ETracePracticePadRole Role)
	{
		for (const ATraceHUD::FHudKitRecord::FPadLabel& Drawn : Hud.GetHudKitRecord().PadLabels)
		{
			if (Drawn.PadRoleIndex == static_cast<int32>(Role))
			{
				return &Drawn;
			}
		}
		return nullptr;
	}

	static void AimAt(APlayerController& LocalPC, const FVector& Target)
	{
		FVector EyeLocation = FVector::ZeroVector;
		FRotator EyeRotation = FRotator::ZeroRotator;
		LocalPC.GetPlayerViewPoint(EyeLocation, EyeRotation);
		LocalPC.SetControlRotation((Target - EyeLocation).Rotation());
	}

	/** The harness's OWN answer to "is there a wall between the player's eye and this label?". */
	static bool IsLabelOccluded(UWorld& WorldRef, APlayerController& LocalPC, const ATracePracticePad& Pad)
	{
		FVector EyeLocation = FVector::ZeroVector;
		FRotator EyeRotation = FRotator::ZeroRotator;
		LocalPC.GetPlayerViewPoint(EyeLocation, EyeRotation);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TracePadVerifySight), /*bTraceComplex=*/false);
		Params.AddIgnoredActor(&Pad);
		if (const APawn* const ViewPawn = LocalPC.GetPawn())
		{
			Params.AddIgnoredActor(ViewPawn);
		}
		return WorldRef.LineTraceTestByObjectType(EyeLocation, Pad.GetLabelAnchor(),
			FCollisionObjectQueryParams(ECC_WorldStatic), Params);
	}

	/** Puts the player where @p View says, facing where a player arriving there would face. */
	static void StandFor(const FRun::FView& View, APlayerController& LocalPC, const UTracePracticeRangeSubsystem& Range)
	{
		APawn* const PlayerPawn = LocalPC.GetPawn();
		const ATracePracticePad* const Pad = View.Pad.Get();
		const AActor* const StartPost = Range.GetPlayerStartPost();
		if (PlayerPawn == nullptr || Pad == nullptr || StartPost == nullptr)
		{
			return;
		}
		if (View.bFromSpawnLine)
		{
			PlayerPawn->TeleportTo(StartPost->GetActorLocation() + FVector(0.f, 0.f, 120.f), StartPost->GetActorRotation(),
				/*bIsATest=*/false, /*bNoCheck=*/true);
		}
		else
		{
			// On the pad's own floor, 350 uu toward the spawn side, facing it: walking up to the rack.
			PlayerPawn->TeleportTo(Pad->GetActorLocation() + FVector(350.f, 0.f, 100.f), FRotator(0.f, 180.f, 0.f),
				/*bIsATest=*/false, /*bNoCheck=*/true);
		}
	}

	/**
	 * Every VISIBLE world-space text on @p Pad, projected through the player's view: true when all of
	 * them read left to right (their last glyph lands right of their first). An engine text render
	 * advances along its local -Y, so its reading line runs from +Y to -Y through the component.
	 */
	static bool WorldTextReadsLeftToRight(APlayerController& LocalPC, const ATracePracticePad& Pad, int32& OutSeen, FString& OutDetail)
	{
		OutSeen = 0;
		bool bAllReadable = true;
		TArray<UTextRenderComponent*> Texts;
		Pad.GetComponents<UTextRenderComponent>(Texts);
		for (const UTextRenderComponent* WorldText : Texts)
		{
			if (WorldText == nullptr || !WorldText->IsVisible() || WorldText->bHiddenInGame)
			{
				continue;
			}
			++OutSeen;
			const FVector Middle = WorldText->GetComponentLocation();
			const FVector Along = -WorldText->GetRightVector() * 50.f;
			FVector2D First;
			FVector2D Last;
			const bool bProjected = UGameplayStatics::ProjectWorldToScreen(&LocalPC, Middle - Along, First)
				&& UGameplayStatics::ProjectWorldToScreen(&LocalPC, Middle + Along, Last);
			const bool bReadable = bProjected && Last.X > First.X;
			bAllReadable &= bReadable;
			OutDetail += FString::Printf(TEXT("world text \"%s\": first glyph at x=%.0f, last at x=%.0f (%s); "),
				*WorldText->Text.ToString().Replace(TEXT("\n"), TEXT(" / ")), First.X, Last.X,
				bReadable ? TEXT("reads left to right") : TEXT("MIRRORED"));
		}
		return bAllReadable;
	}

	static bool IsHouseStyle(const FString& Text)
	{
		if (Text.IsEmpty() || Text.Len() > 24 || Text.Contains(TEXT("\n")))
		{
			return false;
		}
		for (const TCHAR Char : Text)
		{
			if (FChar::IsLower(Char))
			{
				return false;
			}
		}
		return true;
	}

	static const TCHAR* RoleName(ETracePracticePadRole Role)
	{
		switch (Role)
		{
		case ETracePracticePadRole::CoreRack:          return TEXT("core rack");
		case ETracePracticePadRole::InfiniteAbilities: return TEXT("infinite abilities");
		default:                                       return TEXT("loadout");
		}
	}

	static void Finish(FRun& Run)
	{
		if (Run.Failures == 0 && Run.Passes > 0)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[PadLabels] VERDICT: ===== PASS (%d checks) ====="), Run.Passes);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[PadLabels] VERDICT: ===== *** FAIL *** %d of %d check(s) ====="),
				Run.Failures, Run.Failures + Run.Passes);
		}
	}

	/** One ticker pass. Returns false when the run is over. */
	static bool Tick(FRun& Run)
	{
		UWorld* const WorldPtr = Run.World.Get();
		APlayerController* const LocalPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		ATraceHUD* const Hud = LocalHud(WorldPtr);
		UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(WorldPtr);
		if (WorldPtr == nullptr || LocalPC == nullptr || Hud == nullptr || Range == nullptr)
		{
			Report(Run, false, TEXT("a practice-range world with a local player, a HUD and the range"), TEXT("missing"));
			Finish(Run);
			return false;
		}
		if (FPlatformTime::Seconds() > Run.Deadline)
		{
			Report(Run, false, TEXT("the run finished inside its time limit"), FString::Printf(TEXT("stuck at step %d"), Run.Step));
			Finish(Run);
			return false;
		}

		const double SinceStep = FPlatformTime::Seconds() - Run.StepStart;
		switch (Run.Step)
		{
		case 0:   // wait for the range to be built and for no overlay (the loadout page opens first)
		{
			APawn* const PlayerPawn = LocalPC->GetPawn();
			if (!Range->IsBuilt() || Range->GetPlayerStartPost() == nullptr || PlayerPawn == nullptr
				|| Hud->GetHudKitRecord().bOverlayUp)
			{
				if (SinceStep > 20.0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[PadLabels]   INCONCLUSIVE: no live frame on a built range within 20 s (built %d, post %d, pawn %d, overlay %d)."),
						Range->IsBuilt() ? 1 : 0, Range->GetPlayerStartPost() != nullptr ? 1 : 0, PlayerPawn != nullptr ? 1 : 0,
						Hud->GetHudKitRecord().bOverlayUp ? 1 : 0);
					Run.Failures += 1;
					Finish(Run);
					return false;
				}
				return true;
			}

			TArray<ATracePracticePad*> ByRole;
			for (const ETracePracticePadRole Role : { ETracePracticePadRole::InfiniteAbilities,
				ETracePracticePadRole::CharacterSwap, ETracePracticePadRole::CoreRack })
			{
				for (TActorIterator<ATracePracticePad> It(WorldPtr); It; ++It)
				{
					if (It->GetPadRole() == Role)
					{
						ByRole.Add(*It);
						break;
					}
				}
			}
			Report(Run, ByRole.Num() == 3, TEXT("the range has its three pads"), FString::Printf(TEXT("%d found"), ByRole.Num()));
			for (ATracePracticePad* const Pad : ByRole)
			{
				Run.Views.Add({ Pad, /*bFromSpawnLine=*/true });
				if (Pad->GetPadRole() == ETracePracticePadRole::CoreRack)
				{
					Run.Views.Add({ Pad, /*bFromSpawnLine=*/false });
				}
				if (Pad->GetPadRole() == ETracePracticePadRole::InfiniteAbilities)
				{
					Run.InfinitePad = Pad;
				}
			}
			Range->SetInfiniteAbilities(false);
			GoTo(Run, 1);
			return true;
		}

		case 1:   // stand where the next look is taken from, and aim at the pad's label
		{
			if (!Run.Views.IsValidIndex(Run.ViewIndex))
			{
				GoTo(Run, 3);
				return true;
			}
			const FRun::FView& View = Run.Views[Run.ViewIndex];
			if (SinceStep < 0.05)
			{
				StandFor(View, *LocalPC, *Range);
				return true;
			}
			if (SinceStep < 0.3)
			{
				return true;
			}
			if (const ATracePracticePad* const Pad = View.Pad.Get())
			{
				AimAt(*LocalPC, Pad->GetLabelAnchor());
			}
			GoTo(Run, 2);
			return true;
		}

		case 2:   // ...and read what is on screen
		{
			if (SinceStep < 0.4)
			{
				return true;
			}
			const FRun::FView& View = Run.Views[Run.ViewIndex];
			const ATracePracticePad* const Pad = View.Pad.Get();
			if (Pad != nullptr)
			{
				const ETracePracticePadRole Role = Pad->GetPadRole();
				const FString Says = Pad->GetPadLabel();
				const ATraceHUD::FHudKitRecord::FPadLabel* const Drawn = FindDrawn(*Hud, Role);
				const FVector2D Aim = Hud->GetHudKitRecord().ViewSize * 0.5f;
				const TCHAR* const From = View.bFromSpawnLine ? TEXT("on the spawn line") : TEXT("beside it");

				if (IsLabelOccluded(*WorldPtr, *LocalPC, *Pad))
				{
					// Beside the pad, nothing may be in the way: that is where a player reads it from.
					if (!View.bFromSpawnLine)
					{
						Report(Run, false, FString::Printf(TEXT("a player beside the %s pad has a clear sight of its label"), RoleName(Role)),
							TEXT("world geometry between the eye and the label anchor"));
						++Run.ViewIndex;
						GoTo(Run, 1);
						return true;
					}

					// A wall in the way: the right answer is NO plate, not one floating through the wall.
					Report(Run, Drawn == nullptr,
						FString::Printf(TEXT("with a wall between them, a player %s is not shown the %s pad's label through it"), From, RoleName(Role)),
						(Drawn != nullptr) ? FString::Printf(TEXT("drew \"%s\""), *Drawn->Text) : FString(TEXT("nothing drawn")));
					++Run.ViewIndex;
					GoTo(Run, 1);
					return true;
				}

				Report(Run, Drawn != nullptr && Drawn->Rect.IsInside(Aim) && Drawn->Alpha >= 0.9f,
					FString::Printf(TEXT("the %s pad's label is drawn where a player %s is looking"), RoleName(Role), From),
					(Drawn != nullptr)
						? FString::Printf(TEXT("\"%s\" plate (%.0f,%.0f)-(%.0f,%.0f), aim (%.0f,%.0f), alpha %.2f"), *Drawn->Text,
							Drawn->Rect.Min.X, Drawn->Rect.Min.Y, Drawn->Rect.Max.X, Drawn->Rect.Max.Y, Aim.X, Aim.Y, Drawn->Alpha)
						: FString(TEXT("the HUD drew no label for it")));

				// An OPAQUE plate: at the HUD's panel alpha the range's bright pillar stripes ran through it
				// and behind the words. Only the label's own distance fade may thin it.
				if (Drawn != nullptr)
				{
					Report(Run, Drawn->PlateAlpha >= Drawn->Alpha - 0.001f,
						FString::Printf(TEXT("*** the %s pad's plate is opaque, so the arena does not show through its words ***"), RoleName(Role)),
						FString::Printf(TEXT("plate alpha %.3f at label alpha %.3f"), Drawn->PlateAlpha, Drawn->Alpha));
				}

				int32 WorldTexts = 0;
				FString WorldDetail;
				const bool bWorldReadable = WorldTextReadsLeftToRight(*LocalPC, *Pad, WorldTexts, WorldDetail);
				Report(Run, bWorldReadable && (WorldTexts > 0 || Drawn != nullptr),
					FString::Printf(TEXT("every label on the %s pad reads left to right from %s"), RoleName(Role),
						View.bFromSpawnLine ? TEXT("the spawn line") : TEXT("beside it")),
					WorldTexts > 0 ? WorldDetail : FString(TEXT("no world-space text; the HUD's plate is drawn left to right")));

				const FString Shown = (Drawn != nullptr) ? Drawn->Text : FString();
				Report(Run, Shown == Says && IsHouseStyle(Shown),
					FString::Printf(TEXT("the %s pad shows its own line, one short upper-case line"), RoleName(Role)),
					FString::Printf(TEXT("shows \"%s\", the pad says \"%s\""), *Shown, *Says));

				if (Role == ETracePracticePadRole::CharacterSwap)
				{
					Report(Run, !Says.Contains(TEXT("CHARACTER")) && !WorldDetail.Contains(TEXT("CHARACTER")),
						TEXT("the loadout pad talks about the loadout, not a character"),
						FString::Printf(TEXT("\"%s\""), *Says));
				}
			}
			++Run.ViewIndex;
			GoTo(Run, 1);
			return true;
		}

		case 3:   // INFINITE ABILITIES on, from the spawn line: the plate says so, on the amber-ringed plate
		{
			if (SinceStep < 0.05)
			{
				Range->SetInfiniteAbilities(true);
				if (Run.InfinitePad.IsValid())
				{
					StandFor({ Run.InfinitePad, /*bFromSpawnLine=*/true }, *LocalPC, *Range);
				}
				return true;
			}
			if (SinceStep < 0.3)
			{
				return true;
			}
			if (const ATracePracticePad* const Pad = Run.InfinitePad.Get())
			{
				AimAt(*LocalPC, Pad->GetLabelAnchor());
			}
			GoTo(Run, 4);
			return true;
		}

		case 4:
		{
			if (SinceStep < 0.4)
			{
				return true;
			}
			const ATraceHUD::FHudKitRecord::FPadLabel* const Drawn = FindDrawn(*Hud, ETracePracticePadRole::InfiniteAbilities);
			const FString OnLine = ATracePracticePad::LabelFor(ETracePracticePadRole::InfiniteAbilities, true);
			const FString OffLine = ATracePracticePad::LabelFor(ETracePracticePadRole::InfiniteAbilities, false);
			Report(Run, Drawn != nullptr && Drawn->bLit && Drawn->Text == OnLine && OnLine != OffLine,
				TEXT("switched ON, the infinite-abilities plate says ON and wears the amber ring"),
				(Drawn != nullptr)
					? FString::Printf(TEXT("\"%s\", lit %d (on line \"%s\", off line \"%s\")"), *Drawn->Text, Drawn->bLit ? 1 : 0, *OnLine, *OffLine)
					: FString(TEXT("not drawn")));

			// A harness leaves the range the way the next player expects it.
			Range->SetInfiniteAbilities(false);
			Finish(Run);
			return false;
		}

		default:
			Finish(Run);
			return false;
		}
	}

	static void Start(const TArray<FString>& /*Args*/, UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr || !TracePracticeRange::IsActive(WorldPtr))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[PadLabels] Trace.Practice.PadLabels runs on the practice range only."));
			return;
		}

		TSharedRef<FRun> Run = MakeShared<FRun>();
		Run->World = WorldPtr;
		Run->Deadline = FPlatformTime::Seconds() + 60.0;
		GoTo(*Run, 0);

		UE_LOG(LogTraceGame, Display, TEXT("[PadLabels] ===== the range's pad labels, read from the spawn line ====="));
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float /*Delta*/) -> bool
		{
			return Tick(*Run);
		}), 0.f);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdPadLabels(
		TEXT("Trace.Practice.PadLabels"),
		TEXT("Stands the player on the range's spawn line, aims at each pad and checks what is on screen: ")
		TEXT("the label is drawn where they look, reads left to right (world-space text is projected both ")
		TEXT("ends to catch a mirrored one), is the pad's own short upper-case line, the loadout pad does ")
		TEXT("not say CHARACTER, and INFINITE ABILITIES ON shows on the amber-ringed plate."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Start));
}

#endif // !UE_BUILD_SHIPPING
