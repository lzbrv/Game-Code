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
#include "GameFramework/GameStateBase.h"   // AGameStateBase::PlayerArray
#include "GameFramework/PlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Settings/TraceGameplayCompat.h" // which of the new mechanics this build actually has
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
	 * Where the third-person pass reticle is anchored along the pass ray when there is no receiver
	 * under it, in world units. See DrawPassReticle(): the reticle marks a POINT on that ray, and a
	 * ray drawn from an origin the camera is not sitting at has no single screen position — so one
	 * has to be picked. ~16 m is a typical in-arena pass; the anchor slides to the real distance the
	 * moment an actual receiver is acquired.
	 */
	static constexpr float PassAnchorDefaultDistance = 1600.f;

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
}

void ATraceHUD::BeginPlay()
{
	Super::BeginPlay();

#if !UE_BUILD_SHIPPING
	TraceAutoShot::Arm(this, TEXT("Match"));

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
		DrawHitMarker();
		DrawHealthAndDash();
		DrawScoresAndClock();
		DrawCoreBanner();
		DrawPhaseBanner();
		DrawScoreFlash();
		DrawDeathPanel();
		DrawScoreboard();
	}

	DrawMatchResult();

	// Last, over everything including the full-time takeover. A no-op while closed.
	PauseMenu.Tick(this, TracePC.Get(), ViewW, ViewH, UIScale, Now);
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
				PC->SetGameInputSuppressed(false);
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

	// Reports whether the ACCESSOR exists, not whether the mechanic is active right now — a pass is
	// never in progress on the first frame, so asking GetPassProgress() would report 0 forever.
	//
	// Display, not Verbose, and deliberately so. Twice now this project has lost time to a mechanic
	// declared dead when its only log line was suppressed. "boost=0" in the log the player already
	// has is the whole diagnosis. Once the movement and character slices land, all three read 1.
	UE_LOG(LogTraceGame, Display,
		TEXT("[HUD] Affordances: dashCharges=%d boost=%d passProgress=%d ")
		TEXT("(0 = that gameplay slice has not landed its accessor yet; see Settings/TraceGameplayCompat.h)"),
		TraceCompat::THasDashCharges<UTraceCharacterMovementComponent>::value ? 1 : 0,
		TraceCompat::THasBoostCooldownRemaining<UTraceCharacterMovementComponent>::value ? 1 : 0,
		TraceCompat::THasPassProgress<ATraceCharacter>::value ? 1 : 0);
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

	if (!bLocalAlive || LocalChar == nullptr || Canvas == nullptr || ViewBlend <= 0.02f)
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
	ATraceCore* Core = (TraceGS != nullptr) ? TraceGS->Core : nullptr;
	if (Core != nullptr && bLocalCarrying)
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
	// A CROSSHAIR IN BOTH CAMERA MODES, and they are two different instruments.
	//
	// First person is the shooting view: the reticle is the single most important pixel on the
	// screen, a literal promise about where the bullet goes, and it is unchanged from the version
	// that measured aimErr 0.0000 deg.
	//
	// Third person is the CARRYING view, and the carrier has no gun (contract §3) — but mouse1 is
	// not idle there either. Spec §4 makes it the PASS button, held for 0.5s while hovering a
	// teammate, so the carrier is still aiming at a target with the mouse and still needs to know
	// what is under the pointer. The reticle therefore does not disappear when the camera pulls
	// back; it CHANGES JOB, from "where the bullet goes" to "who catches the Core", and changes
	// shape to say so.
	//
	// Both fade with the camera blend rather than with the carrier bool, so a pick-up cross-fades
	// one instrument into the other over the 0.35s the camera is travelling.
	if (!bLocalAlive)
	{
		return;
	}

	const float FirstPersonVisibility = 1.f - ViewBlend;
	if (FirstPersonVisibility > 0.02f)
	{
		DrawAimReticle(ReticleX, ReticleY, FirstPersonVisibility);
	}
	if (ViewBlend > 0.02f)
	{
		DrawPassReticle(ViewBlend);
	}
}

void ATraceHUD::DrawAimReticle(float CX, float CY, float Visibility)
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
	const float T   = FMath::Max(2.f, FMath::RoundToFloat(2.5f * UIScale));
	const float Arm = FMath::Max(6.f, FMath::RoundToFloat(11.f * UIScale));
	const float Gap = FMath::Max(3.f, FMath::RoundToFloat(5.f  * UIScale));

	// Half a bar, floored, so the bar's own pixels straddle the centre symmetrically for odd T and
	// sit flush against it for even T. Both are exact; neither is a half-pixel.
	const float Half = FMath::FloorToFloat(T * 0.5f);

	const FLinearColor Ink    = FLinearColor(1.f, 1.f, 1.f, 0.92f * Visibility);
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
	// A DIFFERENT SHAPE, on purpose. The two reticles mean two different things and the player must
	// never have to look twice to know which one is on screen: the aim reticle is a cross whose
	// centre is a point, this is a frame whose centre is a TARGET. A cross would also read as "you
	// can shoot", which is the one thing a carrier cannot do.
	//
	// Everything below is integer-snapped for the same reason the aim reticle is: at 720p a
	// fractional rect is a grey smudge, and a smudged reticle is what "blurry" looks like.
	const float T = FMath::Max(2.f, FMath::RoundToFloat(2.5f * UIScale));
	const float Half = FMath::FloorToFloat(T * 0.5f);
	const float Len = FMath::Max(5.f, FMath::RoundToFloat(9.f * UIScale));

	// The brackets CLOSE when a receiver is acquired. Motion is what the eye catches; a colour swap
	// alone can be missed in the middle of a chase, and one that could only be seen by its colour
	// would be invisible to a colour-blind player.
	const float Radius = FMath::RoundToFloat(FMath::Lerp(24.f, 15.f, PassLockAlpha) * UIScale);

	const float X = ReticleX;
	const float Y = ReticleY;

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

	const float Bars[9][4] =
	{
		{ X0,        Y0,        Len, T   },   // top-left, horizontal
		{ X0,        Y0,        T,   Len },   // top-left, vertical
		{ X1 - Tail, Y0,        Len, T   },   // top-right, horizontal
		{ X1,        Y0,        T,   Len },   // top-right, vertical
		{ X0,        Y1,        Len, T   },   // bottom-left, horizontal
		{ X0,        Y1 - Tail, T,   Len },   // bottom-left, vertical
		{ X1 - Tail, Y1,        Len, T   },   // bottom-right, horizontal
		{ X1,        Y1 - Tail, T,   Len },   // bottom-right, vertical
		{ X - Half,  Y - Half,  T,   T   },   // centre dot: the exact point the pass is aimed at
	};

	for (const float* B : Bars)
	{
		DrawRect(Shadow, B[0] - 1.f, B[1] - 1.f, B[2] + 2.f, B[3] + 2.f);
	}
	for (const float* B : Bars)
	{
		DrawRect(Ink, B[0], B[1], B[2], B[3]);
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

	FString Caption;
	FLinearColor CaptionColor = TraceHUDStyle::InkDim;
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

	// The ring closes around the RETICLE, not around screen centre. In third person — which is the
	// only view a pass is ever made from — those are not the same pixel (see UpdateReticleAnchor),
	// and a progress ring that is not concentric with the thing it is the progress of reads as two
	// unrelated pieces of UI.
	const float CX = ReticleX;
	const float CY = ReticleY;
	const float Radius = 34.f * UIScale;
	const float Thickness = FMath::Max(2.f, 3.f * UIScale);

	// Canvas has no arc primitive, so the ring is a fan of short chords. 48 segments is smooth at any
	// size this is ever drawn at and costs ~48 DrawLine calls on the one frame in a hundred that a
	// pass is actually being held.
	constexpr int32 Segments = 48;
	const float Alpha = FMath::Clamp(Progress, 0.f, 1.f);

	auto PointAt = [CX, CY, Radius](float T)
	{
		// Starts at twelve o'clock and closes clockwise: the direction every progress ring in every
		// game closes, and the one a player reads without being told.
		const float Angle = -UE_HALF_PI + T * UE_TWO_PI;
		return FVector2D(CX + Radius * FMath::Cos(Angle), CY + Radius * FMath::Sin(Angle));
	};

	// Unfilled track first, so the ring reads as a dial rather than as a growing arc from nowhere.
	const FLinearColor Track = FLinearColor(1.f, 1.f, 1.f, 0.18f);
	for (int32 Index = 0; Index < Segments; ++Index)
	{
		const FVector2D A = PointAt(static_cast<float>(Index) / Segments);
		const FVector2D B = PointAt(static_cast<float>(Index + 1) / Segments);
		DrawLine(A.X, A.Y, B.X, B.Y, Track, Thickness * 0.7f);
	}

	// The filled arc is TEAM COLOURED, because a pass is the one action whose whole point is the
	// teammate on the other end of it.
	const FLinearColor Fill = TraceHUDStyle::Shade(TraceTeamColor(LocalTeam), 1.0f, 0.30f);
	const int32 Filled = FMath::CeilToInt(Alpha * Segments);
	for (int32 Index = 0; Index < Filled; ++Index)
	{
		const FVector2D A = PointAt(static_cast<float>(Index) / Segments);
		const FVector2D B = PointAt(FMath::Min(Alpha, static_cast<float>(Index + 1) / Segments));
		DrawLine(A.X, A.Y, B.X, B.Y, Fill, Thickness);
	}

	DrawTextCentered(TEXT("PASSING"), TraceHUDStyle::WithAlpha(TraceHUDStyle::Ink, 0.85f),
		CX, CY + Radius + (10.f * UIScale), FontSmall, UIScale);
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
	if (LocalChar == nullptr || TracePC == nullptr)
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

	// The ability stack grows UPWARDS from the health bar, so adding boost does not move health — the
	// one element a player finds by muscle memory rather than by reading.
	float RowY = HealthY - (14.f * UIScale) - RowH;

	const FLinearColor TeamTint = TraceTeamColor(LocalTeam);

	// ---- Boost (spec §5: ground-only super-jump, 12s) ---------------------------------------
	//
	// Drawn only when the movement component actually has a boost cooldown to report. A meter that
	// is permanently full is worse than no meter: it teaches the player that the row means nothing.
	{
		float BoostRemaining = 0.f;
		float BoostTotal = 0.f;
		if (TracePC->GetBoostHudState(BoostRemaining, BoostTotal))
		{
			const float Charge = FMath::Clamp(1.f - (BoostRemaining / FMath::Max(TraceHUDStyle::TimeEpsilon, BoostTotal)), 0.f, 1.f);
			const bool bReady = (BoostRemaining <= TraceHUDStyle::TimeEpsilon);

			const FString Label(TEXT("BOOST"));
			DrawTextLeft(Label, bReady ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim,
				Margin, VCenterTextY(Label, FontSmall, UIScale, RowY, RowH), FontSmall, UIScale);

			// Amber rather than the team tint: boost is a movement cost the player spends, and giving
			// it its own hue stops the two meters reading as one two-line bar.
			const FLinearColor BoostColor = bReady
				? FLinearColor(1.00f, 0.72f, 0.28f, 1.f)
				: FLinearColor(0.42f, 0.30f, 0.12f, 1.f);

			DrawMeter(Margin + LabelW, RowY, BarW - LabelW, RowH, Charge, BoostColor);

			if (!bReady)
			{
				const FString CountdownText = FString::Printf(TEXT("%.1f"), BoostRemaining);
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
		}
	}

	// ---- Health -----------------------------------------------------------------------------
	if (const UTraceHealthComponent* HealthComp = LocalChar->Health)
	{
		const float Percent = FMath::Clamp(HealthComp->GetHealthPercent(), 0.f, 1.f);
		const FLinearColor Fill = TraceHUDStyle::LerpColor(TraceHUDStyle::Danger, TraceHUDStyle::Good, Percent);

		DrawMeter(Margin, HealthY, BarW, HealthH, Percent, Fill);

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

	// Bottom line of the panel: the target score, prefixed by the phase when the phase is anything
	// other than "the match you are playing right now".
	// "FIRST TO N" is no longer the rule: spec §1 runs a fixed two halves and the mercy rule is off
	// by default (see ATraceGameMode::bEndMatchAtScoreToWin), so the phase is the useful thing to
	// show. ATraceGameState::GetHalfLabel() already reads "1ST HALF" / "HALF TIME" / "2ND HALF".
	FString FooterText = (TraceGS != nullptr) ? TraceGS->GetHalfLabel() : FString(TEXT("MATCH"));
	if (TraceGS != nullptr && TraceGS->TraceMatchState == ETraceMatchState::WaitingForPlayers)
	{
		FooterText = TEXT("WARM UP");
	}
	else if (TraceGS != nullptr && TraceGS->TraceMatchState == ETraceMatchState::PostMatch)
	{
		FooterText = TEXT("FULL TIME");
	}

	DrawTextCentered(FooterText, TraceHUDStyle::InkDim, CX, PanelY + PanelH - (22.f * UIScale), FontSmall, UIScale);
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
		// SPEC §2: the Core is a STATUS. It is never loose on the ground and never in flight, so
		// there is nothing here for the player to run over and grab — telling them "CORE LOOSE"
		// sent them chasing a pickup that no longer exists. The only two holderless states left are
		// both transient and both mean "wait":
		//
		//   out of play  — the half-time interval and the post-match window, where the Core is
		//                  deliberately parked and granted to nobody (ATraceCore::KickoffTo(None));
		//   kickoff      — the ~1s grant delay after a whistle or a score, which exists so the Core
		//                  is not handed over mid-teleport.
		BannerText = Core->IsOutOfPlay() ? TEXT("CORE OUT OF PLAY") : TEXT("CORE KICKOFF");
		BannerColor = TraceHUDStyle::InkDim;
	}
	else if (Carrier == LocalChar.Get())
	{
		BannerText = TEXT("YOU HAVE THE CORE");
		BannerColor = TraceTeamColor(LocalTeam);
	}
	else
	{
		const ETraceTeam CarrierTeam = Carrier->GetTeam();
		BannerText = FString::Printf(TEXT("%s HAS THE CORE"), *TraceTeamName(CarrierTeam).ToString().ToUpper());
		BannerColor = TraceTeamColor(CarrierTeam);
	}

	// Pulse anything that demands a reaction: you are carrying it, or an enemy is.
	const bool bUrgent = (Carrier != nullptr) && (Carrier == LocalChar.Get() || Carrier->GetTeam() != LocalTeam);
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

	FString ResultText;
	FLinearColor ResultColor;
	if (Blue == Orange)
	{
		ResultText = TEXT("DRAW");
		ResultColor = TraceHUDStyle::Ink;
	}
	else
	{
		const ETraceTeam Winner = (Blue > Orange) ? ETraceTeam::Blue : ETraceTeam::Orange;
		ResultText = FString::Printf(TEXT("%s WINS"), *TraceTeamName(Winner).ToString().ToUpper());
		ResultColor = TraceTeamColor(Winner);
	}

	DrawTextCentered(TEXT("FULL TIME"), TraceHUDStyle::InkDim, CX, ViewH * 0.095f, FontSmall, 1.3f * UIScale);
	DrawTextCentered(ResultText, ResultColor, CX, ViewH * 0.135f, FontLarge, 2.6f * UIScale);

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

	// Column anchors. Name is left-aligned; the three numeric columns are right-aligned so their
	// digits line up regardless of width.
	const float NameX   = X + (12.f * UIScale);
	const float KillsX  = X + Width * 0.66f;
	const float DeathsX = X + Width * 0.80f;
	const float PingX   = X + Width * 0.97f;

	// ---- Header -------------------------------------------------------------------------------
	DrawRect(TraceHUDStyle::WithAlpha(TeamColor, 0.25f), X, Y, Width, HeaderH);

	const FString TeamLabel = TraceTeamName(Team).ToString().ToUpper();
	DrawTextLeft(TeamLabel, TeamColor, NameX, VCenterTextY(TeamLabel, FontMedium, UIScale, Y, HeaderH), FontMedium, UIScale);

	const float HeaderTextY = VCenterTextY(TEXT("K"), FontSmall, UIScale, Y, HeaderH);
	DrawTextRight(TEXT("K"),    TraceHUDStyle::InkDim, KillsX,  HeaderTextY, FontSmall, UIScale);
	DrawTextRight(TEXT("D"),    TraceHUDStyle::InkDim, DeathsX, HeaderTextY, FontSmall, UIScale);
	DrawTextRight(TEXT("PING"), TraceHUDStyle::InkDim, PingX,   HeaderTextY, FontSmall, UIScale);

	// ---- Gather this team's players -----------------------------------------------------------
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
