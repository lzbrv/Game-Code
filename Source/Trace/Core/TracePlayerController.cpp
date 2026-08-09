// Trace — player controller implementation.

#include "Core/TracePlayerController.h"

#include "Abilities/TraceAbilityComponent.h"                  // spec v14 §5 — the E / V binds
#include "Abilities/Characters/TraceAbilityInputRelay.h"      // ... and Mace's reactivation routing
#include "Containers/Ticker.h"             // FTSTicker — the v13 §2 hotkey probe
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerState.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"           // the concrete UPlayerInput Enhanced Input requires
#include "Engine/Engine.h"                 // GEngine->GameViewport, for the capture-mode diagnostic
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/NetDriver.h"           // UNetDriver::ClientConnections, for the solo-pause test
#include "Engine/World.h"
#include "GameFramework/Character.h"       // ACharacter::Jump / StopJumping on ATraceCharacter
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"   // PlayerArray, for the TraceNetInfo roster dump
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"     // APlayerState::IsABot / GetPlayerName
#include "GameFramework/PlayerInput.h"    // APlayerController::PlayerInput, for the setup diagnostic
#include "HAL/IConsoleManager.h"           // Trace.LogInput
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"                // EKeys
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"                 // ETriggerEvent
#include "Gameplay/TraceMelee.h"           // TraceMelee::RequestSwapWeapon (spec v10 §1)
#include "Movement/TraceCharacterMovementComponent.h"   // dash charges, for the HUD accessors
#include "Settings/TraceUserSettings.h"    // sensitivity, invert-Y and the key bindings
#include "Trace.h"                         // LogTraceGame
#include "TraceSettings.h"                 // gameplay tuning (dash cooldown, and so on)
#include "UI/TraceNetworking.h"            // TraceNet — failure handlers and the address helpers

/**
 * Per-event Display logging for the input path. Off by default — on at 60 Hz the Move handler alone
 * would write a line a frame — but every automated verification run turns it on, and it is the only
 * way to see from a log that a key actually reached a bound delegate.
 *
 *     Trace.LogInput 1        (console, -ExecCmds, or the harness in Source/Trace/Debug)
 */
static TAutoConsoleVariable<int32> CVarTraceLogInput(
	TEXT("Trace.LogInput"),
	0,
	TEXT("Log every Enhanced Input event that reaches ATracePlayerController, at Display level.\n")
	TEXT("0: off (default)  1: buttons only  2: buttons + per-frame Move/Look axes"),
	ECVF_Default);

namespace
{
	/** 1 = buttons, 2 = buttons and axes. */
	FORCEINLINE int32 InputLogLevel()
	{
		return CVarTraceLogInput.GetValueOnGameThread();
	}

	/**
	 * A 1D key (W/S/A/D, MouseX, MouseY) always delivers its value on the X component. Swizzling
	 * with YXZ moves it onto Y, which is how a single key becomes the "forward" axis of an
	 * Axis2D action.
	 */
	UInputModifierSwizzleAxis* MakeSwizzleXToY(UObject* Outer)
	{
		UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(Outer);
		Swizzle->Order = EInputAxisSwizzle::YXZ;
		return Swizzle;
	}

	/**
	 * UInputModifierNegate does expose per-axis bX/bY/bZ flags (all defaulting to true), but we never
	 * need them: every mapping this is attached to carries a single non-zero component, because each
	 * key is 1D and any swizzle has already run. If you ever need to invert one axis of a genuinely
	 * 2D key such as EKeys::Mouse2D, split it into two 1D mappings the way the Look binding below
	 * does — that stays readable, whereas the flags do not.
	 */
	UInputModifierNegate* MakeNegate(UObject* Outer)
	{
		return NewObject<UInputModifierNegate>(Outer);
	}

	/** Uniform XY multiplier. Used to give mouse-look a sensitivity that is actually configurable. */
	UInputModifierScalar* MakeScalar(UObject* Outer, float Scale)
	{
		UInputModifierScalar* Scalar = NewObject<UInputModifierScalar>(Outer);
		Scalar->Scalar = FVector(Scale, Scale, 1.f);
		return Scalar;
	}
}

ATracePlayerController::ATracePlayerController()
{
	bShowMouseCursor = false;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;

	// Pawns are destroyed and respawned constantly; let the engine keep the view target pointed at
	// whatever we currently possess. This is the engine default — stated explicitly because the
	// death/respawn flow depends on it.
	bAutoManageActiveCameraTarget = true;
}

void ATracePlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
		ApplyGameInputMode();

		// SetupInputComponent has normally already run, but possession/init order differs between
		// standalone, listen server and client, so make sure the context really is live.
		AddInputMappings();

		// The options screen is reachable from an in-game pause menu, so a sensitivity change has to
		// land on the very next mouse event. Weak-lambda: the delegate lives in a static and outlives
		// every controller that ever subscribes to it.
		SettingsChangedHandle = UTraceUserSettings::OnChanged().AddWeakLambda(this,
			[this]() { ApplyControlSettings(); });

		// A failure that arrives while a match is running must still be reported, and a client can
		// reach this map without ever having seen the title screen (Scripts/run-client.sh, or `open
		// <ip>` from the console). Idempotent.
		TraceNet::BindFailureHandlers();
	}

	// Every controller, not just the local one, and at Display: on a listen server this is the line
	// that proves a remote player's controller actually reached the server. During the Demo 5
	// playtest nobody could establish even that much, which is why the multiplayer question was
	// unanswerable rather than merely unanswered.
	LogNetworkRole(TEXT("PlayerController BeginPlay"));
}

void ATracePlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The subsystem outlives this controller (it belongs to the ULocalPlayer). Leaving our context
	// registered would keep a dead controller's actions in the mapping stack across travel.
	RemoveInputMappings();

	// AddWeakLambda already makes the callback safe after destruction, but the delegate itself is a
	// process-lifetime static: without this, every controller ever created leaves an entry behind and
	// the list grows for the life of the session.
	if (SettingsChangedHandle.IsValid())
	{
		UTraceUserSettings::OnChanged().Remove(SettingsChangedHandle);
		SettingsChangedHandle.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

void ATracePlayerController::ApplyGameInputMode()
{
	if (!IsLocalController())
	{
		return;
	}

	// Mouse-look shooter: the viewport swallows the cursor and nothing else wants input.
	//
	// SetConsumeCaptureMouseDown(false) is load-bearing. FInputModeGameOnly defaults it to true,
	// and FInputModeGameOnly::ApplyInputMode then downgrades the viewport from
	// CapturePermanently_IncludingInitialMouseDown to plain CapturePermanently — silently undoing
	// DefaultViewportMouseCaptureMode in Config/DefaultInput.ini one frame after BeginPlay
	// (measured: "Viewport MouseCaptureMode Changed, CapturePermanently_IncludingInitialMouseDown ->
	// CapturePermanently"). The consequence is that the click which RE-captures the viewport is
	// eaten and never reaches Enhanced Input. On macOS in a window, capture is lost on every
	// Cmd-Tab or click-away, so a large share of shots silently do nothing.
	FInputModeGameOnly InputMode;
	InputMode.SetConsumeCaptureMouseDown(false);
	SetInputMode(InputMode);

	bShowMouseCursor = false;

	// Swallow look input briefly. When the viewport takes the mouse it warps the OS cursor to the
	// centre of the window, and the delta between the cursor's old position and that warp arrives as
	// a single enormous mouse move. Measured on this build at spawn: one frame carrying +83.8 deg of
	// yaw and +17.9 deg of pitch at LookSensitivity 1.0 — reproducible to eight decimal places
	// across separate processes, and multiplied to +209.5 / +44.6 at the shipped sensitivity of 2.5.
	// The player spawns facing down the field and is instantly spun around and pointed at the sky.
	if (const UWorld* World = GetWorld())
	{
		IgnoreLookUntilTime = World->GetTimeSeconds() + LookSuppressAfterCapture;
	}
}

// -------------------------------------------------------------------------------------------
// Enhanced Input construction
// -------------------------------------------------------------------------------------------

void ATracePlayerController::BuildInputData()
{
	if (InputMapping == nullptr)
	{
		// Everything below is outered to `this` (or to the mapping context) AND referenced by a
		// UPROPERTY, so the whole object graph survives GC for as long as the controller does.
		InputMapping = NewObject<UInputMappingContext>(this, TEXT("IMC_Trace"));

		auto MakeAction = [this](const TCHAR* Name, EInputActionValueType ValueType) -> UInputAction*
		{
			UInputAction* Action = NewObject<UInputAction>(this, Name);
			// Must be set before anything binds to the action: a mismatched ValueType silently yields
			// zeroes in the handler instead of failing loudly.
			Action->ValueType = ValueType;
			return Action;
		};

		IA_Move       = MakeAction(TEXT("IA_Move"),       EInputActionValueType::Axis2D);
		IA_Look       = MakeAction(TEXT("IA_Look"),       EInputActionValueType::Axis2D);
		IA_Jump       = MakeAction(TEXT("IA_Jump"),       EInputActionValueType::Boolean);
		IA_Fire       = MakeAction(TEXT("IA_Fire"),       EInputActionValueType::Boolean);
		IA_Pass       = MakeAction(TEXT("IA_Pass"),       EInputActionValueType::Boolean);
		IA_Dash       = MakeAction(TEXT("IA_Dash"),       EInputActionValueType::Boolean);
		IA_Scoreboard = MakeAction(TEXT("IA_Scoreboard"), EInputActionValueType::Boolean);
		IA_Crouch     = MakeAction(TEXT("IA_Crouch"),     EInputActionValueType::Boolean);
		IA_Parry      = MakeAction(TEXT("IA_Parry"),      EInputActionValueType::Boolean);
		IA_SwapWeapon = MakeAction(TEXT("IA_SwapWeapon"), EInputActionValueType::Boolean);
		// Spec v13 §2, the two direct-select binds. Boolean like every other button here.
		IA_EquipKnife = MakeAction(TEXT("IA_EquipKnife"), EInputActionValueType::Boolean);
		IA_EquipGun   = MakeAction(TEXT("IA_EquipGun"),   EInputActionValueType::Boolean);
		// SPEC v14 §5 — the ability binds.
		IA_Ability          = MakeAction(TEXT("IA_Ability"),          EInputActionValueType::Boolean);
		IA_AbilitySecondary = MakeAction(TEXT("IA_AbilitySecondary"), EInputActionValueType::Boolean);

		// Cumulative accumulation is REQUIRED for opposing keys to cancel. UInputAction defaults to
		// TakeHighestAbsoluteValue, and UEnhancedPlayerInput::ProcessActionMappingEvent merges with
		// `if (Abs(Modified[C]) >= Abs(Merged[C])) Merged[C] = Modified[C];` — note the `>=`, so on a
		// tie the LAST mapping added to the context wins. With the mapping order below that made W+S
		// walk backwards and A+D strafe right, and counter-strafing impossible. The engine's own docs
		// name WASD as the motivating case for Cumulative.
		IA_Move->AccumulationBehavior = EInputActionAccumulationBehavior::Cumulative;
	}

	ApplyControlSettings();
}

void ATracePlayerController::ApplyControlSettings()
{
	if (InputMapping == nullptr)
	{
		// Called from the settings delegate before this controller ever built its input (a controller
		// on a client that has not finished initialising). BuildInputData will call us back.
		return;
	}

	const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();

	// Torn down and rebuilt wholesale rather than edited in place. Editing means finding the right
	// FEnhancedActionKeyMapping by (action, old key) and mutating it, which is fiddly, and the
	// modifier objects would have to be reused or leaked; a context with a dozen mappings costs
	// nothing to rebuild and the result cannot be half-applied.
	InputMapping->UnmapAll();

	auto KeyFor = [&UserSettings](ETraceInputAction Action) { return UserSettings.GetKey(Action); };

	/** Maps @p Key to @p Action, skipping the mapping entirely when the player has unbound it. */
	auto MapButton = [this](UInputAction* Action, const FKey& Key)
	{
		// No explicit triggers: an action with no trigger uses the implicit "down" trigger, which
		// gives us Started on press, Triggered while held and Completed on release. That is exactly
		// the shape the handlers below expect.
		if (Action != nullptr && Key.IsValid())
		{
			InputMapping->MapKey(Action, Key);
		}
	};

	// --- Move: four 1D keys -> (X = strafe, +right), (Y = forward, +forward) --------------------
	//
	// MapKey returns a reference *into* the context's mapping array, so it is invalidated by the
	// next MapKey call. Each binding therefore gets its own scope and uses the reference
	// immediately — never cache one.
	if (const FKey Key = KeyFor(ETraceInputAction::MoveForward); Key.IsValid())
	{
		// Forward. 1D X -> Y.
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, Key);
		Mapping.Modifiers.Add(MakeSwizzleXToY(InputMapping));
	}
	if (const FKey Key = KeyFor(ETraceInputAction::MoveBack); Key.IsValid())
	{
		// Backward. 1D X -> Y, then inverted.
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, Key);
		Mapping.Modifiers.Add(MakeSwizzleXToY(InputMapping));
		Mapping.Modifiers.Add(MakeNegate(InputMapping));
	}
	if (const FKey Key = KeyFor(ETraceInputAction::MoveLeft); Key.IsValid())
	{
		// Strafe left — X, inverted.
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, Key);
		Mapping.Modifiers.Add(MakeNegate(InputMapping));
	}
	if (const FKey Key = KeyFor(ETraceInputAction::MoveRight); Key.IsValid())
	{
		// Strafe right — raw X, no modifiers at all.
		InputMapping->MapKey(IA_Move, Key);
	}

	// --- Look: mouse -> (X = yaw delta, Y = pitch delta) ---------------------------------------
	//
	// THE INVERT-Y BUG, AND WHY THE DEFAULT HAD TO CHANGE RATHER THAN JUST GAIN A TOGGLE.
	//
	// Raw EKeys::MouseY is positive when the mouse moves UP: FSceneViewport::OnMouseMove accumulates
	// `MouseDelta.Y -= CursorDelta.Y`, negating the screen-space (down-positive) delta. And with
	// Config/DefaultInput.ini setting bEnableLegacyInputScales=False, APlayerController::
	// AddPitchInput adds `Val * 1.0` straight onto RotationInput.Pitch, where positive pitch is UP.
	// So mouse-up arrives positive and should be consumed as-is.
	//
	// This context used to put a Negate modifier on MouseY. That is correct ONLY under the legacy
	// input scales, where InputPitchScale_DEPRECATED is -2.5 and supplies an inversion of its own for
	// the Negate to cancel. With legacy scales off the Negate was the only sign flip left in the
	// chain, so pushing the mouse forward looked DOWN — which is exactly what the player reported.
	//
	// The Negate is therefore gone, and inversion now lives in the SIGN of the Y scalar. That keeps
	// the modifier list a fixed shape, so a live rebuild only ever changes numbers, and it means the
	// invert toggle and the sensitivity slider share one code path instead of two.
	//
	// The Scalars are not optional: with legacy scales off, nothing else in the chain scales the
	// mouse at all, and raw delta would be consumed directly as degrees.
	{
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Look, EKeys::MouseX);
		Mapping.Modifiers.Add(MakeScalar(InputMapping, UserSettings.GetLookScaleX()));
	}
	{
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Look, EKeys::MouseY);
		Mapping.Modifiers.Add(MakeSwizzleXToY(InputMapping));
		Mapping.Modifiers.Add(MakeScalar(InputMapping, UserSettings.GetLookScaleY()));
	}

	// --- Buttons -------------------------------------------------------------------------------
	MapButton(IA_Jump,       KeyFor(ETraceInputAction::Jump));
	MapButton(IA_Crouch,     KeyFor(ETraceInputAction::Crouch));
	MapButton(IA_Fire,       KeyFor(ETraceInputAction::Fire));
	MapButton(IA_Pass,       KeyFor(ETraceInputAction::Pass));
	MapButton(IA_Dash,       KeyFor(ETraceInputAction::Dash));
	MapButton(IA_Parry,      KeyFor(ETraceInputAction::Parry));
	MapButton(IA_Scoreboard, KeyFor(ETraceInputAction::Scoreboard));
	MapButton(IA_SwapWeapon, KeyFor(ETraceInputAction::SwapWeapon));
	// Spec v13 §2. Mapped through the same KeyFor/MapButton path as everything else, so the player's
	// rebind of "1" is honoured on the next settings change without a restart — and so an action the
	// player has deliberately UNBOUND gets no mapping at all rather than a dead one.
	MapButton(IA_EquipKnife, KeyFor(ETraceInputAction::EquipKnife));
	MapButton(IA_EquipGun,   KeyFor(ETraceInputAction::EquipGun));
	// SPEC v14 §5. Through the same KeyFor/MapButton path as everything else, so a player who
	// rebinds E in the options screen is rebinding the ability and not just the label — and so
	// UTraceAbilityInputRelay's ConfigId lookup and this mapping can never disagree about the key.
	MapButton(IA_Ability,          KeyFor(ETraceInputAction::Ability));
	MapButton(IA_AbilitySecondary, KeyFor(ETraceInputAction::AbilitySecondary));

	// The context is already registered by the time a settings change arrives, and Enhanced Input
	// caches the resolved key->action table. Without this the new bindings sit in the context and
	// are never consulted, which looks exactly like "rebinding does nothing".
	if (const ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			Subsystem->RequestRebuildControlMappings();
		}
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[%s] Input mappings applied: %d key mappings, sensitivity %.2f (yaw) / %.2f (pitch, %s)."),
		*GetName(), InputMapping->GetMappings().Num(),
		UserSettings.GetLookScaleX(), FMath::Abs(UserSettings.GetLookScaleY()),
		UserSettings.bInvertMouseY ? TEXT("INVERTED") : TEXT("standard"));
}

void ATracePlayerController::AddInputMappings()
{
	// Remote controllers on the server have no local player and therefore no input system.
	if (!IsLocalController())
	{
		return;
	}

	BuildInputData();

	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (LocalPlayer == nullptr)
	{
		// Normal during early init; BeginPlay/AcknowledgePossession will call us again.
		return;
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (Subsystem == nullptr)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[%s] No UEnhancedInputLocalPlayerSubsystem — is the EnhancedInput plugin enabled in Trace.uproject?"),
			*GetName());
		return;
	}

	// The context is fully built before it is added and nothing mutates it afterwards, so we never
	// need UEnhancedInputLibrary::RequestRebuildControlMappingsUsingContext.
	if (!Subsystem->HasMappingContext(InputMapping))
	{
		Subsystem->AddMappingContext(InputMapping, InputMappingPriority);
	}

	// AddMappingContext is a silent no-op when APlayerController::PlayerInput is not a
	// UEnhancedPlayerInput — and a project that overrides DefaultPlayerInputClass in config hits
	// exactly that, with no error anywhere and no input in game. The subsystem records the context
	// on the player input synchronously, so reading it straight back is a real check rather than a
	// guess.
	//
	// The check itself runs EVERY time, because registration can start failing later than the first
	// call (seamless travel, a second local player). Only the *reporting* is latched, so a
	// persistent failure logs once instead of once per respawn — and a failure that appears after a
	// success is still reported.
	if (!Subsystem->HasMappingContext(InputMapping) && !bInputFailureReported)
	{
		bInputFailureReported = true;

		UE_LOG(LogTraceGame, Error,
			TEXT("[%s] Enhanced Input refused the mapping context — no gameplay input will reach the pawn. ")
			TEXT("Check [/Script/Engine.InputSettings] DefaultPlayerInputClass; PlayerInput is currently '%s'."),
			*GetName(),
			PlayerInput ? *PlayerInput->GetClass()->GetName() : TEXT("<none>"));
	}
}

void ATracePlayerController::RemoveInputMappings()
{
	if (InputMapping == nullptr || !IsLocalController())
	{
		return;
	}

	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			Subsystem->RemoveMappingContext(InputMapping);
		}
	}
}

void ATracePlayerController::SetupInputComponent()
{
	// Guarantee an EnhancedInputComponent whatever DefaultInputComponentClass says in config.
	// Creating it *before* Super means APlayerController::SetupInputComponent leaves it alone;
	// this mirrors what the engine itself does, minus the dependency on the ini being right.
	if (InputComponent == nullptr)
	{
		UEnhancedInputComponent* EnhancedInput =
			NewObject<UEnhancedInputComponent>(this, TEXT("TraceEnhancedInputComponent"));
		EnhancedInput->RegisterComponent();
		InputComponent = EnhancedInput;
	}

	Super::SetupInputComponent();

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	if (EIC == nullptr)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[%s] InputComponent is not a UEnhancedInputComponent — no gameplay input will be bound."),
			*GetName());
		return;
	}

	BuildInputData();

	// Axes fire every frame the value is non-zero.
	EIC->BindAction(IA_Move, ETriggerEvent::Triggered, this, &ATracePlayerController::OnMoveInput);
	EIC->BindAction(IA_Look, ETriggerEvent::Triggered, this, &ATracePlayerController::OnLookInput);

	// Buttons: Started on press, Completed on release.
	EIC->BindAction(IA_Jump, ETriggerEvent::Started,   this, &ATracePlayerController::OnJumpStarted);
	EIC->BindAction(IA_Jump, ETriggerEvent::Completed, this, &ATracePlayerController::OnJumpCompleted);

	EIC->BindAction(IA_Fire, ETriggerEvent::Started,   this, &ATracePlayerController::OnFireStarted);
	EIC->BindAction(IA_Fire, ETriggerEvent::Completed, this, &ATracePlayerController::OnFireCompleted);
	// Mouse1 is a held button now for the carrier (§4 pass), so an interrupted trigger has to reach
	// the release path too, or a focus loss mid-pass strands the shield down.
	EIC->BindAction(IA_Fire, ETriggerEvent::Canceled,  this, &ATracePlayerController::OnFireCompleted);

	// Pass is a HELD action now (spec §4: hover a teammate and hold for 0.5s), so it needs the
	// release edge — and Canceled too, because losing window focus mid-hold must not leave the
	// carrier's shield welded down.
	EIC->BindAction(IA_Pass, ETriggerEvent::Started,   this, &ATracePlayerController::OnPassStarted);
	EIC->BindAction(IA_Pass, ETriggerEvent::Completed, this, &ATracePlayerController::OnPassCompleted);
	EIC->BindAction(IA_Pass, ETriggerEvent::Canceled,  this, &ATracePlayerController::OnPassCompleted);

	EIC->BindAction(IA_Dash, ETriggerEvent::Started,   this, &ATracePlayerController::OnDashStarted);

	// Parry (spec v3 §3). Bound on all three edges for the symmetry argument in the header, even
	// though the release path is deliberately empty today.
	EIC->BindAction(IA_Parry, ETriggerEvent::Started,   this, &ATracePlayerController::OnParryStarted);
	EIC->BindAction(IA_Parry, ETriggerEvent::Completed, this, &ATracePlayerController::OnParryCompleted);
	EIC->BindAction(IA_Parry, ETriggerEvent::Canceled,  this, &ATracePlayerController::OnParryCompleted);

	// Weapon swap (spec v10 §1). PRESS EDGE ONLY. Every other button in this class binds Completed
	// and Canceled too, for the symmetry argument above — this one must not. The swap is a toggle,
	// so a release binding would fire a second swap on key-up and the weapon would come back the
	// instant you let go. There is no held state to release and nothing to strand on lost focus.
	EIC->BindAction(IA_SwapWeapon, ETriggerEvent::Started, this, &ATracePlayerController::OnSwapWeaponStarted);

	// Spec v13 §2, direct select. PRESS EDGE ONLY, same argument as the toggle above: there is no
	// held state to release, and a Completed binding would send a second equip request on key-up.
	// (For the toggle that would have swapped back; for a direct select it would be a redundant
	// request, which the idempotence guard in HandleDirectEquip would swallow — but a binding that
	// only works because something downstream ignores it is a binding waiting to be a bug.)
	EIC->BindAction(IA_EquipKnife, ETriggerEvent::Started, this, &ATracePlayerController::OnEquipKnifeStarted);
	EIC->BindAction(IA_EquipGun,   ETriggerEvent::Started, this, &ATracePlayerController::OnEquipGunStarted);

	// SPEC v14 §5. Ability is a PRESS. AbilitySecondary is a HOLD, so it gets the full
	// Started/Completed/Canceled shape — Canceled included, for the reason IA_Pass documents: a
	// release edge that never arrives leaves Mace suspended in mid-air with gravity switched off.
	EIC->BindAction(IA_Ability, ETriggerEvent::Started, this, &ATracePlayerController::OnAbilityStarted);
	EIC->BindAction(IA_AbilitySecondary, ETriggerEvent::Started,   this, &ATracePlayerController::OnAbilitySecondaryStarted);
	EIC->BindAction(IA_AbilitySecondary, ETriggerEvent::Completed, this, &ATracePlayerController::OnAbilitySecondaryCompleted);
	EIC->BindAction(IA_AbilitySecondary, ETriggerEvent::Canceled,  this, &ATracePlayerController::OnAbilitySecondaryCompleted);

	// Crouch is a HELD action — ground slide while down, stand on release — so it needs the release
	// edge as well, and Canceled for the same reason the scoreboard does: losing window focus with
	// the key down must not weld the player into a crouch.
	EIC->BindAction(IA_Crouch, ETriggerEvent::Started,   this, &ATracePlayerController::OnCrouchStarted);
	EIC->BindAction(IA_Crouch, ETriggerEvent::Completed, this, &ATracePlayerController::OnCrouchCompleted);
	EIC->BindAction(IA_Crouch, ETriggerEvent::Canceled,  this, &ATracePlayerController::OnCrouchCompleted);

	EIC->BindAction(IA_Scoreboard, ETriggerEvent::Started,   this, &ATracePlayerController::OnScoreboardStarted);
	EIC->BindAction(IA_Scoreboard, ETriggerEvent::Completed, this, &ATracePlayerController::OnScoreboardCompleted);
	// Canceled as well as Completed: this is the only *held* binding in the game, so it is the only
	// one that can be left stuck on if the trigger is interrupted rather than released — losing
	// window focus with Tab down, for instance. A scoreboard welded over the match is unrecoverable
	// without this; the extra binding on a press/release action is otherwise a no-op.
	EIC->BindAction(IA_Scoreboard, ETriggerEvent::Canceled,  this, &ATracePlayerController::OnScoreboardCompleted);

	AddInputMappings();
}

// -------------------------------------------------------------------------------------------
// Possession
// -------------------------------------------------------------------------------------------

void ATracePlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	// Server path. Only meaningful on a listen-server host, where this controller is also local.
	AddInputMappings();

	// Re-assert the capture mode on every possession, not just once at BeginPlay: anything that
	// pushes a different input mode (a menu, a travel, the engine's own focus handling) would
	// otherwise leave the viewport swallowing the first click of every burst.
	ApplyGameInputMode();
}

void ATracePlayerController::AcknowledgePossession(APawn* P)
{
	Super::AcknowledgePossession(P);

	// Client path: the local player definitely exists by now, even if SetupInputComponent ran
	// before it did.
	AddInputMappings();
	ApplyGameInputMode();

	// Deliberately does NOT reset bScoreboardOpen. Enhanced Input state lives on the local player,
	// not the pawn, so a player holding Tab through their own death and respawn is still holding it
	// afterwards — clearing the flag here would blank the scoreboard mid-hold and it would not come
	// back until they released and pressed again. The Completed/Canceled bindings own that flag.
}

// -------------------------------------------------------------------------------------------
// Networking
//
// Two small things, and both exist because the Demo 5 multiplayer report contained no evidence at
// all — not a failure message, not a role, not an address. Neither of these changes behaviour; they
// change what a log and a console can tell you.
// -------------------------------------------------------------------------------------------

void ATracePlayerController::LogNetworkRole(const TCHAR* Context) const
{
	const UWorld* ControllerWorld = GetWorld();
	const ENetMode NetMode = (ControllerWorld != nullptr) ? ControllerWorld->GetNetMode() : NM_Standalone;

	const TCHAR* NetModeName =
		(NetMode == NM_Standalone)      ? TEXT("Standalone")    :
		(NetMode == NM_ListenServer)    ? TEXT("ListenServer")  :
		(NetMode == NM_DedicatedServer) ? TEXT("DedicatedServer") : TEXT("Client");

	// "local" distinguishes the host's own controller from the proxies the server holds for remote
	// players — the single most useful bit when reading a listen server's log, because a match with
	// two humans in it has three controllers on the server and only one of them is local.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Net] %s: netMode=%s local=%d authority=%d name='%s' playerState=%s"),
		Context, NetModeName, IsLocalController() ? 1 : 0, HasAuthority() ? 1 : 0,
		*GetName(),
		PlayerState != nullptr ? *PlayerState->GetPlayerName() : TEXT("<none>"));
}

void ATracePlayerController::TraceNetInfo()
{
	TraceNet::LogNetworkDiagnostics(GetWorld(), TEXT("TraceNetInfo"));
	LogNetworkRole(TEXT("TraceNetInfo"));

	// Every player state on this machine, which on a server is the direct answer to "are we actually
	// in the same match?" — the acceptance test for spec v5 §0 is exactly this list containing two
	// non-bot entries.
	if (const UWorld* ControllerWorld = GetWorld())
	{
		if (const AGameStateBase* GameStateBase = ControllerWorld->GetGameState())
		{
			int32 Humans = 0;
			for (const APlayerState* PlayerStateEntry : GameStateBase->PlayerArray)
			{
				if (PlayerStateEntry == nullptr)
				{
					continue;
				}
				if (!PlayerStateEntry->IsABot())
				{
					++Humans;
				}
				UE_LOG(LogTraceGame, Display, TEXT("[Net]   playerState '%s' bot=%d"),
					*PlayerStateEntry->GetPlayerName(), PlayerStateEntry->IsABot() ? 1 : 0);
			}
			UE_LOG(LogTraceGame, Display, TEXT("[Net] %d player state(s), %d human(s)."),
				GameStateBase->PlayerArray.Num(), Humans);
		}
	}
}

// -------------------------------------------------------------------------------------------
// Accessors
// -------------------------------------------------------------------------------------------

ATraceCharacter* ATracePlayerController::GetTraceCharacter() const
{
	return Cast<ATraceCharacter>(GetPawn());
}

ATracePlayerState* ATracePlayerController::GetTracePlayerState() const
{
	return GetPlayerState<ATracePlayerState>();
}

ATraceCharacter* ATracePlayerController::GetLivingCharacter() const
{
	ATraceCharacter* TraceChar = GetTraceCharacter();
	return (TraceChar != nullptr && TraceChar->IsAlive()) ? TraceChar : nullptr;
}

// -------------------------------------------------------------------------------------------
// Menu suppression
// -------------------------------------------------------------------------------------------

namespace
{
	/**
	 * True when pausing this world would stop the game for nobody but the person who asked.
	 *
	 * REGRESSION THIS FIXES, and it is a pure consequence of spec v5 §0. The two tests below used to
	 * be a single `GetNetMode() == NM_Standalone`, written when PLAY produced a standalone session,
	 * with the comment "Solo-with-bots — which is every session today — gets a real pause". Adding
	 * `?listen` to the PLAY URL made every session a LISTEN SERVER instead, including a solo one, so
	 * that test stopped being true on the very first frame of the fix and the settings screen quietly
	 * stopped pausing anything for everybody.
	 *
	 * The rule the old code meant is not "standalone", it is "no other human is depending on this
	 * world advancing". A listen server with zero client connections satisfies that exactly, and is
	 * now the normal solo case. The moment one person joins, pausing is refused again - the original
	 * reasoning ("pausing a listen server would stop the world for everybody else because one player
	 * opened their settings") is untouched.
	 */
	bool CanPauseWithoutAffectingAnybodyElse(const UWorld* PauseWorld)
	{
		if (PauseWorld == nullptr)
		{
			return false;
		}

		const ENetMode Mode = PauseWorld->GetNetMode();
		if (Mode == NM_Standalone)
		{
			return true;
		}
		if (Mode != NM_ListenServer)
		{
			// A pure client cannot pause anything: SetPause routes through the game mode, which is
			// server-only. Unchanged behaviour, just stated.
			return false;
		}

		// ClientConnections holds the REMOTE players only - the listen host itself is not in it - so
		// an empty list is precisely "I am hosting and nobody has turned up".
		const UNetDriver* Driver = PauseWorld->GetNetDriver();
		return Driver == nullptr || Driver->ClientConnections.Num() == 0;
	}
}

void ATracePlayerController::SetGameInputSuppressed(bool bSuppressed)
{
	if (!IsLocalController() || bGameInputSuppressed == bSuppressed)
	{
		return;
	}

	bGameInputSuppressed = bSuppressed;

	if (bSuppressed)
	{
		// GameAndUI rather than UIOnly, and for the same reason the title screen uses it: this menu
		// is drawn on a Canvas and owns no Slate widget to focus, so UIOnly would route key presses
		// at a widget that does not exist and the overlay would see nothing.
		FInputModeGameAndUI InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		SetInputMode(InputMode);
		bShowMouseCursor = true;

		// Release any held gameplay input before the handlers go quiet. Without this a player who
		// opens the menu mid-burst comes back still firing, and one who opens it while walking keeps
		// walking — the release edges are about to be swallowed.
		if (ATraceCharacter* TraceChar = GetTraceCharacter())
		{
			TraceChar->DoFireReleased();
			TraceChar->StopJumping();
			TraceChar->DoMove(FVector2D::ZeroVector);
		}
		bScoreboardOpen = false;

		// SetPause routes through the game mode and is meaningless on a client, and pausing a
		// populated listen server would stop the world for everybody else because one player opened
		// their settings. Solo-with-bots still gets a real pause — see the helper above for why this
		// can no longer be a plain NM_Standalone test.
		if (CanPauseWithoutAffectingAnybodyElse(GetWorld()))
		{
			SetPause(true);
		}
	}
	else
	{
		// NOT symmetric with the pause test on purpose: if somebody joined while the settings screen
		// was open, this must still UNpause. SetPause(false) on an unpaused world is a no-op, so the
		// unconditional-on-the-server form is the safe one.
		if (HasAuthority())
		{
			SetPause(false);
		}

		// Re-takes mouse capture, hides the cursor, and — the part that matters — re-arms the
		// look-suppression window. Reacquiring capture warps the OS cursor to the centre of the
		// viewport, and that warp arrives as one enormous mouse delta; without this the player closes
		// the settings screen and is instantly spun around. Exactly the spawn-time bug, same fix.
		ApplyGameInputMode();
	}

	// The world's pause state is printed alongside, because "does opening the settings screen still
	// stop the game?" became a question the moment PLAY started opening a listen server, and the
	// answer used to be silently no. See CanPauseWithoutAffectingAnybodyElse().
	const UWorld* LogWorld = GetWorld();
	UE_LOG(LogTraceGame, Display, TEXT("[%s] Gameplay input %s. netMode=%d paused=%d"),
		*GetName(), bSuppressed ? TEXT("suppressed (menu open)") : TEXT("restored"),
		LogWorld != nullptr ? static_cast<int32>(LogWorld->GetNetMode()) : -1,
		(LogWorld != nullptr && LogWorld->IsPaused()) ? 1 : 0);
}

// -------------------------------------------------------------------------------------------
// HUD data sources
//
// These call the real gameplay accessors directly. They used to go through a SFINAE compat header
// (Settings/TraceGameplayCompat.h) while the movement and character slices were still landing;
// those slices have landed and that header is deleted, so a renamed or removed accessor is a
// compile error again rather than a meter that silently reverts to a fallback value.
// -------------------------------------------------------------------------------------------

bool ATracePlayerController::GetDashHudState(FTraceDashHudState& OutState) const
{
	const ATraceCharacter* TraceChar = GetTraceCharacter();
	if (TraceChar == nullptr || !TraceChar->IsAlive())
	{
		return false;
	}

	UTraceCharacterMovementComponent* Movement = TraceChar->GetTraceMovement();
	if (Movement == nullptr)
	{
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// GetDashCooldownRemaining() covers the whole lockout — the active dash window PLUS the cooldown
	// that follows it, because the movement component starts the timer at DashDuration + DashCooldown.
	// Dividing by DashCooldown alone would pin the meter at empty for the first DashDuration seconds.
	const float FullWindow = FMath::Max(1.e-4f, Settings.DashDuration + Settings.DashCooldown);
	const float Remaining = FMath::Max(0.f, Movement->GetDashCooldownRemaining());

	OutState.Remaining = Remaining;
	OutState.RechargeFraction = FMath::Clamp(1.f - (Remaining / FullWindow), 0.f, 1.f);

	// Direct calls, not the old SFINAE shim: the charge system has landed, so a rename in the
	// movement component must break this build rather than quietly reverting the meter to a fallback.
	OutState.MaxCharges = Movement->GetMaxDashCharges();
	OutState.Charges = Movement->GetDashCharges();

	OutState.MaxCharges = FMath::Max(1, OutState.MaxCharges);
	OutState.Charges = FMath::Clamp(OutState.Charges, 0, OutState.MaxCharges);

	// A full bank has nothing regenerating, whatever the underlying timer happens to say.
	if (OutState.Charges >= OutState.MaxCharges)
	{
		OutState.RechargeFraction = 1.f;
		OutState.Remaining = 0.f;
	}

	return true;
}

// BOOST IS GONE (spec v3 §1: "remove boost from the game entirely"). GetBoostHudState() lived here
// and fed the HUD's BOOST row; both were deleted with the feature, along with IA_Boost, its binding,
// its keybind and the whole Settings/TraceGameplayCompat.h shim that let this file call a boost that
// might not exist. The parry took its slot in the ability stack — see ATraceCharacter::
// GetParryHudState(), which the HUD calls directly.

float ATracePlayerController::GetPassProgress() const
{
	const ATraceCharacter* TraceChar = GetTraceCharacter();
	if (TraceChar == nullptr || !TraceChar->IsAlive())
	{
		return -1.f;
	}

	return TraceChar->GetPassProgress();
}

// -------------------------------------------------------------------------------------------
// Input handlers
//
// Every one of these must tolerate a null pawn: input keeps flowing during the respawn window,
// across seamless travel, and for a frame or two after the pawn is destroyed on death.
// -------------------------------------------------------------------------------------------

void ATracePlayerController::OnMoveInput(const FInputActionValue& Value)
{
	if (bGameInputSuppressed)
	{
		return;
	}

	const FVector2D MoveValue = Value.Get<FVector2D>();

	++DebugMoveEventCount;
	DebugLastMoveValue = MoveValue;
	LogFirstEventOfAction(FirstEvent_Move, TEXT("Move"));

	ATraceCharacter* TraceChar = GetLivingCharacter();

	if (InputLogLevel() >= 2)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("INPUT Move  #%d value=(%.3f, %.3f) pawn=%s"),
			DebugMoveEventCount, MoveValue.X, MoveValue.Y,
			(TraceChar != nullptr) ? *TraceChar->GetName() : TEXT("<none/dead>"));
	}

	if (TraceChar != nullptr)
	{
		TraceChar->DoMove(MoveValue);
	}
}

void ATracePlayerController::OnLookInput(const FInputActionValue& Value)
{
	if (bGameInputSuppressed)
	{
		// The overlay has the mouse. Nothing about the view may move while the player is dragging a
		// sensitivity slider with the very device that drives it.
		return;
	}

	const UWorld* World = GetWorld();
	if (World != nullptr && World->GetTimeSeconds() < IgnoreLookUntilTime)
	{
		// Inside the post-capture window; see ApplyGameInputMode.
		return;
	}

	const FVector2D LookDelta = Value.Get<FVector2D>();

	// Spike guard for every capture we are NOT told about. On macOS the viewport loses the mouse on
	// any Cmd-Tab or click-away and re-warps the cursor when the player clicks back in, producing
	// the same one-frame teleport that the suppression window above handles at spawn. Measured
	// without this guard: single frames carrying 210 deg and 59 deg of yaw, in otherwise identical
	// runs — the arrival time of the warp varies, so a time window alone cannot catch it.
	//
	// This is a RATE limit, not a clamp, and an offending event is dropped whole rather than
	// clipped: a clamped spike still snaps the view by the clamp value, which is exactly the bug.
	// The budget is deliberately far above a human hand — a hard 180 flick runs at roughly
	// 1800 deg/s — and the floor keeps the threshold sane at very high frame rates.
	const UWorld* TickWorld = World;
	const float FrameSeconds = (TickWorld != nullptr) ? TickWorld->GetDeltaSeconds() : 0.f;
	const float Threshold = FMath::Max(MinLookSpikeDegrees, MaxLookDegreesPerSecond * FrameSeconds);

	if (FMath::Abs(LookDelta.X) > Threshold || FMath::Abs(LookDelta.Y) > Threshold)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[%s] Dropped an implausible look delta (%.1f, %.1f) over %.4fs; threshold %.1f deg. ")
			TEXT("This is almost always the viewport's cursor warp on mouse capture."),
			*GetName(), LookDelta.X, LookDelta.Y, FrameSeconds, Threshold);
		return;
	}

	++DebugLookEventCount;
	DebugLastLookValue = LookDelta;
	LogFirstEventOfAction(FirstEvent_Look, TEXT("Look"));

	if (InputLogLevel() >= 2)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Look  #%d delta=(%.3f, %.3f)"),
			DebugLookEventCount, LookDelta.X, LookDelta.Y);
	}

	// Looking stays available while dead so players can watch the fight that killed them.
	if (ATraceCharacter* TraceChar = GetTraceCharacter())
	{
		TraceChar->DoLook(LookDelta);
	}
}

void ATracePlayerController::OnJumpStarted()
{
	if (bGameInputSuppressed)
	{
		return;
	}

	++DebugJumpCount;
	LogFirstEventOfAction(FirstEvent_Jump, TEXT("Jump"));
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Jump pressed #%d"), DebugJumpCount);
	}

	// ATraceCharacter deliberately exposes no DoJump — ACharacter::Jump is already
	// prediction-safe and routes through the movement component's saved moves.
	if (ATraceCharacter* TraceChar = GetLivingCharacter())
	{
		// SPEC v14 §5/§6 — THE ABILITY GETS FIRST REFUSAL ON THE JUMP KEY, and a TRUE return means
		// the normal jump must NOT also run.
		//
		// Two characters claim this key and each does so under a condition the other cannot meet, so
		// there is no arbitration to do here: Rocco's second jump only fires while AIRBORNE (on the
		// ground it declines, or a 600 uu/s launch would be replaced by a 260 uu/s one and read as
		// the jump being broken), and Oyster's jar jump only fires while STOOD ON one of his jars.
		// Everybody else — every Mannequin, every bot, every character without a jump ability —
		// returns false here and reaches ACharacter::Jump() exactly as before.
		//
		// Sent to the server as well as run locally. Neither ability is saved-move state (see the
		// report), so the local write alone would be corrected away within a round trip; running the
		// identical hook on both ends is what makes it survive. This is a documented limitation, not
		// a claim of prediction correctness.
		UTraceAbilityComponent* Abilities = TraceAbilityIntegration::IsEnabled()
			? UTraceAbilityComponent::Get(TraceChar) : nullptr;
		if (Abilities != nullptr)
		{
			if (Abilities->HandleJumpPressed())
			{
				++TraceAbilityIntegration::Counters().JumpConsumed;
				// Not on a listen-server host: there the call above WAS the authoritative one, and a
				// Server RPC executes locally, which would run the hook a second time in the same
				// frame. Both hooks happen to be latched against that, and neither is asked to be.
				if (!HasAuthority())
				{
					Abilities->ServerHandleJumpPressed();
				}
				return;
			}
		}

		TraceChar->Jump();
	}
}

void ATracePlayerController::OnJumpCompleted()
{
	// Not gated on IsAlive: releasing must always clear bPressedJump, even on a dying pawn.
	if (ATraceCharacter* TraceChar = GetTraceCharacter())
	{
		TraceChar->StopJumping();
	}
}

void ATracePlayerController::OnFireStarted()
{
	if (bGameInputSuppressed)
	{
		// Also covers the respawn request below: clicking RESUME must not double as "put me back in".
		return;
	}

	++DebugFireStartedCount;
	LogFirstEventOfAction(FirstEvent_Fire, TEXT("Fire"));

	ATraceCharacter* TraceChar = GetTraceCharacter();

	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("INPUT Fire pressed #%d pawn=%s alive=%d carrier=%d locallyControlled=%d"),
			DebugFireStartedCount,
			(TraceChar != nullptr) ? *TraceChar->GetName() : TEXT("<none>"),
			(TraceChar != nullptr) ? TraceChar->IsAlive() : 0,
			(TraceChar != nullptr) ? TraceChar->IsCarrier() : 0,
			(TraceChar != nullptr) ? TraceChar->IsLocallyControlled() : 0);
	}

	if (TraceChar == nullptr || !TraceChar->IsAlive())
	{
		// Dead: fire doubles as "get me back in". The game mode still owns respawn timing.
		ServerRequestRespawn();
		return;
	}

	// The weapon component decides whether firing is actually legal (carrying the Core, cooldown,
	// and so on) — the controller never second-guesses it.
	TraceChar->DoFirePressed();
}

void ATracePlayerController::OnFireCompleted()
{
	++DebugFireCompletedCount;
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Fire released #%d"), DebugFireCompletedCount);
	}

	// Release always propagates, so a pawn that dies mid-burst does not come back still firing.
	if (ATraceCharacter* TraceChar = GetTraceCharacter())
	{
		TraceChar->DoFireReleased();
	}
}

void ATracePlayerController::OnPassStarted()
{
	if (bGameInputSuppressed)
	{
		return;
	}

	++DebugPassCount;
	LogFirstEventOfAction(FirstEvent_Pass, TEXT("Pass"));
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Pass pressed #%d"), DebugPassCount);
	}

	if (ATraceCharacter* TraceChar = GetLivingCharacter())
	{
		TraceChar->DoPassPressed();
	}
}

void ATracePlayerController::OnPassCompleted()
{
	// Deliberately NOT gated on bGameInputSuppressed or on GetLivingCharacter(). A release that is
	// dropped leaves ATraceCore::bPassInputHeld latched, which means a shield that never comes back
	// up. Opening the pause menu mid-hold suppresses input; dying mid-hold makes the pawn non-living.
	// Both are exactly the cases where the release must still be delivered.
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Pass released #%d"), DebugPassCount);
	}

	if (ATraceCharacter* TraceChar = GetPawn<ATraceCharacter>())
	{
		TraceChar->DoPassReleased();
	}
}

void ATracePlayerController::OnDashStarted()
{
	if (bGameInputSuppressed)
	{
		return;
	}

	++DebugDashCount;
	LogFirstEventOfAction(FirstEvent_Dash, TEXT("Dash"));
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Dash pressed #%d"), DebugDashCount);
	}

	if (ATraceCharacter* TraceChar = GetLivingCharacter())
	{
		TraceChar->DoDash();
	}
}

void ATracePlayerController::OnParryStarted()
{
	if (bGameInputSuppressed)
	{
		return;
	}

	++DebugParryCount;
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Parry pressed #%d"), DebugParryCount);
	}

	// Spec v3 §3. Everything that can refuse a parry — not carrying, cooldown, dead — lives behind
	// ATraceCharacter::DoParryPressed -> TraceParry::RequestParry, which also owns the local tint
	// prediction and the server RPC. This handler deliberately makes no decision of its own; a
	// second opinion about whether a parry is legal is exactly how prediction and authority diverge.
	if (ATraceCharacter* TraceChar = GetLivingCharacter())
	{
		TraceChar->DoParryPressed();
	}
}

void ATracePlayerController::OnParryCompleted()
{
	// Intentionally not gated on bGameInputSuppressed, and intentionally empty. See the header: the
	// window is a fixed duration owned by the trail component, so there is no held state to release.
	if (ATraceCharacter* TraceChar = GetPawn<ATraceCharacter>())
	{
		TraceChar->DoParryReleased();
	}
}

void ATracePlayerController::OnSwapWeaponStarted()
{
	// FIRST LINE, before any gate: this counts "the key reached a bound delegate", which is a
	// different question from "the swap happened" and is the one the v13 §2 harness must be able to
	// ask. See the counter's declaration for the vacuous-test it exists to prevent.
	++DebugSwapPressCount;

	if (bGameInputSuppressed)
	{
		return;
	}

	// Spec v10 §1. Same discipline as the parry handler: this makes NO decision of its own.
	// TraceMelee::RequestSwapWeapon owns every refusal (dead, carrying the Core, mid-pullout,
	// mid-dash) and owns the client-side prediction plus the server RPC, so it is safe to call from
	// a listen-server host and from a remote client alike. A second opinion here is how the
	// predicted weapon and the authoritative weapon start disagreeing.
	if (ATraceCharacter* TraceChar = GetLivingCharacter())
	{
		ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
		const bool bSwapped = TraceMelee::RequestSwapWeapon(TraceChar, &Refusal);
		if (InputLogLevel() >= 1)
		{
			UE_LOG(LogTraceGame, Display, TEXT("INPUT SwapWeapon pressed -> %s (refusal=%d)"),
				bSwapped ? TEXT("swapping") : TEXT("refused"), static_cast<int32>(Refusal));
		}
	}
}

void ATracePlayerController::OnEquipKnifeStarted()
{
	HandleDirectEquip(/*bWantKnife=*/true, TEXT("EquipKnife"));
}

void ATracePlayerController::OnEquipGunStarted()
{
	HandleDirectEquip(/*bWantKnife=*/false, TEXT("EquipGun"));
}

void ATracePlayerController::HandleDirectEquip(bool bWantKnife, const TCHAR* ActionLabel)
{
	// FIRST LINE, before any gate — see OnSwapWeaponStarted above and the counter's declaration.
	++DebugEquipPressCount;

	if (bGameInputSuppressed)
	{
		return;
	}

	ATraceCharacter* TraceChar = GetLivingCharacter();
	if (TraceChar == nullptr)
	{
		return;
	}

	// =============================================================================================
	// DIRECT SELECT, spec v13 §2: "pressing 1 while already holding the knife does nothing (and must
	// not re-trigger the 0.2 s pullout)."
	//
	// THE GATE IS NOT HERE. It is TraceMelee::RequestEquipIfDifferent -> the weapon component's
	// RequestEquipIfDifferent, and it was moved down there during integration rather than being
	// re-implemented at every caller. This handler had its own copy of the "same weapon? then do
	// nothing" test for one pass, which worked for the two binds it guarded and left the rule absent
	// everywhere else — a bot or a console command doing a direct select still restarted the pullout.
	// A game rule that only holds when a particular key is the thing that asked is not a game rule.
	//
	// RequestEquip (the toggle's entry point) is deliberately UNCHANGED and still costs a pullout on
	// a redundant request; IA_SwapWeapon still calls it. The two verbs want opposite things from a
	// repeat press and the component now offers both, which is why this is a second function rather
	// than an edit to the first. Read UTraceWeaponComponent::RequestEquipIfDifferent for the full
	// argument, including why it is correct mid-pullout.
	//
	// The refusal reason is logged rather than acted on: "already holding it" comes back as
	// false/None, which is distinguishable from a genuine refusal (Dead, Carrying, NoPawn) and is
	// what the log line below prints.
	// =============================================================================================
	const ETraceEquippedWeapon Desired = bWantKnife
		? ETraceEquippedWeapon::Knife
		: ETraceEquippedWeapon::Gun;

	const bool bAlready = (TraceMelee::IsKnifeEquipped(TraceChar) == bWantKnife);

	ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
	const bool bEquipped = TraceMelee::RequestEquipIfDifferent(TraceChar, Desired, &Refusal);

	if (InputLogLevel() >= 1)
	{
		if (bAlready)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("INPUT %s pressed -> ignored, %s already equipped (deployRemaining=%.3f, NOT restarted)"),
				ActionLabel, bWantKnife ? TEXT("knife") : TEXT("gun"),
				TraceMelee::GetDeployRemaining(TraceChar));
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("INPUT %s pressed -> %s (refusal=%d, deployRemaining=%.3f)"),
				ActionLabel, bEquipped ? TEXT("equipping") : TEXT("refused"),
				static_cast<int32>(Refusal), TraceMelee::GetDeployRemaining(TraceChar));
		}
	}
}

void ATracePlayerController::OnCrouchStarted()
{
	if (bGameInputSuppressed)
	{
		return;
	}

	++DebugCrouchCount;
	LogFirstEventOfAction(FirstEvent_Crouch, TEXT("Crouch"));
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Crouch pressed #%d"), DebugCrouchCount);
	}

	// ACharacter::Crouch(), not a bespoke DoCrouch. It sets bWantsToCrouch, which is already part of
	// FSavedMove_Character's compressed flags and therefore already client-predicted and already
	// replicated — exactly the round-trip spec §5 demands. The movement slice hooks the resulting
	// OnStartCrouch/OnEndCrouch to turn it into a ground slide or an air fast-fall; the controller
	// deliberately does not encode which of those it is, because that decision needs the movement
	// mode and belongs on the movement component.
	if (ATraceCharacter* TraceChar = GetLivingCharacter())
	{
		TraceChar->Crouch();
	}
}

void ATracePlayerController::OnCrouchCompleted()
{
	// Not gated on IsAlive or on suppression: a release must ALWAYS propagate, or a pawn that dies
	// (or opens the menu) mid-crouch comes back permanently crouched.
	if (ATraceCharacter* TraceChar = GetTraceCharacter())
	{
		TraceChar->UnCrouch();
	}
}

void ATracePlayerController::OnScoreboardStarted()
{
	if (bGameInputSuppressed)
	{
		return;
	}

	++DebugScoreboardCount;
	LogFirstEventOfAction(FirstEvent_Scoreboard, TEXT("Scoreboard"));
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Scoreboard down #%d"), DebugScoreboardCount);
	}

	bScoreboardOpen = true;
}

void ATracePlayerController::OnScoreboardCompleted()
{
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT Scoreboard up"));
	}

	bScoreboardOpen = false;
}

// -------------------------------------------------------------------------------------------
// SPEC v14 §5 — the ability binds
//
// Both handlers are deliberately three lines and hold no rules of their own. Every refusal an
// ability can have — dead, no pawn, no character, characters disabled (mode A or the toggle),
// match not live, half-time break, cooldown still running, the character's own CanActivate() —
// lives in UTraceAbilityComponent::TryActivate(), which is also what the console command and the
// interim relay call. A second copy of any of those tests here is how the two paths would come to
// disagree about whether E did anything.
//
// The PlayerState, not the pawn, is the component's home (it must survive death — see the cooldown
// contract), but Get() resolves through either, so the pawn is the natural handle to pass.
// -------------------------------------------------------------------------------------------

void ATracePlayerController::OnAbilityStarted()
{
	if (bGameInputSuppressed)
	{
		return;
	}

	if (!TraceAbilityIntegration::IsEnabled())
	{
		return;   // RED ARM: the key is bound but reaches nothing, exactly as before this pass.
	}

	// Through the relay's router, not straight to HandleActivatePressed: see the declaration.
	// The router works with or without a relay component present, and applies Mace's reactivation
	// rule in exactly one place.
	++TraceAbilityIntegration::Counters().ActivatePressed;
	UTraceAbilityInputRelay::RouteActivatePressed(GetPlayerState<APlayerState>());
}

void ATracePlayerController::OnAbilitySecondaryStarted()
{
	if (bGameInputSuppressed)
	{
		return;
	}

	if (!TraceAbilityIntegration::IsEnabled())
	{
		return;
	}

	++TraceAbilityIntegration::Counters().SecondaryEdges;
	UTraceAbilityInputRelay::RouteSecondaryEdge(GetPlayerState<APlayerState>(), /*bDown=*/true);
}

void ATracePlayerController::OnAbilitySecondaryCompleted()
{
	// NOT gated on suppression, exactly like OnPassCompleted and OnCrouchCompleted: a release must
	// always propagate. Opening the pause menu while holding V must not leave Mace hanging in the
	// air with gravity switched off until she dies.
	//
	// NOT gated on the integration red arm either, for the same reason: disarming the wiring
	// mid-hold must not strand her. Release is always safe — the ability's own hook decides whether
	// there is anything to end.
	++TraceAbilityIntegration::Counters().SecondaryEdges;
	UTraceAbilityInputRelay::RouteSecondaryEdge(GetPlayerState<APlayerState>(), /*bDown=*/false);
}

// -------------------------------------------------------------------------------------------
// Diagnostics
// -------------------------------------------------------------------------------------------

void ATracePlayerController::LogFirstEventOfAction(uint8 Bit, const TCHAR* ActionName)
{
	if ((FirstEventLoggedMask & Bit) != 0)
	{
		return;
	}
	FirstEventLoggedMask |= Bit;

	UE_LOG(LogTraceGame, Display,
		TEXT("[%s] Input OK: the '%s' action reached the controller for the first time."),
		*GetName(), ActionName);
}

void ATracePlayerController::LogInputDiagnostics(const TCHAR* Context) const
{
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();

	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTDIAG [%s] controller=%s netmode=%d isLocal=%d localPlayer=%s"),
		Context, *GetName(), static_cast<int32>(GetNetMode()), IsLocalController() ? 1 : 0,
		(LocalPlayer != nullptr) ? *LocalPlayer->GetName() : TEXT("<none>"));

	// UEnhancedPlayerInput is the hard requirement nothing else checks: AddMappingContext is a
	// silent no-op against a plain UPlayerInput, and DefaultPlayerInputClass lives in an ini.
	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTDIAG [%s] PlayerInput=%s isEnhanced=%d"),
		Context,
		(PlayerInput != nullptr) ? *PlayerInput->GetClass()->GetName() : TEXT("<null>"),
		(Cast<UEnhancedPlayerInput>(PlayerInput) != nullptr) ? 1 : 0);

	const UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTDIAG [%s] InputComponent=%s isEnhanced=%d actionBindings=%d"),
		Context,
		(InputComponent != nullptr) ? *InputComponent->GetClass()->GetName() : TEXT("<null>"),
		(EIC != nullptr) ? 1 : 0,
		(EIC != nullptr) ? EIC->GetActionEventBindings().Num() : -1);

	int32 MappingCount = -1;
	int32 bContextRegistered = -1;
	if (InputMapping != nullptr)
	{
		MappingCount = InputMapping->GetMappings().Num();

		if (LocalPlayer != nullptr)
		{
			if (const UEnhancedInputLocalPlayerSubsystem* Subsystem =
					LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
			{
				bContextRegistered = Subsystem->HasMappingContext(InputMapping) ? 1 : 0;
			}
		}
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTDIAG [%s] MappingContext=%s keyMappings=%d registeredWithSubsystem=%d"),
		Context,
		(InputMapping != nullptr) ? *InputMapping->GetName() : TEXT("<null>"),
		MappingCount, bContextRegistered);

	// A wrong ValueType is the classic silent failure: Axis2D read as a bool, or a bool action
	// asked for a FVector2D, both yield zero with no warning anywhere.
	auto LogAction = [Context, this](const TCHAR* Label, const UInputAction* Action)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUTDIAG [%s]   %s valid=%d valueType=%d"),
			Context, Label, (Action != nullptr) ? 1 : 0,
			(Action != nullptr) ? static_cast<int32>(Action->ValueType) : -1);
	};
	LogAction(TEXT("IA_Move      "), IA_Move);
	LogAction(TEXT("IA_Look      "), IA_Look);
	LogAction(TEXT("IA_Jump      "), IA_Jump);
	LogAction(TEXT("IA_Fire      "), IA_Fire);
	LogAction(TEXT("IA_Pass      "), IA_Pass);
	LogAction(TEXT("IA_Dash      "), IA_Dash);
	LogAction(TEXT("IA_Parry     "), IA_Parry);
	LogAction(TEXT("IA_Crouch    "), IA_Crouch);
	LogAction(TEXT("IA_Scoreboard"), IA_Scoreboard);
	LogAction(TEXT("IA_SwapWeapon"), IA_SwapWeapon);
	LogAction(TEXT("IA_EquipKnife"), IA_EquipKnife);
	LogAction(TEXT("IA_EquipGun  "), IA_EquipGun);

	// The player's own settings are now part of "why does input feel wrong", so they belong in the
	// same dump. A hand-edited or half-migrated TraceUserSettings.ini is otherwise invisible.
	const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();
	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTDIAG [%s] settings sensitivity=%.2f yScale=%.2f invertY=%d lookScale=(%.3f, %.3f) menuSuppressed=%d"),
		Context, UserSettings.MouseSensitivity, UserSettings.MouseSensitivityYScale,
		UserSettings.bInvertMouseY ? 1 : 0,
		UserSettings.GetLookScaleX(), UserSettings.GetLookScaleY(), bGameInputSuppressed ? 1 : 0);

	for (const FTraceInputActionInfo& Info : TraceInputActions::All())
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUTDIAG [%s]   bind %-16s -> %s"),
			Context, Info.ConfigId, *UTraceUserSettings::DescribeKey(UserSettings.GetKey(Info.Action)));
	}

	// The mouse-capture mode is the one part of this that is not ours and can change behind our
	// back a frame after we set it — see ApplyGameInputMode. Anything other than
	// CapturePermanently_IncludingInitialMouseDown (2) means the click that re-captures the window
	// is being eaten, which on macOS is felt as "some of my shots just do nothing".
	if (GEngine != nullptr && GEngine->GameViewport != nullptr)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("INPUTDIAG [%s] viewport captureMode=%d (2 = CapturePermanently_IncludingInitialMouseDown) lockMode=%d ignoreInput=%d"),
			Context,
			static_cast<int32>(GEngine->GameViewport->GetMouseCaptureMode()),
			static_cast<int32>(GEngine->GameViewport->GetMouseLockMode()),
			GEngine->GameViewport->IgnoreInput() ? 1 : 0);
	}

	const ATraceCharacter* TraceChar = GetTraceCharacter();
	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTDIAG [%s] pawn=%s alive=%d locallyControlled=%d location=%s"),
		Context,
		(TraceChar != nullptr) ? *TraceChar->GetName() : TEXT("<none>"),
		(TraceChar != nullptr) ? TraceChar->IsAlive() : 0,
		(TraceChar != nullptr) ? TraceChar->IsLocallyControlled() : 0,
		(TraceChar != nullptr) ? *TraceChar->GetActorLocation().ToCompactString() : TEXT("-"));

	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTDIAG [%s] counters move=%d look=%d fireDown=%d fireUp=%d jump=%d pass=%d dash=%d parry=%d crouch=%d hitConfirm=%d lastMove=(%.2f, %.2f)"),
		Context, DebugMoveEventCount, DebugLookEventCount, DebugFireStartedCount,
		DebugFireCompletedCount, DebugJumpCount, DebugPassCount, DebugDashCount,
		DebugParryCount, DebugCrouchCount,
		DebugHitConfirmCount, DebugLastMoveValue.X, DebugLastMoveValue.Y);
}

// -------------------------------------------------------------------------------------------
// RPCs
// -------------------------------------------------------------------------------------------

void ATracePlayerController::ClientNotifyHit_Implementation(bool bKilled, ETraceHitZone Zone)
{
	if (const UWorld* World = GetWorld())
	{
		LastHitMarkerTime = World->GetTimeSeconds();
	}
	bLastHitMarkerWasKill = bKilled;
	LastHitMarkerZone = Zone;

	// End-to-end proof for the harness: the server only ever sends this after ServerFire ran the
	// lag-compensated resolver and found one of our bullets on somebody. It cannot be produced by
	// any local, client-side part of the fire path.
	++DebugHitConfirmCount;
	if (InputLogLevel() >= 1)
	{
		UE_LOG(LogTraceGame, Display, TEXT("INPUT HitConfirm #%d killed=%d zone=%s"),
			DebugHitConfirmCount, bKilled ? 1 : 0, TraceHitZoneToString(Zone));
	}
}

void ATracePlayerController::ClientNotifyKilledBy_Implementation(const FString& KillerName, FName Cause)
{
	LastKillerName = KillerName;
	LastDeathCause = Cause;

	UE_LOG(LogTraceGame, Verbose, TEXT("[%s] Killed by '%s' (%s)"), *GetName(), *KillerName, *Cause.ToString());
}

void ATracePlayerController::ClientNotifyParryKill_Implementation(const FString& VictimName)
{
	// CLIENT-LOCAL time on purpose, exactly like LastHitMarkerTime: the HUD fades this banner out
	// against UWorld::GetTimeSeconds() on this machine, and a server timestamp would be offset by
	// the client's own clock delta and could fade the banner before it was ever drawn.
	if (const UWorld* World = GetWorld())
	{
		LastParryKillTime = World->GetTimeSeconds();
	}
	LastParryKillVictim = VictimName;

	UE_LOG(LogTraceGame, Verbose, TEXT("[%s] Parry kill on '%s'"), *GetName(), *VictimName);
}

void ATracePlayerController::ServerRequestRespawn_Implementation()
{
	// Contract §8: never check() on network input — validate and return.
	if (!HasAuthority())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	if (Now - LastRespawnRequestTime < RespawnRequestCooldown)
	{
		return;
	}
	LastRespawnRequestTime = Now;

	// Deliberately conservative: ATraceGameMode owns respawn scheduling and its timer will restart
	// us on RespawnDelay. We only ever help when there is no pawn at all — with a (dead) pawn
	// still possessed, AGameModeBase::RestartPlayer reuses it and would just teleport the corpse
	// to a spawn point, and we would be racing the mode's own timer into a double respawn.
	//
	// The case this really covers is a controller that never got a pawn in the first place
	// (joined during warm-up, spawn failed, travelled in mid-match).
	if (GetPawn() != nullptr)
	{
		return;
	}

	if (AGameModeBase* GameMode = World->GetAuthGameMode())
	{
		GameMode->RestartPlayer(this);
	}
}


#if !UE_BUILD_SHIPPING

// =================================================================================================
// Trace.V13.Hotkeys — SPEC v13 §2, THE EVIDENCE HALF.
//
// WHY THIS COMMAND EXISTS. §2 is three claims, and only the first is visible in a bind list:
//
//   1. 1 equips the knife and 2 equips the gun.
//   2. They are DIRECT SELECT, not a toggle — 1 pressed twice leaves the knife out, it does not
//      put it away again.
//   3. Pressing 1 with the knife already out does NOT re-trigger the 0.2 s pullout.
//
// The third is the one that cannot be seen by looking, and it is the one the shipped weapon
// component does not give for free: UTraceWeaponComponent::RequestEquip restarts the pullout on a
// redundant request BY DESIGN, for the toggle's sake. So it gets measured rather than asserted.
//
// IT DRIVES THE REAL KEYS THROUGH THE REAL PIPELINE. Every press below is Trace.SimInput on the key
// UTraceUserSettings currently has bound to the action — not a call to the handler, not a call to
// TraceMelee. A rebind, an unbound action, a mapping that never reached the Enhanced Input
// subsystem and a handler that was never bound all fail this test.
//
// ---------------------------------------------------------------------------------------------
// HOW IT GOES RED, WHICH IS THE PART THAT MATTERS
//
//   Trace.V13.Hotkeys toggle
//       THE RED ARM. Identical sequence, identical assertions, but every press that would be "1"
//       is sent to the SwapWeapon TOGGLE — the same code path minus the idempotence guard §2 added.
//       Steps 5 fails on it (the third press swaps the weapon back AND starts a fresh pullout),
//       which is precisely the behaviour §2 asked to be rid of. Same build, same run, same checks.
//
// AND HOW IT WAS ITSELF WRONG ONCE, RECORDED BECAUSE THE PROJECT KEEPS PAYING FOR THIS CLASS OF
// MISTAKE: the first version held each synthetic key for 0.05 s and pressed again 0.05 s later, so
// the redundant press landed while the key was still logically DOWN. Enhanced Input emits Started
// on a down EDGE only, the handler never ran, and "the pullout was not restarted" was true because
// nothing had happened at all. It passed — and passed in the red arm too, which is what exposed it.
// Two things changed: the presses are spaced well clear of their own release, and every press now
// asserts that ATracePlayerController's press counter actually moved. A step whose key never
// arrived now fails the run instead of quietly passing it.
// ---------------------------------------------------------------------------------------------
//
//     UnrealEditor Trace.uproject /Game/Maps/Arena -game -RenderOffScreen \
//         -ExecCmds="Trace.V13.Hotkeys" -abslog=/tmp/v13.log
//
// Self-schedules until a living, non-carrying local pawn exists, so it is safe at launch.
// =================================================================================================

namespace
{
	/** Synthetic hold, in seconds. Must be well under the gap between steps — see the header. */
	constexpr float V13PressHoldSeconds = 0.02f;

	struct FV13HotkeyProbe
	{
		double StartTime = -1.0;
		int32 Step = 0;
		int32 Passes = 0;
		int32 Failures = 0;

		/** Pullout remaining sampled just before the redundant press. */
		float DeployBeforeRedundant = -1.f;

		/** Press counter sampled at the moment of the last synthetic press. */
		int32 CountAtPress = -1;

		/**
		 * THE RED ARM. When true, every press that would be "1" is sent to the SwapWeapon TOGGLE
		 * instead, against the identical checks. See the file-block header.
		 */
		bool bToggleArm = false;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition)
			{
				++Passes;
				UE_LOG(LogTraceGame, Display, TEXT("[V13.Hotkeys]   PASS  %s"), *What);
			}
			else
			{
				++Failures;
				UE_LOG(LogTraceGame, Error, TEXT("[V13.Hotkeys]   FAIL  %s"), *What);
			}
		}
	};

	ATracePlayerController* V13Controller(UWorld* World)
	{
		return (World != nullptr) ? Cast<ATracePlayerController>(World->GetFirstPlayerController()) : nullptr;
	}

	ATraceCharacter* V13LocalPawn(UWorld* World)
	{
		ATracePlayerController* PC = V13Controller(World);
		ATraceCharacter* Character = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		return (Character != nullptr && Character->IsAlive()) ? Character : nullptr;
	}

	/** Presses the key the player currently has bound to @p Action, through the real input path. */
	bool V13PressBoundKey(UWorld* World, ETraceInputAction Action, const TCHAR* Label)
	{
		const FKey Key = UTraceUserSettings::Get().GetKey(Action);
		if (!Key.IsValid())
		{
			UE_LOG(LogTraceGame, Error, TEXT("[V13.Hotkeys]   %s is UNBOUND — cannot press it."), Label);
			return false;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[V13.Hotkeys]   pressing %s (key %s)"), Label, *Key.GetFName().ToString());
		GEngine->Exec(World, *FString::Printf(TEXT("Trace.SimInput %s %.2f"),
			*Key.GetFName().ToString(), V13PressHoldSeconds));
		return true;
	}

	void V13RunHotkeyProbe(bool bToggleArm)
	{
		UWorld* World = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->IsGameWorld())
				{
					World = Context.World();
					break;
				}
			}
		}

		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[V13.Hotkeys] no game world yet."));
			return;
		}

		// The handler's own log lines are half the evidence — "ignored, knife already equipped" is
		// the sentence §2 asks for — so the probe turns them on rather than hoping the run did.
		// NOT named LogInput. InputCore declares a global log category by that exact name
		// (DECLARE_LOG_CATEGORY_EXTERN(LogInput, ...) in InputCoreTypes.h), so a local called
		// LogInput is C4459 "declaration hides global declaration" — an ERROR on MSVC under
		// Unreal's warnings-as-errors, and completely silent on clang. That asymmetry has now
		// broken this project's Windows build three separate times. The identical call in
		// Source/Trace/Debug/TraceInputHarness.cpp is already named LogInputCVar for this reason.
		if (IConsoleVariable* LogInputCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.LogInput")))
		{
			LogInputCVar->Set(1, ECVF_SetByConsole);
		}

		TSharedRef<FV13HotkeyProbe> Probe = MakeShared<FV13HotkeyProbe>();
		Probe->bToggleArm = bToggleArm;

		// The knife presses go to the direct-select bind, or — in the red arm — to the toggle.
		const ETraceInputAction KnifeAction = bToggleArm ? ETraceInputAction::SwapWeapon : ETraceInputAction::EquipKnife;
		const TCHAR* const KnifeLabel = bToggleArm ? TEXT("SwapWeapon [RED ARM]") : TEXT("EquipKnife");

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Probe, World, KnifeAction, KnifeLabel](float /*Delta*/) -> bool
			{
				if (!IsValid(World))
				{
					return false;
				}

				ATracePlayerController* PC = V13Controller(World);
				ATraceCharacter* Pawn = V13LocalPawn(World);
				if (PC == nullptr || Pawn == nullptr || Pawn->IsCarrier())
				{
					// Wait for a living pawn that is allowed to swap at all. A carrier is refused by
					// RequestEquip by design (hands full), so testing §2 on one would measure the
					// carrier rule rather than the hotkeys.
					return true;
				}

				// Whichever handler this arm is exercising, this is the count of presses that have
				// reached a bound delegate. Comparing it either side of a synthetic press is what
				// makes a swallowed key a FAILURE rather than a silent pass.
				const int32 PressCount = Probe->bToggleArm
					? PC->GetDebugSwapPressCount()
					: PC->GetDebugEquipPressCount();

				const double Now = FPlatformTime::Seconds();
				if (Probe->StartTime < 0.0)
				{
					Probe->StartTime = Now;
					UE_LOG(LogTraceGame, Display,
						TEXT("[V13.Hotkeys] start on %s, arm=%s. EquipKnife=%s EquipGun=%s SwapWeapon=%s | pullout=%.3fs hold=%.2fs"),
						*GetNameSafe(Pawn),
						Probe->bToggleArm ? TEXT("TOGGLE (RED ARM — these checks are EXPECTED to fail)") : TEXT("direct select"),
						*UTraceUserSettings::DescribeKey(UTraceUserSettings::Get().GetKey(ETraceInputAction::EquipKnife)),
						*UTraceUserSettings::DescribeKey(UTraceUserSettings::Get().GetKey(ETraceInputAction::EquipGun)),
						*UTraceUserSettings::DescribeKey(UTraceUserSettings::Get().GetKey(ETraceInputAction::SwapWeapon)),
						TraceMelee::GetSwapSeconds(), V13PressHoldSeconds);
				}

				const double T = Now - Probe->StartTime;
				const bool bKnife = TraceMelee::IsKnifeEquipped(Pawn);
				const float Deploy = TraceMelee::GetDeployRemaining(Pawn);

				switch (Probe->Step)
				{
				case 0:
					// Baseline: put the GUN in hand with the "2" key, so the run starts from a known
					// weapon whatever the pawn spawned with.
					V13PressBoundKey(World, ETraceInputAction::EquipGun, TEXT("EquipGun"));
					++Probe->Step;
					break;

				case 1:
					if (T > 0.35)
					{
						Probe->Check(!bKnife, TEXT("step 1: '2' put the GUN in hand"));
						Probe->Check(FMath::IsNearlyZero(Deploy, 1e-3f),
							FString::Printf(TEXT("step 1: baseline pullout finished (remaining=%.3f)"), Deploy));
						Probe->CountAtPress = PressCount;
						V13PressBoundKey(World, KnifeAction, KnifeLabel);
						++Probe->Step;
					}
					break;

				case 2:
					if (T > 0.45)
					{
						Probe->Check(PressCount > Probe->CountAtPress,
							FString::Printf(TEXT("step 2: the press REACHED the handler (count %d -> %d) — without this the rest of this test would be vacuous"),
								Probe->CountAtPress, PressCount));
						Probe->Check(bKnife, TEXT("step 2: '1' put the KNIFE in hand"));
						Probe->Check(Deploy > 0.f,
							FString::Printf(TEXT("step 2: a real pullout started (remaining=%.3f)"), Deploy));
						Probe->DeployBeforeRedundant = Deploy;
						// THE MEASUREMENT. Press again while the knife's own pullout is still
						// running. If the request goes through, DeployEndServerTime is re-anchored to
						// now and the remaining time JUMPS BACK UP to the full 0.2 s.
						Probe->CountAtPress = PressCount;
						V13PressBoundKey(World, KnifeAction, KnifeLabel);
						++Probe->Step;
					}
					break;

				case 3:
					if (T > 0.53)
					{
						Probe->Check(PressCount > Probe->CountAtPress,
							FString::Printf(TEXT("step 3: the REDUNDANT mid-pullout press reached the handler (count %d -> %d)"),
								Probe->CountAtPress, PressCount));
						Probe->Check(bKnife, TEXT("step 3: still the KNIFE — direct select did not toggle back to the gun"));
						Probe->Check(Deploy < Probe->DeployBeforeRedundant,
							FString::Printf(TEXT("step 3: pullout NOT restarted — remaining fell %.3f -> %.3f"),
								Probe->DeployBeforeRedundant, Deploy));
						++Probe->Step;
					}
					break;

				case 4:
					if (T > 0.65)
					{
						Probe->Check(FMath::IsNearlyZero(Deploy, 1e-3f),
							FString::Printf(TEXT("step 4: pullout ran to completion (remaining=%.3f)"), Deploy));
						// And again with the weapon fully up: still nothing.
						Probe->CountAtPress = PressCount;
						V13PressBoundKey(World, KnifeAction, KnifeLabel);
						++Probe->Step;
					}
					break;

				case 5:
					if (T > 0.75)
					{
						Probe->Check(PressCount > Probe->CountAtPress,
							FString::Printf(TEXT("step 5: the third press reached the handler (count %d -> %d)"),
								Probe->CountAtPress, PressCount));
						Probe->Check(bKnife, TEXT("step 5: still the KNIFE after a third press"));
						Probe->Check(FMath::IsNearlyZero(Deploy, 1e-3f),
							FString::Printf(TEXT("step 5: NO new pullout from the redundant press (remaining=%.3f)"), Deploy));
						V13PressBoundKey(World, ETraceInputAction::EquipGun, TEXT("EquipGun"));
						++Probe->Step;
					}
					break;

				case 6:
					if (T > 0.85)
					{
						Probe->Check(!bKnife, TEXT("step 6: '2' switched back to the GUN"));
						Probe->Check(Deploy > 0.f,
							FString::Printf(TEXT("step 6: a genuine change DOES cost a pullout (remaining=%.3f)"), Deploy));
						++Probe->Step;
					}
					break;

				default:
					if (Probe->Failures == 0)
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[V13.Hotkeys] RESULT: PASS — %d checks (arm=%s). 1 = knife, 2 = gun, direct select, "
							     "every press verified to have reached the handler, and a redundant press costs no pullout."),
							Probe->Passes, Probe->bToggleArm ? TEXT("toggle") : TEXT("direct select"));
					}
					else
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[V13.Hotkeys] RESULT: FAIL — %d passed, %d FAILED (arm=%s%s)."),
							Probe->Passes, Probe->Failures,
							Probe->bToggleArm ? TEXT("toggle") : TEXT("direct select"),
							Probe->bToggleArm ? TEXT(" — THIS IS THE RED ARM AND FAILING IS THE POINT") : TEXT(""));
					}
					return false;   // done
				}

				return true;
			}), 0.f);
	}

	FAutoConsoleCommand CmdV13Hotkeys(
		TEXT("Trace.V13.Hotkeys"),
		TEXT("Dev only. Spec v13 §2: press the bound EquipKnife/EquipGun keys through the real input pipeline and prove direct select does not re-trigger the pullout. Pass 'toggle' for the RED ARM, which sends the same presses to the SwapWeapon toggle and is expected to FAIL."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const bool bToggleArm = (Args.Num() > 0) && Args[0].Equals(TEXT("toggle"), ESearchCase::IgnoreCase);
			V13RunHotkeyProbe(bToggleArm);
		}));
}

#endif // !UE_BUILD_SHIPPING
