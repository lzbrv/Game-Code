#include "UI/TraceLoadoutSelect.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"      // the dev press queue's injector
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"     // FInputKeyEventArgs — same injector
#include "Misc/CoreMiscDefines.h"  // FInputDeviceId

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterRoster.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Settings/TraceGamepadInput.h"
#include "Settings/TraceUserSettings.h"
#include "Trace.h"
#include "UI/TraceAbilityNames.h"
#include "UI/Text/TraceCanvasText.h"
#include "UI/Text/TraceGameText.h"
#include "UI/Text/TraceText.h"
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"
#include "UI/Widgets/Menu/TraceMenuKit.h"

// NAMED, not anonymous: this module builds as a unity blob, and an anonymous namespace's names are
// loose for the rest of that translation unit.
namespace TraceLoadoutSelectArm
{
	static int32 GArmed = 1;
	static FAutoConsoleVariableRef CVarLoadoutScreen(
		TEXT("Trace.UI.LoadoutScreen"),
		GArmed,
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
		return TraceLoadoutSelectArm::GArmed != 0;
	}

	bool IsReopenAllowed(ETraceMatchState MatchState, bool bHalfTimeBreak, bool bLoadoutArmed)
	{
		// The match is over: there is nothing left to play a loadout in.
		if (MatchState == ETraceMatchState::PostMatch)
		{
			return false;
		}

		// The legacy character page kept D31's mid-match character switch. It stays exactly as it was
		// behind the arm, so putting that page back puts its behaviour back with it.
		if (!bLoadoutArmed)
		{
			return true;
		}

		// [S4] "loadout is locked until halftime": before the whistle, or in the break, and never in
		// live play.
		return bHalfTimeBreak || MatchState != ETraceMatchState::InProgress;
	}

	bool IsReopenAllowed(const UObject* WorldContext)
	{
		const UWorld* World = (WorldContext != nullptr) ? WorldContext->GetWorld() : nullptr;
		const ATraceGameState* TraceGS = (World != nullptr) ? World->GetGameState<ATraceGameState>() : nullptr;
		if (TraceGS == nullptr)
		{
			return true;   // a fixture world with no match to lock
		}
		return IsReopenAllowed(TraceGS->TraceMatchState, TraceGS->IsHalfTimeBreak(), IsArmed());
	}
}

// =================================================================================================
// THE LAYOUT — every number a 1080p design pixel, multiplied by UIScale.
//
//    56  [0 - 0] SIDES SWITCHED (half time) ... title (centred) ......... countdown value box
//        (the line their caps are centred on)
//   100  Q LB  [ MOVEMENT ]  [ PASSIVE ]  [ ACTIVATED ]  E RB   tabs, key chips at each end
//   198  the 5 x 2 grid of cards
//   888  SAVED [1 RIPPLE] [2] ...                   [F] [X] [ LOCK IN ]
//   950  the message line
//   982  the keyboard legend, and 1022 the pad legend (a pad's chips and line once one has been
//        seen and CONTROLLER INPUT is on)
// =================================================================================================
namespace TraceLoadoutLayout
{
	constexpr float Margin      = TraceMenuKit::PageMarginPx;

	/**
	 * THE PAGE TITLE (BUILD YOUR LOADOUT, HALF TIME, LOADOUT n) IS SET LIKE EVERY OTHER SCREEN TITLE:
	 * Sofachrome, white, untracked, at the page titles' 30 px cap height — SETTINGS, PAUSED, JOIN A GAME,
	 * FULL TIME. It was type size 40 (a 27 px cap) and letter-spaced, so going from PAUSED or SETTINGS
	 * to this page changed the title's size and spacing. Its caps are centred where they always were,
	 * so the score and the countdown beside it did not move.
	 */
	constexpr float TitleCapMid = TraceMenuKit::PageTitleCapMidPx;
	constexpr float TitleCap    = TraceMenuKit::PageTitleCapPx;
	constexpr float CountBoxH   = TraceMenuKit::PageClockBoxPx;

	constexpr float TabY        = 100.f;
	constexpr float TabH        = 74.f;
	constexpr float TabGap      = 22.f;
	constexpr float TabChipH    = 40.f;

	/** The words on a tab, given as the plate height DrawLabel sizes caps from (0.37 of it). */
	constexpr float TabHeadingPlate = 48.f;
	constexpr float TabSummaryPlate = 31.f;

	constexpr float GridTop     = 198.f;
	constexpr float GridBottom  = 868.f;
	constexpr float TileGapX    = 20.f;
	constexpr float TileGapY    = 18.f;
	constexpr int32 Columns     = 5;
	constexpr int32 Rows        = 2;

	/** A tall card keeps a button-sized corner (stylespec §6), or the artist's corner becomes a lozenge. */
	constexpr float CardCorner  = 60.f;
	constexpr float CardPad     = 20.f;
	constexpr float EquippedH   = 34.f;

	constexpr float ActionY     = 888.f;
	constexpr float ActionH     = 54.f;
	constexpr float ActionW     = 250.f;
	constexpr float ActionChipH = 40.f;
	constexpr float SavedSlotW  = 200.f;

	constexpr float MessageY    = 950.f;
	constexpr float FooterY     = 982.f;
	constexpr float FooterChipH = 32.f;
	constexpr float PadLineGap  = 40.f;

	constexpr float SizeName    = 26.f;

	/**
	 * The descriptions, largest first. The size is solved ONCE FOR THE WHOLE SCREEN as the largest
	 * that fits every card on every tab (SolveTypeSizes) — the activated abilities are paragraphs (ZIP
	 * is two hundred characters), and at one fixed size they were cut off mid-sentence at the card's
	 * foot. It used to be solved per tab, which drew the same card at three sizes on one screen. Never
	 * below SizeBodyLast; only a card that cannot fit even there would shrink on its own.
	 */
	constexpr float SizeBody    = 19.f;
	constexpr float SizeBodyLast = 10.f;
	constexpr float BodyLeading = 3.f;
	constexpr float SizeMessage = 18.f;
	constexpr float SizeTimer   = TraceMenuKit::PageClockLabelPx;

	/** How long a status line stays up. */
	constexpr float MessageSeconds = 4.f;

	/** The staged loadout is sent this long before the select deadline, so the picks are the answer. */
	constexpr float AutoSendLead = 1.f;

	static_assert(Columns * Rows == FTraceLoadoutSelect::MaxCards, "MaxCards is the grid");
}

// A NAMED NAMESPACE, LIKE TraceCharacterSelectFile AND TraceTeamSelectFile NEXT DOOR, and no
// `using namespace` anywhere in this file: under a unity build a using-directive leaks into every
// file compiled after this one (the C4459 that broke the Windows build once already).
namespace TraceLoadoutSelectFile
{
	/** NavLastDir's "a direction was held when input arrived; wait for it to be let go". */
	constexpr int32 NavHeld = 99;

	/**
	 * The abilities that fill @p Slot, in table order — one card each.
	 *
	 * *** THE GRID WALKS ABILITIES, NOT THE TEN KITS. *** Demo 35 left nine movement abilities and
	 * ten of each of the others.
	 */
	const TArray<ETraceAbilityId>& AbilitiesFor(ETraceLoadoutSlot Slot)
	{
		static TArray<ETraceAbilityId> Lists[static_cast<int32>(ETraceLoadoutSlot::Count)];
		static bool bBuilt = false;
		if (!bBuilt)
		{
			for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
			{
				TraceAbilityTable::AllForSlot(static_cast<ETraceLoadoutSlot>(Index), Lists[Index]);
			}
			bBuilt = true;
		}

		const int32 SlotIndex = FMath::Clamp(static_cast<int32>(Slot), 0,
			static_cast<int32>(ETraceLoadoutSlot::Count) - 1);
		return Lists[SlotIndex];
	}

	int32 CardCount(ETraceLoadoutSlot Slot)
	{
		return FMath::Min(AbilitiesFor(Slot).Num(), FTraceLoadoutSelect::MaxCards);
	}

	ETraceAbilityId AbilityAtCard(ETraceLoadoutSlot Slot, int32 CardIndex)
	{
		const TArray<ETraceAbilityId>& List = AbilitiesFor(Slot);
		if (List.Num() == 0)
		{
			return ETraceAbilityId::None;
		}
		const int32 Wrapped = ((CardIndex % List.Num()) + List.Num()) % List.Num();
		return List[Wrapped];
	}

	int32 CardForAbility(ETraceLoadoutSlot Slot, ETraceAbilityId Id)
	{
		const int32 Found = AbilitiesFor(Slot).IndexOfByKey(Id);
		return (Found != INDEX_NONE) ? Found : 0;
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

	/** What a tab shows under its heading: the ability chosen for that slot. */
	FString TabSummary(const FTraceLoadout& Loadout, ETraceLoadoutSlot Slot)
	{
		const ETraceAbilityId Id = Loadout.Get(Slot);
		if (Id == ETraceAbilityId::None)
		{
			return TRACE_TEXT("LOADOUT.TAB_EMPTY", "NONE");
		}
		return TraceAbilityNames::ShortLabel(Id);
	}

	/**
	 * The face the DESCRIPTIONS are set in. The owner's v25 instruction, "Use Erbaum Bold for in game
	 * hud and character ability descriptions", read the way the character page reads it — and behind
	 * the same switch, so Trace.HUD.Text.Erbaum 0 puts every half of that change back at once.
	 */
	ETraceTextWeight DescriptionWeight()
	{
		// P11: the variable is FOUND once and READ every call. A console lookup is a lock plus a map
		// probe with a string built for the key, and this runs for every card on every frame. Only a
		// successful find is remembered, so a variable not registered yet is looked for again next time
		// rather than baked in as missing (the reason the character page's copy was never cached).
		static const IConsoleVariable* Arm = nullptr;
		if (Arm == nullptr)
		{
			Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.HUD.Text.Erbaum"));
		}
		const bool bErbaum = (Arm == nullptr) || (Arm->GetInt() != 0);
		return bErbaum ? ETraceTextWeight::Hud : ETraceTextWeight::Light;
	}

	/** How tall @p Lines lines of body text stand at @p Size. */
	float BlockHeight(int32 Lines, float Size, float S)
	{
		return (Lines <= 0) ? 0.f
			: Lines * TraceText::LineHeight(Size) + (Lines - 1) * TraceLoadoutLayout::BodyLeading * S;
	}

	/** @p Text broken on spaces into lines no wider than @p MaxWidth, measured in the style it draws in. */
	void WrapInto(const FString& Text, float MaxWidth, const TraceText::FStyle& Style, TArray<FString>& OutLines)
	{
		OutLines.Reset();
		if (Text.IsEmpty() || MaxWidth <= 0.f)
		{
			return;
		}

		TArray<FString> Words;
		Text.ParseIntoArray(Words, TEXT(" "), /*InCullEmpty=*/true);

		FString Line;
		for (const FString& Word : Words)
		{
			const FString Candidate = Line.IsEmpty() ? Word : (Line + TEXT(" ") + Word);
			if (Line.IsEmpty() || TraceText::MeasureWidth(Candidate, Style) <= MaxWidth)
			{
				// A single word wider than the card still gets its own line rather than vanishing.
				Line = Candidate;
				continue;
			}
			OutLines.Add(Line);
			Line = Word;
		}
		if (!Line.IsEmpty())
		{
			OutLines.Add(Line);
		}
	}

	/**
	 * The size one card's description draws at, wrapped into @p OutLines: the screen's solved
	 * @p ScreenSize, or smaller (never below SizeBodyLast) only if even that does not fit @p Room.
	 * DrawCard and the harness's DebugTypeSizes both come through here.
	 */
	float CardBodySize(const FString& Body, float TextW, float ScreenSize, float Room, ETraceTextWeight Weight,
		float S, TArray<FString>& OutLines)
	{
		float Size = ScreenSize;
		for (;;)
		{
			WrapInto(Body, TextW, TraceText::FStyle(Size, FLinearColor::White, Weight), OutLines);
			if (Size <= TraceLoadoutLayout::SizeBodyLast * S || BlockHeight(OutLines.Num(), Size, S) <= Room)
			{
				return Size;
			}
			Size = FMath::Max(TraceLoadoutLayout::SizeBodyLast * S, Size - 0.5f * S);
		}
	}

	/**
	 * THE KEY TABLE — which physical keys mean which verb. One place, read by the live page through
	 * the controller and by the harness through a fake key set.
	 *
	 * SPACE IS NOT HERE. It is Jump, and a SPACE held at the half-time whistle used to lock in on the
	 * first frame of the page.
	 */
	FTraceLoadoutKeys ReadKeysWith(TFunctionRef<bool(const FKey&)> IsDown, bool bPadEnabled, bool bLibrary)
	{
		auto Pad = [&IsDown, bPadEnabled](const FKey& Key) { return bPadEnabled && IsDown(Key); };

		FTraceLoadoutKeys Keys;
		Keys.NavX = ((IsDown(EKeys::Right) || IsDown(EKeys::D)) ? 1 : 0)
		          - ((IsDown(EKeys::Left)  || IsDown(EKeys::A)) ? 1 : 0);
		Keys.NavY = ((IsDown(EKeys::Down)  || IsDown(EKeys::S)) ? 1 : 0)
		          - ((IsDown(EKeys::Up)    || IsDown(EKeys::W)) ? 1 : 0);

		Keys.bEquip    = IsDown(EKeys::Enter) || Pad(TracePadMenu::ConfirmKey());
		Keys.bLock     = IsDown(EKeys::F) || Pad(TracePadMenu::AltKey());
		Keys.bBack     = IsDown(EKeys::BackSpace) || Pad(TracePadMenu::BackKey())
		               || (bLibrary && IsDown(EKeys::Escape));
		Keys.bTabLeft  = IsDown(EKeys::Q) || Pad(EKeys::Gamepad_LeftShoulder);
		Keys.bTabRight = IsDown(EKeys::E) || Pad(EKeys::Gamepad_RightShoulder);

		// THE SAVED LOADOUTS ON A PAD, on the two buttons nothing else on this page reads. The pad has
		// no number keys, so Y walks the slots (load the next); LT is the pad's SHIFT (save instead).
		Keys.bShift    = IsDown(EKeys::LeftShift) || IsDown(EKeys::RightShift) || Pad(EKeys::Gamepad_LeftTrigger);
		Keys.bSavedNext = Pad(EKeys::Gamepad_FaceButton_Top);

		static const FKey NumberKeys[5] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five };
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Keys.bNumber[Index] = IsDown(NumberKeys[Index]);
		}
		return Keys;
	}

	/** One direction for the repeat clock: +-1 across, +-2 down. One axis per step, across first. */
	int32 NavDirOf(const FTraceLoadoutKeys& Keys)
	{
		return (Keys.NavX != 0) ? Keys.NavX : Keys.NavY * 2;
	}

	/** A saved slot that can be loaded: not empty, and legal under today's rules. Else its plate is disabled. */
	bool SavedUsable(const FTraceLoadout& Loadout)
	{
		return !Loadout.IsEmpty() && UTraceAbilityComponent::IsLoadoutLegal(Loadout);
	}

	/** How a SAVED slot is drawn: the kit plate's state, and whether its word is the quiet grey. */
	struct FSavedSlotLook
	{
		ETraceKitState State = ETraceKitState::Default;
		bool bQuietWord = false;
	};

	/**
	 * EMPTY IS NOT DISABLED. On this kit the dark DISABLED plate means "you cannot act here", and an
	 * empty slot is a live save target (SHIFT+click, SHIFT+1-5, pad LT + Y all store into it). So an
	 * empty slot is the DEFAULT plate with its number in the quiet grey (the KEYBOARD page's "+" chip),
	 * and only a slot today's rules refuse to load keeps the DISABLED plate. Lit (the pointer on it with
	 * SHIFT held, or on a usable slot), any slot wears the hover look: a click there does something.
	 */
	FSavedSlotLook SavedSlotLook(const FTraceLoadout& Saved, bool bLit, bool bPressed)
	{
		FSavedSlotLook Look;
		const bool bEmpty = Saved.IsEmpty();
		Look.State = TraceMenuKit::StateFor(/*bEnabled=*/bLit || bEmpty || SavedUsable(Saved), bLit, bLit && bPressed);
		Look.bQuietWord = bEmpty && !bLit;
		return Look;
	}

	/**
	 * Does the page name PAD buttons as well as keys? Once a pad has been seen on this machine, and
	 * only while CONTROLLER INPUT is on: with it off the page ignores the pad (ReadKeys), and a chip
	 * or a legend line naming a button that does nothing would be false. The title, the options
	 * overlay and the results screen ask the same two questions.
	 */
	bool PadHintsShown(const APlayerController* PC)
	{
		return TracePadMenu::IsEnabled() && TracePadMenu::HasSeenPad(PC);
	}

	/** The controls that carry key chips on the page itself (the legend is separate). */
	enum class EChip : uint8
	{
		TabLeft,
		TabRight,
		Confirm,   // LOCK IN, or SAVE in the library
		Back,      // the library's BACK plate
	};

	/**
	 * The one or two key chips of one group: the KEYBOARD key, and the PAD button after it. Pointers into
	 * the text store, whose strings live for the whole process (TraceGameText::Get), so building the
	 * group every drawn frame copies and allocates nothing (P11).
	 */
	struct FChipKeys
	{
		const FString* Keys[2] = { nullptr, nullptr };
		int32 Num = 0;

		void Add(const FString& Key)
		{
			// An emptied line (Ranen's "KEY =") takes its chip with it.
			if (!Key.IsEmpty() && Num < 2)
			{
				Keys[Num++] = &Key;
			}
		}

		const FString& operator[](int32 Index) const { return *Keys[Index]; }
	};

	/**
	 * The chips beside @p Which: the KEYBOARD key always, and the PAD button after it while pad hints
	 * show — both, the way every other screen's legend names both. They used to swap to the pad's
	 * alone once any pad input had been seen, so a keyboard player read LB / RB / X.
	 */
	FChipKeys ChipKeys(EChip Which, bool bLibrary, bool bPadHints)
	{
		FChipKeys Out;
		switch (Which)
		{
		case EChip::TabLeft:
			Out.Add(TRACE_TEXT("LOADOUT.KEY_TAB_LEFT", "Q"));
			if (bPadHints) { Out.Add(TRACE_TEXT("LOADOUT.PAD_KEY_TAB_LEFT", "LB")); }
			break;
		case EChip::TabRight:
			Out.Add(TRACE_TEXT("LOADOUT.KEY_TAB_RIGHT", "E"));
			if (bPadHints) { Out.Add(TRACE_TEXT("LOADOUT.PAD_KEY_TAB_RIGHT", "RB")); }
			break;
		case EChip::Confirm:
			Out.Add(TRACE_TEXT("LOADOUT.KEY_LOCK_IN", "F"));
			if (bPadHints) { Out.Add(TRACE_TEXT("LOADOUT.PAD_KEY_LOCK_IN", "X")); }
			break;
		case EChip::Back:
			Out.Add(bLibrary ? TRACE_TEXT("LOADOUT.KEY_BACK", "ESC") : TRACE_TEXT("LOADOUT.KEY_BACK_TAB", "BKSP"));
			if (bPadHints) { Out.Add(TRACE_TEXT("LOADOUT.PAD_KEY_BACK", "B")); }
			break;
		default:
			break;
		}
		return Out;
	}

	/** The gap between two chips of one group. */
	constexpr float ChipPairGap = 8.f;

	/** How wide DrawChipGroup draws @p Keys at chip height @p ChipH. */
	float ChipGroupWidth(const FChipKeys& Keys, float ChipH, float Gap)
	{
		float Width = 0.f;
		for (int32 Index = 0; Index < Keys.Num; ++Index)
		{
			const float ChipW = TraceMenuKit::KeyChipWidth(Keys[Index], ChipH);
			if (ChipW > 0.f)
			{
				Width += (Width > 0.f ? Gap : 0.f) + ChipW;
			}
		}
		return Width;
	}

	/** @p Keys as chips left to right from @p X. Returns the width drawn. */
	float DrawChipGroup(AHUD* HUD, float X, float Y, float ChipH, float Gap, const FChipKeys& Keys,
		float NowSeconds)
	{
		float PenX = X;
		for (int32 Index = 0; Index < Keys.Num; ++Index)
		{
			const float ChipW = TraceMenuKit::KeyChipWidth(Keys[Index], ChipH);
			if (ChipW <= 0.f)
			{
				continue;
			}
			TraceMenuKit::DrawKeyChip(HUD, ETraceKitState::Default, PenX, Y, ChipH, Keys[Index], NowSeconds);
			PenX += ChipW + Gap;
		}
		return FMath::Max(0.f, PenX - X - Gap);
	}

	/**
	 * THE FOOTER: every key that does something on this page right now, the keyboard's line and — while
	 * pad hints show — the pad's line under it. LABELS, NOT SENTENCES.
	 *
	 * BACK is listed where it does something: always in the library (ESC / B leave), and on the match
	 * page only on a tab after the first (BACKSPACE / B step back a question, and on the first tab there
	 * is nowhere to step back to). The pad's Y and LT + Y are the saved loadouts' pad route.
	 */
	void BuildLegend(bool bLibrary, int32 InTab, bool bPadHints, TArray<FTraceKitLegendItem>& OutKeyboard,
		TArray<FTraceKitLegendItem>& OutPad)
	{
		const FString LockWord = bLibrary ? TRACE_TEXT("LOADOUT.SAVE_SLOT", "SAVE") : TRACE_TEXT("LOADOUT.LOCK_IN", "LOCK IN");
		const FString TabWord = TRACE_TEXT("LOADOUT.LEGEND_TAB", "TAB");
		const FString EquipWord = TRACE_TEXT("LOADOUT.LEGEND_EQUIP", "EQUIP");
		const FString ChooseWord = TRACE_TEXT("LOADOUT.LEGEND_CHOOSE", "CHOOSE");
		const FString BackWord = TRACE_TEXT("LOADOUT.BACK", "BACK");
		const FString LoadWord = TRACE_TEXT("LOADOUT.LEGEND_LOAD", "LOAD");
		const FString StoreWord = TRACE_TEXT("LOADOUT.LEGEND_STORE", "SAVE");
		const bool bBack = bLibrary || InTab > 0;

		OutKeyboard.Reset();
		OutKeyboard.Add({ TRACE_TEXT("LOADOUT.KEY_MOVE", "ARROWS"), ChooseWord });
		OutKeyboard.Add({ TRACE_TEXT("LOADOUT.KEY_TABS", "Q / E"), TabWord });
		OutKeyboard.Add({ TRACE_TEXT("LOADOUT.KEY_EQUIP", "ENTER"), EquipWord });
		OutKeyboard.Add({ TRACE_TEXT("LOADOUT.KEY_LOCK_IN", "F"), LockWord });
		if (bBack)
		{
			OutKeyboard.Add({ bLibrary ? TRACE_TEXT("LOADOUT.KEY_BACK", "ESC") : TRACE_TEXT("LOADOUT.KEY_BACK_TAB", "BKSP"),
				BackWord });
		}
		if (!bLibrary)
		{
			OutKeyboard.Add({ TRACE_TEXT("LOADOUT.KEY_LOAD", "1 - 5"), LoadWord });
			OutKeyboard.Add({ TRACE_TEXT("LOADOUT.KEY_STORE", "SHIFT 1 - 5"), StoreWord });
		}

		OutPad.Reset();
		if (!bPadHints)
		{
			return;
		}
		OutPad.Add({ TRACE_TEXT("LOADOUT.PAD_KEY_MOVE", "D-PAD"), ChooseWord });
		OutPad.Add({ TRACE_TEXT("LOADOUT.PAD_KEY_TABS", "LB / RB"), TabWord });
		OutPad.Add({ TRACE_TEXT("LOADOUT.PAD_KEY_EQUIP", "A"), EquipWord });
		OutPad.Add({ TRACE_TEXT("LOADOUT.PAD_KEY_LOCK_IN", "X"), LockWord });
		if (bBack)
		{
			OutPad.Add({ TRACE_TEXT("LOADOUT.PAD_KEY_BACK", "B"), BackWord });
		}
		if (!bLibrary)
		{
			OutPad.Add({ TRACE_TEXT("LOADOUT.PAD_KEY_LOAD", "Y"), LoadWord });
			OutPad.Add({ TRACE_TEXT("LOADOUT.PAD_KEY_STORE", "LT + Y"), StoreWord });
		}
	}

	/** Is @p WorldContext's match in its half-time break? False with no Trace game state. */
	bool InHalfTimeBreak(const UObject* WorldContext)
	{
		const UWorld* World = (WorldContext != nullptr) ? WorldContext->GetWorld() : nullptr;
		const ATraceGameState* TraceGS = (World != nullptr) ? World->GetGameState<ATraceGameState>() : nullptr;
		return TraceGS != nullptr && TraceGS->IsHalfTimeBreak();
	}
}

namespace TraceLoadoutSelect
{
	bool ReadsKey(const FKey& Key)
	{
		// The page's own table, asked "what would this ONE key do?" — pad on and library mode on, the
		// widest the page ever reads, so nothing it can act on is missed.
		const FTraceLoadoutKeys OneKey = TraceLoadoutSelectFile::ReadKeysWith(
			[&Key](const FKey& Asked) { return Asked == Key; }, /*bPadEnabled=*/true, /*bLibrary=*/true);

		bool bNumberKey = false;
		for (const bool bEachNumber : OneKey.bNumber)
		{
			bNumberKey = bNumberKey || bEachNumber;
		}

		return OneKey.NavX != 0 || OneKey.NavY != 0 || OneKey.bEquip || OneKey.bLock || OneKey.bBack
			|| OneKey.bTabLeft || OneKey.bTabRight || OneKey.bShift || OneKey.bSavedNext || bNumberKey
			// The pointer (StepPointer), and the pad's D-pad / stick (ReadKeys, through TracePadMenu).
			|| Key == EKeys::LeftMouseButton
			|| Key == EKeys::Gamepad_DPad_Up || Key == EKeys::Gamepad_DPad_Down
			|| Key == EKeys::Gamepad_DPad_Left || Key == EKeys::Gamepad_DPad_Right
			|| Key == EKeys::Gamepad_LeftStick_Up || Key == EKeys::Gamepad_LeftStick_Down
			|| Key == EKeys::Gamepad_LeftStick_Left || Key == EKeys::Gamepad_LeftStick_Right;
	}
}

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Loadout.Press — drive the LIVE page from a headless run, for screenshots
//
// A queue of verbs the open page (match or library) takes one per frame, each followed by a frame
// with nothing pressed, so every verb is a real press edge through StepInput. It ORs into what the
// keyboard and pad actually report and changes nothing else. "wait=<seconds>" spaces the verbs so a
// -TraceAutoShot capture can land between them; "pad" taps the right stick click through the real
// input path so the page believes a controller has been seen and draws its pad captions.
// =================================================================================================
namespace TraceLoadoutPressQueue
{
	static TArray<FString> GPending;
	static float GNextTime = 0.f;
	static bool bGReleaseFrame = false;
	static bool bGPadHeld = false;

	static void InjectPadKey(APlayerController* PC, bool bPressed)
	{
		const FInputKeyEventArgs Args(nullptr, FInputDeviceId::CreateFromInternalId(0),
			EKeys::Gamepad_RightThumbstick, bPressed ? IE_Pressed : IE_Released, bPressed ? 1.f : 0.f,
			/*bIsTouchEvent=*/false, FPlatformTime::Cycles64());
		PC->InputKey(Args);
	}

	/** ORs this frame's queued verb, if any, into @p Down. */
	static void Apply(APlayerController* PC, FTraceLoadoutKeys& Down, float NowSeconds)
	{
		if (bGReleaseFrame)
		{
			bGReleaseFrame = false;
			if (bGPadHeld && PC != nullptr)
			{
				InjectPadKey(PC, false);
				bGPadHeld = false;
			}
			return;
		}
		if (GPending.Num() == 0 || NowSeconds < GNextTime)
		{
			return;
		}

		const FString Verb = GPending[0].ToLower();
		GPending.RemoveAt(0);

		if (Verb.StartsWith(TEXT("wait=")))
		{
			GNextTime = NowSeconds + FCString::Atof(*Verb.Mid(5));
			return;
		}

		if      (Verb == TEXT("left"))     { Down.NavX = -1; }
		else if (Verb == TEXT("right"))    { Down.NavX = 1; }
		else if (Verb == TEXT("up"))       { Down.NavY = -1; }
		else if (Verb == TEXT("down"))     { Down.NavY = 1; }
		else if (Verb == TEXT("equip"))    { Down.bEquip = true; }
		else if (Verb == TEXT("lock"))     { Down.bLock = true; }
		else if (Verb == TEXT("back"))     { Down.bBack = true; }
		else if (Verb == TEXT("tableft"))  { Down.bTabLeft = true; }
		else if (Verb == TEXT("tabright")) { Down.bTabRight = true; }
		else if (Verb == TEXT("y"))        { Down.bSavedNext = true; }
		else if (Verb == TEXT("lty"))      { Down.bShift = true; Down.bSavedNext = true; }
		else if (Verb == TEXT("pad"))
		{
			if (PC != nullptr)
			{
				InjectPadKey(PC, true);
				bGPadHeld = true;
			}
		}
		else if (Verb.Len() == 1 && FChar::IsDigit(Verb[0]) && Verb[0] >= TEXT('1') && Verb[0] <= TEXT('5'))
		{
			Down.bNumber[Verb[0] - TEXT('1')] = true;
		}
		else if (Verb.Len() == 2 && Verb[0] == TEXT('s') && Verb[1] >= TEXT('1') && Verb[1] <= TEXT('5'))
		{
			Down.bShift = true;
			Down.bNumber[Verb[1] - TEXT('1')] = true;
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[LoadoutScreen] Trace.Loadout.Press: unknown verb '%s'."), *Verb);
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen] Trace.Loadout.Press: %s"), *Verb);
		bGReleaseFrame = true;
	}

	static FAutoConsoleCommand CmdPress(
		TEXT("Trace.Loadout.Press"),
		TEXT("Dev only. Trace.Loadout.Press <verb> [verb ...]: queues presses for the open loadout page ")
		TEXT("(match or library), one per frame pair. Verbs: left right up down equip lock back tableft ")
		TEXT("tabright 1-5 s1-s5 (shift+n) y (pad Y) lty (pad LT+Y) pad (a real pad tap, so pad captions ")
		TEXT("show) wait=<seconds>."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			for (const FString& Arg : Args)
			{
				TArray<FString> Parts;
				Arg.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
				GPending.Append(Parts);
			}
			UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen] Trace.Loadout.Press: %d verb(s) queued."), GPending.Num());
		}));
}
#endif

// =================================================================================================
// Frame
// =================================================================================================

bool FTraceLoadoutSelect::WantsOpen(const ATracePlayerState* LocalState)
{
	// THE SERVER DECIDES WHETHER THIS IS UP, AND THE ARM DECIDES WHICH PAGE IT IS.
	return TraceLoadoutSelect::IsArmed()
		&& (LocalState != nullptr) && LocalState->IsCharacterSelectOpen();
}

void FTraceLoadoutSelect::OnOpened(ATracePlayerState* LocalState)
{
	SyncStagedFromServer(LocalState);

	bStagedDirty = false;
	bAutoSent = false;
	bLockHint = false;
	bLockSent = false;
	LastSavedSlot = INDEX_NONE;
	LastMessage.Reset();

	// The frame that opened the page reads nothing...
	IgnoreInputBeforeFrame = GFrameCounter + 1;

	// ...and EVERY KEY IS ASSUMED HELD until the page has seen it up. An ENTER still down from the team
	// screen's confirm (the page opens a moment later) or a SPACE held at the whistle is then a key
	// that was already down, not a press, and nothing fires until it is let go and pressed again.
	Held = FTraceLoadoutKeys::AllHeld();
	NavLastDir = TraceLoadoutSelectFile::NavHeld;
	NavNextTime = 0.f;

	// THE FIRST POINTER SAMPLE OF THIS OPENING IS NOT A MOVE. bHasCursor used to survive from the last
	// opening, so at half time the stale warm-up position counted as movement and the highlight
	// jumped to whatever card the pointer rested on.
	bHasCursor = false;
	bMouseArmed = false;
	HoveredCard = INDEX_NONE;
	HoveredTab = INDEX_NONE;
	HoveredSaved = INDEX_NONE;
	bHoveredConfirm = false;
	bHoveredBack = false;
	bPointerLed = false;   // the page opens on the keys' highlight, whatever the pointer rests on

	// FBox2D's default leaves bIsValid uninitialised, and last opening's rects must not be hit.
	for (FBox2D& Rect : CardRects)
	{
		Rect = FBox2D(ForceInit);
	}
	for (FBox2D& Rect : TabRects)
	{
		Rect = FBox2D(ForceInit);
	}
	for (FBox2D& Rect : SavedRects)
	{
		Rect = FBox2D(ForceInit);
	}
	ConfirmRect = FBox2D(ForceInit);
	BackRect = FBox2D(ForceInit);
}

void FTraceLoadoutSelect::Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
	float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed)
{
	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;
	Now = InNow;
	{
		const UWorld* const ClockWorld = (HUD != nullptr) ? HUD->GetWorld() : nullptr;
		AnimNow = (ClockWorld != nullptr) ? static_cast<float>(ClockWorld->GetRealTimeSeconds()) : InNow;
	}

	// ---- HALF TIME SAYS SO -----------------------------------------------------------------------
	//
	// The break opens this page over the match, and the HUD's HALF TIME card is under it, so for the
	// whole break nothing said it was half time: the page read BUILD YOUR LOADOUT, the same as before
	// kickoff. It is titled HALF TIME in the break (the HUD card's own line), with the score and
	// SIDES SWITCHED beside it (DrawHeader). Decided while the page is up and kept through its fade:
	// the break ends by closing the window, and the page must not fade out on the other title.
	const bool bWantOpen = WantsOpen(LocalState);
	if (bWantOpen || PageTitle.IsEmpty())
	{
		bHalfTimeHeader = TraceLoadoutSelectFile::InHalfTimeBreak(LocalState);
		PageTitle = bHalfTimeHeader ? TRACE_TEXT("HUD.BANNER_HALF_TIME", "HALF TIME")
		                            : TRACE_TEXT("LOADOUT.TITLE", "BUILD YOUR LOADOUT");
	}

	if (bWantOpen && !bOpen)
	{
		OnOpened(LocalState);
	}

	// THE WINDOW CLOSED WITH A LOCK IN IN FLIGHT: LOCKED IN only if the server now holds what was sent.
	// A deadline that shut the window under the request (the server's auto-assign) is not a lock in.
	if (bOpen && !bWantOpen && bLockSent)
	{
		bLockSent = false;
		const UTraceAbilityComponent* Comp = (LocalState != nullptr)
			? LocalState->FindComponentByClass<UTraceAbilityComponent>() : nullptr;
		if (Comp != nullptr && Comp->GetLoadout() == SentLoadout)
		{
			SetMessage(TRACE_TEXT("LOADOUT.LOCKED_IN", "LOCKED IN"), /*bWarning=*/false);
		}
	}
	bOpen = bWantOpen;

	// P10 — THE PAGE FADES OPEN AND CLOSED, on real time.
	const float PageAlpha = Fade.Update(bOpen);

	if (!bOpen)
	{
		// THE CLOSE: LOCK IN (or the deadline) shut the window, and the page fades off the match as it
		// last stood — the picks the player just sent, no countdown, no input, no pointer.
		if (PageAlpha > 0.f && HUD != nullptr && TraceLoadoutSelect::IsArmed())
		{
			bPointerOwned = false;
			TraceMenuKit::FScopedOpacity Fading(PageAlpha);
			Draw(HUD, PC, LocalState, PageTitle);
		}
		return;
	}

	// A refusal of a LOCK IN sent on an earlier frame (a real client hears it a round trip later).
	ReadLockInReply(LocalState);

	// ONE POINTER ON SCREEN. While the pause menu is in front it owns the pointer, and this page —
	// still running underneath so the pick is not cancelled, hidden by the menu's black, which draws
	// this page's clock for it — must not leave a frozen blade under the scrim beside the live one.
	bPointerOwned = bInputAllowed;

	const bool bAct = bInputAllowed && PC != nullptr && GFrameCounter >= IgnoreInputBeforeFrame;
	if (PC != nullptr)
	{
		// SAMPLED EVERY FRAME, acted on only when allowed: see StepInput.
		FTraceLoadoutKeys Down = ReadKeys(PC);
#if !UE_BUILD_SHIPPING
		if (bAct)
		{
			TraceLoadoutPressQueue::Apply(PC, Down, Now);
		}
#endif
		StepInput(Down, LocalState, bAct);

		// A LOCK IN just closed the window on the server; nothing else may click this frame.
		PollPointer(PC, LocalState, bAct && WantsOpen(LocalState));
	}

	// ---- THE DEADLINE KEEPS WHAT THE PLAYER CHOSE ------------------------------------------------
	//
	// The server's timeout auto-assigns and never looks at this page, so a player who spent the
	// window choosing cards and did not press LOCK IN lost every one of them — while the cards said
	// EQUIPPED. Their picks are sent a second before the deadline instead. Only if they changed
	// something: a player who touched nothing still gets the server's own timeout, as before.
	//
	// NOT WHILE A LOCK IN IS ON THE WIRE: that request already carries the picks, and a second one would
	// be refused as LOCKED by the server the first one closed (see Confirm).
	if (bStagedDirty && !bAutoSent && !bLockSent && LocalState != nullptr && WantsOpen(LocalState)
		&& LocalState->CharacterSelectDeadlineServerTime > 0.f
		&& LocalState->GetCharacterSelectTimeRemaining() <= TraceLoadoutLayout::AutoSendLead)
	{
		bAutoSent = true;
		UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen] the select window is closing; sending the staged picks."));
		Confirm(LocalState);
	}

	TraceMenuKit::FScopedOpacity Fading(PageAlpha);
	Draw(HUD, PC, LocalState, PageTitle);
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

	// A player who has never picked gets a legal, complete starting point: the first ability of each
	// column.
	if (Staged.IsEmpty())
	{
		for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
		{
			const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Index);
			Staged.Set(Slot, TraceLoadoutSelectFile::AbilityAtCard(Slot, 0));
		}
	}

	for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
	{
		Highlighted[Index] = TraceLoadoutSelectFile::CardForAbility(static_cast<ETraceLoadoutSlot>(Index),
			Staged.Get(static_cast<ETraceLoadoutSlot>(Index)));
	}
	Tab = 0;
}

// =================================================================================================
// Input — keys and pad
// =================================================================================================

FTraceLoadoutKeys FTraceLoadoutSelect::ReadKeys(const APlayerController* PC) const
{
	if (PC == nullptr)
	{
		return FTraceLoadoutKeys();
	}

	FTraceLoadoutKeys Keys = TraceLoadoutSelectFile::ReadKeysWith(
		[PC](const FKey& Key) { return PC->IsInputKeyDown(Key); }, TracePadMenu::IsEnabled(), IsLibraryOpen());

	// The D-pad and the stick through the shared pad vocabulary, per axis, the keyboard first.
	if (Keys.NavX == 0)
	{
		Keys.NavX = TracePadMenu::NavX(PC);
	}
	if (Keys.NavY == 0)
	{
		Keys.NavY = TracePadMenu::NavY(PC);
	}

	// MENU/START reaches a menu as a synthetic Escape pressed and released in one frame, which the
	// level read above never sees. In library mode that is a way back, so the press event counts too.
	Keys.bEscapePressed = IsLibraryOpen() && PC->WasInputKeyJustPressed(EKeys::Escape);
	return Keys;
}

FTraceLoadoutKeys FTraceLoadoutSelect::DebugKeysFor(const TArray<FKey>& DownKeys, bool bLibrary)
{
	return TraceLoadoutSelectFile::ReadKeysWith(
		[&DownKeys](const FKey& Key) { return DownKeys.Contains(Key); }, /*bPadEnabled=*/true, bLibrary);
}

void FTraceLoadoutSelect::StepInput(const FTraceLoadoutKeys& Down, ATracePlayerState* LocalState, bool bInputAllowed)
{
	// ---- 1. EDGES, ON EVERY FRAME THE PAGE IS OPEN — GATED OR NOT --------------------------------
	//
	// A press is "down now, up last frame", and "last frame" has to mean the last frame that
	// HAPPENED, not the last frame this page was allowed to read. The first version only remembered a
	// key while it could act on it, so an ENTER that pressed RESUME on the pause menu, and was still
	// down on the next frame, reached this page as a brand new press against a memory from before the
	// pause — and locked in. Recording the level before the gate is what makes a gate of any kind
	// (the pause menu, team select in front, the opening frame) unable to manufacture a press.
	auto Rose = [](bool bNowDown, bool bBefore) { return bNowDown && !bBefore; };

	const bool bEquipPressed    = Rose(Down.bEquip, Held.bEquip);
	const bool bLockPressed     = Rose(Down.bLock, Held.bLock);
	const bool bBackPressed     = Rose(Down.bBack, Held.bBack) || Down.bEscapePressed;
	const bool bTabLeftPressed  = Rose(Down.bTabLeft, Held.bTabLeft);
	const bool bTabRightPressed = Rose(Down.bTabRight, Held.bTabRight);
	const bool bSavedNextPressed = Rose(Down.bSavedNext, Held.bSavedNext);
	bool bNumberPressed[5];
	for (int32 Index = 0; Index < 5; ++Index)
	{
		bNumberPressed[Index] = Rose(Down.bNumber[Index], Held.bNumber[Index]);
	}
	Held = Down;

	const int32 NavDir = TraceLoadoutSelectFile::NavDirOf(Down);

	if (!bInputAllowed)
	{
		// A direction held across the gate must be let go before it walks the grid.
		NavLastDir = (NavDir != 0) ? TraceLoadoutSelectFile::NavHeld : 0;
		return;
	}

	// ---- 2. THE VERBS ------------------------------------------------------------------------------

	// ANY VERB HANDS THE HOVER LOOK BACK TO THE KEYS' HIGHLIGHT. Without this a pointer resting on a
	// tab or LOCK IN kept its glow while the arrows walked the grid, and the page showed two.
	bool bAnyDigit = false;
	for (const bool bEach : bNumberPressed)
	{
		bAnyDigit = bAnyDigit || bEach;
	}
	if (bTabLeftPressed || bTabRightPressed || bEquipPressed || bBackPressed || bLockPressed || bAnyDigit
		|| bSavedNextPressed)
	{
		bPointerLed = false;
	}

	// Q / E and LB / RB change tab. The comment above this line claimed the shoulder buttons for as
	// long as the page existed and nothing read them: a pad player could not leave MOVEMENT.
	if (bTabLeftPressed != bTabRightPressed)
	{
		MoveTab(bTabRightPressed ? 1 : -1);
	}

	// The grid, on the menus' one shared repeat clock.
	if (NavLastDir == TraceLoadoutSelectFile::NavHeld)
	{
		if (NavDir == 0)
		{
			NavLastDir = 0;
		}
	}
	else if (TracePadMenu::StepRepeat(NavDir, NavLastDir, NavNextTime, Now))
	{
		MoveCard((FMath::Abs(NavDir) == 1) ? NavDir : 0, (FMath::Abs(NavDir) == 2) ? NavDir / 2 : 0);
		bPointerLed = false;
	}

	// THE FIVE SAVED LOADOUTS, in match mode. In the library the page IS one of them, and a number key
	// that quietly overwrote a different slot is exactly the surprise a library editor must not have.
	if (!IsLibraryOpen())
	{
		for (int32 Index = 0; Index < 5; ++Index)
		{
			if (bNumberPressed[Index])
			{
				Down.bShift ? Store(Index) : Recall(Index);
			}
		}

		// ...AND ON A PAD, which has no number keys and could only reach them by pointer: Y loads the
		// next saved loadout, LT + Y saves (StoreCurrent says where).
		if (bSavedNextPressed)
		{
			Down.bShift ? StoreCurrent() : RecallNext();
		}
	}

	// EQUIP: the highlighted card goes into this tab's slot, and the page moves on to the next
	// question, so ENTER ENTER ENTER builds a loadout. The last tab has nothing after it; the LOCK IN
	// plate lights up instead, which is the one thing left to do.
	if (bEquipPressed)
	{
		EquipHighlighted();
		if (Tab < static_cast<int32>(ETraceLoadoutSlot::Count) - 1)
		{
			MoveTab(1);
		}
		else
		{
			bLockHint = true;
		}
	}

	// BACK: the library editor leaves without saving; the match page has nowhere to go back to, so it
	// steps back a question — the inverse of what ENTER's advance just did.
	if (bBackPressed)
	{
		if (IsLibraryOpen())
		{
			UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen] library editor: back, nothing saved."));
			CloseLibrary();
			return;
		}
		if (Tab > 0)
		{
			MoveTab(-1);
		}
	}

	if (bLockPressed)
	{
		LockIn(LocalState);
	}
}

void FTraceLoadoutSelect::MoveTab(int32 Delta)
{
	const int32 Count = static_cast<int32>(ETraceLoadoutSlot::Count);
	Tab = ((Tab + Delta) % Count + Count) % Count;
	bLockHint = false;
}

void FTraceLoadoutSelect::MoveCard(int32 DeltaX, int32 DeltaY)
{
	const int32 Count = TraceLoadoutSelectFile::CardCount(static_cast<ETraceLoadoutSlot>(Tab));
	if (Count <= 0)
	{
		return;
	}

	int32& Index = Highlighted[Tab];
	Index = FMath::Clamp(Index, 0, Count - 1);

	if (DeltaX != 0)
	{
		// Across walks reading order and wraps, so right from the end of a row is the next row.
		Index = ((Index + DeltaX) % Count + Count) % Count;
	}
	else if (DeltaY != 0)
	{
		// Up and down STAY IN THE COLUMN. They used to add a row's worth modulo the card count, so on
		// the nine-card MOVEMENT tab Up from card 2 landed on card 6 — a diagonal jump.
		constexpr int32 GridColumns = TraceLoadoutLayout::Columns;
		const int32 Column = Index % GridColumns;
		const int32 RowsInColumn = (Count - Column + GridColumns - 1) / GridColumns;
		if (RowsInColumn > 1)
		{
			const int32 Row = ((Index / GridColumns + DeltaY) % RowsInColumn + RowsInColumn) % RowsInColumn;
			Index = Row * GridColumns + Column;
		}
	}

	bLockHint = false;
}

void FTraceLoadoutSelect::EquipHighlighted()
{
	const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Tab);
	const ETraceAbilityId Id = TraceLoadoutSelectFile::AbilityAtCard(Slot, Highlighted[Tab]);
	if (Staged.Get(Slot) != Id)
	{
		Staged.Set(Slot, Id);
		bStagedDirty = true;
	}
}

void FTraceLoadoutSelect::LockIn(ATracePlayerState* LocalState)
{
	if (IsLibraryOpen())
	{
		Store(LibrarySlot);
		CloseLibrary();
		return;
	}
	Confirm(LocalState);
}

// =================================================================================================
// Input — the pointer
// =================================================================================================

void FTraceLoadoutSelect::PollPointer(APlayerController* PC, ATracePlayerState* LocalState, bool bAct)
{
	float MouseX = 0.f;
	float MouseY = 0.f;
	const bool bSampled = PC->GetMousePosition(MouseX, MouseY);
	StepPointer(bSampled, FVector2D(MouseX, MouseY), PC->IsInputKeyDown(EKeys::LeftMouseButton),
		PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift), LocalState, bAct);
}

void FTraceLoadoutSelect::StepPointer(bool bSampled, const FVector2D& SamplePos, bool bButtonDown, bool bShiftHeld,
	ATracePlayerState* LocalState, bool bAct)
{
	// SAMPLED EVERY FRAME, like the keys: the position stays current under the pause menu, so its
	// first sample after the menu closes is not a "move" that drags the highlight.
	bool bCursorMoved = false;
	if (bSampled)
	{
		bCursorMoved = bHasCursor && FVector2D::DistSquared(SamplePos, CursorPos) > 4.f;   // 2 px
		CursorPos = SamplePos;
		bHasCursor = true;
	}

	const bool bPressedNow = bButtonDown && !bMouseWasDown;
	const bool bJustReleased = !bButtonDown && bMouseWasDown;
	bMouseWasDown = bButtonDown;
	bPointerShift = bShiftHeld;

	if (!bAct)
	{
		// A press that began under the pause menu, or on the frame the page opened, is not a click.
		bMouseArmed = false;
		HoveredCard = INDEX_NONE;
		HoveredTab = INDEX_NONE;
		HoveredSaved = INDEX_NONE;
		bHoveredConfirm = false;
		bHoveredBack = false;
		return;
	}

	if (bPressedNow)
	{
		bMouseArmed = true;
	}

	if (!bHasCursor)
	{
		return;
	}

	HoveredCard = INDEX_NONE;
	for (int32 Index = 0; Index < TraceLoadoutSelectFile::CardCount(static_cast<ETraceLoadoutSlot>(Tab)); ++Index)
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
	bHoveredBack = BackRect.bIsValid && BackRect.IsInside(CursorPos);

	// *** THE POINTER MUST HAVE MOVED BEFORE IT MAY TAKE THE HIGHLIGHT. *** Otherwise a pointer left
	// resting over the grid pins the highlight and neither the keys nor the pad can shift it. A PRESS
	// takes it too, moved or not, so the card that shows PRESSED is the one the release equips.
	if (bCursorMoved || bPressedNow)
	{
		bPointerLed = true;   // the pointer leads the hover look until a key is pressed (ResolveLit)
		if (HoveredCard != INDEX_NONE)
		{
			Highlighted[Tab] = HoveredCard;
			bLockHint = false;
		}
	}

	if (!bJustReleased || !bMouseArmed)
	{
		return;
	}
	bMouseArmed = false;

	// ---- a click ---------------------------------------------------------------------------------
	if (HoveredTab != INDEX_NONE)
	{
		Tab = HoveredTab;
		bLockHint = false;
		return;
	}

	if (HoveredCard != INDEX_NONE)
	{
		// A click equips and stays: the pointer can see and click the tabs, and a tab that changed
		// under it would turn a double-click into two slots.
		Highlighted[Tab] = HoveredCard;
		EquipHighlighted();
		return;
	}

	if (HoveredSaved != INDEX_NONE)
	{
		// A plain click RECALLS; shift-click stores. Same pairing as the number keys.
		bShiftHeld ? Store(HoveredSaved) : Recall(HoveredSaved);
		return;
	}

	if (bHoveredBack && IsLibraryOpen())
	{
		UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen] library editor: BACK clicked, nothing saved."));
		CloseLibrary();
		return;
	}

	if (bHoveredConfirm)
	{
		LockIn(LocalState);
	}
}

FTraceLoadoutLit FTraceLoadoutSelect::ResolveLit() const
{
	// ONE HOVER ON THE PAGE. The first draw of this page lit up to three things at once: the tab you
	// were on (always the hover plate), a tab under the pointer (the PRESSED plate, which is the hover
	// plate at 0.72 with the same olive word, so it read as a second hover), and the keys' card, which
	// kept its glow while the pointer sat on LOCK IN. Now exactly one thing wears the look.
	FTraceLoadoutLit Out;

	// 1. THE POINTER'S TARGET, while the pointer is what the player used last. A saved slot is a
	//    target when a click there does something: a usable slot always (a click loads it), and any
	//    slot while SHIFT is held (a shift-click saves there, empty or not). Pointing at a slot a click
	//    cannot use leaves the keys' card lit, as a non-selectable row does on the options page.
	if (bPointerLed)
	{
		if (HoveredTab != INDEX_NONE)
		{
			Out.Tab = HoveredTab;
		}
		else if (bHoveredConfirm)
		{
			Out.bConfirm = true;
		}
		else if (bHoveredBack)
		{
			Out.bBack = true;
		}
		else if (HoveredSaved != INDEX_NONE
			&& (bPointerShift
				|| TraceLoadoutSelectFile::SavedUsable(UTraceUserSettings::Get().GetSavedLoadout(HoveredSaved))))
		{
			Out.Saved = HoveredSaved;
		}
	}

	// 2. OTHERWISE THE KEYS' FOCUS: LOCK IN once ENTER has answered the last question, else the
	//    highlighted card (a pointer that walked onto a card moved the highlight there).
	if (Out.Count() == 0)
	{
		if (bLockHint)
		{
			Out.bConfirm = true;
		}
		else
		{
			const int32 CardIndex = Highlighted[Tab];
			if (CardIndex >= 0 && CardIndex < TraceLoadoutSelectFile::CardCount(static_cast<ETraceLoadoutSlot>(Tab)))
			{
				Out.Card = CardIndex;
			}
		}
	}

	// 3. PRESSED is what the kit means by it: the button held on the lit thing, the press begun here.
	if (bMouseArmed && bMouseWasDown)
	{
		Out.bPressed = (Out.Tab != INDEX_NONE && Out.Tab == HoveredTab)
			|| (Out.Card != INDEX_NONE && Out.Card == HoveredCard)
			|| (Out.Saved != INDEX_NONE && Out.Saved == HoveredSaved)
			|| (Out.bConfirm && bHoveredConfirm)
			|| (Out.bBack && bHoveredBack);
	}
	return Out;
}

// =================================================================================================
// Sending, and the saved library
// =================================================================================================

void FTraceLoadoutSelect::SetMessage(const FString& Text, bool bWarning)
{
	LastMessage = Text;
	LastMessageTime = Now;
	bMessageIsWarning = bWarning;
}

void FTraceLoadoutSelect::Confirm(ATracePlayerState* LocalState)
{
	if (LocalState == nullptr)
	{
		return;
	}

	// *** ONE LOCK IN ON THE WIRE AT A TIME. *** A second send before the first is answered (a double
	// tap inside one server frame, or F pressed in the last second as the deadline's auto-send fires)
	// reset the refusal baseline below. The server accepted the first, which closed its window, and
	// refused the second as LOCKED; the page took that refusal for the answer to the lock-in it had made
	// and faded out on LOADOUT LOCKED over a loadout that had been applied. The request in flight
	// already carries the player's intent, so a press while it is out is simply ignored: the window
	// closing says LOCKED IN, and a refusal (which clears bLockSent) lets the next press through.
	if (bLockSent)
	{
		UE_LOG(LogTraceGame, Log, TEXT("[LoadoutScreen] LOCK IN already sent; waiting for the server's answer."));
		return;
	}

	UTraceAbilityComponent* Comp = LocalState->FindComponentByClass<UTraceAbilityComponent>();
	if (Comp == nullptr)
	{
		return;
	}

	// ASKED LOCALLY FIRST so the common refusal needs no round trip — and SENT ANYWAY, because this
	// screen is not the authority.
	FString Reason;
	if (!UTraceAbilityComponent::IsLoadoutLegal(Staged, &Reason))
	{
		// The reason is built from enum names ("STICKYGLOVES IS A MOVEMENT ABILITY..."): a log line,
		// not something to put in front of a player.
		UE_LOG(LogTraceGame, Warning, TEXT("[LoadoutScreen] not sending %s: %s"), *TraceLoadoutToString(Staged), *Reason);
		SetMessage(TRACE_TEXT("LOADOUT.CANT_LOCK_IN", "CAN'T LOCK IN THAT LOADOUT"), /*bWarning=*/true);
		return;
	}

	// NOT "LOCKED IN" YET. That is the server's answer, and it used to be claimed the moment the
	// request left: a refusal (the lock, a window the deadline shut mid-send) then left the page up
	// saying LOCKED IN. SENDING until the window closes on what was sent (Tick says LOCKED IN) or a
	// refusal arrives (ReadLockInReply says why).
	RefusalsSeen = Comp->GetLockInRefusalCount();
	SentLoadout = Staged;
	bLockSent = true;
	Comp->ServerRequestSetLoadout(Staged);
	SetMessage(TRACE_TEXT("LOADOUT.SENDING", "SENDING"), /*bWarning=*/false);

	UE_LOG(LogTraceGame, Log, TEXT("[LoadoutScreen] sent %s"), *TraceLoadoutToString(Staged));

	// On a listen server or offline the server has already answered, inside the call above.
	ReadLockInReply(LocalState);
}

void FTraceLoadoutSelect::ReadLockInReply(const ATracePlayerState* LocalState)
{
	if (!bLockSent || LocalState == nullptr)
	{
		return;
	}
	const UTraceAbilityComponent* Comp = LocalState->FindComponentByClass<UTraceAbilityComponent>();
	if (Comp == nullptr || Comp->GetLockInRefusalCount() == RefusalsSeen)
	{
		return;
	}

	// REFUSED. The page stays as it is — open, the picks staged, every key live — and says why in
	// the words the player can act on: the lock, or a loadout the server will not take.
	bLockSent = false;
	RefusalsSeen = Comp->GetLockInRefusalCount();
	const bool bLocked = (Comp->GetLastLockInRefusal() == ETraceLockInRefusal::Locked);
	SetMessage(bLocked ? TRACE_TEXT("LOADOUT.REFUSED_LOCKED", "LOADOUT LOCKED")
	                   : TRACE_TEXT("LOADOUT.CANT_LOCK_IN", "CAN'T LOCK IN THAT LOADOUT"), /*bWarning=*/true);
	UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen] LOCK IN refused (%s); the page stays up."),
		bLocked ? TEXT("locked") : TEXT("not a legal loadout"));
}

void FTraceLoadoutSelect::Recall(int32 Index)
{
	const FTraceLoadout Saved = UTraceUserSettings::Get().GetSavedLoadout(Index);

	// AN EMPTY SLOT IS NOT A LOADOUT. Recalling one would wipe the cards the player just chose.
	if (Saved.IsEmpty())
	{
		SetMessage(FString::Format(*TRACE_TEXT("LOADOUT.SLOT_EMPTY", "SLOT {0} IS EMPTY"),
			FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(Index + 1)) }), /*bWarning=*/true);
		return;
	}

	// NOR IS ONE THE RULES NO LONGER ALLOW. A slot saved before Demo 35 moved BASH and ACROBATICS
	// between columns would stage fine and then be refused at LOCK IN with enum names for a reason.
	FString Reason;
	if (!UTraceAbilityComponent::IsLoadoutLegal(Saved, &Reason))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[LoadoutScreen] saved slot %d is not a legal loadout any more: %s"),
			Index + 1, *Reason);
		SetMessage(TRACE_TEXTF("LOADOUT.SLOT_UNUSABLE", "CAN'T LOAD {0}", { Index + 1 }), /*bWarning=*/true);
		return;
	}

	Staged = Saved;
	bStagedDirty = true;
	LastSavedSlot = Index;
	for (int32 SlotIndex = 0; SlotIndex < static_cast<int32>(ETraceLoadoutSlot::Count); ++SlotIndex)
	{
		Highlighted[SlotIndex] = TraceLoadoutSelectFile::CardForAbility(static_cast<ETraceLoadoutSlot>(SlotIndex),
			Staged.Get(static_cast<ETraceLoadoutSlot>(SlotIndex)));
	}

	SetMessage(FString::Format(*TRACE_TEXT("LOADOUT.SLOT_LOADED", "LOADED {0}"),
		FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(Index + 1)) }), /*bWarning=*/false);
}

void FTraceLoadoutSelect::RecallNext()
{
	// In slot order, starting after the slot last loaded or saved on this opening (slot 1 first), and
	// round again: with one usable slot, Y reloads it. Empty and no-longer-legal slots are passed over
	// — their plates are the disabled ones.
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();
	const int32 Count = UTraceUserSettings::SavedLoadoutCount;
	const int32 After = (LastSavedSlot != INDEX_NONE) ? LastSavedSlot : (Count - 1);
	for (int32 Step = 1; Step <= Count; ++Step)
	{
		const int32 Candidate = (After + Step) % Count;
		if (TraceLoadoutSelectFile::SavedUsable(Settings.GetSavedLoadout(Candidate)))
		{
			Recall(Candidate);
			return;
		}
	}
	SetMessage(TRACE_TEXT("LOADOUT.NOTHING_SAVED", "NOTHING SAVED"), /*bWarning=*/true);
}

void FTraceLoadoutSelect::StoreCurrent()
{
	// The slot this opening last loaded or saved (load 2, change a card, save: it goes back to 2);
	// otherwise the first empty slot, so a pad's first save never overwrites anything. With every
	// slot full and none loaded, it says so rather than guessing which one to overwrite.
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();
	int32 Target = LastSavedSlot;
	for (int32 Index = 0; Target == INDEX_NONE && Index < UTraceUserSettings::SavedLoadoutCount; ++Index)
	{
		if (Settings.GetSavedLoadout(Index).IsEmpty())
		{
			Target = Index;
		}
	}
	if (Target == INDEX_NONE)
	{
		SetMessage(TRACE_TEXT("LOADOUT.NO_EMPTY_SLOT", "NO EMPTY SLOT"), /*bWarning=*/true);
		return;
	}
	Store(Target);
}

void FTraceLoadoutSelect::Store(int32 Index)
{
	UTraceUserSettings::Get().SetSavedLoadout(Index, Staged);
	LastSavedSlot = Index;
	SetMessage(FString::Format(*TRACE_TEXT("LOADOUT.SLOT_SAVED", "SAVED TO {0}"),
		FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(Index + 1)) }), /*bWarning=*/false);

	UE_LOG(LogTraceGame, Log, TEXT("[LoadoutScreen] saved %s to slot %d"),
		*TraceLoadoutToString(Staged), Index + 1);
}

// =================================================================================================
// Library mode
// =================================================================================================

void FTraceLoadoutSelect::OpenLibrary(int32 SlotIndex)
{
	LibrarySlot = FMath::Clamp(SlotIndex, 0, UTraceUserSettings::SavedLoadoutCount - 1);

	// Everything the in-match page resets on its open edge, and for the same reasons: the ENTER or A
	// that opened this editor is still down, and last edit's pointer sample is stale.
	OnOpened(/*LocalState=*/nullptr);

	Staged = UTraceUserSettings::Get().GetSavedLoadout(LibrarySlot);
	if (Staged.IsEmpty())
	{
		for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
		{
			const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Index);
			Staged.Set(Slot, TraceLoadoutSelectFile::AbilityAtCard(Slot, 0));
		}
	}
	for (int32 Index = 0; Index < static_cast<int32>(ETraceLoadoutSlot::Count); ++Index)
	{
		Highlighted[Index] = TraceLoadoutSelectFile::CardForAbility(static_cast<ETraceLoadoutSlot>(Index),
			Staged.Get(static_cast<ETraceLoadoutSlot>(Index)));
	}
	Tab = 0;
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
	AnimNow = InNow;   // the options overlay's clock, which is already real time

	// The editor is the front-most surface wherever it is hosted; its bInputAllowed is only the
	// host's one-frame debounce, and a pointer that vanished for it would flicker on every open.
	bPointerOwned = true;

	const bool bAct = bInputAllowed && PC != nullptr && GFrameCounter >= IgnoreInputBeforeFrame;
	if (PC != nullptr)
	{
		FTraceLoadoutKeys Down = ReadKeys(PC);
#if !UE_BUILD_SHIPPING
		if (bAct)
		{
			TraceLoadoutPressQueue::Apply(PC, Down, Now);
		}
#endif
		StepInput(Down, /*LocalState=*/nullptr, bAct);   // no player state: cannot reach a server
		if (LibrarySlot != INDEX_NONE)
		{
			PollPointer(PC, /*LocalState=*/nullptr, bAct);
		}
	}

	if (LibrarySlot == INDEX_NONE)
	{
		return false;   // SAVE or BACK inside the input above closed us
	}

	const FString Title = FString::Format(*TRACE_TEXT("LOADOUT.LIBRARY_TITLE", "LOADOUT {0}"),
		FStringFormatOrderedArguments{ FStringFormatArg(FString::FromInt(LibrarySlot + 1)) });
	Draw(HUD, PC, /*LocalState=*/nullptr, Title);
	return true;
}

// =================================================================================================
// Drawing — the handmade kit (UI/Widgets/Menu/TraceMenuKit.h), on opaque black
// =================================================================================================

void FTraceLoadoutSelect::Draw(AHUD* HUD, APlayerController* PC, const ATracePlayerState* LocalState,
	const FString& Title)
{
	if (HUD == nullptr || ViewW <= 0.f || ViewH <= 0.f)
	{
		return;
	}

	const float S = UIScale;
	const float X = TraceLoadoutLayout::Margin * S;
	const float W = ViewW - TraceLoadoutLayout::Margin * 2.f * S;

	// THIS PAGE'S OWN HOVER EASES. The pause menu's LOADOUTS editor is a second one of these, drawn over
	// the match page in the same frame with the same layout; keyed by rect alone the two shared every
	// card's hover blend, and the editor's ring followed the page's highlight beneath it.
	TraceMenuKit::FScopedHoverSalt HoverScope(HoverSalt());

	// The one thing in the hover look this frame, asked once so the tabs, the grid and the buttons agree.
	FrameLit = ResolveLit();

	// OPAQUE BLACK (stylespec §1). The old translucent navy scrim let the match clock, the CORE LOOSE
	// banner and the first-person weapon ghost through behind the title and the cards; the countdown
	// the clock stood in for is on this page now.
	TraceMenuKit::DrawBackground(HUD, ViewW, ViewH);

	DrawHeader(HUD, LocalState, Title);
	DrawTabs(HUD, PC, X, TraceLoadoutLayout::TabY * S, W);
	DrawGrid(HUD, X, TraceLoadoutLayout::GridTop * S);
	DrawActionRow(HUD, PC, X, TraceLoadoutLayout::ActionY * S, W);

	if (!LastMessage.IsEmpty() && (Now - LastMessageTime) < TraceLoadoutLayout::MessageSeconds)
	{
		TraceText::FStyle Style(TraceLoadoutLayout::SizeMessage * S,
			bMessageIsWarning ? TraceMenuArtStyle::AmberLifted() : TraceMenuArtStyle::WordDefault,
			ETraceTextWeight::Light);
		Style.HAlign = TraceText::EHAlign::Center;
		TraceCanvasText::Draw(HUD, LastMessage, ViewW * 0.5f, TraceLoadoutLayout::MessageY * S, Style);
	}

	DrawFooter(HUD, PC, TraceLoadoutLayout::FooterY * S);
	DrawPointer(HUD, PC);
}

void FTraceLoadoutSelect::DrawHeader(AHUD* HUD, const ATracePlayerState* LocalState, const FString& Title)
{
	const float S = UIScale;
	const float CapMid = TraceLoadoutLayout::TitleCapMid * S;
	const float BoxH = TraceLoadoutLayout::CountBoxH * S;
	const float BoxW = BoxH * (TraceMenuArtStyle::ValueFrame.PlateW / TraceMenuArtStyle::ValueFrame.PlateH);
	const bool bMatchHeader = (LocalState != nullptr) && !IsLibraryOpen();

	// The furniture either side of the title, as drawn below: the title is kept clear of both.
	float FurnitureLeft = TraceLoadoutLayout::Margin * S;
	float FurnitureRight = ViewW - TraceLoadoutLayout::Margin * S;

	// ---- THE BREAK: THE SCORE, AND SIDES SWITCHED ----------------------------------------------
	//
	// The left end mirrors TIME on the right: the score in the kit's value box, blue then orange in
	// their team colours (the results screen's order), and the HUD card's SIDES SWITCHED after it —
	// what the HALF TIME card under this page would have said.
	const ATraceGameState* const HalfGS = (bMatchHeader && LocalState->GetWorld() != nullptr)
		? LocalState->GetWorld()->GetGameState<ATraceGameState>() : nullptr;
	if (bMatchHeader && bHalfTimeHeader && HalfGS != nullptr)
	{
		const FString BlueText = FString::FromInt(HalfGS->GetScore(ETraceTeam::Blue));
		const FString OrangeText = FString::FromInt(HalfGS->GetScore(ETraceTeam::Orange));
		const FString Dash = TRACE_TEXT("HUD.RESULT_SCORE_SEPARATOR", "-");
		const float CapH = BoxH * TraceMenuKit::LabelCapFraction;
		const float Spread = CapH * 0.9f;   // from the box's centre to the inner edge of each number
		const float ScoreW = FMath::Max(BoxW, 2.f * (Spread + TraceMenuKit::CapTextWidth(TEXT("00"), CapH))
			+ BoxH * TraceMenuKit::LabelPadFraction * 2.f);
		const float BoxLeft = TraceLoadoutLayout::Margin * S;
		const float BoxMid = BoxLeft + ScoreW * 0.5f;

		TraceMenuKit::DrawValueBoxPlate(HUD, BoxLeft, CapMid - BoxH * 0.5f, ScoreW, BoxH);
		TraceMenuKit::DrawCapText(HUD, BlueText, BoxMid - Spread, CapMid, CapH, TraceTeamColor(ETraceTeam::Blue),
			ETraceTextWeight::Light, TraceText::EHAlign::Right);
		TraceMenuKit::DrawCapText(HUD, Dash, BoxMid, CapMid, CapH, TraceMenuArtStyle::WordDefault,
			ETraceTextWeight::Light, TraceText::EHAlign::Center);
		TraceMenuKit::DrawCapText(HUD, OrangeText, BoxMid + Spread, CapMid, CapH, TraceTeamColor(ETraceTeam::Orange),
			ETraceTextWeight::Light, TraceText::EHAlign::Left);
		FurnitureLeft = BoxLeft + ScoreW;

		const FString& Switched = TRACE_TEXT("HUD.BANNER_SIDES_SWITCHED_SHORT", "SIDES SWITCHED");
		if (!Switched.IsEmpty())
		{
			TraceText::FStyle NoteStyle(TraceLoadoutLayout::SizeTimer * S, TraceMenuKit::FurnitureUnselected,
				ETraceTextWeight::Light);
			NoteStyle.HAlign = TraceText::EHAlign::Left;
			const float NoteX = BoxLeft + ScoreW + 14.f * S;
			FurnitureLeft = NoteX + TraceMenuKit::DrawTextCapCentered(HUD, Switched, NoteX, CapMid, NoteStyle);
		}
	}

	// ---- THE COUNTDOWN ------------------------------------------------------------------------
	//
	// The server closes this window on a deadline, and the page used to show no clock at all: the
	// match clock was the only one, under the scrim and counting something else. The kit draws it
	// (DrawPageClock), because the pause menu draws the same clock in the same place over this page.
	if (bMatchHeader && LocalState->CharacterSelectDeadlineServerTime > 0.f)
	{
		FurnitureRight = TraceMenuKit::DrawPageClock(HUD, ViewW, S, LocalState->GetCharacterSelectTimeRemaining(), AnimNow);
	}

	// ---- THE TITLE, centred, set like every screen title (TraceLoadoutLayout::TitleCap) ---------
	//
	// Its width limit is the room the furniture leaves either side of the centre, so a long title
	// shrinks rather than running into the score or the countdown on a narrow window.
	const float CentreX = ViewW * 0.5f;
	const float HalfRoom = FMath::Min(CentreX - FurnitureLeft, FurnitureRight - CentreX) - 24.f * S;
	TraceMenuKit::DrawCapText(HUD, Title, CentreX, CapMid, TraceLoadoutLayout::TitleCap * S,
		TraceMenuArtStyle::WordDefault, ETraceTextWeight::Light, TraceText::EHAlign::Center, FMath::Max(1.f, 2.f * HalfRoom));
#if !UE_BUILD_SHIPPING
	DebugTitleCapPx = TraceLoadoutLayout::TitleCap;
	DebugTitleCapMidPx = TraceLoadoutLayout::TitleCapMid;
#endif
}

void FTraceLoadoutSelect::DrawTabs(AHUD* HUD, APlayerController* PC, float X, float Y, float W)
{
	const float S = UIScale;
	const float TabH = TraceLoadoutLayout::TabH * S;
	const float Gap = TraceLoadoutLayout::TabGap * S;

	// THE TAB KEYS, AT THE TWO ENDS OF THE ROW. Nothing on the page ever said Q and E changed tab, so
	// keyboard players only found the other two questions by clicking. Q / E always, and the pad's
	// shoulders beside them while pad hints show (ChipKeys).
	const bool bPadHints = TraceLoadoutSelectFile::PadHintsShown(PC);
	const TraceLoadoutSelectFile::FChipKeys LeftKeys =
		TraceLoadoutSelectFile::ChipKeys(TraceLoadoutSelectFile::EChip::TabLeft, IsLibraryOpen(), bPadHints);
	const TraceLoadoutSelectFile::FChipKeys RightKeys =
		TraceLoadoutSelectFile::ChipKeys(TraceLoadoutSelectFile::EChip::TabRight, IsLibraryOpen(), bPadHints);

	const float ChipH = TraceLoadoutLayout::TabChipH * S;
	const float ChipY = Y + (TabH - ChipH) * 0.5f;
	const float PairGap = TraceLoadoutSelectFile::ChipPairGap * S;
	float RowX = X;
	float RowW = W;

	const float LeftW = TraceLoadoutSelectFile::ChipGroupWidth(LeftKeys, ChipH, PairGap);
	if (LeftW > 0.f)
	{
		TraceLoadoutSelectFile::DrawChipGroup(HUD, X, ChipY, ChipH, PairGap, LeftKeys, AnimNow);
		RowX += LeftW + Gap;
		RowW -= LeftW + Gap;
	}
	const float RightW = TraceLoadoutSelectFile::ChipGroupWidth(RightKeys, ChipH, PairGap);
	if (RightW > 0.f)
	{
		TraceLoadoutSelectFile::DrawChipGroup(HUD, X + W - RightW, ChipY, ChipH, PairGap, RightKeys, AnimNow);
		RowW -= RightW + Gap;
	}

	const int32 Count = static_cast<int32>(ETraceLoadoutSlot::Count);
	const float TabW = (RowW - Gap * (Count - 1)) / static_cast<float>(Count);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Index);
		const float TabX = RowX + (TabW + Gap) * Index;
		TabRects[Index] = FBox2D(FVector2D(TabX, Y), FVector2D(TabX + TabW, Y + TabH));

		// A TAB WEARS THE HOVER PLATE ONLY WHILE THE POINTER IS ON IT (PRESSED while the button is
		// down on it). The question you are ON is not a hover — it used to wear the hover plate for as
		// long as it was current, beside the keys' lit card — so it keeps the default plate and says
		// which it is with the olive word and white summary.
		const bool bLitTab = (Index == FrameLit.Tab);
		const ETraceKitState State = TraceMenuKit::StateFor(/*bEnabled=*/true, bLitTab, bLitTab && FrameLit.bPressed);
		TraceMenuKit::DrawStatePlate(HUD, State, TabX, Y, TabW, TabH, AnimNow);
		FTraceKitVisuals Visuals = TraceMenuKit::VisualsAt(State, TabX, Y, TabW, TabH);
		if (Index == Tab && !bLitTab)
		{
			Visuals.Label = TraceMenuArtStyle::WordHoverLifted();
			Visuals.Furniture = TraceMenuKit::FurnitureSelected;
		}

		// WHAT IS IN THE SLOT, on the tab itself, so the whole loadout reads without visiting all three.
		const float TextMax = TabW - TabH * 0.5f;
		TraceMenuKit::DrawLabel(HUD, TraceLoadoutSelectFile::SlotHeading(Slot), TabX + TabW * 0.5f,
			Y + TabH * 0.36f, TraceLoadoutLayout::TabHeadingPlate * S, Visuals.Label, TextMax);
		TraceMenuKit::DrawLabel(HUD, TraceLoadoutSelectFile::TabSummary(Staged, Slot), TabX + TabW * 0.5f,
			Y + TabH * 0.72f, TraceLoadoutLayout::TabSummaryPlate * S, Visuals.Furniture, TextMax);
	}
}

void FTraceLoadoutSelect::DrawGrid(AHUD* HUD, float X, float Y)
{
	const float S = UIScale;
	const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Tab);
	const int32 Count = TraceLoadoutSelectFile::CardCount(Slot);

	constexpr int32 GridColumns = TraceLoadoutLayout::Columns;
	const float GapX = TraceLoadoutLayout::TileGapX * S;
	const float GapY = TraceLoadoutLayout::TileGapY * S;
	float TileW = 0.f;
	float TileH = 0.f;
	GridTile(TileW, TileH);

	for (FBox2D& Rect : CardRects)
	{
		Rect = FBox2D(ForceInit);
	}

	// ONE NAME SIZE AND ONE DESCRIPTION SIZE FOR THE WHOLE SCREEN, every tab (SolveTypeSizes).
	SolveTypeSizes(TileW, TileH);

	// THE HIGHLIGHTED CARD LAST: its glow overhangs the plate and a neighbour drawn after it would
	// paint over the ring.
	const int32 LastCard = Highlighted[Tab];
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if ((Index == LastCard) != (Pass == 1))
			{
				continue;
			}
			DrawCard(HUD, Index,
				X + (TileW + GapX) * (Index % GridColumns),
				Y + (TileH + GapY) * (Index / GridColumns),
				TileW, TileH, NameFitSize, BodyFitSize, BodyFitRoom);
		}
	}
}

void FTraceLoadoutSelect::GridTile(float& OutTileW, float& OutTileH) const
{
	// The grid's own box, from the layout: the page's width inside its margins, GridTop to GridBottom.
	const float S = UIScale;
	const float GridW = ViewW - TraceLoadoutLayout::Margin * 2.f * S;
	const float GridH = (TraceLoadoutLayout::GridBottom - TraceLoadoutLayout::GridTop) * S;
	OutTileW = (GridW - TraceLoadoutLayout::TileGapX * S * (TraceLoadoutLayout::Columns - 1))
		/ static_cast<float>(TraceLoadoutLayout::Columns);
	OutTileH = (GridH - TraceLoadoutLayout::TileGapY * S * (TraceLoadoutLayout::Rows - 1))
		/ static_cast<float>(TraceLoadoutLayout::Rows);
}

void FTraceLoadoutSelect::SolveTypeSizes(float TileW, float TileH)
{
	const float S = UIScale;
	const ETraceTextWeight BodyWeight = TraceLoadoutSelectFile::DescriptionWeight();
	if (NameFitSize > 0.f && FitTileW == TileW && FitTileH == TileH && FitWeight == BodyWeight)
	{
		return;   // solved for this layout and face already
	}
	FitTileW = TileW;
	FitTileH = TileH;
	FitWeight = BodyWeight;

	// *** OVER EVERY CARD OF EVERY TAB. *** This was solved per tab, so the same card drew at three
	// type sizes on one screen and a tab switch visibly rescaled the grid (names 13 / 12 / 18 px caps,
	// descriptions 14 / 10 / 11 on MOVEMENT / PASSIVE / ACTIVATED). The page is one screen; its cards
	// are one set.
	const float TextW = TileW - TraceLoadoutLayout::CardPad * 2.f * S;
	const int32 SlotCount = static_cast<int32>(ETraceLoadoutSlot::Count);

	// NAMES: the smallest size any name on any tab needs to fit its card.
	const float Full = TraceLoadoutLayout::SizeName * S;
	const TraceText::FStyle NameStyle(Full, FLinearColor::White, ETraceTextWeight::Light);
	float Fitted = Full;
	for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
	{
		const ETraceLoadoutSlot EachSlot = static_cast<ETraceLoadoutSlot>(SlotIndex);
		for (int32 Index = 0; Index < TraceLoadoutSelectFile::CardCount(EachSlot); ++Index)
		{
			const FString Name = TraceAbilityNames::Get(TraceLoadoutSelectFile::AbilityAtCard(EachSlot, Index));
			const float Measured = Name.IsEmpty() ? 0.f : TraceText::MeasureWidth(Name, NameStyle);
			if (Measured > TextW)
			{
				Fitted = FMath::Min(Fitted, Full * (TextW / Measured));
			}
		}
	}
	NameFitSize = Fitted;

	// DESCRIPTIONS: the largest size at which every card on every tab fits above the EQUIPPED box
	// (reserved on every card, so the marker can never cover a line). Down to SizeBodyLast, the size a
	// single card used to shrink to on its own, so no card is left drawing at a size of its own.
	const float Room = TileH - TraceLoadoutLayout::CardPad * 2.f * S
		- (TraceText::LineHeight(TraceLoadoutLayout::SizeName * S) + 4.f * S)
		- (TraceLoadoutLayout::EquippedH + 6.f) * S;
	float BodySize = TraceLoadoutLayout::SizeBody * S;
	const float SizeFloor = TraceLoadoutLayout::SizeBodyLast * S;
	TArray<FString> Lines;
	while (BodySize > SizeFloor)
	{
		bool bAllFit = true;
		const TraceText::FStyle TryStyle(BodySize, FLinearColor::White, BodyWeight);
		for (int32 SlotIndex = 0; SlotIndex < SlotCount && bAllFit; ++SlotIndex)
		{
			const ETraceLoadoutSlot EachSlot = static_cast<ETraceLoadoutSlot>(SlotIndex);
			for (int32 Index = 0; Index < TraceLoadoutSelectFile::CardCount(EachSlot) && bAllFit; ++Index)
			{
				TraceLoadoutSelectFile::WrapInto(
					TraceAbilityNames::Describe(TraceLoadoutSelectFile::AbilityAtCard(EachSlot, Index)), TextW, TryStyle, Lines);
				bAllFit = TraceLoadoutSelectFile::BlockHeight(Lines.Num(), BodySize, S) <= Room;
			}
		}
		if (bAllFit)
		{
			break;
		}
		BodySize = FMath::Max(SizeFloor, BodySize - 0.5f * S);
	}
	BodyFitSize = BodySize;
	BodyFitRoom = Room;

	UE_LOG(LogTraceGame, Display, TEXT("[LoadoutScreen] one type size for every tab, %.0f x %.0f tile: names %.1f, descriptions %.1f."),
		TileW, TileH, NameFitSize, BodyFitSize);
}

void FTraceLoadoutSelect::DrawCard(AHUD* HUD, int32 Index, float X, float Y, float W, float H, float NameSize,
	float TabBodySize, float BodyRoom)
{
	const float S = UIScale;
	const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(Tab);
	const ETraceAbilityId Id = TraceLoadoutSelectFile::AbilityAtCard(Slot, Index);

	// Lit only when the page's one hover is on the grid: not while the pointer leads on a tab, LOCK
	// IN or a saved slot, and not while ENTER has handed the look to LOCK IN (ResolveLit).
	const bool bLitCard = (Index == FrameLit.Card);
	const bool bEquipped = (Staged.Get(Slot) == Id);

	CardRects[Index] = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));

	const ETraceKitState State = TraceMenuKit::StateFor(/*bEnabled=*/true, bLitCard, bLitCard && FrameLit.bPressed);
	TraceMenuKit::DrawStatePlate(HUD, State, X, Y, W, H, AnimNow, TraceLoadoutLayout::CardCorner * S);
	// The card's words follow its hover ring as it eases on and off.
	const FTraceKitVisuals Visuals = TraceMenuKit::VisualsAt(State, X, Y, W, H);

	const float Pad = TraceLoadoutLayout::CardPad * S;
	const float TextX = X + Pad;
	const float TextW = W - Pad * 2.f;
	float TextY = Y + Pad;

	// THE NAME, WHERE THERE IS ONE. Three abilities are deliberately unnamed; their card is the
	// description alone.
	const FString Name = TraceAbilityNames::Get(Id);
	if (!Name.IsEmpty())
	{
		TraceCanvasText::Draw(HUD, Name, TextX, TextY,
			TraceText::FStyle(NameSize, Visuals.Label, ETraceTextWeight::Light));

		// The full size, not the fitted one, so every card's description starts on the same line.
		TextY += TraceText::LineHeight(TraceLoadoutLayout::SizeName * S) + 4.f * S;
	}

	// THE DESCRIPTION at the screen's solved size, wrapped once per ability, width, size and face rather
	// than every frame. The solve already fits every card down to SizeBodyLast, so CardBodySize only
	// goes smaller for a card that could not fit even there.
	const ETraceTextWeight BodyWeight = TraceLoadoutSelectFile::DescriptionWeight();
	FWrapCache& Cache = WrapCache[Index];
	if (Cache.Id != Id || Cache.Width != TextW || Cache.TabSize != TabBodySize || Cache.Weight != BodyWeight)
	{
		Cache.Size = TraceLoadoutSelectFile::CardBodySize(TraceAbilityNames::Describe(Id), TextW, TabBodySize,
			BodyRoom, BodyWeight, S, Cache.Lines);
		Cache.Id = Id;
		Cache.Width = TextW;
		Cache.TabSize = TabBodySize;
		Cache.Weight = BodyWeight;
	}

	const TraceText::FStyle BodyStyle(Cache.Size, Visuals.Furniture, BodyWeight);
	const float ChipH = TraceLoadoutLayout::EquippedH * S;
	const float ChipY = Y + H - Pad - ChipH;
	const float LineH = TraceText::LineHeight(Cache.Size);
	for (const FString& Line : Cache.Lines)
	{
		TraceCanvasText::Draw(HUD, Line, TextX, TextY, BodyStyle);
		TextY += LineH + TraceLoadoutLayout::BodyLeading * S;
	}

	// THE EQUIPPED MARKER is a word on the kit's value box — the gold-edged plate — rather than a
	// colour: one of these cards is the answer to the tab's question, and the box says which.
	if (bEquipped)
	{
		const FString Word = TRACE_TEXT("LOADOUT.EQUIPPED", "EQUIPPED");
		if (!Word.IsEmpty())
		{
			const float WordW = TraceText::MeasureWidth(Word,
				TraceText::FStyle(TraceMenuKit::LabelSize(ChipH), FLinearColor::White, ETraceTextWeight::Light));
			const float ChipW = FMath::Min(TextW, FMath::Max(
				ChipH * (TraceMenuArtStyle::ValueFrame.PlateW / TraceMenuArtStyle::ValueFrame.PlateH),
				WordW + ChipH * TraceMenuKit::LabelPadFraction * 2.f));
			TraceMenuKit::DrawValueBox(HUD, TextX, ChipY, ChipW, ChipH, Word, TraceMenuArtStyle::WordDefault);
		}
	}
}

void FTraceLoadoutSelect::DrawActionRow(AHUD* HUD, APlayerController* PC, float X, float Y, float W)
{
	const float S = UIScale;
	const float BtnH = TraceLoadoutLayout::ActionH * S;
	const float BtnW = TraceLoadoutLayout::ActionW * S;
	const float ChipH = TraceLoadoutLayout::ActionChipH * S;
	const float ChipY = Y + (BtnH - ChipH) * 0.5f;
	const float ChipGap = 12.f * S;
	const float GroupGap = 30.f * S;
	const float PairGap = TraceLoadoutSelectFile::ChipPairGap * S;
	const bool bPadHints = TraceLoadoutSelectFile::PadHintsShown(PC);
	const bool bLibrary = IsLibraryOpen();

	// ---- LOCK IN (SAVE), at the right edge, with its keys (F, and X beside it for a pad) ----------
	float Right = X + W;
	{
		const FString Word = bLibrary ? TRACE_TEXT("LOADOUT.SAVE_SLOT", "SAVE") : TRACE_TEXT("LOADOUT.LOCK_IN", "LOCK IN");
		const TraceLoadoutSelectFile::FChipKeys Keys =
			TraceLoadoutSelectFile::ChipKeys(TraceLoadoutSelectFile::EChip::Confirm, bLibrary, bPadHints);
		const float PlateX = Right - BtnW;
		const ETraceKitState State = TraceMenuKit::StateFor(/*bEnabled=*/true, FrameLit.bConfirm,
			FrameLit.bConfirm && FrameLit.bPressed);
		ConfirmRect = TraceMenuKit::DrawButton(HUD, State, PlateX, Y, BtnW, BtnH, Word, AnimNow)
			? FBox2D(FVector2D(PlateX, Y), FVector2D(PlateX + BtnW, Y + BtnH))
			: FBox2D(ForceInit);
		Right = PlateX;
		if (ConfirmRect.bIsValid)
		{
			const float KeysW = TraceLoadoutSelectFile::ChipGroupWidth(Keys, ChipH, PairGap);
			TraceLoadoutSelectFile::DrawChipGroup(HUD, Right - ChipGap - KeysW, ChipY, ChipH, PairGap, Keys, AnimNow);
			Right -= (KeysW > 0.f) ? (ChipGap + KeysW) : 0.f;
		}
		Right -= GroupGap;
	}

	// ---- BACK, in the library only: the pointer and the pad had no way out but SAVE ----------------
	BackRect = FBox2D(ForceInit);
	if (bLibrary)
	{
		const FString Word = TRACE_TEXT("LOADOUT.BACK", "BACK");
		const TraceLoadoutSelectFile::FChipKeys Keys =
			TraceLoadoutSelectFile::ChipKeys(TraceLoadoutSelectFile::EChip::Back, bLibrary, bPadHints);
		const float PlateX = Right - BtnW;
		const ETraceKitState State = TraceMenuKit::StateFor(/*bEnabled=*/true, FrameLit.bBack,
			FrameLit.bBack && FrameLit.bPressed);
		if (TraceMenuKit::DrawButton(HUD, State, PlateX, Y, BtnW, BtnH, Word, AnimNow))
		{
			BackRect = FBox2D(FVector2D(PlateX, Y), FVector2D(PlateX + BtnW, Y + BtnH));
			const float KeysW = TraceLoadoutSelectFile::ChipGroupWidth(Keys, ChipH, PairGap);
			TraceLoadoutSelectFile::DrawChipGroup(HUD, PlateX - ChipGap - KeysW, ChipY, ChipH, PairGap, Keys, AnimNow);
		}
		for (FBox2D& Rect : SavedRects)
		{
			Rect = FBox2D(ForceInit);
		}
		return;
	}

	DrawSavedRow(HUD, X, Y, Right - X);
}

void FTraceLoadoutSelect::DrawSavedRow(AHUD* HUD, float X, float Y, float MaxW)
{
	const float S = UIScale;
	const float H = TraceLoadoutLayout::ActionH * S;
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();

	for (FBox2D& Rect : SavedRects)
	{
		Rect = FBox2D(ForceInit);
	}

	// The label is MEASURED and the slots start after it; they used to start at a fixed offset the
	// word was wider than.
	float PenX = X;
	const FString Label = TRACE_TEXT("LOADOUT.SAVED_LABEL", "SAVED");
	if (!Label.IsEmpty())
	{
		const TraceText::FStyle Style(TraceMenuKit::LabelSize(H * 0.8f), TraceMenuKit::FurnitureUnselected,
			ETraceTextWeight::Light);
		PenX += TraceMenuKit::DrawTextCapCentered(HUD, Label, PenX, Y + H * 0.5f, Style) + 18.f * S;
	}

	const float Gap = 12.f * S;
	const int32 Count = UTraceUserSettings::SavedLoadoutCount;
	const float SlotW = FMath::Min(TraceLoadoutLayout::SavedSlotW * S,
		(X + MaxW - PenX - Gap * (Count - 1)) / static_cast<float>(Count));
	if (SlotW < H)
	{
		return;   // no room on a very narrow window; the number keys still work
	}

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float SlotX = PenX + (SlotW + Gap) * Index;
		const FTraceLoadout Saved = Settings.GetSavedLoadout(Index);

		// WHAT THE SLOT HOLDS, not just its number: its activated ability names it (or the name the
		// player gave it).
		FString Word = Settings.GetSavedLoadoutName(Index);
		if (Word.IsEmpty())
		{
			const FString Activated = Saved.IsEmpty() ? FString() : TraceAbilityNames::ShortLabel(Saved.Activated);
			Word = Activated.IsEmpty()
				? FString::FromInt(Index + 1)
				: TRACE_TEXTF("LOADOUT.SAVED_SLOT", "{0} {1}", { Index + 1, Activated });
		}
		if (Word.IsEmpty())
		{
			continue;   // an emptied name draws no plate, and leaves nothing to click (DrawButton's rule)
		}

		// EMPTY IS NOT DISABLED (SavedSlotLook): an empty slot is a live save target, so it is the
		// kit's navy plate with its number in the quiet grey, like the KEYBOARD page's "+" chip. Only a
		// slot the rules no longer allow wears the dark DISABLED plate.
		const bool bLit = (Index == FrameLit.Saved);
		const TraceLoadoutSelectFile::FSavedSlotLook Look =
			TraceLoadoutSelectFile::SavedSlotLook(Saved, bLit, bLit && FrameLit.bPressed);
		TraceMenuKit::DrawStatePlate(HUD, Look.State, SlotX, Y, SlotW, H, AnimNow);
		const FLinearColor WordColor = Look.bQuietWord ? TraceMenuArtStyle::WordDisabled
			: TraceMenuKit::VisualsAt(Look.State, SlotX, Y, SlotW, H).Label;
		TraceMenuKit::DrawLabel(HUD, Word, SlotX + SlotW * 0.5f, Y + H * 0.5f, H, WordColor,
			SlotW - H * TraceMenuKit::LabelPadFraction * 2.f);
		SavedRects[Index] = FBox2D(FVector2D(SlotX, Y), FVector2D(SlotX + SlotW, Y + H));
	}
}

void FTraceLoadoutSelect::DrawFooter(AHUD* HUD, APlayerController* PC, float Y)
{
	const float S = UIScale;

	// LABELS, NOT SENTENCES, and every key that does something on this page right now — the old line
	// promised "ARROWS TO CHOOSE" when the arrows chose nothing, and never mentioned the tab keys; the
	// one after it left out BACKSPACE / B, which step back a tab. The pad's line once a pad has been
	// seen and CONTROLLER INPUT is on (the hint is gated; the input never is on the first).
	TArray<FTraceKitLegendItem> Keyboard;
	TArray<FTraceKitLegendItem> Pad;
	TraceLoadoutSelectFile::BuildLegend(IsLibraryOpen(), Tab, TraceLoadoutSelectFile::PadHintsShown(PC), Keyboard, Pad);

	// ONE SCALE FOR BOTH LINES: two legends for two devices at two sizes would read as a mistake.
	const float FullChipH = TraceLoadoutLayout::FooterChipH * S;
	const float ChipH = TraceMenuKit::KeyLegendFit(FullChipH, ViewW - TraceLoadoutLayout::Margin * 2.f * S,
		{ TraceMenuKit::KeyLegendWidth(Keyboard, FullChipH), TraceMenuKit::KeyLegendWidth(Pad, FullChipH) });

	TraceMenuKit::DrawKeyLegend(HUD, Keyboard, ViewW * 0.5f, Y, ChipH, AnimNow);
	TraceMenuKit::DrawKeyLegend(HUD, Pad, ViewW * 0.5f, Y + TraceLoadoutLayout::PadLineGap * S, ChipH, AnimNow);
}

void FTraceLoadoutSelect::DrawPointer(AHUD* HUD, APlayerController* PC)
{
	// THE KIT'S BLADE, and the OS arrow hidden while it is drawn (stylespec §9). This page drew a white
	// L-bracket and renewed nothing, so a match showed the OS arrow AND the bracket. Not at all while
	// something in front owns the pointer.
	if (!bHasCursor || !bPointerOwned)
	{
		return;
	}

	if (TraceMenuKit::ShowCursor(HUD, PC, TEXT("loadout"), CursorPos, UIScale))
	{
		return;
	}

	// The fallback every Canvas surface keeps, for a build with no menu art or the frame or two before
	// the sprite's texture lands.
	const float Size = 9.f * UIScale;
	const float Thick = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceMenuArtStyle::WordDefault;
	HUD->DrawLine(CursorPos.X - Size, CursorPos.Y, CursorPos.X - Size * 0.35f, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X + Size * 0.35f, CursorPos.Y, CursorPos.X + Size, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y - Size, CursorPos.X, CursorPos.Y - Size * 0.35f, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y + Size * 0.35f, CursorPos.X, CursorPos.Y + Size, Color, Thick);
}

// =================================================================================================
// Test seams
// =================================================================================================

void FTraceLoadoutSelect::DebugTypeSizes(float InViewW, float InViewH, float InUIScale, TArray<float>& OutNameSizes,
	TArray<float>& OutBodySizes)
{
	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;

	// The steps DrawGrid and DrawCard take with each tab on screen in turn — the layout's tile, the
	// solve, each card's own size — so a solve that depended on the tab would show here as it did on
	// screen.
	const int32 TabWas = Tab;
	OutNameSizes.Reset();
	OutBodySizes.Reset();
	TArray<FString> Lines;
	for (int32 SlotIndex = 0; SlotIndex < static_cast<int32>(ETraceLoadoutSlot::Count); ++SlotIndex)
	{
		Tab = SlotIndex;
		float TileW = 0.f;
		float TileH = 0.f;
		GridTile(TileW, TileH);
		SolveTypeSizes(TileW, TileH);
		const float TextW = TileW - TraceLoadoutLayout::CardPad * 2.f * UIScale;

		const ETraceLoadoutSlot EachSlot = static_cast<ETraceLoadoutSlot>(SlotIndex);
		for (int32 Index = 0; Index < TraceLoadoutSelectFile::CardCount(EachSlot); ++Index)
		{
			const ETraceAbilityId Id = TraceLoadoutSelectFile::AbilityAtCard(EachSlot, Index);
			OutNameSizes.Add(TraceAbilityNames::Get(Id).IsEmpty() ? 0.f : NameFitSize);
			OutBodySizes.Add(TraceLoadoutSelectFile::CardBodySize(TraceAbilityNames::Describe(Id), TextW,
				BodyFitSize, BodyFitRoom, FitWeight, UIScale, Lines));
		}
	}
	Tab = TabWas;
}

uint32 FTraceLoadoutSelect::HoverSalt() const
{
	// The instance's address: the match page and the pause menu's editor are two members of two
	// objects, so two different salts, and each stays the same for as long as its page exists. Never
	// 0, which is the unsalted key every other screen uses.
	const uint32 FromAddress = PointerHash(this);
	return (FromAddress != 0u) ? FromAddress : 1u;
}

void FTraceLoadoutSelect::DebugPick(ETraceLoadoutSlot Slot, ETraceAbilityId Id)
{
	Staged.Set(Slot, Id);
	Highlighted[static_cast<int32>(Slot)] = TraceLoadoutSelectFile::CardForAbility(Slot, Id);
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

int32 FTraceLoadoutSelect::GetHighlighted(ETraceLoadoutSlot Slot) const
{
	const int32 Index = static_cast<int32>(Slot);
	return (Index >= 0 && Index < static_cast<int32>(ETraceLoadoutSlot::Count)) ? Highlighted[Index] : INDEX_NONE;
}

void FTraceLoadoutSelect::DebugBeginInput(ATracePlayerState* LocalState)
{
	OnOpened(LocalState);
}

void FTraceLoadoutSelect::DebugInput(const FTraceLoadoutKeys& Down, ATracePlayerState* LocalState,
	bool bInputAllowed, float InNow)
{
	Now = InNow;
	StepInput(Down, LocalState, bInputAllowed);
}

void FTraceLoadoutSelect::DebugSetCursor(const FVector2D& Pos)
{
	CursorPos = Pos;
	bHasCursor = true;
}

FBox2D FTraceLoadoutSelect::DebugCardRect(int32 Index) const
{
	return (Index >= 0 && Index < MaxCards) ? CardRects[Index] : FBox2D(ForceInit);
}

void FTraceLoadoutSelect::DebugSetTabRect(int32 Index, const FBox2D& Rect)
{
	if (Index >= 0 && Index < static_cast<int32>(ETraceLoadoutSlot::Count))
	{
		TabRects[Index] = Rect;
	}
}

void FTraceLoadoutSelect::DebugSetCardRect(int32 Index, const FBox2D& Rect)
{
	if (Index >= 0 && Index < MaxCards)
	{
		CardRects[Index] = Rect;
	}
}

void FTraceLoadoutSelect::DebugSetSavedRect(int32 Index, const FBox2D& Rect)
{
	if (Index >= 0 && Index < static_cast<int32>(UE_ARRAY_COUNT(SavedRects)))
	{
		SavedRects[Index] = Rect;
	}
}

void FTraceLoadoutSelect::DebugPointer(const FVector2D& Pos, bool bButtonDown, ATracePlayerState* LocalState,
	bool bShiftHeld)
{
	StepPointer(/*bSampled=*/true, Pos, bButtonDown, bShiftHeld, LocalState, /*bAct=*/true);
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

		// THE HUMAN, when there is one: the keyboard, handoff and CHANGE LOADOUT checks below drive a
		// real player controller. Any player state with a component otherwise, as before.
		ATracePlayerState* Subject = nullptr;
		UTraceAbilityComponent* Comp = nullptr;
		if (const AGameStateBase* GS = World->GetGameState())
		{
			for (int32 Pass = 0; Pass < 2 && Subject == nullptr; ++Pass)
			{
				for (APlayerState* Each : GS->PlayerArray)
				{
					ATracePlayerState* Candidate = Cast<ATracePlayerState>(Each);
					UTraceAbilityComponent* Found = (Candidate != nullptr)
						? Candidate->FindComponentByClass<UTraceAbilityComponent>() : nullptr;
					const bool bHuman = (Candidate != nullptr)
						&& Cast<ATracePlayerController>(Candidate->GetOwningController()) != nullptr;
					if (Found != nullptr && (bHuman || Pass == 1))
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
		const float RestoreDeadline = Subject->CharacterSelectDeadlineServerTime;
		const bool bRestoreLocked = Subject->IsCharacterLocked();
		const bool bRestoreChosen = Subject->WasCharacterChosen();

		UE_LOG(LogTraceGame, Display,
			TEXT("[LoadoutScreen] ===== does a press on the page change what you play? ====="));

		Check(TEXT("the loadout page is the armed one"), TraceLoadoutSelect::IsArmed(),
			TEXT("Trace.UI.LoadoutScreen"));

		// ---- DISARMED, THIS PAGE STAYS SHUT ------------------------------------------------------
		//
		// With Trace.UI.LoadoutScreen 0 the character page draws its ten cards, and this page used to
		// open on the same select window and draw its three columns over them. Asked with the window
		// OPEN, so the only thing that can keep it shut is the arm.
		{
			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
			const int32 SavedArm = TraceLoadoutSelectArm::GArmed;

			TraceLoadoutSelectArm::GArmed = 1;
			const bool bArmedOpens = FTraceLoadoutSelect::WantsOpen(Subject);
			TraceLoadoutSelectArm::GArmed = 0;
			const bool bDisarmedOpens = FTraceLoadoutSelect::WantsOpen(Subject);
			TraceLoadoutSelectArm::GArmed = SavedArm;

			Check(TEXT("armed, the select window opens this page"), bArmedOpens,
				TEXT("the window is open"));
			Check(TEXT("disarmed, it stays shut over the character page"), !bDisarmedOpens,
				TEXT("Trace.UI.LoadoutScreen 0 must show ONE screen"));
		}

		FTraceLoadoutSelect Screen;
		Screen.DebugPick(ETraceLoadoutSlot::Movement,  ETraceAbilityId::StickyGloves);
		Screen.DebugPick(ETraceLoadoutSlot::Passive,   ETraceAbilityId::Magnet);
		Screen.DebugPick(ETraceLoadoutSlot::Activated, ETraceAbilityId::Snap);

		FTraceLoadout Wanted;
		Wanted.Movement  = ETraceAbilityId::StickyGloves;
		Wanted.Passive   = ETraceAbilityId::Magnet;
		Wanted.Activated = ETraceAbilityId::Snap;

		Check(TEXT("three picks stage three different kits"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
		Comp->ApplyLoadout(Restore);
		Screen.DebugConfirm(Subject);
		Check(TEXT("LOCK IN during play changes nothing"),
			Comp->GetLoadout() == Restore, TraceLoadoutToString(Comp->GetLoadout()));

		// ...AND SAYS SO. The page used to print LOCKED IN the moment the request left, and the server's
		// refusal came back to nobody. The server now replies (ClientLockInRefused) and the page says why.
		const FString SaidLocked = TRACE_TEXT("LOADOUT.LOCKED_IN", "LOCKED IN");
		const FString SaidSending = TRACE_TEXT("LOADOUT.SENDING", "SENDING");
		const FString SaidRefused = TRACE_TEXT("LOADOUT.REFUSED_LOCKED", "LOADOUT LOCKED");
		Check(TEXT("*** a refused LOCK IN says why, never LOCKED IN ***"),
			Screen.DebugMessage() == SaidRefused,
			FString::Printf(TEXT("the page says '%s'"), *Screen.DebugMessage()));

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

		// *** AND IT STAYS CLOSED. *** This is the assertion the previous version was missing, and its
		// absence shipped a build where the menu vanished on LOCK IN and came straight back: the
		// select poll runs at 4 Hz for the whole match, decided the player had not picked (their
		// CHARACTER id is still None — they picked a LOADOUT), and reopened it. Closing was never the
		// problem. Staying closed was.
		ATraceGameMode* const Mode = World->GetAuthGameMode<ATraceGameMode>();
		ATraceGameState* const TraceGS = World->GetGameState<ATraceGameState>();
		if (Mode != nullptr && TraceGS != nullptr)
		{
			for (int32 Pass = 0; Pass < 4; ++Pass)
			{
				Mode->DebugPollCharacterSelect();
			}
			Check(TEXT("...and four select polls do NOT reopen it"),
				!Subject->IsCharacterSelectOpen(),
				TEXT("the poll ran at 4Hz all match and undid the close"));

			// ---- THE MIRROR OF THE SAME BUG ---------------------------------------------------
			//
			// The branch that reopens a screen for somebody unsorted also CLOSES one for somebody
			// sorted — so at half time, when almost everyone is sorted, the poll would have shut the
			// loadout window within 250 ms of the break opening it. Nobody could have changed a
			// loadout at half time at all, and the S4 harness would not have noticed: it asked
			// whether the server ACCEPTED a change, never whether the screen survived long enough to
			// ask for one.
			const bool bWasBreak = TraceGS->IsHalfTimeBreak();
			const int32 HalfNow = TraceGS->CurrentHalf;
			const int32 HalvesNow = FMath::Max(1, TraceGS->NumHalves);

			TraceGS->SetHalfState(HalfNow, HalvesNow, /*bInHalfTimeBreak=*/true);
			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
			for (int32 Pass = 0; Pass < 4; ++Pass)
			{
				Mode->DebugPollCharacterSelect();
			}
			Check(TEXT("the half time window SURVIVES the poll"),
				Subject->IsCharacterSelectOpen(),
				TEXT("the break owns the window; the poll must stand off"));

			TraceGS->SetHalfState(HalfNow, HalvesNow, bWasBreak);
		}

		// ---- *** E MUST ACTUALLY FIRE. *** ------------------------------------------------------
		//
		// The gap every earlier harness left. They proved the loadout was ACCEPTED and that the kits
		// were BUILT, and then stopped — so a build shipped in which locking in an activated ability
		// did nothing when you pressed E. TryActivate refused on its FIRST LINE because the player's
		// CHARACTER id was None, which is true of every loadout-only player; movement and passive
		// abilities kept working because their hooks already asked the slots.
		//
		// SO THIS PRESSES E, rather than checking that the kit exists. Checking the kit exists is what
		// the previous version of this block did and it is worth nothing: the kit DID exist, and the
		// press was refused anyway by a gate above it. The bug lived in the gap between "equipped"
		// and "works", which is exactly the gap a test has to cross.
		//
		// Driven on a subject with a LIVE PAWN and a cleared cooldown, with the character id left
		// EMPTY, because that combination is precisely the state that was broken.
		{
			UTraceAbilityComponent* FireComp = nullptr;
			if (const AGameStateBase* GS = World->GetGameState())
			{
				for (APlayerState* Each : GS->PlayerArray)
				{
					ATracePlayerState* Candidate = Cast<ATracePlayerState>(Each);
					UTraceAbilityComponent* Found = (Candidate != nullptr)
						? Candidate->FindComponentByClass<UTraceAbilityComponent>() : nullptr;
					const ATraceCharacter* Pawn = (Found != nullptr) ? Found->GetOwningCharacter() : nullptr;
					if (Pawn != nullptr && Pawn->IsAlive() && !Pawn->IsCarrier())
					{
						FireComp = Found;
						break;
					}
				}
			}

			if (FireComp == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[LoadoutScreen]   (no live non-carrier pawn; the E-fires check did not run)"));
			}
			else
			{
				const FTraceLoadout FireRestore = FireComp->GetLoadout();

				FireComp->ApplyLoadout(Wanted);            // Elle's SNAP in the activated slot
				FireComp->DebugSetActivatedCooldown(0.f);  // so a running timer cannot be the refusal

				Check(TEXT("the firing subject really has no character id"),
					FireComp->GetCharacterId() == ETraceCharacterId::None,
					TEXT("this is the state that broke; a character id would hide the bug"));

				const bool bFired = FireComp->TryActivate();
				Check(TEXT("*** E FIRES on a loadout with no character id ***"), bFired,
					TEXT("TryActivate returned false on its first line for every loadout player"));

				FireComp->ApplyLoadout(FireRestore);
			}
		}

		// ---- THE NAMES ARE THE ORIGINALS, AND NOTHING INVENTS ONE ------------------------------
		//
		// The reverted state, asserted rather than eyeballed: exactly the ten roster ActivatedNames
		// exist, and movement and passive have no name at all. An earlier pass invented thirty and
		// renamed three real ones, which is the regression this guards.
		// ---- EVERY ABILITY HAS SOMETHING TO DRAW, AND THE ACTIVATED ONES KEEP THE ROSTER'S NAMES ----
		//
		// Rewritten for Demo 35. The old version walked ten kits and asserted that movement and
		// passive abilities had NO name — true then, and wrong now: the owner named most of them.
		// What still holds is the rule underneath: a name is either the owner's or the roster's, and
		// nothing is invented. So this checks that every ACTIVATED ability still carries exactly the
		// roster's ActivatedName, and that no ability is left with nothing at all to put on a card.
		int32 NameProblems = 0;
		for (int32 IdIndex = 1; IdIndex < static_cast<int32>(ETraceAbilityId::Count); ++IdIndex)
		{
			const ETraceAbilityId Ability = static_cast<ETraceAbilityId>(IdIndex);
			const FTraceAbilityDef* Def = TraceAbilityTable::Find(Ability);
			if (Def == nullptr)
			{
				++NameProblems;
				UE_LOG(LogTraceGame, Error,
					TEXT("[LoadoutScreen]   ability %d has no row in the ability table"), IdIndex);
				continue;
			}

			if (Def->Slot == ETraceLoadoutSlot::Activated)
			{
				const TraceCharacterRoster::FTraceCharacterEntry* Entry =
					TraceCharacterRoster::Find(static_cast<uint8>(Def->Kit));
				const FString Name = TraceAbilityNames::Get(Ability);
				if (Entry == nullptr || Name != FString(Entry->ActivatedName))
				{
					++NameProblems;
					UE_LOG(LogTraceGame, Error,
						TEXT("[LoadoutScreen]   %s: activated name '%s' is not the roster's '%s'"),
						TraceAbilityIdToString(Ability), *Name,
						(Entry != nullptr) ? Entry->ActivatedName : TEXT("<none>"));
				}
			}

			// A card shows the name where there is one and the description otherwise, so an ability
			// with neither would draw an empty plate. Three are deliberately unnamed (Demo 35 left
			// two blank and marked one TBD) and they lean on their description entirely.
			if (TraceAbilityNames::ShortLabel(Ability).IsEmpty())
			{
				++NameProblems;
				UE_LOG(LogTraceGame, Error,
					TEXT("[LoadoutScreen]   %s has nothing to draw on a card"),
					TraceAbilityIdToString(Ability));
			}
		}
		// ...AND NO TWO CARDS ON A TAB SAY THE SAME THING. Oyster's unnamed dash cloak read Oyster's one
		// passive line, which is PICKLE JAR's, so the passive tab showed PICKLE JAR's rules twice.
		for (int32 SlotIndex = 0; SlotIndex < static_cast<int32>(ETraceLoadoutSlot::Count); ++SlotIndex)
		{
			const ETraceLoadoutSlot Slot = static_cast<ETraceLoadoutSlot>(SlotIndex);
			TMap<FString, ETraceAbilityId> Seen;
			for (const ETraceAbilityId Ability : TraceLoadoutSelectFile::AbilitiesFor(Slot))
			{
				const FString Body = TraceAbilityNames::Describe(Ability);
				if (const ETraceAbilityId* Twin = Seen.Find(Body))
				{
					++NameProblems;
					UE_LOG(LogTraceGame, Error, TEXT("[LoadoutScreen]   %s and %s show the same description"),
						TraceAbilityIdToString(*Twin), TraceAbilityIdToString(Ability));
				}
				Seen.Add(Body, Ability);
			}
		}

		Failures += NameProblems;
		Check(TEXT("every ability has a label, names match, no card is a twin"), NameProblems == 0,
			FString::Printf(TEXT("%d problem(s) across %d abilities"),
				NameProblems, static_cast<int32>(ETraceAbilityId::Count) - 1));


		// =========================================================================================
		// P04 — THE PAGE WORKS FROM A KEYBOARD AND A PAD, NOT ONLY A MOUSE
		// =========================================================================================

		// ---- THE KEY TABLE ---------------------------------------------------------------------
		//
		// Which physical key means which verb, read through the SAME table the live page reads the
		// controller with. The page shipped with no pad key for the tabs (a pad player was stuck on
		// MOVEMENT) and with ENTER / A meaning LOCK IN while the arrows only moved a highlight.
		{
			auto KeysFor = [](std::initializer_list<FKey> Pressed, bool bLibrary)
			{
				return FTraceLoadoutSelect::DebugKeysFor(TArray<FKey>(Pressed), bLibrary);
			};
			auto AnyVerb = [](const FTraceLoadoutKeys& K)
			{
				bool bDigit = false;
				for (const bool bEach : K.bNumber)
				{
					bDigit = bDigit || bEach;
				}
				return K.bEquip || K.bLock || K.bBack || K.bTabLeft || K.bTabRight || K.bSavedNext || K.NavX != 0
					|| K.NavY != 0 || bDigit;
			};

			Check(TEXT("pad LB / RB change tab, as Q / E do"),
				KeysFor({ EKeys::Gamepad_LeftShoulder }, false).bTabLeft
					&& KeysFor({ EKeys::Gamepad_RightShoulder }, false).bTabRight
					&& KeysFor({ EKeys::Q }, false).bTabLeft && KeysFor({ EKeys::E }, false).bTabRight,
				TEXT("the comment claimed the shoulders for as long as the page existed"));
			Check(TEXT("ENTER and pad A EQUIP, and do not lock in"),
				KeysFor({ EKeys::Enter }, false).bEquip && !KeysFor({ EKeys::Enter }, false).bLock
					&& KeysFor({ EKeys::Gamepad_FaceButton_Bottom }, false).bEquip
					&& !KeysFor({ EKeys::Gamepad_FaceButton_Bottom }, false).bLock,
				TEXT("ENTER used to lock in the unchanged loadout"));
			Check(TEXT("F and pad X LOCK IN"),
				KeysFor({ EKeys::F }, false).bLock && KeysFor({ EKeys::Gamepad_FaceButton_Left }, false).bLock,
				TEXT("a separate key, so equipping never ends the page"));
			Check(TEXT("pad B is BACK; ESC only in the library"),
				KeysFor({ EKeys::Gamepad_FaceButton_Right }, false).bBack
					&& KeysFor({ EKeys::Gamepad_FaceButton_Right }, true).bBack
					&& KeysFor({ EKeys::Escape }, true).bBack && !KeysFor({ EKeys::Escape }, false).bBack,
				TEXT("in a match ESC is the pause menu"));
			Check(TEXT("SPACE (jump) does nothing on this page"),
				!AnyVerb(KeysFor({ EKeys::SpaceBar }, false)) && !AnyVerb(KeysFor({ EKeys::SpaceBar }, true)),
				TEXT("a SPACE held at the half-time whistle locked in"));
		}

		// ---- KEYS AND PAD BUILD A LOADOUT, THROUGH THE REAL INPUT PATH ---------------------------
		//
		// Frame by frame through StepInput — the same sample-then-act path Tick runs — from the state
		// the page is in on the frame it opens. What the old page did with these frames: the arrows
		// moved a highlight, ENTER sent the untouched default and closed the page, and LB / RB / B did
		// nothing at all.
		{
			Comp->ApplyLoadout(FTraceLoadout());   // a known start: the page seeds the first card of each column
			Subject->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);
			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);

			FTraceLoadoutSelect Page;
			float Clock = 100.f;
			auto Frame = [&Page, Subject, &Clock](const FTraceLoadoutKeys& FrameKeys, bool bAllowed)
			{
				Clock += 0.05f;
				Page.DebugInput(FrameKeys, Subject, bAllowed, Clock);
			};
			const FTraceLoadoutKeys Nothing;
			FTraceLoadoutKeys Enter;  Enter.bEquip = true;
			FTraceLoadoutKeys PadA;   PadA.bEquip = true;
			FTraceLoadoutKeys Right;  Right.NavX = 1;
			FTraceLoadoutKeys Down;   Down.NavY = 1;
			FTraceLoadoutKeys RB;     RB.bTabRight = true;
			FTraceLoadoutKeys LB;     LB.bTabLeft = true;
			FTraceLoadoutKeys PadB;   PadB.bBack = true;
			FTraceLoadoutKeys Lock;   Lock.bLock = true;

			Page.DebugBeginInput(Subject);
			const FTraceLoadout Seed = Page.GetStaged();

			// An ENTER still held from the team screen's confirm when the page opens.
			Frame(Enter, true);
			Frame(Enter, true);
			Check(TEXT("an ENTER held from team select does nothing"),
				Page.GetStaged() == Seed && Page.GetTab() == 0 && Subject->IsCharacterSelectOpen(),
				TraceLoadoutToString(Page.GetStaged()));
			Frame(Nothing, true);

			// Arrow right, ENTER.
			Frame(Right, true);
			Frame(Nothing, true);
			Frame(Enter, true);
			Frame(Nothing, true);
			const ETraceAbilityId WantMovement = TraceLoadoutSelectFile::AbilityAtCard(ETraceLoadoutSlot::Movement, 1);
			Check(TEXT("*** keys: an arrow and ENTER EQUIP the card ***"),
				Page.GetStaged().Movement == WantMovement,
				FString::Printf(TEXT("staged %s, wanted %s"), *TraceLoadoutToString(Page.GetStaged()),
					TraceAbilityIdToString(WantMovement)));
			Check(TEXT("...and ENTER moves on to the next tab"), Page.GetTab() == 1,
				FString::Printf(TEXT("tab %d"), Page.GetTab()));

			// The shoulders.
			Frame(RB, true);
			Frame(Nothing, true);
			const int32 AfterRB = Page.GetTab();
			Frame(LB, true);
			Frame(Nothing, true);
			Check(TEXT("*** pad: RB and LB change tab ***"), AfterRB == 2 && Page.GetTab() == 1,
				FString::Printf(TEXT("RB -> %d, LB -> %d"), AfterRB, Page.GetTab()));

			// Stick down, A: a column move, not a diagonal one.
			Frame(Down, true);
			Frame(Nothing, true);
			Frame(PadA, true);
			Frame(Nothing, true);
			const ETraceAbilityId WantPassive = TraceLoadoutSelectFile::AbilityAtCard(ETraceLoadoutSlot::Passive,
				TraceLoadoutLayout::Columns);
			Check(TEXT("*** pad: stick down and A equip the card below ***"),
				Page.GetStaged().Passive == WantPassive && Page.GetTab() == 2,
				FString::Printf(TEXT("staged %s, tab %d"), *TraceLoadoutToString(Page.GetStaged()), Page.GetTab()));

			// B steps back a question, and RB returns.
			Frame(PadB, true);
			Frame(Nothing, true);
			const int32 AfterB = Page.GetTab();
			Frame(RB, true);
			Frame(Nothing, true);
			Check(TEXT("pad B goes back a tab on the match page"), AfterB == 1 && Page.GetTab() == 2,
				FString::Printf(TEXT("B -> %d"), AfterB));

			// The last tab: two rights and ENTER equip and STAY.
			Frame(Right, true);
			Frame(Nothing, true);
			Frame(Right, true);
			Frame(Nothing, true);
			Frame(Enter, true);
			Frame(Nothing, true);
			const ETraceAbilityId WantActivated = TraceLoadoutSelectFile::AbilityAtCard(ETraceLoadoutSlot::Activated,
				TraceLoadoutSelectFile::CardForAbility(ETraceLoadoutSlot::Activated, Seed.Activated) + 2);
			Check(TEXT("ENTER on the last tab equips and does NOT lock in"),
				Page.GetStaged().Activated == WantActivated && Page.GetTab() == 2
					&& Subject->IsCharacterSelectOpen() && Comp->GetLoadout().IsEmpty(),
				TraceLoadoutToString(Page.GetStaged()));

			// F pressed while the pause menu is in front, still down when it closes.
			Frame(Lock, false);
			Frame(Lock, true);
			Frame(Lock, true);
			Check(TEXT("*** a LOCK IN held through the pause menu does not fire ***"),
				Subject->IsCharacterSelectOpen() && Comp->GetLoadout().IsEmpty(),
				TEXT("RESUME's ENTER used to lock in on the next frame"));
			Frame(Nothing, true);

			// A fresh F.
			const FTraceLoadout Built = Page.GetStaged();
			Frame(Lock, true);
			Frame(Nothing, true);
			Check(TEXT("*** F / pad X locks in exactly what the keys built ***"),
				Comp->GetLoadout() == Built && Built != Seed && !Subject->IsCharacterSelectOpen(),
				FString::Printf(TEXT("equipped %s, built %s"), *TraceLoadoutToString(Comp->GetLoadout()),
					*TraceLoadoutToString(Built)));
		}

		// ---- ONE POINTER, AND A FRESH ONE ON EVERY OPENING ---------------------------------------
		{
			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
			FTraceLoadoutSelect Page;
			Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 1.0f, /*bInputAllowed=*/true);
			Page.DebugSetCursor(FVector2D(400.f, 400.f));
			Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 1.1f, /*bInputAllowed=*/false);
			const bool bUnderPause = Page.DebugWouldDrawPointer();
			Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 1.2f, /*bInputAllowed=*/true);
			const bool bAfterPause = Page.DebugWouldDrawPointer();
			Check(TEXT("*** under the pause menu the page draws no pointer ***"), !bUnderPause && bAfterPause,
				TEXT("a frozen blade under the scrim beside the pause menu's live one"));

			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
			Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 1.3f, /*bInputAllowed=*/true);
			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
			Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 1.4f, /*bInputAllowed=*/true);
			Check(TEXT("a reopened page carries no stale pointer sample"), !Page.DebugWouldDrawPointer(),
				TEXT("the half-time page's first sample counted as a move and dragged the highlight"));
		}

		// ---- ONE HOVER ON THE PAGE ------------------------------------------------------------------
		//
		// The kit's rule (stylespec §5): hover and keyboard selection are ONE state, so one thing wears
		// the look. The page lit up to three at once: the current tab always, a pointed-at tab in
		// PRESSED (the same plate, dimmed), and the keys' card while the pointer sat on LOCK IN or a
		// tab. Driven through the real pointer and key paths, against hit rects where a 1080p draw
		// leaves them (tabs 143 + 552 i wide 530, cards 366 apart, LOCK IN at the bottom right).
		{
			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
			FTraceLoadoutSelect Page;
			Page.DebugBeginInput(Subject);
			for (int32 TabIndex = 0; TabIndex < static_cast<int32>(ETraceLoadoutSlot::Count); ++TabIndex)
			{
				const float TabLeftX = 143.f + 552.f * TabIndex;
				Page.DebugSetTabRect(TabIndex, FBox2D(FVector2D(TabLeftX, 100.f), FVector2D(TabLeftX + 530.f, 174.f)));
			}
			for (int32 CardIndex = 0; CardIndex < FTraceLoadoutSelect::MaxCards; ++CardIndex)
			{
				const FVector2D CardTopLeft(54.f + 366.f * (CardIndex % TraceLoadoutLayout::Columns),
					198.f + 344.f * (CardIndex / TraceLoadoutLayout::Columns));
				Page.DebugSetCardRect(CardIndex, FBox2D(CardTopLeft, CardTopLeft + FVector2D(346.f, 326.f)));
			}
			Page.DebugSetConfirmRect(FBox2D(FVector2D(1616.f, 888.f), FVector2D(1866.f, 942.f)));

			float Clock = 300.f;
			auto KeyFrame = [&Page, Subject, &Clock](const FTraceLoadoutKeys& FrameKeys)
			{
				Clock += 0.05f;
				Page.DebugInput(FrameKeys, Subject, /*bInputAllowed=*/true, Clock);
			};
			auto PointerTo = [&Page, Subject](float PX, float PY, bool bButton)
			{
				Page.DebugPointer(FVector2D(PX, PY), bButton, Subject);
			};
			auto DescribeLit = [](const FTraceLoadoutLit& LitState)
			{
				return FString::Printf(TEXT("lit: card %d, tab %d, saved %d, lock in %d, back %d, pressed %d = %d"),
					LitState.Card, LitState.Tab, LitState.Saved, LitState.bConfirm ? 1 : 0, LitState.bBack ? 1 : 0,
					LitState.bPressed ? 1 : 0, LitState.Count());
			};
			const FTraceLoadoutKeys Nothing;
			FTraceLoadoutKeys Right;  Right.NavX = 1;
			FTraceLoadoutKeys Enter;  Enter.bEquip = true;

			KeyFrame(Nothing);                 // the open edge assumed every key held
			PointerTo(960.f, 60.f, false);     // on the title: the first sample, not a move
			const FTraceLoadoutLit OnOpen = Page.DebugLit();
			PointerTo(960.f, 140.f, false);    // onto PASSIVE, not the tab the page is on
			const FTraceLoadoutLit OnOtherTab = Page.DebugLit();
			PointerTo(408.f, 140.f, false);    // onto MOVEMENT, the tab the page is on
			const FTraceLoadoutLit OnOwnTab = Page.DebugLit();
			PointerTo(1740.f, 915.f, false);   // onto LOCK IN
			const FTraceLoadoutLit OnLockIn = Page.DebugLit();

			Check(TEXT("*** one hover: the page opens with only its card lit ***"),
				OnOpen.Count() == 1 && OnOpen.Card == Page.GetHighlighted(ETraceLoadoutSlot::Movement),
				DescribeLit(OnOpen));
			Check(TEXT("*** one hover: a pointer on another tab lights only it ***"),
				OnOtherTab.Count() == 1 && OnOtherTab.Tab == 1 && !OnOtherTab.bPressed && Page.GetTab() == 0,
				DescribeLit(OnOtherTab));
			Check(TEXT("one hover: on the current tab, or LOCK IN, only that"),
				OnOwnTab.Count() == 1 && OnOwnTab.Tab == 0 && OnLockIn.Count() == 1 && OnLockIn.bConfirm,
				DescribeLit(OnOwnTab) + TEXT(" / ") + DescribeLit(OnLockIn));

			// An arrow while the pointer RESTS on LOCK IN: the keys lead, and the resting pointer
			// lights nothing (it used to keep LOCK IN glowing beside the card the arrow moved to).
			const int32 BeforeKey = Page.GetHighlighted(ETraceLoadoutSlot::Movement);
			KeyFrame(Right);
			PointerTo(1740.f, 915.f, false);
			KeyFrame(Nothing);
			PointerTo(1740.f, 915.f, false);
			const FTraceLoadoutLit AfterKey = Page.DebugLit();
			Check(TEXT("*** one hover: a key takes it back from a resting pointer ***"),
				AfterKey.Count() == 1 && AfterKey.Card == Page.GetHighlighted(ETraceLoadoutSlot::Movement)
					&& AfterKey.Card != BeforeKey,
				DescribeLit(AfterKey));

			// PRESSED means a finger down: a press on ACTIVATED, then its release.
			PointerTo(1512.f, 140.f, true);
			const FTraceLoadoutLit WhilePressed = Page.DebugLit();
			PointerTo(1512.f, 140.f, false);
			const FTraceLoadoutLit AfterClick = Page.DebugLit();
			Check(TEXT("a press on a tab is PRESSED; its release changes tab"),
				WhilePressed.Count() == 1 && WhilePressed.Tab == 2 && WhilePressed.bPressed
					&& AfterClick.Count() == 1 && AfterClick.Tab == 2 && !AfterClick.bPressed && Page.GetTab() == 2,
				DescribeLit(WhilePressed) + TEXT(" / ") + DescribeLit(AfterClick));

			// The pointer walks onto card 3: the one hover goes with it.
			PointerTo(54.f + 366.f * 3.f + 170.f, 360.f, false);
			const FTraceLoadoutLit OnCard = Page.DebugLit();
			Check(TEXT("a pointer on the grid moves the one hover to its card"),
				OnCard.Count() == 1 && OnCard.Card == 3 && Page.GetHighlighted(ETraceLoadoutSlot::Activated) == 3,
				DescribeLit(OnCard));

			// ENTER on the last tab: LOCK IN takes the look, and the card gives it up.
			KeyFrame(Enter);
			PointerTo(54.f + 366.f * 3.f + 170.f, 360.f, false);
			KeyFrame(Nothing);
			PointerTo(54.f + 366.f * 3.f + 170.f, 360.f, false);
			const FTraceLoadoutLit AfterEnter = Page.DebugLit();
			Check(TEXT("*** ENTER on the last tab hands the one hover to LOCK IN ***"),
				AfterEnter.Count() == 1 && AfterEnter.bConfirm && Subject->IsCharacterSelectOpen(),
				DescribeLit(AfterEnter));
		}

		// ---- THE LIBRARY EDITOR: BACK, EQUIP, SAVE --------------------------------------------------
		{
			UTraceUserSettings& Settings = UTraceUserSettings::Get();
			constexpr int32 LibrarySlotIndex = 4;
			const FTraceLoadout SavedBefore = Settings.GetSavedLoadout(LibrarySlotIndex);

			FTraceLoadoutSelect Library;
			float Clock = 200.f;
			auto Frame = [&Library, &Clock](const FTraceLoadoutKeys& FrameKeys)
			{
				Clock += 0.05f;
				Library.DebugInput(FrameKeys, nullptr, true, Clock);
			};
			const FTraceLoadoutKeys Nothing;
			FTraceLoadoutKeys Right;  Right.NavX = 1;
			FTraceLoadoutKeys Enter;  Enter.bEquip = true;
			FTraceLoadoutKeys PadB;   PadB.bBack = true;
			FTraceLoadoutKeys Save;   Save.bLock = true;
			FTraceLoadoutKeys Start;  Start.bEscapePressed = true;

			Library.OpenLibrary(LibrarySlotIndex);
			Frame(Nothing);
			Frame(Right);
			Frame(Nothing);
			Frame(Enter);
			Frame(Nothing);
			Frame(PadB);
			Check(TEXT("*** library: pad B leaves without saving ***"),
				!Library.IsLibraryOpen() && Settings.GetSavedLoadout(LibrarySlotIndex) == SavedBefore,
				TEXT("B was not read; a pad's only way out was A, which saved"));

			Library.OpenLibrary(LibrarySlotIndex);
			Frame(Nothing);
			Frame(Start);
			Check(TEXT("library: MENU/START's one-frame ESC leaves too"), !Library.IsLibraryOpen(),
				TEXT("a press and release in one frame is invisible to a level read"));

			Library.OpenLibrary(LibrarySlotIndex);
			Frame(Nothing);
			Frame(Right);
			Frame(Nothing);
			Frame(Enter);
			Frame(Nothing);
			const FTraceLoadout Edited = Library.GetStaged();
			Frame(Save);
			Check(TEXT("*** library: keys equip, F saves the slot ***"),
				!Library.IsLibraryOpen() && Settings.GetSavedLoadout(LibrarySlotIndex) == Edited,
				TraceLoadoutToString(Settings.GetSavedLoadout(LibrarySlotIndex)));

			Settings.SetSavedLoadout(LibrarySlotIndex, SavedBefore);
		}

		// =========================================================================================
		// THE LOW-FINDINGS PASS: what the page says, and what a pad can reach
		// =========================================================================================

		// ---- LOCK IN SAYS SENDING UNTIL THE SERVER AGREES, THEN LOCKED IN ------------------------
		{
			Comp->ApplyLoadout(Restore);
			Subject->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);
			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
			FTraceLoadoutSelect Page;
			Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 5.0f, /*bInputAllowed=*/true);   // up
			Page.DebugPick(ETraceLoadoutSlot::Movement,  ETraceAbilityId::StickyGloves);
			Page.DebugPick(ETraceLoadoutSlot::Passive,   ETraceAbilityId::Magnet);
			Page.DebugPick(ETraceLoadoutSlot::Activated, ETraceAbilityId::Snap);
			Page.DebugConfirm(Subject);   // accepted: the server shuts the window inside the call
			const FString AtSend = Page.DebugMessage();
			Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 5.1f, /*bInputAllowed=*/true);   // sees it shut
			Check(TEXT("*** LOCK IN says SENDING, and LOCKED IN only once the window shuts ***"),
				AtSend == SaidSending && Page.DebugMessage() == SaidLocked && !Subject->IsCharacterSelectOpen()
					&& Comp->GetLoadout() == Wanted,
				FString::Printf(TEXT("at the send '%s', after the close '%s'"), *AtSend, *Page.DebugMessage()));
		}

		// ---- A REFUSAL WITH THE PAGE STILL UP: IT SAYS WHY, AND THE PAGE STILL WORKS -------------
		//
		// The race the finding named: the server's window shuts (the deadline) while this client's page
		// is still up and its LOCK IN is on the wire. The server refuses; the page must say so and stay
		// usable, not sit there saying LOCKED IN.
		{
			const ATraceGameState* const RaceGS = World->GetGameState<ATraceGameState>();
			const bool bLivePlay = RaceGS != nullptr && RaceGS->TraceMatchState == ETraceMatchState::InProgress
				&& !RaceGS->IsHalfTimeBreak();
			if (!bLivePlay)
			{
				Check(TEXT("a refused LOCK IN leaves the page up and working"), false,
					TEXT("INCONCLUSIVE: the server only refuses in live play - run this after warm-up"));
			}
			else
			{
				Comp->ApplyLoadout(Restore);
				Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
				FTraceLoadoutSelect Page;
				Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 6.0f, /*bInputAllowed=*/true);   // up
				Page.DebugPick(ETraceLoadoutSlot::Movement, ETraceAbilityId::StickyGloves);
				Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);   // the deadline, mid-send
				Page.DebugConfirm(Subject);
				const FString Said = Page.DebugMessage();

				// Still up on this client: a key still moves and equips.
				Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
				const FTraceLoadout BeforeKeys = Page.GetStaged();
				FTraceLoadoutKeys Right;  Right.NavX = 1;
				FTraceLoadoutKeys Enter;  Enter.bEquip = true;
				const FTraceLoadoutKeys Nothing;
				Page.DebugInput(Nothing, Subject, true, 6.1f);
				Page.DebugInput(Right, Subject, true, 6.2f);
				Page.DebugInput(Nothing, Subject, true, 6.3f);
				Page.DebugInput(Enter, Subject, true, 6.4f);
				Page.DebugInput(Nothing, Subject, true, 6.5f);
				Check(TEXT("*** a refused LOCK IN says why and leaves the page working ***"),
					Said == SaidRefused && Comp->GetLoadout() == Restore && Page.GetStaged() != BeforeKeys,
					FString::Printf(TEXT("said '%s', server kept %s, a key then staged %s"), *Said,
						*TraceLoadoutToString(Comp->GetLoadout()), *TraceLoadoutToString(Page.GetStaged())));
				Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
			}
		}

		// ---- TWO LOCK INS BEFORE THE ANSWER: STILL LOCKED IN -------------------------------------
		//
		// A double tap inside one server frame, or F pressed in the last second as the deadline's
		// auto-send fires: two requests before this page has heard the answer to the first. The server
		// accepts the first (and closes its window) and refuses the second as LOCKED. The page used to
		// reset its refusal baseline on the second send, read that refusal as the answer to the lock-in
		// it made, and fade out on LOADOUT LOCKED over a loadout the server had applied. In live play
		// only: before the whistle the server takes the second one too, and nothing is refused.
		{
			const ATraceGameState* const TwiceGS = World->GetGameState<ATraceGameState>();
			const bool bTwiceLive = TwiceGS != nullptr && TwiceGS->TraceMatchState == ETraceMatchState::InProgress
				&& !TwiceGS->IsHalfTimeBreak();
			if (!bTwiceLive)
			{
				Check(TEXT("two LOCK INs before the answer still end on LOCKED IN"), false,
					TEXT("INCONCLUSIVE: the server only refuses in live play - run this after warm-up"));
			}
			else
			{
				Comp->ApplyLoadout(Restore);
				Subject->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);
				Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
				const int32 RefusalsBefore = Comp->GetLockInRefusalCount();
				FTraceLoadoutSelect Page;
				Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 7.0f, /*bInputAllowed=*/true);   // up
				Page.DebugPick(ETraceLoadoutSlot::Movement,  ETraceAbilityId::StickyGloves);
				Page.DebugPick(ETraceLoadoutSlot::Passive,   ETraceAbilityId::Magnet);
				Page.DebugPick(ETraceLoadoutSlot::Activated, ETraceAbilityId::Snap);
				Page.DebugConfirm(Subject);   // accepted: the server shuts its window inside the call...
				Page.DebugConfirm(Subject);   // ...and a second press before this page has seen that
				const int32 RefusalsAfter = Comp->GetLockInRefusalCount();
				Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 7.1f, /*bInputAllowed=*/true);   // sees it shut
				Check(TEXT("*** two LOCK INs before the answer still end on LOCKED IN ***"),
					Page.DebugMessage() == SaidLocked && Comp->GetLoadout() == Wanted && !Subject->IsCharacterSelectOpen()
						&& RefusalsAfter == RefusalsBefore,
					FString::Printf(TEXT("the page says '%s', server holds %s, %d refusal(s) came back"), *Page.DebugMessage(),
						*TraceLoadoutToString(Comp->GetLoadout()), RefusalsAfter - RefusalsBefore));
			}
		}

		// ---- HALF TIME: THE PAGE SAYS SO ---------------------------------------------------------
		{
			ATraceGameState* const BreakGS = World->GetGameState<ATraceGameState>();
			if (BreakGS == nullptr)
			{
				Check(TEXT("the half-time page is titled HALF TIME"), false, TEXT("INCONCLUSIVE: no Trace game state"));
			}
			else
			{
				const bool bWasBreak = BreakGS->IsHalfTimeBreak();
				const int32 HalfNow = BreakGS->CurrentHalf;
				const int32 HalvesNow = FMath::Max(1, BreakGS->NumHalves);
				Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);

				FTraceLoadoutSelect Page;
				BreakGS->SetHalfState(HalfNow, HalvesNow, /*bInHalfTimeBreak=*/true);
				Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 8.0f, /*bInputAllowed=*/true);
				const FString InBreak = Page.DebugTitle();
				const bool bBreakHeader = Page.DebugHalfTimeHeader();
				BreakGS->SetHalfState(HalfNow, HalvesNow, /*bInHalfTimeBreak=*/false);
				Page.Tick(nullptr, nullptr, Subject, 1920.f, 1080.f, 1.f, 8.1f, /*bInputAllowed=*/true);
				const FString OutOfBreak = Page.DebugTitle();
				const bool bPlayHeader = Page.DebugHalfTimeHeader();
				BreakGS->SetHalfState(HalfNow, HalvesNow, bWasBreak);

				Check(TEXT("*** at half time the page is titled HALF TIME, score beside it ***"),
					InBreak == TRACE_TEXT("HUD.BANNER_HALF_TIME", "HALF TIME") && bBreakHeader
						&& OutOfBreak == TRACE_TEXT("LOADOUT.TITLE", "BUILD YOUR LOADOUT") && !bPlayHeader,
					FString::Printf(TEXT("break: '%s' (score %d), otherwise: '%s' (score %d)"), *InBreak,
						bBreakHeader ? 1 : 0, *OutOfBreak, bPlayHeader ? 1 : 0));
			}
		}

		// ---- ONE TYPE SIZE ON EVERY TAB ------------------------------------------------------------
		//
		// Every card of every tab, at the sizes DrawCard would use: one name size, one description
		// size. They were solved per tab (names 13 / 12 / 18 px caps on the three tabs).
		{
			FTraceLoadoutSelect Page;
			FString Detail;
			bool bOneSize = true;
			const float ViewWidths[] = { 1920.f, 1440.f };   // 16:9 and 4:3, both at 1080 tall
			for (const float ViewWidth : ViewWidths)
			{
				TArray<float> NameSizes;
				TArray<float> BodySizes;
				Page.DebugTypeSizes(ViewWidth, 1080.f, 1.f, NameSizes, BodySizes);
				float NameLo = 1000.f, NameHi = 0.f, BodyLo = 1000.f, BodyHi = 0.f;
				for (int32 Index = 0; Index < NameSizes.Num(); ++Index)
				{
					if (NameSizes[Index] > 0.f)
					{
						NameLo = FMath::Min(NameLo, NameSizes[Index]);
						NameHi = FMath::Max(NameHi, NameSizes[Index]);
					}
					BodyLo = FMath::Min(BodyLo, BodySizes[Index]);
					BodyHi = FMath::Max(BodyHi, BodySizes[Index]);
				}
				bOneSize = bOneSize && NameSizes.Num() > 0 && FMath::IsNearlyEqual(NameLo, NameHi, 0.01f)
					&& FMath::IsNearlyEqual(BodyLo, BodyHi, 0.01f);
				Detail += FString::Printf(TEXT("%.0fx1080: %d cards, names %.1f-%.1f, bodies %.1f-%.1f  "),
					ViewWidth, NameSizes.Num(), NameLo, NameHi, BodyLo, BodyHi);
			}
			Check(TEXT("*** one name size and one description size on every tab ***"), bOneSize, Detail);
		}

		// ---- EVERY CHIP AND LEGEND NAMES THE KEYBOARD, AND THE PAD BESIDE IT -----------------------
		{
			const TraceLoadoutSelectFile::FChipKeys TabLeftKeys =
				TraceLoadoutSelectFile::ChipKeys(TraceLoadoutSelectFile::EChip::TabLeft, false, true);
			const TraceLoadoutSelectFile::FChipKeys LockKeys =
				TraceLoadoutSelectFile::ChipKeys(TraceLoadoutSelectFile::EChip::Confirm, false, true);
			const TraceLoadoutSelectFile::FChipKeys LockKeysNoPad =
				TraceLoadoutSelectFile::ChipKeys(TraceLoadoutSelectFile::EChip::Confirm, false, false);
			auto JoinChips = [](const TraceLoadoutSelectFile::FChipKeys& Chips)
			{
				FString Joined;
				for (int32 ChipIndex = 0; ChipIndex < Chips.Num; ++ChipIndex)
				{
					Joined += (ChipIndex > 0 ? TEXT(" ") : TEXT("")) + Chips[ChipIndex];
				}
				return Joined;
			};
			Check(TEXT("*** chips: Q and LB, F and X once a pad is seen; F alone before ***"),
				TabLeftKeys.Num == 2 && TabLeftKeys[0] == TRACE_TEXT("LOADOUT.KEY_TAB_LEFT", "Q")
					&& TabLeftKeys[1] == TRACE_TEXT("LOADOUT.PAD_KEY_TAB_LEFT", "LB")
					&& LockKeys.Num == 2 && LockKeys[0] == TRACE_TEXT("LOADOUT.KEY_LOCK_IN", "F")
					&& LockKeysNoPad.Num == 1 && LockKeysNoPad[0] == LockKeys[0],
				FString::Printf(TEXT("tab left [%s], lock in [%s], lock in with no pad [%s]"),
					*JoinChips(TabLeftKeys), *JoinChips(LockKeys), *JoinChips(LockKeysNoPad)));

			auto HasItem = [](const TArray<FTraceKitLegendItem>& Items, const FString& Key, const FString& Verb)
			{
				return Items.ContainsByPredicate([&Key, &Verb](const FTraceKitLegendItem& Item)
				{
					return Item.Key == Key && Item.Label == Verb;
				});
			};
			const FString BackWord = TRACE_TEXT("LOADOUT.BACK", "BACK");
			const FString KeyBack = TRACE_TEXT("LOADOUT.KEY_BACK_TAB", "BKSP");
			const FString PadBack = TRACE_TEXT("LOADOUT.PAD_KEY_BACK", "B");
			TArray<FTraceKitLegendItem> FirstKeys, FirstPad, LaterKeys, LaterPad, NoPadKeys, NoPadPad;
			TraceLoadoutSelectFile::BuildLegend(false, 0, true, FirstKeys, FirstPad);
			TraceLoadoutSelectFile::BuildLegend(false, 2, true, LaterKeys, LaterPad);
			TraceLoadoutSelectFile::BuildLegend(false, 2, false, NoPadKeys, NoPadPad);
			Check(TEXT("*** legend: BKSP / B BACK on the tabs after the first ***"),
				!HasItem(FirstKeys, KeyBack, BackWord) && !HasItem(FirstPad, PadBack, BackWord)
					&& HasItem(LaterKeys, KeyBack, BackWord) && HasItem(LaterPad, PadBack, BackWord),
				TEXT("BACKSPACE and pad B step back a tab and the legend never said so"));
			Check(TEXT("*** legend: the pad line has Y LOAD and LT + Y SAVE; no pad, no pad line ***"),
				HasItem(LaterPad, TRACE_TEXT("LOADOUT.PAD_KEY_LOAD", "Y"), TRACE_TEXT("LOADOUT.LEGEND_LOAD", "LOAD"))
					&& HasItem(LaterPad, TRACE_TEXT("LOADOUT.PAD_KEY_STORE", "LT + Y"), TRACE_TEXT("LOADOUT.LEGEND_STORE", "SAVE"))
					&& NoPadPad.Num() == 0 && NoPadKeys.Num() == LaterKeys.Num(),
				FString::Printf(TEXT("pad line %d items, keyboard %d, no-pad pad line %d"), LaterPad.Num(),
					LaterKeys.Num(), NoPadPad.Num()));

			// CONTROLLER INPUT OFF: the page ignores the pad, so it names no pad button either.
			APlayerController* const HintPC = Cast<APlayerController>(Subject->GetOwningController());
			if (HintPC != nullptr && TracePadMenu::HasSeenPad(HintPC))
			{
				UTraceUserSettings& PadSettings = UTraceUserSettings::Get();
				const bool bPadWas = PadSettings.bPadEnabled;
				PadSettings.bPadEnabled = true;
				const bool bHintsOn = TraceLoadoutSelectFile::PadHintsShown(HintPC);
				PadSettings.bPadEnabled = false;
				const bool bHintsOff = TraceLoadoutSelectFile::PadHintsShown(HintPC);
				PadSettings.bPadEnabled = bPadWas;
				Check(TEXT("pad chips and line with CONTROLLER INPUT on, none with it off"), bHintsOn && !bHintsOff,
					FString::Printf(TEXT("on %d, off %d"), bHintsOn ? 1 : 0, bHintsOff ? 1 : 0));
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[LoadoutScreen]   (no pad seen on this machine; the CONTROLLER INPUT hint check did not run)"));
			}
		}

		// ---- A PAD CAN LOAD AND SAVE THE SAVED LOADOUTS -------------------------------------------
		//
		// 1-5 and SHIFT+1-5 are keyboard keys and the slots are pointer targets; a pad could reach
		// neither. Y loads the next usable slot, LT + Y saves. Slots set up and put back here.
		{
			UTraceUserSettings& Settings = UTraceUserSettings::Get();
			TArray<FTraceLoadout> SavedBefore;
			for (int32 Index = 0; Index < UTraceUserSettings::SavedLoadoutCount; ++Index)
			{
				SavedBefore.Add(Settings.GetSavedLoadout(Index));
			}

			FTraceLoadout SlotB;
			SlotB.Movement  = ETraceAbilityId::JetBoots;
			SlotB.Passive   = ETraceAbilityId::Blasters;
			SlotB.Activated = ETraceAbilityId::Ripple;
			FTraceLoadout Illegal = Wanted;
			Illegal.Passive = ETraceAbilityId::StickyGloves;   // a movement ability in the passive slot

			Settings.SetSavedLoadout(0, FTraceLoadout());
			Settings.SetSavedLoadout(1, Wanted);
			Settings.SetSavedLoadout(2, FTraceLoadout());
			Settings.SetSavedLoadout(3, SlotB);
			Settings.SetSavedLoadout(4, Illegal);

			const FTraceLoadoutKeys PadY = FTraceLoadoutSelect::DebugKeysFor(
				TArray<FKey>{ EKeys::Gamepad_FaceButton_Top }, false);
			const FTraceLoadoutKeys PadLtY = FTraceLoadoutSelect::DebugKeysFor(
				TArray<FKey>{ EKeys::Gamepad_LeftTrigger, EKeys::Gamepad_FaceButton_Top }, false);
			Check(TEXT("pad Y is the saved-loadout key, LT its SHIFT"),
				PadY.bSavedNext && !PadY.bShift && PadLtY.bSavedNext && PadLtY.bShift
					&& TraceLoadoutSelect::ReadsKey(EKeys::Gamepad_FaceButton_Top)
					&& TraceLoadoutSelect::ReadsKey(EKeys::Gamepad_LeftTrigger),
				TEXT("and both are the page's, so a press on it cannot come back as ABILITY or PARRY"));

			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/true, 0.f);
			FTraceLoadoutSelect Page;
			float Clock = 400.f;
			auto Frame = [&Page, Subject, &Clock](const FTraceLoadoutKeys& FrameKeys)
			{
				Clock += 0.05f;
				Page.DebugInput(FrameKeys, Subject, /*bInputAllowed=*/true, Clock);
			};
			const FTraceLoadoutKeys Nothing;
			FTraceLoadoutKeys Right;  Right.NavX = 1;
			FTraceLoadoutKeys Enter;  Enter.bEquip = true;

			Page.DebugBeginInput(Subject);
			Frame(Nothing);
			Frame(PadY);
			Frame(Nothing);
			const FTraceLoadout FirstY = Page.GetStaged();
			Frame(PadY);
			Frame(Nothing);
			const FTraceLoadout SecondY = Page.GetStaged();
			Frame(PadY);
			Frame(Nothing);
			const FTraceLoadout ThirdY = Page.GetStaged();
			Check(TEXT("*** pad Y loads slot 2, then 4, then 2 (empty and illegal slots passed) ***"),
				FirstY == Wanted && SecondY == SlotB && ThirdY == Wanted,
				FString::Printf(TEXT("%s / %s / %s"), *TraceLoadoutToString(FirstY), *TraceLoadoutToString(SecondY),
					*TraceLoadoutToString(ThirdY)));

			// Change a card, LT + Y: back into slot 2, the one just loaded.
			Frame(Right);
			Frame(Nothing);
			Frame(Enter);
			Frame(Nothing);
			const FTraceLoadout Edited = Page.GetStaged();
			Frame(PadLtY);
			Frame(Nothing);
			Check(TEXT("*** pad LT + Y saves back into the slot just loaded ***"),
				Edited != Wanted && Settings.GetSavedLoadout(1) == Edited && Settings.GetSavedLoadout(0).IsEmpty(),
				FString::Printf(TEXT("slot 2 now %s"), *TraceLoadoutToString(Settings.GetSavedLoadout(1))));

			// A fresh opening with nothing loaded: LT + Y goes to the first EMPTY slot, never over one.
			FTraceLoadoutSelect Fresh;
			float FreshClock = 500.f;
			Fresh.DebugBeginInput(Subject);
			Fresh.DebugInput(Nothing, Subject, true, FreshClock += 0.05f);
			Fresh.DebugInput(PadLtY, Subject, true, FreshClock += 0.05f);
			Fresh.DebugInput(Nothing, Subject, true, FreshClock += 0.05f);
			Check(TEXT("pad LT + Y with nothing loaded saves to the first empty slot"),
				Settings.GetSavedLoadout(0) == Fresh.GetStaged() && Settings.GetSavedLoadout(1) == Edited
					&& Settings.GetSavedLoadout(3) == SlotB,
				FString::Printf(TEXT("slot 1 now %s"), *TraceLoadoutToString(Settings.GetSavedLoadout(0))));

			// ---- AN EMPTY SLOT IS A SAVE TARGET, NOT A DEAD ONE ------------------------------------
			//
			// Slot 3 is still empty and slot 5 still illegal. The empty one was drawn on the DISABLED
			// plate, which on this kit means "cannot act", while SHIFT+click, SHIFT+3 and LT + Y all
			// save into it; and it never took the pointer's hover, even with SHIFT down. Driven through
			// the look DrawSavedRow uses and the real pointer path, against 1080p slot rects.
			{
				const TraceLoadoutSelectFile::FSavedSlotLook EmptyLook =
					TraceLoadoutSelectFile::SavedSlotLook(Settings.GetSavedLoadout(2), /*bLit=*/false, /*bPressed=*/false);
				const TraceLoadoutSelectFile::FSavedSlotLook RefusedLook =
					TraceLoadoutSelectFile::SavedSlotLook(Settings.GetSavedLoadout(4), false, false);
				const TraceLoadoutSelectFile::FSavedSlotLook FullLook =
					TraceLoadoutSelectFile::SavedSlotLook(Settings.GetSavedLoadout(3), false, false);
				Check(TEXT("*** an empty SAVED slot is the navy plate with a quiet number, not DISABLED ***"),
					Settings.GetSavedLoadout(2).IsEmpty() && EmptyLook.State == ETraceKitState::Default && EmptyLook.bQuietWord
						&& RefusedLook.State == ETraceKitState::Disabled
						&& FullLook.State == ETraceKitState::Default && !FullLook.bQuietWord,
					FString::Printf(TEXT("empty: state %d quiet %d; refused: state %d; full: state %d quiet %d"),
						static_cast<int32>(EmptyLook.State), EmptyLook.bQuietWord ? 1 : 0, static_cast<int32>(RefusedLook.State),
						static_cast<int32>(FullLook.State), FullLook.bQuietWord ? 1 : 0));

				FTraceLoadoutSelect SlotPage;
				SlotPage.DebugBeginInput(Subject);
				for (int32 SlotIndex = 0; SlotIndex < UTraceUserSettings::SavedLoadoutCount; ++SlotIndex)
				{
					const float SlotLeft = 262.f + 212.f * SlotIndex;
					SlotPage.DebugSetSavedRect(SlotIndex, FBox2D(FVector2D(SlotLeft, 888.f), FVector2D(SlotLeft + 200.f, 942.f)));
				}
				const FVector2D OnEmpty(262.f + 212.f * 2.f + 100.f, 915.f);
				SlotPage.DebugPointer(OnEmpty + FVector2D(0.f, -40.f), false, Subject);   // the first sample: not a move
				SlotPage.DebugPointer(OnEmpty, false, Subject);                           // onto slot 3
				const FTraceLoadoutLit PlainLit = SlotPage.DebugLit();
				SlotPage.DebugPointer(OnEmpty, false, Subject, /*bShiftHeld=*/true);
				const FTraceLoadoutLit ShiftLit = SlotPage.DebugLit();
				SlotPage.DebugPointer(OnEmpty, true, Subject, true);    // SHIFT+click: the press...
				SlotPage.DebugPointer(OnEmpty, false, Subject, true);   // ...and the release, which saves
				Check(TEXT("*** SHIFT lights an empty slot under the pointer; SHIFT+click saves there ***"),
					PlainLit.Saved == INDEX_NONE && ShiftLit.Saved == 2 && ShiftLit.Count() == 1
						&& Settings.GetSavedLoadout(2) == SlotPage.GetStaged() && !SlotPage.GetStaged().IsEmpty(),
					FString::Printf(TEXT("no SHIFT: saved lit %d; SHIFT: saved lit %d (%d lit); slot 3 now %s"),
						PlainLit.Saved, ShiftLit.Saved, ShiftLit.Count(), *TraceLoadoutToString(Settings.GetSavedLoadout(2))));
			}

			for (int32 Index = 0; Index < SavedBefore.Num(); ++Index)
			{
				Settings.SetSavedLoadout(Index, SavedBefore[Index]);
			}
			Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
		}

		// ---- TWO PAGES, TWO HOVER SALTS ------------------------------------------------------------
		//
		// The match page and the pause menu's LOADOUTS editor draw the same rects in one frame; each
		// draws under its own salt (Trace.UI.Kit.Verify proves two salts keep two blends).
		{
			FTraceLoadoutSelect Other;
			Check(TEXT("two loadout pages draw under two different hover salts"),
				Screen.HoverSalt() != 0u && Other.HoverSalt() != 0u && Screen.HoverSalt() != Other.HoverSalt(),
				FString::Printf(TEXT("0x%08x / 0x%08x"), Screen.HoverSalt(), Other.HoverSalt()));
		}

		// ---- THE LOCK'S RULE, AS THE SERVER AND THE TEAM SCREEN BOTH ASK IT -------------------------
		Check(TEXT("CHANGE LOADOUT: allowed before the match and at half time"),
			TraceLoadoutSelect::IsReopenAllowed(ETraceMatchState::WaitingForPlayers, false, true)
				&& TraceLoadoutSelect::IsReopenAllowed(ETraceMatchState::InProgress, true, true),
			TEXT(""));
		Check(TEXT("CHANGE LOADOUT: refused in live play and after the match"),
			!TraceLoadoutSelect::IsReopenAllowed(ETraceMatchState::InProgress, false, true)
				&& !TraceLoadoutSelect::IsReopenAllowed(ETraceMatchState::PostMatch, false, true),
			TEXT("\"loadout is locked until halftime\""));
		Check(TEXT("...and the legacy character page keeps its mid-match switch"),
			TraceLoadoutSelect::IsReopenAllowed(ETraceMatchState::InProgress, false, false),
			TEXT("Trace.UI.LoadoutScreen 0"));

		// ---- ON THE SERVER: THE HANDOFF, AND C / X DURING PLAY AND AT HALF TIME ----------------------
		{
			ATracePlayerController* const SubjectPC = Cast<ATracePlayerController>(Subject->GetOwningController());
			ATraceGameState* const LiveGS = World->GetGameState<ATraceGameState>();
			if (SubjectPC == nullptr || LiveGS == nullptr || !UTraceAbilityComponent::AreCharactersEnabled(World))
			{
				Check(TEXT("server checks ran"), false,
					TEXT("INCONCLUSIVE: needs a human player controller, a Trace game state and characters on"));
			}
			else
			{
				const bool bWasBreak = LiveGS->IsHalfTimeBreak();
				const int32 HalfNow = LiveGS->CurrentHalf;
				const int32 HalvesNow = FMath::Max(1, LiveGS->NumHalves);

				// (a) THE HANDOFF: closing the team screen opens the select window in the same call.
				if (!bWasBreak && !Subject->HasCharacter())
				{
					Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
					Subject->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);
					SubjectPC->ServerSetTeamSelectOpen(/*bOpen=*/true, 0.f);
					SubjectPC->ServerSetTeamSelectOpen(/*bOpen=*/false, 0.f);
					Check(TEXT("*** closing team select opens the loadout in the SAME call ***"),
						Subject->IsCharacterSelectOpen(),
						TEXT("it waited for the 4 Hz poll: live input and the arena between two menus"));
				}
				else
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[LoadoutScreen]   (the subject holds a character id; the handoff check did not run)"));
				}

				// (b) C / X DURING LIVE PLAY is refused, and changes nothing.
				Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
				Subject->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/true);
				const FTraceLoadout Held = Comp->GetLoadout();
				if (LiveGS->TraceMatchState == ETraceMatchState::InProgress && !bWasBreak && TraceLoadoutSelect::IsArmed())
				{
					SubjectPC->DebugResetTeamRequestCooldown();
					SubjectPC->ServerRequestCharacterSwitch();
					Check(TEXT("*** CHANGE LOADOUT during live play is refused ***"),
						!Subject->IsCharacterSelectOpen() && Subject->IsCharacterLocked() && Comp->GetLoadout() == Held,
						FString::Printf(TEXT("window %d, locked %d, loadout %s"), Subject->IsCharacterSelectOpen() ? 1 : 0,
							Subject->IsCharacterLocked() ? 1 : 0, *TraceLoadoutToString(Comp->GetLoadout())));
				}
				else
				{
					Check(TEXT("CHANGE LOADOUT during live play is refused"), false,
						TEXT("INCONCLUSIVE: run this in live play (after warm-up, not in the break)"));
				}

				// (c) ...AND AT HALF TIME IT REOPENS THE WINDOW AT ONCE, KEEPING THE LOCK AND THE LOADOUT.
				Subject->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
				Subject->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/true);
				LiveGS->SetHalfState(HalfNow, HalvesNow, /*bInHalfTimeBreak=*/true);
				SubjectPC->DebugResetTeamRequestCooldown();
				SubjectPC->ServerRequestCharacterSwitch();
				Check(TEXT("*** CHANGE LOADOUT at half time reopens the page now, lock kept ***"),
					Subject->IsCharacterSelectOpen() && Subject->IsCharacterLocked() && Comp->GetLoadout() == Held,
					TEXT("the poll stands off in the break, so waiting for it opened the page in the 2nd half"));
				LiveGS->SetHalfState(HalfNow, HalvesNow, bWasBreak);
				SubjectPC->DebugResetTeamRequestCooldown();
			}
		}

		Comp->ApplyLoadout(Restore);
		Subject->ServerMarkCharacterResolved(bRestoreLocked, bRestoreChosen);
		Subject->ServerSetCharacterSelectOpen(bRestoreSelect, RestoreDeadline);

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[LoadoutScreen] ===== PASS — the screen stages, keys and pad build a loadout, the "
				     "server decides, and every ability is named the way it always was. ====="));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[LoadoutScreen] ===== *** FAIL *** %d check(s) — see above ====="), Failures);
		}
	}

	FAutoConsoleCommand Cmd(
		TEXT("Trace.Loadout.Screen"),
		TEXT("Drive the loadout screen end to end: keys and pad equip, change tab and lock in; nothing "
		     "held through a gate fires; the pick reaches the server; closing team select opens the page "
		     "at once; CHANGE LOADOUT obeys the lock; the ability names are the roster's originals. Run "
		     "in live play."),
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
		Wanted.Movement  = ETraceAbilityId::StickyGloves;
		Wanted.Passive   = ETraceAbilityId::Magnet;
		Wanted.Activated = ETraceAbilityId::Snap;

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
		Screen.DebugPick(ETraceLoadoutSlot::Movement,  ETraceAbilityId::JetBoots);
		Screen.DebugPick(ETraceLoadoutSlot::Passive,   ETraceAbilityId::Blasters);
		Screen.DebugPick(ETraceLoadoutSlot::Activated, ETraceAbilityId::Ripple);

		Screen.DebugRecall(3);
		Check(TEXT("recalling slot 4 loads it into the page"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		// Slot 2 made empty FIRST: this check used to lean on the player's own slot 2 happening to be
		// empty, and failed on a machine where someone had saved there. Put back with the rest below.
		Settings.SetSavedLoadout(1, FTraceLoadout());
		Screen.DebugRecall(1);
		Check(TEXT("recalling an EMPTY slot changes nothing"),
			Screen.GetStaged() == Wanted, TraceLoadoutToString(Screen.GetStaged()));

		Screen.DebugPick(ETraceLoadoutSlot::Movement, ETraceAbilityId::Overload);
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
