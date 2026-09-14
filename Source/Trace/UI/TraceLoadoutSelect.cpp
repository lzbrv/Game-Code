#include "UI/TraceLoadoutSelect.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/PlayerController.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TracePlayerState.h"
#include "Settings/TraceGamepadInput.h"
#include "Settings/TraceUserSettings.h"
#include "Trace.h"
#include "UI/TraceAbilityNames.h"
#include "UI/Text/TraceCanvasText.h"
#include "UI/Text/TraceGameText.h"

namespace
{
	int32 GTraceLoadoutScreenArmed = 1;
	FAutoConsoleVariableRef CVarLoadoutScreen(
		TEXT("Trace.UI.LoadoutScreen"),
		GTraceLoadoutScreenArmed,
		TEXT("1 (default): after team select, show the LOADOUT screen - three columns, one pick each. ")
		TEXT("0: show the old ten-card CHARACTER page instead. The two are mutually exclusive and the ")
		TEXT("character page is still whole behind this, so a playtest that dislikes the loadout screen ")
		TEXT("can go back without a build."),
		ECVF_Default);
}

namespace TraceLoadoutSelect
{
	bool IsArmed()
	{
		return GTraceLoadoutScreenArmed != 0;
	}
}

namespace TraceLoadoutSelectLayout
{
	// Proportions, not pixels — every number here is a fraction of the viewport, so the screen is the
	// same screen at 1280x720 and at 4K. Same rule the character screen follows.
	constexpr float TopY          = 0.16f;
	constexpr float ColumnTop     = 0.26f;
	constexpr float ColumnBottom  = 0.80f;
	constexpr float SideMargin    = 0.08f;
	constexpr float ColumnGap     = 0.02f;

	constexpr float SizeTitle     = 44.f;
	constexpr float SizeHeading   = 26.f;
	constexpr float SizeRow       = 22.f;
	constexpr float SizeBody      = 17.f;
	constexpr float SizeFooter    = 18.f;

	const FLinearColor Dim      (0.55f, 0.60f, 0.66f, 1.f);
	const FLinearColor Bright   (0.94f, 0.97f, 1.00f, 1.f);
	const FLinearColor Accent   (0.35f, 0.92f, 1.00f, 1.f);
	const FLinearColor Equipped (0.55f, 1.00f, 0.62f, 1.f);
	const FLinearColor Refused  (1.00f, 0.45f, 0.42f, 1.f);
}

namespace
{
	/** The ten pickable kits, in roster order. Index 0 is the first real kit, never None. */
	int32 KitCount()
	{
		return static_cast<int32>(TraceCharacterRoster::LastId) - static_cast<int32>(TraceCharacterRoster::FirstId) + 1;
	}

	ETraceCharacterId KitAtRow(int32 RowIndex)
	{
		const int32 Count = KitCount();
		if (Count <= 0)
		{
			return ETraceCharacterId::None;
		}
		const int32 Wrapped = ((RowIndex % Count) + Count) % Count;
		return static_cast<ETraceCharacterId>(TraceCharacterRoster::FirstId + Wrapped);
	}

	int32 RowForKit(ETraceCharacterId Id)
	{
		const int32 Row = static_cast<int32>(Id) - static_cast<int32>(TraceCharacterRoster::FirstId);
		return (Row >= 0 && Row < KitCount()) ? Row : 0;
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
}

// =================================================================================================

void FTraceLoadoutSelect::Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
	float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed)
{
	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;
	Now = InNow;

	// THE SERVER DECIDES WHETHER THIS IS UP. Same single condition the character screen used, which
	// is what makes "the half time break shows this" true without a client-side clock that could
	// disagree with the whistle by a frame.
	const bool bWantOpen = (LocalState != nullptr) && LocalState->IsCharacterSelectOpen();

	if (bWantOpen && !bOpen)
	{
		// Opening. Seed from what the player actually has, so half time is an EDIT of your loadout
		// rather than a blank page you must refill under a 45 second clock.
		bStagedSeeded = false;
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

	if (bInputAllowed && PC != nullptr)
	{
		PollInput(PC, LocalState);
	}

	Draw(HUD, LocalState);
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

	// A player who has never picked gets a legal starting point rather than three empty slots: an
	// empty loadout is legal but it is nobody's intent, and the 45 second clock can run out.
	if (Staged.IsEmpty())
	{
		Staged = FTraceLoadout::Uniform(KitAtRow(0));
	}

	for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
	{
		Row[Index] = RowForKit(Staged.Get(static_cast<ETraceLoadoutSlot>(Index)));
	}
	Column = 0;
	LastRefusal.Reset();
}

// =================================================================================================
// Input
// =================================================================================================

void FTraceLoadoutSelect::PollInput(APlayerController* PC, ATracePlayerState* LocalState)
{
	// Arrow keys and WASD, plus the pad — the same pairing and the same repeat clock as the character
	// screen, so a thumb and a finger walk the grid at one speed.
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

		// X BEFORE Y, and only one per step. A diagonal on a stick would otherwise change the question
		// AND the answer in the same frame, which reads as the cursor jumping.
		if (NavX != 0)
		{
			MoveColumn(NavX);
		}
		else
		{
			MoveRow(NavY);
		}
	}

	// ---- THE FIVE SAVED LOADOUTS -----------------------------------------------------------
	//
	// A number key RECALLS slot N into the columns; SHIFT plus that number STORES what is staged
	// into it. Recall deliberately does NOT send: it fills the screen in, and the player confirms it
	// like anything else. That is what keeps the library from being a way around the lock — it is a
	// faster way to type, never a second door into ServerSetLoadout.
	//
	// Edge-triggered per key for the same reason ENTER is: a held 3 must recall once.
	static const FKey NumberKeys[5] =
	{
		EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five
	};
	const bool bShift = PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift);

	for (int32 Index = 0; Index < 5; ++Index)
	{
		const bool bDown = PC->IsInputKeyDown(NumberKeys[Index]);
		if (bDown && !bNumberWasDown[Index])
		{
			if (bShift)
			{
				Store(Index);
			}
			else
			{
				Recall(Index);
			}
		}
		bNumberWasDown[Index] = bDown;
	}

	// CONFIRM IS EDGE-TRIGGERED. Held ENTER must send one request, not one per frame: this screen is
	// up for 45 seconds and the server would see 2700 of them.
	const bool bConfirmDown = PC->IsInputKeyDown(EKeys::Enter)
		|| PC->IsInputKeyDown(EKeys::SpaceBar)
		|| TracePadMenu::ConfirmPressed(PC);

	if (bConfirmDown && !bConfirmWasDown)
	{
		Confirm(LocalState);
	}
	bConfirmWasDown = bConfirmDown;
}

void FTraceLoadoutSelect::MoveColumn(int32 Delta)
{
	const int32 Count = static_cast<int32>(ETraceLoadoutSlot::Count);
	Column = ((Column + Delta) % Count + Count) % Count;
}

void FTraceLoadoutSelect::MoveRow(int32 Delta)
{
	const int32 Count = KitCount();
	if (Count <= 0)
	{
		return;
	}
	Row[Column] = ((Row[Column] + Delta) % Count + Count) % Count;

	// STAGING IS IMMEDIATE, SENDING IS NOT. Moving the cursor changes what the screen shows equipped,
	// so the three columns always describe one coherent loadout the player can read; the server only
	// hears about it on confirm. That keeps a 45 second browse to ONE request.
	Staged.Set(static_cast<ETraceLoadoutSlot>(Column), KitAtRow(Row[Column]));
	LastRefusal.Reset();
}

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

	// ASKED LOCALLY FIRST so the common refusal needs no round trip — and then SENT ANYWAY, because
	// this screen is not the authority. If the server disagrees it wins and the player is told.
	FString Reason;
	if (!UTraceAbilityComponent::IsLoadoutLegal(Staged, &Reason))
	{
		LastRefusal = Reason;
		LastRefusalTime = Now;
		return;
	}

	Comp->ServerRequestSetLoadout(Staged);

	UE_LOG(LogTraceGame, Log, TEXT("[LoadoutScreen] sent %s"), *TraceLoadoutToString(Staged));
}

void FTraceLoadoutSelect::OpenLibrary(int32 SlotIndex)
{
	LibrarySlot = FMath::Clamp(SlotIndex, 0, UTraceUserSettings::SavedLoadoutCount - 1);

	// SEEDED FROM THE SLOT, not from what you are playing. This page is about the library.
	Staged = UTraceUserSettings::Get().GetSavedLoadout(LibrarySlot);
	if (Staged.IsEmpty())
	{
		// An empty slot opens on a legal starting point rather than three blanks, so the first thing
		// the player sees is a loadout they could save, not a puzzle.
		Staged = FTraceLoadout::Uniform(KitAtRow(0));
	}

	for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
	{
		Row[Index] = RowForKit(Staged.Get(static_cast<ETraceLoadoutSlot>(Index)));
	}
	Column = 0;
	LastRefusal.Reset();
	LastSavedSlotTouched = INDEX_NONE;
	LastSavedSlotVerb.Reset();

	// Swallow the press that opened this page, so it does not immediately confirm on the same frame.
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

	if (bInputAllowed && PC != nullptr)
	{
		// Navigation only — the number keys are deliberately NOT read here. In library mode the slot
		// is chosen by the page that opened this, and a 3 that jumped to another slot mid-edit would
		// throw away work the player had not saved yet.
		const int32 KeyX = (PC->IsInputKeyDown(EKeys::Right) || PC->IsInputKeyDown(EKeys::D) ? 1 : 0)
		                 - (PC->IsInputKeyDown(EKeys::Left)  || PC->IsInputKeyDown(EKeys::A) ? 1 : 0);
		const int32 KeyY = (PC->IsInputKeyDown(EKeys::Down)  || PC->IsInputKeyDown(EKeys::S) ? 1 : 0)
		                 - (PC->IsInputKeyDown(EKeys::Up)    || PC->IsInputKeyDown(EKeys::W) ? 1 : 0);

		const int32 NavX = (KeyX != 0) ? KeyX : TracePadMenu::NavX(PC);
		const int32 NavY = (KeyY != 0) ? KeyY : TracePadMenu::NavY(PC);

		if (NavX == 0 && NavY == 0)
		{
			NextNavTime = 0.f;
		}
		else if (Now >= NextNavTime)
		{
			NextNavTime = Now + ((NextNavTime <= 0.f) ? NavRepeatDelay : NavRepeatInterval);
			if (NavX != 0) { MoveColumn(NavX); } else { MoveRow(NavY); }
		}

		const bool bConfirmDown = PC->IsInputKeyDown(EKeys::Enter)
			|| PC->IsInputKeyDown(EKeys::SpaceBar)
			|| TracePadMenu::ConfirmPressed(PC);
		if (bConfirmDown && !bConfirmWasDown)
		{
			// ENTER SAVES AND LEAVES. There is no server in this conversation.
			Store(LibrarySlot);
			CloseLibrary();
			bConfirmWasDown = true;
			return false;
		}
		bConfirmWasDown = bConfirmDown;

		const bool bCancelDown = PC->IsInputKeyDown(EKeys::Escape) || PC->IsInputKeyDown(EKeys::BackSpace);
		if (bCancelDown && !bCancelWasDown)
		{
			// ESCAPE LEAVES WITHOUT SAVING, which is the whole reason a library editor needs a cancel:
			// the slot the player was browsing from must still be there when they change their mind.
			CloseLibrary();
			bCancelWasDown = true;
			return false;
		}
		bCancelWasDown = bCancelDown;
	}

	DrawLibrary(HUD);
	return true;
}

void FTraceLoadoutSelect::Recall(int32 Index)
{
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();
	const FTraceLoadout Saved = Settings.GetSavedLoadout(Index);

	// AN EMPTY SLOT IS NOT A LOADOUT. Recalling one would silently wipe the columns the player just
	// filled in, which is the most expensive possible misreading of a keypress on a 45 second clock.
	if (Saved.IsEmpty())
	{
		LastSavedSlotTouched = Index;
		LastSavedSlotVerb = TRACE_TEXT("LOADOUT.SLOT_EMPTY", "SLOT {0} IS EMPTY");
		return;
	}

	Staged = Saved;
	for (int32 SlotIndex = 0; SlotIndex < static_cast<int32>(ETraceLoadoutSlot::Count); ++SlotIndex)
	{
		Row[SlotIndex] = RowForKit(Staged.Get(static_cast<ETraceLoadoutSlot>(SlotIndex)));
	}

	LastSavedSlotTouched = Index;
	LastSavedSlotVerb = TRACE_TEXT("LOADOUT.SLOT_LOADED", "LOADED {0}");
	LastRefusal.Reset();
}

void FTraceLoadoutSelect::Store(int32 Index)
{
	UTraceUserSettings::Get().SetSavedLoadout(Index, Staged);

	LastSavedSlotTouched = Index;
	LastSavedSlotVerb = TRACE_TEXT("LOADOUT.SLOT_SAVED", "SAVED TO {0}");

	UE_LOG(LogTraceGame, Log, TEXT("[LoadoutScreen] saved %s to slot %d"),
		*TraceLoadoutToString(Staged), Index + 1);
}

void FTraceLoadoutSelect::DebugRecall(int32 Index)
{
	Recall(Index);
}

void FTraceLoadoutSelect::DebugStore(int32 Index)
{
	Store(Index);
}

void FTraceLoadoutSelect::DebugPick(ETraceLoadoutSlot Slot, ETraceCharacterId Id)
{
	Staged.Set(Slot, Id);
	Row[static_cast<int32>(Slot)] = RowForKit(Id);
}

void FTraceLoadoutSelect::DebugConfirm(ATracePlayerState* LocalState)
{
	Confirm(LocalState);
}

// =================================================================================================
// Drawing
// =================================================================================================

void FTraceLoadoutSelect::Draw(AHUD* HUD, ATracePlayerState* LocalState)
{
	using namespace TraceLoadoutSelectLayout;

	if (HUD == nullptr || ViewW <= 0.f || ViewH <= 0.f)
	{
		return;
	}

	const float S = UIScale;

	TraceCanvasText::DrawCentered(HUD, TRACE_TEXT("LOADOUT.TITLE", "BUILD YOUR LOADOUT"),
		ViewW * 0.5f, ViewH * TopY, SizeTitle * S, Bright);

	const float ColumnsX = ViewW * SideMargin;
	const float ColumnsW = ViewW * (1.f - SideMargin * 2.f);
	const int32 ColumnCount = static_cast<int32>(ETraceLoadoutSlot::Count);
	const float GapW = ViewW * ColumnGap;
	const float EachW = (ColumnsW - GapW * (ColumnCount - 1)) / static_cast<float>(ColumnCount);

	const float ColumnY = ViewH * ColumnTop;
	const float ColumnH = ViewH * (ColumnBottom - ColumnTop);

	for (int32 Index = 0; Index < ColumnCount; ++Index)
	{
		DrawColumn(HUD, Index, ColumnsX + (EachW + GapW) * Index, ColumnY, EachW, ColumnH);
	}

	DrawFooter(HUD, ColumnsX, ViewH * (ColumnBottom + 0.04f), ColumnsW);
}

void FTraceLoadoutSelect::DrawColumn(AHUD* HUD, int32 ColumnIndex, float X, float Y, float W, float H)
{
	using namespace TraceLoadoutSelectLayout;

	const float S = UIScale;
	const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(ColumnIndex);
	const bool bActiveColumn = (ColumnIndex == Column);

	// The heading tells you which of the three questions this column asks, and brightens when the
	// cursor is in it — the only "where am I" cue a three column screen needs.
	TraceCanvasText::Draw(HUD, SlotHeading(Slot), X, Y,
		SizeHeading * S, bActiveColumn ? Accent : Dim);

	const float RowHeight = (SizeRow + 6.f) * S;
	float RowY = Y + (SizeHeading + 14.f) * S;

	const int32 Count = KitCount();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const ETraceCharacterId Id = KitAtRow(Index);
		const bool bHovered = bActiveColumn && (Index == Row[ColumnIndex]);
		const bool bEquipped = (Staged.Get(Slot) == Id);

		FLinearColor Color = Dim;
		if (bEquipped)
		{
			Color = Equipped;
		}
		if (bHovered)
		{
			Color = Bright;
		}

		const FString Label = TraceAbilityNames::Get(Id, Slot);

		// The cursor is a caret rather than a filled bar: three columns of highlight bars at once
		// would be three things claiming to be selected, and only one of them is.
		if (bHovered)
		{
			TraceCanvasText::Draw(HUD, TEXT(">"), X, RowY, SizeRow * S, Accent);
		}
		TraceCanvasText::DrawBold(HUD, Label, X + 18.f * S, RowY, SizeRow * S, Color);

		RowY += RowHeight;
	}

	// The description belongs to whatever the cursor is on IN THIS COLUMN, so all three read at once
	// and a player can compare a movement ability against the passive they already chose.
	const ETraceCharacterId Described = KitAtRow(Row[ColumnIndex]);
	const FString Body = TraceAbilityNames::Describe(Described, Slot);
	if (!Body.IsEmpty())
	{
		TraceCanvasText::Draw(HUD, Body, X, Y + H - (SizeBody * 4.f) * S, SizeBody * S,
			bActiveColumn ? Bright : Dim);
	}
}

void FTraceLoadoutSelect::DrawLibrary(AHUD* HUD)
{
	using namespace TraceLoadoutSelectLayout;

	if (HUD == nullptr || ViewW <= 0.f || ViewH <= 0.f)
	{
		return;
	}

	const float S = UIScale;

	// THE SLOT NUMBER IS THE TITLE. The player arrived here from a list of five and must never be
	// unsure which one they are about to overwrite.
	const FString Title = FString::Format(*TRACE_TEXT("LOADOUT.LIBRARY_TITLE", "LOADOUT {0}"),
		FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(LibrarySlot + 1)) });
	TraceCanvasText::DrawCentered(HUD, Title, ViewW * 0.5f, ViewH * TopY, SizeTitle * S, Bright);

	const float ColumnsX = ViewW * SideMargin;
	const float ColumnsW = ViewW * (1.f - SideMargin * 2.f);
	const int32 ColumnCount = static_cast<int32>(ETraceLoadoutSlot::Count);
	const float GapW = ViewW * ColumnGap;
	const float EachW = (ColumnsW - GapW * (ColumnCount - 1)) / static_cast<float>(ColumnCount);

	for (int32 Index = 0; Index < ColumnCount; ++Index)
	{
		DrawColumn(HUD, Index, ColumnsX + (EachW + GapW) * Index,
			ViewH * ColumnTop, EachW, ViewH * (ColumnBottom - ColumnTop));
	}

	const float FooterY = ViewH * (ColumnBottom + 0.04f);
	TraceCanvasText::DrawCentered(HUD,
		TRACE_TEXT("LOADOUT.LIBRARY_FOOTER", "ARROWS TO CHOOSE     ENTER TO SAVE     ESC TO GO BACK"),
		ViewW * 0.5f, FooterY, SizeFooter * S, Dim);

	TraceCanvasText::DrawCentered(HUD, TraceLoadoutToString(Staged),
		ViewW * 0.5f, FooterY + (SizeFooter + 8.f) * S, SizeBody * S, Equipped);
}

void FTraceLoadoutSelect::DrawFooter(AHUD* HUD, float X, float Y, float W)
{
	using namespace TraceLoadoutSelectLayout;

	const float S = UIScale;

	// A REFUSAL IS REPORTED, NEVER SWALLOWED. If the server said no, that sentence is what the player
	// needs, and it outranks the key prompt for a few seconds.
	if (!LastRefusal.IsEmpty() && (Now - LastRefusalTime) < 4.f)
	{
		TraceCanvasText::DrawCentered(HUD, LastRefusal.ToUpper(), X + W * 0.5f, Y, SizeFooter * S, Refused);
		return;
	}

	TraceCanvasText::DrawCentered(HUD,
		TRACE_TEXT("LOADOUT.FOOTER",
			"ARROWS OR STICK TO CHOOSE     1-5 LOAD A SAVED LOADOUT     SHIFT+1-5 SAVE     ENTER TO LOCK IN"),
		X + W * 0.5f, Y, SizeFooter * S, Dim);

	// The five slots, drawn as a row so the player can see which are filled without pressing anything.
	// A named slot shows its name; an unnamed but filled one shows its number in the equipped colour;
	// an empty one is dim. Three states, no legend needed.
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();
	const float SlotY = Y + (SizeFooter + 8.f) * S;
	const float SlotSpan = W * 0.5f;
	const float SlotX = X + W * 0.25f;
	for (int32 Index = 0; Index < UTraceUserSettings::SavedLoadoutCount; ++Index)
	{
		const bool bFilled = !Settings.GetSavedLoadout(Index).IsEmpty();
		FString Label = Settings.GetSavedLoadoutName(Index);
		if (Label.IsEmpty())
		{
			Label = FString::FromInt(Index + 1);
		}
		TraceCanvasText::DrawCentered(HUD, Label,
			SlotX + SlotSpan * (static_cast<float>(Index) / (UTraceUserSettings::SavedLoadoutCount - 1)),
			SlotY, SizeBody * S, bFilled ? Equipped : Dim);
	}

	// What the last number key did, if anything, above the staged line.
	if (LastSavedSlotTouched != INDEX_NONE && !LastSavedSlotVerb.IsEmpty())
	{
		const FString Said = FString::Format(*LastSavedSlotVerb,
			FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(LastSavedSlotTouched + 1)) });
		TraceCanvasText::DrawCentered(HUD, Said, X + W * 0.5f, SlotY + (SizeBody + 6.f) * S,
			SizeBody * S, Accent);
	}

	TraceCanvasText::DrawCentered(HUD, TraceLoadoutToString(Staged),
		X + W * 0.5f, Y + (SizeFooter + 8.f) * S + (SizeBody + 6.f) * 2.f * S, SizeBody * S, Equipped);
}

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Loadout.Screen — S5. Does the page actually change what you are playing?
//
// A screen is only real if a press on it reaches the server and comes back as a different set of
// abilities. Everything in between — three columns, a staged pick, an RPC, a legality check, the
// lock — is machinery, and machinery that has never been driven end to end is a guess.
//
// So this drives the SCREEN's own entry points rather than reaching past them to the component:
// DebugPick is what an arrow key does, DebugConfirm is what ENTER does. If the wiring between them
// and ServerSetLoadout is broken, this fails — which is the entire reason it does not just call
// ServerSetLoadout and declare victory.
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

		// Any player state with an ability component will do; on a listen server the local one is a
		// real client path and a bot's is not, but both exercise the same request -> apply chain.
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
			UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen]   %-4s %-46s %s"),
				bPass ? TEXT("ok") : TEXT("FAIL"), Label, *Detail);
		};

		const FTraceLoadout Restore = Comp->GetLoadout();
		const bool bRestoreSelect = Subject->IsCharacterSelectOpen();

		UE_LOG(LogTraceGame, Display,
			TEXT("[LoadoutScreen] ===== S5: does a press on the screen change what you play? ====="));

		Check(TEXT("the loadout page is the armed one"), TraceLoadoutSelect::IsArmed(),
			TEXT("Trace.UI.LoadoutScreen"));

		FTraceLoadoutSelect Screen;

		// A loadout that is legal and is NOT what the player already has, so "it worked" and "nothing
		// happened" cannot look the same.
		Screen.DebugPick(ETraceLoadoutSlot::Movement,  ETraceCharacterId::Chut);
		Screen.DebugPick(ETraceLoadoutSlot::Passive,   ETraceCharacterId::Mace);
		Screen.DebugPick(ETraceLoadoutSlot::Activated, ETraceCharacterId::Elle);

		FTraceLoadout Wanted;
		Wanted.Movement  = ETraceCharacterId::Chut;
		Wanted.Passive   = ETraceCharacterId::Mace;
		Wanted.Activated = ETraceCharacterId::Elle;

		Check(TEXT("three arrow keys stage three different kits"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		// ---- ENTER WHILE LOCKED: refused, and the player keeps what they had --------------------
		Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
		Comp->ApplyLoadout(Restore);
		Screen.DebugConfirm(Subject);
		Check(TEXT("ENTER during play changes nothing"),
			Comp->GetLoadout() == Restore, TraceLoadoutToString(Comp->GetLoadout()));

		// ---- ENTER WITH THE WINDOW OPEN: it lands ----------------------------------------------
		Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
		Screen.DebugConfirm(Subject);
		Check(TEXT("ENTER at the screen equips exactly what was staged"),
			Comp->GetLoadout() == Wanted, TraceLoadoutToString(Comp->GetLoadout()));

		// ---- and the kits behind it are really built --------------------------------------------
		bool bAllSlotsBuilt = true;
		for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
		{
			bAllSlotsBuilt = bAllSlotsBuilt
				&& (Comp->GetAbilitySetForSlot(static_cast<ETraceLoadoutSlot>(Index)) != nullptr);
		}
		Check(TEXT("all three slots hold a live kit afterwards"), bAllSlotsBuilt,
			TEXT("a loadout that applied but built nothing would be worse than a refusal"));

		// ---- every name the screen would print exists -------------------------------------------
		int32 Missing = 0;
		for (uint8 Id = TraceCharacterRoster::FirstId; Id <= TraceCharacterRoster::LastId; ++Id)
		{
			for (int32 SlotIndex = 0; SlotIndex < static_cast<int32>(ETraceLoadoutSlot::Count); ++SlotIndex)
			{
				const FString Name = TraceAbilityNames::Get(
					static_cast<ETraceCharacterId>(Id), static_cast<ETraceLoadoutSlot>(SlotIndex));
				if (Name.IsEmpty())
				{
					++Missing;
					UE_LOG(LogTraceGame, Error, TEXT("[LoadoutScreen]   no name for kit %d slot %s"),
						Id, TraceLoadoutSlotToString(static_cast<ETraceLoadoutSlot>(SlotIndex)));
				}
			}
		}
		Failures += Missing;
		Check(TEXT("all 30 abilities have a name to draw"), Missing == 0,
			FString::Printf(TEXT("%d missing"), Missing));

		// ---- put it back -------------------------------------------------------------------------
		Comp->ApplyLoadout(Restore);
		Subject->ServerSetCharacterSelectOpen(bRestoreSelect, 0.f);

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadoutScreen] ===== PASS — the screen stages, the server decides, and the kits "
				     "get built. ====="));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[LoadoutScreen] ===== *** FAIL *** %d check(s) — see above ====="), Failures);
		}
	}

	FAutoConsoleCommand Cmd(
		TEXT("Trace.Loadout.Screen"),
		TEXT("S5. Drive the loadout screen's own keys end to end and prove the pick reaches the server."),
		FConsoleCommandDelegate::CreateStatic(&Run));
}
#endif   // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Loadout.Library — S6 and S7.
//
// S6: five saved loadouts that survive the process. The failure this guards against is the one the
// player cannot diagnose — a library that remembers until you quit and then does not, leaving them
// unable to tell which of their five are real. So the test writes through the real settings object,
// flushes, and reads back through the same accessors the screen uses.
//
// S7: BOTS KEEP UNIFORM LOADOUTS. Nothing in AI/ mentions a loadout, so this is true by
// construction — which is exactly the kind of claim that quietly stops being true. A bot running a
// mixed loadout would not crash; it would just be a bot with abilities nobody designed together,
// and nobody would notice for a long time.
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
			TEXT("[LoadoutLibrary] ===== S6/S7: five saved loadouts, and bots that stay uniform ====="));

		UTraceUserSettings& Settings = UTraceUserSettings::Get();

		// ---- save what we are about to trample ----------------------------------------------
		TArray<FTraceLoadout> Restore;
		for (int32 Index = 0; Index < UTraceUserSettings::SavedLoadoutCount; ++Index)
		{
			Restore.Add(Settings.GetSavedLoadout(Index));
		}

		// ---- S6: a slot round-trips ------------------------------------------------------------
		FTraceLoadout Wanted;
		Wanted.Movement  = ETraceCharacterId::Chut;
		Wanted.Passive   = ETraceCharacterId::Mace;
		Wanted.Activated = ETraceCharacterId::Elle;

		// SLOT 4, NOT SLOT 0, deliberately. Writing the last-but-one slot into an empty array is the
		// case that would silently land at index 0 and become slot 1 — the array has to GROW, with
		// the slots in between existing and empty.
		Settings.SetSavedLoadout(3, Wanted);
		Check(TEXT("slot 4 reads back exactly what was written"),
			Settings.GetSavedLoadout(3) == Wanted, TraceLoadoutToString(Settings.GetSavedLoadout(3)));
		Check(TEXT("slot 3 is still empty, not the one we wrote"),
			Settings.GetSavedLoadout(2).IsEmpty(), TraceLoadoutToString(Settings.GetSavedLoadout(2)));
		Check(TEXT("an index past the end reads empty, not garbage"),
			Settings.GetSavedLoadout(99).IsEmpty(), TEXT("out of range is 'empty', never a bounds bug"));

		// ---- S6: the screen's number keys ------------------------------------------------------
		FTraceLoadoutSelect Screen;
		Screen.DebugPick(ETraceLoadoutSlot::Movement,  ETraceCharacterId::Rocco);
		Screen.DebugPick(ETraceLoadoutSlot::Passive,   ETraceCharacterId::Rocco);
		Screen.DebugPick(ETraceLoadoutSlot::Activated, ETraceCharacterId::Rocco);

		Screen.DebugRecall(3);
		Check(TEXT("pressing 4 loads saved slot 4 into the columns"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		// AN EMPTY SLOT MUST NOT WIPE THE PAGE. This is the expensive misread: a stray keypress on a
		// 45 second clock throwing away a loadout the player had just built.
		Screen.DebugRecall(1);
		Check(TEXT("pressing an EMPTY slot changes nothing"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		// ---- S6: storing from the screen -------------------------------------------------------
		Screen.DebugPick(ETraceLoadoutSlot::Movement, ETraceCharacterId::Lily);
		Screen.DebugStore(0);
		Check(TEXT("SHIFT+1 writes the staged loadout to slot 1"),
			Settings.GetSavedLoadout(0) == Screen.GetStaged(),
			TraceLoadoutToString(Settings.GetSavedLoadout(0)));

		// ---- S6: the library cannot reach the server -------------------------------------------
		// Not a behaviour test so much as a statement of the design: library mode has no player state
		// in its signature at all, so there is no path from it to ServerSetLoadout to get wrong.
		Screen.OpenLibrary(2);
		Check(TEXT("library mode opens on the slot it was given"),
			Screen.GetLibrarySlot() == 2, FString::FromInt(Screen.GetLibrarySlot()));
		Screen.CloseLibrary();
		Check(TEXT("and closes"), !Screen.IsLibraryOpen(), TEXT(""));

		// ---- S7: every bot in the world is uniform ---------------------------------------------
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

		// ---- put the library back ----------------------------------------------------------------
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
		TEXT("S6/S7. Prove the five saved loadouts round-trip through the settings file and that no bot "
		     "is running a mixed loadout."),
		FConsoleCommandDelegate::CreateStatic(&Run));
}
#endif   // !UE_BUILD_SHIPPING
