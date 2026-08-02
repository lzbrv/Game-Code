// Trace — player controller implementation.

#include "Core/TracePlayerController.h"

#include "Core/TraceCharacter.h"
#include "Core/TracePlayerState.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"       // ACharacter::Jump / StopJumping on ATraceCharacter
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerInput.h"    // APlayerController::PlayerInput, for the setup diagnostic
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"                // EKeys
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"                 // ETriggerEvent
#include "Trace.h"                         // LogTraceGame

namespace
{
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
	 * NOTE (unverified API detail): UInputModifierNegate exposes per-axis bX/bY/bZ flags, but we
	 * cannot verify their exact names or defaults without compiling. So we rely only on the
	 * documented default behaviour — negate every component — which is exactly right here because
	 * every mapping we attach this to carries a single non-zero component (each key is 1D, and the
	 * swizzle happens first). If you ever need to invert one axis of a genuinely 2D key such as
	 * EKeys::Mouse2D, split it into two 1D mappings the way the Look binding below does rather
	 * than reaching for the per-axis flags.
	 */
	UInputModifierNegate* MakeNegate(UObject* Outer)
	{
		return NewObject<UInputModifierNegate>(Outer);
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
		// Mouse-look shooter: the viewport swallows the cursor and nothing else wants input.
		SetInputMode(FInputModeGameOnly());
		bShowMouseCursor = false;

		// SetupInputComponent has normally already run, but possession/init order differs between
		// standalone, listen server and client, so make sure the context really is live.
		AddInputMappings();
	}
}

void ATracePlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The subsystem outlives this controller (it belongs to the ULocalPlayer). Leaving our context
	// registered would keep a dead controller's actions in the mapping stack across travel.
	RemoveInputMappings();

	Super::EndPlay(EndPlayReason);
}

// -------------------------------------------------------------------------------------------
// Enhanced Input construction
// -------------------------------------------------------------------------------------------

void ATracePlayerController::BuildInputData()
{
	if (InputMapping != nullptr)
	{
		return;
	}

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

	// --- Move: WASD -> (X = strafe, +right), (Y = forward, +forward) ---------------------------
	//
	// Opposing keys sum to zero because Enhanced Input combines every mapping of an action.
	//
	// MapKey returns a reference *into* the context's mapping array, so it is invalidated by the
	// next MapKey call. Each binding therefore gets its own scope and uses the reference
	// immediately — never cache one.
	{
		// W: forward. 1D X -> Y.
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, EKeys::W);
		Mapping.Modifiers.Add(MakeSwizzleXToY(InputMapping));
	}
	{
		// S: backward. 1D X -> Y, then inverted.
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, EKeys::S);
		Mapping.Modifiers.Add(MakeSwizzleXToY(InputMapping));
		Mapping.Modifiers.Add(MakeNegate(InputMapping));
	}
	{
		// A: strafe left — X, inverted.
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, EKeys::A);
		Mapping.Modifiers.Add(MakeNegate(InputMapping));
	}
	{
		// D: strafe right — raw X, no modifiers at all.
		InputMapping->MapKey(IA_Move, EKeys::D);
	}

	// --- Look: mouse -> (X = yaw delta, Y = pitch delta) ---------------------------------------
	//
	// Deliberately two 1D mappings rather than one EKeys::Mouse2D mapping: raw MouseY is positive
	// when the mouse moves *up*, but AddControllerPitchInput treats positive as *down*, so Y needs
	// inverting. Splitting the axes lets us use a whole-vector Negate — the only Negate behaviour
	// we are confident about — instead of the per-axis flags.
	{
		InputMapping->MapKey(IA_Look, EKeys::MouseX);
	}
	{
		FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Look, EKeys::MouseY);
		Mapping.Modifiers.Add(MakeSwizzleXToY(InputMapping));
		Mapping.Modifiers.Add(MakeNegate(InputMapping));
	}

	// --- Buttons -------------------------------------------------------------------------------
	//
	// No explicit triggers: an action with no trigger uses the implicit "down" trigger, which
	// gives us Started on press, Triggered while held and Completed on release. That is exactly
	// the shape the handlers below expect.
	InputMapping->MapKey(IA_Jump,       EKeys::SpaceBar);
	InputMapping->MapKey(IA_Fire,       EKeys::LeftMouseButton);
	InputMapping->MapKey(IA_Pass,       EKeys::RightMouseButton);
	InputMapping->MapKey(IA_Dash,       EKeys::LeftShift);
	InputMapping->MapKey(IA_Scoreboard, EKeys::Tab);

	UE_LOG(LogTraceGame, Verbose, TEXT("[%s] Built C++ Enhanced Input data (%d key mappings)"),
		*GetName(), InputMapping->GetMappings().Num());
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
	// guess. Latched: without the flag this would fire again on every respawn.
	if (!bInputContextChecked)
	{
		bInputContextChecked = true;

		if (!Subsystem->HasMappingContext(InputMapping))
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] Enhanced Input refused the mapping context — no gameplay input will reach the pawn. ")
				TEXT("Check [/Script/Engine.InputSettings] DefaultPlayerInputClass; PlayerInput is currently '%s'."),
				*GetName(),
				PlayerInput ? *PlayerInput->GetClass()->GetName() : TEXT("<none>"));
		}
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

	EIC->BindAction(IA_Pass, ETriggerEvent::Started,   this, &ATracePlayerController::OnPassStarted);
	EIC->BindAction(IA_Dash, ETriggerEvent::Started,   this, &ATracePlayerController::OnDashStarted);

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
}

void ATracePlayerController::AcknowledgePossession(APawn* P)
{
	Super::AcknowledgePossession(P);

	// Client path: the local player definitely exists by now, even if SetupInputComponent ran
	// before it did.
	AddInputMappings();

	// Deliberately does NOT reset bScoreboardOpen. Enhanced Input state lives on the local player,
	// not the pawn, so a player holding Tab through their own death and respawn is still holding it
	// afterwards — clearing the flag here would blank the scoreboard mid-hold and it would not come
	// back until they released and pressed again. The Completed/Canceled bindings own that flag.
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
	ATraceCharacter* Character = GetTraceCharacter();
	return (Character != nullptr && Character->IsAlive()) ? Character : nullptr;
}

// -------------------------------------------------------------------------------------------
// Input handlers
//
// Every one of these must tolerate a null pawn: input keeps flowing during the respawn window,
// across seamless travel, and for a frame or two after the pawn is destroyed on death.
// -------------------------------------------------------------------------------------------

void ATracePlayerController::OnMoveInput(const FInputActionValue& Value)
{
	if (ATraceCharacter* Character = GetLivingCharacter())
	{
		Character->DoMove(Value.Get<FVector2D>());
	}
}

void ATracePlayerController::OnLookInput(const FInputActionValue& Value)
{
	// Looking stays available while dead so players can watch the fight that killed them.
	if (ATraceCharacter* Character = GetTraceCharacter())
	{
		Character->DoLook(Value.Get<FVector2D>());
	}
}

void ATracePlayerController::OnJumpStarted()
{
	// ATraceCharacter deliberately exposes no DoJump — ACharacter::Jump is already
	// prediction-safe and routes through the movement component's saved moves.
	if (ATraceCharacter* Character = GetLivingCharacter())
	{
		Character->Jump();
	}
}

void ATracePlayerController::OnJumpCompleted()
{
	// Not gated on IsAlive: releasing must always clear bPressedJump, even on a dying pawn.
	if (ATraceCharacter* Character = GetTraceCharacter())
	{
		Character->StopJumping();
	}
}

void ATracePlayerController::OnFireStarted()
{
	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsAlive())
	{
		// Dead: fire doubles as "get me back in". The game mode still owns respawn timing.
		ServerRequestRespawn();
		return;
	}

	// The weapon component decides whether firing is actually legal (carrying the Core, cooldown,
	// and so on) — the controller never second-guesses it.
	Character->DoFirePressed();
}

void ATracePlayerController::OnFireCompleted()
{
	// Release always propagates, so a pawn that dies mid-burst does not come back still firing.
	if (ATraceCharacter* Character = GetTraceCharacter())
	{
		Character->DoFireReleased();
	}
}

void ATracePlayerController::OnPassStarted()
{
	if (ATraceCharacter* Character = GetLivingCharacter())
	{
		Character->DoPass();
	}
}

void ATracePlayerController::OnDashStarted()
{
	if (ATraceCharacter* Character = GetLivingCharacter())
	{
		Character->DoDash();
	}
}

void ATracePlayerController::OnScoreboardStarted()
{
	bScoreboardOpen = true;
}

void ATracePlayerController::OnScoreboardCompleted()
{
	bScoreboardOpen = false;
}

// -------------------------------------------------------------------------------------------
// RPCs
// -------------------------------------------------------------------------------------------

void ATracePlayerController::ClientNotifyHit_Implementation(bool bKilled)
{
	if (const UWorld* World = GetWorld())
	{
		LastHitMarkerTime = World->GetTimeSeconds();
	}
	bLastHitMarkerWasKill = bKilled;
}

void ATracePlayerController::ClientNotifyKilledBy_Implementation(const FString& KillerName, FName Cause)
{
	LastKillerName = KillerName;
	LastDeathCause = Cause;

	UE_LOG(LogTraceGame, Verbose, TEXT("[%s] Killed by '%s' (%s)"), *GetName(), *KillerName, *Cause.ToString());
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
