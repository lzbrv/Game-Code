// Trace — title-screen input.
//
// Deliberately NOT ATracePlayerController: that class builds the whole Enhanced Input mapping
// context for a shooter (move, look, fire, dash, pass, scoreboard), takes permanent mouse capture
// and hides the cursor. A menu wants the opposite of all of that. Sharing the class would mean
// gating half of it behind a "am I in a menu?" flag, which is exactly how input bugs get made.
//
// The bindings here are classic UInputComponent::BindKey rather than Enhanced Input actions. A
// menu has no axes, no modifiers and no remapping story; three static keys per direction is the
// whole requirement, and UEnhancedInputComponent inherits BindKey unchanged, so this composes with
// the project's Enhanced-Input-everywhere setting rather than fighting it.
//
// All state lives on ATraceMenuHUD. This class only routes.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "UObject/ObjectMacros.h"

#include "TraceMenuPlayerController.generated.h"

class ATraceMenuHUD;

UCLASS()
class TRACE_API ATraceMenuPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ATraceMenuPlayerController();

	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

protected:
	/** Null until the HUD exists (it is spawned by the game mode after this controller). */
	ATraceMenuHUD* GetMenuHUD() const;

	void OnUp();
	void OnDown();
	void OnLeft();
	void OnRight();
	void OnAccept();
	void OnCancel();

	/**
	 * Mouse down and mouse up are bound SEPARATELY, and only the release can activate anything.
	 *
	 * The title screen was spuriously taking PLAY a second or two after appearing. The cause was
	 * here: LeftMouseButton was bound on IE_Pressed alone, so any mouse-down over the default
	 * selection activated it — including the click macOS delivers to an app when that click is what
	 * brings its window to the front. Press-to-arm / release-to-fire is how every button in every
	 * toolkit behaves, it makes the stray down harmless on its own, and it gives the player the
	 * usual escape hatch of dragging off a row before letting go.
	 */
	void OnMouseDown();
	void OnMouseUp();
};
