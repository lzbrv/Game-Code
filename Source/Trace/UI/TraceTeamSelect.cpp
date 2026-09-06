// Trace — the team select screen. See TraceTeamSelect.h.

#include "UI/TraceTeamSelect.h"

#include "Engine/Engine.h"                        // GEngine->GetWorldContexts, for the report command
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"                        // the repeating report
#include "InputCoreTypes.h"

#include "Settings/TraceGamepadInput.h"   // D32-PADMENU — TracePadMenu, the shared pad vocabulary

#include "Core/TraceGameMode.h"          // IsTeamSwitchAllowed — the ONE copy of the balance rule
#include "Core/TracePlayerController.h"  // the session, and every request RPC
#include "Core/TracePlayerState.h"
#include "Core/TraceCharacterRoster.h"           // NameFor, for the report
#include "GameFramework/Pawn.h"
#include "Gameplay/TraceHealthComponent.h"        // the health a character switch must leave sane
#include "Trace.h"                       // LogTraceGame
#include "TraceSettings.h"               // PlayersPerTeam, for the "3 / 5" line
#include "TraceTypes.h"                  // TraceTeamColor / TraceTeamName
#include "UI/TraceHardwareCursor.h"      // spec v24 §2 — one pointer on screen, not two
#include "UI/Text/TraceCanvasText.h"     // spec v22 §A1 — this screen types from the glyph atlas
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

#if !UE_BUILD_SHIPPING
int32 GTraceTeamSelectDebugPick = 0;
#endif

// NAMED, not anonymous: UBT compiles this module as a unity/jumbo build, so two files that each
// define something at the top of an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
namespace TraceTeamSelectStyle
{
	// The same palette FTraceCharacterSelect uses. Duplicated as four constants rather than reached
	// for across a file boundary because that screen's are file-local statics inside its .cpp — and
	// four literals is a smaller lie than exporting a style header for one caller. If the two ever
	// need to move together, the shared home is TraceMenuArtStyle, which both already include.
	static const FLinearColor Ink    (0.94f, 0.97f, 1.00f, 1.00f);
	static const FLinearColor InkSoft(0.76f, 0.84f, 0.90f, 1.00f);
	static const FLinearColor InkDim (0.56f, 0.66f, 0.75f, 1.00f);
	static const FLinearColor Danger (0.95f, 0.28f, 0.22f, 1.00f);

	static const FLinearColor Plate = TraceMenuArtStyle::PlateFill;

	/** OPAQUE, for the reason spelled out on FTraceCharacterSelect's own backdrop: the HUD has
	 *  already drawn ammo, health, the scoreboard and the crosshair by the time this ticks, and one
	 *  opaque rectangle is what makes a modal modal. */
	static const FLinearColor Backdrop(0.0055f, 0.0090f, 0.0190f, 1.00f);

	static FLinearColor WithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, A);
	}

	/** Scales RGB and leaves alpha alone — AHUD::DrawLine discards alpha, so dimming must be in RGB. */
	static FLinearColor Dimmed(const FLinearColor& C, float Mul)
	{
		return FLinearColor(C.R * Mul, C.G * Mul, C.B * Mul, C.A);
	}

	/** How long a server verdict stays on screen. Matched to the character select's 3.5 s. */
	static constexpr float MessageDuration = 3.5f;
}

/**
 * THE LAYOUT, IN ONE PLACE — every number is a 1080p design pixel and is multiplied by UIScale,
 * exactly like TraceSelectLayout in the character select.
 *
 * The vertical budget adds up to 1080:
 *   34..104 title and countdown | 128 the rule line | 190..760 the two plates | 812 the verdict line
 *   | 900 the footer controls.
 */
namespace TraceTeamSelectLayout
{
	constexpr float Margin      = 54.f;
	constexpr float HeaderTop   = 34.f;
	constexpr float TitleSize   = 42.f;
	constexpr float TitleTrack  = 7.0f;
	constexpr float SubY        = 128.f;

	constexpr float PlateTop    = 190.f;
	constexpr float PlateH      = 570.f;
	constexpr float PlateGap    = 40.f;
	constexpr float PlatePad    = 30.f;

	/** Plates are capped so they do not become billboards on an ultrawide viewport. */
	constexpr float PlateMaxW   = 520.f;

	constexpr float VerdictY    = 812.f;
	constexpr float FooterY     = 900.f;

	/**
	 * D32-PADMENU — the pad legend's baseline, below the keyboard's.
	 *
	 * 28 rather than the 24 the body face would need on its own: the two lines are the same size and
	 * a gap equal to the leading reads as one wrapped paragraph rather than as two legends for two
	 * devices.
	 */
	constexpr float PadFooterGap = 28.f;

	constexpr float SizeDisplay = 46.f;
	constexpr float SizeLead    = 22.f;
	constexpr float SizeBody    = 18.f;
	constexpr float SizeLabel   = 14.f;

	constexpr float TrackLabel  = 2.6f;
}

namespace TraceTeamSelectFile
{
	/** Uppercased player name, or a placeholder. Never returns empty — an empty row looks like a bug. */
	FString SafeName(const APlayerState* State)
	{
		if (State == nullptr)
		{
			return TEXT("?");
		}
		const FString Name = State->GetPlayerName();
		return Name.IsEmpty() ? TEXT("PLAYER") : Name.ToUpper();
	}

	/**
	 * Every non-spectating member of @p Team, in PlayerArray order.
	 *
	 * Walks the same array and applies the same spectator skip ATraceGameState::CountTeamMembers and
	 * ATraceGameMode::IsTeamSwitchAllowed do. A third opinion about who is on a team would let the
	 * screen show four names beside a count of three, which is precisely the kind of drift that makes
	 * a player distrust the whole panel.
	 */
	void GatherTeam(const AGameStateBase* InGameState, ETraceTeam Team, TArray<const ATracePlayerState*>& Out)
	{
		Out.Reset();
		if (InGameState == nullptr)
		{
			return;
		}

		for (const APlayerState* const EachState : InGameState->PlayerArray)
		{
			const ATracePlayerState* const Member = Cast<ATracePlayerState>(EachState);
			if (Member == nullptr || Member->IsOnlyASpectator() || Member->Team != Team)
			{
				continue;
			}
			Out.Add(Member);
		}
	}

	/** One text draw at a point size, in the menu face. The whole text API this file needs. */
	float Text(AHUD* HUD, const FString& InText, const FLinearColor& Color, float X, float Y,
		float Size, float Tracking = 0.f, TraceText::EHAlign Align = TraceText::EHAlign::Left)
	{
		TraceText::FStyle Style(Size, Color);
		Style.Tracking = Tracking;
		Style.HAlign = Align;
		return TraceCanvasText::Draw(HUD, InText, X, Y, Style);
	}

	float Width(const FString& InText, float Size, float Tracking = 0.f)
	{
		TraceText::FStyle Style(Size);
		Style.Tracking = Tracking;
		return TraceText::MeasureWidth(InText, Style);
	}

#if !UE_BUILD_SHIPPING
	/**
	 * Trace.Teams.Report — the whole verification surface for D31-TEAMS, printed from THIS machine.
	 *
	 * WHY IT IS ONE COMMAND AND NOT FOUR. Every claim this feature makes is a claim about a
	 * RELATIONSHIP between facts that live in different objects: the ordering claim is "the team flag
	 * is up AND the character flag is not", the balance claim is "the rule says yes to one side and
	 * no to the other, for a stated reason", and the character-switch claim is "the character changed
	 * AND health is inside the new maximum AND the cooldown did not reset". Printing them separately
	 * invites a run that captures three of the four and reads the fourth off an assumption.
	 *
	 * AND IT PRINTS THE OPPOSITE CASE ON PURPOSE. Both teams are evaluated, always, whichever one the
	 * player is on — so the line that says ORANGE is allowed sits next to the line that says why BLUE
	 * is not, in the same units, from the same predicate. A report that only printed the answer for
	 * the row the player asked about could not fail.
	 */
	void PrintReport(const ATracePlayerController* PC, const ATracePlayerState* LocalState)
	{
		const UWorld* const World = (PC != nullptr) ? PC->GetWorld() : nullptr;
		const AGameStateBase* const BaseGameState = (World != nullptr) ? World->GetGameState() : nullptr;

		const TCHAR* const Role =
			(World == nullptr) ? TEXT("?") :
			(World->GetNetMode() == NM_Client) ? TEXT("CLIENT") :
			(World->GetNetMode() == NM_ListenServer) ? TEXT("LISTEN-SERVER") :
			(World->GetNetMode() == NM_DedicatedServer) ? TEXT("DEDICATED-SERVER") : TEXT("STANDALONE");

		const uint8 CharId = (LocalState != nullptr) ? LocalState->GetSelectedCharacter() : 0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[TeamSelect.Report] %s | me='%s' team=%s character=%s locked=%d | teamSelectOpen=%d "
			     "charSelectOpen=%d | teamSelectLeft=%.1f charSelectLeft=%.1f"),
			Role,
			(LocalState != nullptr) ? *LocalState->GetPlayerName() : TEXT("<none>"),
			(LocalState != nullptr) ? *TraceTeamName(LocalState->Team).ToString() : TEXT("?"),
			*TraceCharacterRoster::NameFor(CharId),
			(LocalState != nullptr) ? (LocalState->bCharacterLocked ? 1 : 0) : -1,
			(PC != nullptr) ? (PC->IsTeamSelectOpen() ? 1 : 0) : -1,
			(LocalState != nullptr) ? (LocalState->IsCharacterSelectOpen() ? 1 : 0) : -1,
			(PC != nullptr) ? PC->GetTeamSelectTimeRemaining() : -1.f,
			(LocalState != nullptr) ? LocalState->GetCharacterSelectTimeRemaining() : -1.f);

		// Health and the E cooldown, which are the two things a mid-match character switch is most
		// likely to have got wrong (spec v19 §3's Lily-at-60, and "a swap must not buy a free E").
		{
			const APawn* const Body = (LocalState != nullptr) ? LocalState->GetPawn() : nullptr;
			const UTraceHealthComponent* const HealthComponent =
				(Body != nullptr) ? Body->FindComponentByClass<UTraceHealthComponent>() : nullptr;

			UE_LOG(LogTraceGame, Display,
				TEXT("[TeamSelect.Report]   health=%s activatedCooldown=%.2fs pawn=%s"),
				(HealthComponent != nullptr)
					? *FString::Printf(TEXT("%.1f/%.1f"), HealthComponent->Health, HealthComponent->GetMaxHealth())
					: TEXT("<no pawn>"),
				(LocalState != nullptr) ? LocalState->GetActivatedCooldownRemaining() : -1.f,
				*GetNameSafe(Body));
		}

		for (const ETraceTeam Team : { ETraceTeam::Blue, ETraceTeam::Orange })
		{
			TArray<const ATracePlayerState*> Members;
			GatherTeam(BaseGameState, Team, Members);

			FString Line;
			for (const ATracePlayerState* const Member : Members)
			{
				Line += FString::Printf(TEXT("%s%s%s(%s)"),
					Line.IsEmpty() ? TEXT("") : TEXT(", "),
					*SafeName(Member),
					Member->IsABot() ? TEXT("[BOT]") : TEXT(""),
					*TraceCharacterRoster::NameFor(Member->GetSelectedCharacter()));
			}

			FString Reason;
			const bool bAllowed = ATraceGameMode::IsTeamSwitchAllowed(BaseGameState, LocalState, Team, Reason);
			const bool bAlready = (LocalState != nullptr) && (LocalState->Team == Team);

			UE_LOG(LogTraceGame, Display,
				TEXT("[TeamSelect.Report]   %-6s %d/%d  switch=%s  [%s]"),
				*TraceTeamName(Team).ToString().ToUpper(), Members.Num(),
				FMath::Max(1, UTraceSettings::Get().PlayersPerTeam),
				bAlready ? TEXT("ALREADY THERE") : (bAllowed ? TEXT("ALLOWED") : *FString::Printf(TEXT("REFUSED - %s"), *Reason)),
				*Line);
		}
	}
#endif

	/** A one-pixel outline. AHUD has no stroked rect; four thin fills is what one is on Canvas. */
	void StrokeRect(AHUD* HUD, float X, float Y, float W, float H, float Thick, const FLinearColor& Color)
	{
		HUD->DrawRect(Color, X, Y, W, Thick);
		HUD->DrawRect(Color, X, Y + H - Thick, W, Thick);
		HUD->DrawRect(Color, X, Y, Thick, H);
		HUD->DrawRect(Color, X + W - Thick, Y, Thick, H);
	}
}

// =============================================================================================
// Lifecycle + input
// =============================================================================================

bool FTraceTeamSelect::PollOpenHotkey(ATracePlayerController* PC)
{
	if (PC == nullptr)
	{
		return false;
	}

	// H, HARDCODED, and that is a stated decision rather than an oversight. Every gameplay key in
	// this project is rebindable through UTraceUserSettings and resolved through ETraceInputAction —
	// and this is not a gameplay key. It is polled (see the header for why a bound delegate is wrong
	// for a screen that opens before a pawn exists), it is unbound by default in the shipped key
	// table (checked: nothing in TraceUserSettings.cpp's Default_* returns EKeys::H), and the brief
	// names it literally: "Players can hit H". Adding a rebindable action for it means touching the
	// input asset generator, the keybind page and the settings file format, which is a bigger change
	// than the feature. It is in the report as a known limitation.
	// *** D32-PADMENU — THERE IS NO PAD BUTTON HERE, AND THAT IS MEASURED RATHER THAN OVERLOOKED. ***
	//
	// This poll runs during GAMEPLAY, on every frame both screens are closed, so anything it read
	// would have to be a button no gameplay verb is using. The shipped pad layout uses all sixteen a
	// standard controller has — A B X Y, four D-pad, two shoulders, two triggers, two stick clicks,
	// VIEW for the scoreboard — and the sixteenth, MENU/START, is the pause key (see
	// UTraceGamepadInputSubsystem::TickMenuButton). `Trace.Pad.Verify` asserts that count from the
	// live table, so it is a fact about the build and not a claim in a comment.
	//
	// A chord or a long-press was the alternative and was refused: every one of those buttons does
	// something the instant it is touched in a match, and "hold LB for half a second" would fire PULL
	// CORE first. So mid-match REOPENING of this screen is keyboard-only, and it is written up as a
	// known limitation with the two fixes that would remove it — a row on the pause menu, or a
	// gameplay verb giving a button back.
	//
	// NOTHING A PAD PLAYER NEEDS IS BEHIND THIS. The join flow OPENS this screen by itself
	// (ATracePlayerController::bTeamSelectOpen, replicated), and once it is up a pad drives all of it,
	// including X to reach the character screen. This is the convenience path, not the only one.
	if (!PC->WasInputKeyJustPressed(EKeys::H))
	{
		return false;
	}

	PC->ServerRequestOpenTeamSelect();
	return true;
}

void FTraceTeamSelect::Tick(AHUD* HUD, ATracePlayerController* PC, ATracePlayerState* LocalState,
	float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed)
{
	if (HUD == nullptr || InViewW <= 0.f || InViewH <= 0.f)
	{
		return;
	}

	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;
	Now = InNow;

	// THE ONLY CONDITION. Replicated from the server, so "may this player change team at all" is
	// answered upstream and nothing is re-derived here. See the header.
	const bool bShouldBeOpen = (PC != nullptr) && PC->IsTeamSelectOpen();

	if (bShouldBeOpen != bOpen)
	{
		bOpen = bShouldBeOpen;

		if (bOpen)
		{
			HoveredRow = INDEX_NONE;

			// FBox2D's default constructor leaves bIsValid UNINITIALISED and PollInput runs before
			// Draw on this very frame, so without this the first hit test reads garbage and can
			// report the pointer as being inside a plate that has never been drawn.
			for (int32 Row = 0; Row < RowCount; ++Row)
			{
				RowRects[Row] = FBox2D(ForceInit);
			}

			// Swallow the remainder of this frame's input: the H that opened this must not also
			// confirm a row, and a movement key held during warm-up must not either.
			IgnoreInputBeforeFrame = GFrameCounter + 1;

			// Start on the team the player is NOT on. It is the only row that does anything, and a
			// default that does nothing is how a screen ends up feeling broken on the first press.
			const ETraceTeam Current = (LocalState != nullptr) ? LocalState->Team : ETraceTeam::None;
			Highlighted = (Current == ETraceTeam::Blue) ? RowOrange : RowBlue;

			UE_LOG(LogTraceGame, Display,
				TEXT("[TeamSelect] Screen opened (currently %s, %.0fs). Press %s again to close."),
				*TraceTeamName(Current).ToString(),
				(PC != nullptr) ? PC->GetTeamSelectTimeRemaining() : 0.f,
				OpenKeyName());
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("[TeamSelect] Screen closed."));
		}
	}

	// ---- SPEC v24 §2 — the OS pointer stops being drawn over ours ------------------------------
	//
	// Renewed, never latched: the lease expires two frames after this stops being called, so a screen
	// that closes hands the hardware pointer straight back. The character select renews it for its
	// own frames; this covers the frames where THIS screen is the one with a pointer on it.
	if (bOpen && bHasCursor)
	{
		TraceHardwareCursor::EnsureRunning();
		TraceHardwareCursor::RenewSuppression(PC, TEXT("team select"));
	}

	if (!bOpen)
	{
		return;
	}

	if (bInputAllowed && PC != nullptr && GFrameCounter >= IgnoreInputBeforeFrame)
	{
		PollInput(PC, LocalState);
	}

#if !UE_BUILD_SHIPPING
	if (GTraceTeamSelectDebugPick != 0)
	{
		const int32 Requested = GTraceTeamSelectDebugPick;
		GTraceTeamSelectDebugPick = 0;
		DebugPick((Requested == 2) ? ETraceTeam::Orange : ETraceTeam::Blue, PC, LocalState);
	}
#endif

	Draw(HUD, PC, LocalState);
}

void FTraceTeamSelect::PollInput(ATracePlayerController* PC, ATracePlayerState* LocalState)
{
	// ---- H closes it again. The key that opens a screen should close it. ------------------------
	//
	// D32-PADMENU — and B, because that is what BACK means on every other screen in this build (see
	// TracePadMenu in Settings/TraceGamepadInput.h). It is the whole reason this screen is not a trap
	// for a pad: without it a player who opened team select — or who was PUT here by the join flow,
	// which is the common case since D31 — could highlight a plate and never leave.
	//
	// MENU/START gets out too, by a different door: the subsystem turns it into an Escape, ATraceHUD
	// opens the pause menu on that, and this screen keeps drawing underneath with bInputAllowed
	// false. That is a pause, not a close, and B is the one that actually dismisses the screen.
	{
		const bool bPadBack = TracePadMenu::BackPressed(PC);
		if (PC->WasInputKeyJustPressed(EKeys::H) || bPadBack)
		{
			if (bPadBack)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[TeamSelect] Pad B -> close."));
			}
			PC->ServerRequestCloseTeamSelect();
			return;
		}
	}

	// ---- C — D31-TEAMS (b), the mid-match character switch --------------------------------------
	//
	// It lives on THIS screen rather than on a key of its own, and that is a deliberate design
	// choice: one hotkey (H) opens one menu that carries both of the changes a player can make to
	// themselves mid-match. A second free-floating key would be a second thing to discover and a
	// second thing to press by accident with a Core in hand.
	//
	// D32-PADMENU — X is the pad's C. It is TracePadMenu's "second verb" slot, which exists for
	// exactly this: a screen whose whole job is two choices, where A must stay CONFIRM and B must
	// stay BACK. Y is deliberately left alone — the options overlay spends it on UNBIND, and a
	// button that means "delete a binding" on one screen and "change your character" on the next is
	// the kind of overload this tranche exists to avoid.
	{
		const bool bPadAlt = TracePadMenu::AltPressed(PC);
		if (PC->WasInputKeyJustPressed(EKeys::C) || bPadAlt)
		{
			if (bPadAlt)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[TeamSelect] Pad X -> change character."));
			}
			PC->ServerRequestCharacterSwitch();
			return;
		}
	}

	// ---- Direct number keys. One key per plate — no walking required. ---------------------------
	if (PC->WasInputKeyJustPressed(EKeys::One))
	{
		Highlighted = RowBlue;
		Confirm(PC, LocalState);
		return;
	}
	if (PC->WasInputKeyJustPressed(EKeys::Two))
	{
		Highlighted = RowOrange;
		Confirm(PC, LocalState);
		return;
	}

	// ---- Left / right, with repeat --------------------------------------------------------------
	//
	// D32-PADMENU — the D-pad and the left stick fold into the SAME direction and the SAME repeat
	// clock as the arrow keys, rather than getting one of their own. Two clocks would have meant this
	// screen scrolling at one speed under a thumb and another under a finger, and a player holding
	// both would have stepped twice per repeat.
	const bool bLeft  = PC->IsInputKeyDown(EKeys::Left)  || PC->IsInputKeyDown(EKeys::A);
	const bool bRight = PC->IsInputKeyDown(EKeys::Right) || PC->IsInputKeyDown(EKeys::D);

	const int32 PadDir = TracePadMenu::NavX(PC);
	const int32 NavDir = FMath::Clamp((bRight ? 1 : 0) - (bLeft ? 1 : 0) + PadDir, -1, 1);
	const int32 HighlightBefore = Highlighted;
	if (NavDir != 0)
	{
		if (NavDir != LastNavDir)
		{
			LastNavDir = NavDir;
			NextNavTime = Now + NavRepeatDelay;
			Highlighted = FMath::Clamp(Highlighted + NavDir, 0, RowCount - 1);
		}
		else if (Now >= NextNavTime)
		{
			NextNavTime = Now + NavRepeatInterval;
			Highlighted = FMath::Clamp(Highlighted + NavDir, 0, RowCount - 1);
		}
	}
	else
	{
		LastNavDir = 0;
	}

	// D32-PADMENU — a line per PAD-driven move, and only for a pad-driven one.
	//
	// This screen is a highlight and two plates; nothing else it does is visible in a log, so without
	// this there is no way to tell a stick that moved the highlight from a stick that did nothing —
	// the difference is one outlined rectangle in a screenshot. It is also the first thing to read
	// when a player says "my controller does not move the selection".
	//
	// GATED ON THE PAD HAVING CONTRIBUTED, not on the highlight having moved, so the keyboard's own
	// arrow keys log exactly what they logged before this tranche: nothing. And gated on the highlight
	// actually CHANGING as well, because the clamp means a held direction at either end calls this
	// every repeat while the screen stands still.
	if (PadDir != 0 && Highlighted != HighlightBefore)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[TeamSelect] Pad %s -> %s."),
			(PadDir > 0) ? TEXT("RIGHT") : TEXT("LEFT"),
			*TraceTeamName(TeamForRow(Highlighted)).ToString());
	}

	// ---- Commit ---------------------------------------------------------------------------------
	//
	// D32-PADMENU — A, through the SAME Confirm the keyboard and the mouse call, so the screen's own
	// belief test and its request cooldown apply to a pad exactly as they do to a key.
	{
		const bool bPadConfirm = TracePadMenu::ConfirmPressed(PC);
		if (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::SpaceBar)
			|| bPadConfirm)
		{
			if (bPadConfirm)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[TeamSelect] Pad A -> confirm %s."),
					*TraceTeamName(TeamForRow(Highlighted)).ToString());
			}
			Confirm(PC, LocalState);
			return;
		}
	}

	// ---- Mouse ----------------------------------------------------------------------------------
	float MouseX = 0.f;
	float MouseY = 0.f;

	// Measured before CursorPos is overwritten — the hover guard below compares against the PREVIOUS
	// frame's position.
	bool bCursorMoved = false;
	if (PC->GetMousePosition(MouseX, MouseY))
	{
		const FVector2D NewPos(MouseX, MouseY);

		// THE FIRST SAMPLE IS NOT A MOVE. bHasCursor is false for the frames before the screen has
		// ever read a pointer position, and counting that first read as movement would hand the
		// highlight to whatever the pointer happens to be resting over at the instant the screen
		// opens — which is exactly the opening choice this screen works to get right (the first
		// plate no team-mate is believed to hold). Measured: without this the end-to-end run opened
		// on card 3 instead of card 1, because the pointer was parked there from the title screen.
		bCursorMoved = bHasCursor && FVector2D::DistSquared(NewPos, CursorPos) > 4.f;   // 2 px
		CursorPos = NewPos;
		bHasCursor = true;
	}

	const bool bDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);
	const bool bJustReleased = !bDown && bMouseWasDown;
	bMouseWasDown = bDown;

	if (!bHasCursor)
	{
		return;
	}

	HoveredRow = INDEX_NONE;
	for (int32 Row = 0; Row < RowCount; ++Row)
	{
		if (RowRects[Row].bIsValid && RowRects[Row].IsInside(CursorPos))
		{
			HoveredRow = Row;
			break;
		}
	}

	// *** THE POINTER HAS TO HAVE MOVED BEFORE IT MAY TAKE THE HIGHLIGHT. ***
	// The same guard, for the same measured reason, as the character select's — read the long note
	// there. Two plates fill most of this screen, so a resting pointer is MORE likely to be over one
	// here than over a card there, and the effect was the same: nothing else could move the highlight.
	if (HoveredRow != INDEX_NONE && bCursorMoved)
	{
		Highlighted = HoveredRow;
	}

	// ONE PRESS = ONE ACTION (spec v15 §4). The action fires on RELEASE inside the plate the press
	// began over, which is the convention the rest of the menus use.
	if (bJustReleased && HoveredRow != INDEX_NONE)
	{
		// A click takes the plate it lands on whether or not the pointer moved first. See the note in
		// the character select's twin of this line.
		Highlighted = HoveredRow;
		Confirm(PC, LocalState);
	}
}

void FTraceTeamSelect::Confirm(ATracePlayerController* PC, ATracePlayerState* LocalState)
{
	if (PC == nullptr)
	{
		return;
	}

	// A held key or a fast double click must not send ten requests a second. Deliberately a plain
	// local cooldown rather than a pending-request latch like the character select's: a team request
	// is answered by a verdict RPC that also arrives as a visible state change (the screen closes),
	// so there is nothing to unlatch if a packet were lost.
	if ((Now - LastRequestTime) < RequestCooldown)
	{
		return;
	}

	const ETraceTeam Wanted = TeamForRow(Highlighted);

	// Believed-illegal rows are not sent. The SAME predicate the server will apply — see the header
	// for why that is a belief here and a verdict there — so a refusal the screen can already see
	// costs no round trip and produces the same message.
	const AGameStateBase* const BaseGameState = (PC->GetWorld() != nullptr) ? PC->GetWorld()->GetGameState() : nullptr;
	FString Reason;
	if (LocalState != nullptr && LocalState->Team != Wanted
		&& !ATraceGameMode::IsTeamSwitchAllowed(BaseGameState, LocalState, Wanted, Reason))
	{
		UE_LOG(LogTraceGame, Log, TEXT("[TeamSelect] Not sending %s: %s"),
			*TraceTeamName(Wanted).ToString(), *Reason);
		return;
	}

	LastRequestTime = Now;
	PC->ServerRequestTeam(Wanted);

	UE_LOG(LogTraceGame, Display, TEXT("[TeamSelect] Requested %s."), *TraceTeamName(Wanted).ToString());
}

#if !UE_BUILD_SHIPPING
void FTraceTeamSelect::DebugPick(ETraceTeam Team, ATracePlayerController* PC, ATracePlayerState* LocalState)
{
	Highlighted = (Team == ETraceTeam::Orange) ? RowOrange : RowBlue;

	// Deliberately clears the cooldown first: a scripted run presses once and must not be swallowed
	// by a cooldown a previous press armed.
	LastRequestTime = -1000.f;
	Confirm(PC, LocalState);
}
#endif

// =============================================================================================
// Draw
// =============================================================================================

FString FTraceTeamSelect::VerdictLine(const ATracePlayerController* PC) const
{
	if (PC == nullptr || (Now - PC->LastTeamResultLocalTime) > TraceTeamSelectStyle::MessageDuration)
	{
		return FString();
	}

	const FString Team = TraceTeamName(PC->LastTeamResultTeam).ToString().ToUpper();

	switch (PC->LastTeamResult)
	{
	case ETraceTeamChangeResult::Granted:       return FString::Printf(TEXT("MOVED TO %s"), *Team);
	case ETraceTeamChangeResult::AlreadyOnTeam: return FString::Printf(TEXT("ALREADY ON %s"), *Team);
	case ETraceTeamChangeResult::WouldUnbalance:return TEXT("REFUSED - THAT WOULD STACK THE TEAMS");
	case ETraceTeamChangeResult::TeamFull:      return FString::Printf(TEXT("REFUSED - %s IS FULL"), *Team);
	case ETraceTeamChangeResult::NotAllowed:    return TEXT("REFUSED");
	default:                                    return FString();
	}
}

void FTraceTeamSelect::Draw(AHUD* HUD, ATracePlayerController* PC, ATracePlayerState* LocalState)
{
	using namespace TraceTeamSelectLayout;
	using namespace TraceTeamSelectStyle;

	const float S = UIScale;
	const float CenterX = ViewW * 0.5f;

	HUD->DrawRect(Backdrop, 0.f, 0.f, ViewW, ViewH);

	// A wash at the top and the floor in the colour of the team the player is ON, so the screen is
	// legible at a glance as "you are currently blue" before a word is read.
	{
		const FLinearColor Tint = TraceTeamColor((LocalState != nullptr) ? LocalState->Team : ETraceTeam::None);
		constexpr int32 Bands = 10;
		const float WashH = 300.f * S;
		for (int32 Band = 0; Band < Bands; ++Band)
		{
			const float T = static_cast<float>(Band) / static_cast<float>(Bands);
			const float BandY = WashH * T;
			const float BandH = (WashH / Bands) + 1.f;

			HUD->DrawRect(WithAlpha(Tint, 0.055f * (1.f - T)), 0.f, BandY, ViewW, BandH);
			HUD->DrawRect(WithAlpha(Plate, 0.32f * (1.f - T)), 0.f, ViewH - BandY - BandH, ViewW, BandH);
		}
	}

	// ---- Header ---------------------------------------------------------------------------------
	TraceTeamSelectFile::Text(HUD, TEXT("SELECT YOUR TEAM"), Ink, CenterX, HeaderTop * S,
		TitleSize * S, TitleTrack * S, TraceText::EHAlign::Center);

	// The close-out countdown, right-aligned against the margin. A timeout the player cannot see is
	// indistinguishable from the game deciding at random for them — the same argument the character
	// select's own countdown makes.
	if (PC != nullptr && PC->TeamSelectDeadlineServerTime > 0.f)
	{
		const float Remaining = PC->GetTeamSelectTimeRemaining();
		const bool bUrgent = Remaining <= 5.f;
		const FLinearColor CountColor = bUrgent
			? WithAlpha(Danger, 0.72f + 0.28f * FMath::Sin(Now * 9.f))
			: InkSoft;

		TraceTeamSelectFile::Text(HUD,
			FString::Printf(TEXT("KEEPING YOUR TEAM IN %d"), FMath::Max(0, FMath::CeilToInt(Remaining))),
			CountColor, ViewW - (Margin * S), (HeaderTop + 16.f) * S,
			SizeLabel * S, TrackLabel * S, TraceText::EHAlign::Right);
	}

	// ---- The rule, said out loud ----------------------------------------------------------------
	//
	// The balance rule refuses things, and a refusal a player was never warned about reads as a bug.
	// One line, always on screen, in the same words the refusal uses.
	TraceTeamSelectFile::Text(HUD,
		TEXT("A SWITCH IS REFUSED IF IT WOULD LEAVE ONE SIDE MORE THAN ONE PLAYER LARGER. A BOT WILL STAND DOWN FOR YOU."),
		InkDim, CenterX, SubY * S, SizeLabel * S, TrackLabel * S, TraceText::EHAlign::Center);

	// ---- The two plates -------------------------------------------------------------------------
	const float AvailW = ViewW - (2.f * Margin * S) - (PlateGap * S);
	const float PlateW = FMath::Min(PlateMaxW * S, AvailW * 0.5f);
	const float TotalW = (PlateW * 2.f) + (PlateGap * S);
	const float FirstX = CenterX - (TotalW * 0.5f);

	DrawTeamPlate(HUD, PC, LocalState, RowBlue, FirstX, PlateTop * S, PlateW, PlateH * S);
	DrawTeamPlate(HUD, PC, LocalState, RowOrange, FirstX + PlateW + (PlateGap * S), PlateTop * S, PlateW, PlateH * S);

	// ---- The server's verdict -------------------------------------------------------------------
	{
		const FString Verdict = VerdictLine(PC);
		if (!Verdict.IsEmpty())
		{
			const bool bRefusal = Verdict.StartsWith(TEXT("REFUSED"));
			TraceTeamSelectFile::Text(HUD, Verdict, bRefusal ? Danger : Ink, CenterX, VerdictY * S,
				SizeLead * S, 1.4f * S, TraceText::EHAlign::Center);
		}
	}

	// ---- Footer ---------------------------------------------------------------------------------
	//
	// D32-PADMENU — ONE line, not two, and the pad's half only appears once a controller has been
	// seen on this machine (UTraceGamepadInputSubsystem::HasSeenGamepadInput). A keyboard-only player
	// reads exactly what they read before this tranche; a player holding a pad is told the three
	// buttons that do something here and nothing else. The hint is gated, the INPUT never is — the
	// first button a player presses has to work, and it cannot if the screen is waiting to have seen
	// one.
	{
		const FString KeyboardLine = FString::Printf(
			TEXT("1 / 2 OR ARROWS + ENTER   SELECT TEAM        C   CHANGE CHARACTER        %s   CLOSE"),
			OpenKeyName());

		// A SECOND LINE, NOT A LONGER ONE. The keyboard line is already 95 characters and fills a
		// 1280-wide window at this point size; appending the pad's three buttons to it would have run
		// off both edges, and the shrink-to-fit below would then have answered by making the whole
		// legend too small to read. The reference layout is 1080 high and this line sits at 900, so
		// there is room under it — measured, not assumed: FooterY + PadFooterGap is 928 of 1080.
		const bool bPadLine = TracePadMenu::HasSeenPad(PC);
		const FString PadLine = TEXT("A   SELECT TEAM        X   CHANGE CHARACTER        B   CLOSE");

		// ---- FIT, and it turned out to be needed for a line that predates this tranche -----------
		//
		// PHOTOGRAPHED, not assumed: at 800x600 the KEYBOARD line above already ran off BOTH edges of
		// the screen — it is typeset against a 1920-wide reference and UIScale only follows the
		// HEIGHT, so a 4:3 window gets full-size type in two thirds of the width. The pad line is
		// shorter and fitted anyway; both are measured together and given the SAME scale, because two
		// legends for two devices set at two sizes would read as a mistake.
		//
		// The 0.70 floor is the same trade UI plan WP5 made on the JOIN panel: below it the type is
		// unreadable and clipping is the better failure. Nothing changes at 16:9, where both lines
		// already fit at 1.0 — verified at 1280x720.
		const float FooterRoom = ViewW - 2.f * Margin * S;
		float FooterScale = 1.f;
		{
			float Widest = TraceTeamSelectFile::Width(KeyboardLine, SizeBody * S, TrackLabel * S);
			if (bPadLine)
			{
				Widest = FMath::Max(Widest, TraceTeamSelectFile::Width(PadLine, SizeBody * S, TrackLabel * S));
			}
			if (Widest > FooterRoom && Widest > 1.f)
			{
				FooterScale = FMath::Max(0.70f, FooterRoom / Widest);
			}
		}

		TraceTeamSelectFile::Text(HUD, KeyboardLine, InkSoft, CenterX, FooterY * S,
			SizeBody * S * FooterScale, TrackLabel * S * FooterScale, TraceText::EHAlign::Center);

		if (bPadLine)
		{
			TraceTeamSelectFile::Text(HUD, PadLine, Ink, CenterX, (FooterY + PadFooterGap) * S,
				SizeBody * S * FooterScale, TrackLabel * S * FooterScale, TraceText::EHAlign::Center);
		}
	}

	DrawCursor(HUD);
}

void FTraceTeamSelect::DrawTeamPlate(AHUD* HUD, ATracePlayerController* PC, ATracePlayerState* LocalState,
	int32 Row, float X, float Y, float W, float H)
{
	using namespace TraceTeamSelectLayout;
	using namespace TraceTeamSelectStyle;

	const float S = UIScale;
	const ETraceTeam Team = TeamForRow(Row);
	const FLinearColor Tint = TraceTeamColor(Team);

	RowRects[Row] = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));

	const bool bHighlighted = (Highlighted == Row);
	const bool bCurrent = (LocalState != nullptr) && (LocalState->Team == Team);

	// The rule, asked exactly as the server will ask it. bAllowed is FALSE only for a row that would
	// actually be refused, so "greyed" and "would be refused" are the same fact rather than two.
	const AGameStateBase* const BaseGameState =
		(PC != nullptr && PC->GetWorld() != nullptr) ? PC->GetWorld()->GetGameState() : nullptr;

	FString Reason;
	const bool bAllowed = bCurrent
		|| ATraceGameMode::IsTeamSwitchAllowed(BaseGameState, LocalState, Team, Reason);

	// ---- Plate ---------------------------------------------------------------------------------
	const float FillMul = bAllowed ? 1.f : 0.45f;
	HUD->DrawRect(WithAlpha(Dimmed(Plate, 0.62f * FillMul), 0.94f), X, Y, W, H);

	// A band of the team's own colour across the top of the plate. It is the fastest read on the
	// screen and the one thing that makes an ORANGE choice look orange before any text is parsed —
	// which is the whole reason the amber team has never appeared in a screenshot of this game.
	HUD->DrawRect(WithAlpha(Dimmed(Tint, FillMul), bHighlighted ? 1.f : 0.75f), X, Y, W, 10.f * S);

	TraceTeamSelectFile::StrokeRect(HUD, X, Y, W, H, FMath::Max(1.f, (bHighlighted ? 3.f : 1.f) * S),
		bHighlighted ? WithAlpha(Tint, 1.f) : WithAlpha(Ink, 0.28f));

	const float PadX = PlatePad * S;
	float CursorY = Y + (PlatePad * S) + (14.f * S);

	// ---- Name and key chip ----------------------------------------------------------------------
	TraceTeamSelectFile::Text(HUD, TraceTeamName(Team).ToString().ToUpper(),
		bAllowed ? Ink : Dimmed(Ink, 0.6f), X + PadX, CursorY, SizeDisplay * S, 3.0f * S);

	TraceTeamSelectFile::Text(HUD, (Row == RowBlue) ? TEXT("1") : TEXT("2"),
		InkDim, X + W - PadX, CursorY + (10.f * S), SizeLead * S, 0.f, TraceText::EHAlign::Right);

	CursorY += TraceText::LineHeight(SizeDisplay * S) + (14.f * S);

	// ---- The count, which is what the rule is about ---------------------------------------------
	TArray<const ATracePlayerState*> Members;
	TraceTeamSelectFile::GatherTeam(BaseGameState, Team, Members);

	int32 BotCount = 0;
	for (const ATracePlayerState* const Member : Members)
	{
		if (Member != nullptr && Member->IsABot())
		{
			++BotCount;
		}
	}

	const int32 TeamCap = FMath::Max(1, UTraceSettings::Get().PlayersPerTeam);
	TraceTeamSelectFile::Text(HUD,
		FString::Printf(TEXT("%d / %d   (%d BOT%s)"), Members.Num(), TeamCap, BotCount,
			(BotCount == 1) ? TEXT("") : TEXT("S")),
		InkSoft, X + PadX, CursorY, SizeLead * S, 1.4f * S);

	CursorY += TraceText::LineHeight(SizeLead * S) + (18.f * S);

	// A hairline under the header block.
	HUD->DrawRect(WithAlpha(Ink, 0.18f), X + PadX, CursorY, W - (2.f * PadX), FMath::Max(1.f, 1.f * S));
	CursorY += 18.f * S;

	// ---- The roster ------------------------------------------------------------------------------
	//
	// Names, because "3 / 5" does not answer the question a player actually has, which is "are my
	// friends on that side". Bots are labelled rather than hidden: a bot on the destination is the
	// reason a switch that looks like it would stack the teams is allowed, and a player who cannot
	// see the bots cannot see why.
	const float RowH = TraceText::LineHeight(SizeBody * S) + (6.f * S);
	for (const ATracePlayerState* const Member : Members)
	{
		if (CursorY + RowH > Y + H - (PlatePad * S) - (46.f * S))
		{
			TraceTeamSelectFile::Text(HUD, TEXT("..."), InkDim, X + PadX, CursorY, SizeBody * S);
			break;
		}

		const bool bIsYou = (Member == LocalState);
		const FLinearColor NameColor = bIsYou ? Tint : (Member->IsABot() ? InkDim : InkSoft);

		TraceTeamSelectFile::Text(HUD, TraceTeamSelectFile::SafeName(Member), NameColor,
			X + PadX, CursorY, SizeBody * S);

		if (bIsYou)
		{
			TraceTeamSelectFile::Text(HUD, TEXT("YOU"), Tint, X + W - PadX, CursorY,
				SizeLabel * S, TrackLabel * S, TraceText::EHAlign::Right);
		}
		else if (Member->IsABot())
		{
			TraceTeamSelectFile::Text(HUD, TEXT("BOT"), InkDim, X + W - PadX, CursorY,
				SizeLabel * S, TrackLabel * S, TraceText::EHAlign::Right);
		}

		CursorY += RowH;
	}

	// ---- The footer of the plate: what pressing it would do ---------------------------------------
	const float StatusY = Y + H - (PlatePad * S) - TraceText::LineHeight(SizeLabel * S);
	if (bCurrent)
	{
		TraceTeamSelectFile::Text(HUD, TEXT("YOUR TEAM"), Tint, X + PadX, StatusY,
			SizeLabel * S, TrackLabel * S);
	}
	else if (!bAllowed)
	{
		// The reason, VERBATIM from the rule, so the greyed plate and the server's refusal say the
		// same thing in the same words.
		TraceTeamSelectFile::Text(HUD, Reason, Danger, X + PadX, StatusY, SizeLabel * S, TrackLabel * S);
	}
	else
	{
		TraceTeamSelectFile::Text(HUD, TEXT("PRESS TO JOIN"), Ink, X + PadX, StatusY,
			SizeLabel * S, TrackLabel * S);
	}
}

void FTraceTeamSelect::DrawCursor(AHUD* HUD)
{
	if (!bHasCursor)
	{
		return;
	}

	// ONE POINTER, DRAWN IN ONE PLACE. The same shared draw the character select, the options menu
	// and the menu HUD all call; the fallback cross below is the same fallback they use.
	if (TraceHardwareCursor::DrawPointer(HUD, CursorPos, UIScale))
	{
		return;
	}

	const float Size = 9.f * UIScale;
	const float Thick = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceTeamSelectStyle::WithAlpha(TraceTeamSelectStyle::Ink, 0.95f);

	HUD->DrawLine(CursorPos.X - Size, CursorPos.Y, CursorPos.X - Size * 0.35f, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X + Size * 0.35f, CursorPos.Y, CursorPos.X + Size, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y - Size, CursorPos.X, CursorPos.Y - Size * 0.35f, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y + Size * 0.35f, CursorPos.X, CursorPos.Y + Size, Color, Thick);
}

#if !UE_BUILD_SHIPPING

// =============================================================================================
// Trace.Teams.* — the console surface a headless run drives this screen through
//
// Every command below sets a plain int that the next Tick consumes, for the reason
// GTraceCharacterSelectDebugPick's comment gives: a console command that held a pointer to a member
// of an object owned by an AHUD is a dangling pointer waiting for a map change.
//
// AND EVERY ONE OF THEM GOES THROUGH THE REAL PATH. Trace.Teams.Pick does not call
// ATraceGameMode::RequestTeamChange; it drives FTraceTeamSelect::Confirm, which applies the local
// belief, sends the same Server RPC a key press sends, and gets the same verdict back. A harness
// that reached past this file would pass with the entire screen disconnected — which is exactly the
// failure the character select's own DebugPick comment records.
// =============================================================================================

namespace TraceTeamSelectCommands
{
	/**
	 * The local game world, or null.
	 *
	 * Same shape TracePracticeRange.cpp's FindPracticeWorld uses, and for the same reason: a console
	 * command has no world of its own, and a PIE session has more than one.
	 */
	UWorld* FindGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* const Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	ATracePlayerController* LocalController()
	{
		UWorld* const World = FindGameWorld();
		ATracePlayerController* const PC = (World != nullptr)
			? Cast<ATracePlayerController>(World->GetFirstPlayerController()) : nullptr;

		if (PC == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[TeamSelect] no local player controller."));
		}
		return PC;
	}

	// ---------------------------------------------------------------------------------------------
	// THREE OF THESE ARE IMMEDIATE AND ONE IS LATCHED. The split is not arbitrary.
	//
	//   * Select / Close / Character each call exactly ONE function on the controller, and that call
	//     IS the whole body of the H and C key handlers in PollInput. Routing them through a latch
	//     would add a frame of delay and bypass nothing, because there is nothing on the screen
	//     object between the key and the RPC.
	//   * Pick has to go through FTraceTeamSelect::Confirm, which applies this screen's own belief
	//     about the balance rule and its own cooldown. A command that sent ServerRequestTeam directly
	//     would pass with the entire screen disconnected — the exact failure the character select's
	//     DebugPick comment records — so it stays latched and lands on a Tick where the screen is up.
	// ---------------------------------------------------------------------------------------------

	void OpenCommand(const TArray<FString>& Args)
	{
		if (ATracePlayerController* const PC = LocalController())
		{
			PC->ServerRequestOpenTeamSelect();
		}
	}

	void CloseCommand(const TArray<FString>& Args)
	{
		if (ATracePlayerController* const PC = LocalController())
		{
			PC->ServerRequestCloseTeamSelect();
		}
	}

	/** Handle for the delayed character switch below. */
	FTimerHandle GCharacterSwitchTimer;

	/**
	 * Trace.Teams.Character [delaySeconds]
	 *
	 * THE DELAY IS NOT A CONVENIENCE. What a verification of (b) has to show is that the E COOLDOWN
	 * SURVIVES the switch — and a cooldown only exists after an ability has actually fired, which
	 * takes an input tick after the key goes down. Every command in a -TraceExec round runs in ONE
	 * callback, so "press E, then switch" written on one line switches while the press is still in
	 * flight and measures a cooldown of zero against a cooldown of zero: a check that cannot fail.
	 * The delay is what puts the switch after the ability, using the run's two rounds rather than a
	 * third that does not exist.
	 */
	void CharacterCommand(const TArray<FString>& Args)
	{
		const float Delay = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 0.f;

		if (Delay <= 0.f)
		{
			if (ATracePlayerController* const PC = LocalController())
			{
				PC->ServerRequestCharacterSwitch();
			}
			return;
		}

		UWorld* const World = FindGameWorld();
		if (World == nullptr)
		{
			return;
		}

		World->GetTimerManager().ClearTimer(GCharacterSwitchTimer);
		World->GetTimerManager().SetTimer(GCharacterSwitchTimer, FTimerDelegate::CreateLambda([]()
		{
			if (ATracePlayerController* const PC = LocalController())
			{
				PC->ServerRequestCharacterSwitch();
			}
		}), Delay, /*bLoop=*/false);

		UE_LOG(LogTraceGame, Display,
			TEXT("[TeamSelect] Console: character switch in %.1fs."), Delay);
	}

	void PickCommand(const TArray<FString>& Args)
	{
		const FString Wanted = (Args.Num() > 0) ? Args[0].ToLower() : FString(TEXT("orange"));
		GTraceTeamSelectDebugPick = Wanted.StartsWith(TEXT("b")) ? 1 : 2;

		UE_LOG(LogTraceGame, Display, TEXT("[TeamSelect] Console: %s queued (screen must be open)."),
			(GTraceTeamSelectDebugPick == 1) ? TEXT("BLUE") : TEXT("ORANGE"));
	}

	/**
	 * Trace.Teams.PickRaw <blue|orange> — THE CLIENT-BELIEF BYPASS, and the only way to see the
	 * SERVER refuse.
	 *
	 * FTraceTeamSelect::Confirm applies the balance rule locally and does not send a request it
	 * already believes will be refused. That is the right behaviour — it costs no round trip and
	 * gives the same message — but it means a healthy client can never demonstrate the server's own
	 * enforcement, and "the rule is enforced on the server" would be a claim about code nobody had
	 * run. This sends ServerRequestTeam with no local test at all, which is exactly what a modified
	 * client would do, and the refusal that comes back is the server's.
	 *
	 * Dev only, so it is not a cheat surface: the whole file is compiled out of Shipping.
	 */
	void PickRawCommand(const TArray<FString>& Args)
	{
		const FString Wanted = (Args.Num() > 0) ? Args[0].ToLower() : FString(TEXT("orange"));
		const ETraceTeam Team = Wanted.StartsWith(TEXT("b")) ? ETraceTeam::Blue : ETraceTeam::Orange;

		if (ATracePlayerController* const PC = LocalController())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[TeamSelect] Console: RAW %s — sent with NO local belief test, so whatever comes "
				     "back is the server's own verdict."),
				*TraceTeamName(Team).ToString());
			PC->ServerRequestTeam(Team);
		}
	}

	void ReportOnce()
	{
		if (ATracePlayerController* const PC = LocalController())
		{
			TraceTeamSelectFile::PrintReport(PC, PC->GetTracePlayerState());
		}
	}

	/** Handle for the repeating report below. One per process; a second call re-arms it. */
	FTimerHandle GReportTimer;

	/** Reports still owed by the repeat schedule. File scope for the same reason the handle is. */
	int32 GReportsRemaining = 0;

	/**
	 * Trace.Teams.Report [repeats] [interval]
	 *
	 * *** THE REPEAT IS WHAT MAKES A BEFORE/AFTER MEASURABLE FROM A HEADLESS RUN, and it is here
	 * because the alternative was worse. *** The deferred-exec harness gives a run TWO moments
	 * (-TraceExecAt and -TraceExec2At). Every claim this feature makes needs THREE — the state
	 * before, the action, and the state once it has settled a round trip and a 4 Hz poll later — and
	 * an action whose result is read in the same callback that caused it reads the BEFORE picture and
	 * calls it an after. That is not hypothetical: it is exactly the character-census failure
	 * recorded at the top of TraceAutoShot.cpp. A report that keeps printing on its own clock gives a
	 * run as many moments as it needs without a third round.
	 */
	void ReportCommand(const TArray<FString>& Args)
	{
		ReportOnce();

		const int32 Repeats = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 0, 60) : 0;
		const float Interval = (Args.Num() > 1) ? FMath::Max(0.25f, FCString::Atof(*Args[1])) : 2.f;
		if (Repeats <= 0)
		{
			return;
		}

		UWorld* const World = FindGameWorld();
		if (World == nullptr)
		{
			return;
		}

		GReportsRemaining = Repeats;
		World->GetTimerManager().ClearTimer(GReportTimer);
		World->GetTimerManager().SetTimer(GReportTimer, FTimerDelegate::CreateLambda([]()
		{
			ReportOnce();
			if (--GReportsRemaining <= 0)
			{
				if (UWorld* const Live = FindGameWorld())
				{
					Live->GetTimerManager().ClearTimer(GReportTimer);
				}
			}
		}), Interval, /*bLoop=*/true, Interval);

		UE_LOG(LogTraceGame, Display,
			TEXT("[TeamSelect.Report] %d more report(s) queued, every %.1fs."), Repeats, Interval);
	}

	FAutoConsoleCommand CmdReport(
		TEXT("Trace.Teams.Report"),
		TEXT("Dev only. Prints, FROM THIS MACHINE, the local player's team / character / screen "
		     "state, their health and E cooldown, both rosters as this machine has them, and what "
		     "the balance rule says about EACH side with its reason. Run it on the server and on a "
		     "client to show that a team change reached both."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceTeamSelectCommands::ReportCommand));

	FAutoConsoleCommand CmdOpen(
		TEXT("Trace.Teams.Select"),
		TEXT("Dev only. Asks the server to open the team-select screen for the local player, through "
		     "the same Server RPC the H key sends. No effect for a player who is still picking a "
		     "character - team select comes first, and reopening it over a character screen would put "
		     "the flow back to front."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceTeamSelectCommands::OpenCommand));

	FAutoConsoleCommand CmdClose(
		TEXT("Trace.Teams.Close"),
		TEXT("Dev only. Closes the team-select screen with nothing changed."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceTeamSelectCommands::CloseCommand));

	FAutoConsoleCommand CmdPick(
		TEXT("Trace.Teams.Pick"),
		TEXT("Dev only. Trace.Teams.Pick <blue|orange> - highlights that plate and confirms it, "
		     "exactly as pressing 1 or 2 on the open screen would. Applies this screen's own belief "
		     "about the balance rule first (so a stacking request is refused locally and says why) "
		     "and otherwise sends the real request and prints the server's verdict."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceTeamSelectCommands::PickCommand));

	FAutoConsoleCommand CmdPickRaw(
		TEXT("Trace.Teams.PickRaw"),
		TEXT("Dev only. Trace.Teams.PickRaw <blue|orange> - sends the team request with NO local "
		     "belief test, bypassing the screen entirely. The point is the refusal: a healthy client "
		     "never sends a request it knows will be refused, so this is the only way to make the "
		     "SERVER's own enforcement of the balance rule visible in a log."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceTeamSelectCommands::PickRawCommand));

	FAutoConsoleCommand CmdCharacter(
		TEXT("Trace.Teams.Character"),
		TEXT("Dev only. D31-TEAMS (b). Hands this player's character back so the shipped character "
		     "select reopens mid-match, with no reconnect. Same Server RPC the C key on the team "
		     "screen sends; refused when characters are off for the match."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceTeamSelectCommands::CharacterCommand));
}

#endif
