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
#include "Abilities/TraceAbilityComponent.h"      // AreCharactersEnabled, for CHANGE LOADOUT
#include "UI/TraceLoadoutSelect.h"       // IsReopenAllowed — the loadout lock, asked here and by the server
#include "UI/Text/TraceCanvasText.h"     // spec v22 §A1 — this screen types from the glyph atlas
#include "UI/Text/TraceGameText.h"       // the editable wording, Config/TraceGameText.ini
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"
#include "UI/Widgets/Menu/TraceMenuKit.h"        // the handmade kit: plates, chips, the blade pointer

#if !UE_BUILD_SHIPPING
int32 GTraceTeamSelectDebugPick = 0;
#endif

// THE HANDMADE KIT (UI/Widgets/Menu/TraceMenuKit.h): opaque black, navy plates with the orange hover
// glow baked into the hover plate, the dark disabled plate for a side the balance rule refuses, white
// words, KEY chips, the blade pointer. The pre-kit neon palette this screen used (Ink / InkDim /
// Danger on a navy backdrop, flat rects with 1-3 px borders) is gone; the team colours are the kit's
// own (TraceTeamColor: lifted navy and AmberLifted), carried as an accent inside each plate.
//
// NAMED, not anonymous: UBT compiles this module as a unity/jumbo build, so two files that each
// define something at the top of an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
namespace TraceTeamSelectStyle
{
	static FLinearColor WithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, A);
	}

	/** How long a verdict stays on screen. Matched to the character select's 3.5 s. */
	static constexpr float MessageDuration = 3.5f;
}

/**
 * THE LAYOUT, IN ONE PLACE — every number is a 1080p design pixel and is multiplied by UIScale,
 * exactly like TraceSelectLayout in the character select.
 *
 * The vertical budget adds up to 1080:
 *   34..104 title and countdown | 214..654 the two plates | 700 the verdict line | 960 the key
 *   legend, and 1002 the pad's once a pad has been seen — the loadout page's legend lines exactly, so
 *   the page turn does not move the footer. The plates were 570 tall for at most five names and left
 *   their lower 40 % empty.
 */
namespace TraceTeamSelectLayout
{
	constexpr float Margin      = 54.f;
	constexpr float HeaderTop   = 34.f;
	constexpr float TitleSize   = 42.f;
	constexpr float TitleTrack  = 7.0f;

	constexpr float PlateTop    = 214.f;
	constexpr float PlateH      = 440.f;
	constexpr float PlateGap    = 48.f;
	constexpr float PlatePad    = 30.f;

	/** A tall plate keeps a button-sized corner (stylespec §6). */
	constexpr float PlateCorner = 60.f;

	/** The 1 / 2 KEY chip in each plate's corner. */
	constexpr float PlateChipH  = 40.f;

	/** Plates are capped so they do not become billboards on an ultrawide viewport. */
	constexpr float PlateMaxW   = 520.f;

	constexpr float VerdictY    = 700.f;
	constexpr float FooterY     = 960.f;
	constexpr float FooterChipH = 32.f;

	/** D32-PADMENU — the pad legend's chips, below the keyboard's. */
	constexpr float PadFooterGap = 42.f;

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
			return TRACE_TEXT("TEAMSELECT.ROSTER_UNKNOWN_NAME", "?");
		}
		const FString Name = State->GetPlayerName();
		return Name.IsEmpty() ? TRACE_TEXT("TEAMSELECT.ROSTER_DEFAULT_NAME", "PLAYER") : Name.ToUpper();
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
			LocalVerdict.Reset();

			// THE FIRST POINTER SAMPLE OF THIS OPENING IS NOT A MOVE. A position left from the last
			// time the screen was up (an H mid-match) would otherwise count as one and take the
			// highlight.
			bHasCursor = false;

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

			// START ON THE PLAYER'S OWN TEAM. ENTER / A is the reflex "continue", and it now keeps the
			// team and moves on (the server answers AlreadyOnTeam and closes the screen). It used to
			// start on the other team, so the same reflex asked to switch sides — respawning the
			// player and bumping a bot — or, when the switch was refused, silently did nothing.
			const ETraceTeam Current = (LocalState != nullptr) ? LocalState->Team : ETraceTeam::None;
			Highlighted = (Current == ETraceTeam::Orange) ? RowOrange : RowBlue;

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

	if (!bOpen)
	{
		return;
	}

	// ONE POINTER ON SCREEN. While the pause menu (or the character page) is in front, it owns the
	// pointer; this screen keeps drawing underneath but not its own blade, which used to sit frozen
	// under the scrim beside the live one. The OS-arrow lease is renewed where the blade is drawn
	// (TraceMenuKit::ShowCursor), and by the character select for the overlay in front.
	bPointerOwned = bInputAllowed;

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
	//
	// *** ONLY WHILE THE LOADOUT MAY CHANGE. *** With the loadout page this is CHANGE LOADOUT, and in
	// live play it was a way round "locked until halftime". The server refuses it now; the key is not
	// read and not offered here either, by the same rule, so the legend never shows a dead key.
	if (CanChangeLoadout(PC))
	{
		const bool bPadAlt = TracePadMenu::AltPressed(PC);
		if (PC->WasInputKeyJustPressed(EKeys::C) || bPadAlt)
		{
			if (bPadAlt)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[TeamSelect] Pad X -> change loadout."));
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
		// The menus' one shared repeat clock, so the team page, the loadout page and the pause menu
		// scroll at one speed.
		if (NavDir != LastNavDir)
		{
			LastNavDir = NavDir;
			NextNavTime = Now + TracePadMenu::RepeatDelay;
			Highlighted = FMath::Clamp(Highlighted + NavDir, 0, RowCount - 1);
		}
		else if (Now >= NextNavTime)
		{
			NextNavTime = Now + TracePadMenu::RepeatInterval;
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
	// belief test and its request cooldown apply to a pad exactly as they do to a key. NOT SPACE: it is
	// Jump, and it is the key a player is most likely to be holding when this screen appears.
	{
		const bool bPadConfirm = TracePadMenu::ConfirmPressed(PC);
		if (PC->WasInputKeyJustPressed(EKeys::Enter) || bPadConfirm)
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
	bool bDestinationFull = false;
	if (LocalState != nullptr && LocalState->Team != Wanted
		&& !ATraceGameMode::IsTeamSwitchAllowed(BaseGameState, LocalState, Wanted, Reason, &bDestinationFull))
	{
		UE_LOG(LogTraceGame, Log, TEXT("[TeamSelect] Not sending %s: %s"),
			*TraceTeamName(Wanted).ToString(), *Reason);

		// SAID ON SCREEN, in the server's own words for the same refusal. It used to be a log line
		// only, so the press looked like it had done nothing.
		LocalVerdict = bDestinationFull
			? TRACE_TEXTF("TEAMSELECT.VERDICT_TEAM_FULL", "REFUSED - {0} IS FULL", { TraceTeamName(Wanted).ToString().ToUpper() })
			: TRACE_TEXT("TEAMSELECT.VERDICT_WOULD_UNBALANCE", "REFUSED - THAT WOULD STACK THE TEAMS");
		LocalVerdictTime = Now;
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

bool FTraceTeamSelect::CanChangeLoadout(const ATracePlayerController* PC)
{
	// The server's own rule (ServerRequestCharacterSwitch asks the same function), plus the switch
	// that makes the key meaningless altogether.
	return PC != nullptr && UTraceAbilityComponent::AreCharactersEnabled(PC)
		&& TraceLoadoutSelect::IsReopenAllowed(PC);
}

FString FTraceTeamSelect::VerdictLine(const ATracePlayerController* PC) const
{
	// THIS SCREEN'S OWN REFUSAL, when it is the newer of the two: the balance rule said no before
	// anything was sent, and the player is told so rather than seeing a press do nothing.
	const bool bLocalIsNewer = (PC == nullptr) || (LocalVerdictTime >= PC->LastTeamResultLocalTime);
	if (!LocalVerdict.IsEmpty() && bLocalIsNewer && (Now - LocalVerdictTime) <= TraceTeamSelectStyle::MessageDuration)
	{
		return LocalVerdict;
	}

	if (PC == nullptr || (Now - PC->LastTeamResultLocalTime) > TraceTeamSelectStyle::MessageDuration)
	{
		return FString();
	}

	const FString Team = TraceTeamName(PC->LastTeamResultTeam).ToString().ToUpper();

	switch (PC->LastTeamResult)
	{
	case ETraceTeamChangeResult::Granted:
		return TRACE_TEXTF("TEAMSELECT.VERDICT_MOVED_TO", "MOVED TO {0}", { Team });
	case ETraceTeamChangeResult::AlreadyOnTeam:
		return TRACE_TEXTF("TEAMSELECT.VERDICT_ALREADY_ON", "ALREADY ON {0}", { Team });
	case ETraceTeamChangeResult::WouldUnbalance:
		return TRACE_TEXT("TEAMSELECT.VERDICT_WOULD_UNBALANCE", "REFUSED - THAT WOULD STACK THE TEAMS");
	case ETraceTeamChangeResult::TeamFull:
		return TRACE_TEXTF("TEAMSELECT.VERDICT_TEAM_FULL", "REFUSED - {0} IS FULL", { Team });
	case ETraceTeamChangeResult::NotAllowed:
		return TRACE_TEXT("TEAMSELECT.VERDICT_REFUSED", "REFUSED");
	default:
		return FString();
	}
}

void FTraceTeamSelect::Draw(AHUD* HUD, ATracePlayerController* PC, ATracePlayerState* LocalState)
{
	const float S = UIScale;
	const float CenterX = ViewW * 0.5f;

	// OPAQUE BLACK (stylespec §1): by the time this ticks the HUD has drawn ammo, health, the
	// scoreboard and the crosshair, and one opaque page is what makes a modal modal.
	TraceMenuKit::DrawBackground(HUD, ViewW, ViewH);

	// ---- Header ---------------------------------------------------------------------------------
	const float TitleSize = TraceTeamSelectLayout::TitleSize * S;
	const float TitleTop = TraceTeamSelectLayout::HeaderTop * S;
	const FString TitleText = TRACE_TEXT("TEAMSELECT.TITLE", "SELECT YOUR TEAM");
	TraceTeamSelectFile::Text(HUD, TitleText, TraceMenuArtStyle::WordDefault,
		CenterX, TitleTop, TitleSize, TraceTeamSelectLayout::TitleTrack * S, TraceText::EHAlign::Center);
	const float TitleRight = CenterX
		+ 0.5f * TraceTeamSelectFile::Width(TitleText, TitleSize, TraceTeamSelectLayout::TitleTrack * S);

	// The close-out countdown, right-aligned against the margin and sat on the title's cap line. A
	// timeout the player cannot see is indistinguishable from the game deciding at random for them.
	if (PC != nullptr && PC->TeamSelectDeadlineServerTime > 0.f)
	{
		const float Remaining = PC->GetTeamSelectTimeRemaining();
		const bool bUrgent = Remaining <= 5.f;
		const FLinearColor Amber = TraceMenuArtStyle::AmberLifted();
		const FLinearColor CountColor = bUrgent
			? TraceTeamSelectStyle::WithAlpha(Amber, 0.72f + 0.28f * FMath::Sin(Now * 9.f))
			: TraceMenuKit::FurnitureUnselected;

		TraceText::FStyle CountStyle(TraceTeamSelectLayout::SizeLabel * S, CountColor, ETraceTextWeight::Light);
		CountStyle.Tracking = TraceTeamSelectLayout::TrackLabel * S;
		CountStyle.HAlign = TraceText::EHAlign::Right;
		const FString CountText = TRACE_TEXTF("TEAMSELECT.COUNTDOWN", "KEEPING YOUR TEAM IN {0}",
			{ FMath::Max(0, FMath::CeilToInt(Remaining)) });
		const float CountRight = ViewW - TraceTeamSelectLayout::Margin * S;

		// Beside the title where there is room, under it where there is not: on a 5:4 window the two
		// ran into each other.
		float CapMid = TitleTop + TraceText::Ascent(TitleSize, ETraceTextWeight::Light)
			- TraceText::CapHeight(TitleSize, ETraceTextWeight::Light) * 0.5f;
		if (CountRight - TraceText::MeasureWidth(CountText, CountStyle) < TitleRight + 24.f * S)
		{
			CapMid = TitleTop + TraceText::LineHeight(TitleSize) + 10.f * S;
		}
		TraceMenuKit::DrawTextCapCentered(HUD, CountText, CountRight, CapMid, CountStyle);
	}

	// ---- The two plates -------------------------------------------------------------------------
	const float Gap = TraceTeamSelectLayout::PlateGap * S;
	const float AvailW = ViewW - (2.f * TraceTeamSelectLayout::Margin * S) - Gap;
	const float PlateW = FMath::Min(TraceTeamSelectLayout::PlateMaxW * S, AvailW * 0.5f);
	const float TotalW = (PlateW * 2.f) + Gap;
	const float FirstX = CenterX - (TotalW * 0.5f);
	const float PlateY = TraceTeamSelectLayout::PlateTop * S;
	const float PlateHeight = TraceTeamSelectLayout::PlateH * S;

	// The highlighted plate LAST, so a neighbour cannot paint over its glow.
	const int32 FirstRow = (Highlighted == RowBlue) ? RowOrange : RowBlue;
	for (const int32 Row : { FirstRow, Highlighted })
	{
		DrawTeamPlate(HUD, PC, LocalState, Row, FirstX + (Row == RowOrange ? PlateW + Gap : 0.f), PlateY,
			PlateW, PlateHeight);
	}

	// ---- The verdict ----------------------------------------------------------------------------
	{
		const FString Verdict = VerdictLine(PC);
		if (!Verdict.IsEmpty())
		{
			// THE COLOUR ASKS THE VERDICT, NOT THE WORDS, so an owner who rewords a refusal cannot turn
			// it white. Amber is the kit's danger colour.
			const bool bLocal = !LocalVerdict.IsEmpty() && Verdict == LocalVerdict;
			const bool bRefusal = bLocal || ((PC != nullptr)
				&& (PC->LastTeamResult == ETraceTeamChangeResult::WouldUnbalance
					|| PC->LastTeamResult == ETraceTeamChangeResult::TeamFull
					|| PC->LastTeamResult == ETraceTeamChangeResult::NotAllowed));
			TraceTeamSelectFile::Text(HUD, Verdict,
				bRefusal ? TraceMenuArtStyle::AmberLifted() : TraceMenuArtStyle::WordDefault,
				CenterX, TraceTeamSelectLayout::VerdictY * S, TraceTeamSelectLayout::SizeLead * S, 1.4f * S,
				TraceText::EHAlign::Center);
		}
	}

	DrawFooter(HUD, PC);
	DrawCursor(HUD, PC);
}

void FTraceTeamSelect::DrawFooter(AHUD* HUD, ATracePlayerController* PC)
{
	// ---- Key legend -----------------------------------------------------------------------------
	//
	// KEY chips and one-word verbs, the loadout page's legend on the same line, so the page turn does
	// not move the footer. The pad's line appears once a controller has been seen on this machine
	// (the hint is gated; the input never is). CHANGE LOADOUT appears only while it is allowed.
	const float S = UIScale;
	const bool bLoadout = CanChangeLoadout(PC);
	const FString ChooseWord = TRACE_TEXT("TEAMSELECT.LEGEND_CHOOSE", "CHOOSE");
	const FString SelectWord = TRACE_TEXT("TEAMSELECT.LEGEND_SELECT", "SELECT");
	const FString LoadoutWord = TRACE_TEXT("TEAMSELECT.LEGEND_LOADOUT", "LOADOUT");
	const FString CloseWord = TRACE_TEXT("TEAMSELECT.LEGEND_CLOSE", "CLOSE");

	TArray<FTraceKitLegendItem> Keyboard;
	Keyboard.Add({ TRACE_TEXT("TEAMSELECT.KEY_MOVE", "ARROWS"), ChooseWord });
	Keyboard.Add({ TRACE_TEXT("TEAMSELECT.KEY_SELECT", "ENTER"), SelectWord });
	if (bLoadout)
	{
		Keyboard.Add({ TRACE_TEXT("TEAMSELECT.KEY_LOADOUT", "C"), LoadoutWord });
	}
	Keyboard.Add({ TRACE_TEXTF("TEAMSELECT.KEY_CLOSE", "{0}", { FString(OpenKeyName()) }), CloseWord });

	TArray<FTraceKitLegendItem> Pad;
	if (TracePadMenu::HasSeenPad(PC))
	{
		Pad.Add({ TRACE_TEXT("TEAMSELECT.PAD_KEY_MOVE", "D-PAD"), ChooseWord });
		Pad.Add({ TRACE_TEXT("TEAMSELECT.PAD_KEY_SELECT", "A"), SelectWord });
		if (bLoadout)
		{
			Pad.Add({ TRACE_TEXT("TEAMSELECT.PAD_KEY_LOADOUT", "X"), LoadoutWord });
		}
		Pad.Add({ TRACE_TEXT("TEAMSELECT.PAD_KEY_CLOSE", "B"), CloseWord });
	}

	// ONE SCALE FOR BOTH LINES, fitted to the window: UIScale follows the height, so a 4:3 window gets
	// full-size type in two thirds of the width.
	const float FullChipH = TraceTeamSelectLayout::FooterChipH * S;
	const float ChipH = TraceMenuKit::KeyLegendFit(FullChipH, ViewW - 2.f * TraceTeamSelectLayout::Margin * S,
		{ TraceMenuKit::KeyLegendWidth(Keyboard, FullChipH), TraceMenuKit::KeyLegendWidth(Pad, FullChipH) });

	const float FooterTop = TraceTeamSelectLayout::FooterY * S;
	TraceMenuKit::DrawKeyLegend(HUD, Keyboard, ViewW * 0.5f, FooterTop, ChipH, Now);
	TraceMenuKit::DrawKeyLegend(HUD, Pad, ViewW * 0.5f, FooterTop + TraceTeamSelectLayout::PadFooterGap * S, ChipH, Now);
}

void FTraceTeamSelect::DrawTeamPlate(AHUD* HUD, ATracePlayerController* PC, ATracePlayerState* LocalState,
	int32 Row, float X, float Y, float W, float H)
{
	const float S = UIScale;
	const ETraceTeam Team = TeamForRow(Row);
	const FLinearColor Tint = TraceTeamColor(Team);

	RowRects[Row] = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));

	const bool bHighlighted = (Highlighted == Row);
	const bool bCurrent = (LocalState != nullptr) && (LocalState->Team == Team);

	// The rule, asked exactly as the server will ask it. bAllowed is FALSE only for a row that would
	// actually be refused, so "the disabled plate" and "would be refused" are the same fact.
	const AGameStateBase* const BaseGameState =
		(PC != nullptr && PC->GetWorld() != nullptr) ? PC->GetWorld()->GetGameState() : nullptr;

	FString Reason;
	bool bDestinationFull = false;
	const bool bAllowed = bCurrent
		|| ATraceGameMode::IsTeamSwitchAllowed(BaseGameState, LocalState, Team, Reason, &bDestinationFull);

	// ---- Plate: the kit's, in the state the row is in -------------------------------------------
	const ETraceKitState State = TraceMenuKit::StateFor(bAllowed, bHighlighted);
	const FTraceKitVisuals Visuals = TraceMenuKit::VisualsFor(State);
	TraceMenuKit::DrawStatePlate(HUD, State, X, Y, W, H, Now, TraceTeamSelectLayout::PlateCorner * S);

	const float PadX = TraceTeamSelectLayout::PlatePad * S;
	float CursorY = Y + PadX;

	// ---- Name, the team's colour under it, and the key chip -----------------------------------------
	const float NameSize = TraceTeamSelectLayout::SizeDisplay * S;
	TraceTeamSelectFile::Text(HUD, TraceTeamName(Team).ToString().ToUpper(), Visuals.Label,
		X + PadX, CursorY, NameSize, 3.0f * S);

	const float ChipH = TraceTeamSelectLayout::PlateChipH * S;
	const FString KeyName = (Row == RowBlue)
		? TRACE_TEXT("TEAMSELECT.PLATE_KEY_BLUE", "1")
		: TRACE_TEXT("TEAMSELECT.PLATE_KEY_ORANGE", "2");
	const float ChipW = TraceMenuKit::KeyChipWidth(KeyName, ChipH);
	TraceMenuKit::DrawKeyChip(HUD, bAllowed ? ETraceKitState::Default : ETraceKitState::Disabled,
		X + W - PadX - ChipW, CursorY + (TraceText::LineHeight(NameSize) - ChipH) * 0.5f, ChipH, KeyName, Now);

	CursorY += TraceText::LineHeight(NameSize) + (4.f * S);

	// THE TEAM'S COLOUR, as an accent under its name: the fastest read on the screen, and the one
	// thing that makes the orange side look orange before a word is parsed. Dimmed with the plate.
	HUD->DrawRect(bAllowed ? Tint : TraceTeamSelectStyle::WithAlpha(Tint, 0.35f), X + PadX, CursorY, 96.f * S,
		FMath::Max(2.f, 4.f * S));
	CursorY += 18.f * S;

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

	// The plural is its own slot rather than two spellings of the whole line: an owner who renames
	// BOT has one place to do it, and the singular stays a deliberate empty string.
	const FString BotSuffix = (BotCount == 1)
		? FString()
		: TRACE_TEXT("TEAMSELECT.PLATE_BOT_PLURAL_SUFFIX", "S");

	TraceTeamSelectFile::Text(HUD,
		TRACE_TEXTF("TEAMSELECT.PLATE_COUNT", "{0} / {1}   ({2} BOT{3})",
			{ Members.Num(), TeamCap, BotCount, BotSuffix }),
		Visuals.Furniture, X + PadX, CursorY, TraceTeamSelectLayout::SizeLead * S, 1.4f * S);

	CursorY += TraceText::LineHeight(TraceTeamSelectLayout::SizeLead * S) + (14.f * S);

	// A hairline under the header block.
	HUD->DrawRect(TraceTeamSelectStyle::WithAlpha(Visuals.Furniture, 0.22f), X + PadX, CursorY, W - (2.f * PadX),
		FMath::Max(1.f, 1.f * S));
	CursorY += 16.f * S;

	// ---- The roster ------------------------------------------------------------------------------
	//
	// Names, because "3 / 5" does not answer "are my friends on that side". Bots are labelled rather
	// than hidden: a bot on the destination is why a switch that looks like it would stack the teams
	// is allowed.
	const float BodySize = TraceTeamSelectLayout::SizeBody * S;
	const float LabelSize = TraceTeamSelectLayout::SizeLabel * S;
	const float RowH = TraceText::LineHeight(BodySize) + (6.f * S);
	const float StatusY = Y + H - PadX - TraceText::LineHeight(LabelSize);
	const FLinearColor Quiet = TraceMenuArtStyle::WordDisabled;
	for (const ATracePlayerState* const Member : Members)
	{
		if (CursorY + RowH > StatusY - (8.f * S))
		{
			TraceTeamSelectFile::Text(HUD, TRACE_TEXT("TEAMSELECT.ROSTER_MORE", "..."), Quiet,
				X + PadX, CursorY, BodySize);
			break;
		}

		const bool bIsYou = (Member == LocalState);
		const FLinearColor NameColor = bIsYou ? Tint : (Member->IsABot() ? Quiet : Visuals.Furniture);

		TraceTeamSelectFile::Text(HUD, TraceTeamSelectFile::SafeName(Member), NameColor,
			X + PadX, CursorY, BodySize);

		if (bIsYou)
		{
			TraceTeamSelectFile::Text(HUD, TRACE_TEXT("TEAMSELECT.ROSTER_YOU", "YOU"), Tint,
				X + W - PadX, CursorY, LabelSize, TraceTeamSelectLayout::TrackLabel * S, TraceText::EHAlign::Right);
		}
		else if (Member->IsABot())
		{
			TraceTeamSelectFile::Text(HUD, TRACE_TEXT("TEAMSELECT.ROSTER_BOT", "BOT"), Quiet,
				X + W - PadX, CursorY, LabelSize, TraceTeamSelectLayout::TrackLabel * S, TraceText::EHAlign::Right);
		}

		CursorY += RowH;
	}

	// ---- The foot of the plate: what pressing it would do ---------------------------------------
	if (bCurrent)
	{
		TraceTeamSelectFile::Text(HUD, TRACE_TEXT("TEAMSELECT.PLATE_STATUS_CURRENT", "YOUR TEAM"), Tint,
			X + PadX, StatusY, LabelSize, TraceTeamSelectLayout::TrackLabel * S);
	}
	else if (!bAllowed)
	{
		// TWO WORDS, NOT THE RULE'S SENTENCE. The rule's reason ("BLUE IS FULL (5/5) AND HAS NO BOT TO
		// STAND DOWN") is a log line built outside the text document; the plate says which of the two
		// refusals it is, in words the document owns.
		TraceTeamSelectFile::Text(HUD,
			bDestinationFull ? TRACE_TEXT("TEAMSELECT.PLATE_STATUS_FULL", "FULL")
			                 : TRACE_TEXT("TEAMSELECT.PLATE_STATUS_UNEVEN", "UNEVEN TEAMS"),
			TraceMenuArtStyle::AmberLifted(), X + PadX, StatusY, LabelSize, TraceTeamSelectLayout::TrackLabel * S);
	}
	else
	{
		TraceTeamSelectFile::Text(HUD, TRACE_TEXT("TEAMSELECT.PLATE_STATUS_JOIN", "PRESS TO JOIN"), Visuals.Label,
			X + PadX, StatusY, LabelSize, TraceTeamSelectLayout::TrackLabel * S);
	}
}

void FTraceTeamSelect::DrawCursor(AHUD* HUD, ATracePlayerController* PC)
{
	// Not while something in front owns the pointer: that surface draws the live one.
	if (!bHasCursor || !bPointerOwned)
	{
		return;
	}

	// ONE POINTER, DRAWN IN ONE PLACE: the kit's blade, with the OS arrow's lease renewed as it draws.
	if (TraceMenuKit::ShowCursor(HUD, PC, TEXT("team select"), CursorPos, UIScale))
	{
		return;
	}

	const float Size = 9.f * UIScale;
	const float Thick = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceMenuArtStyle::WordDefault;

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
