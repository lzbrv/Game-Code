#include "UI/TraceLoadoutSelect.h"

#include "Engine/Canvas.h"
#include "GameFramework/HUD.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TracePlayerState.h"
#include "Settings/TraceGamepadInput.h"
#include "Settings/TraceUserSettings.h"
#include "Trace.h"
#include "UI/TraceAbilityNames.h"
#include "UI/Text/TraceCanvasText.h"
#include "UI/Text/TraceText.h"
#include "UI/Text/TraceGameText.h"

namespace
{
	int32 GTraceLoadoutScreenArmed = 1;
	FAutoConsoleVariableRef CVarLoadoutScreen(
		TEXT("Trace.UI.LoadoutScreen"),
		GTraceLoadoutScreenArmed,
		TEXT("1 (default): after team select, show the LOADOUT screen. 0: show the old ten-card "
		     "CHARACTER page instead. The two are mutually exclusive and the character page is still "
		     "whole behind this, so a playtest that dislikes the loadout screen can go back without a "
		     "build."),
		ECVF_Default);
}

namespace TraceLoadoutSelect
{
	bool IsArmed()
	{
		return GTraceLoadoutScreenArmed != 0;
	}
}

// =================================================================================================
// THE LAYOUT — every number a 1080p design pixel, multiplied by UIScale.
//
// The constants are deliberately the character screen's where they can be (Margin, HeaderTop,
// TitleSize, the grid gaps), because the two pages sit either side of one button press and any
// difference in margin or title size reads as the program changing rather than the page turning.
// =================================================================================================
namespace TraceLoadoutLayout
{
	constexpr float Margin      = 54.f;
	constexpr float HeaderTop   = 34.f;
	constexpr float TitleSize   = 42.f;
	constexpr float RuleY       = 106.f;

	constexpr float TabY        = 132.f;
	constexpr float TabH        = 64.f;
	constexpr float TabGap      = 14.f;

	constexpr float GridTop     = 226.f;
	constexpr float TileGapX    = 18.f;
	constexpr float TileGapY    = 16.f;
	constexpr int32 Columns     = 5;
	constexpr int32 Rows        = 2;

	constexpr float SavedY      = 900.f;
	constexpr float SavedH      = 46.f;
	constexpr float FooterY     = 986.f;

	constexpr float SizeTab     = 22.f;
	constexpr float SizeTabSub  = 15.f;
	constexpr float SizeName    = 24.f;
	constexpr float SizeBody    = 15.f;
	constexpr float SizeFooter  = 17.f;
	constexpr float SizeSaved   = 16.f;

	constexpr float CardPad     = 14.f;

	// The palette IS the character screen's, read off TraceCharacterSelect.cpp rather than re-picked.
	static const FLinearColor Cyan   (0.16f, 0.88f, 1.00f, 1.00f);
	static const FLinearColor Ink    (0.94f, 0.97f, 1.00f, 1.00f);
	static const FLinearColor InkSoft(0.76f, 0.84f, 0.90f, 1.00f);
	static const FLinearColor InkDim (0.56f, 0.66f, 0.75f, 1.00f);
	static const FLinearColor Good   (0.24f, 0.90f, 0.42f, 1.00f);
	static const FLinearColor Plate  (0.11f, 0.16f, 0.32f, 0.92f);
	static const FLinearColor PlateHi(0.16f, 0.24f, 0.46f, 0.96f);
}

// A NAMED NAMESPACE, LIKE TraceCharacterSelectFile AND TraceTeamSelectFile NEXT DOOR.
//
// An anonymous namespace would have been the obvious choice and is the wrong one here: its names are
// visible unqualified for the rest of the TRANSLATION UNIT, and Unreal's unity builds put many .cpp
// files in one. StrokeRect and DrawWrapped would then be loose in every file compiled after this
// one — which is exactly how the sibling screens' own file-locals are kept out of each other's way,
// and why both of them are named. Following the convention that already exists beats discovering
// why it exists a second time.
namespace TraceLoadoutSelectFile
{
	// *** NO `using namespace TraceLoadoutLayout;` AT THIS SCOPE EITHER, AND THAT IS DELIBERATE. ***
	//
	// There was one here, and under Unreal's UNITY BUILDS it broke the Windows compile. A
	// using-directive at namespace scope applies to the rest of the TRANSLATION UNIT, and a unity
	// build concatenates many .cpp files into one: every name this namespace exports — Margin, Cyan,
	// Ink, Good, Columns, Rows, Plate, RuleY, twenty-nine of them, most far too common to be safe —
	// became visible unqualified in every file compiled after this one. TraceMenuHUD.cpp then
	// declared its own local `RuleY` and `Plate` and MSVC raised C4459 "hides global declaration",
	// which is an error under Unreal's warnings-as-errors. Clang on macOS does not enable it.
	//
	// The directives now live INSIDE the functions that need them, where they cannot escape the
	// function, let alone the file.
	void StrokeRect(AHUD* HUD, float X, float Y, float W, float H, float Thick, const FLinearColor& C)
	{
		if (HUD == nullptr || W <= 0.f || H <= 0.f)
		{
			return;
		}
		const float T = FMath::Max(1.f, Thick);
		HUD->DrawRect(C, X, Y, W, T);
		HUD->DrawRect(C, X, Y + H - T, W, T);
		HUD->DrawRect(C, X, Y, T, H);
		HUD->DrawRect(C, X + W - T, Y, T, H);
	}

	int32 KitCount()
	{
		return static_cast<int32>(TraceCharacterRoster::LastId)
			- static_cast<int32>(TraceCharacterRoster::FirstId) + 1;
	}

	ETraceCharacterId KitAtCard(int32 CardIndex)
	{
		const int32 Count = KitCount();
		if (Count <= 0)
		{
			return ETraceCharacterId::None;
		}
		const int32 Wrapped = ((CardIndex % Count) + Count) % Count;
		return static_cast<ETraceCharacterId>(TraceCharacterRoster::FirstId + Wrapped);
	}

	int32 CardForKit(ETraceCharacterId Id)
	{
		const int32 Card = static_cast<int32>(Id) - static_cast<int32>(TraceCharacterRoster::FirstId);
		return (Card >= 0 && Card < KitCount()) ? Card : 0;
	}

	/**
	 * Draws @p Text inside @p MaxWidth, breaking on spaces, and returns the Y below the last line.
	 *
	 * *** THE FIRST BUILD OF THIS SCREEN DREW EACH DESCRIPTION AS ONE LINE. *** Every card's prose ran
	 * straight through its own border and across its neighbour — five overlapping sentences per row,
	 * which is precisely the "completely messed up" the rebuild was asked to fix, reappearing in a new
	 * form. A card whose whole content is prose has to wrap; there is no shorter version to fall back
	 * on, because these abilities have no names.
	 *
	 * Measured in the SAME style it draws in. TraceText's own note is blunt about this: MeasureWidth
	 * is only right if the caller hands it the weight it is going to draw with, because erosion makes
	 * the light cut narrower than the bold one at the same size.
	 */
	float DrawWrapped(AHUD* HUD, const FString& Text, float X, float Y, float MaxWidth,
		float Size, const FLinearColor& Color, float LineGap)
	{
		if (HUD == nullptr || Text.IsEmpty() || MaxWidth <= 0.f)
		{
			return Y;
		}

		TArray<FString> Words;
		Text.ParseIntoArray(Words, TEXT(" "), /*InCullEmpty=*/true);

		const float LineStep = Size + LineGap;
		FString Line;
		float CursorY = Y;

		for (const FString& Word : Words)
		{
			const FString Candidate = Line.IsEmpty() ? Word : (Line + TEXT(" ") + Word);
			if (TraceText::MeasureWidth(Candidate, Size) <= MaxWidth || Line.IsEmpty())
			{
				// A single word wider than the card still goes on its own line rather than vanishing:
				// overflowing by a few pixels is a smaller lie than dropping a word of the rules.
				Line = Candidate;
				continue;
			}

			TraceCanvasText::Draw(HUD, Line, X, CursorY, Size, Color);
			CursorY += LineStep;
			Line = Word;
		}

		if (!Line.IsEmpty())
		{
			TraceCanvasText::Draw(HUD, Line, X, CursorY, Size, Color);
			CursorY += LineStep;
		}
		return CursorY;
	}

	const TCHAR* SlotHeading(ETraceLoadoutSlot Slot)
	{
		switch (Slot)
		{
		case ETraceLoadoutSlot::Movement:  return *TRACE_TEXT("LOADOUT.HEADING_MOVEMENT", "MOVEMENT");
		case ETraceLoadoutSlot::Passive:   return *TRACE_TEXT("LOADOUT.HEADING_PASSIVE", "PASSIVE");
		case ETraceLoadoutSlot::Activated: return *TRACE_TEXT("LOADOUT.HEADING_ACTIVATED", "ACTIVATED");
		default:                           return TEXT("");
		}
	}

	/**
	 * What a tab shows UNDER its heading: the ability chosen for that slot.
	 *
	 * The activated slot can say its name. The other two have none, so they say the first few words
	 * of what the ability does — which is the only honest short form available and is still enough to
	 * tell at a glance that the slot is filled and roughly with what.
	 */
	FString TabSummary(const FTraceLoadout& Loadout, ETraceLoadoutSlot Slot)
	{
		const ETraceCharacterId Id = Loadout.Get(Slot);
		if (Id == ETraceCharacterId::None)
		{
			return TRACE_TEXT("LOADOUT.TAB_EMPTY", "NONE");
		}
		return TraceAbilityNames::ShortLabel(Id, Slot);
	}
}

// =================================================================================================
// Frame
// =================================================================================================

void FTraceLoadoutSelect::Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
	float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed)
{
	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;
	Now = InNow;

	// THE SERVER DECIDES WHETHER THIS IS UP — the one replicated condition, so "the half time break
	// shows this screen" is true by construction rather than by a clock that could miss the whistle.
	const bool bWantOpen = (LocalState != nullptr) && LocalState->IsCharacterSelectOpen();

	if (bWantOpen && !bOpen)
	{
		bStagedSeeded = false;

		// Swallow the remainder of this frame's input, exactly as the character screen does: the key
		// that was held when the page appeared would otherwise land on a card.
		IgnoreInputBeforeFrame = GFrameCounter + 1;
	}
	bOpen = bWantOpen;

	if (!bOpen)
	{
		bStagedSeeded = false;
		return;
	}

	if (!bStagedSeeded)
	{
		SyncStagedFromServer(LocalState);
		bStagedSeeded = true;
	}

	if (bInputAllowed && PC != nullptr && GFrameCounter >= IgnoreInputBeforeFrame)
	{
		PollKeys(PC, LocalState);
		PollPointer(PC, LocalState);
	}

	Draw(HUD, *TRACE_TEXT("LOADOUT.TITLE", "BUILD YOUR LOADOUT"),
		*TRACE_TEXT("LOADOUT.FOOTER",
			"CLICK OR ARROWS TO CHOOSE     1-5 LOAD SAVED     SHIFT+1-5 SAVE     ENTER TO LOCK IN"));
}

void FTraceLoadoutSelect::SyncStagedFromServer(ATracePlayerState* LocalState)
{
	Staged = FTraceLoadout();

	if (LocalState != nullptr)
	{
		if (const UTraceAbilityComponent* Comp = LocalState->FindComponentByClass<UTraceAbilityComponent>())
		{
			Staged = Comp->GetLoadout();
		}
	}

	// A player who has never picked gets a legal starting point rather than three empty slots: empty
	// is legal but it is nobody's intent, and the half-time clock can run out while they read.
	if (Staged.IsEmpty())
	{
		Staged = FTraceLoadout::Uniform(TraceLoadoutSelectFile::KitAtCard(0));
	}

	for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
	{
		Highlighted[Index] = TraceLoadoutSelectFile::CardForKit(Staged.Get(static_cast<ETraceLoadoutSlot>(Index)));
	}
	Tab = 0;
	LastMessage.Reset();
}

// =================================================================================================
// Input — keys and pad
// =================================================================================================

void FTraceLoadoutSelect::PollKeys(APlayerController* PC, ATracePlayerState* LocalState)
{
	// Q/E and the shoulder buttons change TAB; arrows and the stick walk the grid. Separating them
	// matters on a grid: on the character screen left/right walked the only row there was, and here
	// left/right has to mean "next card", or the two pages disagree about what an arrow does.
	const bool bTabLeft  = PC->WasInputKeyJustPressed(EKeys::Q);
	const bool bTabRight = PC->WasInputKeyJustPressed(EKeys::E);
	if (bTabLeft != bTabRight)
	{
		MoveTab(bTabRight ? 1 : -1);
	}

	const int32 KeyX = (PC->IsInputKeyDown(EKeys::Right) || PC->IsInputKeyDown(EKeys::D) ? 1 : 0)
	                 - (PC->IsInputKeyDown(EKeys::Left)  || PC->IsInputKeyDown(EKeys::A) ? 1 : 0);
	const int32 KeyY = (PC->IsInputKeyDown(EKeys::Down)  || PC->IsInputKeyDown(EKeys::S) ? 1 : 0)
	                 - (PC->IsInputKeyDown(EKeys::Up)    || PC->IsInputKeyDown(EKeys::W) ? 1 : 0);

	const int32 NavX = (KeyX != 0) ? KeyX : TracePadMenu::NavX(PC);
	const int32 NavY = (KeyY != 0) ? KeyY : TracePadMenu::NavY(PC);

	if (NavX == 0 && NavY == 0)
	{
		NextNavTime = 0.f;   // released: the next press is immediate, not delayed
	}
	else if (Now >= NextNavTime)
	{
		NextNavTime = Now + ((NextNavTime <= 0.f) ? NavRepeatDelay : NavRepeatInterval);

		// One axis per step. A diagonal on a stick would otherwise move a column AND a row in one
		// frame, which reads as the highlight jumping rather than walking.
		MoveCard((NavX != 0) ? NavX : NavY * TraceLoadoutLayout::Columns);
	}

	// THE FIVE SAVED LOADOUTS. A number recalls, SHIFT+number stores. Edge-triggered per key: this
	// page is up for forty-five seconds and a held 3 must recall once.
	static const FKey NumberKeys[5] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five };
	const bool bShift = PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift);
	for (int32 Index = 0; Index < 5; ++Index)
	{
		const bool bDown = PC->IsInputKeyDown(NumberKeys[Index]);
		if (bDown && !bNumberWasDown[Index])
		{
			bShift ? Store(Index) : Recall(Index);
		}
		bNumberWasDown[Index] = bDown;
	}

	// Confirm is edge-triggered too, or a held ENTER would send one request per frame.
	const bool bConfirmDown = PC->IsInputKeyDown(EKeys::Enter)
		|| PC->IsInputKeyDown(EKeys::SpaceBar)
		|| TracePadMenu::ConfirmPressed(PC);
	if (bConfirmDown && !bConfirmWasDown)
	{
		if (LibrarySlot != INDEX_NONE)
		{
			Store(LibrarySlot);
			CloseLibrary();
		}
		else
		{
			Confirm(LocalState);
		}
	}
	bConfirmWasDown = bConfirmDown;
}

void FTraceLoadoutSelect::MoveTab(int32 Delta)
{
	const int32 Count = static_cast<int32>(ETraceLoadoutSlot::Count);
	Tab = ((Tab + Delta) % Count + Count) % Count;
	LastMessage.Reset();
}

void FTraceLoadoutSelect::MoveCard(int32 Delta)
{
	const int32 Count = TraceLoadoutSelectFile::KitCount();
	if (Count <= 0)
	{
		return;
	}
	Highlighted[Tab] = ((Highlighted[Tab] + Delta) % Count + Count) % Count;

	// HIGHLIGHTING IS NOT EQUIPPING on a grid. The text version staged on every cursor move, which is
	// fine for a list you scroll but wrong for cards you point at: a pointer crossing the grid on its
	// way somewhere would rewrite the loadout under the player. You equip by clicking, or by ENTER.
}

void FTraceLoadoutSelect::EquipHighlighted()
{
	const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Tab);
	Staged.Set(Slot, TraceLoadoutSelectFile::KitAtCard(Highlighted[Tab]));
	LastMessage.Reset();
}

// =================================================================================================
// Input — the pointer
// =================================================================================================

void FTraceLoadoutSelect::PollPointer(APlayerController* PC, ATracePlayerState* LocalState)
{
	float MouseX = 0.f;
	float MouseY = 0.f;

	// MEASURED BEFORE CursorPos IS OVERWRITTEN — the whole point is the comparison against the
	// previous frame. Both rules below are the character screen's, carried over rather than
	// rediscovered; see TraceCharacterSelect.cpp for the screenshots that found them.
	bool bCursorMoved = false;
	if (PC->GetMousePosition(MouseX, MouseY))
	{
		const FVector2D NewPos(MouseX, MouseY);

		// THE FIRST SAMPLE IS NOT A MOVE. Counting it would hand the highlight to wherever the pointer
		// happened to be resting when the page opened.
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

	HoveredCard = INDEX_NONE;
	for (int32 Index = 0; Index < TraceLoadoutSelectFile::KitCount(); ++Index)
	{
		if (CardRects[Index].bIsValid && CardRects[Index].IsInside(CursorPos))
		{
			HoveredCard = Index;
			break;
		}
	}

	HoveredTab = INDEX_NONE;
	for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
	{
		if (TabRects[Index].bIsValid && TabRects[Index].IsInside(CursorPos))
		{
			HoveredTab = Index;
			break;
		}
	}

	HoveredSaved = INDEX_NONE;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		if (SavedRects[Index].bIsValid && SavedRects[Index].IsInside(CursorPos))
		{
			HoveredSaved = Index;
			break;
		}
	}

	bHoveredConfirm = ConfirmRect.bIsValid && ConfirmRect.IsInside(CursorPos);

	// *** THE POINTER MUST HAVE MOVED BEFORE IT MAY TAKE THE HIGHLIGHT. *** Without this a pointer
	// left resting over the grid pins the highlight there and NOTHING else can move it — not the
	// arrow keys, not the pad. The character screen shipped that bug and has the log to prove it.
	if (HoveredCard != INDEX_NONE && bCursorMoved)
	{
		Highlighted[Tab] = HoveredCard;
	}

	if (!bJustReleased)
	{
		return;
	}

	// ---- a click, released inside the thing it went down on ------------------------------------
	if (HoveredTab != INDEX_NONE)
	{
		Tab = HoveredTab;
		LastMessage.Reset();
		return;
	}

	if (HoveredCard != INDEX_NONE)
	{
		Highlighted[Tab] = HoveredCard;
		EquipHighlighted();
		return;
	}

	if (HoveredSaved != INDEX_NONE)
	{
		// A plain click RECALLS; shift-click stores. Same pairing as the number keys, so the two ways
		// of reaching the library cannot disagree about which is which.
		const bool bShift = PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift);
		bShift ? Store(HoveredSaved) : Recall(HoveredSaved);
		return;
	}

	if (bHoveredConfirm)
	{
		if (LibrarySlot != INDEX_NONE)
		{
			Store(LibrarySlot);
			CloseLibrary();
		}
		else
		{
			Confirm(LocalState);
		}
	}
}

// =================================================================================================
// Sending, and the saved library
// =================================================================================================

void FTraceLoadoutSelect::Confirm(ATracePlayerState* LocalState)
{
	if (LocalState == nullptr)
	{
		return;
	}

	UTraceAbilityComponent* Comp = LocalState->FindComponentByClass<UTraceAbilityComponent>();
	if (Comp == nullptr)
	{
		return;
	}

	// ASKED LOCALLY FIRST so the common refusal needs no round trip — and SENT ANYWAY, because this
	// screen is not the authority. If the server disagrees it wins and the player is told.
	FString Reason;
	if (!UTraceAbilityComponent::IsLoadoutLegal(Staged, &Reason))
	{
		LastMessage = Reason.ToUpper();
		LastMessageTime = Now;
		return;
	}

	Comp->ServerRequestSetLoadout(Staged);
	LastMessage = TRACE_TEXT("LOADOUT.LOCKED_IN", "LOCKED IN");
	LastMessageTime = Now;

	UE_LOG(LogTraceGame, Log, TEXT("[LoadoutScreen] sent %s"), *TraceLoadoutToString(Staged));
}

void FTraceLoadoutSelect::Recall(int32 Index)
{
	const FTraceLoadout Saved = UTraceUserSettings::Get().GetSavedLoadout(Index);

	// AN EMPTY SLOT IS NOT A LOADOUT. Recalling one would wipe the cards the player just chose, which
	// is the most expensive way to misread a keypress on a forty-five second clock.
	if (Saved.IsEmpty())
	{
		LastMessage = FString::Format(*TRACE_TEXT("LOADOUT.SLOT_EMPTY", "SLOT {0} IS EMPTY"),
			FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(Index + 1)) });
		LastMessageTime = Now;
		return;
	}

	Staged = Saved;
	for (int32 SlotIndex = 0; SlotIndex < static_cast<int32>(ETraceLoadoutSlot::Count); ++SlotIndex)
	{
		Highlighted[SlotIndex] = TraceLoadoutSelectFile::CardForKit(Staged.Get(static_cast<ETraceLoadoutSlot>(SlotIndex)));
	}

	LastMessage = FString::Format(*TRACE_TEXT("LOADOUT.SLOT_LOADED", "LOADED {0}"),
		FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(Index + 1)) });
	LastMessageTime = Now;
}

void FTraceLoadoutSelect::Store(int32 Index)
{
	UTraceUserSettings::Get().SetSavedLoadout(Index, Staged);
	LastMessage = FString::Format(*TRACE_TEXT("LOADOUT.SLOT_SAVED", "SAVED TO {0}"),
		FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(Index + 1)) });
	LastMessageTime = Now;

	UE_LOG(LogTraceGame, Log, TEXT("[LoadoutScreen] saved %s to slot %d"),
		*TraceLoadoutToString(Staged), Index + 1);
}

// =================================================================================================
// Library mode
// =================================================================================================

void FTraceLoadoutSelect::OpenLibrary(int32 SlotIndex)
{
	LibrarySlot = FMath::Clamp(SlotIndex, 0, UTraceUserSettings::SavedLoadoutCount - 1);

	Staged = UTraceUserSettings::Get().GetSavedLoadout(LibrarySlot);
	if (Staged.IsEmpty())
	{
		Staged = FTraceLoadout::Uniform(TraceLoadoutSelectFile::KitAtCard(0));
	}
	for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
	{
		Highlighted[Index] = TraceLoadoutSelectFile::CardForKit(Staged.Get(static_cast<ETraceLoadoutSlot>(Index)));
	}
	Tab = 0;
	LastMessage.Reset();
	IgnoreInputBeforeFrame = GFrameCounter + 1;
	bConfirmWasDown = true;
	bCancelWasDown = true;
}

void FTraceLoadoutSelect::CloseLibrary()
{
	LibrarySlot = INDEX_NONE;
}

bool FTraceLoadoutSelect::TickLibrary(AHUD* HUD, APlayerController* PC,
	float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed)
{
	if (LibrarySlot == INDEX_NONE)
	{
		return false;
	}

	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;
	Now = InNow;

	if (bInputAllowed && PC != nullptr && GFrameCounter >= IgnoreInputBeforeFrame)
	{
		PollKeys(PC, /*LocalState=*/nullptr);   // no player state here: this mode cannot reach a server
		PollPointer(PC, /*LocalState=*/nullptr);

		// ESCAPE LEAVES WITHOUT SAVING, which is why a library editor needs a cancel at all: the slot
		// the player was browsing from must still be there when they change their mind.
		const bool bCancelDown = PC->IsInputKeyDown(EKeys::Escape) || PC->IsInputKeyDown(EKeys::BackSpace);
		if (bCancelDown && !bCancelWasDown)
		{
			CloseLibrary();
			bCancelWasDown = true;
			return false;
		}
		bCancelWasDown = bCancelDown;
	}

	if (LibrarySlot == INDEX_NONE)
	{
		return false;   // a confirm inside PollKeys closed us
	}

	const FString Title = FString::Format(*TRACE_TEXT("LOADOUT.LIBRARY_TITLE", "LOADOUT {0}"),
		FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(LibrarySlot + 1)) });
	Draw(HUD, *Title,
		*TRACE_TEXT("LOADOUT.LIBRARY_FOOTER", "CLICK OR ARROWS TO CHOOSE     ENTER TO SAVE     ESC TO GO BACK"));
	return true;
}

// =================================================================================================
// Drawing
// =================================================================================================

void FTraceLoadoutSelect::Draw(AHUD* HUD, const TCHAR* Title, const TCHAR* FooterHint)
{
	using namespace TraceLoadoutLayout;

	if (HUD == nullptr || ViewW <= 0.f || ViewH <= 0.f)
	{
		return;
	}

	const float S = UIScale;
	const float X = Margin * S;
	const float W = ViewW - Margin * 2.f * S;

	// A full-bleed scrim, so the match behind cannot be read as part of the page.
	HUD->DrawRect(FLinearColor(0.02f, 0.03f, 0.07f, 0.88f), 0.f, 0.f, ViewW, ViewH);

	TraceCanvasText::DrawBold(HUD, Title, X, HeaderTop * S, TitleSize * S, Ink);
	HUD->DrawRect(Cyan, X, RuleY * S, W, 2.f * S);

	DrawTabs(HUD, X, TabY * S, W);

	// ---- the grid -------------------------------------------------------------------------------
	const float GridY = GridTop * S;
	const float GridH = (SavedY - 24.f) * S - GridY;
	const float TileW = (W - TileGapX * S * (Columns - 1)) / static_cast<float>(Columns);
	const float TileH = (GridH - TileGapY * S * (Rows - 1)) / static_cast<float>(Rows);

	for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
	{
		CardRects[Index] = FBox2D(ForceInit);
	}

	const int32 Count = FMath::Min(TraceLoadoutSelectFile::KitCount(), Columns * Rows);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 Col = Index % Columns;
		const int32 Row = Index / Columns;
		DrawCard(HUD, Index,
			X + (TileW + TileGapX * S) * Col,
			GridY + (TileH + TileGapY * S) * Row,
			TileW, TileH);
	}

	DrawSavedRow(HUD, X, SavedY * S, W);

	// ---- footer: the hint, or whatever just happened ---------------------------------------------
	const bool bHasMessage = !LastMessage.IsEmpty() && (Now - LastMessageTime) < 4.f;
	TraceCanvasText::DrawCentered(HUD,
		bHasMessage ? LastMessage : FString(FooterHint),
		ViewW * 0.5f, FooterY * S, SizeFooter * S, bHasMessage ? Good : InkDim);

	DrawPointer(HUD);
}

void FTraceLoadoutSelect::DrawTabs(AHUD* HUD, float X, float Y, float W)
{
	using namespace TraceLoadoutLayout;

	const float S = UIScale;
	const int32 Count = static_cast<int32>(ETraceLoadoutSlot::Count);
	const float TabW = (W - TabGap * S * (Count - 1)) / static_cast<float>(Count);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Index);
		const float TabX = X + (TabW + TabGap * S) * Index;
		const bool bActive = (Index == Tab);
		const bool bHovered = (Index == HoveredTab);

		TabRects[Index] = FBox2D(FVector2D(TabX, Y), FVector2D(TabX + TabW, Y + TabH * S));

		HUD->DrawRect(bActive ? PlateHi : Plate, TabX, Y, TabW, TabH * S);
		TraceLoadoutSelectFile::StrokeRect(HUD, TabX, Y, TabW, TabH * S, (bActive ? 2.f : 1.f) * S,
			bActive ? Cyan : (bHovered ? InkSoft : InkDim));

		TraceCanvasText::DrawBold(HUD, TraceLoadoutSelectFile::SlotHeading(Slot), TabX + CardPad * S, Y + 10.f * S,
			SizeTab * S, bActive ? Cyan : InkSoft);

		// WHAT IS IN THE SLOT, on the tab itself. The point of three tabs is that you can see your
		// whole loadout without visiting all three, so a tab that only said its own name would make
		// the player click through to check what they had already chosen.
		TraceCanvasText::Draw(HUD, TraceLoadoutSelectFile::TabSummary(Staged, Slot), TabX + CardPad * S, Y + 36.f * S,
			SizeTabSub * S, bActive ? Ink : InkDim);
	}
}

void FTraceLoadoutSelect::DrawCard(AHUD* HUD, int32 Index, float X, float Y, float W, float H)
{
	using namespace TraceLoadoutLayout;

	const float S = UIScale;
	const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Tab);
	const ETraceCharacterId Id = TraceLoadoutSelectFile::KitAtCard(Index);

	const bool bHighlighted = (Index == Highlighted[Tab]);
	const bool bEquipped = (Staged.Get(Slot) == Id);

	CardRects[Index] = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));

	HUD->DrawRect(bHighlighted ? PlateHi : Plate, X, Y, W, H);
	TraceLoadoutSelectFile::StrokeRect(HUD, X, Y, W, H, (bEquipped ? 3.f : (bHighlighted ? 2.f : 1.f)) * S,
		bEquipped ? Good : (bHighlighted ? Cyan : InkDim));

	float TextY = Y + CardPad * S;
	const float TextX = X + CardPad * S;
	const float TextW = W - CardPad * 2.f * S;

	// THE NAME, WHERE THERE IS ONE. Only the activated ability has one; movement and passive never
	// did, and inventing one for them was the mistake this screen was rebuilt to undo. Their card is
	// its description, which is exactly what the character screen showed for them.
	const FString Name = TraceAbilityNames::Get(Id, Slot);
	if (!Name.IsEmpty())
	{
		TraceCanvasText::DrawBold(HUD, Name, TextX, TextY, SizeName * S,
			bEquipped ? Good : (bHighlighted ? Ink : InkSoft));
		TextY += (SizeName + 8.f) * S;
	}

	const FString Body = TraceAbilityNames::Describe(Id, Slot);
	TraceLoadoutSelectFile::DrawWrapped(HUD, Body, TextX, TextY, TextW, SizeBody * S,
		bHighlighted ? InkSoft : InkDim, 4.f * S);

	// The equipped marker is a word, not only a colour: a green border alone is a legend the player
	// has to learn, and one of these cards is the answer to the question the tab is asking.
	if (bEquipped)
	{
		TraceCanvasText::DrawBold(HUD, TRACE_TEXT("LOADOUT.EQUIPPED", "EQUIPPED"),
			TextX, Y + H - (SizeBody + CardPad) * S, SizeBody * S, Good);
	}

	// Deliberately nothing else. NO CHARACTER NAME, no portrait, no accent taken from a character:
	// the abilities are freestanding and this card is the ability, not whoever used to own it.
}

void FTraceLoadoutSelect::DrawSavedRow(AHUD* HUD, float X, float Y, float W)
{
	using namespace TraceLoadoutLayout;

	const float S = UIScale;
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();

	TraceCanvasText::Draw(HUD, TRACE_TEXT("LOADOUT.SAVED_LABEL", "SAVED"),
		X, Y + 12.f * S, SizeSaved * S, InkDim);

	const float FirstX = X + 90.f * S;
	const float SlotW = 118.f * S;
	const float SlotGap = 10.f * S;

	for (int32 Index = 0; Index < UTraceUserSettings::SavedLoadoutCount; ++Index)
	{
		const float SlotX = FirstX + (SlotW + SlotGap) * Index;
		const bool bFilled = !Settings.GetSavedLoadout(Index).IsEmpty();
		const bool bHovered = (Index == HoveredSaved);

		SavedRects[Index] = FBox2D(FVector2D(SlotX, Y), FVector2D(SlotX + SlotW, Y + SavedH * S));

		HUD->DrawRect(Plate, SlotX, Y, SlotW, SavedH * S);
		TraceLoadoutSelectFile::StrokeRect(HUD, SlotX, Y, SlotW, SavedH * S, 1.f * S,
			bHovered ? Cyan : (bFilled ? InkSoft : InkDim));

		FString Label = Settings.GetSavedLoadoutName(Index);
		if (Label.IsEmpty())
		{
			Label = FString::FromInt(Index + 1);
		}
		TraceCanvasText::DrawCentered(HUD, Label, SlotX + SlotW * 0.5f, Y + 14.f * S,
			SizeSaved * S, bFilled ? Ink : InkDim);
	}

	// ---- LOCK IN ---------------------------------------------------------------------------------
	const float ConfirmW = 220.f * S;
	const float ConfirmX = X + W - ConfirmW;
	ConfirmRect = FBox2D(FVector2D(ConfirmX, Y), FVector2D(ConfirmX + ConfirmW, Y + SavedH * S));

	HUD->DrawRect(bHoveredConfirm ? PlateHi : Plate, ConfirmX, Y, ConfirmW, SavedH * S);
	TraceLoadoutSelectFile::StrokeRect(HUD, ConfirmX, Y, ConfirmW, SavedH * S, 2.f * S, bHoveredConfirm ? Cyan : Good);
	TraceCanvasText::DrawBold(HUD,
		(LibrarySlot != INDEX_NONE) ? TRACE_TEXT("LOADOUT.SAVE_SLOT", "SAVE")
		                            : TRACE_TEXT("LOADOUT.LOCK_IN", "LOCK IN"),
		ConfirmX + 24.f * S, Y + 13.f * S, SizeTab * S, bHoveredConfirm ? Cyan : Good);
}

void FTraceLoadoutSelect::DrawPointer(AHUD* HUD)
{
	using namespace TraceLoadoutLayout;

	// The OS cursor does not appear in captured frames and is hidden during a match, so the overlay
	// draws its own — the same reason and the same shape as the character screen's.
	if (!bHasCursor)
	{
		return;
	}

	const float S = UIScale;
	const float Size = 12.f * S;
	HUD->DrawRect(Ink, CursorPos.X, CursorPos.Y, 2.f * S, Size);
	HUD->DrawRect(Ink, CursorPos.X, CursorPos.Y, Size, 2.f * S);
}

// =================================================================================================
// Test seams
// =================================================================================================

void FTraceLoadoutSelect::DebugPick(ETraceLoadoutSlot Slot, ETraceCharacterId Id)
{
	Staged.Set(Slot, Id);
	Highlighted[static_cast<int32>(Slot)] = TraceLoadoutSelectFile::CardForKit(Id);
}

void FTraceLoadoutSelect::DebugConfirm(ATracePlayerState* LocalState)
{
	Confirm(LocalState);
}

void FTraceLoadoutSelect::DebugRecall(int32 Index)
{
	Recall(Index);
}

void FTraceLoadoutSelect::DebugStore(int32 Index)
{
	Store(Index);
}

FBox2D FTraceLoadoutSelect::DebugCardRect(int32 Index) const
{
	return (Index >= 0 && Index < TraceCharacterRoster::Count) ? CardRects[Index] : FBox2D(ForceInit);
}

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Loadout.Screen — does a press on the page change what you are playing?
//
// Drives the SCREEN's own entry points rather than reaching past them to the component, so a break
// anywhere in the chain — staging, the RPC, the legality check, the lock — fails this.
// =================================================================================================
namespace TraceLoadoutScreenVerify
{
	void Run()
	{
		UWorld* World = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->GetAuthGameMode() != nullptr)
				{
					World = Context.World();
					break;
				}
			}
		}
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[LoadoutScreen] no authoritative world — run this in a match."));
			return;
		}

		ATracePlayerState* Subject = nullptr;
		UTraceAbilityComponent* Comp = nullptr;
		if (const AGameStateBase* GS = World->GetGameState())
		{
			for (APlayerState* Each : GS->PlayerArray)
			{
				if (ATracePlayerState* Candidate = Cast<ATracePlayerState>(Each))
				{
					if (UTraceAbilityComponent* Found = Candidate->FindComponentByClass<UTraceAbilityComponent>())
					{
						Subject = Candidate;
						Comp = Found;
						break;
					}
				}
			}
		}
		if (Comp == nullptr || Subject == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[LoadoutScreen] no player state with an ability component."));
			return;
		}

		int32 Failures = 0;
		auto Check = [&Failures](const TCHAR* Label, bool bPass, const FString& Detail)
		{
			Failures += bPass ? 0 : 1;
			UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen]   %-4s %-52s %s"),
				bPass ? TEXT("ok") : TEXT("FAIL"), Label, *Detail);
		};

		const FTraceLoadout Restore = Comp->GetLoadout();
		const bool bRestoreSelect = Subject->IsCharacterSelectOpen();

		UE_LOG(LogTraceGame, Display,
			TEXT("[LoadoutScreen] ===== does a press on the page change what you play? ====="));

		Check(TEXT("the loadout page is the armed one"), TraceLoadoutSelect::IsArmed(),
			TEXT("Trace.UI.LoadoutScreen"));

		FTraceLoadoutSelect Screen;
		Screen.DebugPick(ETraceLoadoutSlot::Movement,  ETraceCharacterId::Chut);
		Screen.DebugPick(ETraceLoadoutSlot::Passive,   ETraceCharacterId::Mace);
		Screen.DebugPick(ETraceLoadoutSlot::Activated, ETraceCharacterId::Elle);

		FTraceLoadout Wanted;
		Wanted.Movement  = ETraceCharacterId::Chut;
		Wanted.Passive   = ETraceCharacterId::Mace;
		Wanted.Activated = ETraceCharacterId::Elle;

		Check(TEXT("three picks stage three different kits"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
		Comp->ApplyLoadout(Restore);
		Screen.DebugConfirm(Subject);
		Check(TEXT("LOCK IN during play changes nothing"),
			Comp->GetLoadout() == Restore, TraceLoadoutToString(Comp->GetLoadout()));

		Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
		Screen.DebugConfirm(Subject);
		Check(TEXT("LOCK IN at the screen equips exactly what was staged"),
			Comp->GetLoadout() == Wanted, TraceLoadoutToString(Comp->GetLoadout()));

		bool bAllSlotsBuilt = true;
		for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
		{
			bAllSlotsBuilt = bAllSlotsBuilt
				&& (Comp->GetAbilitySetForSlot(static_cast<ETraceLoadoutSlot>(Index)) != nullptr);
		}
		Check(TEXT("all three slots hold a live kit afterwards"), bAllSlotsBuilt,
			TEXT("a loadout that applied but built nothing would be worse than a refusal"));

		// *** AND THE SCREEN CLOSES. *** The select window is the only thing holding this page up, so
		// a LOCK IN that applies the loadout and leaves the window open is a page that never goes
		// away — the match running behind a menu the player cannot dismiss. That shipped, and it made
		// the build unplayable. The assertion is one line and it is the difference between "the
		// loadout was applied" and "the player can now play".
		Check(TEXT("LOCK IN closes the screen"),
			!Subject->IsCharacterSelectOpen(),
			TEXT("the select window is the ONLY condition that keeps this page up"));

		// ---- THE NAMES ARE THE ORIGINALS, AND NOTHING INVENTS ONE ------------------------------
		//
		// The reverted state, asserted rather than eyeballed: exactly the ten roster ActivatedNames
		// exist, and movement and passive have no name at all. An earlier pass invented thirty and
		// renamed three real ones, which is the regression this guards.
		int32 NameProblems = 0;
		for (uint8 Id = TraceCharacterRoster::FirstId; Id <= TraceCharacterRoster::LastId; ++Id)
		{
			const ETraceCharacterId Kit = static_cast<ETraceCharacterId>(Id);

			if (!TraceAbilityNames::Get(Kit, ETraceLoadoutSlot::Movement).IsEmpty()
				|| !TraceAbilityNames::Get(Kit, ETraceLoadoutSlot::Passive).IsEmpty())
			{
				++NameProblems;
				UE_LOG(LogTraceGame, Error,
					TEXT("[LoadoutScreen]   kit %d has an invented movement/passive name"), Id);
			}

			const FString Activated = TraceAbilityNames::Get(Kit, ETraceLoadoutSlot::Activated);
			const TraceCharacterRoster::FTraceCharacterEntry* Entry = TraceCharacterRoster::Find(Id);
			if (Entry == nullptr || Activated != FString(Entry->ActivatedName))
			{
				++NameProblems;
				UE_LOG(LogTraceGame, Error,
					TEXT("[LoadoutScreen]   kit %d activated name '%s' is not the roster's '%s'"),
					Id, *Activated, (Entry != nullptr) ? Entry->ActivatedName : TEXT("<none>"));
			}

			// Every ability must still have SOMETHING to draw, or a card would be blank.
			for (int32 SlotIndex = 0; SlotIndex < static_cast<int32>(ETraceLoadoutSlot::Count); ++SlotIndex)
			{
				const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(SlotIndex);
				if (TraceAbilityNames::Describe(Kit, Slot).IsEmpty()
					|| TraceAbilityNames::ShortLabel(Kit, Slot).IsEmpty())
				{
					++NameProblems;
					UE_LOG(LogTraceGame, Error,
						TEXT("[LoadoutScreen]   kit %d slot %s has nothing to draw"),
						Id, TraceLoadoutSlotToString(Slot));
				}
			}
		}
		Failures += NameProblems;
		Check(TEXT("names are the roster's, and nothing is invented"), NameProblems == 0,
			FString::Printf(TEXT("%d problem(s) across 10 kits"), NameProblems));

		Comp->ApplyLoadout(Restore);
		Subject->ServerSetCharacterSelectOpen(bRestoreSelect, 0.f);

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadoutScreen] ===== PASS — the screen stages, the server decides, and every "
				     "ability is named the way it always was. ====="));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[LoadoutScreen] ===== *** FAIL *** %d check(s) — see above ====="), Failures);
		}
	}

	FAutoConsoleCommand Cmd(
		TEXT("Trace.Loadout.Screen"),
		TEXT("Drive the loadout screen end to end and prove the pick reaches the server, and that the "
		     "ability names are the roster's originals."),
		FConsoleCommandDelegate::CreateStatic(&Run));
}

// =================================================================================================
// Trace.Loadout.Library — five saved loadouts, and bots that stay uniform.
// =================================================================================================
namespace TraceLoadoutLibraryVerify
{
	void Run()
	{
		int32 Failures = 0;
		auto Check = [&Failures](const TCHAR* Label, bool bPass, const FString& Detail)
		{
			Failures += bPass ? 0 : 1;
			UE_LOG(LogTraceGame, Display, TEXT("[LoadoutLibrary]   %-4s %-50s %s"),
				bPass ? TEXT("ok") : TEXT("FAIL"), Label, *Detail);
		};

		UE_LOG(LogTraceGame, Display,
			TEXT("[LoadoutLibrary] ===== five saved loadouts, and bots that stay uniform ====="));

		UTraceUserSettings& Settings = UTraceUserSettings::Get();

		TArray<FTraceLoadout> Restore;
		for (int32 Index = 0; Index < UTraceUserSettings::SavedLoadoutCount; ++Index)
		{
			Restore.Add(Settings.GetSavedLoadout(Index));
		}

		FTraceLoadout Wanted;
		Wanted.Movement  = ETraceCharacterId::Chut;
		Wanted.Passive   = ETraceCharacterId::Mace;
		Wanted.Activated = ETraceCharacterId::Elle;

		// SLOT 4, NOT SLOT 0: writing the last-but-one slot into an empty array is the case that would
		// silently land at index 0 and become slot 1. The array has to GROW, with the gaps empty.
		Settings.SetSavedLoadout(3, Wanted);
		Check(TEXT("slot 4 reads back exactly what was written"),
			Settings.GetSavedLoadout(3) == Wanted, TraceLoadoutToString(Settings.GetSavedLoadout(3)));
		Check(TEXT("slot 3 is still empty, not the one we wrote"),
			Settings.GetSavedLoadout(2).IsEmpty(), TraceLoadoutToString(Settings.GetSavedLoadout(2)));
		Check(TEXT("an index past the end reads empty, not garbage"),
			Settings.GetSavedLoadout(99).IsEmpty(), TEXT("out of range is 'empty', never a bounds bug"));

		FTraceLoadoutSelect Screen;
		Screen.DebugPick(ETraceLoadoutSlot::Movement,  ETraceCharacterId::Rocco);
		Screen.DebugPick(ETraceLoadoutSlot::Passive,   ETraceCharacterId::Rocco);
		Screen.DebugPick(ETraceLoadoutSlot::Activated, ETraceCharacterId::Rocco);

		Screen.DebugRecall(3);
		Check(TEXT("recalling slot 4 loads it into the page"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		Screen.DebugRecall(1);
		Check(TEXT("recalling an EMPTY slot changes nothing"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		Screen.DebugPick(ETraceLoadoutSlot::Movement, ETraceCharacterId::Lily);
		Screen.DebugStore(0);
		Check(TEXT("storing writes the staged loadout to slot 1"),
			Settings.GetSavedLoadout(0) == Screen.GetStaged(),
			TraceLoadoutToString(Settings.GetSavedLoadout(0)));

		Screen.OpenLibrary(2);
		Check(TEXT("library mode opens on the slot it was given"),
			Screen.GetLibrarySlot() == 2, FString::FromInt(Screen.GetLibrarySlot()));
		Screen.CloseLibrary();
		Check(TEXT("and closes"), !Screen.IsLibraryOpen(), TEXT(""));

		int32 Bots = 0;
		int32 MixedBots = 0;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* World = Context.World();
				if (World == nullptr || World->GetAuthGameMode() == nullptr)
				{
					continue;
				}
				if (const AGameStateBase* GS = World->GetGameState())
				{
					for (APlayerState* Each : GS->PlayerArray)
					{
						if (Each == nullptr || !Each->IsABot())
						{
							continue;
						}
						if (const UTraceAbilityComponent* Comp = Each->FindComponentByClass<UTraceAbilityComponent>())
						{
							++Bots;
							const FTraceLoadout BotLoadout = Comp->GetLoadout();
							if (!BotLoadout.IsUniform() && !BotLoadout.IsEmpty())
							{
								++MixedBots;
								UE_LOG(LogTraceGame, Error,
									TEXT("[LoadoutLibrary]   bot %s has a MIXED loadout %s"),
									*GetNameSafe(Each), *TraceLoadoutToString(BotLoadout));
							}
						}
					}
				}
			}
		}
		Failures += MixedBots;
		Check(TEXT("every bot is uniform (or characterless)"), MixedBots == 0,
			FString::Printf(TEXT("%d bot(s) checked, %d mixed"), Bots, MixedBots));

		for (int32 Index = 0; Index < Restore.Num(); ++Index)
		{
			Settings.SetSavedLoadout(Index, Restore[Index]);
		}

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadoutLibrary] ===== PASS — five slots that persist, and bots that stayed "
				     "uniform. ====="));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[LoadoutLibrary] ===== *** FAIL *** %d check(s) — see above ====="), Failures);
		}
	}

	FAutoConsoleCommand Cmd(
		TEXT("Trace.Loadout.Library"),
		TEXT("Prove the five saved loadouts round-trip through the settings file and that no bot is "
		     "running a mixed loadout."),
		FConsoleCommandDelegate::CreateStatic(&Run));
}
#endif   // !UE_BUILD_SHIPPING
