// Trace — title-screen input implementation. See TraceMenuPlayerController.h.

#include "UI/TraceMenuPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"          // EKeys
#include "Trace.h"                   // LogTraceGame
#include "UI/TraceMenuHUD.h"

ATraceMenuPlayerController::ATraceMenuPlayerController()
{
	// A title screen is a pointer-driven surface. Everything the shooter controller does — capture
	// the mouse, hide the cursor, swallow the first click — is wrong here.
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
}

void ATraceMenuPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (!IsLocalController())
	{
		return;
	}

	// GameAndUI rather than UIOnly: UIOnly routes key presses at a focused Slate widget, and this
	// menu has no widgets to focus — the presses would be swallowed before reaching InputComponent.
	// GameAndUI keeps the cursor free AND keeps the key bindings below alive.
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputMode.SetHideCursorDuringCapture(false);
	SetInputMode(InputMode);

	bShowMouseCursor = true;

	// THE SOURCE OF THE STRAY PLAY ACTIVATION.
	//
	// The shooter controller leaves the viewport in CapturePermanently_IncludingInitialMouseDown.
	// Coming into the title screen the viewport transitions out of that mode, and the transition
	// delivers the "initial mouse down" the mode is named after: a complete, well-formed down/up
	// pair at wherever the pointer is parked, one to seven seconds after the map loads. Logged
	// across ten launches it fired on five of them, always within ~150px of screen centre — which is
	// exactly where the PLAY row sits.
	//
	// That is why it looked like a focus click and why a grace period never reliably caught it: it
	// is not tied to window activation at all, it is tied to the capture-mode change, and it can
	// arrive well after any sane deadline. NoCapture means there is no capture to acquire and no
	// initial mouse-down to replay. The menu does not need capture — it needs a visible cursor and
	// key bindings, both of which are unaffected.
	if (UWorld* World = GetWorld())
	{
		if (UGameViewportClient* GameViewport = World->GetGameViewport())
		{
			GameViewport->SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
			GameViewport->SetMouseLockMode(EMouseLockMode::DoNotLock);
		}
	}
}

void ATraceMenuPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (InputComponent == nullptr)
	{
		return;
	}

	// bConsumeInput = false throughout: nothing else on this map wants input, and consuming would
	// only make a future addition (a background scene, a debug key) fail silently.
	auto Bind = [this](const FKey& Key, void (ATraceMenuPlayerController::*Handler)())
	{
		InputComponent->BindKey(Key, IE_Pressed, this, Handler).bConsumeInput = false;
	};

	auto BindReleased = [this](const FKey& Key, void (ATraceMenuPlayerController::*Handler)())
	{
		InputComponent->BindKey(Key, IE_Released, this, Handler).bConsumeInput = false;
	};

	Bind(EKeys::Up,        &ATraceMenuPlayerController::OnUp);
	Bind(EKeys::W,         &ATraceMenuPlayerController::OnUp);
	Bind(EKeys::Down,      &ATraceMenuPlayerController::OnDown);
	Bind(EKeys::S,         &ATraceMenuPlayerController::OnDown);
	Bind(EKeys::Left,      &ATraceMenuPlayerController::OnLeft);
	Bind(EKeys::A,         &ATraceMenuPlayerController::OnLeft);
	Bind(EKeys::Right,     &ATraceMenuPlayerController::OnRight);
	Bind(EKeys::D,         &ATraceMenuPlayerController::OnRight);
	Bind(EKeys::Enter,     &ATraceMenuPlayerController::OnAccept);
	Bind(EKeys::SpaceBar,  &ATraceMenuPlayerController::OnAccept);
	Bind(EKeys::Escape,    &ATraceMenuPlayerController::OnCancel);

	// Down arms, up fires. See OnMouseDown/OnMouseUp in the header for why the down edge alone was
	// the stray-activation bug rather than a symptom of it.
	Bind(EKeys::LeftMouseButton,         &ATraceMenuPlayerController::OnMouseDown);
	BindReleased(EKeys::LeftMouseButton, &ATraceMenuPlayerController::OnMouseUp);
}

ATraceMenuHUD* ATraceMenuPlayerController::GetMenuHUD() const
{
	return Cast<ATraceMenuHUD>(GetHUD());
}

void ATraceMenuPlayerController::OnUp()
{
	if (ATraceMenuHUD* MenuHUD = GetMenuHUD()) { MenuHUD->MoveSelection(-1); }
}

void ATraceMenuPlayerController::OnDown()
{
	if (ATraceMenuHUD* MenuHUD = GetMenuHUD()) { MenuHUD->MoveSelection(1); }
}

void ATraceMenuPlayerController::OnLeft()
{
	if (ATraceMenuHUD* MenuHUD = GetMenuHUD()) { MenuHUD->AdjustSelection(-1); }
}

void ATraceMenuPlayerController::OnRight()
{
	if (ATraceMenuHUD* MenuHUD = GetMenuHUD()) { MenuHUD->AdjustSelection(1); }
}

void ATraceMenuPlayerController::OnAccept()
{
	// Logged because the title screen was observed activating PLAY on its own, seconds after the map
	// loaded, with no AutoPlay timer armed. Naming the exact input that did it is the difference
	// between fixing the cause and guessing at it.
	UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] Accept (Enter/Space)."));
	if (ATraceMenuHUD* MenuHUD = GetMenuHUD()) { MenuHUD->ActivateSelection(); }
}

void ATraceMenuPlayerController::OnCancel()
{
	UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] Cancel (Escape)."));
	if (ATraceMenuHUD* MenuHUD = GetMenuHUD()) { MenuHUD->CancelPressed(); }
}

void ATraceMenuPlayerController::OnMouseDown()
{
	float MouseX = 0.f;
	float MouseY = 0.f;
	const bool bHavePosition = GetMousePosition(MouseX, MouseY);
	UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] LMB down at (%.0f, %.0f) valid=%d."),
		MouseX, MouseY, bHavePosition ? 1 : 0);

	if (ATraceMenuHUD* MenuHUD = GetMenuHUD()) { MenuHUD->MousePressed(); }
}

void ATraceMenuPlayerController::OnMouseUp()
{
	float MouseX = 0.f;
	float MouseY = 0.f;
	const bool bHavePosition = GetMousePosition(MouseX, MouseY);
	UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] LMB up at (%.0f, %.0f) valid=%d."),
		MouseX, MouseY, bHavePosition ? 1 : 0);

	if (ATraceMenuHUD* MenuHUD = GetMenuHUD()) { MenuHUD->MouseReleased(); }
}
