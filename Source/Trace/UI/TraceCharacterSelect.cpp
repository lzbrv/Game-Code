// Trace — the character select screen. See TraceCharacterSelect.h.

#include "UI/TraceCharacterSelect.h"

#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"           // FPlatformTime::Cycles64 — the click harness's injector
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"          // FInputKeyEventArgs — same injector
#include "Misc/CoreMiscDefines.h"       // FInputDeviceId

#include "Core/TracePlayerState.h"
#include "Trace.h"                      // LogTraceGame
#include "TraceTypes.h"                 // TraceTeamColor / TraceTeamName

#if !UE_BUILD_SHIPPING
int32 GTraceCharacterSelectDebugPick = 0;
int32 GTraceCharacterSelectClickTest = 0;

// NAMED, not anonymous: UBT compiles this module as a unity/jumbo build, so two files that each
// define something at the top of an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
namespace TraceCharSelectClickTest
{
	/** Frames between stages. Enough that a Tick, and therefore a Draw and a PollInput, lands between. */
	constexpr uint64 FramesPerStage = 6;

	/** Complete down/up pairs a card is given before the test is called a failure. */
	constexpr int32 MaxPairs = 3;

	/** One key edge through the same entry point a physical mouse reaches. */
	void InjectKey(APlayerController* PC, const FKey& Key, bool bPressed)
	{
		if (PC == nullptr)
		{
			return;
		}

		// Internal id 0 rather than IPlatformInputDeviceMapper::GetDefaultInputDevice(): that lives in
		// the ApplicationCore module, and desktop maps keyboard and mouse to id 0. Same call and same
		// reasoning as ATraceMenuHUD's injector.
		const FInputKeyEventArgs Args(
			/*Viewport*/ nullptr,
			FInputDeviceId::CreateFromInternalId(0),
			Key,
			bPressed ? IE_Pressed : IE_Released,
			/*AmountDepressed*/ bPressed ? 1.f : 0.f,
			/*bIsTouchEvent*/ false,
			FPlatformTime::Cycles64());

		PC->InputKey(Args);
	}
}
#endif

namespace TraceSelectStyle
{
	// The same neon instrument-panel palette the title screen and the options overlay use, so this
	// screen reads as another page of one machine rather than as a dialog dropped on top of it.
	static const FLinearColor Cyan   (0.16f, 0.88f, 1.00f, 1.00f);
	static const FLinearColor Ink    (0.90f, 0.97f, 1.00f, 1.00f);
	static const FLinearColor InkDim (0.42f, 0.58f, 0.66f, 1.00f);
	static const FLinearColor Panel  (0.004f, 0.014f, 0.026f, 0.96f);
	static const FLinearColor Danger (0.95f, 0.28f, 0.22f, 1.00f);
	static const FLinearColor Good   (0.24f, 0.90f, 0.42f, 1.00f);

	/**
	 * Full-screen scrim. Opaque enough to read a card over, transparent enough to keep the arena.
	 *
	 * 0.94 rather than the 0.86 this shipped with for one frame: the first capture showed the
	 * bottom-left ability stack (DASH / WEAPON / health) and the top-centre score panel reading
	 * clearly THROUGH the overlay and colliding with this screen's own footer. A menu you can read a
	 * health bar through is a menu that looks broken.
	 */
	static const FLinearColor Scrim  (0.00f, 0.005f, 0.02f, 0.94f);

	static FLinearColor WithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, A);
	}

	/** Everything below is authored against a 1080p-tall viewport, exactly like the HUD. */
	static constexpr float ReferenceHeight = 1080.f;

	/** How long a server verdict stays on screen. Long enough to read at a glance, short enough to go. */
	static constexpr float MessageDuration = 3.5f;
}

/**
 * THE CARD GRID — spec v18 §2, because eight cards do not fit the row that five did.
 *
 * Everything here is compile-time, derived from TraceCharacterRoster::Count and nothing else, which
 * is what lets PollInput (which runs BEFORE the first Draw of a frame) and Draw agree about where the
 * rows are without one of them caching the other's arithmetic.
 *
 * WHY IT WRAPS RATHER THAN JUST GETTING NARROWER. Measured at the 1280x720 the harnesses capture:
 * five cards across leave 219 px of text width per card, and Oyster's — the longest — fills most of
 * the 280 px of card height that leaves. Eight across would have given 127 px of text width, i.e.
 * roughly DOUBLE the wrapped lines in a card that is no taller, so every long card would have run its
 * activated block off the bottom edge. Two rows of four give 280 px of text width — WIDER than the
 * five-card row this replaces — at a little over half the height, which is the trade that fits.
 *
 * A ROSTER OF FIVE OR FEWER STILL DRAWS EXACTLY AS IT DID: Rows collapses to 1 and every number
 * below reduces to the old expression. That is deliberate, so this change cannot be what altered the
 * look of a screen nobody asked to change.
 */
namespace TraceSelectGrid
{
	/** Cards per row before the screen wraps. Five is what the pre-v18 layout was authored against. */
	constexpr int32 MaxPerRow = 5;

	/** 1 up to five characters, 2 beyond. A third row would need the header to shrink; say so then. */
	constexpr int32 Rows = (TraceCharacterRoster::Count <= MaxPerRow) ? 1 : 2;

	/** Rounded UP, so with 9 characters the last row is the short one and gets centred. */
	constexpr int32 Columns = (TraceCharacterRoster::Count + Rows - 1) / Rows;

	static_assert(Rows * Columns >= TraceCharacterRoster::Count,
		"The card grid must have room for every character, or the last ones would never be drawn - "
		"and an undrawn card cannot be clicked, because the hit test reads the rects the draw left.");
}

// NAMED after the file rather than anonymous. UBT compiles this module as a unity/jumbo build, so two
// files that each open `namespace { }` become ONE namespace holding both sets of definitions, and
// "NumberKeyForIndex" is exactly the kind of name a second UI file would also want.
// Scripts/check-jumbo-build-collisions.py gates the build on it; this used to be anonymous.
namespace TraceCharacterSelectFile
{
	/**
	 * The number keys, in roster order, so "press 3 for Mace" is literally true.
	 *
	 * ONE KEY PER CARD ONLY WORKS WHILE THE ROSTER FITS THE NUMBER ROW, AND SPEC v19 §3 IS EXACTLY
	 * WHERE IT RUNS OUT. This used to stop at 9 and hand the tenth card nothing but the arrows and
	 * the mouse; the tenth character now exists (Lily), so the tenth card takes ZERO — the last key
	 * on the number row, and the one every player already reads as "ten".
	 *
	 * IT STOPS THERE FOR REAL. An ELEVENTH character has no key left and gets EKeys::Invalid, which
	 * is deliberate rather than an omission: WasInputKeyJustPressed on Invalid is simply always
	 * false, so a card is never picked by a key that is really some other card's. The next roster
	 * addition needs a different scheme (modifiers, or a second page), not another entry here.
	 */
	const FKey& NumberKeyForIndex(int32 Index)
	{
		switch (Index)
		{
		case 0:  return EKeys::One;
		case 1:  return EKeys::Two;
		case 2:  return EKeys::Three;
		case 3:  return EKeys::Four;
		case 4:  return EKeys::Five;
		case 5:  return EKeys::Six;
		case 6:  return EKeys::Seven;
		case 7:  return EKeys::Eight;
		case 8:  return EKeys::Nine;
		case 9:  return EKeys::Zero;      // spec v19 §3 — Lily. "0" is the tenth key, not a tenth name.
		default: return EKeys::Invalid;
		}
	}

	/** "TAKEN BY BOB" needs the holder's name; APlayerState::GetPlayerName is not const-safe to call blind. */
	FString SafePlayerName(const ATracePlayerState* State)
	{
		if (State == nullptr)
		{
			return FString(TEXT("A TEAM-MATE"));
		}

		// The BOT suffix is not decoration. Since spec v15 §2 a card can be greyed out by a computer
		// team-mate, and "TAKEN BY BOT BLUE 3" is the difference between a player understanding why
		// and a player thinking the screen is broken. The bot names this project generates already
		// begin with "BOT ", so the suffix is only added when they do not — a server may rename them.
		const FString Name = State->GetPlayerName();
		if (State->IsABot() && !Name.StartsWith(TEXT("BOT"), ESearchCase::IgnoreCase))
		{
			return Name + TEXT(" (BOT)");
		}

		return Name;
	}
}

// =============================================================================================
// Lifecycle + input
// =============================================================================================

void FTraceCharacterSelect::Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
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

	if (GEngine != nullptr)
	{
		FontSmall  = GEngine->GetSmallFont();
		FontMedium = GEngine->GetMediumFont();
		FontLarge  = GEngine->GetLargeFont();
	}

	// THE ONLY CONDITION. Replicated from the server, so mode A, the settings toggle, bots and
	// "already picked" are all answered upstream and none of them are re-derived here. See the
	// header for why that matters.
	const bool bShouldBeOpen = (LocalState != nullptr) && LocalState->IsCharacterSelectOpen();

	if (bShouldBeOpen != bOpen)
	{
		bOpen = bShouldBeOpen;

		if (bOpen)
		{
			HoveredCard = INDEX_NONE;
			PendingRequest = TraceCharacterRoster::NoneId;

			// FBox2D's default constructor leaves bIsValid UNINITIALISED, and PollInput runs before
			// Draw on this very frame — so without this the first frame's hit test would read garbage
			// and could report the pointer as being inside a card that has never been drawn. Cleared
			// here rather than in a member initialiser so the array's size stays a single constant.
			for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
			{
				CardRects[Index] = FBox2D(ForceInit);
			}

			// Swallow the remainder of this frame's input. Without it, the key that was being held
			// when the screen appeared — most often a movement key during warm-up — lands on a card.
			IgnoreInputBeforeFrame = GFrameCounter + 1;

			// Start the highlight on the first character no team-mate is believed to hold, so the
			// default action is a legal one rather than one that will be refused.
			Highlighted = 0;
			bool bFoundFree = false;

			// The believed roster, printed once per opening.
			//
			// It is here because spec v15 §2 made this screen's belief able to be wrong in a NEW way:
			// a BOT team-mate can now hold a card, and until §2 this function skipped bots entirely.
			// A greyed card is otherwise invisible to a headless run and indistinguishable in a
			// screenshot from a card that failed to draw, so the screen says out loud what it thinks
			// is taken and by whom — which is also the first thing to read when a player reports
			// "it would not let me pick".
			FString RosterBelief;
			for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
			{
				const uint8 CandidateId = static_cast<uint8>(TraceCharacterRoster::FirstId + Index);
				const ATracePlayerState* const Holder = FindTeammateHolding(LocalState, CandidateId);

				if (Holder == nullptr && !bFoundFree)
				{
					Highlighted = Index;
					bFoundFree = true;
				}

				RosterBelief += FString::Printf(TEXT("%s%s=%s"),
					(Index > 0) ? TEXT(" ") : TEXT(""),
					*TraceCharacterRoster::NameFor(CandidateId),
					(Holder != nullptr) ? *TraceCharacterSelectFile::SafePlayerName(Holder).ToUpper() : TEXT("free"));
			}

			UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Screen opened (team %s, %.0fs to pick). Believes: %s"),
				*TraceTeamName(LocalState->Team).ToString(), LocalState->GetCharacterSelectTimeRemaining(),
				*RosterBelief);

			if (OnOpened)
			{
				OnOpened();
			}
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Screen closed."));
#if !UE_BUILD_SHIPPING
			// A click that WORKS closes the screen, and Tick returns below without ever reaching the
			// judging stage. Reporting here is what makes the passing run the one that prints a
			// verdict, instead of the one that goes quiet.
			ReportClickTest(LocalState);
#endif
			if (OnClosed)
			{
				OnClosed();
			}
		}
	}

	if (!bOpen)
	{
		return;
	}

	// A request that never came back must not lock the screen. Reliable RPCs do not get dropped, but
	// a server that travelled mid-request, or a listen server that lost its game mode, would leave
	// this pending forever — and an unresponsive select screen with a running auto-pick timer is the
	// worst combination this feature can produce.
	if (PendingRequest != TraceCharacterRoster::NoneId && (Now - PendingRequestTime) > PendingRequestTimeout)
	{
		PendingRequest = TraceCharacterRoster::NoneId;
	}

	if (bInputAllowed && PC != nullptr && GFrameCounter >= IgnoreInputBeforeFrame)
	{
		PollInput(PC, LocalState);
	}

#if !UE_BUILD_SHIPPING
	// Consumed here rather than acted on inside the console command, so the scripted pick lands on a
	// frame where the screen is genuinely up and the rects are genuinely drawn.
	if (GTraceCharacterSelectDebugPick != 0)
	{
		const int32 RequestedId = GTraceCharacterSelectDebugPick;
		GTraceCharacterSelectDebugPick = 0;
		DebugPick(RequestedId);
		ConfirmHighlighted(LocalState);
	}

	// Spec v15 §4. Armed the same way, and for the same reason: the cards have to have been DRAWN
	// before a cursor can be parked on one.
	if (GTraceCharacterSelectClickTest != 0 && ClickTestCard == INDEX_NONE)
	{
		ClickTestCard = FMath::Clamp(GTraceCharacterSelectClickTest, 1, TraceCharacterRoster::Count) - 1;
		GTraceCharacterSelectClickTest = 0;
		ClickTestStage = 0;
		ClickTestClicks = 0;
		ClickTestNextFrame = GFrameCounter;
	}
	if (ClickTestCard != INDEX_NONE)
	{
		ClickTestStep(PC, LocalState);
	}
#endif

	Draw(HUD, LocalState);
}

void FTraceCharacterSelect::PollInput(APlayerController* PC, ATracePlayerState* LocalState)
{
	// ---- Direct number keys. One key per card — no walking required. -----------------------------
	for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
	{
		if (PC->WasInputKeyJustPressed(TraceCharacterSelectFile::NumberKeyForIndex(Index)))
		{
			Highlighted = Index;
			ConfirmHighlighted(LocalState);
			return;
		}
	}

	// ---- Left / right / up / down, with repeat ---------------------------------------------------
	//
	// UP AND DOWN ARE NEW IN SPEC v18 §2 and they exist because the cards do. With one row they moved
	// nothing and were not offered; with two, a player who wants the card directly below theirs would
	// otherwise have to walk the whole rest of the top row to reach it. Left/right still walk the
	// roster in reading order and still step from the end of one row to the start of the next, which
	// is what makes "press right four times from ROCCO" land where the numbers say it should.
	const bool bLeft  = PC->IsInputKeyDown(EKeys::Left)  || PC->IsInputKeyDown(EKeys::A);
	const bool bRight = PC->IsInputKeyDown(EKeys::Right) || PC->IsInputKeyDown(EKeys::D);
	// bNavUp / bNavDown rather than bUp / bDown: `bDown` is already the LEFT MOUSE BUTTON further down
	// this same function, and clang caught the collision as a redefinition. Worth the ugly prefix —
	// had the mouse's line come FIRST this would have been a shadow in a nested scope instead, which
	// MSVC reports as C4458/C4459 and this platform structurally cannot see.
	const bool bNavUp   = PC->IsInputKeyDown(EKeys::Up)   || PC->IsInputKeyDown(EKeys::W);
	const bool bNavDown = PC->IsInputKeyDown(EKeys::Down) || PC->IsInputKeyDown(EKeys::S);

	// One repeat clock for both axes, and horizontal wins a diagonal. Two independent clocks would
	// let a player holding right-and-down travel twice as fast as one holding either.
	int32 NavDir = (bRight ? 1 : 0) - (bLeft ? 1 : 0);
	if (NavDir == 0)
	{
		NavDir = ((bNavDown ? 1 : 0) - (bNavUp ? 1 : 0)) * TraceSelectGrid::Columns;
	}

	if (NavDir != 0)
	{
		if (NavDir != LastNavDir)
		{
			LastNavDir = NavDir;
			NextNavTime = Now + NavRepeatDelay;
			MoveHighlight(NavDir);
		}
		else if (Now >= NextNavTime)
		{
			NextNavTime = Now + NavRepeatInterval;
			MoveHighlight(NavDir);
		}
	}
	else
	{
		LastNavDir = 0;
	}

	// ---- Commit ---------------------------------------------------------------------------------
	if (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::SpaceBar))
	{
		ConfirmHighlighted(LocalState);
		return;
	}

	// ---- Mouse ----------------------------------------------------------------------------------
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (PC->GetMousePosition(MouseX, MouseY))
	{
		CursorPos = FVector2D(MouseX, MouseY);
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
	for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
	{
		if (CardRects[Index].bIsValid && CardRects[Index].IsInside(CursorPos))
		{
			HoveredCard = Index;
			Highlighted = Index;
			break;
		}
	}

	// ACTIVATION ON RELEASE, matching the options overlay: a press that started on one card and
	// finished on another is a slip, not a choice, and this is a decision the player lives with for
	// the whole match.
	if (bJustReleased && HoveredCard != INDEX_NONE)
	{
		ConfirmHighlighted(LocalState);
	}
}

void FTraceCharacterSelect::MoveHighlight(int32 Delta)
{
	// CLAMPED, NOT WRAPPED. Wrapping from the last card back to the first makes a held key cycle
	// forever and makes the highlight's position uninformative about where the ends are. Same call
	// the options overlay makes for the same reason.
	//
	// One clamp covers both axes because Delta is already an index step: +/-1 walks the roster and
	// +/-Columns is a row. Down from the bottom row lands on the last card rather than doing nothing,
	// which is the behaviour a player reads as "that is the end" instead of "that key is broken".
	Highlighted = FMath::Clamp(Highlighted + Delta, 0, TraceCharacterRoster::Count - 1);
}

void FTraceCharacterSelect::ConfirmHighlighted(ATracePlayerState* LocalState)
{
	if (LocalState == nullptr || !TraceCharacterRoster::All().IsValidIndex(Highlighted))
	{
		return;
	}

	const uint8 RequestedId = TraceCharacterRoster::All()[Highlighted].Id;

	// One request in flight at a time. Without this a held Enter sends one per frame, and each one is
	// a reliable RPC the server must process — the second onwards would all come back AlreadyLocked
	// and the screen would end on a refusal message for a pick that actually succeeded.
	if (PendingRequest != TraceCharacterRoster::NoneId)
	{
		return;
	}

	// A card we believe a team-mate holds is not sent. This is the ONLY place local belief is allowed
	// to stop anything, and it is a courtesy rather than a rule: it saves a round trip in the common
	// case. If the belief is stale the server refuses, which is the path that actually enforces §3.
	if (const ATracePlayerState* Holder = FindTeammateHolding(LocalState, RequestedId))
	{
		LocalState->LastPickResult = ETraceCharacterPickResult::TakenByTeammate;
		LocalState->LastPickResultCharacter = RequestedId;
		LocalState->LastPickResultLocalTime = Now;

		UE_LOG(LogTraceGame, Verbose, TEXT("[CharSelect] %s is held by team-mate '%s'; not sending."),
			*TraceCharacterRoster::NameFor(RequestedId), *TraceCharacterSelectFile::SafePlayerName(Holder));
		return;
	}

	PendingRequest = RequestedId;
	PendingRequestTime = Now;

	UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Requesting %s."), *TraceCharacterRoster::NameFor(RequestedId));

	// THE ONE CALL OFF THIS SCREEN. Everything about whether it succeeds happens on the server.
	LocalState->ServerRequestCharacter(RequestedId);
}

const ATracePlayerState* FTraceCharacterSelect::FindTeammateHolding(const ATracePlayerState* LocalState, uint8 CharacterId) const
{
	if (LocalState == nullptr || LocalState->Team == ETraceTeam::None || !TraceCharacterRoster::IsValidId(CharacterId))
	{
		return nullptr;
	}

	const UWorld* const ThisWorld = LocalState->GetWorld();
	const AGameStateBase* const BaseGameState = (ThisWorld != nullptr) ? ThisWorld->GetGameState() : nullptr;
	if (BaseGameState == nullptr)
	{
		return nullptr;
	}

	for (APlayerState* const EachState : BaseGameState->PlayerArray)
	{
		const ATracePlayerState* Candidate = Cast<ATracePlayerState>(EachState);
		if (Candidate == nullptr || Candidate == LocalState)
		{
			continue;
		}

		// BOTS ARE CONSULTED NOW. Spec v14 §3 made them permanently characterless and this test used to
		// skip them for that reason; spec v15 §2 reverses it, and a bot team-mate holding MACE greys
		// MACE out exactly as a human team-mate would. Leaving the skip in would have been a card that
		// looked free, was sent, and came back refused — the one outcome this local belief exists to
		// avoid.
		//
		// WHEN THIS ACTUALLY SHOWS SOMETHING. Under §2's ordering the bots on your team hold nothing
		// while your screen is open, so on a fresh match every card is free. The case it is for is the
		// player who joins a match ALREADY IN PROGRESS: the bots filled long ago, and their picks are
		// the only reason a card would be grey.
		//
		// Enemies are still skipped: mirroring the pick is legal (§3), so an enemy holding a character
		// blocks nothing.
		if (Candidate->Team != LocalState->Team)
		{
			continue;
		}

		if (Candidate->GetSelectedCharacter() == CharacterId)
		{
			return Candidate;
		}
	}

	return nullptr;
}

#if !UE_BUILD_SHIPPING
void FTraceCharacterSelect::ReportClickTest(ATracePlayerState* LocalState)
{
	if (ClickTestCard == INDEX_NONE)
	{
		return;
	}

	const uint8 Picked = (LocalState != nullptr) ? LocalState->GetSelectedCharacter() : TraceCharacterRoster::NoneId;
	const bool bPicked = TraceCharacterRoster::IsValidId(Picked);

	if (bPicked && ClickTestClicks == 1)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect.ClickTest] VERDICT: ONE PRESS = ONE ACTION. Card %d locked in %s on click %d."),
			ClickTestCard + 1, *TraceCharacterRoster::NameFor(Picked), ClickTestClicks);
	}
	else if (bPicked)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[CharSelect.ClickTest] VERDICT: A CARD NEEDS MORE THAN ONE CLICK. Card %d locked in %s only on click %d."),
			ClickTestCard + 1, *TraceCharacterRoster::NameFor(Picked), ClickTestClicks);
	}
	else
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[CharSelect.ClickTest] VERDICT: card %d picked NOTHING after %d complete click(s)."),
			ClickTestCard + 1, ClickTestClicks);
	}

	ClickTestCard = INDEX_NONE;
}

void FTraceCharacterSelect::ClickTestStep(APlayerController* PC, ATracePlayerState* LocalState)
{
	if (PC == nullptr || LocalState == nullptr || !CardRects[ClickTestCard].bIsValid)
	{
		return;
	}
	if (GFrameCounter < ClickTestNextFrame)
	{
		return;
	}
	ClickTestNextFrame = GFrameCounter + TraceCharSelectClickTest::FramesPerStage;

	switch (ClickTestStage)
	{
	case 0:
	{
		// SetMouseLocation writes the viewport's cached cursor position, which is the exact value
		// PollInput reads back out of GetMousePosition above.
		const FVector2D Point = CardRects[ClickTestCard].GetCenter();
		PC->SetMouseLocation(FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y));

		float ReadX = 0.f;
		float ReadY = 0.f;
		const bool bReadBack = PC->GetMousePosition(ReadX, ReadY);
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect.ClickTest] card %d: cursor -> (%.0f, %.0f); the screen reads it back as (%.0f, %.0f) valid=%d."),
			ClickTestCard + 1, Point.X, Point.Y, ReadX, ReadY, bReadBack ? 1 : 0);

		if (!bReadBack)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CharSelect.ClickTest] No cursor position in this run, so no click can be aimed. "
				     "That is a harness failure, not a screen failure."));
			ClickTestCard = INDEX_NONE;
			return;
		}
		ClickTestStage = 1;
		return;
	}

	case 1:
		UE_LOG(LogTraceGame, Display, TEXT("[CharSelect.ClickTest] card %d, click %d: pressing."),
			ClickTestCard + 1, ClickTestClicks + 1);
		TraceCharSelectClickTest::InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/true);
		ClickTestStage = 2;
		return;

	case 2:
		TraceCharSelectClickTest::InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/false);
		// Counted HERE, on the release, not at the judging stage below. A successful click closes the
		// screen, which stops Tick calling this function at all — so if the count only moved at
		// judging time the run that PASSES would be the one that never printed a verdict. Measured
		// exactly that way once: "click 1: pressing" then "Requesting OYSTER" and then silence.
		++ClickTestClicks;
		ClickTestStage = 3;
		return;

	default:
		break;
	}

	// JUDGE, a stage after the release. APlayerController::InputKey queues the event for the next
	// input pass and this screen then polls IsInputKeyDown from Tick, so the release needs a pass and
	// a Tick before it can have had any effect. Judging on the release call itself reports every
	// surface as needing one extra click — which is the reported bug, manufactured by the harness.
	//
	// The server's answer, not our own request: PendingRequest is cleared as soon as the reply lands,
	// so a test that watched it would race. A granted pick is the only thing a player would call
	// "the click worked".
	const bool bPicked = TraceCharacterRoster::IsValidId(LocalState->GetSelectedCharacter());

	if (bPicked)
	{
		ReportClickTest(LocalState);
		return;
	}

	if (ClickTestClicks >= TraceCharSelectClickTest::MaxPairs)
	{
		ReportClickTest(LocalState);
		return;
	}

	// Same card, same cursor, another complete click. Back to the press rather than the cursor move:
	// re-parking every attempt would hide a bug that only bites the FIRST click.
	ClickTestStage = 1;
}

void FTraceCharacterSelect::DebugPick(int32 CharacterId)
{
	if (!TraceCharacterRoster::IsValidId(static_cast<uint8>(CharacterId)))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[CharSelect] Trace.Characters.Select: %d is not %d..%d."),
			CharacterId, static_cast<int32>(TraceCharacterRoster::FirstId),
			static_cast<int32>(TraceCharacterRoster::LastId));
		return;
	}

	Highlighted = CharacterId - TraceCharacterRoster::FirstId;
}
#endif

// =============================================================================================
// Draw
// =============================================================================================

void FTraceCharacterSelect::Draw(AHUD* HUD, ATracePlayerState* LocalState)
{
	if (LocalState == nullptr)
	{
		return;
	}

	const FLinearColor TeamTint = TraceTeamColor(LocalState->Team);

	// ---- Scrim ----------------------------------------------------------------------------------
	//
	// Not opaque. The arena stays faintly visible behind the cards, which is the difference between
	// "the match is loading" and "the match is running and waiting for you" — and the second is the
	// truth: the warm-up clock is ticking underneath this.
	HUD->DrawRect(TraceSelectStyle::Scrim, 0.f, 0.f, ViewW, ViewH);

	// ---- Title ----------------------------------------------------------------------------------
	const float Margin = 44.f * UIScale;
	float HeaderY = 40.f * UIScale;

	DrawTextCentered(HUD, TEXT("SELECT YOUR CHARACTER"), TraceSelectStyle::Ink, ViewW * 0.5f, HeaderY, FontLarge, UIScale * 1.15f);
	HeaderY += MeasureHeight(HUD, TEXT("X"), FontLarge, UIScale * 1.15f) + (6.f * UIScale);

	{
		// The team, in the team's colour. It is the single most load-bearing fact on this screen —
		// per-team uniqueness means the greyed-out cards only make sense once you know which team you
		// are on, and a player who just joined does not.
		const FString TeamLine = FString::Printf(TEXT("%s TEAM"), *TraceTeamName(LocalState->Team).ToString().ToUpper());
		DrawTextCentered(HUD, TeamLine, TeamTint, ViewW * 0.5f, HeaderY, FontMedium, UIScale);
		HeaderY += MeasureHeight(HUD, TeamLine, FontMedium, UIScale) + (4.f * UIScale);
	}

	{
		const FString RuleLine(TEXT("NOBODY ON YOUR TEAM MAY TAKE THE SAME CHARACTER. THE ENEMY MAY MIRROR YOUR PICK."));
		DrawTextCentered(HUD, RuleLine, TraceSelectStyle::InkDim, ViewW * 0.5f, HeaderY, FontSmall, UIScale);
		HeaderY += MeasureHeight(HUD, RuleLine, FontSmall, UIScale) + (10.f * UIScale);
	}

	// ---- The auto-pick countdown ----------------------------------------------------------------
	//
	// Spec v14 §3 [ASSUMPTION] gives this screen a timeout so one idle player cannot stall the match.
	// A timeout the player cannot see is indistinguishable from the game choosing for them at random,
	// so it is drawn as prominently as the title.
	const float SelectRemaining = LocalState->GetCharacterSelectTimeRemaining();
	if (LocalState->CharacterSelectDeadlineServerTime > 0.f)
	{
		const bool bUrgent = (SelectRemaining <= 5.f);
		const FLinearColor CountColor = bUrgent
			? TraceSelectStyle::WithAlpha(TraceSelectStyle::Danger, 0.65f + 0.35f * FMath::Sin(Now * 12.f))
			: TraceSelectStyle::Cyan;

		const FString CountLine = FString::Printf(TEXT("AUTO-PICK IN %d"), FMath::CeilToInt(SelectRemaining));
		DrawTextCentered(HUD, CountLine, CountColor, ViewW * 0.5f, HeaderY, FontMedium, UIScale);
		HeaderY += MeasureHeight(HUD, CountLine, FontMedium, UIScale) + (12.f * UIScale);
	}

	// ---- Cards ----------------------------------------------------------------------------------
	const float FooterH = 74.f * UIScale;
	const float CardGap = 12.f * UIScale;
	const float RowGap  = 12.f * UIScale;

	// HEIGHT IS CAPPED PER ROW, NOT STRETCHED, and the first capture is why. Filling the space between
	// the header and the footer gave 555-pixel-tall cards holding 170 pixels of text, i.e. three
	// quarters of each card was empty box — which reads as text that failed to load rather than as a
	// layout choice. 420 (at the 1080p reference) comfortably clears the longest card, Oyster's, with
	// room for a narrower viewport to wrap another line or two into.
	//
	// PER ROW is the v18 §2 change. It used to cap the whole block, which with two rows would have
	// squeezed both of them into the space one used to have.
	const float AvailableH = ViewH - HeaderY - FooterH - Margin;
	const float CardH = FMath::Clamp(
		(AvailableH - RowGap * (TraceSelectGrid::Rows - 1)) / static_cast<float>(TraceSelectGrid::Rows),
		160.f * UIScale, 420.f * UIScale);

	/** The whole grid's height, which is what the block is centred by. */
	const float CardsH = CardH * TraceSelectGrid::Rows + RowGap * (TraceSelectGrid::Rows - 1);

	// Capping the height leaves slack, so the block is floated rather than pinned under the header —
	// otherwise a 720p screen gets a tidy row of cards glued to the top and 300 px of nothing beneath
	// it. 40% of the slack rather than 50%: the eye reads a menu as centred when it sits slightly
	// above the true middle, and the header above it already carries weight.
	const float CardsY = HeaderY + FMath::Max(0.f, (AvailableH - CardsH) * 0.40f);

	const float TotalW = ViewW - (2.f * Margin);
	const float CardW = (TotalW - (CardGap * (TraceSelectGrid::Columns - 1))) / TraceSelectGrid::Columns;

	// One line, once per opening. A screenshot cannot tell "the cap is not compiled in" from "the cap
	// is compiled in and computes a big number", and this screen has already cost one round trip to
	// that exact ambiguity — the first capture after the cap was added looked identical to the one
	// before it and there was no way to tell which of the two had happened. The grid shape is on the
	// same line for the same reason: "8 cards" and "2 rows of 4" look identical in a log that only
	// prints the count.
	if (!bLoggedLayoutOnce)
	{
		bLoggedLayoutOnce = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharSelect] Layout: view %.0fx%.0f scale %.3f | %d cards as %d row(s) x %d col(s) | "
			     "grid y=%.0f h=%.0f, card %.0fx%.0f (row-height cap %.0f)"),
			ViewW, ViewH, UIScale, TraceCharacterRoster::Count, TraceSelectGrid::Rows,
			TraceSelectGrid::Columns, CardsY, CardsH, CardW, CardH, 420.f * UIScale);
	}

	for (int32 Index = 0; Index < TraceCharacterRoster::Count; ++Index)
	{
		const int32 Row = Index / TraceSelectGrid::Columns;
		const int32 Column = Index % TraceSelectGrid::Columns;

		// A SHORT LAST ROW IS CENTRED, not left-aligned. With eight characters both rows are full and
		// this is a no-op; with nine the second row holds four and hanging them off the left edge under
		// five would read as a card having failed to draw rather than as the roster being odd.
		const int32 CardsInRow = FMath::Min(TraceSelectGrid::Columns,
			TraceCharacterRoster::Count - Row * TraceSelectGrid::Columns);
		const float RowW = CardsInRow * CardW + (CardsInRow - 1) * CardGap;
		const float RowX = Margin + FMath::Max(0.f, (TotalW - RowW) * 0.5f);

		DrawCard(HUD, LocalState, Index,
			RowX + Column * (CardW + CardGap), CardsY + Row * (CardH + RowGap), CardW, CardH);
	}

	// ---- Footer: controls, and the last server verdict -------------------------------------------
	float FooterY = CardsY + CardsH + (14.f * UIScale);

	{
		// Built from the roster rather than written out, so adding a character cannot leave the screen
		// telling the player about one fewer key than it accepts. The upper bound is the number ROW,
		// which holds TEN keys and not nine — 1..9 then 0 — so at the spec v19 §3 roster of ten this
		// prints "1-9 AND 0". See TraceCharacterSelectFile::NumberKeyForIndex for the eleventh.
		const FString Controls = (TraceCharacterRoster::Count >= 10)
			? FString(TEXT("1-9 AND 0, OR ARROWS TO CHOOSE      ENTER TO LOCK IN      OR CLICK A CARD"))
			: FString::Printf(
				TEXT("1-%d OR ARROWS TO CHOOSE      ENTER TO LOCK IN      OR CLICK A CARD"),
				TraceCharacterRoster::Count);
		DrawTextCentered(HUD, Controls, TraceSelectStyle::InkDim, ViewW * 0.5f, FooterY, FontSmall, UIScale);
		FooterY += MeasureHeight(HUD, Controls, FontSmall, UIScale) + (8.f * UIScale);
	}

	// THE VERDICT LINE. Spec v14 §3's "the loser is TOLD and re-picks" is this. It reads the answer
	// the server sent back to ClientCharacterPickResult and prints it in plain words — a refusal that
	// only appeared in a log is a refusal the player never received.
	if ((Now - LocalState->LastPickResultLocalTime) < TraceSelectStyle::MessageDuration)
	{
		FString Message;
		FLinearColor MessageColor = TraceSelectStyle::Danger;

		const FString PickName = TraceCharacterRoster::NameFor(LocalState->LastPickResultCharacter);

		switch (LocalState->LastPickResult)
		{
		case ETraceCharacterPickResult::Granted:
			Message = FString::Printf(TEXT("%s LOCKED IN"), *PickName);
			MessageColor = TraceSelectStyle::Good;
			break;
		case ETraceCharacterPickResult::TakenByTeammate:
			Message = FString::Printf(TEXT("%s WAS TAKEN BY A TEAM-MATE FIRST - PICK ANOTHER"), *PickName);
			break;
		case ETraceCharacterPickResult::AlreadyLocked:
			Message = TEXT("YOU HAVE ALREADY LOCKED IN");
			break;
		case ETraceCharacterPickResult::Disabled:
			Message = TEXT("CHARACTERS ARE OFF IN THIS MATCH");
			break;
		case ETraceCharacterPickResult::NotSelecting:
			Message = TEXT("NOT PICKING RIGHT NOW");
			break;
		default:
			Message = TEXT("THAT IS NOT ONE OF THE CHARACTERS");
			break;
		}

		// Pulsed, because it may replace a message that was already there — a static line that merely
		// changed its words would be missed by a player who is looking at the cards.
		const FLinearColor Pulsed = TraceSelectStyle::WithAlpha(MessageColor, 0.7f + 0.3f * FMath::Sin(Now * 10.f));
		DrawTextCentered(HUD, Message, Pulsed, ViewW * 0.5f, FooterY, FontMedium, UIScale);
	}
	else if (PendingRequest != TraceCharacterRoster::NoneId)
	{
		DrawTextCentered(HUD, FString::Printf(TEXT("ASKING THE SERVER FOR %s..."),
			*TraceCharacterRoster::NameFor(PendingRequest)),
			TraceSelectStyle::InkDim, ViewW * 0.5f, FooterY, FontSmall, UIScale);
	}

	DrawCursor(HUD);
}

void FTraceCharacterSelect::DrawCard(AHUD* HUD, ATracePlayerState* LocalState, int32 CardIndex, float X, float Y, float W, float H)
{
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Roster = TraceCharacterRoster::All();
	if (!Roster.IsValidIndex(CardIndex))
	{
		return;
	}

	const TraceCharacterRoster::FTraceCharacterEntry& Entry = Roster[CardIndex];

	CardRects[CardIndex] = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));

	const ATracePlayerState* Holder = FindTeammateHolding(LocalState, Entry.Id);
	const bool bTaken = (Holder != nullptr);
	const bool bSelected = (CardIndex == Highlighted);

	// Dead cards are drawn, never removed. A card that vanished would move its neighbours under the
	// player's pointer and would hide the ONE thing they need to understand — that a team-mate has it.
	const float Dim = bTaken ? 0.32f : 1.f;

	HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Panel, bTaken ? 0.80f : 0.96f), X, Y, W, H);

	// The accent stripe down the left edge is the card's identity at a glance, and it is per-character
	// rather than per-team on purpose: on this one screen both teams look identical, because an enemy
	// mirroring your pick is legal, so a team colour here would carry no information at all.
	const float StripeW = FMath::Max(2.f, 4.f * UIScale);
	HUD->DrawRect(TraceSelectStyle::WithAlpha(Entry.Accent, Dim), X, Y, StripeW, H);

	DrawFrame(HUD, X, Y, W, H,
		bSelected ? TraceSelectStyle::WithAlpha(Entry.Accent, 0.95f)
		          : TraceSelectStyle::WithAlpha(TraceSelectStyle::Cyan, bTaken ? 0.12f : 0.30f));

	if (bSelected)
	{
		// A second, breathing frame just inside the first. The highlight has to survive being one of
		// eight similar rectangles on a dark screen; a single border does not manage it.
		const float Inset = 3.f * UIScale;
		DrawFrame(HUD, X + Inset, Y + Inset, W - 2.f * Inset, H - 2.f * Inset,
			TraceSelectStyle::WithAlpha(Entry.Accent, 0.35f + 0.25f * FMath::Sin(Now * 6.f)));
	}

	const float PadX = 12.f * UIScale;
	const float TextW = W - (2.f * PadX) - StripeW;
	const float TextX = X + StripeW + PadX;
	float TextY = Y + (12.f * UIScale);

	// ---- Number + name --------------------------------------------------------------------------
	{
		// THE KEY THAT ACTUALLY PICKS THIS CARD, not the card's ordinal — the two stop agreeing at the
		// tenth character, whose key is 0 (see NumberKeyForIndex). Printing "10" beside a card that
		// only answers to 0 would be the screen lying about its own controls. Past the tenth there is
		// no key at all and the hint is blank rather than wrong.
		const FString KeyHint = (CardIndex < 9)
			? FString::Printf(TEXT("%d"), CardIndex + 1)
			: (CardIndex == 9 ? FString(TEXT("0")) : FString());
		HUD->DrawText(KeyHint, TraceSelectStyle::WithAlpha(TraceSelectStyle::InkDim, Dim), TextX, TextY, FontSmall, UIScale);

		const FString NameText = Entry.Name;
		HUD->DrawText(NameText, TraceSelectStyle::WithAlpha(bTaken ? TraceSelectStyle::InkDim : Entry.Accent, 1.f),
			TextX + (20.f * UIScale), TextY - (2.f * UIScale), FontMedium, UIScale * 1.1f);

		TextY += MeasureHeight(HUD, NameText, FontMedium, UIScale * 1.1f) + (8.f * UIScale);
	}

	// ---- Taken banner ----------------------------------------------------------------------------
	if (bTaken)
	{
		const FString TakenLine = FString::Printf(TEXT("TAKEN BY %s"), *TraceCharacterSelectFile::SafePlayerName(Holder).ToUpper());
		HUD->DrawRect(TraceSelectStyle::WithAlpha(TraceSelectStyle::Danger, 0.18f),
			TextX, TextY - (2.f * UIScale), TextW, MeasureHeight(HUD, TakenLine, FontSmall, UIScale) + (4.f * UIScale));
		TextY = DrawWrapped(HUD, TakenLine, TraceSelectStyle::Danger, TextX, TextY, TextW, FontSmall, UIScale, 2.f * UIScale);
		TextY += 6.f * UIScale;
	}

	// ---- The three abilities ----------------------------------------------------------------------
	//
	// Spec v14 §3 asks for exactly this: "show each one's movement, passive and activated ability so a
	// player can choose meaningfully". The order is the doc's, and the ACTIVATED block carries its
	// key and its cooldown because those are the two facts that decide whether a player will actually
	// use it.
	auto DrawBlock = [&](const TCHAR* Caption, const FString& Body, const FLinearColor& CaptionColor)
	{
		HUD->DrawText(Caption, TraceSelectStyle::WithAlpha(CaptionColor, Dim), TextX, TextY, FontSmall, UIScale * 0.95f);
		TextY += MeasureHeight(HUD, FString(Caption), FontSmall, UIScale * 0.95f) + (2.f * UIScale);
		TextY = DrawWrapped(HUD, Body, TraceSelectStyle::WithAlpha(TraceSelectStyle::Ink, Dim * 0.92f),
			TextX, TextY, TextW, FontSmall, UIScale, 2.f * UIScale);
		TextY += 9.f * UIScale;
	};

	DrawBlock(TEXT("MOVEMENT"), Entry.Movement, TraceSelectStyle::Cyan);
	DrawBlock(TEXT("PASSIVE"), Entry.Passive, TraceSelectStyle::Cyan);

	DrawBlock(*FString::Printf(TEXT("ACTIVATED [E] - %s - %ds CD"),
		Entry.ActivatedName, FMath::RoundToInt(Entry.ActivatedCooldown)), Entry.Activated, Entry.Accent);
}

float FTraceCharacterSelect::DrawWrapped(AHUD* HUD, const FString& Text, const FLinearColor& Color,
	float X, float Y, float MaxWidth, UFont* Font, float Scale, float LineGap)
{
	// Greedy word wrap. The engine's bitmap fonts have no shaping and no kerning, so measuring a
	// candidate line and backing off one word is exact rather than approximate — there is no case
	// where adding a word makes the line narrower.
	TArray<FString> Words;
	Text.ParseIntoArray(Words, TEXT(" "), /*InCullEmpty=*/true);

	const float LineH = MeasureHeight(HUD, TEXT("X"), Font, Scale);
	float CursorY = Y;
	FString Line;

	for (const FString& Word : Words)
	{
		const FString Candidate = Line.IsEmpty() ? Word : (Line + TEXT(" ") + Word);
		if (!Line.IsEmpty() && MeasureWidth(HUD, Candidate, Font, Scale) > MaxWidth)
		{
			HUD->DrawText(Line, Color, X, CursorY, Font, Scale);
			CursorY += LineH + LineGap;
			Line = Word;
		}
		else
		{
			Line = Candidate;
		}
	}

	if (!Line.IsEmpty())
	{
		HUD->DrawText(Line, Color, X, CursorY, Font, Scale);
		CursorY += LineH + LineGap;
	}

	return CursorY;
}

void FTraceCharacterSelect::DrawFrame(AHUD* HUD, float X, float Y, float W, float H, const FLinearColor& Color)
{
	const float Thin = FMath::Max(1.f, 1.f * UIScale);

	HUD->DrawRect(Color, X, Y, W, Thin);
	HUD->DrawRect(Color, X, Y + H - Thin, W, Thin);
	HUD->DrawRect(Color, X, Y, Thin, H);
	HUD->DrawRect(Color, X + W - Thin, Y, Thin, H);
}

void FTraceCharacterSelect::DrawCursor(AHUD* HUD)
{
	// The OS cursor does not appear in captured frames and is hidden outright during a match, so the
	// overlay draws its own — the same shape and reasoning as FTraceOptionsMenu::DrawCursor.
	if (!bHasCursor)
	{
		return;
	}

	const float Size = 9.f * UIScale;
	const float Thick = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceSelectStyle::WithAlpha(TraceSelectStyle::Cyan, 0.95f);

	HUD->DrawLine(CursorPos.X - Size, CursorPos.Y, CursorPos.X - Size * 0.35f, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X + Size * 0.35f, CursorPos.Y, CursorPos.X + Size, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y - Size, CursorPos.X, CursorPos.Y - Size * 0.35f, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y + Size * 0.35f, CursorPos.X, CursorPos.Y + Size, Color, Thick);
}

float FTraceCharacterSelect::MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale) const
{
	float OutWidth = 0.f;
	float OutHeight = 0.f;
	HUD->GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
	return OutWidth;
}

float FTraceCharacterSelect::MeasureHeight(AHUD* HUD, const FString& Text, UFont* Font, float Scale) const
{
	float OutWidth = 0.f;
	float OutHeight = 0.f;
	HUD->GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
	return OutHeight;
}

void FTraceCharacterSelect::DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color,
	float CenterX, float Y, UFont* Font, float Scale)
{
	HUD->DrawText(Text, Color, CenterX - MeasureWidth(HUD, Text, Font, Scale) * 0.5f, Y, Font, Scale);
}

#if !UE_BUILD_SHIPPING
// Named after the file, like the block at the top of it and for the same jumbo-build reason —
// "TraceCharacterSelectCommand" is a name a second console-command file could plausibly reuse, and
// under the unity build both would land in the same anonymous namespace and fail to link on MSVC.
namespace TraceCharacterSelectCommands
{
	/**
	 * Trace.Characters.Select <1..N>   (N = TraceCharacterRoster::LastId, 8 since spec v18 §2)
	 *
	 * Drives a pick from the console so a headless run can prove the SCREEN's request path, not just
	 * the game mode's. It sets a plain int that the open screen consumes on its next Tick; see the
	 * note on GTraceCharacterSelectDebugPick in the header for why it is not a pointer.
	 */
	void TraceCharacterSelectCommand(const TArray<FString>& Args)
	{
		const int32 Requested = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 1;
		GTraceCharacterSelectDebugPick = Requested;

		UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Console pick queued: %s."),
			*TraceCharacterRoster::NameFor(static_cast<uint8>(Requested)));
	}

	/**
	 * Trace.Characters.ClickTest <1..N>   (N = TraceCharacterRoster::Count, 8 since spec v18 §2)
	 *
	 * Spec v15 §4 for this screen. Parks a real cursor on the card and clicks it through the real
	 * input pipeline, then reports how many complete clicks it took. One is the requirement.
	 */
	void TraceCharacterSelectClickTestCommand(const TArray<FString>& Args)
	{
		const int32 Requested = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 1;
		GTraceCharacterSelectClickTest = FMath::Clamp(Requested, 1, TraceCharacterRoster::Count);

		UE_LOG(LogTraceGame, Display, TEXT("[CharSelect.ClickTest] Queued a click on card %d."),
			GTraceCharacterSelectClickTest);
	}

	FAutoConsoleCommand CmdTraceCharacterSelectClickTest(
		TEXT("Trace.Characters.ClickTest"),
		TEXT("Dev only. Spec v15 s4. Parks the cursor on card 1..N (N = the roster size, 10 since spec "
		     "v19 s3 added Mortimer and Lily) and clicks it through the real input pipeline, then "
		     "reports how many complete clicks the card needed. Card 10 is the one the 0 key selects. "
		     "No effect unless the select screen is open on this machine."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceCharacterSelectClickTestCommand));

	FAutoConsoleCommand CmdTraceCharacterSelect(
		TEXT("Trace.Characters.Select"),
		TEXT("Dev only. Spec v14 3. Picks character 1..N (N = the roster size, 8 since spec v18 s2) "
		     "through the select screen's own request path (highlight, then confirm), so a headless run "
		     "exercises the screen rather than bypassing it. No effect unless the select screen is open "
		     "on this machine."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceCharacterSelectCommand));
}
#endif
