// Trace — `Trace.UI.VerifyMenu`, the proof that the two title-screen renderers are interchangeable.
//
// WHY A MEASUREMENT AND NOT A SCREENSHOT
// A screenshot answers "does it look right", which matters and which spec v17 §4 also demands. It
// does NOT answer the question that decides whether this migration is safe: does the player's MOUSE
// still land on the row they are pointing at? Hover, the press-arms/release-fires contract of spec
// v15 §4 and the -TraceMenuClickTest harness all run off ATraceMenuHUD::RowRects, and on the widget
// path those rectangles come out of Slate's layout instead of out of the Canvas layout maths.
//
// So this command measures exactly that: for every row, the rectangle Slate actually laid the widget
// out at, against the rectangle the Canvas renderer WOULD have produced this same frame, in the same
// viewport-pixel space APlayerController::GetMousePosition works in. If those agree, the two
// renderers are the same screen as far as every input path in this project is concerned.
//
// IT GOES RED ON PURPOSE. `Trace.UI.VerifyMenu redarm` compares each row against its NEIGHBOUR's
// rectangle instead of its own. A check that cannot fail is not evidence, and the row pitch (71
// reference pixels) is exactly the kind of error this is meant to catch.

#include "CoreMinimal.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#include "Trace.h"                                    // LogTraceGame
#include "UI/TraceMenuHUD.h"
#include "UI/Widgets/Menu/TraceMenuPalette.h"

#if !UE_BUILD_SHIPPING

// Named after the file, not anonymous — Scripts/check-jumbo-build-collisions.py gates on it, and
// two anonymous namespaces merged into one unity translation unit is MSVC C2084 on Windows only.
namespace TraceMenuUIVerifyFile
{
	/**
	 * Rectangles this far apart or closer are the same rectangle for a mouse.
	 *
	 * Two pixels, and the number is chosen rather than guessed: the smallest thing on this screen a
	 * click has to discriminate is the 11-pixel gap between two rows, so an error that mattered would
	 * have to be five times this before it could put a click on the wrong row. Slate rounds layout to
	 * whole pixels and the Canvas path does not, so sub-pixel disagreement is expected and harmless.
	 */
	static constexpr float TolerancePixels = 2.0f;

	static ATraceMenuHUD* FindMenuHUD()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
			{
				continue;
			}
			UWorld* World = Context.World();
			if (World == nullptr)
			{
				continue;
			}
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				if (APlayerController* PC = It->Get())
				{
					if (ATraceMenuHUD* MenuHUD = Cast<ATraceMenuHUD>(PC->GetHUD()))
					{
						return MenuHUD;
					}
				}
			}
		}
		return nullptr;
	}

	static void VerifyMenu(const TArray<FString>& Args)
	{
		const bool bRedArm = Args.ContainsByPredicate([](const FString& Arg)
		{
			return Arg.Equals(TEXT("redarm"), ESearchCase::IgnoreCase);
		});

		UE_LOG(LogTraceGame, Display, TEXT("================ Trace.UI.VerifyMenu ================"));

		ATraceMenuHUD* MenuHUD = FindMenuHUD();
		if (MenuHUD == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[MenuUI] No title screen is up. This command only means anything on /Game/Maps/MainMenu."));
			UE_LOG(LogTraceGame, Display, TEXT("[MenuUI] VERDICT: NOT APPLICABLE (no title screen)."));
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[MenuUI] Adoption: %s"), *MenuHUD->GetMenuUmgStatus());
		UE_LOG(LogTraceGame, Display, TEXT("[MenuUI] Renderer drawing right now: %s"),
			MenuHUD->IsMenuUmgActive() ? TEXT("UMG widget") : TEXT("Canvas"));

		// The two paths agree on where things go only because UMG's DPI scale happens to be
		// (shortest side / 1080) under the engine's default curve, which is the same number
		// ATraceMenuHUD::UIScale is. Nothing in this project SETS that curve, so it is worth saying
		// out loud rather than assuming: if somebody edits Project Settings > User Interface > DPI
		// Scaling, the widget moves and the Canvas path does not.
		const float DpiScale = UWidgetLayoutLibrary::GetViewportScale(MenuHUD);
		UE_LOG(LogTraceGame, Display,
			TEXT("[MenuUI] UMG DPI scale = %.4f. The Canvas path's own UIScale is clamp(height/1080, 0.5, 2). ")
			TEXT("These must match or the widget lands somewhere the Canvas maths would not."), DpiScale);

		if (!MenuHUD->IsMenuUmgActive())
		{
			// An honest verdict, not a failure. Canvas is a supported, shipped configuration — it is
			// what draws when Trace.UI.UseUMG is 0, when the asset is missing, and on every frame the
			// settings overlay or the JOIN prompt is up.
			UE_LOG(LogTraceGame, Display,
				TEXT("[MenuUI] VERDICT: CANVAS PATH — supported configuration. Nothing to compare; the ")
				TEXT("widget is not drawing. Set Trace.UI.UseUMG 1 (and generate the asset) to compare."));
			UE_LOG(LogTraceGame, Display, TEXT("====================================================="));
			return;
		}

		const int32 RowCount = static_cast<int32>(ETraceMenuRow::Count);
		float WorstError = 0.f;
		int32 WorstRow = INDEX_NONE;
		int32 Compared = 0;
		int32 Missing = 0;

		for (int32 Index = 0; Index < RowCount; ++Index)
		{
			FBox2D UmgRect(ForceInit);
			FBox2D CanvasRect(ForceInit);

			// The red arm compares each row against the NEXT row's Canvas rectangle. One row pitch is
			// 71 reference pixels, so a working comparison must report roughly that and fail.
			const int32 CanvasIndex = bRedArm ? ((Index + 1) % RowCount) : Index;

			if (!MenuHUD->GetUmgRowRect(Index, UmgRect) || !MenuHUD->GetCanvasRowRect(CanvasIndex, CanvasRect))
			{
				++Missing;
				UE_LOG(LogTraceGame, Warning, TEXT("[MenuUI]   row %d: no rectangle yet (not laid out)."), Index);
				continue;
			}

			const float MinError = static_cast<float>(FVector2D::Distance(UmgRect.Min, CanvasRect.Min));
			const float MaxError = static_cast<float>(FVector2D::Distance(UmgRect.Max, CanvasRect.Max));
			const float RowError = FMath::Max(MinError, MaxError);
			++Compared;

			if (RowError > WorstError)
			{
				WorstError = RowError;
				WorstRow = Index;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[MenuUI]   row %d  UMG (%.1f, %.1f)-(%.1f, %.1f)  Canvas (%.1f, %.1f)-(%.1f, %.1f)  worst corner %.2f px"),
				Index,
				UmgRect.Min.X, UmgRect.Min.Y, UmgRect.Max.X, UmgRect.Max.Y,
				CanvasRect.Min.X, CanvasRect.Min.Y, CanvasRect.Max.X, CanvasRect.Max.Y,
				RowError);
		}

		if (Compared == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[MenuUI] VERDICT: INCONCLUSIVE — %d row(s) had no layout yet. Run it a second later."), Missing);
			UE_LOG(LogTraceGame, Display, TEXT("====================================================="));
			return;
		}

		const bool bPass = (WorstError <= TolerancePixels) && (Missing == 0);

		if (bRedArm)
		{
			// The red arm's PASS condition is inverted on purpose: the point is to demonstrate that the
			// comparison above is capable of failing at all.
			if (bPass)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[MenuUI] RED ARM VERDICT: BROKEN HARNESS — comparing every row against its ")
					TEXT("NEIGHBOUR still passed (worst %.2f px). This check cannot detect a real ")
					TEXT("misalignment and must not be trusted."), WorstError);
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[MenuUI] RED ARM VERDICT: GOOD — off-by-one-row was caught, worst %.2f px on row %d ")
					TEXT("(the row pitch is %.0f reference px). The real check is meaningful."),
					WorstError, WorstRow, TraceMenuStyle::RowSpacing);
			}
			UE_LOG(LogTraceGame, Display, TEXT("====================================================="));
			return;
		}

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[MenuUI] VERDICT: PASS — %d of %d rows, worst corner error %.2f px (tolerance %.1f). ")
				TEXT("The widget's rows are where the Canvas renderer would have put them, so hover, ")
				TEXT("clicking and -TraceMenuClickTest all behave identically on both paths."),
				Compared, RowCount, WorstError, TolerancePixels);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[MenuUI] VERDICT: FAIL — worst corner error %.2f px on row %d (tolerance %.1f), ")
				TEXT("%d row(s) with no layout. The widget and the Canvas maths disagree about where a ")
				TEXT("row is; clicks may land on the wrong row. Set Trace.UI.UseUMG 0 and re-run ")
				TEXT("Scripts/generate-menu-widgets.py."),
				WorstError, WorstRow, TolerancePixels, Missing);
		}

		UE_LOG(LogTraceGame, Display, TEXT("====================================================="));
	}

	static FAutoConsoleCommand CmdVerifyMenu(
		TEXT("Trace.UI.VerifyMenu"),
		TEXT("Measures the UMG title screen's row rectangles against the Canvas layout maths, in the ")
		TEXT("viewport pixel space the mouse works in. Pass 'redarm' to compare each row against its ")
		TEXT("neighbour instead, which must FAIL — that is what proves the check can detect anything."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyMenu));
}

#endif // !UE_BUILD_SHIPPING
