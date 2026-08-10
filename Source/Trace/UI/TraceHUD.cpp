// Trace — Canvas HUD implementation.

#include "UI/TraceHUD.h"

#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "EngineUtils.h"                  // TActorIterator, for the replicated-character count
#include "HAL/IConsoleManager.h"          // FAutoConsoleVariableRef, for the v9 §11 A/B arm
#include "GameFramework/GameStateBase.h"   // AGameStateBase::PlayerArray
#include "GameFramework/PlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceMelee.h"          // v10 §1 — the equipped-weapon row and its two timers
#include "Gameplay/TraceParry.h"          // v6 §3 — the parry-kill banner and the death-panel line
#include "Gameplay/TraceWeaponComponent.h" // v16 §1/§2 — the ammo block reads the clip through this
// v16 §2 — the status stack. Six statuses live on four different owners, so the corner has to ask
// four different objects; every one of these is a READ of an accessor that slice already published
// for the HUD, and nothing here writes gameplay state.
#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Abilities/Characters/TraceAbilitySetChut.h"   // Chud
#include "Abilities/Characters/TraceAbilitySetMace.h"   // suspend / Spike pull
#include "Abilities/Characters/TraceAbilitySetRocco.h"  // the headshot speed stack
#include "Abilities/Characters/TraceAbilitySetX.h"      // v16 §1 — the bee clip's loaded size
#include "Abilities/Characters/TraceOysterPoison.h"     // poisoned + slowed
#include "Containers/Ticker.h"                          // FTSTicker — the v16 §2 shot sequence
#include "HAL/PlatformFileManager.h"                    // the screenshot directory
#include "Movement/TraceCharacterMovementComponent.h"
#include "Core/TraceCharacterRoster.h"    // v14 §3 — the accent colour and the ability's name
#include "Settings/TraceUserSettings.h"   // v14 §5 — the ability's bound key, if the input slice has one
#include "InputCoreTypes.h"               // EKeys::Escape, for the pause poll
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"                 // FScreenshotRequest
#include "Kismet/GameplayStatics.h"       // OpenLevel, for RETURN TO TITLE
#include "Kismet/KismetSystemLibrary.h"   // QuitGame
#include "Trace.h"                        // LogTraceGame
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "UI/TraceAutoShot.h"
#include "UI/TraceMatchOptions.h"         // TraceMatchFlow::PostMatchDuration, TraceMaps
#include "UI/TraceNetworking.h"           // TraceNet — host address, connection state, failures
// v17 §4 (step 4b) — the bottom-right corner's second presenter. UMG is linked by Trace.Build.cs,
// which retired "contract 7: Canvas only" in this same pass; nothing else on this HUD uses it.
#include "Blueprint/UserWidget.h"         // CreateWidget
#include "Blueprint/WidgetLayoutLibrary.h" // GetViewportScale — the DPI half of the design scale
#include "UI/Widgets/HUD/TraceHudCornerWidget.h"
#include "UI/Widgets/HUD/TraceHudStatusChipWidget.h" // Trace.HUD.Corner.Verify probes the chip too

namespace
{
	/**
	 * A/B ARM FOR THE DEFERRED-WHISTLE INDICATOR (spec v9 §11, HUD half).
	 *
	 * 1 (default): the footer under the clock is replaced by "HALF/MATCH ENDS AT NEXT DEAD BALL"
	 * while the whistle is armed. 0 restores the pre-v9 footer — the half label and nothing else —
	 * which is what a player saw before this change: a clock frozen at 0:00, "1ST HALF" underneath
	 * it, and no way to tell a deferred whistle from a hung timer.
	 *
	 * It exists so the RED case can be photographed in the SAME binary as the green one. Spec v9 §0
	 * is about exactly that: a harness that never went red is not evidence. Cheat-only, no ini
	 * override, so a shipping player can never turn the indicator off.
	 */
	int32 GHudPendingPeriodEndLabel = 1;

#if !UE_BUILD_SHIPPING
	FAutoConsoleVariableRef CVarHudPendingPeriodEndLabel(
		TEXT("Trace.HUD.PendingPeriodEndLabel"),
		GHudPendingPeriodEndLabel,
		TEXT("1 (default, spec v9 11): while the clock has expired and play continues, the panel "
		     "footer reads 'HALF/MATCH ENDS AT NEXT DEAD BALL' and the hard-cap countdown sits in "
		     "the panel's right gutter. 0 draws the pre-v9 footer for A/B capture ONLY - a 0:00 "
		     "clock with no explanation reads as a hung timer."),
		ECVF_Cheat);
#endif
}

/**
 * SPEC v16 §2's A/B ARM, and the whole of this pass's red-arm discipline.
 *
 * Named after the file rather than anonymous, per the build contract: this .cpp already carries one
 * unnamed namespace and a second set of symbols in it is how the Windows jumbo build breaks.
 *
 * 1 (default) is the shipped v16 HUD: ammo and the status stack in the bottom-right corner, and the
 * throw charge drawn as the crosshair ring. 0 restores EXACTLY what a player saw before this pass —
 * no ammo anywhere, no statuses anywhere, and the throw charge as a bar in the bottom-left stack.
 *
 * *** IT EXISTS SO THE RED CASE CAN BE PHOTOGRAPHED IN THE SAME BINARY, AGAINST THE SAME FIXTURE,
 * AS THE GREEN ONE. *** Spec v16's testing rule is that a harness which cannot go red is not
 * evidence, and for a drawing change the only evidence is a frame. With the arm, Trace.HUD.V16Shots
 * stages one set of real gameplay states and photographs both HUDs off it seconds apart — so a
 * "before" screenshot cannot be a different build, a different fixture or a different camera.
 *
 * Cheat-only and no ini override, exactly like Trace.HUD.PendingPeriodEndLabel above: a shipping
 * player must not be able to switch their own ammo counter off.
 */
namespace TraceHUDV16
{
	static int32 GArmed = 1;

#if !UE_BUILD_SHIPPING
	static FAutoConsoleVariableRef CVarArmed(
		TEXT("Trace.HUD.V16"),
		GArmed,
		TEXT("1 (default, spec v16 2): ammo + the status stack in the bottom-right corner, and the "
		     "throw charge on the crosshair ring. 0 is the RED ARM - the pre-v16 HUD, with no ammo, "
		     "no statuses and the throw charge back on a bottom-left bar. Capture ONLY; it is the "
		     "before-and-after pair, not a preference."),
		ECVF_Cheat);
#endif

	static bool IsArmed() { return GArmed != 0; }
}

namespace TraceHUDStyle
{
	/** Everything in this file is authored against a 1080p-tall viewport and scaled from there. */
	static constexpr float ReferenceHeight = 1080.f;

	// Palette. Deliberately desaturated so the two team colours are the only saturated things on
	// screen — that is what makes "who has the Core" readable at a glance.
	static const FLinearColor Ink        (0.95f, 0.96f, 1.00f, 1.00f);
	static const FLinearColor InkDim     (0.68f, 0.72f, 0.78f, 1.00f);
	static const FLinearColor PanelFill  (0.02f, 0.03f, 0.05f, 0.72f);
	static const FLinearColor PanelBorder(0.55f, 0.62f, 0.72f, 0.35f);
	static const FLinearColor Trough     (0.06f, 0.07f, 0.09f, 0.85f);
	static const FLinearColor Shadow     (0.00f, 0.00f, 0.00f, 0.55f);
	static const FLinearColor Danger     (0.95f, 0.22f, 0.18f, 1.00f);
	static const FLinearColor Good       (0.24f, 0.90f, 0.42f, 1.00f);
	/** Headshot hitmarker. Hot amber: distinct from both the white body tick and the red kill tick. */
	static const FLinearColor Warning    (1.00f, 0.78f, 0.20f, 1.00f);

	// Shared geometry for the top-centre score panel. The Core banner hangs directly off the
	// bottom of it, so both passes read these rather than repeating literals.
	static constexpr float TopPanelY = 14.f;
	static constexpr float TopPanelW = 520.f;
	static constexpr float TopPanelH = 92.f;
	static constexpr float BannerGap = 10.f;

	/** Hit-marker fade-out windows, in seconds. */
	static constexpr float HitMarkerDuration = 0.35f;
	static constexpr float KillMarkerDuration = 0.60f;

	/** Match clock turns red inside this many seconds. */
	static constexpr float ClockUrgentSeconds = 30.f;

	/**
	 * How many points short of UTraceSettings::MercyRuleLead the mercy warning appears.
	 *
	 * Two. The rule can end a match in the middle of the first half (spec v4 §6), and a match that
	 * simply stops with no warning is indistinguishable from the "keeps stopping and restarting"
	 * complaint this HUD has spent two passes answering. Any earlier than two points and the warning
	 * is on screen for most of a lopsided match, at which point it stops being a warning.
	 */
	static constexpr int32 MercyWarningPoints = 2;

	/**
	 * Where the third-person pass reticle is anchored along the pass ray when there is no receiver
	 * under it, in world units. See DrawPassReticle(): the reticle marks a POINT on that ray, and a
	 * ray drawn from an origin the camera is not sitting at has no single screen position — so one
	 * has to be picked. ~16 m is a typical in-arena pass; the anchor slides to the real distance the
	 * moment an actual receiver is acquired.
	 */
	static constexpr float PassAnchorDefaultDistance = 1600.f;

	/**
	 * How much bigger the centre crosshair is drawn in third person than in first.
	 *
	 * Not a style flourish and not an arbitrary number. In third person the crosshair stops being the
	 * gun's aim point and becomes the pointer a pass is aimed with, over a frame that now contains
	 * the player's own body, their trail and a Core — and the previous build's crosshair was reported
	 * MISSING there. At 1280x720 (UIScale 0.667) 1.0 gives a 22 px cross; 1.6 gives 37 px with 3 px
	 * bars, which is unmistakable in a screenshot and still nowhere near obscuring a receiver.
	 *
	 * PROMOTED TO UTraceSettings::ThirdPersonCrosshairScale (Category "HUD") — this is the fallback
	 * used only if the settings object is somehow unavailable, and it must stay equal to the shipped
	 * default there. Read it through ThirdPersonCrosshairScaleSetting() below, never directly.
	 */
	static constexpr float ThirdPersonCrosshairScale = 1.6f;

	/** Ease rates for the pass reticle: anchor distance (uu/s-ish) and the lock-on close. */
	static constexpr float PassAnchorInterpSpeed = 6.f;
	static constexpr float PassLockInterpSpeed = 14.f;

	/**
	 * How often the "who would receive a pass" probe may run, in seconds.
	 *
	 * ATraceCore::FindPassTargetFor() line-traces once per candidate. That is cheap, but it is not
	 * free and nothing about the answer changes meaningfully inside 50 ms — and it is only ever run
	 * on the one machine whose local player is currently holding the Core.
	 */
	static constexpr float PassTargetPollInterval = 0.05f;

	/** How long "BLUE SCORES" stays up after a capture, and "GO" after the whistle. */
	static constexpr float ScoreFlashDuration = 2.4f;
	/** How long the "TEAM WIPE +2" flash stays up, in seconds. */
	static constexpr float WipeBonusDuration = 3.0f;

	static constexpr float GoBannerDuration = 1.4f;

	/**
	 * "Close enough to zero" for cooldown comparisons. A literal rather than KINDA_SMALL_NUMBER:
	 * the legacy math macros were re-spelled UE_KINDA_SMALL_NUMBER during the 5.x line and the
	 * contract asks us to avoid anything deprecated along the way.
	 */
	static constexpr float TimeEpsilon = 1.e-4f;

	/**
	 * Component-wise colour lerp.
	 *
	 * Written out rather than using FMath::Lerp<FLinearColor>: that template expands to
	 * `A + Alpha * (B - A)`, i.e. it needs `float * FLinearColor`, and FLinearColor only provides
	 * the member `FLinearColor * float`. Use this everywhere instead — every colour blend in this
	 * file goes through it.
	 */
	static FLinearColor LerpColor(const FLinearColor& A, const FLinearColor& B, float Alpha)
	{
		const float T = FMath::Clamp(Alpha, 0.f, 1.f);
		return FLinearColor(
			FMath::Lerp(A.R, B.R, T),
			FMath::Lerp(A.G, B.G, T),
			FMath::Lerp(A.B, B.B, T),
			FMath::Lerp(A.A, B.A, T));
	}

	/** Scales RGB and lifts it toward white. Used to make a "ready" indicator pop. */
	static FLinearColor Shade(const FLinearColor& C, float Multiply, float Lift)
	{
		return FLinearColor(C.R * Multiply + Lift, C.G * Multiply + Lift, C.B * Multiply + Lift, C.A);
	}

	/** Same colour, new alpha. */
	static FLinearColor WithAlpha(const FLinearColor& C, float Alpha)
	{
		return FLinearColor(C.R, C.G, C.B, Alpha);
	}

	/**
	 * The live third-person crosshair scale, from Project Settings, clamped to the panel's own range.
	 *
	 * The ONE reader of UTraceSettings::ThirdPersonCrosshairScale. Going through here rather than
	 * touching the setting at each use site is what makes it live-editable in PIE with no caching
	 * seam, and the clamp keeps a hand-edited ini from producing a crosshair that covers the receiver
	 * the player is trying to aim at.
	 */
	static float ThirdPersonCrosshairScaleSetting()
	{
		return FMath::Clamp(UTraceSettings::Get().ThirdPersonCrosshairScale, 1.f, 3.f);
	}
}

void ATraceHUD::BeginPlay()
{
	Super::BeginPlay();

	// FIRST-RUN ART CHECK, before a single character has spawned. Cheap (a package-store lookup, no
	// load) and idempotent, and it is what puts DrawArtWarning() on screen during warm-up rather than
	// after the first pawn happens to be dressed. See ETraceCharacterArtStatus: a silent degrade to
	// capsules is what produced the "the character models were not replaced" bug report.
	ATraceCharacter::VerifyCharacterArtInstalled();

	// Idempotent; the title screen usually got there first. Bound again here because a client can
	// arrive in this map without ever having seen the menu (Scripts/run-client.sh, or `open <ip>`
	// from the console), and a mid-match disconnect has to be reported on those paths too.
	TraceNet::BindFailureHandlers();

	// One Display line naming the net mode, the endpoint and every local adapter. This is the line
	// anyone debugging a failed playtest will be asked for first, so it is emitted unconditionally
	// rather than behind a cvar nobody sets before playing.
	TraceNet::LogNetworkDiagnostics(GetWorld(), TEXT("Match HUD ready"));

	// Spec v14 §3. Bound once, here: the select screen opens itself off replicated state, so if the
	// callbacks were wired lazily at the moment it opened there would be a window in which the screen
	// was up and gameplay input was still live — the player would be walking around behind it.
	WireCharacterSelect();

#if !UE_BUILD_SHIPPING
	TraceAutoShot::Arm(this, TEXT("Match"));
	// -TraceExec="a|b" -TraceExecAt=<sec>: run the in-match verification commands headlessly. See
	// TraceAutoShot.h - the engine's own -ExecCmds fires on the title screen, where they are no-ops.
	TraceAutoShot::ArmDeferredExec(this, TEXT("Match"));

	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceAutoPause="), AutoPauseAtSeconds))
	{
		AutoPauseAtSeconds = -1.f;
	}
#endif
}

void ATraceHUD::DrawHUD()
{
	Super::DrawHUD();

	UWorld* World = GetWorld();
	if (Canvas == nullptr || World == nullptr || GEngine == nullptr)
	{
		return;
	}

	ViewW = static_cast<float>(Canvas->SizeX);
	ViewH = static_cast<float>(Canvas->SizeY);
	if (ViewW <= 0.f || ViewH <= 0.f)
	{
		return;
	}

	UIScale = FMath::Clamp(ViewH / TraceHUDStyle::ReferenceHeight, 0.6f, 2.0f);
	Now = World->GetTimeSeconds();

	FontSmall  = GEngine->GetSmallFont();
	FontMedium = GEngine->GetMediumFont();
	FontLarge  = GEngine->GetLargeFont();

	TracePC   = Cast<ATracePlayerController>(PlayerOwner.Get());
	LocalChar = (TracePC != nullptr) ? TracePC->GetTraceCharacter() : nullptr;
	LocalPS   = (TracePC != nullptr) ? TracePC->GetTracePlayerState() : nullptr;
	TraceGS   = World->GetGameState<ATraceGameState>();

	// The player state survives death, the pawn does not — so prefer the player state for team.
	LocalTeam = (LocalPS != nullptr)
		? LocalPS->Team
		: ((LocalChar != nullptr) ? LocalChar->GetTeam() : ETraceTeam::None);

	bLocalAlive    = (LocalChar != nullptr) && LocalChar->IsAlive();
	bLocalCarrying = bLocalAlive && LocalChar->IsCarrier();

	// WHICH OF THE TWO GAMES THIS IS (spec v4 §7), resolved once a frame from the replicated
	// GameState rather than per pass. Every prompt below has to follow it — mode B changes what LMB
	// does while carrying, and a HUD that keeps saying "HOLD TO PASS" in a mode where the button
	// throws is actively teaching the wrong control.
	//
	// The GameState, not ATraceCore::IsModeB: the Core is null before it has replicated, and the
	// mode chip has to be right on the first drawn frame of the warm-up.
	bGoalMode = (TraceGS != nullptr) && TraceGS->IsGoalMode();

	// Death edge detection, purely local — see the note on LocalDeathTime in the header.
	if (bLocalAlive)
	{
		bHasSpawnedOnce = true;
	}
	bLocalDead = bHasSpawnedOnce && !bLocalAlive;
	if (bLocalDead && !bWasDeadLastDraw)
	{
		LocalDeathTime = Now;
	}
	bWasDeadLastDraw = bLocalDead;

	// ---- Phase / capture edge detection --------------------------------------------------------
	if (TraceGS != nullptr)
	{
		const int32 BlueNow = TraceGS->GetScore(ETraceTeam::Blue);
		const int32 OrangeNow = TraceGS->GetScore(ETraceTeam::Orange);

		// The first draw after joining establishes the baseline. Without that guard a late joiner
		// would be greeted by "BLUE SCORES" for every goal scored before they arrived.
		if (bScoreCacheValid)
		{
			if (BlueNow > LastSeenBlueScore)
			{
				ScoreFlashTeam = ETraceTeam::Blue;
				ScoreFlashTime = Now;
			}
			else if (OrangeNow > LastSeenOrangeScore)
			{
				ScoreFlashTeam = ETraceTeam::Orange;
				ScoreFlashTime = Now;
			}
		}
		LastSeenBlueScore = BlueNow;
		LastSeenOrangeScore = OrangeNow;
		bScoreCacheValid = true;

		const ETraceMatchState StateNow = TraceGS->TraceMatchState;
		if (bMatchStateCacheValid && StateNow != LastSeenMatchState && StateNow == ETraceMatchState::InProgress)
		{
			MatchStartTime = Now;
		}
		LastSeenMatchState = StateNow;
		bMatchStateCacheValid = true;
	}

	LogAffordanceAvailabilityOnce();

	// Spec v17 §4 (step 4b). Cleared here and set by DrawAmmoAndStatuses; read at the bottom of this
	// function. A UMG widget keeps painting until something tells it not to, and there are three
	// perfectly ordinary frames on which the corner pass does not run at all — post-match, and the
	// two frames either side of a travel.
	bCornerAddressedThisDraw = false;

#if !UE_BUILD_SHIPPING
	// Spec v16 §2's draw record, cleared before anything draws. A stale record read back as a live
	// one is precisely how a harness ends up reporting PASS while striking a corpse.
	//
	// It is cleared for BOTH corner presenters and written by whichever one runs, so the record keeps
	// meaning "what the corner actually put on screen" after spec v17 §4 rather than "what the Canvas
	// pass did". That is what lets Trace.HUD.V16Shots verify the UMG corner without being rewritten.
	bDrewAmmoBlock = false;
	DrawnAmmoText.Reset();
	bDrewBeeClip = false;
	bDrewReloadBar = false;
	DrawnMagazineTicks = 0;
	DrawnStatusChips.Reset();
	bDrewChargeRing = false;
	DrawnChargeRingAlpha = -1.f;
	DrawnChargeRingSegments = 0;
	bDrewChargeBar = false;
#endif

	// Escape raises the pause menu. Polled rather than bound for the same reason the overlay itself
	// polls: the menu pauses the world in standalone, and a bound delegate only survives that if
	// every binding remembered bExecuteWhenPaused. FTraceOptionsMenu::OpenRoot swallows input for the
	// remainder of this frame, so the very Escape that opened it cannot also close it.
	if (TracePC != nullptr && !PauseMenu.IsOpen() && TracePC->WasInputKeyJustPressed(EKeys::Escape))
	{
		OpenPauseMenu();
	}

#if !UE_BUILD_SHIPPING
	// Measured from the first drawn frame rather than from BeginPlay: the match spends its opening
	// seconds in warm-up and the controller may not have a pawn yet, and a pause menu raised before
	// TracePC resolves would silently do nothing.
	if (FirstDrawTime < 0.f && TracePC != nullptr)
	{
		FirstDrawTime = Now;
	}
	if (AutoPauseAtSeconds >= 0.f && !bAutoPauseFired && FirstDrawTime >= 0.f
		&& (Now - FirstDrawTime) >= AutoPauseAtSeconds && !PauseMenu.IsOpen())
	{
		bAutoPauseFired = true;
		UE_LOG(LogTraceGame, Display, TEXT("[AutoPause] Raising the pause menu."));
		OpenPauseMenu();

		// ~20 drawn frames later, so the panel is definitely on screen and settled.
		AutoPauseShotAtDraw = DrawCount + 20;
	}

	++DrawCount;
	if (AutoPauseShotAtDraw >= 0 && DrawCount >= AutoPauseShotAtDraw)
	{
		AutoPauseShotAtDraw = -1;

		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"),
			FString::Printf(TEXT("TraceAutoShot_Pause_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));

		UE_LOG(LogTraceGame, Display, TEXT("[AutoPause] Screenshot requested: %s"), *Path);
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
	}
#endif

	// Once the whistle has gone the live chrome is noise: a crosshair you cannot shoot with, a
	// clock that has stopped, a Core banner nobody can act on. DrawMatchResult takes the screen.
	const bool bPostMatch = (TraceGS != nullptr) && (TraceGS->TraceMatchState == ETraceMatchState::PostMatch);
	if (!bPostMatch)
	{
		// Before any pass draws: the crosshair and the pass ring have to sit on the same pixel, and
		// the pass-target probe has to run exactly once per frame.
		UpdateReticleAnchor();

		DrawCrosshair();
		DrawPassProgress();

		// Spec v16 §2 — the throw charge, on the ring the pass hold has always used. Immediately
		// after the pass so the two are unmistakably the same element drawn from two sources.
		DrawThrowChargeRing();

		DrawHitMarker();
		DrawHealthAndDash();

		// Spec v16 §2 — the bottom-right corner. After the bottom-left stack purely so the two
		// corners are read in the same order in this function as they are on screen.
		DrawAmmoAndStatuses();

		DrawScoresAndClock();
		DrawCoreBanner();
		DrawPhaseBanner();
		DrawScoreFlash();
		DrawParryKillBanner();
		DrawDeathPanel();
		DrawScoreboard();
	}

	DrawMatchResult();

	// Outside the bPostMatch gate on purpose: a broken install is broken on the result screen too,
	// and this is the one message that must not be possible to wait out.
	DrawArtWarning();

	// Same reasoning: who is hosting, and whether the connection just broke, are true in every phase
	// of the match including the full-time screen.
	//
	// KillFeedTopY is reset here and republished by DrawNetworkStatus below, so the feed hangs off
	// whatever height that panel actually took this frame rather than off a guessed clearance.
	KillFeedTopY = TraceHUDStyle::TopPanelY * UIScale;
	DrawNetworkStatus();
	DrawNetworkFailureBanner();

	// After the network panel, and outside the bPostMatch gate: the last few kills are still worth
	// reading on the full-time screen, and they are the only record of how a half ended.
	DrawKillFeed();

	// Spec v14 §3 — the character select screen, over the match and under the pause menu.
	//
	// UNDER the pause menu, and it keeps DRAWING while the pause menu is up rather than hiding: a
	// select screen that vanished when the player pressed Escape would read as the pick having been
	// cancelled, and the auto-pick clock is still running underneath. It stops taking input instead,
	// which is what bInputAllowed says.
	//
	// Outside the bPostMatch gate for a duller reason: the screen is closed by then anyway (the server
	// closes it the moment a character is held), and a gate here would be a second condition able to
	// disagree with the replicated one.
	CharacterSelect.Tick(this, TracePC.Get(), LocalPS.Get(), ViewW, ViewH, UIScale, Now,
		/*bInputAllowed=*/!PauseMenu.IsOpen());

	// Last, over everything including the full-time takeover. A no-op while closed.
	PauseMenu.Tick(this, TracePC.Get(), ViewW, ViewH, UIScale, Now);

	// SPEC v17 §4 (step 4b). The Canvas corner simply is not drawn on a frame where its pass does not
	// run; a UMG corner has to be TOLD, or the last ammo count of the match hangs over the full-time
	// screen forever. This is the "draws nothing, or draws twice" failure the spec names, closed at
	// the one place that knows the whole frame is over.
	if (!bCornerAddressedThisDraw)
	{
		HideCornerWidget();
	}
}

void ATraceHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The corner is added to the PLAYER's screen, not to this actor, so it outlives the HUD unless it
	// is taken down by hand — and a travel builds a new HUD, which would then add a second one over
	// the first. Measured elsewhere in this project as "the menu appeared twice"; not a theory.
	if (CornerWidget != nullptr)
	{
		CornerWidget->RemoveFromParent();
		CornerWidget = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void ATraceHUD::WireCharacterSelect()
{
	TWeakObjectPtr<ATraceHUD> WeakThis(this);

	// Nearly the contract the pause menu has: gameplay input goes quiet and the mouse comes back
	// while the overlay owns the screen, and is restored from ONE place however the overlay closed —
	// a pick, a timeout, or the server closing it because the toggle was switched off.
	//
	// *** WITH ONE DELIBERATE DIFFERENCE: THIS SCREEN MUST NOT PAUSE THE WORLD. ***
	//
	// MEASURED, not theorised. The first headless run of this feature logged
	//     [TracePlayerController_0] Gameplay input suppressed (menu open). netMode=2 paused=1
	// and then the log went completely silent for the rest of the run. SetGameInputSuppressed pauses
	// whenever it can do so without affecting anybody else — which is exactly the solo-with-bots case
	// every playtest of this build actually runs — and a paused world stops every FTimerManager timer
	// with it. That takes out, in order:
	//
	//   * ATraceGameMode::PollCharacterSelect, so the AUTO-PICK TIMEOUT NEVER FIRES. Spec v14 §3's
	//     [ASSUMPTION] exists precisely "so one idle player cannot stall the match", and a paused
	//     select screen is a match stalled forever by one idle player — the opposite of the rule.
	//   * GetServerWorldTimeSeconds, so the countdown this screen draws freezes at its opening value
	//     while claiming to be counting down.
	//   * the warm-up clock, the bots, and every verification hook (which is how the run announced it).
	//
	// So the pause is undone on the same frame it is taken. SetPause(false) on an unpaused world is a
	// documented no-op (see the else branch of SetGameInputSuppressed), and on a remote client the
	// pause was never taken in the first place — so this is correct on every machine and costs
	// nothing on the ones that did not need it.
	CharacterSelect.OnOpened = [WeakThis]()
	{
		if (const ATraceHUD* Strong = WeakThis.Get())
		{
			if (ATracePlayerController* PC = Strong->TracePC.Get())
			{
				PC->SetGameInputSuppressed(true);
				PC->SetPause(false);
			}
		}
	};

	CharacterSelect.OnClosed = [WeakThis]()
	{
		if (const ATraceHUD* Strong = WeakThis.Get())
		{
			if (ATracePlayerController* PC = Strong->TracePC.Get())
			{
				// Only if the pause menu is not ALSO holding input down. Restoring unconditionally
				// here would hand movement back to a player staring at a pause menu — the exact class
				// of bug the pause menu's own single-exit-path comment is about.
				if (!Strong->PauseMenu.IsOpen())
				{
					PC->SetGameInputSuppressed(false);
				}
			}
		}
	};
}

// -------------------------------------------------------------------------------------------
// Pause menu
// -------------------------------------------------------------------------------------------

void ATraceHUD::OpenPauseMenu()
{
	if (TracePC == nullptr)
	{
		return;
	}

	// Gameplay input goes quiet and the mouse comes back BEFORE the overlay draws, so the first
	// frame the panel is on screen is already a frame the player can point at it.
	TracePC->SetGameInputSuppressed(true);

	// Captured weakly: the overlay outlives nothing, but these fire from inside its Tick and the HUD
	// is destroyed by a level travel that one of them can itself start.
	TWeakObjectPtr<ATraceHUD> WeakThis(this);

	PauseMenu.OnClosed = [WeakThis]()
	{
		// The ONE thing that must happen however the overlay closed — RESUME, Escape, QUIT or a
		// travel. Restoring input from each exit path separately is how a menu ends up leaving the
		// game permanently paused with a free cursor.
		if (ATraceHUD* Strong = WeakThis.Get())
		{
			if (ATracePlayerController* PC = Strong->TracePC.Get())
			{
				// ...unless the character select screen (spec v14 §3) is still up underneath. It is
				// perfectly reachable to open the pause menu over a select screen and close it again,
				// and handing movement back to a player who is still choosing a character would let
				// them walk out of the arena behind the overlay.
				if (!Strong->CharacterSelect.IsOpen())
				{
					PC->SetGameInputSuppressed(false);
				}
			}
		}
	};

	// Present but empty: OnClosed has already done the work. Its presence is what puts the RESUME row
	// on the page at all — the title screen leaves it unset and gets a settings-only panel from the
	// same class.
	PauseMenu.OnResume = []() {};

	PauseMenu.OnReturnToTitle = [WeakThis]()
	{
		if (ATraceHUD* Strong = WeakThis.Get())
		{
			UE_LOG(LogTraceGame, Log, TEXT("Pause menu: RETURN TO TITLE."));
			UGameplayStatics::OpenLevel(Strong, FName(TraceMaps::MainMenu), /*bAbsolute=*/true);
		}
	};

	PauseMenu.OnQuit = [WeakThis]()
	{
		if (ATraceHUD* Strong = WeakThis.Get())
		{
			UE_LOG(LogTraceGame, Log, TEXT("Pause menu: QUIT."));
			UKismetSystemLibrary::QuitGame(Strong, Strong->TracePC.Get(), EQuitPreference::Quit,
				/*bIgnorePlatformRestrictions=*/false);
		}
	};

	PauseMenu.OpenRoot();
}

void ATraceHUD::LogAffordanceAvailabilityOnce()
{
	if (bLoggedAffordances || TracePC == nullptr || LocalChar == nullptr)
	{
		return;
	}
	bLoggedAffordances = true;

	// These used to be compile-time SFINAE probes, back when the mechanics lived in slices that had
	// not landed. They have landed and the shim is deleted, so these are now RUNTIME answers: what
	// the HUD actually got when it asked, on this pawn, on this frame.
	//
	// Display, not Verbose, and deliberately so. Twice now this project has lost time to a mechanic
	// declared dead when its only log line was suppressed. "parry=0" in the log the player already
	// has is the whole diagnosis. Boost is no longer listed: spec §1 deleted the feature.
	float ParryRemaining = 0.f;
	float ParryTotal = 0.f;
	bool bParryActive = false;
	const bool bHasParry = LocalChar->GetParryHudState(ParryRemaining, ParryTotal, bParryActive);

	FTraceDashHudState DashState;
	const bool bHasDash = TracePC->GetDashHudState(DashState);

	// GetPassProgress() is negative when no pass is in progress, which is always true on the first
	// frame — so this reports "the accessor answered", not "a pass is happening".
	const bool bHasPass = (TracePC->GetPassProgress() >= -1.f);

	UE_LOG(LogTraceGame, Display,
		TEXT("[HUD] Affordances: dashCharges=%d (%d/%d) passProgress=%d parry=%d ")
		TEXT("(0 = the HUD asked and got nothing back; see Gameplay/TraceParry.h and ")
		TEXT("ATracePlayerController's HUD data sources)"),
		bHasDash ? 1 : 0, DashState.Charges, DashState.MaxCharges,
		bHasPass ? 1 : 0,
		bHasParry ? 1 : 0);
}

// -------------------------------------------------------------------------------------------
// Missing character art
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawArtWarning()
{
	// THE FIX FOR A BUG REPORT, not decoration. The user asked for "default unreal engine human
	// character models that have running animations, heads, and limbs" — which were already
	// implemented and already working on the machine they were implemented on. What they were looking
	// at was the FALLBACK: character art is gitignored by design (126 MB of binaries), so a clone that
	// has not run Scripts/import-mannequin.sh silently draws capsules. The only evidence was one
	// Warning line in a log nobody reads during a playtest.
	//
	// So the degrade stops being silent. This panel is up for the entire session, in every phase
	// including the post-match screen, it names the exact command, and it cannot be dismissed —
	// because the correct response to it is to run one command and relaunch, not to close it.
	//
	// It draws NOTHING in the normal case, and nothing under -TraceNoCharacterArt either: somebody
	// deliberately exercising the fallback does not need to be nagged, and a warning that appears
	// when nothing is wrong is a warning people learn to ignore.
	FString Headline;
	FString Detail;
	if (!ATraceCharacter::GetCharacterArtWarning(Headline, Detail))
	{
		return;
	}

	const float PanelX = 40.f * UIScale;
	const float PanelY = 18.f * UIScale;
	const float PadX = 16.f * UIScale;
	const float PadY = 10.f * UIScale;
	const float LineGap = 4.f * UIScale;

	// Headline in the MEDIUM font, not the small one. Measured at 1280x720 (UIScale 0.667) the
	// small-font headline was a 6px-tall strip in the corner — technically present, which is exactly
	// the failure mode this whole panel exists to correct. It has to be readable at a glance from
	// across a desk.
	// The headline is drawn at 1.35x the medium font. Measured at 1280x720 (UIScale 0.667) the
	// original small-font headline was a 6px-tall strip in the corner — technically present, which is
	// exactly the failure mode this panel exists to correct. The scale is capped by the space to the
	// LEFT of the top score panel (which starts at reference x=700), so the two can never overlap.
	const float HeadScale = UIScale * 1.35f;
	const float HeadlineW = MeasureWidth(Headline, FontMedium, HeadScale);
	const float DetailW = MeasureWidth(Detail, FontSmall, UIScale);
	const float HeadlineH = MeasureHeight(Headline, FontMedium, HeadScale);
	const float DetailH = MeasureHeight(Detail, FontSmall, UIScale);

	const float PanelW = FMath::Max(HeadlineW, DetailW) + (PadX * 2.f);
	const float PanelH = HeadlineH + DetailH + LineGap + (PadY * 2.f);

	// Pulsed rather than static. A steady red box at the edge of the screen becomes furniture inside
	// a minute; a slow 1 Hz pulse keeps catching the eye without ever obscuring anything, and it is
	// also the difference between "this build has a red box in the corner" and "something is wrong".
	const float Pulse = 0.5f + 0.5f * FMath::Sin(Now * UE_TWO_PI * 0.9f);

	const FLinearColor Fill = FLinearColor(0.22f, 0.02f, 0.02f, 0.86f);
	const FLinearColor Border = TraceHUDStyle::WithAlpha(TraceHUDStyle::Danger, 0.55f + 0.45f * Pulse);
	DrawPanel(PanelX, PanelY, PanelW, PanelH, Fill, Border);

	DrawTextLeft(Headline, TraceHUDStyle::LerpColor(TraceHUDStyle::Danger, TraceHUDStyle::Ink, 0.35f + 0.35f * Pulse),
		PanelX + PadX, PanelY + PadY, FontMedium, HeadScale);
	DrawTextLeft(Detail, TraceHUDStyle::Ink,
		PanelX + PadX, PanelY + PadY + HeadlineH + LineGap, FontSmall, UIScale);
}

// -------------------------------------------------------------------------------------------
// Reticle
// -------------------------------------------------------------------------------------------

void ATraceHUD::UpdateReticleAnchor()
{
	// First person: the viewport centre is not an approximation of the aim point, it IS the aim
	// point. The spring arm collapses to length 0 at GetPawnViewLocation() and looks along the
	// control rotation, which is the exact ray GetAimDirection() builds — Trace.DebugViewProbe
	// measures aimErr 0.0000 deg off that identity. So the centre is the default and every
	// first-person path below leaves it untouched.
	const float CX = FMath::RoundToFloat(ViewW * 0.5f);
	const float CY = FMath::RoundToFloat(ViewH * 0.5f);
	ReticleX = CX;
	ReticleY = CY;

	// Keyed off the CAMERA's eased blend rather than off bLocalCarrying, so everything that follows
	// the view mode moves over the same ~0.35s the camera takes rather than snapping a beat early.
	ViewBlend = (LocalChar != nullptr)
		? FMath::Clamp(LocalChar->GetViewBlendAlpha(), 0.f, 1.f)
		: (bLocalCarrying ? 1.f : 0.f);

	const float DeltaSeconds = (GetWorld() != nullptr) ? GetWorld()->GetDeltaSeconds() : 0.f;

	// GATED ON CARRYING, NOT ON THE CAMERA BLEND. This used to early-out on `ViewBlend <= 0.02f`,
	// which is a bug with a measurable cost: ViewBlend starts at 0 the instant you receive the Core
	// and takes ~0.35 s to ease in, so NO pass affordance was computed at all for the first ~20
	// frames of every single carry — precisely the frames in which a player who caught a pass is
	// looking for who to send it to next. That is one of the mechanisms behind "sometimes the pass
	// option doesn't show up" (spec §4.1).
	//
	// ViewBlend still gates the VISUALS below, which is what it is for: the third-person reticle
	// should fade in with the camera. It must never gate the query.
	if (!bLocalAlive || LocalChar == nullptr || Canvas == nullptr || (!bLocalCarrying && ViewBlend <= 0.02f))
	{
		HoveredPassTarget = nullptr;
		LastLoggedPassTarget = nullptr;   // so the next carry announces its first receiver again
		PassLockAlpha = 0.f;
		PassAnchorDistance = -1.f;
		return;
	}

	// ---- Who would receive a pass right now ----------------------------------------------------
	//
	// ATraceCore::FindPassTargetFor() is public precisely so the HUD can answer this before the
	// button is pressed (see its comment). Asking the Core rather than re-implementing the cone here
	// is the whole point: the highlight can then never disagree with the pass that actually happens.
	//
	// SKIPPED ENTIRELY IN MODE B. A throw has no receiver to acquire — it goes where the crosshair
	// points and the first player to touch it takes it, teammate or not (spec v4 §7). Running the
	// probe anyway would light a teammate up as "the person this is going to", which is not a thing
	// mode B does, and would spend a handful of line traces a frame proving it.
	ATraceCore* Core = (TraceGS != nullptr) ? TraceGS->Core : nullptr;
	if (Core != nullptr && bLocalCarrying && !bGoalMode)
	{
		if ((Now - LastPassTargetPollTime) >= TraceHUDStyle::PassTargetPollInterval)
		{
			LastPassTargetPollTime = Now;
			HoveredPassTarget = Core->FindPassTargetFor(LocalChar.Get());
		}

		// Once a pass is actually in flight the Core's own choice of receiver outranks our probe:
		// that is the player the server will hand the Core to, whatever the aim has drifted onto.
		if (ATraceCharacter* Locked = Core->GetEffectivePassTarget())
		{
			HoveredPassTarget = Locked;
		}
	}
	else
	{
		HoveredPassTarget = nullptr;
	}

	ATraceCharacter* Target = HoveredPassTarget.Get();
	if (Target != nullptr && !Target->IsAlive())
	{
		Target = nullptr;
		HoveredPassTarget = nullptr;
	}

	PassLockAlpha = (DeltaSeconds > 0.f)
		? FMath::FInterpTo(PassLockAlpha, (Target != nullptr) ? 1.f : 0.f, DeltaSeconds, TraceHUDStyle::PassLockInterpSpeed)
		: ((Target != nullptr) ? 1.f : 0.f);

	// ---- Where the pass ray lands on screen ----------------------------------------------------
	//
	// THIS IS THE PART THAT IS NOT OBVIOUS, so it is spelled out. A pass is aimed from
	// GetPawnViewLocation() along the control rotation — the same ray a bullet uses. In first person
	// the camera sits ON that origin, so the ray projects to a single point: screen centre. In third
	// person the camera is 450 uu behind it and ~110 uu above it, and a ray seen from a point that is
	// not on it does NOT project to one screen position: it projects to a line. Screen centre is that
	// line's vanishing point, i.e. the pass ray at INFINITY, and every finite distance sits below it —
	// by ~3 deg (about 35 px at 720p) at a typical 16 m pass, more the closer the receiver is. A
	// centred reticle in third person would therefore be a lie of exactly the kind the first-person
	// crosshair goes to such lengths to avoid.
	//
	// So the reticle marks the pass ray at a DISTANCE, and the distance is the receiver's when there
	// is one — which lands the bracket squarely on the teammate the Core is about to go to.
	const FVector ViewLocation = LocalChar->GetPawnViewLocation();
	const FVector AimDirection = LocalChar->GetAimDirection();

	const float DesiredDistance = (Target != nullptr)
		? FMath::Max(200.f, static_cast<float>(FVector::Dist(ViewLocation, Target->GetActorLocation())))
		: TraceHUDStyle::PassAnchorDefaultDistance;

	PassAnchorDistance = (PassAnchorDistance <= 0.f || DeltaSeconds <= 0.f)
		? DesiredDistance
		: FMath::FInterpTo(PassAnchorDistance, DesiredDistance, DeltaSeconds, TraceHUDStyle::PassAnchorInterpSpeed);

	const FVector AimPoint = ViewLocation + AimDirection * PassAnchorDistance;
	const FVector Projected = Canvas->Project(AimPoint);

	// Behind the camera is arithmetically impossible here (the anchor is >= 200 uu in front of an eye
	// that is itself 450 uu in front of the camera), but a projected point that has been clamped to
	// the near plane would put the reticle somewhere meaningless, so it is checked rather than
	// assumed. Falling back to the centre is the honest failure: it is the ray's vanishing point.
	if (Projected.Z <= 0.f)
	{
		return;
	}

	// Cross-faded with the camera blend, so the reticle TRAVELS from the centre to the pass point
	// over the same 0.35s the camera takes to pull back, instead of jumping the moment the Core is
	// picked up. At ViewBlend 0 this is exactly the centre, bit for bit.
	ReticleX = FMath::RoundToFloat(FMath::Lerp(CX, static_cast<float>(Projected.X), ViewBlend));
	ReticleY = FMath::RoundToFloat(FMath::Lerp(CY, static_cast<float>(Projected.Y), ViewBlend));

	// A receiver at the very edge of the aim cone can put the anchor near the frame edge; keep the
	// whole reticle on screen so it can never be half-drawn.
	const float EdgePad = 48.f * UIScale;
	ReticleX = FMath::Clamp(ReticleX, EdgePad, FMath::Max(EdgePad, ViewW - EdgePad));
	ReticleY = FMath::Clamp(ReticleY, EdgePad, FMath::Max(EdgePad, ViewH - EdgePad));

	// One Display line whenever the answer CHANGES, and Display deliberately: the reticle's whole
	// claim is that the player it highlights is the player the Core will actually be given to, and
	// when a pass visibly fails this line is the first thing worth reading. It is also the only way
	// to tell "the HUD did not find a receiver" from "the HUD found one and drew it wrong" — a
	// distinction no screenshot can make. Silent while nothing changes.
	if (bLocalCarrying && Target != LastLoggedPassTarget.Get())
	{
		LastLoggedPassTarget = Target;
		UE_LOG(LogTraceGame, Display,
			TEXT("[Reticle] pass target: %s (blend=%.2f anchor=%.0fuu reticle=%.0f,%.0f centre=%.0f,%.0f)"),
			(Target != nullptr) ? *Target->GetName() : TEXT("<none>"),
			ViewBlend, PassAnchorDistance, ReticleX, ReticleY, CX, CY);
	}
}

void ATraceHUD::DrawCrosshair()
{
	// ============================================================================================
	// "THERE'S STILL NO CROSSHAIR IN THIRD PERSON." — reported twice. Read this before changing it.
	// ============================================================================================
	//
	// The previous version CROSS-FADED the first-person cross out and the third-person pass brackets
	// in, and anchored those brackets on the projected pass ray, which in third person sits about
	// 30 px BELOW screen centre. Every individual decision there was defensible and the net result
	// was still a bug report: the player looks at the middle of the screen, finds nothing there, and
	// concludes there is no crosshair. They were right — there was nothing at the centre.
	//
	// The rule now, and it is not negotiable without another bug report:
	//
	//   THERE IS ALWAYS A CROSSHAIR AT THE EXACT CENTRE OF THE SCREEN, IN BOTH CAMERA MODES.
	//
	// The pass state is LAYERED ON TOP of it — brackets that close around it, a colour change, a
	// caption — instead of replacing it. Three layers, back to front:
	//
	//   1. the centre crosshair          — always, both modes, never fades below full strength;
	//   2. the pass brackets             — third person only, CONCENTRIC with the centre crosshair,
	//                                      so the closing bracket reads as the same instrument
	//                                      changing state rather than as a second one appearing;
	//   3. the pass-point marker         — a small diamond at the actual projected pass ray, which
	//                                      is what keeps the reticle honest: the Core leaves along
	//                                      that ray, not along the screen centre, and in third
	//                                      person those are genuinely different pixels. It is a
	//                                      SUBORDINATE mark on a receiver, not the crosshair.
	//
	// What changes with the camera blend is now only the crosshair's SIZE and COLOUR, never its
	// presence and never its position.
	if (!bLocalAlive)
	{
		return;
	}

	// Screen centre, integer-snapped. Not ReticleX/ReticleY — that is the pass anchor, and using it
	// here is exactly what moved the crosshair off centre and produced the bug report.
	const float CX = FMath::RoundToFloat(ViewW * 0.5f);
	const float CY = FMath::RoundToFloat(ViewH * 0.5f);

	// Third person is the CARRYING view: no gun (contract §3), so the crosshair stops being a promise
	// about a bullet and becomes the pointer the pass is aimed with. It grows, because it is now the
	// only thing on screen doing that job and it has to survive a busy third-person frame, and it
	// takes the team colour once a receiver is under it.
	const float Scale = FMath::Lerp(1.f, TraceHUDStyle::ThirdPersonCrosshairScaleSetting(), ViewBlend);

	FLinearColor CrosshairInk = FLinearColor(1.f, 1.f, 1.f, 0.94f);
	if (ViewBlend > 0.02f && PassLockAlpha > 0.f)
	{
		// Only ever a LIFT toward the team colour, never a fade to it: a colour-blind player, or one
		// looking at a cyan wall, still has the white core and the shape.
		CrosshairInk = TraceHUDStyle::LerpColor(CrosshairInk,
			TraceHUDStyle::Shade(TraceTeamColor(LocalTeam), 1.0f, 0.45f), PassLockAlpha * ViewBlend);
	}

	DrawAimReticle(CX, CY, /*Visibility=*/1.f, Scale, CrosshairInk);

	if (ViewBlend > 0.02f)
	{
		DrawPassReticle(ViewBlend);
	}
}

void ATraceHUD::DrawAimReticle(float CX, float CY, float Visibility, float Scale, const FLinearColor& InkColor)
{
	// In first person the weapon fires from GetPawnViewLocation() along the control rotation, which
	// projects to exactly the centre of the viewport, so this reticle is a literal promise about
	// where the bullet goes.
	//
	// Two consequences, both handled below: it has to be PIXEL-CRISP, and it has to stay legible
	// over the arena's bright neon as well as over its black floor.

	// --- Pixel snapping -------------------------------------------------------------------------
	//
	// The old reticle was built from DrawLine at fractional coordinates and a fractional thickness.
	// At 1280x720 UIScale is 0.667, so every arm was 1.33px wide sitting on a half-pixel boundary
	// and the "centre dot" was a 1.33px square straddling four pixels — Canvas anti-aliases that
	// into a grey smudge. That is a crosshair that looks out of focus no matter how sharp the rest
	// of the frame is, and it is the part of "everything is blurry" that belongs to the HUD.
	//
	// So: integer thickness, integer lengths, integer origin, and axis-aligned DrawRect instead of
	// DrawLine. Rects at integer coordinates land on exact pixels and receive no anti-aliasing at
	// all, which is the whole point.
	//
	// FLOORS, not just scales. At 1280x720 UIScale is 0.667, and the reference sizes below rounded
	// down to a ONE-PIXEL-wide reticle. It was pixel-exact and unblurred — and invisible: a captured
	// in-match frame had to be checked by sampling individual pixels at screen centre to confirm the
	// crosshair had been drawn at all, because against a lit cyan surface (193, 252, 253) a single
	// white pixel with a single 55%-black neighbour is nothing. In first person this reticle is the
	// aim point, so the minimums below are what it may never shrink past, whatever the resolution.
	//
	// @param Scale     1.0 in first person; larger in third person, where this is the pass pointer
	//                  rather than the gun's aim point and has to hold its own in a busier frame.
	// @param InkColor  the fill; alpha is multiplied by Visibility, so callers pass an opaque colour.
	const float S   = FMath::Max(0.5f, Scale);
	const float T   = FMath::Max(2.f, FMath::RoundToFloat(2.5f * UIScale * S));
	const float Arm = FMath::Max(6.f, FMath::RoundToFloat(11.f * UIScale * S));
	const float Gap = FMath::Max(3.f, FMath::RoundToFloat(5.f  * UIScale * S));

	// Half a bar, floored, so the bar's own pixels straddle the centre symmetrically for odd T and
	// sit flush against it for even T. Both are exact; neither is a half-pixel.
	const float Half = FMath::FloorToFloat(T * 0.5f);

	const FLinearColor Ink    = TraceHUDStyle::WithAlpha(InkColor, InkColor.A * Visibility);
	// A one-pixel dark surround. The arena is black floor plus saturated cyan and amber neon, and a
	// plain white reticle disappears the moment it crosses a lit edge or a bright trail. The
	// outline costs nothing and makes the aim point unconditionally readable — but only if it is
	// actually opaque enough to separate white from cyan, which at 0.55 it was not.
	const FLinearColor Shadow = FLinearColor(0.f, 0.f, 0.f, 0.80f * Visibility);

	// Each arm and the centre dot as an integer rect: (left, top, width, height).
	const float Bars[5][4] =
	{
		{ CX - Gap - Arm,  CY - Half,        Arm,  T   },   // left
		{ CX + Gap,        CY - Half,        Arm,  T   },   // right
		{ CX - Half,       CY - Gap - Arm,   T,    Arm },   // up
		{ CX - Half,       CY + Gap,         T,    Arm },   // down
		{ CX - Half,       CY - Half,        T,    T   },   // centre dot: the exact aim point
	};

	for (const float* B : Bars)
	{
		DrawRect(Shadow, B[0] - 1.f, B[1] - 1.f, B[2] + 2.f, B[3] + 2.f);
	}
	for (const float* B : Bars)
	{
		DrawRect(Ink, B[0], B[1], B[2], B[3]);
	}
}

void ATraceHUD::DrawPassReticle(float Visibility)
{
	// A LAYER ON THE CROSSHAIR, not a replacement for it (see DrawCrosshair). The brackets are drawn
	// CONCENTRIC with the centre crosshair so the two read as one instrument in two states, and the
	// only thing that ever sits away from the centre is the small pass-point marker at the end of
	// this function.
	//
	// Everything below is integer-snapped for the same reason the aim reticle is: at 720p a
	// fractional rect is a grey smudge, and a smudged reticle is what "blurry" looks like.
	const float T = FMath::Max(2.f, FMath::RoundToFloat(2.5f * UIScale));
	const float Len = FMath::Max(6.f, FMath::RoundToFloat(11.f * UIScale));

	// The brackets CLOSE when a receiver is acquired. Motion is what the eye catches; a colour swap
	// alone can be missed in the middle of a chase, and one that could only be seen by its colour
	// would be invisible to a colour-blind player.
	//
	// The OPEN radius must CLEAR the enlarged third-person crosshair so the brackets frame the cross
	// instead of colliding with it, and the closed radius must stop just outside it.
	//
	// DERIVED from the crosshair's own arm reach, not a matching pair of literals. The scale is a
	// live setting now (UTraceSettings::ThirdPersonCrosshairScale, 1.0-3.0), and hardcoded 40/30
	// against a 1.6 crosshair would have the brackets drawn straight through the cross the moment
	// anyone raised it — a knob that visibly breaks the UI at the top of its own clamp is not a knob.
	// The cross's arms reach (5 + 11) * UIScale * Scale; the constants below are the clearances.
	const float ArmReach = (5.f + 11.f) * UIScale * TraceHUDStyle::ThirdPersonCrosshairScaleSetting();
	const float OpenRadius = ArmReach + (14.f * UIScale);
	const float ClosedRadius = ArmReach + (4.f * UIScale);
	const float Radius = FMath::RoundToFloat(FMath::Lerp(OpenRadius, ClosedRadius, PassLockAlpha));

	const float X = FMath::RoundToFloat(ViewW * 0.5f);
	const float Y = FMath::RoundToFloat(ViewH * 0.5f);

	const bool bLocked = HoveredPassTarget.IsValid();

	// Cooldown state is worth its own colour: a player hovering a teammate and wondering why nothing
	// happens is exactly the confusion this reticle exists to remove.
	//
	// Gated on actually being the carrier: the cooldown is a property of the CORE, not of a player,
	// so the teammate who just received a pass inherits the tail of it — and telling someone who is
	// not holding anything that their pass is not ready would be noise.
	ATraceCore* Core = (TraceGS != nullptr) ? TraceGS->Core : nullptr;
	const float CooldownRemaining = (Core != nullptr) ? Core->GetPassCooldownRemaining() : 0.f;
	const bool bOnCooldown = bLocalCarrying && (CooldownRemaining > TraceHUDStyle::TimeEpsilon);

	FLinearColor Base = TraceHUDStyle::WithAlpha(TraceHUDStyle::Ink, 0.72f);
	if (bOnCooldown)
	{
		Base = TraceHUDStyle::WithAlpha(TraceHUDStyle::Danger, 0.75f);
	}
	else if (bLocked)
	{
		// Team coloured and lifted toward white: a pass is the one action whose whole point is the
		// teammate on the other end of it, so the lock wears the team's colour.
		Base = TraceHUDStyle::Shade(TraceTeamColor(LocalTeam), 1.0f, 0.35f);
	}

	const FLinearColor Ink = TraceHUDStyle::WithAlpha(Base, Base.A * Visibility);
	const FLinearColor Shadow = FLinearColor(0.f, 0.f, 0.f, 0.80f * Visibility);

	// Four L-shaped corner brackets, as eight axis-aligned integer rects: (left, top, width, height).
	const float X0 = X - Radius;              // outer left edge
	const float X1 = X + Radius - T;          // outer right edge (inset by its own thickness)
	const float Y0 = Y - Radius;
	const float Y1 = Y + Radius - T;
	const float Tail = Len - T;               // where a right/bottom arm starts so it ENDS on the edge

	// NO CENTRE DOT HERE. The centre belongs to the crosshair this is layered over — drawing a second
	// dot on the same pixel is how two elements that are meant to read as one instrument end up
	// looking like two.
	const float Bars[8][4] =
	{
		{ X0,        Y0,        Len, T   },   // top-left, horizontal
		{ X0,        Y0,        T,   Len },   // top-left, vertical
		{ X1 - Tail, Y0,        Len, T   },   // top-right, horizontal
		{ X1,        Y0,        T,   Len },   // top-right, vertical
		{ X0,        Y1,        Len, T   },   // bottom-left, horizontal
		{ X0,        Y1 - Tail, T,   Len },   // bottom-left, vertical
		{ X1 - Tail, Y1,        Len, T   },   // bottom-right, horizontal
		{ X1,        Y1 - Tail, T,   Len },   // bottom-right, vertical
	};

	for (const float* B : Bars)
	{
		DrawRect(Shadow, B[0] - 1.f, B[1] - 1.f, B[2] + 2.f, B[3] + 2.f);
	}
	for (const float* B : Bars)
	{
		DrawRect(Ink, B[0], B[1], B[2], B[3]);
	}

	// ---- The pass-point marker -------------------------------------------------------------------
	//
	// THE HONESTY LAYER, and the reason the old code put the whole reticle down here. A pass is aimed
	// from GetPawnViewLocation() along the control rotation. In first person the camera sits ON that
	// ray so it projects to screen centre; in third person the camera is 450 uu behind it, the ray
	// projects to a LINE, and screen centre is only that line's vanishing point — every finite pass
	// distance lands below it (~30 px at 720p for a typical 16 m pass).
	//
	// So the centre crosshair is where you POINT and this diamond is where the Core actually goes.
	// It is deliberately small and subordinate: it marks a receiver, it is not the crosshair, and
	// making it the crosshair is precisely the mistake that got reported as "there is no crosshair".
	// Suppressed when it would sit on top of the centre anyway (first-person-ish blends, a receiver
	// near the vanishing point), where two marks on one spot would just look like a smudge.
	const float MarkerDX = ReticleX - X;
	const float MarkerDY = ReticleY - Y;
	if ((MarkerDX * MarkerDX + MarkerDY * MarkerDY) > FMath::Square(10.f * UIScale))
	{
		const float D = FMath::Max(3.f, FMath::RoundToFloat(5.f * UIScale));   // half-diagonal, in px
		const float MX = ReticleX;
		const float MY = ReticleY;

		// A diamond as a stack of horizontal integer rows: axis-aligned rects again, so it stays
		// unblurred at any resolution, and a shape no other HUD element uses.
		for (float Row = -D; Row <= D; Row += 1.f)
		{
			const float HalfWidth = D - FMath::Abs(Row);
			DrawRect(Shadow, MX - HalfWidth - 1.f, MY + Row, (HalfWidth * 2.f) + 3.f, 1.f);
		}
		for (float Row = -D; Row <= D; Row += 1.f)
		{
			const float HalfWidth = D - FMath::Abs(Row);
			DrawRect(Ink, MX - HalfWidth, MY + Row, (HalfWidth * 2.f) + 1.f, 1.f);
		}
	}

	// ---- Caption ---------------------------------------------------------------------------------
	//
	// Suppressed once the hold has started: DrawPassProgress owns the ring and the word PASSING at
	// that point, and two captions on the same pixel is how a HUD starts overlapping itself.
	const bool bPassing = (TracePC != nullptr) && (TracePC->GetPassProgress() >= 0.f);
	if (bPassing)
	{
		return;
	}

	// Spec v16 §2 — the same rule for the throw charge, and it is not a style call. The ring writes
	// "62%  -  POWER 72%" on this exact pixel row, and the first armed capture photographed it
	// printed straight through "LMB  -  THROW" into an unreadable smear. Whichever ring is up owns
	// the caption; the crosshair goes quiet.
	if (IsThrowChargeRingUp())
	{
		return;
	}

	FString Caption;
	FLinearColor CaptionColor = TraceHUDStyle::InkDim;

	// ---- MODE B: the caption is the control scheme, and it is a different one ---------------------
	//
	// Spec v4 §7: "The carrier should be able to throw the core forward by left clicking." There is
	// no receiver to acquire, no hold, and no teammate to name — the throw goes where the crosshair
	// points and the first player to reach it, either team, takes it. So mode B gets its own short
	// caption rather than mode A's, and it takes this branch before any of the receiver logic below
	// can put a teammate's name under a reticle that does not aim at teammates.
	if (bGoalMode)
	{
		if (bOnCooldown)
		{
			Caption = FString::Printf(TEXT("THROW READY IN %.1f"), CooldownRemaining);
			CaptionColor = TraceHUDStyle::Danger;
		}
		else if (bLocalCarrying)
		{
			Caption = TEXT("LMB  -  THROW");
			CaptionColor = TraceHUDStyle::Shade(TraceTeamColor(LocalTeam), 1.0f, 0.35f);
		}

		if (!Caption.IsEmpty())
		{
			DrawTextCentered(Caption, TraceHUDStyle::WithAlpha(CaptionColor, Visibility),
				X, Y + Radius + (14.f * UIScale), FontSmall, UIScale);
		}
		return;
	}

	if (bOnCooldown)
	{
		Caption = FString::Printf(TEXT("PASS READY IN %.1f"), CooldownRemaining);
		CaptionColor = TraceHUDStyle::Danger;
	}
	else if (bLocked)
	{
		// The receiver by NAME. In a 5v5 with two teammates overlapping on screen, "which one" is a
		// real question and the Core picks whoever is nearest the crosshair — so the HUD says so out
		// loud rather than leaving the player to infer it from a bracket.
		FString ReceiverName;
		if (const ATraceCharacter* Target = HoveredPassTarget.Get())
		{
			if (const APlayerState* TargetState = Target->GetPlayerState())
			{
				ReceiverName = TargetState->GetPlayerName();
			}
		}
		Caption = ReceiverName.IsEmpty()
			? FString(TEXT("HOLD TO PASS"))
			: FString::Printf(TEXT("HOLD TO PASS  -  %s"), *ReceiverName.ToUpper());
		CaptionColor = TraceHUDStyle::Shade(TraceTeamColor(LocalTeam), 1.0f, 0.35f);
	}

	if (!Caption.IsEmpty())
	{
		DrawTextCentered(Caption, TraceHUDStyle::WithAlpha(CaptionColor, Visibility),
			X, Y + Radius + (14.f * UIScale), FontSmall, UIScale);
	}
}

void ATraceHUD::DrawPassProgress()
{
	if (TracePC == nullptr)
	{
		return;
	}

	// Negative means no pass in progress — and also means a build whose character slice has not
	// landed GetPassProgress() yet, so the ring simply never appears rather than lying.
	const float Progress = TracePC->GetPassProgress();
	if (Progress < 0.f)
	{
		return;
	}

	// The filled arc is TEAM COLOURED, because a pass is the one action whose whole point is the
	// teammate on the other end of it.
	DrawCrosshairRing(Progress, TraceHUDStyle::Shade(TraceTeamColor(LocalTeam), 1.0f, 0.30f),
		TEXT("PASSING"), TraceHUDStyle::WithAlpha(TraceHUDStyle::Ink, 0.85f));
}

void ATraceHUD::DrawCrosshairRing(float FillAlpha, const FLinearColor& FillColor,
	const FString& Caption, const FLinearColor& CaptionColor)
{
	// The ring closes around the CENTRE CROSSHAIR, because that is now where the crosshair always is
	// (see DrawCrosshair) and a progress ring that is not concentric with the thing it is the
	// progress of reads as two unrelated pieces of UI. It used to close around the pass anchor, back
	// when the anchor was the reticle; that whole arrangement is what produced "there is no
	// crosshair in third person".
	const float CX = FMath::RoundToFloat(ViewW * 0.5f);
	const float CY = FMath::RoundToFloat(ViewH * 0.5f);

	// Outside the open bracket radius, so the ring frames the brackets rather than cutting through
	// them. Derived from the same crosshair arm reach DrawPassReticle() uses, for the same reason:
	// ThirdPersonCrosshairScale is a live setting, and a ring pinned to a literal would be sliced by
	// the brackets as soon as anyone raised it. Clearance is 8 px beyond the open bracket radius.
	const float ArmReach = (5.f + 11.f) * UIScale * TraceHUDStyle::ThirdPersonCrosshairScaleSetting();
	const float Radius = ArmReach + (22.f * UIScale);
	const float Thickness = FMath::Max(2.f, 3.f * UIScale);

	// Canvas has no arc primitive, so the ring is a fan of short chords. 48 segments is smooth at any
	// size this is ever drawn at and costs ~48 DrawLine calls on the one frame in a hundred that a
	// pass is actually being held.
	constexpr int32 Segments = 48;
	const float ClampedAlpha = FMath::Clamp(FillAlpha, 0.f, 1.f);

	auto PointAt = [CX, CY, Radius](float T)
	{
		// Starts at twelve o'clock and closes clockwise: the direction every progress ring in every
		// game closes, and the one a player reads without being told.
		const float Angle = -UE_HALF_PI + T * UE_TWO_PI;
		return FVector2D(CX + Radius * FMath::Cos(Angle), CY + Radius * FMath::Sin(Angle));
	};

	// Unfilled track first, so the ring reads as a dial rather than as a growing arc from nowhere.
	//
	// DIMMED IN RGB, NOT IN ALPHA, and that is not a style preference. AHUD::DrawLine builds an
	// FCanvasLineItem, and FCanvasItem's constructor sets SE_BLEND_Opaque — so the alpha handed to
	// DrawLine is DISCARDED. This line used to be FLinearColor(1,1,1,0.18) and rendered as pure
	// white: the "faint" track was the brightest thing on the dial and the contrast read backwards.
	// Over this HUD's dark backdrop, scaling RGB is what alpha-over-black would have looked like.
	const FLinearColor Track = FLinearColor(0.18f, 0.18f, 0.18f, 1.f);
	for (int32 Index = 0; Index < Segments; ++Index)
	{
		const FVector2D A = PointAt(static_cast<float>(Index) / Segments);
		const FVector2D B = PointAt(static_cast<float>(Index + 1) / Segments);
		DrawLine(A.X, A.Y, B.X, B.Y, Track, Thickness * 0.7f);
	}

	const int32 Filled = FMath::CeilToInt(ClampedAlpha * Segments);
	for (int32 Index = 0; Index < Filled; ++Index)
	{
		const FVector2D A = PointAt(static_cast<float>(Index) / Segments);
		const FVector2D B = PointAt(FMath::Min(ClampedAlpha, static_cast<float>(Index + 1) / Segments));
		DrawLine(A.X, A.Y, B.X, B.Y, FillColor, Thickness);
	}

#if !UE_BUILD_SHIPPING
	// The CHORDS, not the alpha. A ring that computed a healthy 0.62 and then emitted nothing would
	// otherwise report itself as drawn — the exact shape of failure this project has already been
	// caught by once (a geometry index returning 0 while calling itself healthy).
	DrawnChargeRingSegments = Filled;
#endif

	if (!Caption.IsEmpty())
	{
		DrawTextCentered(Caption, CaptionColor, CX, CY + Radius + (10.f * UIScale), FontSmall, UIScale);
	}
}

bool ATraceHUD::IsThrowChargeRingUp() const
{
	// THE RED ARM OWNS THIS TOO, and the first run of Trace.HUD.V16Shots is why the line exists: with
	// only the corner gated, the arm drew the superseded BAR and the new RING at the same time, so
	// the "before" frame was not a before at all. The draw record caught it (chargeRing=YES with
	// arm=0) where a glance at the screenshot would not have.
	if (!TraceHUDV16::IsArmed())
	{
		return false;
	}

	// IsThrowCharging() is false for a bot by construction, so this is the local player's ring only
	// and there is no mode test needed beyond the Core existing — mode A never starts a charge.
	const ATraceCore* ChargeCore = (TraceGS != nullptr) ? TraceGS->Core : nullptr;
	if (ChargeCore == nullptr || !ChargeCore->IsThrowCharging())
	{
		return false;
	}

	// The pass ring wins the pixel. It cannot happen in a shipped mode — mode A passes and never
	// throws, mode B throws and never passes — but the two rings are concentric and identical, so if
	// a future mode ever managed both at once the result would be an unreadable double arc rather
	// than an obvious bug. One line to make that impossible.
	return (TracePC == nullptr) || (TracePC->GetPassProgress() < 0.f);
}

void ATraceHUD::DrawThrowChargeRing()
{
	// LOCAL AND INSTANT, WHICH IS THE REQUIREMENT AND NOT AN OPTIMISATION. Every number here comes
	// from ATraceCore's PREDICTED accessors, which run off a clock started on the frame the button
	// went down on this machine. A meter fed by replicated state would lag the player's own finger by
	// their ping, so they would be aiming a power they are being shown late. The THROW is still
	// resolved entirely on the server from the server's own copy of the hold; the worst this
	// prediction can do is draw a ring for a throw the server then refuses on cooldown.
	if (!IsThrowChargeRingUp())
	{
		return;
	}

	ATraceCore* ChargeCore = TraceGS->Core;

	// *** THE SWEEP IS THE HOLD, LINEARLY, AND FULL AT CoreThrowChargeSeconds (0.8 s after v16 §0).
	// *** Spec v16 §2: "to demonstrate how charged /100% the throw is". GetThrowChargeAlpha() is
	// exactly that fraction and nothing else; GetThrowChargeScaleNow() is the resulting MOMENTUM,
	// which starts at the 15% floor rather than at zero and would therefore draw a ring that is
	// already a sixth full the instant the button goes down.
	const float ChargeAlpha = FMath::Clamp(ChargeCore->GetThrowChargeAlpha(), 0.f, 1.f);
	const float Power = ChargeCore->GetThrowChargeScaleNow();
	const bool  bFull = (ChargeCore->GetThrowChargeAlpha() >= 1.f);

	// Winding up wears the team tint; the instant it is FULL it snaps to Good and pulses at the same
	// 12 rad/s every other "act now" state on this HUD uses. That transition is the single most
	// useful thing the ring does, because past full the charge clamps and holding longer buys
	// nothing — the player needs to know to let go, not to keep holding.
	//
	// THE PULSE IS IN BRIGHTNESS, NOT ALPHA. This was WithAlpha(Good, 0.7 + 0.3*sin), and the ring
	// therefore did not pulse at all: DrawLine discards alpha (see the track comment above), so
	// three full-charge frames captured at unrelated moments measured bit-identical. Two comments
	// claimed a pulse that never once happened. Shade() scales RGB, which survives.
	const float FullPulse = 0.7f + 0.3f * FMath::Sin(Now * 12.f);
	const FLinearColor RingColor = bFull
		? TraceHUDStyle::Shade(TraceHUDStyle::Good, FullPulse, 0.f)
		: TraceHUDStyle::Shade(TraceTeamColor(LocalTeam), 1.0f, 0.30f);

	// THE POWER, not the elapsed hold, and it is what replaces the bar's floor tick.
	//
	// The deleted bar carried a mark at 15% to teach that an instant click is weak rather than
	// nothing. A tick at 15% of a sweep that measures TIME would have been marking a power value on a
	// time axis — the bar's one muddle. The caption says the floor outright instead: at zero charge
	// it already reads "POWER 15%", which is the same lesson stated correctly and read from the same
	// published curve the throw itself releases at, so the readout cannot drift from the game.
	const FString RingCaption = bFull
		? FString(TEXT("FULL  -  POWER 100%"))
		: FString::Printf(TEXT("%.0f%%  -  POWER %.0f%%"),
			100.f * ChargeAlpha, 100.f * FMath::Max(0.f, Power));

	DrawCrosshairRing(ChargeAlpha, RingColor, RingCaption,
		bFull ? TraceHUDStyle::Good : TraceHUDStyle::WithAlpha(TraceHUDStyle::Ink, 0.85f));

#if !UE_BUILD_SHIPPING
	bDrewChargeRing = true;
	DrawnChargeRingAlpha = ChargeAlpha;
#endif
}

void ATraceHUD::DrawHitMarker()
{
	if (TracePC == nullptr)
	{
		return;
	}

	const bool bKill = TracePC->WasLastHitMarkerAKill();
	const float Duration = bKill ? TraceHUDStyle::KillMarkerDuration : TraceHUDStyle::HitMarkerDuration;

	// GetLastHitMarkerTime() is client-local world time, set by ClientNotifyHit. It starts at a
	// large negative sentinel, so Age is enormous until the server first confirms a hit.
	const float Age = Now - TracePC->GetLastHitMarkerTime();
	if (Age < 0.f || Age > Duration)
	{
		return;
	}

	// Fade out, and expand very slightly so the pop still reads at high frame rates.
	const float Alpha = 1.f - (Age / Duration);
	const float Grow = 1.f + (1.f - Alpha) * 0.35f;

	const float CX = ViewW * 0.5f;
	const float CY = ViewH * 0.5f;
	const float Inner = 6.f * UIScale * Grow;
	const float Outer = 14.f * UIScale * Grow;
	const float Thickness = FMath::Max(1.f, 2.5f * UIScale);

	// Spec §6 made damage positional, so the FEEDBACK has to be positional too — otherwise the
	// player has no way to learn the difference between a 100 and a 25. Kill still wins the colour
	// (it is the more important fact); otherwise the zone picks it.
	const ETraceHitZone Zone = TracePC->GetLastHitMarkerZone();
	FLinearColor Base = FLinearColor::White;
	if (bKill)
	{
		Base = TraceHUDStyle::Danger;
	}
	else if (Zone == ETraceHitZone::Head)
	{
		Base = TraceHUDStyle::Warning;
	}
	else if (Zone == ETraceHitZone::Legs)
	{
		Base = TraceHUDStyle::InkDim;
	}
	const FLinearColor Color = TraceHUDStyle::WithAlpha(Base, Alpha);

	// Four diagonal ticks — the classic X, drawn as four separate segments so the middle stays
	// clear and the crosshair underneath is still readable.
	DrawLine(CX - Outer, CY - Outer, CX - Inner, CY - Inner, Color, Thickness);
	DrawLine(CX + Inner, CY - Inner, CX + Outer, CY - Outer, Color, Thickness);
	DrawLine(CX - Outer, CY + Outer, CX - Inner, CY + Inner, Color, Thickness);
	DrawLine(CX + Inner, CY + Inner, CX + Outer, CY + Outer, Color, Thickness);

	// A head hit gets a second, larger ring of ticks: an unmistakable different SHAPE, so it reads
	// even for a player who cannot separate the two colours.
	if (!bKill && Zone == ETraceHitZone::Head)
	{
		const float FarInner = Outer + (4.f * UIScale);
		const float FarOuter = FarInner + (7.f * UIScale);
		DrawLine(CX - FarOuter, CY - FarOuter, CX - FarInner, CY - FarInner, Color, Thickness);
		DrawLine(CX + FarInner, CY - FarInner, CX + FarOuter, CY - FarOuter, Color, Thickness);
		DrawLine(CX - FarOuter, CY + FarOuter, CX - FarInner, CY + FarInner, Color, Thickness);
		DrawLine(CX + FarInner, CY + FarInner, CX + FarOuter, CY + FarOuter, Color, Thickness);
	}
}

// -------------------------------------------------------------------------------------------
// Bottom left: dash pip + health
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawHealthAndDash()
{
	// LocalChar IS ALLOWED TO BE NULL HERE, and that is a spec v14 §5 requirement rather than
	// defensive slack. The activated ability's cooldown keeps running while its owner is dead, so the
	// row that shows it has to be drawn on a frame where there is no pawn at all. Every block below
	// that touches the pawn now null-checks it; the ability row and the stack geometry do not need one
	// because they are fed by the player state.
	if (TracePC == nullptr)
	{
		return;
	}

	const float Margin  = 40.f * UIScale;
	const float BarW    = 340.f * UIScale;
	const float HealthH = 26.f * UIScale;
	const float RowH    = 10.f * UIScale;
	const float RowGap  = 8.f * UIScale;
	const float LabelW  = 58.f * UIScale;

	const float HealthY = ViewH - Margin - HealthH;

	// The ability stack grows UPWARDS from the health bar, so adding a row does not move health — the
	// one element a player finds by muscle memory rather than by reading.
	float RowY = HealthY - (14.f * UIScale) - RowH;

	const FLinearColor TeamTint = TraceTeamColor(LocalTeam);

	// ---- Equipped weapon (spec v10 §1) ----------------------------------------------------------
	//
	// DRAWN FIRST, so it is the row NEAREST the health bar and therefore the one row in this stack
	// whose screen position never moves. Every other row here is conditional — parry only for a
	// carrier, slide only mid-slide — so a row drawn after them would slide up and down the screen
	// as those conditions flicker. The weapon is the one piece of state that is always true, and a
	// player checking "am I holding the knife?" mid-fight must find it in the same place every time.
	//
	// It also carries the two timings the spec names, because both are invisible otherwise and both
	// are refusals the player will otherwise read as the game ignoring their input:
	//   - the 0.2 s PULLOUT, during which you can neither shoot nor swing, drawn as a filling meter;
	//   - the 0.5 s swing COOLDOWN, drawn the same way, so "why did my click do nothing" has an
	//     answer on screen rather than in a log.
	// A dead player gets no row at all: the weapon you are not holding is not information.
	if (LocalChar != nullptr && LocalChar->IsAlive())
	{
		const bool bKnife   = TraceMelee::IsKnifeEquipped(LocalChar);
		const float Deploy  = TraceMelee::GetDeployRemaining(LocalChar);
		const float Cooling = TraceMelee::GetSwingCooldownRemaining(LocalChar);

		// The CARRIER's weapon is stowed, not held: they cannot shoot and cannot swing, and saying
		// "KNIFE" to somebody whose knife does nothing is the same lie the SHIELD DOWN callout exists
		// to avoid. They get the row, but it says what is actually true.
		const FString WeaponText = bLocalCarrying
			? FString(TEXT("STOWED"))
			: (bKnife ? FString(TEXT("KNIFE")) : FString(TEXT("GUN")));

		DrawTextLeft(TEXT("WEAPON"), bLocalCarrying ? TraceHUDStyle::InkDim : TraceHUDStyle::Ink,
			Margin, VCenterTextY(FString(TEXT("WEAPON")), FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

		// One meter, three meanings, in priority order — pullout beats cooldown beats ready, which is
		// exactly the order in which they gate the trigger.
		float  Fraction = 1.f;
		FLinearColor WeaponColor = bKnife
			? FLinearColor(0.85f, 0.85f, 0.92f, 1.f)              // blade white
			: TraceHUDStyle::Shade(TeamTint, 1.0f, 0.25f);        // the gun wears the team's colour
		FString StatusText = WeaponText;

		if (Deploy > TraceHUDStyle::TimeEpsilon)
		{
			const float SwapTotal = FMath::Max(TraceHUDStyle::TimeEpsilon, TraceMelee::GetSwapSeconds());
			Fraction = FMath::Clamp(1.f - (Deploy / SwapTotal), 0.f, 1.f);
			WeaponColor = TraceHUDStyle::Shade(TeamTint, 0.45f, 0.0f);
			StatusText = FString::Printf(TEXT("%s  DRAWING"), *WeaponText);
		}
		else if (bKnife && !bLocalCarrying && Cooling > TraceHUDStyle::TimeEpsilon)
		{
			const float CooldownTotal = FMath::Max(TraceHUDStyle::TimeEpsilon, TraceMelee::GetSwingCooldownSeconds());
			Fraction = FMath::Clamp(1.f - (Cooling / CooldownTotal), 0.f, 1.f);
			WeaponColor = FLinearColor(0.45f, 0.45f, 0.50f, 1.f);
			StatusText = FString::Printf(TEXT("%s  %.1f"), *WeaponText, Cooling);
		}
		else if (bLocalCarrying)
		{
			Fraction = 0.35f;
			WeaponColor = TraceHUDStyle::Shade(TeamTint, 0.45f, 0.0f);
		}

		DrawMeter(Margin + LabelW, RowY, BarW - LabelW, RowH, Fraction, WeaponColor);

		DrawTextLeft(StatusText, TraceHUDStyle::InkDim,
			Margin + BarW + (10.f * UIScale),
			VCenterTextY(StatusText, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

		RowY -= (RowH + RowGap);
	}

	// ---- Throw charge — SUPERSEDED BY THE CROSSHAIR RING (spec v16 §2) --------------------------
	//
	// Verbatim: "For the throw charge, use the old circle around the crosshair animation for game
	// mode a to demonstrate how charged /100% the throw is, RATHER THAN A BAR ON THE HUD." So this
	// row is off in a shipped build; DrawThrowChargeRing() is where the charge is drawn now.
	//
	// IT IS KEPT, BEHIND THE RED ARM, AND ONLY FOR THAT. Trace.HUD.V16 0 puts the bar back so the
	// superseded HUD can be photographed against the same fixture in the same binary as the ring —
	// which for a drawing change is the only "before" that proves anything. Delete it the day the
	// arm goes, not before.
	//
	// The v13 §6 reasoning it was written under is unchanged and now lives on DrawThrowChargeRing().
	if (!TraceHUDV16::IsArmed())
	{
		ATraceCore* ChargeCore = (TraceGS != nullptr) ? TraceGS->Core : nullptr;
		if (ChargeCore != nullptr && ChargeCore->IsThrowCharging())
		{
#if !UE_BUILD_SHIPPING
			bDrewChargeBar = true;
#endif
			const float Alpha = FMath::Max(0.f, ChargeCore->GetThrowChargeAlpha());
			const float Power = ChargeCore->GetThrowChargeScaleNow();
			const bool  bFull = (Alpha >= 1.f);

			const FString ChargeLabel(TEXT("THROW"));
			DrawTextLeft(ChargeLabel, bFull ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim,
				Margin, VCenterTextY(ChargeLabel, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

			// Winding up wears the team tint dimly; the instant it is FULL it snaps to Good and pulses
			// at the same 12 rad/s every other "act now" state on this HUD uses. That transition is the
			// single most useful thing the meter does, because past full the charge clamps and holding
			// longer buys nothing — the player needs to know to let go, not to keep holding.
			const FLinearColor ChargeColor = bFull
				? TraceHUDStyle::WithAlpha(TraceHUDStyle::Good, 0.7f + 0.3f * FMath::Sin(Now * 12.f))
				: TraceHUDStyle::Shade(TeamTint, 0.75f, 0.1f);

			const float MeterX = Margin + LabelW;
			const float MeterW = BarW - LabelW;
			DrawMeter(MeterX, RowY, MeterW, RowH, FMath::Min(Alpha, 1.f), ChargeColor);

			// THE FLOOR TICK. An instant click is not zero power (15%), and a meter that starts empty
			// implies it is — a player would read a fast tap as "the throw did not happen" rather than
			// as "the throw was weak", which is the exact misreading the floor exists to prevent. So the
			// bar carries a mark at the floor: below it is unreachable, and that is worth showing once
			// rather than explaining never.
			const float FloorFraction = FMath::Clamp(ATraceCore::GetThrowChargeScaleForHold(0.f), 0.f, 1.f);
			DrawRect(TraceHUDStyle::WithAlpha(TraceHUDStyle::Ink, 0.55f),
				MeterX + (MeterW * FloorFraction), RowY, FMath::Max(1.f, 1.f * UIScale), RowH);

			// The POWER, not the elapsed hold: momentum is what the player is choosing, and seconds are
			// a number they would have to convert. Printed from the same published curve the throw
			// itself releases at (GetThrowChargeScaleForHold), so the readout cannot drift from the game.
			const FString ChargeText = bFull
				? FString(TEXT("FULL"))
				: FString::Printf(TEXT("%.0f%%"), 100.f * FMath::Max(0.f, Power));
			DrawTextLeft(ChargeText, bFull ? TraceHUDStyle::Good : TraceHUDStyle::InkDim,
				Margin + BarW + (10.f * UIScale),
				VCenterTextY(ChargeText, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

			RowY -= (RowH + RowGap);
		}
	}

	// ---- Parry (spec §3), where BOOST used to be ------------------------------------------------
	//
	// BOOST IS DELETED (spec §1: "remove boost from the game entirely"), and its row is gone with it,
	// including the GetBoostHudState() call that fed it — leaving that call here would have broken
	// the build the moment the movement slice removed the accessor.
	//
	// The ability stack keeps its shape because parry inherits the slot: a carrier-only, cooldown-
	// gated key on the same stack as dash. Drawn only when the mechanic actually reports state (see
	// ATraceCharacter::GetParryHudState) and only for the CARRIER, since a non-carrier pressing parry
	// does nothing at all and a meter for a key that does nothing is worse than no meter.
	if (bLocalCarrying && LocalChar != nullptr)
	{
		float ParryRemaining = 0.f;
		float ParryTotal = 0.f;
		bool bParryActive = false;
		if (LocalChar->GetParryHudState(ParryRemaining, ParryTotal, bParryActive))
		{
			const float Charge = FMath::Clamp(1.f - (ParryRemaining / FMath::Max(TraceHUDStyle::TimeEpsilon, ParryTotal)), 0.f, 1.f);
			const bool bReady = (ParryRemaining <= TraceHUDStyle::TimeEpsilon);

			const FString Label(TEXT("PARRY"));
			DrawTextLeft(Label, bReady ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim,
				Margin, VCenterTextY(Label, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

			// RED while the 0.2s window is open, matching the red the whole trace turns (spec §3), so
			// the meter and the world are saying the same thing at the same moment. Otherwise its own
			// hue, so the stack does not read as one three-line bar.
			const FLinearColor ParryColor = bParryActive
				? TraceHUDStyle::Danger
				: (bReady ? FLinearColor(0.95f, 0.35f, 0.30f, 1.f) : FLinearColor(0.38f, 0.16f, 0.14f, 1.f));

			DrawMeter(Margin + LabelW, RowY, BarW - LabelW, RowH, bParryActive ? 1.f : Charge, ParryColor);

			if (!bReady && !bParryActive)
			{
				const FString CountdownText = FString::Printf(TEXT("%.1f"), ParryRemaining);
				DrawTextLeft(CountdownText, TraceHUDStyle::InkDim,
					Margin + BarW + (10.f * UIScale),
					VCenterTextY(CountdownText, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);
			}

			RowY -= (RowH + RowGap);
		}
	}

	// ---- Dash charges -------------------------------------------------------------------------
	//
	// PIPS, not a bar. Spec §5 gives the Core carrier a second dash charge, and a single fill bar
	// cannot express "one banked, one recharging" — which is exactly the state a carrier has to read
	// before deciding whether to spend one escaping. The pip row degrades to a single pip in a build
	// without the charge system, so it is correct either way.
	{
		FTraceDashHudState Dash;
		if (TracePC->GetDashHudState(Dash))
		{
			const bool bReady = (Dash.Charges > 0);

			const FString DashLabel(TEXT("DASH"));
			DrawTextLeft(DashLabel, bReady ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim,
				Margin, VCenterTextY(DashLabel, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

			// Charging reads as a dim team-tinted sliver; ready snaps to full brightness.
			const FLinearColor PipColor = bReady
				? TraceHUDStyle::Shade(TeamTint, 1.0f, 0.25f)
				: TraceHUDStyle::Shade(TeamTint, 0.45f, 0.0f);

			DrawChargePips(Margin + LabelW, RowY, BarW - LabelW, RowH,
				Dash.Charges, Dash.MaxCharges, Dash.RechargeFraction, PipColor);

			// A number is worth a lot here: dash is the only counterplay to a carrier, so players plan
			// around exactly when it comes back.
			if (Dash.Remaining > TraceHUDStyle::TimeEpsilon)
			{
				const FString CountdownText = FString::Printf(TEXT("%.1f"), Dash.Remaining);
				DrawTextLeft(CountdownText, TraceHUDStyle::InkDim,
					Margin + BarW + (10.f * UIScale),
					VCenterTextY(CountdownText, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);
			}

			// This used to be the last row and so never advanced the cursor. The slide-jump row
			// below is drawn from the same RowY, so without this the two land on top of each other.
			RowY -= (RowH + RowGap);
		}
	}

	// ---- Slide-jump window --------------------------------------------------------------------
	//
	// Spec v4 §1 makes the slide-jump the payoff move and gives it a TIMING WINDOW. A timing window
	// with no feedback is unlearnable: the player presses jump, sometimes gets 110% of their speed
	// back and sometimes gets 100%, and has no way to tell which happened or why. That does not read
	// as a skill they have not learned yet, it reads as an inconsistent game.
	//
	// So the row only exists while a slide-jump is actually available, and it says which of the two
	// states the player is in — armed, or armed AND inside the bonus window. Deliberately a separate
	// row rather than a flash on the dash pips: dash and slide are different resources spent on
	// different keys, and overloading one meter to mean both is how a player learns the wrong thing.
	if (const UTraceCharacterMovementComponent* TraceMove = (LocalChar != nullptr)
			? Cast<UTraceCharacterMovementComponent>(LocalChar->GetCharacterMovement())
			: nullptr)
	{
		if (TraceMove->IsSlideJumpAvailable())
		{
			const bool bWellTimed = TraceMove->IsSlideJumpWellTimed();

			const FString SlideLabel(TEXT("SLIDE"));
			DrawTextLeft(SlideLabel, bWellTimed ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim,
				Margin, VCenterTextY(SlideLabel, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

			// Pulsed while the bonus is live so it reads as a moment to act on, steady-dim while the
			// slide is merely running. The pulse is the same 12 rad/s the SHIELD DOWN callout uses,
			// so "something is happening right now" looks the same everywhere on this HUD.
			const FLinearColor WindowColor = bWellTimed
				? TraceHUDStyle::WithAlpha(TraceHUDStyle::Good, 0.7f + 0.3f * FMath::Sin(Now * 12.f))
				: TraceHUDStyle::Shade(TeamTint, 0.45f, 0.0f);

			DrawMeter(Margin + LabelW, RowY, BarW - LabelW, RowH, bWellTimed ? 1.f : 0.35f, WindowColor);

			const FString WindowText = bWellTimed ? TEXT("JUMP NOW") : TEXT("SLIDING");
			DrawTextLeft(WindowText, TraceHUDStyle::InkDim,
				Margin + BarW + (10.f * UIScale),
				VCenterTextY(WindowText, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

			RowY -= (RowH + RowGap);
		}
	}

	// ---- Activated ability (spec v14 §5) --------------------------------------------------------
	//
	// Drawn LAST of the conditional rows, so it sits highest in the stack and nothing below it moves
	// when it appears — and, critically, it is the only row here that is drawn on a dead player's
	// frame. See DrawAbilityRow().
	RowY = DrawAbilityRow(RowY, Margin, BarW, RowH, LabelW, TeamTint);

	// ---- Health -----------------------------------------------------------------------------
	if (const UTraceHealthComponent* HealthComp = (LocalChar != nullptr) ? LocalChar->Health.Get() : nullptr)
	{
		// The bar, the regen climb and the regen countdown. See DrawHealthBar().
		DrawHealthBar(HealthComp, Margin, HealthY, BarW, HealthH);

		const FString HealthText = FString::Printf(TEXT("%d"), FMath::CeilToInt(HealthComp->Health));
		DrawTextRight(HealthText, TraceHUDStyle::Ink,
			Margin + BarW - (10.f * UIScale),
			VCenterTextY(HealthText, FontMedium, UIScale, HealthY, HealthH), FontMedium, UIScale);

		// Carrying the Core means bullets cannot touch you — the single most important piece of
		// state a player can have, so it gets a callout right on the health bar.
		//
		// ...except during a pass. Spec §4: the moment a pass is input the shield DROPS, and telling
		// the player they are invulnerable at the exact instant they stop being so would be the worst
		// possible lie for this HUD to tell. The pass window has its own callout.
		if (bLocalCarrying)
		{
			const bool bPassing = (TracePC->GetPassProgress() >= 0.f);
			const FString InvulnText = bPassing ? TEXT("SHIELD DOWN") : TEXT("INVULNERABLE");
			const FLinearColor InvulnColor = bPassing
				? TraceHUDStyle::WithAlpha(TraceHUDStyle::Danger, 0.7f + 0.3f * FMath::Sin(Now * 12.f))
				: TraceHUDStyle::Ink;

			DrawTextLeft(InvulnText, InvulnColor,
				Margin + (10.f * UIScale),
				VCenterTextY(InvulnText, FontSmall, UIScale, HealthY, HealthH), FontSmall, UIScale);
		}
	}
}

float ATraceHUD::DrawAbilityRow(float RowY, float Margin, float BarW, float RowH, float LabelW, const FLinearColor& TeamTint)
{
	// FED BY THE PLAYER STATE, NEVER BY THE PAWN. See the header: this row's whole reason to exist is
	// that spec v14 §5 requires the cooldown to be visible on a frame where the pawn does not exist.
	// If this ever starts reading ATraceCharacter or a component on it, the feature is broken again
	// and the symptom will be "the meter disappears when I die", which reads as a HUD bug rather than
	// as the rule violation it is.
	if (LocalPS == nullptr || !LocalPS->HasCharacter())
	{
		// No character means mode A, the settings toggle, a bot, or a player who has not picked yet.
		// In every one of those there is no ability, and a row saying "ABILITY  READY" would be a
		// promise about a key that does nothing.
		return RowY;
	}

	const uint8 CharacterId = LocalPS->GetSelectedCharacter();
	const TraceCharacterRoster::FTraceCharacterEntry* const Entry = TraceCharacterRoster::Find(CharacterId);

	const float Remaining = LocalPS->GetActivatedCooldownRemaining();
	const bool  bReady    = (Remaining <= TraceHUDStyle::TimeEpsilon);

	// The FRACTION is derived from the roster's published cooldown length rather than from a second
	// replicated float. The framework replicates a DEADLINE (an absolute match time), which is the
	// right thing to replicate — it makes a late joiner correct for free — but a meter needs a
	// denominator, and the character's cooldown is a constant the card already prints. Deriving it
	// costs nothing and cannot go stale; storing it would be one more value able to disagree.
	const float CooldownLength = (Entry != nullptr) ? FMath::Max(0.01f, Entry->ActivatedCooldown) : 20.f;
	const float Fraction = FMath::Clamp(1.f - (Remaining / CooldownLength), 0.f, 1.f);

	// THE KEY, read from the player's own bindings rather than hardcoded to "E".
	//
	// Spec v14 §5 says activated abilities bind to E "by default, rebindable", and the binding itself
	// belongs to the input slice, not here. This looks the action up BY ITS STABLE CONFIG ID, so the
	// moment that slice appends an "Ability" action to ETraceInputAction this row starts printing
	// whatever the player actually bound — and until then it prints the documented default. A HUD
	// that hardcodes a key is a HUD that lies to the first player who rebinds it.
	FString KeyLabel(TEXT("E"));
	for (const FTraceInputActionInfo& Info : TraceInputActions::All())
	{
		if (FCString::Stricmp(Info.ConfigId, TEXT("Ability")) == 0)
		{
			const FKey BoundKey = UTraceUserSettings::Get().GetKey(Info.Action);
			if (BoundKey.IsValid())
			{
				KeyLabel = BoundKey.GetDisplayName(/*bLongDisplayName=*/false).ToString().ToUpper();
			}
			break;
		}
	}

	const FString Label = FString::Printf(TEXT("[%s]"), *KeyLabel);
	DrawTextLeft(Label, bReady ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim,
		Margin, VCenterTextY(Label, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

	// Ready pulses in the character's own accent colour; charging is a dim version of it. The accent
	// rather than the team tint, matching the select screen — the player learned the colour there and
	// it is the one thing on this HUD that says WHICH character they are.
	const FLinearColor Accent = (Entry != nullptr) ? Entry->Accent : TeamTint;
	const FLinearColor RowColor = bReady
		? TraceHUDStyle::WithAlpha(Accent, 0.75f + 0.25f * FMath::Sin(Now * 8.f))
		: TraceHUDStyle::Shade(Accent, 0.40f, 0.0f);

	DrawMeter(Margin + LabelW, RowY, BarW - LabelW, RowH, bReady ? 1.f : Fraction, RowColor);

	// The right-hand caption. Three states, and the DEAD one is the reason this pass exists:
	//
	//   ready               -> "<ABILITY NAME>"
	//   cooling, alive      -> "<ABILITY NAME>  12.4"
	//   cooling, DEAD       -> "<ABILITY NAME>  12.4  RUNNING"
	//
	// Spec v14 §5 is explicit that a player "can spawn with an ability timer still counting down".
	// Without the third caption, a dead player watching a number tick down has no way to know whether
	// that is intended or whether the game has forgotten to reset it — and the spec's own note says
	// that reads as a bug. So the HUD says out loud that it is deliberate.
	const FString AbilityName = (Entry != nullptr) ? FString(Entry->ActivatedName) : TraceCharacterRoster::NameFor(CharacterId);

	FString StatusText = AbilityName;
	FLinearColor StatusColor = bReady ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim;

	if (!bReady)
	{
		StatusText = FString::Printf(TEXT("%s  %.1f"), *AbilityName, Remaining);

		if (bLocalDead)
		{
			StatusText += TEXT("  RUNNING");
			StatusColor = TraceHUDStyle::Warning;
		}
	}

	DrawTextLeft(StatusText, StatusColor,
		Margin + BarW + (10.f * UIScale),
		VCenterTextY(StatusText, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

	return RowY - (RowH + (8.f * UIScale));
}

void ATraceHUD::DrawHealthBar(const UTraceHealthComponent* HealthComp, float X, float Y, float W, float H)
{
	if (HealthComp == nullptr)
	{
		return;
	}

	const float Actual = FMath::Clamp(HealthComp->GetHealthPercent(), 0.f, 1.f);

	// ---- Ease the DRAWN width. See DrawnHealthFraction in the header for why this is asymmetric. --
	//
	// Regeneration reaches a client as a replicated float at the actor's net update rate, so the raw
	// value arrives in steps of roughly 0.1 HP to 0.5 HP. Drawing those steps directly makes healing
	// look like packet loss. Easing UP hides the steps; damage still snaps, because a bar that
	// glides down after a body shot is a bar that lies about how close to death the player is.
	ATraceCharacter* HealthPawn = LocalChar.Get();
	const bool bSamePawn = (DrawnHealthPawn.Get() == HealthPawn) && (HealthPawn != nullptr);

	// Clamped: a hitch (or the pause menu, which stops the world clock) must not teleport the ease.
	const float DrawDelta = (bSamePawn && LastHealthDrawTime >= 0.f)
		? FMath::Clamp(Now - LastHealthDrawTime, 0.f, 0.25f)
		: 0.f;
	LastHealthDrawTime = Now;
	DrawnHealthPawn = HealthPawn;

	if (!bSamePawn || DrawnHealthFraction < 0.f || Actual <= DrawnHealthFraction)
	{
		DrawnHealthFraction = Actual;
	}
	else
	{
		DrawnHealthFraction = FMath::FInterpTo(DrawnHealthFraction, Actual, DrawDelta, 9.f);
	}

	const float Fraction = FMath::Clamp(DrawnHealthFraction, 0.f, 1.f);
	const FLinearColor BarColor = TraceHUDStyle::LerpColor(TraceHUDStyle::Danger, TraceHUDStyle::Good, Fraction);
	DrawMeter(X, Y, W, H, Fraction, BarColor);

	// ---- Regeneration (spec v13 §1) -------------------------------------------------------------
	//
	// Negative is "nothing to say": at full health, dead, or the mechanic switched off. The bar goes
	// back to being exactly what it was before this pass, which is what it should be at 100/100.
	const float SecondsUntil = HealthComp->GetSecondsUntilRegen();
	if (SecondsUntil < 0.f)
	{
		return;
	}

	const bool bRegenerating = HealthComp->IsRegenerating();
	const float Pulse = 0.5f + 0.5f * FMath::Sin(Now * 7.f);
	const float FillW = Fraction * W;

	// A fuse UNDERNEATH the bar, filling left to right over the delay and solid while regeneration
	// runs.
	//
	// THE FUSE IS THE TEACHING, not the climb. The climb only tells a player that healing exists;
	// the fuse tells them how long they have to stay out of sight for it to start, which is the
	// behaviour the spec actually wants out of this feature. It hangs off the health bar rather than
	// taking a row in the ability stack because every row up there is conditional and shuffles as
	// parry and slide come and go — this has to be in the one place that never moves.
	//
	// *** IT IS BELOW THE BAR BECAUSE ON THE BAR IT WAS INVISIBLE. *** The first version drew it on
	// the bar's own bottom edge, and the screenshot of a player at 60 HP with 4.0 s to go showed
	// nothing at all: the fill at 60% is already a yellow-green, the fuse was green, and a green
	// sliver inside a green fill is not a readout. Its own strip on its own dark trough is the only
	// arrangement that reads at every health level, which is the point — the countdown matters most
	// when you are nearly dead, i.e. exactly when the fill behind it would be red.
	const float FuseH = FMath::Max(3.f, 4.f * UIScale);
	const float FuseY = Y + H + FMath::Max(2.f, 3.f * UIScale);
	const float Delay = FMath::Max(0.01f, TraceHealthRegen::GetDelaySeconds());
	const float FuseFraction = bRegenerating ? 1.f : FMath::Clamp(1.f - (SecondsUntil / Delay), 0.f, 1.f);

	const float FuseEdge = FMath::Max(1.f, 1.f * UIScale);
	DrawRect(TraceHUDStyle::Shadow, X - FuseEdge, FuseY - FuseEdge, W + FuseEdge * 2.f, FuseH + FuseEdge * 2.f);
	DrawRect(TraceHUDStyle::Trough, X, FuseY, W, FuseH);
	if (FuseFraction > 0.f)
	{
		DrawRect(TraceHUDStyle::WithAlpha(TraceHUDStyle::Good, bRegenerating ? (0.6f + 0.4f * Pulse) : 0.85f),
			X, FuseY, W * FuseFraction, FuseH);
	}

	if (bRegenerating)
	{
		// A bright crest riding the leading edge of the fill, plus a lit top rail across it. Between
		// them they make the bar read as MOVING at a rate the eye can see, which the underlying 10
		// HP/s (0.1 of the bar per second) genuinely does not on its own.
		//
		// Lifted most of the way to Ink rather than left as Good, for the same contrast reason the
		// fuse moved: at high health the fill IS Good, and a Good-coloured crest on it disappears
		// precisely as the player approaches the top of the bar.
		const FLinearColor Crest = TraceHUDStyle::LerpColor(TraceHUDStyle::Good, TraceHUDStyle::Ink, 0.7f);

		const float CrestW = FMath::Min(FMath::Max(4.f, 7.f * UIScale), FMath::Max(0.f, W - FillW) + (7.f * UIScale));
		const float CrestX = FMath::Clamp(X + FillW - (CrestW * 0.5f), X, X + W - CrestW);
		DrawRect(TraceHUDStyle::WithAlpha(Crest, 0.45f + 0.5f * Pulse), CrestX, Y, CrestW, H);

		const float RailH = FMath::Max(1.f, 2.f * UIScale);
		DrawRect(TraceHUDStyle::WithAlpha(Crest, 0.25f + 0.35f * Pulse), X, Y, FillW, RailH);
	}

	// ---- The words, to the right of the bar, where every other row in this stack puts its status --
	//
	// Spelled out rather than left to the colour, because "break line of sight and you heal" is a
	// RULE the player has to be taught once, and a green shimmer teaches nobody anything. The rate
	// is printed with it so the player can judge whether waiting is worth it against pushing.
	const FString RegenText = bRegenerating
		? FString::Printf(TEXT("REGEN  +%.0f/s"), TraceHealthRegen::GetRatePerSecond())
		: FString::Printf(TEXT("REGEN IN  %.1f"), SecondsUntil);

	const FLinearColor RegenColor = bRegenerating
		? TraceHUDStyle::WithAlpha(TraceHUDStyle::Good, 0.7f + 0.3f * Pulse)
		: TraceHUDStyle::InkDim;

	DrawTextLeft(RegenText, RegenColor,
		X + W + (10.f * UIScale),
		VCenterTextY(RegenText, FontSmall, UIScale, Y, H), FontSmall, UIScale);
}

void ATraceHUD::DrawChargePips(float X, float Y, float W, float H, int32 Charges, int32 MaxCharges,
	float PartialFraction, const FLinearColor& FillColor)
{
	const int32 Count = FMath::Max(1, MaxCharges);
	const float Gap = (Count > 1) ? (6.f * UIScale) : 0.f;
	const float PipW = FMath::Max(4.f, (W - Gap * (Count - 1)) / Count);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float PipX = X + Index * (PipW + Gap);

		// Fraction for THIS pip: banked charges are full, the next one shows its recharge progress,
		// and everything beyond it is empty. Left to right, so the row fills the way a bar would.
		float Fraction = 0.f;
		if (Index < Charges)
		{
			Fraction = 1.f;
		}
		else if (Index == Charges)
		{
			Fraction = FMath::Clamp(PartialFraction, 0.f, 1.f);
		}

		DrawMeter(PipX, Y, PipW, H, Fraction, FillColor);
	}
}

// -------------------------------------------------------------------------------------------
// Bottom right: ammo in the corner, statuses stacking above it — spec v16 §2
//
// The layout decision and the "separate from cooldowns" reasoning are on DrawAmmoAndStatuses() in
// the header. This block is only the palette.
// -------------------------------------------------------------------------------------------

namespace TraceHUDStatusStyle
{
	/**
	 * SIX SEPARATED HUES, AND DELIBERATELY *NOT* THE CHARACTER ACCENT COLOURS.
	 *
	 * The accents already mean something on this HUD: the ability row wears them because they say
	 * WHICH CHARACTER YOU ARE, learned on the select screen. A status says what is HAPPENING TO YOU,
	 * which is a different question, and it is frequently not your own character's doing at all — the
	 * poison on a Chut came from an Oyster. Reusing Chut's mint for Chud would also have put two
	 * greens on a poisoned Chut's screen at once, which is the exact collision this palette exists to
	 * avoid. So statuses get their own wheel, spread far enough apart to survive a small window.
	 */
	static const FLinearColor SpeedBoost (0.30f, 0.95f, 0.95f, 1.f);   // electric cyan
	static const FLinearColor Poisoned   (0.35f, 0.95f, 0.20f, 1.f);   // toxic green, the cloud's own
	static const FLinearColor Slowed     (0.35f, 0.55f, 1.00f, 1.f);   // cold blue
	static const FLinearColor Vulnerable (1.00f, 0.35f, 0.45f, 1.f);   // rose — X's mark
	static const FLinearColor Chud       (1.00f, 0.80f, 0.30f, 1.f);   // armour gold
	static const FLinearColor Suspend    (0.72f, 0.55f, 1.00f, 1.f);   // violet — Mace
	static const FLinearColor Pull       (0.85f, 0.72f, 1.00f, 1.f);   // the same family: one ability

	/**
	 * BEE AMBER — the one colour in this file whose whole job is to stop a misreading.
	 *
	 * Spec v16 §1: Sting "reloads the clip with just the 5 bee bullets", REPLACING whatever was in it.
	 * A player holding 25 rounds who presses E and sees "5" has, from their side of the screen,
	 * watched the gun eat twenty of their bullets. The spec's own [ASSUMPTION] says the HUD must make
	 * it obvious, so a bee clip changes THREE independent things at once — this colour, the SHAPE of
	 * the magazine strip (five fat pips instead of thirty thin ticks) and the words "BEE ROUNDS".
	 * Any one of the three survives a compressed screenshot, a colour-blind player or a glance.
	 *
	 * Amber-black is what a bee is; it is deliberately not X's rose accent, which would say "this is
	 * X's gun" rather than "these are not normal bullets".
	 */
	static const FLinearColor BeeRounds  (1.00f, 0.78f, 0.10f, 1.f);

	/** Ordinary rounds wear the ammo block's own neutral, so the bee clip is the only loud state. */
	static const FLinearColor NormalRounds(0.90f, 0.93f, 1.00f, 1.f);

	/** Reload gold: the gun is unavailable, which is the same news the empty clip was. */
	static const FLinearColor Reloading  (1.00f, 0.62f, 0.20f, 1.f);

	/** Below this fraction of a clip the count turns red. Two shots' worth at the shipped 30. */
	static constexpr float LowAmmoFraction = 0.2f;
}

void ATraceHUD::DrawAmmoAndStatuses()
{
	// Told DrawHUD that the corner had its say this frame, so the tail of DrawHUD does not collapse
	// a UMG corner that is legitimately up. See bCornerAddressedThisDraw.
	bCornerAddressedThisDraw = true;

	FTraceHudCornerState CornerState;
	const bool bCornerLive = BuildCornerState(CornerState);

	// *** THE ONE DELIBERATE DIFFERENCE BETWEEN THE TWO PRESENTERS, AND IT IS A PORT OF THE INTENT.
	// *** The pause menu and the character select screen each lay a near-opaque full-screen scrim
	// over everything (alpha 0.88 and 0.94), and the Canvas corner is drawn UNDER it — so a player
	// with the pause menu up cannot see their ammo. A UMG widget is a Slate widget: it paints OVER
	// the HUD canvas, scrim included. Hiding it while either overlay owns the screen is what makes
	// the two paths look the same; leaving it up would be a change in what the player sees, which
	// spec v17 §0 calls a bug rather than a feature.
	const bool bOverlayOwnsScreen = PauseMenu.IsOpen() || CharacterSelect.IsOpen();

	if (PresentCornerUmg(bCornerLive && !bOverlayOwnsScreen, CornerState))
	{
		return;
	}

	// Canvas: the shipped path, and the live fallback whenever the widget is unavailable.
	HideCornerWidget();
	if (bCornerLive)
	{
		PresentCornerCanvas(CornerState);
	}
}

bool ATraceHUD::BuildCornerState(FTraceHudCornerState& OutState) const
{
	// The red arm (Trace.HUD.V16 0) is the pre-v16 corner: empty. See TraceHUDV16.
	if (!TraceHUDV16::IsArmed())
	{
		return false;
	}

	// A dead player has no gun and no live effects — poison dies with the body and the mark is
	// cleared on respawn — so the whole corner goes quiet rather than each block testing separately.
	if (LocalChar == nullptr || !LocalChar->IsAlive())
	{
		return false;
	}

	// ---- The ammo block ---------------------------------------------------------------------------
	//
	// *** ONE GATE, AND IT IS THE GUN'S OWN. *** ShouldShowAmmo() is false while carrying the Core
	// (spec v16 §1: "The Core carrier has no gun, so ammo must not be consumed or shown while
	// carrying"), with the knife out, and while dead. Re-deriving any of that here would give the
	// rule a second definition able to drift from the one the weapon enforces — which is how a HUD
	// ends up showing a count for a gun that cannot fire.
	const UTraceWeaponComponent* WeaponComp = LocalChar->Weapon.Get();
	if (WeaponComp != nullptr && WeaponComp->ShouldShowAmmo())
	{
		OutState.bAmmoBlock = true;
		OutState.InClip = WeaponComp->GetClipAmmo();

		const int32 AbilityRounds = WeaponComp->GetAbilityRoundsInClip();
		OutState.bBeeClip = (AbilityRounds > 0);
		OutState.bReloading = WeaponComp->IsReloading();

		// THE DENOMINATOR IS THE CLIP THAT IS ACTUALLY LOADED. A bee clip holds five, and printing
		// "3/30" for it would say the gun is nearly empty when it is more than half full of the thing
		// X just spent his ability on.
		//
		// *** IT IS THE SIZE THE BEE CLIP WAS LOADED AT, NOT THE ROUNDS STILL IN IT. *** This was
		// written as Max(InClip, AbilityRounds), and UTraceWeaponComponent::ConsumeRound decrements
		// AbilityRoundsInClip in lockstep with ClipAmmo — so the denominator tracked the numerator and
		// the block read "5/5", then "4/4", then "3/3", while the pip strip below LOST a pip per shot
		// instead of dimming one. A bee clip looked full right up until it was gone, which is the exact
		// thing this branch exists to prevent. The capacity now comes from the same knob that produced
		// the clip — XStingBulletCount, which is what UTraceAbilitySetX::ActivateAbility hands to
		// LoadAbilityClip — for the same reason §3's cloud reads the poison's own radius rather than a
		// second copy of it: two numbers for one fact drift, and the HUD is always the copy that lies.
		// Max() against the loaded count so a future ability that loads MORE rounds than X does can
		// never produce a denominator below its own numerator.
		OutState.ClipCapacity = OutState.bBeeClip
			? FMath::Max(AbilityRounds, TraceXBees::GetStingBulletCount())
			: FMath::Max(1, WeaponComp->GetClipSize());

		OutState.RoundsColor = OutState.bBeeClip
			? TraceHUDStatusStyle::BeeRounds
			: TraceHUDStatusStyle::NormalRounds;

		OutState.CapacityText = FString::Printf(TEXT("/%d"), OutState.ClipCapacity);
		OutState.CountText = OutState.bReloading ? FString(TEXT("--")) : FString::FromInt(OutState.InClip);

		OutState.bLowAmmo = !OutState.bBeeClip && !OutState.bReloading
			&& (static_cast<float>(OutState.InClip)
				<= TraceHUDStatusStyle::LowAmmoFraction * OutState.ClipCapacity);

		OutState.CountColor = OutState.bReloading
			? TraceHUDStatusStyle::Reloading
			: (OutState.bLowAmmo ? TraceHUDStyle::Danger : OutState.RoundsColor);

		if (OutState.bReloading)
		{
			const float ReloadTotal = FMath::Max(TraceHUDStyle::TimeEpsilon, TraceAmmo::GetReloadSeconds());
			OutState.ReloadRemaining = WeaponComp->GetReloadRemaining();
			OutState.ReloadFraction =
				FMath::Clamp(ReloadTotal - OutState.ReloadRemaining, 0.f, ReloadTotal) / ReloadTotal;
		}

		// The WORDS are the third independent bee-round signal, and the right-hand half is the reload
		// key — read from the player's own bindings rather than hardcoded to R, exactly as the ability
		// row does. A HUD that hardcodes a key is a HUD that lies to the first player who rebinds it.
		OutState.AmmoLabel = OutState.bBeeClip ? FString(TEXT("BEE ROUNDS")) : FString(TEXT("AMMO"));
		OutState.AmmoLabelColor = OutState.bBeeClip
			? TraceHUDStatusStyle::BeeRounds
			: TraceHUDStyle::InkDim;

		FString ReloadKeyLabel(TEXT("R"));
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			if (FCString::Stricmp(Info.ConfigId, TEXT("Reload")) == 0)
			{
				const FKey BoundKey = UTraceUserSettings::Get().GetKey(Info.Action);
				if (BoundKey.IsValid())
				{
					ReloadKeyLabel = BoundKey.GetDisplayName(/*bLongDisplayName=*/false).ToString().ToUpper();
				}
				break;
			}
		}

		OutState.RightLabel = OutState.bReloading
			? FString::Printf(TEXT("RELOADING  %.1f"), OutState.ReloadRemaining)
			: FString::Printf(TEXT("[%s]  RELOAD"), *ReloadKeyLabel);

		OutState.RightLabelColor = OutState.bReloading
			? TraceHUDStatusStyle::Reloading
			: TraceHUDStyle::InkDim;

		OutState.ReloadBarColor = TraceHUDStatusStyle::Reloading;
	}

	// ---- The status stack, growing upward -------------------------------------------------------
	//
	// ORDER IS FIXED AND IT IS A DECISION. Nearest the corner (first in this array, drawn lowest) are
	// the things being done TO the player, most urgent first: vulnerable, then the two Oyster
	// debuffs, then Mace's control. The player's own buffs sit above them. A status appearing or
	// expiring therefore only ever shuffles statuses ABOVE it and can never move the ammo count.

	// ---- VULNERABLE (spec v16 §4 — "The HUD status from §2 shows the stack count") ---------------
	//
	// GetVulnerableStacks() is the ONE accessor for this and it already answers the two questions the
	// HUD would otherwise get wrong: it returns 0 the instant the deadline passes (all stacks vanish
	// together, which is why this never draws a draining count) and 0 for a Core carrier, who cannot
	// be marked at all. Deriving either here would be a second copy of a rule.
	if (const UTraceHealthComponent* HealthComp = LocalChar->Health.Get())
	{
		const int32 Stacks = HealthComp->GetVulnerableStacks();
		if (Stacks > 0)
		{
			const float Remaining = HealthComp->GetVulnerableRemaining();
			const float Total = FMath::Max(TraceHUDStyle::TimeEpsilon, TraceVulnerable::GetDurationSeconds());

			// The BONUS, printed from the published pure function rather than re-derived from the two
			// knobs — that is how a HUD and a damage path end up agreeing on a shared mistake.
			const float Bonus = 100.f * (TraceVulnerable::GetMultiplierForStacks(Stacks) - 1.f);

			OutState.Chips.Add({
				FString::Printf(TEXT("VULNERABLE  x%d  +%.0f%%"), Stacks, Bonus),
				FString::Printf(TEXT("%.1fs"), Remaining),
				Remaining / Total, TraceHUDStatusStyle::Vulnerable });
		}
	}

	// ---- POISONED and SLOWED (Oyster) ------------------------------------------------------------
	//
	// TWO CHIPS OFF ONE COMPONENT, AND THAT IS THE INFORMATION, not duplication. The poison keeps
	// ticking on a player who picks the Core up, but the §4 choke point switches the SLOW off the
	// same frame — so a carrier sees POISONED without SLOWED, and the pair is the only thing on
	// screen that says the difference out loud. bSlowActive is the server's own per-frame answer,
	// replicated, so this cannot disagree with the speed the player is actually moving at.
	if (const UTraceOysterPoisonComponent* PoisonComp = UTraceOysterPoisonComponent::Find(LocalChar))
	{
		// The poison replicates an ABSOLUTE match time, which is what makes it correct for a client
		// that joined late. GetServerWorldTimeSeconds() is the same clock it was written against.
		const float MatchNow = (TraceGS != nullptr)
			? static_cast<float>(TraceGS->GetServerWorldTimeSeconds()) : 0.f;
		const float Remaining = PoisonComp->GetEndMatchTime() - MatchNow;
		const float Total = FMath::Max(TraceHUDStyle::TimeEpsilon,
			UTraceSettings::Get().OysterPoisonDurationSeconds);

		if (Remaining > 0.f)
		{
			OutState.Chips.Add({
				TEXT("POISONED"), FString::Printf(TEXT("%.1fs"), Remaining),
				Remaining / Total, TraceHUDStatusStyle::Poisoned });

			if (PoisonComp->IsSlowActive())
			{
				const float SlowPercent = 100.f * (1.f - PoisonComp->GetSpeedMultiplier());
				OutState.Chips.Add({
					FString::Printf(TEXT("SLOWED  -%.0f%% SPEED"), SlowPercent),
					FString::Printf(TEXT("%.1fs"), Remaining),
					Remaining / Total, TraceHUDStatusStyle::Slowed });
			}
		}
	}

	// ---- Mace's suspend and pull, and Chut's Chud, and Rocco's speed stack ------------------------
	//
	// All four hang off the local player's OWN ability set, so one lookup serves them and a character
	// who is none of the three simply matches no branch.
	if (const UTraceCharacterAbilitySet* LocalSet = UTraceAbilityComponent::GetAbilitySetFor(LocalChar))
	{
		if (const UTraceAbilitySetMace* MaceSet = Cast<UTraceAbilitySetMace>(LocalSet))
		{
			if (MaceSet->IsSuspending())
			{
				const float Remaining = MaceSet->GetSuspendRemaining();
				const float Total = FMath::Max(TraceHUDStyle::TimeEpsilon,
					UTraceSettings::Get().MaceSuspendMaxSeconds);

				OutState.Chips.Add({
					TEXT("SUSPENDED"), FString::Printf(TEXT("%.2fs"), Remaining),
					Remaining / Total, TraceHUDStatusStyle::Suspend });
			}

			// THE PULL HAS NO CLOCK, so its draining indicator is DISTANCE — which is the pull's real
			// progress and the only honest thing to drain here. Spec v16 §2 asks for "a duration
			// readout OR a draining indicator"; inventing a fake timer to satisfy the first would
			// have been a HUD that lies about a mechanic that ends on arrival, not on a stopwatch.
			if (MaceSet->IsPulling())
			{
				const float ToAnchor = static_cast<float>(
					FVector::Dist(LocalChar->GetActorLocation(), MaceSet->GetSpikeAnchorLocation()));
				const float Range = FMath::Max(1.f, UTraceSettings::Get().MaceSpikeRangeUU);

				OutState.Chips.Add({
					TEXT("SPIKE PULL"), FString::Printf(TEXT("%.0fm"), ToAnchor / 100.f),
					FMath::Clamp(ToAnchor / Range, 0.f, 1.f), TraceHUDStatusStyle::Pull });
			}
		}
		else if (const UTraceAbilitySetChut* ChutSet = Cast<UTraceAbilitySetChut>(LocalSet))
		{
			if (ChutSet->IsChudActive())
			{
				const float Remaining = ChutSet->GetChudSecondsRemaining();
				const float Total = FMath::Max(TraceHUDStyle::TimeEpsilon,
					UTraceSettings::Get().ChudDurationSeconds);
				const float Reduction = 100.f * UTraceSettings::Get().ChudDamageReduction;

				OutState.Chips.Add({
					FString::Printf(TEXT("CHUD  -%.0f%% DAMAGE"), Reduction),
					FString::Printf(TEXT("%.1fs"), Remaining),
					Remaining / Total, TraceHUDStatusStyle::Chud });
			}
		}
		else if (const UTraceAbilitySetRocco* RoccoSet = Cast<UTraceAbilitySetRocco>(LocalSet))
		{
			// ONE timer over the WHOLE stack, refreshed by every headshot kill — not one timer per
			// stack (see TraceAbilitySetRocco.h). So there is exactly one bar to drain, and the stack
			// count belongs in the label beside it rather than as N separate chips.
			const int32 Stacks = RoccoSet->GetLiveStackCount();
			if (Stacks > 0)
			{
				const float Remaining = RoccoSet->GetStackSecondsRemaining();
				const float Total = FMath::Max(TraceHUDStyle::TimeEpsilon,
					UTraceSettings::Get().RoccoHeadshotSpeedDurationSeconds);

				// The multiplier the movement component is ACTUALLY using, not stacks x the per-stack
				// knob: the set clamps at RoccoHeadshotSpeedStackMax and this way the readout keeps
				// telling the truth at the cap instead of counting past it.
				const float BoostPercent = 100.f * (RoccoSet->GetMoveSpeedMultiplier() - 1.f);

				OutState.Chips.Add({
					FString::Printf(TEXT("SPEED BOOST  x%d  +%.0f%%"), Stacks, BoostPercent),
					FString::Printf(TEXT("%.1fs"), Remaining),
					Remaining / Total, TraceHUDStatusStyle::SpeedBoost });
			}
		}
	}

	return true;
}

void ATraceHUD::PresentCornerCanvas(const FTraceHudCornerState& InState)
{
	// Mirrors the bottom-left stack's margin exactly, so the two corners read as one system rather
	// than as two features that landed in different passes.
	const float Margin = TraceHudCornerLayout::StackMarginDesignPx * UIScale;
	const float BlockW = 260.f * UIScale;
	const float RightEdge = ViewW - Margin;

	// Ammo first and pinned: it owns the corner and never moves. See the header.
	const float StackBottom = DrawAmmoBlock(InState, RightEdge, ViewH - Margin, BlockW);

	// The stack grows upward from there, in the order BuildCornerState filled it: index 0 nearest
	// the corner. Twelve pixels of clearance above the ammo block's own top edge.
	float ChipBottom = StackBottom - (12.f * UIScale);
	for (const FTraceHudCornerChip& Chip : InState.Chips)
	{
		ChipBottom = DrawStatusChip(RightEdge, ChipBottom, BlockW,
			Chip.Label, Chip.Readout, Chip.Fraction, Chip.Tint);
	}
}

float ATraceHUD::DrawAmmoBlock(const FTraceHudCornerState& InState, float RightX, float BottomY, float BlockW)
{
	// The gate lives in BuildCornerState now — UTraceWeaponComponent::ShouldShowAmmo(), asked once
	// for both presenters. Returns @p BottomY unchanged when there is nothing to draw, exactly as it
	// always has, so the status stack starts from the corner instead of from a phantom plate.
	if (!InState.bAmmoBlock)
	{
		return BottomY;
	}

	// Local aliases so the drawing below is character-for-character what it was before the state was
	// split out. Deliberately verbatim: this is a REORGANISATION (spec v17 §0), and every one of
	// these numbers was arrived at by looking at a capture.
	const int32 InClip = InState.InClip;
	const int32 ClipCapacity = InState.ClipCapacity;
	const bool  bBeeClip = InState.bBeeClip;
	const bool  bReloading = InState.bReloading;
	const FLinearColor RoundsColor = InState.RoundsColor;

	// ---- Geometry, laid out upward from the corner ------------------------------------------------
	// THE COUNT IS DRAWN AT DOUBLE THE BODY SCALE, and that is a measurement rather than a taste.
	// At 1280x720 (UIScale 0.667) the engine's large font renders about 14 px tall, which the first
	// capture showed sitting no larger than the "AMMO" label beside it — for the one number on this
	// HUD a player reads without looking directly at it. 2.0x puts it at ~28 px, comfortably the
	// biggest thing in the corner, and it still clears the status stack above.
	constexpr float CountScale = 2.0f;
	const float NumberH = 40.f * UIScale;
	const float StripH  = 12.f * UIScale;
	const float LabelH  = 14.f * UIScale;
	const float Gap     = 5.f * UIScale;

	const float NumberTop = BottomY - NumberH;
	const float StripTop  = NumberTop - Gap - StripH;
	const float LabelTop  = StripTop - Gap - LabelH;

	// ---- The plate -------------------------------------------------------------------------------
	//
	// A PANEL BEHIND THE WHOLE BLOCK, and it is not decoration. This arena is emissive neon at Glow
	// 3.5 with a bloom pass to match, and the first armed capture put the white "26" straight over a
	// blown-out light: it was almost unreadable, while the status chips a few pixels above it — which
	// already had a panel — read perfectly. The one number a player checks without looking cannot be
	// allowed to depend on what happens to be behind it.
	//
	// The border is tinted by the ROUNDS colour, so a bee clip changes the plate as well as its
	// contents and the whole corner announces itself.
	const float PlatePad = 6.f * UIScale;
	DrawPanel(RightX - BlockW - PlatePad, LabelTop - PlatePad,
		BlockW + (PlatePad * 2.f), (BottomY - LabelTop) + (PlatePad * 2.f),
		TraceHUDStyle::PanelFill, TraceHUDStyle::WithAlpha(RoundsColor, bBeeClip ? 0.55f : 0.30f));

	// ---- The count -------------------------------------------------------------------------------
	//
	// Right-aligned as two pieces so the number a player actually reads is large and the capacity it
	// is out of is subordinate — "7" has to be legible in peripheral vision; "/30" never does.
	const FString& CapacityText = InState.CapacityText;
	const FString& CountText = InState.CountText;
	const FLinearColor CountColor = InState.CountColor;

	// The capacity is BASELINE-ALIGNED to the bottom of the count rather than centred on it, so
	// "26" and "/30" sit on one line instead of stepping down the way the first capture showed.
	const float CountH = MeasureHeight(CountText, FontLarge, UIScale * CountScale);
	const float CountTop = NumberTop + FMath::Max(0.f, NumberH - CountH);
	const float CapacityH = MeasureHeight(CapacityText, FontSmall, UIScale);

	DrawTextRight(CapacityText, TraceHUDStyle::InkDim, RightX,
		CountTop + FMath::Max(0.f, CountH - CapacityH) - (2.f * UIScale), FontSmall, UIScale);

	const float CapacityW = MeasureWidth(CapacityText, FontSmall, UIScale);
	DrawTextRight(CountText, CountColor, RightX - CapacityW - (4.f * UIScale),
		CountTop, FontLarge, UIScale * CountScale);

	// ---- The magazine strip -----------------------------------------------------------------------
	//
	// ONE TICK PER ROUND, and the tick COUNT is what makes a bee clip unmistakable at a glance: five
	// fat pips is a visibly different object from thirty thin ones, before any colour or word is
	// read. Mid-reload the strip becomes a single filling bar instead — a third shape, so "the gun is
	// coming back" never has to be inferred from a number that is briefly meaningless.
	DrawRect(TraceHUDStyle::Trough, RightX - BlockW, StripTop, BlockW, StripH);

	int32 LitTicks = 0;
	if (bReloading)
	{
		DrawRect(InState.ReloadBarColor, RightX - BlockW, StripTop,
			BlockW * FMath::Clamp(InState.ReloadFraction, 0.f, 1.f), StripH);
	}
	else
	{
		// Integer widths, pixel-snapped, for the same reason the crosshair and the kill-feed glyphs
		// use them: a fractional rect on a half-pixel boundary is anti-aliased into a smudge, and at
		// 1280x720 a 30-tick strip has under five pixels per tick to work with.
		const float TickGap = FMath::Max(1.f, FMath::RoundToFloat(1.5f * UIScale));
		const float TickW = FMath::Max(2.f,
			FMath::FloorToFloat((BlockW - (ClipCapacity - 1) * TickGap) / ClipCapacity));
		const float StrideW = TickW + TickGap;
		const float StripLeft = RightX - ((ClipCapacity * StrideW) - TickGap);

		for (int32 Index = 0; Index < ClipCapacity; ++Index)
		{
			const bool bLit = (Index < InClip);
			DrawRect(bLit ? RoundsColor : TraceHUDStyle::WithAlpha(RoundsColor, 0.14f),
				StripLeft + (Index * StrideW), StripTop, TickW, StripH);
			LitTicks += bLit ? 1 : 0;
		}
	}

	// ---- The label line ---------------------------------------------------------------------------
	//
	// The WORDS are the third independent bee-round signal, and the right-hand half is the reload
	// key — read from the player's own bindings rather than hardcoded to R, exactly as the ability
	// row does. A HUD that hardcodes a key is a HUD that lies to the first player who rebinds it.
	// (Both strings, and both colours, are resolved once in BuildCornerState so the UMG corner says
	// the identical words with the identical key.)
	DrawTextLeft(InState.AmmoLabel, InState.AmmoLabelColor, RightX - BlockW, LabelTop, FontSmall, UIScale);
	DrawTextRight(InState.RightLabel, InState.RightLabelColor, RightX, LabelTop, FontSmall, UIScale);

#if !UE_BUILD_SHIPPING
	bDrewAmmoBlock = true;
	bDrewBeeClip = bBeeClip;
	bDrewReloadBar = bReloading;
	DrawnMagazineTicks = LitTicks;
	DrawnAmmoText = FString::Printf(TEXT("%s%s"), *CountText, *CapacityText);
#endif

	return LabelTop;
}

float ATraceHUD::DrawStatusChip(float RightX, float BottomY, float ChipW, const FString& Label,
	const FString& Readout, float Fraction, const FLinearColor& Tint)
{
	const float ChipH = 24.f * UIScale;
	const float ChipTop = BottomY - ChipH;
	const float ChipLeft = RightX - ChipW;
	const float TabW = FMath::Max(2.f, FMath::RoundToFloat(4.f * UIScale));
	const float DrainH = FMath::Max(2.f, FMath::RoundToFloat(3.f * UIScale));

	// Panel, then a saturated tab down the left edge. The tab is what carries the colour at a glance;
	// the fill stays dark so six chips stacked up never turn the corner into a light box.
	DrawPanel(ChipLeft, ChipTop, ChipW, ChipH,
		TraceHUDStyle::PanelFill, TraceHUDStyle::WithAlpha(Tint, 0.45f));
	DrawRect(Tint, ChipLeft, ChipTop, TabW, ChipH);

	const float TextTop = VCenterTextY(Label, FontSmall, UIScale, ChipTop, ChipH - DrainH);
	DrawTextLeft(Label, TraceHUDStyle::Ink, ChipLeft + TabW + (6.f * UIScale), TextTop, FontSmall, UIScale);
	DrawTextRight(Readout, TraceHUDStyle::WithAlpha(Tint, 0.95f), RightX - (6.f * UIScale),
		TextTop, FontSmall, UIScale);

	// *** THE DRAINING INDICATOR, AND IT DRAINS — the opposite direction to every cooldown meter in
	// the bottom-left stack, which FILLS toward ready. That is the second half of spec v16 §2's
	// "separate from cooldowns": the corner separates them in space, this separates them in motion,
	// so the two never read as the same widget out of the corner of an eye. It also rules out the
	// thing the spec explicitly forbids — "a bare icon that never changes is not a status display".
	const float DrainW = ChipW * FMath::Clamp(Fraction, 0.f, 1.f);
	DrawRect(TraceHUDStyle::WithAlpha(TraceHUDStyle::Shadow, 0.5f),
		ChipLeft, ChipTop + ChipH - DrainH, ChipW, DrainH);
	DrawRect(Tint, ChipLeft, ChipTop + ChipH - DrainH, DrainW, DrainH);

#if !UE_BUILD_SHIPPING
	// The chip records the LABEL AND THE FRACTION IT ACTUALLY DREW, not the state it was handed —
	// so a chip that drew a zero-width drain is distinguishable in the report from one that drew a
	// live one, and neither can be inferred from the gameplay state that fed it.
	DrawnStatusChips.Add(FString::Printf(TEXT("%s | %s | drain=%.2f"), *Label, *Readout,
		FMath::Clamp(Fraction, 0.f, 1.f)));
#endif

	return ChipTop - (6.f * UIScale);
}

// ===================================================================================================
// SPEC v17 §4, STEP 4b — THE UMG PRESENTER FOR THE BOTTOM-RIGHT CORNER
//
// Everything above this line is the Canvas corner, unchanged in what it puts on screen. Everything
// below is the second presenter and the toggle that chooses between them.
//
// ---------------------------------------------------------------------------------------------------
// THE TOGGLE IS SHARED, AND THIS FILE DOES NOT OWN IT
// ---------------------------------------------------------------------------------------------------
// `Trace.UI.UseUMG` is one switch for the whole of spec v17 §4 — the menus and this corner were
// converted by two different agents working at the same time, and TWO FAutoConsoleVariableRef
// registrations of one name is an ensure() at static-init time, i.e. a crash on launch for a
// coordination mistake. So this file FINDS the variable and only registers it if nobody else has,
// lazily, on the first frame that asks — by which time every static initialiser in the module has
// long since run. Whichever file gets there first wins, and both then read the same object.
//
// `Trace.UI.HUD.UseUMG` is the corner's own override and IS owned here: -1 (default) follows the
// shared switch, 0 forces Canvas, 1 forces UMG. It exists so this element can be A/B'd without
// disturbing the menus, and so a corner-specific problem can be turned off by somebody who is not
// prepared to turn the whole of §4 off.
// ===================================================================================================

namespace TraceHudCornerUmg
{
	/**
	 * Default for the SHARED switch, applied ONLY if this file is the one that registers it.
	 *
	 * As things stand it never is: TraceMenuHUD.cpp registers `Trace.UI.UseUMG` with an
	 * FAutoConsoleVariableRef at static-init time, long before the first frame draws, and this
	 * file finds it. The value here matters only if the menus' conversion is ever backed out — and
	 * it agrees with theirs, so the answer does not depend on which file wins the race.
	 *
	 * 1, and the measurement is in the step's report: the spec v16 §2 evidence harness
	 * (Trace.HUD.V16Shots) was run against the same fixture with the corner on Canvas and on UMG and
	 * produced the same verdict — 27 passed, 0 failed, both times — and the same draw record: the
	 * same ammo string, the same lit-tick count, the same chips in the same order with the same
	 * drains. The photographs of the two agree to within a few pixels at 720p (the magazine strip
	 * lands on exactly the same scanlines). Spec v17 §4 asks for the default to be "whichever you can
	 * prove is at least as good"; that is the proof, and the Canvas path is still one cvar away.
	 */
	static constexpr int32 SharedToggleDefault = 1;

	/** Z-order of the corner on the player's screen. Above nothing in particular — it is the only widget. */
	static constexpr int32 CornerZOrder = 10;

	/**
	 * The shared switch, found or (once, lazily) registered. Never a second FAutoConsoleVariableRef.
	 *
	 * The pointer is cached because IConsoleManager::FindConsoleVariable is a lock plus a map lookup
	 * and this is asked once per drawn frame. A console variable object lives for the life of the
	 * process, so caching the pointer is safe; caching the VALUE would not be, and is exactly what
	 * would stop `Trace.UI.UseUMG 0` from taking effect until the next map load.
	 */
	static IConsoleVariable* SharedToggle()
	{
		static IConsoleVariable* CachedShared = nullptr;
		if (CachedShared != nullptr)
		{
			return CachedShared;
		}

		CachedShared = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.UI.UseUMG"));
		if (CachedShared == nullptr)
		{
			CachedShared = IConsoleManager::Get().RegisterConsoleVariable(
				TEXT("Trace.UI.UseUMG"),
				SharedToggleDefault,
				TEXT("Spec v17 4. 1 (default): UI elements that have a UMG asset use it. 0: every one of "
				     "them falls back to the Canvas path that shipped before the migration. Registered by "
				     "whichever converted element runs first; the HUD corner's own override is "
				     "Trace.UI.HUD.UseUMG."),
				ECVF_Default);

			UE_LOG(LogTraceGame, Display,
				TEXT("[HUDUMG] registered the shared toggle Trace.UI.UseUMG (default %d) - no other UI "
				     "element had claimed it yet."),
				SharedToggleDefault);
		}
		return CachedShared;
	}

	/** -1 follows the shared switch, 0 forces Canvas, 1 forces UMG. Owned here. */
	static int32 GCornerOverride = -1;

#if !UE_BUILD_SHIPPING
	static FAutoConsoleVariableRef CVarCornerOverride(
		TEXT("Trace.UI.HUD.UseUMG"),
		GCornerOverride,
		TEXT("Spec v17 4 (step 4b), the bottom-right ammo/status corner ONLY. -1 (default) follows "
		     "Trace.UI.UseUMG. 0 forces the Canvas corner that shipped in v16. 1 forces the UMG corner "
		     "(WBP_TraceHudCorner). Changing it takes effect on the next drawn frame, both ways - it is "
		     "an A/B arm, so the before and the after can be photographed in one binary."),
		ECVF_Default);
#endif

	static bool IsCornerEnabled()
	{
		if (GCornerOverride >= 0)
		{
			return GCornerOverride != 0;
		}

		const IConsoleVariable* Toggle = SharedToggle();
		return (Toggle != nullptr) && (Toggle->GetInt() != 0);
	}
}

bool ATraceHUD::IsUmgCornerEnabled() const
{
	return TraceHudCornerUmg::IsCornerEnabled();
}

void ATraceHUD::SetCornerPath(ECornerPath InPath, const FString& InReason)
{
	if (bLoggedCornerPath && InPath == CornerPath && InReason == CornerFallbackReason)
	{
		return;
	}

	CornerPath = InPath;
	CornerFallbackReason = InReason;
	bLoggedCornerPath = true;

	if (InPath == ECornerPath::Umg)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[HUDUMG] the bottom-right ammo/status corner is drawn by %s. Everything else on this "
			     "HUD is still Canvas."),
			UTraceHudCornerWidget::CornerBlueprintPath());
	}
	else
	{
		// Display, not Warning: a Canvas corner is a SUPPORTED CONFIGURATION and the shipped one.
		// Warning-level noise for a correct outcome is how a log stops being read.
		UE_LOG(LogTraceGame, Display,
			TEXT("[HUDUMG] the bottom-right ammo/status corner is drawn on CANVAS (the v16 path) - %s."),
			*InReason);
	}
}

void ATraceHUD::HideCornerWidget()
{
	if (CornerWidget != nullptr)
	{
		CornerWidget->HideCorner();
	}
}

bool ATraceHUD::PresentCornerUmg(bool bInLive, const FTraceHudCornerState& InState)
{
	if (!TraceHudCornerUmg::IsCornerEnabled())
	{
		SetCornerPath(ECornerPath::Canvas, TEXT("Trace.UI.UseUMG / Trace.UI.HUD.UseUMG is off"));
		return false;
	}

	// ONE ATTEMPT PER HUD. A missing asset is a perfectly ordinary state (a fresh clone that has not
	// run Scripts/generate-hud-widgets.py yet), and retrying LoadClass every frame for the rest of
	// the match would cost a package-store lookup per frame to reach the same answer.
	if (bCornerAdoptFailed)
	{
		return false;
	}

	if (CornerWidget == nullptr)
	{
		// The lambda keeps the four failure exits identical: one reason string, one log line, one
		// latch, and Canvas from here on.
		auto AbandonAdoption = [this](const FString& InWhy) -> bool
		{
			bCornerAdoptFailed = true;
			SetCornerPath(ECornerPath::Canvas, InWhy);
			return false;
		};

		APlayerController* OwningController = PlayerOwner.Get();
		if (OwningController == nullptr || OwningController->GetLocalPlayer() == nullptr)
		{
			// No latch: a HUD can legitimately exist for a frame before its local player does, and
			// latching here would send a perfectly healthy client to Canvas for the whole match.
			return false;
		}

		UClass* LoadedCornerClass = LoadClass<UTraceHudCornerWidget>(nullptr,
			UTraceHudCornerWidget::CornerBlueprintPath());
		if (LoadedCornerClass == nullptr)
		{
			return AbandonAdoption(FString::Printf(
				TEXT("%s did not load (run Scripts/generate-hud-widgets.py to author it). This is a "
				     "supported configuration, not an error"),
				UTraceHudCornerWidget::CornerBlueprintPath()));
		}

		UTraceHudCornerWidget* NewCorner = CreateWidget<UTraceHudCornerWidget>(OwningController, LoadedCornerClass);
		if (NewCorner == nullptr)
		{
			return AbandonAdoption(FString::Printf(TEXT("CreateWidget failed for %s"),
				UTraceHudCornerWidget::CornerBlueprintPath()));
		}

		FString AdoptReason;
		if (!NewCorner->InitialiseCorner(AdoptReason))
		{
			// Error, unlike the missing-asset case: an asset that EXISTS and does not match the C++
			// contract is a broken generate or a hand edit, and it is the one outcome somebody has to
			// go and fix. The game keeps playing on Canvas either way.
			UE_LOG(LogTraceGame, Error,
				TEXT("[HUDUMG] *** %s is present but does not match the C++ contract: %s. *** The corner "
				     "falls back to Canvas. Re-run Scripts/generate-hud-widgets.py."),
				UTraceHudCornerWidget::CornerBlueprintPath(), *AdoptReason);
			return AbandonAdoption(FString::Printf(TEXT("the widget asset failed validation (%s)"), *AdoptReason));
		}

		NewCorner->AddToPlayerScreen(TraceHudCornerUmg::CornerZOrder);
		CornerWidget = NewCorner;
	}

	SetCornerPath(ECornerPath::Umg, FString());

	if (!bInLive)
	{
		CornerWidget->HideCorner();
		return true;
	}

	// THE ONE NUMBER THAT MAKES THE TWO PATHS THE SAME SIZE. The Canvas HUD scales itself by
	// ViewH/1080 (clamped); UMG has already scaled every widget by the project's DPI curve. Dividing
	// one by the other leaves exactly the Canvas geometry. At 720p and 1080p the engine's default
	// curve and the HUD's rule agree, so this is 1.0 and the transform costs nothing.
	const float ViewportDpiScale = FMath::Max(0.01f, UWidgetLayoutLibrary::GetViewportScale(this));
	const float CornerDesignScale = UIScale / ViewportDpiScale;
	const FTraceHudCornerPresented Presented = CornerWidget->PresentCorner(InState, CornerDesignScale);

	// One line, once, naming the three numbers that decide whether the two corners come out the same
	// SIZE. Printed rather than assumed because getting it wrong is invisible in a log and obvious
	// only in a photograph — which is exactly how the first armed capture of this corner came out
	// two and a half times too big.
	if (!bLoggedCornerScale)
	{
		bLoggedCornerScale = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[HUDUMG] scale: viewport=%.0fx%.0f  HUD UIScale=%.4f  viewport DPI scale=%.4f  =>  "
			     "corner render scale=%.4f"),
			ViewW, ViewH, UIScale, ViewportDpiScale, CornerDesignScale);
	}

#if !UE_BUILD_SHIPPING
	// The spec v16 §2 draw record, written from WHAT THE WIDGET REPORTED IT EMITTED — never from the
	// state it was handed. That is what keeps Trace.HUD.V16Shots a real test of this path rather than
	// a test of the state builder: a corner that set every text block and then painted nothing at all
	// would still have to come back with zero lit ticks.
	bDrewAmmoBlock = Presented.bAmmoBlock;
	DrawnAmmoText = Presented.AmmoText;
	bDrewBeeClip = Presented.bBeeClip;
	bDrewReloadBar = Presented.bReloadBar;
	DrawnMagazineTicks = Presented.LitTicks;
	DrawnStatusChips = Presented.Chips;
#endif

	return true;
}

// -------------------------------------------------------------------------------------------
// Top centre: both team scores + match clock
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawScoresAndClock()
{
	const float CX = ViewW * 0.5f;
	const float PanelW = TraceHUDStyle::TopPanelW * UIScale;
	const float PanelH = TraceHUDStyle::TopPanelH * UIScale;
	const float PanelX = CX - PanelW * 0.5f;
	const float PanelY = TraceHUDStyle::TopPanelY * UIScale;

	DrawPanel(PanelX, PanelY, PanelW, PanelH, TraceHUDStyle::PanelFill, TraceHUDStyle::PanelBorder);

	const int32 BlueScore   = (TraceGS != nullptr) ? TraceGS->GetScore(ETraceTeam::Blue) : 0;
	const int32 OrangeScore = (TraceGS != nullptr) ? TraceGS->GetScore(ETraceTeam::Orange) : 0;

	const float ScoreScale = 1.6f * UIScale;
	const float ScoreY = PanelY + (14.f * UIScale);
	const float ScoreInset = 120.f * UIScale;

	// Blue always sits left, Orange always right, on every client — a fixed layout is far easier
	// to read at a glance than a "your team first" one.
	DrawTextRight(FString::FromInt(BlueScore), TraceTeamColor(ETraceTeam::Blue),
		CX - ScoreInset, ScoreY, FontLarge, ScoreScale);
	DrawTextLeft(FString::FromInt(OrangeScore), TraceTeamColor(ETraceTeam::Orange),
		CX + ScoreInset, ScoreY, FontLarge, ScoreScale);

	// Clock / phase in the middle. GetMatchTimeRemaining() is driven by the replicated
	// GetServerWorldTimeSeconds() clock, so every client counts down together.
	FString ClockText(TEXT("--:--"));
	FLinearColor ClockColor = TraceHUDStyle::Ink;

	if (TraceGS != nullptr)
	{
		switch (TraceGS->TraceMatchState)
		{
		case ETraceMatchState::WaitingForPlayers:
		{
			// MatchEndServerTime doubles as the warm-up deadline (see ATraceGameMode::StartWarmup),
			// and is zero when no countdown is running. Showing the real number rather than the word
			// "WARMUP" is the difference between "the game is about to start" and "the game is stuck".
			const float Remaining = TraceGS->GetMatchTimeRemaining();
			ClockText = (TraceGS->MatchEndServerTime > 0.f) ? FormatClock(Remaining) : FString(TEXT("--:--"));
			ClockColor = TraceHUDStyle::InkDim;
			break;
		}

		case ETraceMatchState::InProgress:
		{
			const float Remaining = TraceGS->GetMatchTimeRemaining();
			ClockText = FormatClock(Remaining);
			ClockColor = (Remaining <= TraceHUDStyle::ClockUrgentSeconds) ? TraceHUDStyle::Danger : TraceHUDStyle::Ink;
			break;
		}

		case ETraceMatchState::PostMatch:
			ClockText = TEXT("FINAL");
			ClockColor = TraceHUDStyle::InkDim;
			break;

		default:
			break;
		}
	}

	DrawTextCentered(ClockText, ClockColor, CX, ScoreY + (6.f * UIScale), FontLarge, 1.15f * UIScale);

	// Bottom line of the panel: WHICH PHASE, and — since spec v4 §7 — WHICH OF THE TWO GAMES.
	//
	// "FIRST TO N" was never the rule and the score cap is now deleted outright (spec v4 §6): the
	// clock decides the match, so the phase is the useful thing to show.
	// ATraceGameState::GetHalfLabel() already reads "1ST HALF" / "HALF TIME" / "2ND HALF".
	FString FooterText = (TraceGS != nullptr) ? TraceGS->GetHalfLabel() : FString(TEXT("MATCH"));
	if (TraceGS != nullptr && TraceGS->TraceMatchState == ETraceMatchState::WaitingForPlayers)
	{
		FooterText = TEXT("WARM UP");
	}
	else if (TraceGS != nullptr && TraceGS->TraceMatchState == ETraceMatchState::PostMatch)
	{
		FooterText = TEXT("FULL TIME");
	}

	// ---- Deferred half time / full time (spec v9 §11) --------------------------------------------
	//
	// The clock has hit 0:00 and PLAY IS STILL LIVE. Without this the HUD shows a frozen 0:00 with
	// "1ST HALF" underneath it and nothing happening, which reads exactly like a hung timer — worse
	// than the mid-run cut-off §11 exists to fix. MEASURED, both arms of this same binary at the same
	// moment of the same scenario: Trace.HUD.PendingPeriodEndLabel 0 captured "00:00 / 1ST HALF" with
	// the Core live in Blue's hands and no explanation anywhere on screen.
	//
	// The PHASE half of the footer is what gets replaced — "1ST HALF" is the least useful thing on
	// the panel once the half is over, and it is the exact word that makes the frozen clock look
	// broken. The MODE suffix below is deliberately still appended: it has to be in every capture.
	//
	// ATraceGameState::GetPendingPeriodEndLabel() picks "HALF ENDS AT NEXT DEAD BALL" vs "MATCH ENDS
	// AT NEXT DEAD BALL" itself, from the same CurrentHalf < NumHalves test ATraceGameMode::
	// EndPeriodNow() makes, so this can never promise a half time that is really full time. Both
	// accessors are driven by replicated state and are correct on a client and on the listen host.
	const bool bPendingWhistle = (TraceGS != nullptr) && TraceGS->IsPendingPeriodEnd()
		&& (GHudPendingPeriodEndLabel != 0);
	if (bPendingWhistle)
	{
		FooterText = TraceGS->GetPendingPeriodEndLabel();
	}

	// The mode is appended rather than given a panel of its own. It has to be visible in EVERY
	// screenshot — the whole point of the A/B toggle is comparing two builds' worth of footage, and
	// a capture that does not say which mode it is from is a capture nobody can file — but it is
	// also a constant, and a constant does not deserve to compete with the clock for attention.
	if (TraceGS != nullptr)
	{
		FooterText = FString::Printf(TEXT("%s   -   %s"), *FooterText,
			*TraceScoringModeLabel(TraceGS->GetScoringMode()));
	}

	const float FooterY = PanelY + PanelH - (22.f * UIScale);

	// Danger, and pulsed, for the same reason the mercy warning is: this is a state the player has
	// only a few seconds to act inside. InkDim on a 0:00 clock is invisible.
	const FLinearColor FooterColor = bPendingWhistle
		? TraceHUDStyle::WithAlpha(TraceHUDStyle::Danger, 0.65f + 0.35f * FMath::Sin(Now * 6.f))
		: TraceHUDStyle::InkDim;

	DrawTextCentered(FooterText, FooterColor, CX, FooterY, FontSmall, UIScale);

	// The hard cap (spec v9 §11's "guard against never breaking") sits at the RIGHT EDGE OF THE
	// PANEL, on the footer's own baseline. It was under the panel for one capture and landed on top
	// of the "BLUE HAS THE CORE" banner, which hangs off the panel bottom by TraceHUDStyle::BannerGap
	// — there is no free strip there, and the collision was legible in the screenshot. The footer is
	// centred and short, so the right gutter is empty in every phase.
	//
	// Dim and bare seconds, deliberately: the label is the RULE, this is only the backstop, and a
	// player who reads "78s" as "the half ends in 78 seconds" has been misinformed — it is the
	// latest it can end. GetPendingPeriodEndTimeRemaining() returns 0 when no cap is armed (a
	// PeriodEndMaxDeferSeconds of 0 disables it), and then nothing is drawn at all.
	if (bPendingWhistle)
	{
		const float CapRemaining = TraceGS->GetPendingPeriodEndTimeRemaining();
		if (CapRemaining > 0.f)
		{
			DrawTextRight(FString::Printf(TEXT("%ds"), FMath::CeilToInt(CapRemaining)),
				TraceHUDStyle::InkDim, PanelX + PanelW - (12.f * UIScale), FooterY, FontSmall, UIScale);
		}
	}

	// ---- Mercy-rule warning ---------------------------------------------------------------------
	//
	// The mercy rule (spec v4 §6) ends the match the moment a lead reaches MercyRuleLead, and a
	// match that stops in the middle of the first half with no warning reads exactly like the
	// "keeps stopping and restarting" complaint this HUD has spent two passes answering. So the last
	// couple of points are announced: the losing side knows what is at stake and the winning side
	// knows it is one capture from the whistle.
	//
	// Read at the point of use, like everything else on the settings page — turn the rule off
	// mid-match and this disappears with it.
	const int32 MercyLead = UTraceSettings::Get().MercyRuleLead;
	const bool bMatchLive = (TraceGS != nullptr)
		&& TraceGS->TraceMatchState == ETraceMatchState::InProgress
		&& !TraceGS->IsHalfTimeBreak();

	if (bMatchLive && MercyLead > 0)
	{
		const int32 Lead = FMath::Abs(BlueScore - OrangeScore);
		const int32 Remaining = MercyLead - Lead;

		// Two points out, AND somebody actually ahead. The Lead > 0 test is not redundant: at a
		// mercy threshold of 1 a 0-0 scoreline is "one point from the win" for both teams at once,
		// and the warning was measured naming Orange as the leader of a drawn game.
		if (Lead > 0 && Remaining > 0 && Remaining <= TraceHUDStyle::MercyWarningPoints)
		{
			const ETraceTeam LeadingTeam = (BlueScore > OrangeScore) ? ETraceTeam::Blue : ETraceTeam::Orange;
			const FString MercyText = (Remaining == 1)
				? FString::Printf(TEXT("MERCY RULE: %s WINS ON THE NEXT POINT"), *TraceTeamName(LeadingTeam).ToString().ToUpper())
				: FString::Printf(TEXT("MERCY RULE: %s IS %d POINTS FROM THE WIN"), *TraceTeamName(LeadingTeam).ToString().ToUpper(), Remaining);

			// Pulsed and team coloured, one line UNDER the Core banner.
			//
			// It was above the panel, which is where there is visibly empty screen — and it was
			// clipped off the top edge in the first capture, because TopPanelY is only 14 reference
			// pixels and there is no room up there at all. Below the possession banner is the next
			// free strip and it is the right neighbourhood anyway: these are the two lines that say
			// what is at stake right now.
			const float MercyY = (TraceHUDStyle::TopPanelY + TraceHUDStyle::TopPanelH
				+ TraceHUDStyle::BannerGap + 34.f) * UIScale;

			const FLinearColor MercyColor = TraceHUDStyle::WithAlpha(
				TraceTeamColor(LeadingTeam), 0.65f + 0.35f * FMath::Sin(Now * 5.f));

			DrawTextCentered(MercyText, MercyColor, CX, MercyY, FontSmall, UIScale);
		}
	}
}

// -------------------------------------------------------------------------------------------
// Core status banner
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawCoreBanner()
{
	if (TraceGS == nullptr)
	{
		return;
	}

	// The GameState's Core pointer is replicated; until it arrives we draw nothing rather than
	// claiming a state we cannot know.
	ATraceCore* Core = TraceGS->Core;
	if (Core == nullptr)
	{
		return;
	}

	FString BannerText;
	FLinearColor BannerColor;

	ATraceCharacter* Carrier = Core->GetCarrier();
	if (Carrier == nullptr)
	{
		// MODE A (spec §2): the Core is a STATUS. It is never loose on the ground and never in
		// flight, so there is nothing here for the player to run over and grab — telling them "CORE
		// LOOSE" sent them chasing a pickup that no longer exists. The only two holderless states
		// are both transient and both mean "wait":
		//
		//   out of play  — the half-time interval and the post-match window, where the Core is
		//                  deliberately parked and granted to nobody (ATraceCore::KickoffTo(None));
		//   kickoff      — the ~1s grant delay after a whistle or a score, which exists so the Core
		//                  is not handed over mid-teleport.
		//
		// MODE B REINSTATES THE THIRD STATE, and reinstates it as a real instruction: a thrown Core
		// IS on the field and the first player to touch it takes it, either team. Saying "CORE
		// LOOSE" in mode A was a lie; refusing to say it in mode B would be the opposite lie, and
		// the resulting scramble is the single most important thing on screen when it happens.
		if (bGoalMode && Core->IsLoose())
		{
			BannerText = TEXT("CORE LOOSE - FIRST TOUCH TAKES IT");
			BannerColor = TraceHUDStyle::Danger;
		}
		else
		{
			BannerText = Core->IsOutOfPlay() ? TEXT("CORE OUT OF PLAY") : TEXT("CORE KICKOFF");
			BannerColor = TraceHUDStyle::InkDim;
		}
	}
	else if (Carrier == LocalChar.Get())
	{
		// The verb follows the mode. Spec v4 §7 makes LMB mean two different things while carrying —
		// the 0.5 s hover-hold pass in mode A, an outright throw in mode B — and the banner is the
		// one piece of HUD a carrier is guaranteed to be looking at.
		BannerText = bGoalMode
			? FString(TEXT("YOU HAVE THE CORE - LMB THROWS"))
			: FString(TEXT("YOU HAVE THE CORE - HOLD LMB TO PASS"));
		BannerColor = TraceTeamColor(LocalTeam);
	}
	else
	{
		const ETraceTeam CarrierTeam = Carrier->GetTeam();
		BannerText = FString::Printf(TEXT("%s HAS THE CORE"), *TraceTeamName(CarrierTeam).ToString().ToUpper());
		BannerColor = TraceTeamColor(CarrierTeam);
	}

	// Pulse anything that demands a reaction: you are carrying it, an enemy is, or (mode B) it is
	// lying on the field and the next person to touch it owns the possession.
	const bool bUrgent = (Carrier != nullptr)
		? (Carrier == LocalChar.Get() || Carrier->GetTeam() != LocalTeam)
		: (bGoalMode && Core->IsLoose());
	if (bUrgent)
	{
		BannerColor = TraceHUDStyle::WithAlpha(BannerColor, 0.7f + 0.3f * FMath::Sin(Now * 6.f));
	}

	const float CX = ViewW * 0.5f;
	const float BannerY = (TraceHUDStyle::TopPanelY + TraceHUDStyle::TopPanelH + TraceHUDStyle::BannerGap) * UIScale;
	const float BannerScale = 1.05f * UIScale;

	const float TextW = MeasureWidth(BannerText, FontMedium, BannerScale);
	const float TextH = MeasureHeight(BannerText, FontMedium, BannerScale);
	const float PadX = 16.f * UIScale;
	const float PadY = 5.f * UIScale;

	DrawRect(TraceHUDStyle::Shadow, CX - TextW * 0.5f - PadX, BannerY - PadY, TextW + PadX * 2.f, TextH + PadY * 2.f);
	DrawTextCentered(BannerText, BannerColor, CX, BannerY, FontMedium, BannerScale);
}

// -------------------------------------------------------------------------------------------
// Phase callouts
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawNetworkStatus()
{
	FString Endpoint;
	FString Detail;
	const TraceNet::ERole ConnectionRole = TraceNet::DescribeConnection(GetWorld(), Endpoint, Detail);

	// A standalone match with a free port is an ordinary offline game and needs no chrome. A
	// standalone match whose port was TAKEN is a listen server that silently failed to bind, and that
	// one has to be shouted about — it is precisely the state that looks identical to a working host
	// from the inside, which is the bug this pass exists to close.
	if (ConnectionRole == TraceNet::ERole::Offline && Detail.IsEmpty())
	{
		return;
	}

	FString Headline;
	FLinearColor Accent = TraceHUDStyle::Good;
	switch (ConnectionRole)
	{
	case TraceNet::ERole::Hosting:
		Headline = FString::Printf(TEXT("HOSTING  %s"), *Endpoint);
		Accent = TraceHUDStyle::Good;
		break;

	case TraceNet::ERole::Client:
		Headline = FString::Printf(TEXT("CONNECTED  %s"), *Endpoint);
		Accent = TraceHUDStyle::Good;
		// Bots fill the empty slots and yield them as humans arrive, so the useful second line for a
		// client is simply how many humans are in the match with them.
		Detail.Reset();
		break;

	default:
		Headline = TEXT("OFFLINE - NOBODY CAN JOIN");
		Accent = TraceHUDStyle::Warning;
		break;
	}

	// Human count, on both host and client. Bots carry real PlayerStates (see the bot section of
	// ATraceGameMode), so PlayerArray alone would always read 10 and tell nobody anything.
	if (const AGameStateBase* GameStateBase = GetWorld() != nullptr ? GetWorld()->GetGameState() : nullptr)
	{
		int32 Humans = 0;
		for (const APlayerState* Player : GameStateBase->PlayerArray)
		{
			if (Player != nullptr && !Player->IsABot())
			{
				++Humans;
			}
		}

		const FString HumanText = (Humans == 1)
			? FString(TEXT("1 HUMAN PLAYER"))
			: FString::Printf(TEXT("%d HUMAN PLAYERS"), Humans);

		Detail = Detail.IsEmpty() ? HumanText : FString::Printf(TEXT("%s  -  %s"), *Detail, *HumanText);

		// Logged on CHANGE, never per frame. "A second human arrived" is the single fact the Demo 5
		// report needed and could not get, and putting it in the log at Display means the next person
		// who says "I don't think we were connected" can be answered from the file they already have,
		// on either machine, without anyone having to reproduce anything.
		if (Humans != LastLoggedHumanCount)
		{
			LastLoggedHumanCount = Humans;

			// Character count as well as player-state count, because they answer different
			// questions. Player states prove the ROSTER replicated; characters prove the PAWNS did —
			// "the client can see the host's pawn" is a separate claim from "the client knows the
			// host exists", and only the second one is free.
			int32 Characters = 0;
			for (TActorIterator<ATraceCharacter> It(GetWorld()); It; ++It)
			{
				++Characters;
			}

			UE_LOG(LogTraceGame, Display, TEXT("[Net] %s - %d human player(s) of %d player states, %d characters in the world."),
				*Headline, Humans, GameStateBase->PlayerArray.Num(), Characters);

			for (const APlayerState* Player : GameStateBase->PlayerArray)
			{
				if (Player != nullptr && !Player->IsABot())
				{
					UE_LOG(LogTraceGame, Display, TEXT("[Net]   human player state: '%s'"), *Player->GetPlayerName());
				}
			}
		}
	}

	const float HeadScale = 1.05f * UIScale;
	const float DetailScale = 0.95f * UIScale;
	const float PadX = 12.f * UIScale;
	const float PadY = 7.f * UIScale;

	const float HeadW = MeasureWidth(Headline, FontMedium, HeadScale);
	const float DetailW = MeasureWidth(Detail, FontSmall, DetailScale);
	const float HeadH = MeasureHeight(Headline, FontMedium, HeadScale);
	const float DetailH = Detail.IsEmpty() ? 0.f : MeasureHeight(Detail, FontSmall, DetailScale);

	const float PanelW = FMath::Max(HeadW, DetailW) + PadX * 2.f;
	const float PanelH = HeadH + DetailH + PadY * 2.f;

	// Top-RIGHT. The top-left belongs to the art warning and the top-centre to the score panel; this
	// is the only free corner, and it is a corner the eye visits between fights rather than during
	// one.
	const float PanelX = ViewW - PanelW - (18.f * UIScale);
	const float PanelY = TraceHUDStyle::TopPanelY * UIScale;

	DrawPanel(PanelX, PanelY, PanelW, PanelH, TraceHUDStyle::PanelFill,
		TraceHUDStyle::WithAlpha(Accent, 0.45f));

	DrawTextLeft(Headline, Accent, PanelX + PadX, PanelY + PadY, FontMedium, HeadScale);
	if (!Detail.IsEmpty())
	{
		DrawTextLeft(Detail, TraceHUDStyle::InkDim, PanelX + PadX, PanelY + PadY + HeadH, FontSmall, DetailScale);
	}

	// Publish the bottom edge so DrawKillFeed() can stack underneath this panel instead of over it.
	KillFeedTopY = PanelY + PanelH;
}

void ATraceHUD::DrawNetworkFailureBanner()
{
	FString Headline;
	FString Detail;
	double AgeSeconds = 0.0;
	if (!TraceNet::GetLastFailure(Headline, Detail, AgeSeconds))
	{
		return;
	}

	// Short in-match, unlike the title screen's minute: here the player is looking at the screen when
	// it happens, and a banner that outstays its welcome would sit over a live fight.
	constexpr double VisibleSeconds = 12.0;
	if (AgeSeconds > VisibleSeconds)
	{
		return;
	}
	const float Fade = static_cast<float>(FMath::Clamp((VisibleSeconds - AgeSeconds) / 2.0, 0.0, 1.0));

	const float CX = ViewW * 0.5f;
	const float Y = ViewH * 0.16f;
	const float HeadScale = 1.2f * UIScale;

	const float HeadW = MeasureWidth(Headline, FontMedium, HeadScale);
	const float HeadH = MeasureHeight(Headline, FontMedium, HeadScale);
	const float DetailH = MeasureHeight(Detail, FontSmall, UIScale);
	const float PadX = 20.f * UIScale;
	const float PadY = 10.f * UIScale;

	const float PanelW = FMath::Max(HeadW, MeasureWidth(Detail, FontSmall, UIScale)) + PadX * 2.f;
	const float PanelH = HeadH + DetailH + PadY * 2.f;

	DrawPanel(CX - PanelW * 0.5f, Y, PanelW, PanelH,
		FLinearColor(0.20f, 0.03f, 0.02f, 0.88f * Fade),
		TraceHUDStyle::WithAlpha(TraceHUDStyle::Danger, 0.75f * Fade));

	DrawTextCentered(Headline, TraceHUDStyle::WithAlpha(TraceHUDStyle::Danger, Fade), CX, Y + PadY, FontMedium, HeadScale);
	DrawTextCentered(Detail, TraceHUDStyle::WithAlpha(TraceHUDStyle::InkDim, Fade), CX, Y + PadY + HeadH, FontSmall, UIScale);
}

// -------------------------------------------------------------------------------------------
// Kill feed (spec v8 §6)
// -------------------------------------------------------------------------------------------

namespace TraceKillFeedArt
{
	/**
	 * THE ICONS, AS ASCII BITMAPS.
	 *
	 * Every glyph is a 13x13 grid, '#' lit and '.' clear, rendered as run-length integer DrawRects
	 * with a one-cell-independent 1 px dark surround. No UMG, no texture, no imported art
	 * (contract §2 / spec v8 §6) — and no vector maths either, because at the size a feed row can
	 * afford (26 px) a hand-placed bitmap is the only thing that reliably reads. What you see here
	 * is exactly what appears on screen, which is also why they are editable by anyone.
	 *
	 * The three the spec names, plus the two the cause taxonomy already implies:
	 *
	 *   Skull            HEADSHOT.  Silhouette + two eye sockets + a tooth row. Amber, matching the
	 *                               headshot hit-marker this HUD already draws, so the two pieces of
	 *                               head-shot feedback in the game agree with each other.
	 *   Double chevron   DASH.      A trace kill. Reads as speed/direction at any size and is the
	 *                               least confusable shape of the five.
	 *   Shield           PARRY.     Drawn as an OUTLINE rather than filled: a solid shield at 26 px
	 *                               degenerates into an anonymous downward blob, and the internal
	 *                               void is what makes it a shield. Red, matching the parry flash.
	 *   Round            BULLET.    The default for body and leg shots. Pointed right, with the
	 *                               case/projectile groove that stops it reading as an arrow.
	 *   Cross            WORLD.     Fell out of the arena, or an unattributable death.
	 *   Blade            KNIFE.     A front swipe (30). Neutral, because it is an ordinary trade.
	 *   Blade + chevron  BACKSTAB.  100 from the rear hemisphere. Amber, the same amber the skull
	 *                               wears, because those two are the game's only 100-damage events.
	 */
	static constexpr int32 GlyphGrid = 13;

	// A rifle round, pointed right: base flange, straight case walls, long ogive nose.
	//
	// THREE DRAFTS REJECTED. Each failure is a rule about drawing at 26 px, so they are kept.
	//
	//   Draft 1 had a ONE-CELL dark groove through a solid body. It disappeared: DrawKillIcon's
	//   surround pass grows every lit run by 1 px on all sides, so at Cell = 2 px a one-cell (2 px)
	//   gap is closed by 1 px of surround from each side. That is a hard floor — INTERIOR DETAIL
	//   MUST BE AT LEAST TWO CELLS WIDE or it is not detail, it is nothing.
	//
	//   Draft 2 answered that by dropping the groove for a solid body with a flat back and a long
	//   taper. Rendered at final size it plainly read as a MOUSE CURSOR — because a cursor is a flat
	//   back and a taper, and crucially an ASYMMETRIC one. Symmetry about the horizontal axis is what
	//   separates the two shapes, and this glyph is exactly symmetric for that reason.
	//
	//   Draft 3 restored the rim as a DETACHED bar two cells clear of the case, which is the smallest
	//   gap the dilate leaves open. It survived, and it read as a BATTERY: at 26 px a floating bar is
	//   not a rim, it is a second object. Rendered at 1x it degenerated further, into a dot and a
	//   blob. THE RIM HAS TO BE ATTACHED — a cartridge base is a step in one silhouette, not two
	//   shapes with a gap.
	//
	// So: one connected silhouette, a one-cell step top and bottom at the base for the flange,
	// parallel case walls, and a four-column ogive. It reads as a round at 1x, which none of the
	// three drafts above did.
	static const TCHAR* const GlyphBullet[GlyphGrid] =
	{
		TEXT("............."),
		TEXT("............."),
		TEXT("..##........."),
		TEXT("..#######...."),
		TEXT("..#########.."),
		TEXT("..##########."),
		TEXT("..###########"),
		TEXT("..##########."),
		TEXT("..#########.."),
		TEXT("..#######...."),
		TEXT("..##........."),
		TEXT("............."),
		TEXT(".............")
	};

	static const TCHAR* const GlyphSkull[GlyphGrid] =
	{
		TEXT("....#####...."),
		TEXT("..#########.."),
		TEXT(".###########."),
		TEXT("#############"),
		TEXT("###.#####.###"),
		TEXT("##...###...##"),
		TEXT("##...###...##"),
		TEXT("###.#####.###"),
		TEXT("#####...#####"),
		TEXT(".###########."),
		TEXT("..#########.."),
		TEXT("..#.#.#.#.#.."),
		TEXT("...#######...")
	};

	static const TCHAR* const GlyphDash[GlyphGrid] =
	{
		TEXT("............."),
		TEXT("............."),
		TEXT("##.....##...."),
		TEXT(".##.....##..."),
		TEXT("..##.....##.."),
		TEXT("...##.....##."),
		TEXT("....##.....##"),
		TEXT("...##.....##."),
		TEXT("..##.....##.."),
		TEXT(".##.....##..."),
		TEXT("##.....##...."),
		TEXT("............."),
		TEXT(".............")
	};

	static const TCHAR* const GlyphParry[GlyphGrid] =
	{
		TEXT(".###########."),
		TEXT("#############"),
		TEXT("##.........##"),
		TEXT("##.........##"),
		TEXT("##.........##"),
		TEXT(".##.......##."),
		TEXT(".##.......##."),
		TEXT("..##.....##.."),
		TEXT("..##.....##.."),
		TEXT("...##...##..."),
		TEXT("....##.##...."),
		TEXT(".....###....."),
		TEXT("......#......")
	};

	static const TCHAR* const GlyphWorld[GlyphGrid] =
	{
		TEXT("............."),
		TEXT(".##.......##."),
		TEXT(".###.....###."),
		TEXT("..###...###.."),
		TEXT("...###.###..."),
		TEXT("....#####...."),
		TEXT(".....###....."),
		TEXT("....#####...."),
		TEXT("...###.###..."),
		TEXT("..###...###.."),
		TEXT(".###.....###."),
		TEXT(".##.......##."),
		TEXT(".............")
	};

	// A knife, pointed up-right: a wide handle at the bottom-left, a guard step, then a blade whose
	// spine is straight and whose edge tapers to a point.
	//
	// The bullet glyph's rules above apply and were obeyed: interior detail is at least TWO cells
	// wide (the guard notch is two), and the silhouette is ONE connected shape (no detached guard —
	// draft 3's battery failure is exactly what a floating cross-guard would reproduce). It is
	// deliberately ASYMMETRIC about both axes, which is what separates it from the bullet: the round
	// is exactly horizontally symmetric, so a symmetric blade at 26 px would collide with it.
	static const TCHAR* const GlyphKnife[GlyphGrid] =
	{
		TEXT("..........##."),
		TEXT(".........###."),
		TEXT("........####."),
		TEXT(".......#####."),
		TEXT("......#####.."),
		TEXT(".....#####..."),
		TEXT("....#####...."),
		TEXT("...#####....."),
		TEXT("..#####......"),
		TEXT(".#######....."),
		TEXT(".#######....."),
		TEXT("###.........."),
		TEXT("###..........")
	};

	// The same blade, plus a double chevron BEHIND it pointing at the handle.
	//
	// The chevron is the dash glyph's own shape, and reusing it is the point: a back-stab is "hit
	// from the direction the victim was not looking", which is the same idea the dash chevron already
	// means. Two cells thick with a two-cell gap so the dilate cannot close it (see the bullet note).
	static const TCHAR* const GlyphBackstab[GlyphGrid] =
	{
		TEXT("..........##."),
		TEXT(".........###."),
		TEXT("........####."),
		TEXT("##.....#####."),
		TEXT("###...#####.."),
		TEXT(".###.#####..."),
		TEXT("..#######...."),
		TEXT(".###.#####..."),
		TEXT("###...#####.."),
		TEXT("##...####...."),
		TEXT(".....####...."),
		TEXT("...###......."),
		TEXT("...###.......")
	};

	static const TCHAR* const* GlyphFor(ETraceKillIcon Icon)
	{
		switch (Icon)
		{
		case ETraceKillIcon::Headshot: return GlyphSkull;
		case ETraceKillIcon::Dash:     return GlyphDash;
		case ETraceKillIcon::Parry:    return GlyphParry;
		case ETraceKillIcon::World:    return GlyphWorld;
		case ETraceKillIcon::Knife:    return GlyphKnife;
		case ETraceKillIcon::Backstab: return GlyphBackstab;
		default:                       return GlyphBullet;
		}
	}

	/**
	 * Icon tint. Each one is the colour the game ALREADY uses for that event elsewhere, rather than
	 * a fresh palette: amber is the headshot hit-marker, red is the parry flash. A player who has
	 * learned one has learned the other.
	 */
	static FLinearColor ColorFor(ETraceKillIcon Icon)
	{
		switch (Icon)
		{
		case ETraceKillIcon::Headshot: return TraceHUDStyle::Warning;
		case ETraceKillIcon::Parry:    return TraceHUDStyle::Danger;
		case ETraceKillIcon::World:    return TraceHUDStyle::InkDim;
		// A back-stab is a one-swing kill from behind, so it gets the same amber the head shot gets:
		// this HUD already means "that was the expensive kind of hit" by amber, and the two are the
		// only 100-damage single events in the game. The front swipe stays neutral because it is not
		// one — 30 damage, four swings, an ordinary trade.
		case ETraceKillIcon::Backstab: return TraceHUDStyle::Warning;
		default:                       return TraceHUDStyle::Ink;   // Bullet, Dash and Knife: neutral
		}
	}

	/** Icon box in 1080p reference pixels, before the integer-cell floor below. */
	static constexpr float IconBoxPx = 26.f;

	static constexpr float MarginX = 18.f;   // must match DrawNetworkStatus's right margin
	static constexpr float GapUnderPanel = 8.f;
	static constexpr float RowGap = 4.f;
	static constexpr float PadX = 10.f;
	static constexpr float PadY = 5.f;
	static constexpr float IconGap = 9.f;
	static constexpr float NameScale = 0.95f;

	/** Longest name drawn. Bot names are short; a pasted human name is not necessarily. */
	static constexpr int32 MaxNameChars = 16;

	/** How often a client with no relay yet may re-run the actor search, in seconds. */
	static constexpr float RelayPollInterval = 0.5f;

	static FString Shorten(const FString& Name)
	{
		return (Name.Len() <= MaxNameChars) ? Name : (Name.Left(MaxNameChars - 1) + TEXT("."));
	}
}

void ATraceHUD::DrawKillIcon(ETraceKillIcon Icon, float X, float Y, float Cell, const FLinearColor& Color)
{
	const TCHAR* const* Rows = TraceKillFeedArt::GlyphFor(Icon);

	// Two passes: a dilated black surround, then the fill. Same reasoning as DrawAimReticle's
	// outline — the arena is a black floor plus saturated cyan and amber neon, and an unoutlined
	// white glyph disappears the moment it crosses a lit edge.
	const FLinearColor Surround(0.f, 0.f, 0.f, 0.80f * Color.A);

	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const FLinearColor PassColor = (Pass == 0) ? Surround : Color;
		const float Grow = (Pass == 0) ? 1.f : 0.f;

		for (int32 RowIndex = 0; RowIndex < TraceKillFeedArt::GlyphGrid; ++RowIndex)
		{
			const TCHAR* const Row = Rows[RowIndex];

			// Run-length: one DrawRect per horizontal run of lit cells rather than one per cell,
			// which turns a worst-case 169 rects per icon into at most a handful.
			int32 RunStart = -1;
			for (int32 Col = 0; Col <= TraceKillFeedArt::GlyphGrid; ++Col)
			{
				const bool bLit = (Col < TraceKillFeedArt::GlyphGrid) && (Row[Col] == TEXT('#'));
				if (bLit && RunStart < 0)
				{
					RunStart = Col;
				}
				else if (!bLit && RunStart >= 0)
				{
					DrawRect(PassColor,
						X + RunStart * Cell - Grow,
						Y + RowIndex * Cell - Grow,
						(Col - RunStart) * Cell + Grow * 2.f,
						Cell + Grow * 2.f);
					RunStart = -1;
				}
			}
		}
	}
}

void ATraceHUD::DrawKillFeed()
{
	UWorld* const World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Re-found on demand: on a client the relay arrives by replication some frames after this HUD
	// exists, and a travel destroys it. Cheap once it is cached; an actor iteration until then.
	if (!KillFeedRelay.IsValid())
	{
		if ((Now - LastKillFeedRelayPollTime) < TraceKillFeedArt::RelayPollInterval)
		{
			return;
		}
		LastKillFeedRelayPollTime = Now;

		KillFeedRelay = ATraceKillFeedRelay::Find(World);
		if (!KillFeedRelay.IsValid())
		{
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[KillFeed] HUD found the relay; the feed will draw on this machine."));
	}

	const TArray<FTraceKillFeedEntry>& Rows = KillFeedRelay->GetEntries();
	if (Rows.Num() == 0)
	{
		return;
	}

	// INTEGER CELLS, with a floor of 2. At 1080p this is 2 px per cell (a 26 px icon); at 720p the
	// floor keeps it at 2 rather than collapsing to a 13 px smudge, so the glyph stays legible at
	// the resolution this project is actually played and screenshotted at.
	const float Cell = FMath::Max(2.f,
		FMath::FloorToFloat(TraceKillFeedArt::IconBoxPx * UIScale / static_cast<float>(TraceKillFeedArt::GlyphGrid)));
	const float IconPx = Cell * static_cast<float>(TraceKillFeedArt::GlyphGrid);

	const float NameScale = TraceKillFeedArt::NameScale * UIScale;
	const float PadX = TraceKillFeedArt::PadX * UIScale;
	const float PadY = TraceKillFeedArt::PadY * UIScale;
	const float IconGap = TraceKillFeedArt::IconGap * UIScale;
	const float RightX = ViewW - TraceKillFeedArt::MarginX * UIScale;

	// BY ID, NOT BY NAME. Names are not unique — a listen server and a client on one machine both
	// default to that machine's name, and matching on the string drew "you died" on the host for a
	// kill that happened to the joining player. PlayerId is server-assigned and replicated.
	//
	// ZERO IS NOT AN IDENTITY. Measured on a live match: every bot's APlayerState carries PlayerId 0
	// (they are not registered through the login path that hands out ids; the two humans got 256 and
	// 257). So an id of 0 means "some bot", not "this player", and a local id of 0 must match nothing
	// — otherwise one unlucky assignment would colour every bot-on-bot row as the local player's own
	// kill. Failing to zero costs a missing highlight, which is the harmless direction.
	const int32 RawLocalId = (LocalPS != nullptr) ? LocalPS->GetPlayerId() : INDEX_NONE;
	const int32 LocalPlayerId = (RawLocalId > 0) ? RawLocalId : INDEX_NONE;

	float RowY = KillFeedTopY + TraceKillFeedArt::GapUnderPanel * UIScale;
	int32 Drawn = 0;

	for (const FTraceKillFeedEntry& Entry : Rows)
	{
		if (Drawn >= ATraceKillFeedRelay::MaxDrawnEntries)
		{
			break;
		}

		// AGAINST THE REPLICATED SERVER CLOCK, not against when this machine saw the row. That is
		// what stops a client that joined mid-match from being shown five old kills as if they had
		// just happened, and it is why a row is exactly as old on the client as it is on the host.
		//
		// Rows are newest-first, so the first one that has expired means every later one has too.
		const float Age = ATraceKillFeedRelay::GetEntryAge(Entry, World);
		if (Age >= ATraceKillFeedRelay::EntryLifetime)
		{
			break;
		}

		const float FadeStart = ATraceKillFeedRelay::EntryLifetime - ATraceKillFeedRelay::EntryFadeTime;
		const float Alpha = (Age <= FadeStart)
			? 1.f
			: FMath::Clamp(1.f - (Age - FadeStart) / ATraceKillFeedRelay::EntryFadeTime, 0.f, 1.f);

		const FString KillerText = TraceKillFeedArt::Shorten(Entry.KillerName);
		const FString VictimText = TraceKillFeedArt::Shorten(Entry.VictimName);

		const float KillerW = Entry.bHasKiller ? MeasureWidth(KillerText, FontSmall, NameScale) : 0.f;
		const float VictimW = MeasureWidth(VictimText, FontSmall, NameScale);
		const float TextH = MeasureHeight(VictimText, FontSmall, NameScale);

		const float ContentW = (Entry.bHasKiller ? KillerW + IconGap : 0.f) + IconPx + IconGap + VictimW;
		const float RowW = ContentW + PadX * 2.f;
		const float RowH = FMath::Max(TextH, IconPx) + PadY * 2.f;
		const float RowX = RightX - RowW;

		// A row involving the local player gets a coloured edge — the one thing a player wants to
		// find in a feed without reading it is whether it was about them.
		FLinearColor Border = TraceHUDStyle::PanelBorder;
		if (LocalPlayerId != INDEX_NONE)
		{
			if (Entry.bHasKiller && Entry.KillerPlayerId == LocalPlayerId)
			{
				Border = TraceHUDStyle::WithAlpha(TraceHUDStyle::Good, 0.85f);
			}
			else if (Entry.VictimPlayerId == LocalPlayerId)
			{
				Border = TraceHUDStyle::WithAlpha(TraceHUDStyle::Danger, 0.85f);
			}
		}

		DrawPanel(RowX, RowY, RowW, RowH,
			TraceHUDStyle::WithAlpha(TraceHUDStyle::PanelFill, TraceHUDStyle::PanelFill.A * Alpha),
			TraceHUDStyle::WithAlpha(Border, Border.A * Alpha));

		const float TextY = RowY + (RowH - TextH) * 0.5f;
		const float IconY = FMath::RoundToFloat(RowY + (RowH - IconPx) * 0.5f);

		float CursorX = RowX + PadX;
		if (Entry.bHasKiller)
		{
			DrawTextLeft(KillerText,
				TraceHUDStyle::WithAlpha(TraceTeamColor(Entry.KillerTeam), Alpha),
				CursorX, TextY, FontSmall, NameScale);
			CursorX += KillerW + IconGap;
		}

		DrawKillIcon(Entry.Icon, FMath::RoundToFloat(CursorX), IconY, Cell,
			TraceHUDStyle::WithAlpha(TraceKillFeedArt::ColorFor(Entry.Icon), Alpha));
		CursorX += IconPx + IconGap;

		DrawTextLeft(VictimText,
			TraceHUDStyle::WithAlpha(TraceTeamColor(Entry.VictimTeam), Alpha),
			CursorX, TextY, FontSmall, NameScale);

		RowY += RowH + TraceKillFeedArt::RowGap * UIScale;
		++Drawn;
	}
}

void ATraceHUD::DrawPhaseBanner()
{
	if (TraceGS == nullptr || TraceGS->TraceMatchState == ETraceMatchState::PostMatch)
	{
		return;
	}

	const float CX = ViewW * 0.5f;
	const float BannerY = ViewH * 0.30f;

	if (TraceGS->TraceMatchState == ETraceMatchState::WaitingForPlayers)
	{
		const bool bCountingDown = TraceGS->MatchEndServerTime > 0.f;
		const float Remaining = TraceGS->GetMatchTimeRemaining();

		if (bCountingDown)
		{
			// Label first, then the digit: read top to bottom it says "match starts in ... three".
			// The digit is drawn large and grows as each second lands, so the start of a match is
			// impossible to mistake for the game having reset itself.
			const int32 Seconds = FMath::Max(0, FMath::CeilToInt(Remaining));
			const float Frac = Remaining - FMath::FloorToFloat(Remaining);
			const float Pop = 1.f + 0.18f * FMath::Clamp(Frac, 0.f, 1.f);

			DrawTextCentered(TEXT("MATCH STARTS IN"), TraceHUDStyle::InkDim,
				CX, BannerY, FontSmall, 1.2f * UIScale);
			DrawTextCentered(FString::FromInt(Seconds), TraceHUDStyle::Ink,
				CX, BannerY + (26.f * UIScale), FontLarge, 3.4f * UIScale * Pop);
		}
		else
		{
			DrawTextCentered(TEXT("WARM UP"), TraceHUDStyle::InkDim, CX, BannerY, FontSmall, 1.2f * UIScale);
			DrawTextCentered(TEXT("WAITING FOR PLAYERS"), TraceHUDStyle::Ink,
				CX, BannerY + (26.f * UIScale), FontMedium, 1.4f * UIScale);
		}
		return;
	}

	// HALF TIME. TraceMatchState is deliberately still InProgress during the interval (see
	// ATraceGameState::bHalfTimeBreak), so this has to be asked for explicitly — and it has to come
	// before the GO banner, or the second half's whistle would be drawn over the interval.
	//
	// The side switch has ALREADY happened by the time this shows: the break is spent looking at the
	// end you are about to attack, so telling the player it happened is the whole point of the card.
	if (TraceGS->IsHalfTimeBreak())
	{
		// The subtitle is placed off the MEASURED height of the headline, not off a hand-picked
		// constant. A 3.0-scaled FontLarge is ~64px tall at UIScale 1, so the previous fixed 46px
		// offset drew "SIDES SWITCHED" straight through the middle of "HALF TIME" — confirmed in a
		// captured frame. Measuring makes it correct at every UIScale instead of at one of them.
		const float HalfTimeScale = 3.0f * UIScale;
		DrawTextCentered(TEXT("HALF TIME"), TraceHUDStyle::Ink, CX, BannerY, FontLarge, HalfTimeScale);
		DrawTextCentered(
			FString::Printf(TEXT("SIDES SWITCHED  -  RESUMING IN %.0f"), TraceGS->GetMatchTimeRemaining()),
			TraceHUDStyle::InkDim, CX,
			BannerY + MeasureHeight(TEXT("HALF TIME"), FontLarge, HalfTimeScale) + (10.f * UIScale),
			FontSmall, 1.2f * UIScale);
		return;
	}

	// Wipe bonus flash (§1: +2 for eliminating all five at once). Drawn under the half-time card so
	// a wipe that ends the half does not fight it for the same pixels.
	if (TraceGS->LastWipeBonusTeam != ETraceTeam::None)
	{
		const float WipeAge = static_cast<float>(TraceGS->GetServerWorldTimeSeconds()) - TraceGS->LastWipeBonusServerTime;
		if (WipeAge >= 0.f && WipeAge < TraceHUDStyle::WipeBonusDuration)
		{
			const float Alpha = 1.f - (WipeAge / TraceHUDStyle::WipeBonusDuration);
			const FLinearColor Tint = TraceTeamColor(TraceGS->LastWipeBonusTeam);
			DrawTextCentered(TEXT("TEAM WIPE  +2"), TraceHUDStyle::WithAlpha(Tint, Alpha),
				CX, BannerY - (34.f * UIScale), FontMedium, 1.7f * UIScale);
		}
	}

	// InProgress: a short "GO" on the whistle, then out of the way for good.
	const float Age = Now - MatchStartTime;
	if (Age >= 0.f && Age < TraceHUDStyle::GoBannerDuration)
	{
		const float Alpha = 1.f - (Age / TraceHUDStyle::GoBannerDuration);
		DrawTextCentered(TEXT("GO"), TraceHUDStyle::WithAlpha(TraceHUDStyle::Good, Alpha),
			CX, BannerY + (40.f * UIScale), FontLarge, 3.0f * UIScale);
	}
}

void ATraceHUD::DrawScoreFlash()
{
	const float Age = Now - ScoreFlashTime;
	if (ScoreFlashTeam == ETraceTeam::None || Age < 0.f || Age > TraceHUDStyle::ScoreFlashDuration)
	{
		return;
	}

	// Fade only over the last third, so the message is at full strength for long enough to read.
	const float FadeStart = TraceHUDStyle::ScoreFlashDuration * 0.66f;
	const float Alpha = (Age <= FadeStart)
		? 1.f
		: 1.f - ((Age - FadeStart) / (TraceHUDStyle::ScoreFlashDuration - FadeStart));

	const FLinearColor TeamColor = TraceTeamColor(ScoreFlashTeam);
	const float CX = ViewW * 0.5f;
	const float Y = ViewH * 0.34f;

	// A team-coloured band behind it: at a glance, the colour alone answers "who scored?". The dark
	// underlay is not optional — the arena is a field of bright emissive strips, and a tint alone
	// loses every legibility contest it enters with the geometry behind it.
	const float BandH = 96.f * UIScale;
	const float BandY = Y - (12.f * UIScale);
	const float Rule = FMath::Max(1.f, 2.f * UIScale);

	DrawRect(FLinearColor(0.f, 0.01f, 0.02f, 0.62f * Alpha), 0.f, BandY, ViewW, BandH);
	DrawRect(TraceHUDStyle::WithAlpha(TeamColor, 0.18f * Alpha), 0.f, BandY, ViewW, BandH);
	DrawRect(TraceHUDStyle::WithAlpha(TeamColor, 0.75f * Alpha), 0.f, BandY, ViewW, Rule);
	DrawRect(TraceHUDStyle::WithAlpha(TeamColor, 0.75f * Alpha), 0.f, BandY + BandH, ViewW, Rule);

	const FString Headline = FString::Printf(TEXT("%s SCORES"), *TraceTeamName(ScoreFlashTeam).ToString().ToUpper());
	DrawTextCentered(Headline, TraceHUDStyle::WithAlpha(TeamColor, Alpha), CX, Y, FontLarge, 2.3f * UIScale);

	// The reset is the thing that felt like a crash, so it is named out loud.
	DrawTextCentered(TEXT("CORE RESET  -  BACK TO SPAWNS"),
		TraceHUDStyle::WithAlpha(TraceHUDStyle::Ink, Alpha * 0.9f),
		CX, Y + (62.f * UIScale), FontSmall, 1.15f * UIScale);
}

// -------------------------------------------------------------------------------------------
// Death / respawn
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawParryKillBanner()
{
	const float FadeSeconds = FMath::Max(0.01f, TraceParry::GetParryKillFeedbackSeconds());

	// SOURCE 1: the authoritative server-side record. On a listen server this is the whole of it —
	// the host's pawn IS the carrier the record names, no replication involved and nothing to drop.
	float SecondsAgo = -1.f;
	FString VictimName;
	bool bHave = TraceParry::GetParryKillFeedback(LocalChar.Get(), SecondsAgo, VictimName);

	// SOURCE 2: the remote-client fallback. A client has no access to that record, so the server
	// sends ClientNotifyParryKill and we age it against OUR clock (which is why the RPC stamps the
	// client's own world time and not the server's).
	if (!bHave && TracePC != nullptr && !TracePC->GetLastParryKillVictim().IsEmpty())
	{
		const float Age = Now - TracePC->GetLastParryKillTime();
		if (Age >= 0.f && Age <= FadeSeconds)
		{
			bHave = true;
			SecondsAgo = Age;
			VictimName = TracePC->GetLastParryKillVictim();
		}
	}

	if (!bHave)
	{
		return;
	}

	// Drawn in the PARRY's own red, from TraceParry::GetTintColor(), and that is not decoration: it
	// is the same colour the whole trace flashed at the instant this happened, so the banner and the
	// world are visibly the same event rather than two things that coincided.
	const float Alpha = FMath::Clamp(1.f - (SecondsAgo / FadeSeconds), 0.f, 1.f);
	const FLinearColor Tint = TraceHUDStyle::WithAlpha(TraceParry::GetTintColor(), Alpha);

	const FString Line = FString::Printf(TEXT("PARRIED - %s DASHED YOUR TRACE"), *VictimName.ToUpper());

	// Below the Core banner, above the crosshair: the carrier is looking at one or the other, and
	// this must never sit on top of either.
	const float CX = ViewW * 0.5f;
	const float BannerY = ViewH * 0.30f;
	const float BannerScale = 1.1f * UIScale;

	const float TextW = MeasureWidth(Line, FontMedium, BannerScale);
	const float TextH = MeasureHeight(Line, FontMedium, BannerScale);
	const float PadX = 16.f * UIScale;
	const float PadY = 5.f * UIScale;

	DrawRect(TraceHUDStyle::WithAlpha(TraceHUDStyle::Shadow, Alpha),
		CX - TextW * 0.5f - PadX, BannerY - PadY, TextW + PadX * 2.f, TextH + PadY * 2.f);
	DrawTextCentered(Line, Tint, CX, BannerY, FontMedium, BannerScale);
}

void ATraceHUD::DrawDeathPanel()
{
	if (!bLocalDead)
	{
		return;
	}

	const float CX = ViewW * 0.5f;
	const float PanelW = 560.f * UIScale;
	const float PanelH = 150.f * UIScale;
	const float PanelX = CX - PanelW * 0.5f;
	const float PanelY = ViewH * 0.42f;

	DrawPanel(PanelX, PanelY, PanelW, PanelH, TraceHUDStyle::PanelFill, TraceHUDStyle::PanelBorder);

	DrawTextCentered(TEXT("ELIMINATED"), TraceHUDStyle::Danger, CX, PanelY + (16.f * UIScale), FontLarge, 1.2f * UIScale);

	// Killer line, if the server told us who did it. "Trail" deaths in particular are worth
	// naming — they are the rule nobody believes until they see it attributed.
	if (TracePC != nullptr && !TracePC->GetLastKillerName().IsEmpty())
	{
		const FName Cause = TracePC->GetLastDeathCause();
		const FString CauseText = Cause.IsNone() ? FString() : FString::Printf(TEXT("  (%s)"), *Cause.ToString());
		const FString KillerLine = FString::Printf(TEXT("by %s%s"), *TracePC->GetLastKillerName(), *CauseText);
		DrawTextCentered(KillerLine, TraceHUDStyle::InkDim, CX, PanelY + (62.f * UIScale), FontMedium, 0.95f * UIScale);

		// SPEC v6 §3 asks for feedback that leaves the dasher in no doubt. "by <carrier> (Parried)"
		// names the fact; this names the RULE, because "Parried" is a brand new cause of death and a
		// player who has never met it will read it as a bug — they dashed a trace, which they know
		// kills the carrier, and instead they died. One extra line is the cheapest possible fix, and
		// it says RED because red is the tell they had 0.2 s to notice and did not.
		if (Cause == TraceParry::GetParryKillCause())
		{
			DrawTextCentered(TEXT("YOU DASHED A PARRIED (RED) TRACE"), TraceParry::GetTintColor(),
				CX, PanelY + (82.f * UIScale), FontSmall, 0.85f * UIScale);
		}
	}

	// Countdown, from the AUTHORITATIVE respawn deadline the player state replicates. It used to be
	// derived from UTraceSettings::RespawnDelay, which spec §1 moved to 3s on ATraceGameMode — so
	// the panel counted down from 5 while the respawn actually fired at 3, and the player was put
	// back in the game while their screen still said "RESPAWN IN 2".
	//
	// LocalDeathTime is kept as the fallback for the frame or two before the player state's deadline
	// has replicated, so the panel never shows a blank or a bogus number on the death frame itself.
	// NOTE the test is on the raw deadline, not on the returned seconds: GetRespawnTimeRemaining()
	// clamps to zero both when no respawn is pending AND when the pending one has elapsed, so using
	// the return value to choose the source would fall back to the stale local estimate for the last
	// moment of every death — the one moment the panel must read "RESPAWNING...".
	const ATracePlayerState* TracePS = (TracePC != nullptr) ? TracePC->GetTracePlayerState() : nullptr;
	const bool bHaveAuthoritativeDeadline = (TracePS != nullptr) && (TracePS->RespawnEndServerTime > 0.f);

	const float Remaining = bHaveAuthoritativeDeadline
		? TracePS->GetRespawnTimeRemaining()
		: (UTraceSettings::Get().RespawnDelay - (Now - LocalDeathTime));
	const FString RespawnText = (Remaining > 0.f)
		? FString::Printf(TEXT("RESPAWN IN %d"), FMath::CeilToInt(Remaining))
		: FString(TEXT("RESPAWNING..."));

	DrawTextCentered(RespawnText, TraceHUDStyle::Ink, CX, PanelY + (100.f * UIScale), FontMedium, 1.05f * UIScale);
}

/**
 * The post-match screen.
 *
 * This is a full takeover rather than a banner over the scoreboard, and it is deliberate: the
 * complaint that the game "keeps stopping and restarting" is what an unannounced end-of-match
 * looks like. So the match ends *loudly* — the world dims, the winner and the final score are
 * stated, both rosters are listed with their K/D, and a live countdown says exactly what happens
 * next and when. TraceMatchFlow::PostMatchDuration is shared with the game mode's return timer, so
 * the number on screen is the number that fires.
 */
void ATraceHUD::DrawMatchResult()
{
	if (TraceGS == nullptr || TraceGS->TraceMatchState != ETraceMatchState::PostMatch)
	{
		return;
	}

	const int32 Blue = TraceGS->GetScore(ETraceTeam::Blue);
	const int32 Orange = TraceGS->GetScore(ETraceTeam::Orange);

	// Dim the whole world. Nothing happening out there matters any more.
	DrawRect(FLinearColor(0.f, 0.01f, 0.02f, 0.82f), 0.f, 0.f, ViewW, ViewH);

	const float CX = ViewW * 0.5f;

	// ---- Who won, and WHY the match stopped (spec v4 §6) ----------------------------------------
	//
	// Both are read from the GameState rather than re-derived from the two scores. That mattered
	// less when the clock was the only way to finish; now that the mercy rule can stop a match in
	// the middle of the first half, "9-1" alone does not say whether the whistle went because time
	// ran out or because the rule fired — and the spec asks for that distinction in as many words,
	// because it is the whole difference to somebody reading a result they did not watch.
	//
	// The score fallback below is for the frame or two before MatchWinner has replicated to a client
	// that joined mid-whistle, not for normal play.
	const ETraceMatchEndReason EndReason = TraceGS->GetMatchEndReason();

	ETraceTeam Winner = TraceGS->GetMatchWinner();
	if (Winner == ETraceTeam::None && Blue != Orange)
	{
		Winner = (Blue > Orange) ? ETraceTeam::Blue : ETraceTeam::Orange;
	}

	FString ResultText;
	FLinearColor ResultColor;
	if (Winner == ETraceTeam::None)
	{
		ResultText = TEXT("DRAW");
		ResultColor = TraceHUDStyle::Ink;
	}
	else
	{
		ResultText = FString::Printf(TEXT("%s WINS"), *TraceTeamName(Winner).ToString().ToUpper());
		ResultColor = TraceTeamColor(Winner);
	}

	// The headline is the REASON, not a fixed "FULL TIME": a mercy win says MERCY RULE. Coloured
	// with it too, so the two outcomes are told apart at a glance from a screenshot.
	const bool bMercy = (EndReason == ETraceMatchEndReason::Mercy);
	DrawTextCentered(TraceMatchEndReasonHeadline(EndReason),
		bMercy ? TraceHUDStyle::Danger : TraceHUDStyle::InkDim,
		CX, ViewH * 0.095f, FontSmall, 1.3f * UIScale);

	DrawTextCentered(ResultText, ResultColor, CX, ViewH * 0.135f, FontLarge, 2.6f * UIScale);

	// One line of plain English under the result. A results screen that says "MERCY RULE" and
	// nothing else leaves the reader to guess the threshold; this states the rule that fired, and in
	// the clock case it states which mode the match was played in — which is the fact an A/B
	// playtest's notes need and the one a screenshot otherwise loses.
	const FString SubText = bMercy
		? FString::Printf(TEXT("%s LED BY %d - THE MATCH ENDED EARLY (%s)"),
			*TraceTeamName(Winner).ToString().ToUpper(), FMath::Abs(Blue - Orange),
			*TraceScoringModeLabel(TraceGS->GetScoringMode()))
		: FString::Printf(TEXT("%s HALVES ON THE CLOCK  -  %s"),
			(TraceGS->NumHalves == 2) ? TEXT("TWO") : TEXT("ALL"),
			*TraceScoringModeLabel(TraceGS->GetScoringMode()));

	DrawTextCentered(SubText, TraceHUDStyle::InkDim, CX, ViewH * 0.205f, FontSmall, 1.05f * UIScale);

	// Final score, spelled out as two team-coloured numbers rather than one string, so the winner
	// is legible from across the room.
	const float ScoreY = ViewH * 0.255f;
	const float ScoreInset = 60.f * UIScale;
	DrawTextRight(FString::FromInt(Blue), TraceTeamColor(ETraceTeam::Blue), CX - ScoreInset, ScoreY, FontLarge, 2.4f * UIScale);
	DrawTextCentered(TEXT("-"), TraceHUDStyle::InkDim, CX, ScoreY, FontLarge, 2.4f * UIScale);
	DrawTextLeft(FString::FromInt(Orange), TraceTeamColor(ETraceTeam::Orange), CX + ScoreInset, ScoreY, FontLarge, 2.4f * UIScale);

	// ---- Roster card --------------------------------------------------------------------------
	// The two columns reuse the live scoreboard's renderer so the two views can never drift apart.
	// The card behind them has to be sized BEFORE they are drawn (Canvas is immediate mode and the
	// panel has to land underneath), so the row count is measured up front.
	int32 BlueRows = 0;
	int32 OrangeRows = 0;
	for (const APlayerState* PlayerState : TraceGS->PlayerArray)
	{
		const ATracePlayerState* TracePlayerState = Cast<ATracePlayerState>(PlayerState);
		if (TracePlayerState == nullptr)
		{
			continue;
		}
		BlueRows   += (TracePlayerState->Team == ETraceTeam::Blue)   ? 1 : 0;
		OrangeRows += (TracePlayerState->Team == ETraceTeam::Orange) ? 1 : 0;
	}
	const int32 MaxRows = FMath::Max(BlueRows, OrangeRows);

	const float CardW = FMath::Min(ViewW - (120.f * UIScale), 1040.f * UIScale);
	const float CardX = (ViewW - CardW) * 0.5f;
	const float CardY = ViewH * 0.40f;
	const float CardPad = 20.f * UIScale;
	// Must match DrawScoreboardTeam's own header/row geometry.
	const float CardH = CardPad * 2.f + (30.f * UIScale) + (4.f * UIScale) + MaxRows * (28.f * UIScale);

	DrawPanel(CardX, CardY, CardW, CardH, FLinearColor(0.01f, 0.02f, 0.03f, 0.90f), TraceHUDStyle::PanelBorder);

	const float Gutter = 22.f * UIScale;
	const float ColumnW = (CardW - CardPad * 2.f - Gutter) * 0.5f;

	DrawScoreboardTeam(ETraceTeam::Blue,   CardX + CardPad, CardY + CardPad, ColumnW);
	DrawScoreboardTeam(ETraceTeam::Orange, CardX + CardPad + ColumnW + Gutter, CardY + CardPad, ColumnW);

	// What happens next, and when. GetMatchTimeRemaining() is useless here — MatchEndServerTime was
	// set to the moment the match ended, not to a future deadline — so the countdown is computed
	// against that timestamp directly, on the same replicated server clock everyone else uses.
	const float Elapsed = static_cast<float>(TraceGS->GetServerWorldTimeSeconds()) - TraceGS->MatchEndServerTime;
	const float Remaining = FMath::Max(0.f, TraceMatchFlow::PostMatchDuration - Elapsed);

	const FString ReturnText = (Remaining > 0.5f)
		? FString::Printf(TEXT("RETURNING TO THE TITLE SCREEN IN %d"), FMath::CeilToInt(Remaining))
		: FString(TEXT("RETURNING TO THE TITLE SCREEN..."));

	// Framed, so it reads as the one live thing left on a frozen screen rather than a stray caption.
	const float StripH = 46.f * UIScale;
	const float StripY = ViewH - (110.f * UIScale);
	const float StripW = FMath::Min(ViewW - (120.f * UIScale), 620.f * UIScale);
	DrawPanel(CX - StripW * 0.5f, StripY, StripW, StripH,
		FLinearColor(0.02f, 0.03f, 0.05f, 0.85f), TraceHUDStyle::PanelBorder);

	DrawTextCentered(ReturnText, TraceHUDStyle::Ink, CX,
		VCenterTextY(ReturnText, FontMedium, 1.05f * UIScale, StripY, StripH), FontMedium, 1.05f * UIScale);
}

// -------------------------------------------------------------------------------------------
// Scoreboard (Tab)
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawScoreboard()
{
	// Hold-Tab only. This used to force itself open at full time as well; DrawMatchResult now owns
	// that moment with a bigger version of the same rosters, and DrawHUD does not call this pass at
	// all once the match is over.
	if (TracePC == nullptr || !TracePC->IsScoreboardOpen())
	{
		return;
	}

	const float PanelW = FMath::Min(ViewW - (80.f * UIScale), 1040.f * UIScale);
	const float PanelH = FMath::Min(ViewH - (160.f * UIScale), 560.f * UIScale);
	const float PanelX = (ViewW - PanelW) * 0.5f;
	const float PanelY = ViewH * 0.5f - PanelH * 0.55f;

	DrawPanel(PanelX, PanelY, PanelW, PanelH, FLinearColor(0.01f, 0.02f, 0.03f, 0.88f), TraceHUDStyle::PanelBorder);

	const int32 Blue = (TraceGS != nullptr) ? TraceGS->GetScore(ETraceTeam::Blue) : 0;
	const int32 Orange = (TraceGS != nullptr) ? TraceGS->GetScore(ETraceTeam::Orange) : 0;
	const float CX = PanelX + PanelW * 0.5f;

	DrawTextCentered(FString::Printf(TEXT("%d  -  %d"), Blue, Orange), TraceHUDStyle::Ink,
		CX, PanelY + (14.f * UIScale), FontLarge, 1.2f * UIScale);

	const float Gutter = 22.f * UIScale;
	const float ColumnW = (PanelW - Gutter * 3.f) * 0.5f;
	const float ColumnY = PanelY + (66.f * UIScale);

	DrawScoreboardTeam(ETraceTeam::Blue,   PanelX + Gutter, ColumnY, ColumnW);
	DrawScoreboardTeam(ETraceTeam::Orange, PanelX + Gutter * 2.f + ColumnW, ColumnY, ColumnW);

	DrawTextCentered(TEXT("HOLD TAB"), TraceHUDStyle::InkDim, CX,
		PanelY + PanelH - (24.f * UIScale), FontSmall, UIScale);
}

float ATraceHUD::DrawScoreboardTeam(ETraceTeam Team, float X, float Y, float Width)
{
	const FLinearColor TeamColor = TraceTeamColor(Team);
	const float RowH = 28.f * UIScale;
	const float HeaderH = 30.f * UIScale;

	// Column anchors. Name is left-aligned; the character and the three numeric columns are
	// right-aligned so their digits line up regardless of width.
	const float NameX   = X + (12.f * UIScale);
	const float CharX   = X + Width * 0.58f;
	const float KillsX  = X + Width * 0.66f;
	const float DeathsX = X + Width * 0.80f;
	const float PingX   = X + Width * 0.97f;

	// ---- Gather this team's players -----------------------------------------------------------
	//
	// BEFORE the header, because whether the character column exists at all is a property of the
	// roster. See bShowCharacters below.
	TArray<ATracePlayerState*> Members;
	if (TraceGS != nullptr)
	{
		Members.Reserve(TraceGS->PlayerArray.Num());
		for (APlayerState* PlayerState : TraceGS->PlayerArray)
		{
			ATracePlayerState* TracePlayerState = Cast<ATracePlayerState>(PlayerState);
			if (TracePlayerState != nullptr && TracePlayerState->Team == Team)
			{
				Members.Add(TracePlayerState);
			}
		}
	}

	// ---- THE CHARACTER COLUMN (spec v15 §2) ---------------------------------------------------
	//
	// "A bot's pick must replicate so the HUD, scoreboard and kill feed show it." Nothing here ever
	// showed a character for ANYBODY — human or bot — so this column is new for both. The value is
	// ATracePlayerState::GetSelectedCharacter(), which forwards to the replicated
	// UTraceAbilityComponent on that player state, so it is already correct on every machine
	// (proven on a remote client by Trace.Characters.Dump's no-authority fallback).
	//
	// DRAWN ONLY WHEN SOMEBODY ON THIS TEAM HOLDS ONE, and that is not tidiness. In mode A and with
	// the characters toggle off, every player is deliberately the Mannequin (spec v14 §2 / §3), and a
	// column reading MANNEQUIN ten times is noise that says nothing — worse, it invites the reader to
	// think something failed. No character on the team means the scoreboard is pixel-identical to the
	// one that shipped before this pass.
	bool bShowCharacters = false;
	for (const ATracePlayerState* Member : Members)
	{
		if (Member != nullptr && Member->HasCharacter())
		{
			bShowCharacters = true;
			break;
		}
	}

	// ---- Header -------------------------------------------------------------------------------
	DrawRect(TraceHUDStyle::WithAlpha(TeamColor, 0.25f), X, Y, Width, HeaderH);

	const FString TeamLabel = TraceTeamName(Team).ToString().ToUpper();
	DrawTextLeft(TeamLabel, TeamColor, NameX, VCenterTextY(TeamLabel, FontMedium, UIScale, Y, HeaderH), FontMedium, UIScale);

	const float HeaderTextY = VCenterTextY(TEXT("K"), FontSmall, UIScale, Y, HeaderH);
	if (bShowCharacters)
	{
		DrawTextRight(TEXT("CHAR"), TraceHUDStyle::InkDim, CharX, HeaderTextY, FontSmall, UIScale);
	}
	DrawTextRight(TEXT("K"),    TraceHUDStyle::InkDim, KillsX,  HeaderTextY, FontSmall, UIScale);
	DrawTextRight(TEXT("D"),    TraceHUDStyle::InkDim, DeathsX, HeaderTextY, FontSmall, UIScale);
	DrawTextRight(TEXT("PING"), TraceHUDStyle::InkDim, PingX,   HeaderTextY, FontSmall, UIScale);

	// Best first. TArray's pointer overload of Sort wraps the predicate in TDereferenceWrapper,
	// which is why the lambda takes references rather than pointers.
	Members.Sort([](const ATracePlayerState& A, const ATracePlayerState& B)
	{
		if (A.Kills != B.Kills)
		{
			return A.Kills > B.Kills;
		}
		return A.Deaths < B.Deaths;
	});

	// ---- Rows ---------------------------------------------------------------------------------
	const float RowTextScale = 1.05f * UIScale;
	float RowY = Y + HeaderH + (4.f * UIScale);

	for (const ATracePlayerState* Member : Members)
	{
		const bool bIsLocal = (Member == LocalPS.Get());
		if (bIsLocal)
		{
			// Subtle highlight so you can find yourself instantly.
			DrawRect(TraceHUDStyle::WithAlpha(TeamColor, 0.14f), X, RowY - (2.f * UIScale), Width, RowH);
		}

		FString Name = Member->GetPlayerName();
		if (Member->bIsCarrier)
		{
			// ASCII only: the engine's built-in fonts carry no glyph for anything fancier.
			Name += TEXT("  [CORE]");
		}

		const FLinearColor RowColor = bIsLocal ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim;
		const float TextY = VCenterTextY(Name, FontSmall, RowTextScale, RowY, RowH);

		DrawTextLeft(Name, RowColor, NameX, TextY, FontSmall, RowTextScale);
		if (bShowCharacters)
		{
			// The roster's OWN name, not a second table. Tinted with that character's accent so the
			// column is scannable at a glance, dimmed on somebody else's row exactly as the numbers
			// are. A player the roster could not serve reads MANNEQUIN, which is a real state and
			// not a fault — see TraceCharacterRoster::NameFor's comment on why it never says "NONE".
			const uint8 HeldCharacterId = Member->GetSelectedCharacter();
			const FString CharacterLabel = TraceCharacterRoster::NameFor(HeldCharacterId);

			FLinearColor CharacterColor = RowColor;
			if (const TraceCharacterRoster::FTraceCharacterEntry* RosterEntry = TraceCharacterRoster::Find(HeldCharacterId))
			{
				CharacterColor = bIsLocal ? RosterEntry->Accent
				                          : TraceHUDStyle::WithAlpha(RosterEntry->Accent, 0.75f);
			}

			DrawTextRight(CharacterLabel, CharacterColor, CharX, TextY, FontSmall, RowTextScale);
		}
		DrawTextRight(FString::FromInt(Member->Kills), RowColor, KillsX, TextY, FontSmall, RowTextScale);
		DrawTextRight(FString::FromInt(Member->Deaths), RowColor, DeathsX, TextY, FontSmall, RowTextScale);
		DrawTextRight(FString::FromInt(FMath::RoundToInt(Member->GetPingInMilliseconds())), RowColor,
			PingX, TextY, FontSmall, RowTextScale);

		RowY += RowH;
	}

	return (RowY - Y);
}

// -------------------------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawTextLeft(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale)
{
	DrawText(Text, Color, X, Y, Font, Scale);
}

void ATraceHUD::DrawTextCentered(const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale)
{
	DrawText(Text, Color, CenterX - MeasureWidth(Text, Font, Scale) * 0.5f, Y, Font, Scale);
}

void ATraceHUD::DrawTextRight(const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale)
{
	DrawText(Text, Color, RightX - MeasureWidth(Text, Font, Scale), Y, Font, Scale);
}

float ATraceHUD::MeasureWidth(const FString& Text, UFont* Font, float Scale)
{
	float OutWidth = 0.f;
	float OutHeight = 0.f;
	// AHUD::GetTextSize substitutes the medium font when Font is null, so this is null-safe.
	GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
	return OutWidth;
}

float ATraceHUD::MeasureHeight(const FString& Text, UFont* Font, float Scale)
{
	float OutWidth = 0.f;
	float OutHeight = 0.f;
	GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
	return OutHeight;
}

float ATraceHUD::VCenterTextY(const FString& Text, UFont* Font, float Scale, float BoxY, float BoxH)
{
	return BoxY + (BoxH - MeasureHeight(Text, Font, Scale)) * 0.5f;
}

void ATraceHUD::DrawPanel(float X, float Y, float W, float H, const FLinearColor& Fill, const FLinearColor& Border)
{
	DrawRect(Fill, X, Y, W, H);

	// Canvas has no stroked-rect primitive, so the border is four thin fills.
	const float T = FMath::Max(1.f, 1.5f * UIScale);
	DrawRect(Border, X, Y, W, T);
	DrawRect(Border, X, Y + H - T, W, T);
	DrawRect(Border, X, Y, T, H);
	DrawRect(Border, X + W - T, Y, T, H);
}

void ATraceHUD::DrawMeter(float X, float Y, float W, float H, float Fraction, const FLinearColor& FillColor)
{
	const float Edge = FMath::Max(1.f, 1.f * UIScale);

	DrawRect(TraceHUDStyle::Shadow, X - Edge, Y - Edge, W + Edge * 2.f, H + Edge * 2.f);
	DrawRect(TraceHUDStyle::Trough, X, Y, W, H);

	const float FillW = FMath::Clamp(Fraction, 0.f, 1.f) * W;
	if (FillW > 0.f)
	{
		DrawRect(FillColor, X, Y, FillW, H);
	}
}

FString ATraceHUD::FormatClock(float Seconds)
{
	const int32 Total = FMath::Max(0, FMath::CeilToInt(Seconds));
	return FString::Printf(TEXT("%02d:%02d"), Total / 60, Total % 60);
}

// ===================================================================================================
// SPEC v16 §2 — THE EVIDENCE HARNESS
//
//   Trace.HUD.V16.Report        one line describing WHAT THE LAST DRAWN FRAME CONTAINED.
//   Trace.HUD.V16Shots          stages real gameplay states and photographs the corner, the bee
//                               clip, the reload, four statuses and the charge ring — RED ARM FIRST,
//                               then the same fixture again with the feature on.
//
// ---------------------------------------------------------------------------------------------------
// WHY IT IS BUILT THIS WAY
// ---------------------------------------------------------------------------------------------------
//
// A drawing change can only be verified by looking at a frame, and this project has already been
// caught three times by a harness that certified itself: a wall fitter that indexed 0 boxes while
// reporting healthy, a fix that existed in code but not on disk, a carrier test that printed PASS
// while striking a corpse. So two rules govern everything below.
//
//   1. THE REPORT READS THE DRAW RECORD, NOT THE GAME. Every field it prints was written by the draw
//      pass at the moment it emitted pixels — the ammo string that was rendered, the number of
//      magazine ticks actually issued, the number of ring chords actually issued, one entry per
//      status chip. Ask the weapon component instead and a HUD that draws nothing still reports 30.
//
//   2. RED BEFORE GREEN, SAME BINARY, SAME FIXTURE, SAME CAMERA. Trace.HUD.V16 0 restores the
//      pre-v16 HUD exactly — no corner, and the throw charge back on its bottom-left bar. The
//      sequence photographs the red arm FIRST, from a world already carrying the poison, the three
//      vulnerable stacks and the part-spent clip, and only then flips the arm and photographs the
//      identical frame again. A "before" that came from a different build or a different fixture
//      would prove nothing at all.
//
// SERVER ONLY, and locally controlled: it stages authoritative state (a poison component, vulnerable
// stacks, a character swap) and photographs the machine that owns a viewport. On a listen host those
// are the same process, which is what every headless run of this project already is.
// ===================================================================================================

#if !UE_BUILD_SHIPPING

ATraceHUD::FV16DrawRecord ATraceHUD::GetV16DrawRecord() const
{
	FV16DrawRecord Record;
	Record.bAmmoBlock = bDrewAmmoBlock;
	Record.AmmoText = DrawnAmmoText;
	Record.bBeeClip = bDrewBeeClip;
	Record.bReloadBar = bDrewReloadBar;
	Record.MagazineTicks = DrawnMagazineTicks;
	Record.ChipCount = DrawnStatusChips.Num();
	Record.bChargeRing = bDrewChargeRing;
	Record.ChargeRingAlpha = DrawnChargeRingAlpha;
	Record.ChargeRingChords = DrawnChargeRingSegments;
	Record.bChargeBar = bDrewChargeBar;

	for (const FString& Chip : DrawnStatusChips)
	{
		Record.ChipText += FString::Printf(TEXT("\n[HUDV16]     chip: %s"), *Chip);
	}
	if (DrawnStatusChips.Num() == 0)
	{
		Record.ChipText = TEXT("\n[HUDV16]     chip: <none>");
	}
	return Record;
}

void ATraceHUD::LogV16DrawRecord(const TCHAR* Tag) const
{
	const FV16DrawRecord Record = GetV16DrawRecord();

	UE_LOG(LogTraceGame, Display,
		TEXT("[HUDV16] %s | arm=%d | ammoBlock=%s text='%s' bee=%s reloadBar=%s litTicks=%d | chips=%d%s")
		TEXT("\n[HUDV16]     chargeRing=%s alpha=%.3f chords=%d | SUPERSEDED chargeBar=%s"),
		Tag,
		TraceHUDV16::GArmed,
		Record.bAmmoBlock ? TEXT("YES") : TEXT("no"),
		*Record.AmmoText,
		Record.bBeeClip ? TEXT("YES") : TEXT("no"),
		Record.bReloadBar ? TEXT("YES") : TEXT("no"),
		Record.MagazineTicks,
		Record.ChipCount,
		*Record.ChipText,
		Record.bChargeRing ? TEXT("YES") : TEXT("no"),
		Record.ChargeRingAlpha,
		Record.ChargeRingChords,
		Record.bChargeBar ? TEXT("YES") : TEXT("no"));
}

/**
 * The staging + capture sequence. Named after the file, per the build contract's jumbo rule.
 */
namespace TraceHUDV16Shots
{
	/** One scripted run. Real-time gated, so it survives a stalled or slow headless frame. */
	struct FRun
	{
		int32 Step = 0;
		double NextRealTime = 0.0;
		int32 TicksLeft = 40000;
		bool bModeB = false;
		TWeakObjectPtr<ATraceHUD> Hud;
		TWeakObjectPtr<APlayerController> PC;

		/** Every assertion this run made, so the last line of the log is a verdict and not a vibe. */
		TArray<FString> Passes;
		TArray<FString> Failures;

		/**
		 * Scenes this world CANNOT stage, as opposed to scenes that failed.
		 *
		 * MODE A FREEZES THE CHARACTER SYSTEM OUTRIGHT ("characters are OFF here (mode A is frozen -
		 * spec 2)"), so there is no X to load a bee clip, no Chut to raise Chud, no Rocco to stack a
		 * boost and no Mace to suspend. Reporting those as FAILURES would be a harness crying wolf
		 * about a rule the game is enforcing on purpose; reporting them as passes would be worse.
		 * They get their own list and the verdict names it.
		 */
		TArray<FString> NotApplicable;

		/** False when this world refuses characters at all - see NotApplicable. */
		bool bCharacters = true;
	};

	/** The one local, authoritative player controller - the only machine that can both stage and draw. */
	APlayerController* FindLocalAuthorityPC(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}

		// Listen host or standalone only: this command stages AUTHORITATIVE state and photographs a
		// VIEWPORT, and only these two net modes have both in one process.
		if (!WorldPtr->IsNetMode(NM_ListenServer) && !WorldPtr->IsNetMode(NM_Standalone))
		{
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			if (APlayerController* Candidate = It->Get())
			{
				if (Candidate->IsLocalController())
				{
					return Candidate;
				}
			}
		}
		return nullptr;
	}

	/** The local controller's pass progress, or -1. Sugar; the pass ring is fed by exactly this. */
	float TracePC_PassProgress(const FRun& Run)
	{
		const ATracePlayerController* PC = Cast<ATracePlayerController>(Run.PC.Get());
		return (PC != nullptr) ? PC->GetPassProgress() : -1.f;
	}

	ATraceCharacter* PawnOf(const FRun& Run)
	{
		APlayerController* PC = Run.PC.Get();
		return (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	}

	void SetCVarInt(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	void SetArm(int32 Value) { SetCVarInt(TEXT("Trace.HUD.V16"), Value); }

	/** One assertion against the DRAW RECORD. Never against the gameplay state that fed it. */
	void Expect(FRun& Run, bool bCondition, const FString& Claim)
	{
		if (bCondition)
		{
			Run.Passes.Add(Claim);
		}
		else
		{
			Run.Failures.Add(Claim);
			UE_LOG(LogTraceGame, Warning, TEXT("[HUDV16] *** FAIL: %s ***"), *Claim);
		}
	}

	/**
	 * Requests a full-frame capture and logs the draw record alongside it.
	 *
	 * Filenames are namespaced with the tag AND this process id, because other agents run their own
	 * rigs into the same Saved/Screenshots and an overwritten frame is destroyed evidence.
	 *
	 * *** bShowUI IS TRUE, AND IT WAS FALSE UNTIL SPEC v17 §4 (step 4b). MEASURED. *** The Canvas HUD
	 * draws through FCanvas into the scene, so it lands in a screenshot either way — but a UMG widget
	 * is a SLATE widget, composited after the scene, and bShowUI=false excludes it outright. The
	 * first armed capture of the UMG corner proved it the hard way: the draw record said
	 * "ammoBlock=YES text='26/30' litTicks=26 chips=3" and the photograph of that very frame had an
	 * empty bottom-right corner, with the Canvas score panel and health bar plainly visible in the
	 * same image. Nothing was wrong with the corner; the camera could not see that class of thing.
	 *
	 * So this is not a cosmetic change to an existing harness. With bShowUI=false the harness CANNOT
	 * photograph the element spec v17 §4 converted, which would leave a drawing change verified only
	 * by a log — precisely the self-certifying evidence this whole file exists to refuse. The Canvas
	 * arm's frames are unaffected (checked: same corner, same pixels), so the before/after pair the
	 * red arm produces is still a like-for-like comparison.
	 */
	ATraceHUD::FV16DrawRecord Shot(FRun& Run, const TCHAR* Tag)
	{
		const FString FileName = FString::Printf(TEXT("HudV16_%s_pid%d.png"), Tag, FPlatformProcess::GetCurrentProcessId());
		const FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);

		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(FullPath));
		FScreenshotRequest::RequestScreenshot(FullPath, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);

		UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] shot requested: %s"), *FullPath);

		ATraceHUD* HudPtr = Run.Hud.Get();
		if (HudPtr == nullptr)
		{
			return ATraceHUD::FV16DrawRecord();
		}
		HudPtr->LogV16DrawRecord(Tag);
		return HudPtr->GetV16DrawRecord();
	}

	/** An ability component belonging to somebody on the OTHER team, or null. */
	UTraceAbilityComponent* FindEnemySource(UWorld* WorldPtr, const ATraceCharacter* Victim)
	{
		const AGameStateBase* GS = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (GS == nullptr || Victim == nullptr)
		{
			return nullptr;
		}

		for (APlayerState* Candidate : GS->PlayerArray)
		{
			const ATracePlayerState* TraceState = Cast<ATracePlayerState>(Candidate);
			if (TraceState == nullptr || TraceState == Victim->GetPlayerState())
			{
				continue;
			}
			if (TraceState->Team == Victim->GetTeam())
			{
				continue;
			}
			if (UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Candidate))
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/**
	 * Keeps the two inflicted debuffs live across a multi-second scene, at a STABLE three stacks.
	 *
	 * Re-applied on a threshold rather than every tick: vulnerable refreshes one shared 2 s deadline
	 * and adds a stack each time, so a per-frame re-application would race to the cap of five and the
	 * screenshot would show a number nobody asked for. Topping up only below three keeps the frame
	 * reading "x3 +35%", which is the arithmetic spec v16 4 spells out by name.
	 *
	 * *** THE POISON IS APPLIED BY AN ENEMY, AND THAT IS NOT COSMETIC. *** The first run of this
	 * harness attributed it to the victim's own ability component; the poison attached and the
	 * POISONED chip drew, but the 4 slice's choke point correctly refused to SLOW somebody on
	 * account of themselves, so bSlowActive stayed false and the SLOWED chip never appeared. The
	 * fixture was wrong, not the HUD - but a fixture that cannot produce a state cannot photograph
	 * one either.
	 */
	void HoldDebuffs(UWorld* WorldPtr, ATraceCharacter* Target, APlayerController* Source)
	{
		if (Target == nullptr || !Target->IsAlive())
		{
			return;
		}

		// *** THE MARK IS REBUILT BEFORE IT EXPIRES, NOT AFTER. ***
		//
		// Spec v16 4 is that all stacks vanish together the instant the deadline passes, and
		// GetVulnerableStacks() implements exactly that - so a fixture that only tops up once the
		// count has already fallen to zero leaves a window, one tick wide, in which the correct
		// answer is "no mark". Two captures landed in it and reported the vulnerable chip missing.
		// The HUD was right both times; the fixture was letting the thing it was photographing
		// legitimately end.
		//
		// Rebuilt rather than topped up: ApplyVulnerable ADDS a stack as well as resetting the
		// deadline, so refreshing early would walk the count to the cap of five and the frame would
		// read "x5 +45%" instead of the "x3 +35%" this scene is meant to show. Clearing first pins it
		// at exactly three, and the 0.75 s threshold keeps the deadline more than a second away from
		// any frame the camera might catch.
		if (UTraceHealthComponent* HealthComp = Target->Health.Get())
		{
			const float MarkDuration = TraceVulnerable::GetDurationSeconds();
			if (HealthComp->GetVulnerableStacks() != 3 || HealthComp->GetVulnerableRemaining() < 0.75f)
			{
				HealthComp->ClearVulnerable();
				for (int32 Stack = 0; Stack < 3; ++Stack)
				{
					HealthComp->ApplyVulnerable(MarkDuration, Source);
				}
			}
		}

		const UTraceOysterPoisonComponent* Live = UTraceOysterPoisonComponent::Find(Target);
		const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		const float MatchNow = (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;

		if (Live == nullptr || (Live->GetEndMatchTime() - MatchNow) < 1.5f)
		{
			UTraceOysterPoisonComponent::ApplyTo(Target, FindEnemySource(WorldPtr, Target));
		}
	}

	bool Tick(TSharedPtr<FRun> Run);

	void Schedule(TSharedPtr<FRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + static_cast<double>(DelaySeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					// Debuffs decay in real time while a scene is being held for the camera.
					if (Run->Step >= 3 && Run->Step <= 11)
					{
						APlayerController* PC = Run->PC.Get();
						HoldDebuffs(PC ? PC->GetWorld() : nullptr, PawnOf(*Run), PC);
					}
					return (--Run->TicksLeft) > 0;
				}
				return Tick(Run);
			}), 0.f);
	}

	/** Runs one console command through the engine, so the shipped path is what gets exercised. */
	void Exec(UWorld* WorldPtr, const TCHAR* Command)
	{
		if (GEngine != nullptr && WorldPtr != nullptr)
		{
			GEngine->Exec(WorldPtr, Command);
		}
	}

	/** Puts the local player on @p CharacterName and says whether it took. */
	bool SwitchCharacter(UWorld* WorldPtr, const FRun& Run, const TCHAR* CharacterName, ETraceCharacterId Expected)
	{
		Exec(WorldPtr, *FString::Printf(TEXT("Trace.Ability.SetCharacter %s"), CharacterName));

		const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PawnOf(Run));
		const bool bTook = (Comp != nullptr) && (Comp->GetCharacterId() == Expected);
		if (!bTook)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[HUDV16] character switch to %s did NOT take (now %d). Any status that needs it will be "
				     "reported as NOT PHOTOGRAPHED rather than quietly skipped."),
				CharacterName, Comp != nullptr ? (int32)Comp->GetCharacterId() : -1);
		}
		return bTook;
	}

	bool Tick(TSharedPtr<FRun> Run)
	{
		ATraceHUD* HudPtr = Run->Hud.Get();
		APlayerController* PC = Run->PC.Get();
		UWorld* WorldPtr = (PC != nullptr) ? PC->GetWorld() : nullptr;

		if (HudPtr == nullptr || PC == nullptr || WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[HUDV16] *** ABORTED at step %d: the HUD or the local controller went away. "
				"This is NOT a pass. ***"), Run->Step);
			return false;
		}

		const int32 ThisStep = Run->Step++;
		float NextDelay = 1.0f;

		switch (ThisStep)
		{
		case 0:
			// FIXTURE FIRST, AND OUT LOUD. A run that photographed a HUD with no pawn would produce a
			// set of empty frames and a green-looking log.
			UE_LOG(LogTraceGame, Display,
				TEXT("[HUDV16] ===== spec v16 2 shot sequence ===== mode=%s pawn=%s alive=%d"),
				Run->bModeB ? TEXT("B (goals)") : TEXT("A (endzones)"),
				*GetNameSafe(PawnOf(*Run)), PawnOf(*Run) != nullptr ? (int32)PawnOf(*Run)->IsAlive() : 0);

			// The per-team uniqueness rule hands three of the five characters to bots before this
			// command ever runs, so a fixture that has to BE Chut, Rocco and Mace in turn cannot get
			// there without lifting it. This is the framework's own documented dev arm, not a new one,
			// and it is a FIXTURE change: nothing about the HUD reads it.
			Exec(WorldPtr, TEXT("Trace.Characters.EnforceRosterRules 0"));

			// Mode B refuses to give the Core back to whoever last threw it for a short window. The
			// ring scene picks it up three times in six seconds, so the lockout is stood down for the
			// fixture. Also a gameplay-side knob the HUD never reads.
			SetCVarInt(TEXT("Trace.ModeB.SelfPickupLockout"), 0);

			Run->bCharacters = SwitchCharacter(WorldPtr, *Run, TEXT("X"), ETraceCharacterId::X);
			if (!Run->bCharacters)
			{
				Run->NotApplicable.Add(TEXT("BEE ROUNDS, CHUD, SPEED BOOST and SUSPEND - this world refuses "
					"characters outright (mode A is frozen, spec 2), so none of the four can be staged here. "
					"Run the same command in a mode B world to photograph them."));
			}
			NextDelay = 1.5f;
			break;

		case 1:
			// Spend part of the clip through the REAL trigger, so the count on screen is one the gun
			// arrived at by firing rather than one the harness wrote into it.
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				if (UTraceWeaponComponent* Gun = Pawn->Weapon.Get())
				{
					Gun->StartFire();
				}
			}
			NextDelay = 1.6f;
			break;

		case 2:
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				if (UTraceWeaponComponent* Gun = Pawn->Weapon.Get())
				{
					Gun->StopFire();
				}
			}
			HoldDebuffs(WorldPtr, PawnOf(*Run), PC);
			NextDelay = 0.6f;
			break;

		case 3:
			// *** RED ARM. The pre-v16 HUD, over a world that already has everything to show. ***
			SetArm(0);
			HoldDebuffs(WorldPtr, PawnOf(*Run), PC);
			NextDelay = 0.5f;
			break;

		case 4:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("RED_corner"));
			Expect(*Run, !Record.bAmmoBlock, TEXT("RED: the bottom-right corner draws NO ammo block"));
			Expect(*Run, Record.ChipCount == 0, TEXT("RED: the bottom-right corner draws NO status chips"));
			NextDelay = 1.2f;
			break;
		}

		case 5:
			SetArm(1);
			HoldDebuffs(WorldPtr, PawnOf(*Run), PC);
			NextDelay = 0.5f;
			break;

		case 6:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("GREEN_corner"));
			Expect(*Run, Record.bAmmoBlock, TEXT("GREEN: the ammo block draws"));
			Expect(*Run, Record.MagazineTicks > 0 && Record.MagazineTicks < 30,
				TEXT("GREEN: the magazine strip emitted a PART-SPENT number of lit ticks (not 0, not a full clip)"));
			Expect(*Run, Record.AmmoText.Contains(TEXT("/30")), TEXT("GREEN: the ammo readout is out of a 30-round clip"));
			Expect(*Run, !Record.bBeeClip, TEXT("GREEN: an ordinary clip is NOT flagged as bee rounds"));
			Expect(*Run, Record.ChipCount >= 3,
				TEXT("GREEN: at least three status chips drew (vulnerable + poisoned + slowed)"));
			Expect(*Run, Record.ChipText.Contains(TEXT("VULNERABLE  x3  +35%")),
				TEXT("GREEN: the vulnerable chip shows the STACK COUNT and spec v16 4's arithmetic (x3 = +35%)"));
			Expect(*Run, Record.ChipText.Contains(TEXT("POISONED")), TEXT("GREEN: the poisoned chip drew"));
			Expect(*Run, Record.ChipText.Contains(TEXT("SLOWED")), TEXT("GREEN: the slowed chip drew"));
			Expect(*Run, !Record.ChipText.Contains(TEXT("drain=0.00")),
				TEXT("GREEN: every chip drew a NON-EMPTY draining indicator"));
			NextDelay = 1.2f;
			break;
		}

		case 7:
			// The reload state of the ammo block: a third strip shape and a live countdown.
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				if (UTraceWeaponComponent* Gun = Pawn->Weapon.Get())
				{
					const bool bStarted = Gun->RequestReload();
					UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] manual reload requested: started=%d"), (int32)bStarted);
				}
			}
			NextDelay = 0.15f;
			break;

		case 8:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("GREEN_reload"));
			Expect(*Run, Record.bReloadBar, TEXT("GREEN: mid-reload the magazine strip becomes a filling reload bar"));
			NextDelay = 1.2f;
			break;
		}

		case 9:
			if (!Run->bCharacters)
			{
				Run->Step = 21;   // straight to the ring scene; there is no Sting to photograph
				NextDelay = 0.1f;
				break;
			}
			// STING: the clip becomes five bee rounds. This is spec v16 1's "or it reads as the gun
			// eating your ammo" case, and the only frame that can answer it.
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				if (UTraceCharacterAbilitySet* Set = UTraceAbilityComponent::GetAbilitySetFor(Pawn))
				{
					const bool bActivated = Set->ActivateAbility();
					UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] X Sting activated=%d"), (int32)bActivated);
				}
			}
			NextDelay = 0.6f;
			break;

		case 10:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("GREEN_bee"));
			Expect(*Run, Record.bBeeClip, TEXT("BEE: the ammo block flags the clip as bee rounds"));
			Expect(*Run, Record.AmmoText.Contains(TEXT("/5")),
				TEXT("BEE: the denominator is the 5-round bee clip, not 30 - so 5 rounds does not read as nearly empty"));
			Expect(*Run, Record.MagazineTicks == 5,
				TEXT("BEE: the magazine strip emitted FIVE fat pips, a different shape from the 30-tick strip"));
			NextDelay = 1.2f;
			break;
		}

		case 11:
			Run->Step = 12;
			if (SwitchCharacter(WorldPtr, *Run, TEXT("Chut"), ETraceCharacterId::Chut))
			{
				NextDelay = 1.2f;
			}
			else
			{
				Run->Step = 14;   // skip Chud rather than photograph an empty frame and call it evidence
				NextDelay = 0.1f;
			}
			break;

		case 12:
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				if (UTraceCharacterAbilitySet* Set = UTraceAbilityComponent::GetAbilitySetFor(Pawn))
				{
					const bool bActivated = Set->ActivateAbility();
					UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] Chut Chud activated=%d"), (int32)bActivated);
				}
			}
			NextDelay = 0.6f;
			break;

		case 13:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("GREEN_chud"));
			Expect(*Run, Record.ChipText.Contains(TEXT("CHUD")), TEXT("GREEN: the Chud chip drew, with its damage reduction"));
			NextDelay = 1.2f;
			break;
		}

		case 14:
			Run->Step = 15;
			if (SwitchCharacter(WorldPtr, *Run, TEXT("Rocco"), ETraceCharacterId::Rocco))
			{
				NextDelay = 1.2f;
			}
			else
			{
				Run->Step = 17;
				NextDelay = 0.1f;
			}
			break;

		case 15:
			// Three headshot kills through Rocco's OWN passive hook - the same call the kill router
			// makes - rather than by writing his replicated stack count from outside.
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				if (UTraceCharacterAbilitySet* Set = UTraceAbilityComponent::GetAbilitySetFor(Pawn))
				{
					for (int32 Kill = 0; Kill < 3; ++Kill)
					{
						Set->OnKill(nullptr, FName(TEXT("Bullet")), /*bHeadshot=*/true);
					}
				}
			}
			NextDelay = 0.3f;
			break;

		case 16:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("GREEN_speed"));
			Expect(*Run, Record.ChipText.Contains(TEXT("SPEED BOOST  x3")),
				TEXT("GREEN: the speed-boost chip drew with its stack count"));
			NextDelay = 1.2f;
			break;
		}

		case 17:
			Run->Step = 18;
			if (SwitchCharacter(WorldPtr, *Run, TEXT("Mace"), ETraceCharacterId::Mace))
			{
				NextDelay = 1.2f;
			}
			else
			{
				Run->Step = 21;   // straight to the ring scene; do not photograph a Mace chip without a Mace
				NextDelay = 0.1f;
			}
			break;

		case 18:
			// Mace's suspend only exists in the air, so the fixture has to put her there first - a
			// suspend staged on the ground would be a status chip over a mechanic that is not running.
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				Pawn->LaunchCharacter(FVector(0.f, 0.f, 1600.f), true, true);
			}
			NextDelay = 0.3f;
			break;

		case 19:
			// *** RE-LAUNCHED IMMEDIATELY BEFORE THE PRESS, AND THAT IS A FIX, NOT BELT-AND-BRACES.
			// *** One 900 uu/s hop gave roughly 1.4 s of airtime, which sounds like plenty against a
			// 1.25 s suspend - but this run is frame-rate bound and headless frame times wander, and
			// one capture landed with the pawn already back on the deck: OnSecondaryPressed returned
			// true on a frame where IsFalling() was still set, and ApplySuspend then stopped it with
			// "landed" before the next draw. The chip was correctly absent and the harness correctly
			// failed - the FIXTURE was flaky. Boosting on the same tick as the press removes the
			// dependency on how long the previous step actually took.
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				Pawn->LaunchCharacter(FVector(0.f, 0.f, 1600.f), true, true);

				if (UTraceCharacterAbilitySet* Set = UTraceAbilityComponent::GetAbilitySetFor(Pawn))
				{
					const bool bSuspended = Set->OnSecondaryPressed();
					UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] Mace suspend pressed=%d (falling=%d)"),
						(int32)bSuspended,
						Pawn->GetCharacterMovement() != nullptr ? (int32)Pawn->GetCharacterMovement()->IsFalling() : -1);
				}
			}
			// A SEPARATE STEP FOR THE SHOT, and every other scene above already had one. The draw
			// record is written by DrawHUD, so reading it in the same tick as the state change reads
			// the frame BEFORE the change - which is exactly how the first armed run reported "the
			// suspend chip did not draw" for a chip that was about to. The suspend runs 1.25 s, so
			// two tenths is comfortably inside it.
			NextDelay = 0.2f;
			break;

		case 20:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("GREEN_suspend"));
			Expect(*Run, Record.ChipText.Contains(TEXT("SUSPENDED")), TEXT("GREEN: the suspend chip drew"));
			Expect(*Run, !Record.ChipText.Contains(TEXT("SUSPENDED | 0.00s")),
				TEXT("GREEN: the suspend chip carries a live countdown, not a frozen one"));
			NextDelay = 1.2f;
			break;
		}

		case 21:
			// ---- THE RING SCENE ------------------------------------------------------------------
			//
			// MODE A GETS A SCENE OF ITS OWN RATHER THAN A SKIP, and it is a REGRESSION test, not a
			// bonus. Spec v16 2's ring is mode A's own pass-hold animation, and this pass moved the
			// drawing out of DrawPassProgress into a shared DrawCrosshairRing so that both callers
			// are one animation. That refactor can only break in mode A, which is exactly the world
			// the charge scene cannot run in - so mode A photographs the PASS ring instead and
			// proves the shared geometry still serves its original caller.
			if (!Run->bModeB)
			{
				// *** A PASS NEEDS A RECEIVER, AND THE FIRST ATTEMPT DID NOT STAGE ONE. *** Holding
				// LMB with nobody under the reticle leaves GetPassProgress() at -1 and draws no ring
				// at all, which the harness dutifully reported as a failure of a feature that was
				// never exercised. So the fixture puts a teammate in front of the carrier and turns
				// the carrier to face them before it touches the input.
				if (ATraceCharacter* Pawn = PawnOf(*Run))
				{
					ATraceCharacter* Receiver = nullptr;
					for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
					{
						ATraceCharacter* Candidate = *It;
						if (Candidate != nullptr && Candidate != Pawn && Candidate->IsAlive()
							&& Candidate->GetTeam() == Pawn->GetTeam())
						{
							Receiver = Candidate;
							break;
						}
					}

					if (Receiver != nullptr)
					{
						const FVector Ahead = Pawn->GetActorLocation() + (Pawn->GetActorForwardVector() * 700.f);
						Receiver->SetActorLocation(Ahead, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
						PC->SetControlRotation((Ahead - Pawn->GetActorLocation()).Rotation());
					}

					if (ATraceGameState* GS = WorldPtr->GetGameState<ATraceGameState>())
					{
						if (ATraceCore* CorePtr = GS->Core)
						{
							CorePtr->SetActorLocation(Pawn->GetActorLocation());
							CorePtr->TryPickup(Pawn);
							UE_LOG(LogTraceGame, Display,
								TEXT("[HUDV16] mode A: receiver=%s staged; carrier=%d"),
								*GetNameSafe(Receiver), (int32)Pawn->IsCarrier());
						}
					}
				}
				Run->Step = 199;
				NextDelay = 0.35f;
				break;
			}

			// Put the Core in the local player's hands through the shipped pickup, then start the
			// wind-up through the shipped input entry point - the same call the left mouse button
			// makes. Nothing here writes the charge clock directly.
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				if (ATraceGameState* GS = WorldPtr->GetGameState<ATraceGameState>())
				{
					if (ATraceCore* CorePtr = GS->Core)
					{
						CorePtr->SetActorLocation(Pawn->GetActorLocation());
						CorePtr->TryPickup(Pawn);
						UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] Core pickup attempted; carrier=%d"),
							(int32)Pawn->IsCarrier());
					}
				}
			}
			NextDelay = 0.4f;
			break;

		case 22:
			SetArm(0);                                    // *** RED ARM: the superseded BAR. ***
			if (ATraceGameState* GS = WorldPtr->GetGameState<ATraceGameState>())
			{
				if (ATraceCore* CorePtr = GS->Core)
				{
					CorePtr->RequestPassInput(true, PawnOf(*Run));
				}
			}
			NextDelay = 0.45f;
			break;

		case 23:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("RED_chargebar"));
			Expect(*Run, Record.bChargeBar, TEXT("RED: the throw charge draws as a BAR in the bottom-left stack"));
			Expect(*Run, !Record.bChargeRing, TEXT("RED: no ring is drawn around the crosshair"));
			NextDelay = 0.5f;
			break;
		}

		case 24:
			// Releasing THROWS the Core (there is no other way to end a wind-up), so the green arm
			// gets a fresh pickup and a fresh press rather than inheriting the red arm's hold. The
			// first run of this harness released and re-pressed on one hold and photographed two
			// frames with no charge at all.
			if (ATraceGameState* GS = WorldPtr->GetGameState<ATraceGameState>())
			{
				if (ATraceCore* CorePtr = GS->Core)
				{
					CorePtr->RequestPassInput(false, PawnOf(*Run));
				}
			}
			SetArm(1);
			NextDelay = 0.6f;
			break;

		case 25:
			if (ATraceCharacter* Pawn = PawnOf(*Run))
			{
				if (ATraceGameState* GS = WorldPtr->GetGameState<ATraceGameState>())
				{
					if (ATraceCore* CorePtr = GS->Core)
					{
						CorePtr->SetActorLocation(Pawn->GetActorLocation());
						CorePtr->TryPickup(Pawn);
						CorePtr->RequestPassInput(true, Pawn);
						UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] re-armed the wind-up; carrier=%d"),
							(int32)Pawn->IsCarrier());
					}
				}
			}
			// 0.40 s into a 0.8 s charge (spec v16 0) - the ring must read about HALF full, which is
			// the one measurement that proves it sweeps LINEARLY IN TIME and not in momentum. A ring
			// fed by GetThrowChargeScaleNow() would read ~0.58 here because of the 15% floor.
			NextDelay = 0.40f;
			break;

		case 26:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("GREEN_ring_half"));
			Expect(*Run, Record.bChargeRing, TEXT("GREEN: the throw charge draws as a RING around the crosshair"));
			Expect(*Run, !Record.bChargeBar, TEXT("GREEN: the bottom-left charge BAR is gone"));
			Expect(*Run, Record.ChargeRingChords > 0,
				TEXT("GREEN: the ring emitted actual filled chords (a ring that computes an alpha and draws nothing is not a ring)"));
			Expect(*Run, Record.ChargeRingAlpha > 0.35f && Record.ChargeRingAlpha < 0.75f,
				TEXT("GREEN: 0.40 s into a 0.8 s charge the ring is about HALF full - it sweeps linearly in TIME, not in momentum"));
			NextDelay = 0.7f;
			break;
		}

		case 27:
		{
			const ATraceHUD::FV16DrawRecord Record = Shot(*Run, TEXT("GREEN_ring_full"));
			Expect(*Run, Record.bChargeRing && Record.ChargeRingAlpha >= 0.999f,
				TEXT("GREEN: past 0.8 s the ring reads 100% full"));
			Expect(*Run, Record.ChargeRingChords == 48, TEXT("GREEN: a full ring emitted all 48 chords"));
			NextDelay = 0.5f;
			break;
		}

		case 199:
			// A separate step so the receiver probe has run a frame before the hold is asked for.
			if (ATraceGameState* GS = WorldPtr->GetGameState<ATraceGameState>())
			{
				if (ATraceCore* CorePtr = GS->Core)
				{
					CorePtr->RequestPassInput(true, PawnOf(*Run));
				}
			}
			UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] mode A: pass hold requested; progress=%.3f"),
				TracePC_PassProgress(*Run));
			NextDelay = 0.25f;
			break;

		case 200:
		{
			// The mode A regression frame. Asserted on the CONTROLLER's pass progress rather than on
			// the draw record, because the pass ring predates this pass and deliberately does not
			// write one - what is being proven is that the shared geometry still draws for its
			// original caller, and the frame is what shows that.
			const float Progress = TracePC_PassProgress(*Run);
			Shot(*Run, TEXT("MODEA_passring"));
			Expect(*Run, Progress >= 0.f,
				TEXT("MODE A REGRESSION: the pass hold is live, so DrawPassProgress drew the shared crosshair ring"));
			Run->Step = 300;
			NextDelay = 0.6f;
			break;
		}

		default:
		{
			if (ATraceGameState* GS = WorldPtr->GetGameState<ATraceGameState>())
			{
				if (ATraceCore* CorePtr = GS->Core)
				{
					CorePtr->RequestPassInput(false, PawnOf(*Run));
				}
			}

			UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] ===== VERDICT: %d passed, %d FAILED, %d NOT APPLICABLE HERE ====="),
				Run->Passes.Num(), Run->Failures.Num(), Run->NotApplicable.Num());
			for (const FString& Line : Run->NotApplicable)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[HUDV16]   N/A   %s"), *Line);
			}
			for (const FString& Line : Run->Passes)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[HUDV16]   PASS  %s"), *Line);
			}
			for (const FString& Line : Run->Failures)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[HUDV16]   FAIL  %s"), *Line);
			}
			UE_LOG(LogTraceGame, Display, TEXT("[HUDV16] ===== sequence complete: %s ====="),
				Run->Failures.Num() == 0 ? TEXT("ALL GREEN") : TEXT("*** NOT A PASS ***"));
			return false;
		}
		}

		Schedule(Run, NextDelay);
		return false;   // this ticker is done; Schedule installed the next one
	}

	FAutoConsoleCommandWithWorld CmdReport(
		TEXT("Trace.HUD.V16.Report"),
		TEXT("Spec v16 2. Prints WHAT THE LAST DRAWN HUD FRAME CONTAINED - the ammo string and tick "
		     "count actually emitted, every status chip actually drawn, and the charge ring's chord "
		     "count. Reads the draw record, never the gameplay state that fed it."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* WorldPtr)
		{
			APlayerController* PC = FindLocalAuthorityPC(WorldPtr);
			ATraceHUD* HudPtr = (PC != nullptr) ? Cast<ATraceHUD>(PC->GetHUD()) : nullptr;
			if (HudPtr == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[HUDV16] no local ATraceHUD - nothing to report."));
				return;
			}
			HudPtr->LogV16DrawRecord(TEXT("Report"));
		}));

	FAutoConsoleCommandWithWorld CmdShots(
		TEXT("Trace.HUD.V16Shots"),
		TEXT("Spec v16 2. Stages real gameplay state and photographs the bottom-right corner and the "
		     "crosshair charge ring, RED ARM FIRST (Trace.HUD.V16 0, the superseded HUD) then the same "
		     "fixture armed. Writes Saved/Screenshots/HudV16_*_pid<n>.png, asserts on the draw record "
		     "beside every frame, and ends in a verdict. Listen server or standalone only; the ring "
		     "needs a mode B world (?mode=b)."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* WorldPtr)
		{
			APlayerController* PC = FindLocalAuthorityPC(WorldPtr);
			if (PC == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[HUDV16] *** REFUSED: no local, authoritative player controller. This command stages "
					     "server state and photographs a viewport, so it needs both in one process. ***"));
				return;
			}

			ATraceHUD* HudPtr = Cast<ATraceHUD>(PC->GetHUD());
			if (HudPtr == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[HUDV16] *** REFUSED: the local HUD is not an ATraceHUD. ***"));
				return;
			}

			TSharedPtr<FRun> Run = MakeShared<FRun>();
			Run->Hud = HudPtr;
			Run->PC = PC;

			const ATraceGameState* GS = WorldPtr->GetGameState<ATraceGameState>();
			Run->bModeB = (GS != nullptr) && GS->IsGoalMode();

			Schedule(Run, 0.1f);
		}));
}

// ===================================================================================================
// SPEC v17 §4 (STEP 4b) — THE CORNER'S OWN VERIFIER
//
//   Trace.HUD.Corner.Verify   which presenter drew the last frame, whether the widget assets are
//                             present and whether they satisfy the C++ BindWidget contract, and — in
//                             black and white — what on this HUD was NOT converted.
//
// IT HAS THREE VERDICTS, NOT TWO, and the middle one is the point: "no assets" is a SUPPORTED
// configuration (a fresh clone that has not run the generator yet plays exactly as it did in v16),
// while "assets present but invalid" is a real fault somebody has to fix. Collapsing those two into
// one FAIL would cry wolf at the working case; collapsing them into one PASS would hide the broken
// one. The same three-verdict shape spec v17 §6's input verifier uses, for the same reason.
//
// PARITY between the two presenters is NOT asserted here, and deliberately: a drawing change can
// only be verified by looking at frames. The evidence is Trace.HUD.V16Shots run twice against the
// same fixture — `Trace.UI.HUD.UseUMG 0` then `1` — which photographs both corners and asserts on
// the draw record each time. This command tells you which arm you are in; that one tells you whether
// the arm works.
// ===================================================================================================

namespace TraceHudCornerVerify
{
	/** The local HUD, whatever the net mode. Unlike the shot sequence this stages nothing. */
	ATraceHUD* FindLocalHud(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			if (APlayerController* Candidate = It->Get())
			{
				if (Candidate->IsLocalController())
				{
					if (ATraceHUD* HudPtr = Cast<ATraceHUD>(Candidate->GetHUD()))
					{
						return HudPtr;
					}
				}
			}
		}
		return nullptr;
	}

	/** The first local controller, HUD or no HUD — enough to build a throwaway widget with. */
	APlayerController* FindLocalController(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			if (APlayerController* Candidate = It->Get())
			{
				if (Candidate->IsLocalController())
				{
					return Candidate;
				}
			}
		}
		return nullptr;
	}

	FAutoConsoleCommandWithWorld CmdVerify(
		TEXT("Trace.HUD.Corner.Verify"),
		TEXT("Spec v17 4 (step 4b). Reports which presenter drew the bottom-right ammo/status corner on "
		     "the last frame, whether Content/Trace/UI/HUD's widget assets load and satisfy the C++ "
		     "BindWidget contract, and which HUD elements are still Canvas. Three verdicts: UMG live, "
		     "no assets (supported), or assets present but invalid (a fault)."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* WorldPtr)
		{
			ATraceHUD* HudPtr = FindLocalHud(WorldPtr);

			UE_LOG(LogTraceGame, Display, TEXT("[HUDUMG] ===== spec v17 4 (4b): the bottom-right corner ====="));

			// ---- The toggle ----------------------------------------------------------------------
			const IConsoleVariable* SharedVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.UI.UseUMG"));
			UE_LOG(LogTraceGame, Display,
				TEXT("[HUDUMG] Trace.UI.UseUMG=%s  Trace.UI.HUD.UseUMG=%d (-1 = follow the shared one)  =>  %s"),
				SharedVar != nullptr ? *FString::FromInt(SharedVar->GetInt()) : TEXT("<unregistered>"),
				TraceHudCornerUmg::GCornerOverride,
				TraceHudCornerUmg::IsCornerEnabled() ? TEXT("UMG requested") : TEXT("Canvas requested"));

			// ---- What actually drew --------------------------------------------------------------
			if (HudPtr == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[HUDUMG] no local ATraceHUD in this world - the asset check below still runs, but "
					     "nothing can be said about what drew."));
			}
			else
			{
				switch (HudPtr->GetCornerPath())
				{
				case ATraceHUD::ECornerPath::Umg:
					UE_LOG(LogTraceGame, Display, TEXT("[HUDUMG] LIVE PATH: UMG (%s)"),
						UTraceHudCornerWidget::CornerBlueprintPath());
					break;
				case ATraceHUD::ECornerPath::Canvas:
					UE_LOG(LogTraceGame, Display, TEXT("[HUDUMG] LIVE PATH: CANVAS - %s"),
						*HudPtr->GetCornerFallbackReason());
					break;
				default:
					UE_LOG(LogTraceGame, Display,
						TEXT("[HUDUMG] LIVE PATH: undecided - the corner pass has not run yet (no drawn frame, "
						     "or the player is dead and the corner is correctly quiet)."));
					break;
				}

				HudPtr->LogV16DrawRecord(TEXT("Corner.Verify"));
			}

			// ---- The assets themselves, independently of adoption --------------------------------
			//
			// Loaded and CONSTRUCTED here rather than inspected: BindWidget properties are populated by
			// widget construction, so a class that loads is not yet evidence that a widget built from it
			// binds. The throwaway instance is never added to a viewport and is collected normally.
			UClass* CornerClass = LoadClass<UTraceHudCornerWidget>(nullptr,
				UTraceHudCornerWidget::CornerBlueprintPath());
			UClass* ChipClass = LoadClass<UTraceHudStatusChipWidget>(nullptr,
				UTraceHudCornerWidget::ChipBlueprintPath());

			UE_LOG(LogTraceGame, Display, TEXT("[HUDUMG] %s : %s"),
				UTraceHudCornerWidget::CornerBlueprintPath(),
				CornerClass != nullptr ? TEXT("loads") : TEXT("ABSENT"));
			UE_LOG(LogTraceGame, Display, TEXT("[HUDUMG] %s : %s"),
				UTraceHudCornerWidget::ChipBlueprintPath(),
				ChipClass != nullptr ? TEXT("loads") : TEXT("ABSENT"));

			FString AssetVerdict;
			APlayerController* OwnerPC = FindLocalController(WorldPtr);

			if (CornerClass == nullptr || ChipClass == nullptr)
			{
				AssetVerdict = TEXT("NO ASSETS - the corner is drawn on Canvas exactly as it was in v16. "
				                    "This is a SUPPORTED configuration. Run Scripts/generate-hud-widgets.py "
				                    "to author them.");
			}
			else if (OwnerPC == nullptr)
			{
				AssetVerdict = TEXT("both assets load, but there is no local player controller to build a "
				                    "widget with, so the BindWidget contract could not be checked here.");
			}
			else if (UTraceHudCornerWidget* ProbeCorner = CreateWidget<UTraceHudCornerWidget>(OwnerPC, CornerClass))
			{
				FString ProbeReason;
				if (ProbeCorner->InitialiseCorner(ProbeReason))
				{
					AssetVerdict = FString::Printf(
						TEXT("both assets satisfy the C++ contract (%d bound widgets on the corner, %d on the "
						     "chip)."),
						UTraceHudCornerWidget::RequiredWidgetNames().Num(),
						UTraceHudStatusChipWidget::RequiredWidgetNames().Num());
				}
				else
				{
					AssetVerdict = FString::Printf(
						TEXT("*** ASSETS PRESENT BUT INVALID: %s. Re-run Scripts/generate-hud-widgets.py. ***"),
						*ProbeReason);
				}
				ProbeCorner->RemoveFromParent();
			}
			else
			{
				AssetVerdict = TEXT("*** ASSETS PRESENT BUT CreateWidget FAILED. ***");
			}

			UE_LOG(LogTraceGame, Display, TEXT("[HUDUMG] ASSETS: %s"), *AssetVerdict);

			// ---- What was NOT converted, said out loud -------------------------------------------
			//
			// Spec v17 4: "Anything you do not convert stays on Canvas and is REPORTED as not
			// converted." Reported HERE, in the running game, and not only in a report nobody has open
			// six months from now.
			UE_LOG(LogTraceGame, Display,
				TEXT("[HUDUMG] STILL CANVAS, deliberately: the crosshair and its charge/pass ring; the "
				     "bottom-LEFT stack (health, weapon, dash charges, parry and the ACTIVATED ABILITY "
				     "COOLDOWN row); the kill feed; the score/clock panel; the scoreboard; the death panel; "
				     "every banner; the pause menu; the character select screen."));
			UE_LOG(LogTraceGame, Display,
				TEXT("[HUDUMG] NOTE ON 'COOLDOWNS': the corner this step converted is spec v16 2's ammo + "
				     "STATUS corner. Cooldowns are not in it and never were - they live in the bottom-left "
				     "stack because v16 put them there on purpose, so the two never read as one widget "
				     "(statuses DRAIN, cooldowns FILL). Converting them would have been a redesign."));
			UE_LOG(LogTraceGame, Display, TEXT("[HUDUMG] ====="));
		}));
}

#endif // !UE_BUILD_SHIPPING
