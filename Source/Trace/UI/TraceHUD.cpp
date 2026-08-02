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
#include "TraceSettings.h"
#include "TraceTypes.h"

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

	DrawCrosshair();
	DrawHitMarker();
	DrawHealthAndDash();
	DrawScoresAndClock();
	DrawCoreBanner();
	DrawDeathPanel();
	DrawMatchResult();
	DrawScoreboard();
}

// -------------------------------------------------------------------------------------------
// Reticle
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawCrosshair()
{
	// The carrier has no gun (contract §3), so an aiming reticle would be a lie. Its absence is
	// also the fastest possible read of "you are carrying".
	if (!bLocalAlive || bLocalCarrying)
	{
		return;
	}

	const float CX = ViewW * 0.5f;
	const float CY = ViewH * 0.5f;
	const float Gap = 7.f * UIScale;
	const float Len = 11.f * UIScale;
	const float Thickness = FMath::Max(1.f, 2.f * UIScale);

	const FLinearColor Color(1.f, 1.f, 1.f, 0.85f);

	DrawLine(CX - Gap - Len, CY, CX - Gap, CY, Color, Thickness);
	DrawLine(CX + Gap, CY, CX + Gap + Len, CY, Color, Thickness);
	DrawLine(CX, CY - Gap - Len, CX, CY - Gap, Color, Thickness);
	DrawLine(CX, CY + Gap, CX, CY + Gap + Len, Color, Thickness);

	// Centre dot, so the exact aim point stays unambiguous at any resolution.
	const float Dot = FMath::Max(1.f, 2.f * UIScale);
	DrawRect(Color, CX - Dot * 0.5f, CY - Dot * 0.5f, Dot, Dot);
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

	const FLinearColor Base = bKill ? TraceHUDStyle::Danger : FLinearColor::White;
	const FLinearColor Color = TraceHUDStyle::WithAlpha(Base, Alpha);

	// Four diagonal ticks — the classic X, drawn as four separate segments so the middle stays
	// clear and the crosshair underneath is still readable.
	DrawLine(CX - Outer, CY - Outer, CX - Inner, CY - Inner, Color, Thickness);
	DrawLine(CX + Inner, CY - Inner, CX + Outer, CY - Outer, Color, Thickness);
	DrawLine(CX - Outer, CY + Outer, CX - Inner, CY + Inner, Color, Thickness);
	DrawLine(CX + Inner, CY + Inner, CX + Outer, CY + Outer, Color, Thickness);
}

// -------------------------------------------------------------------------------------------
// Bottom left: dash pip + health
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawHealthAndDash()
{
	if (LocalChar == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	const float Margin  = 40.f * UIScale;
	const float BarW    = 340.f * UIScale;
	const float HealthH = 26.f * UIScale;
	const float DashH   = 10.f * UIScale;

	const float HealthY = ViewH - Margin - HealthH;
	const float DashY   = HealthY - (14.f * UIScale) - DashH;

	// ---- Dash cooldown pip -----------------------------------------------------------------
	if (const UTraceCharacterMovementComponent* Movement = LocalChar->GetTraceMovement())
	{
		// GetDashCooldownRemaining() covers the whole lockout, i.e. the active dash window *plus*
		// the cooldown that follows it (the movement component starts the timer at
		// DashDuration + DashCooldown). Dividing by DashCooldown alone would make the pip sit
		// pinned at empty for the first DashDuration seconds.
		const float FullWindow = FMath::Max(TraceHUDStyle::TimeEpsilon, Settings.DashDuration + Settings.DashCooldown);
		const float Remaining  = FMath::Max(0.f, Movement->GetDashCooldownRemaining());
		const float Charge     = FMath::Clamp(1.f - (Remaining / FullWindow), 0.f, 1.f);
		const bool bReady      = (Remaining <= TraceHUDStyle::TimeEpsilon);

		const FLinearColor TeamTint = TraceTeamColor(LocalTeam);
		// Charging reads as a dim team-tinted sliver; ready snaps to full brightness.
		const FLinearColor PipColor = bReady
			? TraceHUDStyle::Shade(TeamTint, 1.0f, 0.25f)
			: TraceHUDStyle::Shade(TeamTint, 0.45f, 0.0f);

		const float LabelW = 58.f * UIScale;
		const FString DashLabel(TEXT("DASH"));

		DrawTextLeft(DashLabel, bReady ? TraceHUDStyle::Ink : TraceHUDStyle::InkDim,
			Margin, VCenterTextY(DashLabel, FontSmall, UIScale, DashY, DashH), FontSmall, UIScale);

		DrawMeter(Margin + LabelW, DashY, BarW - LabelW, DashH, Charge, PipColor);

		// A number is worth a lot here: dash is the only counterplay to a carrier, so players plan
		// around exactly when it comes back.
		if (!bReady)
		{
			const FString CountdownText = FString::Printf(TEXT("%.1f"), Remaining);
			DrawTextLeft(CountdownText, TraceHUDStyle::InkDim,
				Margin + BarW + (10.f * UIScale),
				VCenterTextY(CountdownText, FontSmall, UIScale, DashY, DashH), FontSmall, UIScale);
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
		if (bLocalCarrying)
		{
			const FString InvulnText(TEXT("INVULNERABLE"));
			DrawTextLeft(InvulnText, TraceHUDStyle::Ink,
				Margin + (10.f * UIScale),
				VCenterTextY(InvulnText, FontSmall, UIScale, HealthY, HealthH), FontSmall, UIScale);
		}
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
			ClockText = TEXT("WARMUP");
			ClockColor = TraceHUDStyle::InkDim;
			break;

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

	// Target score, so nobody has to be told what they are playing to.
	DrawTextCentered(FString::Printf(TEXT("FIRST TO %d"), UTraceSettings::Get().ScoreToWin),
		TraceHUDStyle::InkDim, CX, PanelY + PanelH - (22.f * UIScale), FontSmall, UIScale);
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
		// Covers InFlight as well as Loose: from the player's point of view a pass in the air and
		// a Core on the floor are the same thing — up for grabs.
		BannerText = TEXT("CORE LOOSE");
		BannerColor = TraceHUDStyle::Ink;
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

	// Countdown. RespawnDelay belongs to the game mode's timer; we only mirror it locally from the
	// moment we noticed the pawn die, so treat an expired count as "any moment now" rather than
	// showing a bogus negative number.
	const float Remaining = UTraceSettings::Get().RespawnDelay - (Now - LocalDeathTime);
	const FString RespawnText = (Remaining > 0.f)
		? FString::Printf(TEXT("RESPAWN IN %d"), FMath::CeilToInt(Remaining))
		: FString(TEXT("RESPAWNING..."));

	DrawTextCentered(RespawnText, TraceHUDStyle::Ink, CX, PanelY + (100.f * UIScale), FontMedium, 1.05f * UIScale);
}

void ATraceHUD::DrawMatchResult()
{
	if (TraceGS == nullptr || TraceGS->TraceMatchState != ETraceMatchState::PostMatch)
	{
		return;
	}

	const int32 Blue = TraceGS->GetScore(ETraceTeam::Blue);
	const int32 Orange = TraceGS->GetScore(ETraceTeam::Orange);

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

	DrawTextCentered(ResultText, ResultColor, ViewW * 0.5f, ViewH * 0.18f, FontLarge, 2.0f * UIScale);
}

// -------------------------------------------------------------------------------------------
// Scoreboard (Tab)
// -------------------------------------------------------------------------------------------

void ATraceHUD::DrawScoreboard()
{
	const bool bHeld = (TracePC != nullptr) && TracePC->IsScoreboardOpen();
	const bool bPostMatch = (TraceGS != nullptr) && (TraceGS->TraceMatchState == ETraceMatchState::PostMatch);

	// Force it open once the match is over — that is exactly when everyone wants to see it.
	if (!bHeld && !bPostMatch)
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
