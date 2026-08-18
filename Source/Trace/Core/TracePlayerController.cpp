// Trace — player controller implementation.

#include "Core/TracePlayerController.h"

#include "Abilities/TraceAbilityComponent.h"                  // spec v14 §5 — the E / V binds
#include "Abilities/Characters/TraceAbilityInputRelay.h"      // ... and Mace's reactivation routing
#include "Audio/TraceAudio.h"              // spec v26 §9 — the hitmarker sounds, client-side
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
#include "Misc/CommandLine.h"              // -TraceNoInputAssets (spec v17 §6)
#include "Misc/Parse.h"                    // FParse::Param
#include "UObject/Package.h"               // LoadObject / DuplicateObject for the input assets
#include "Gameplay/TraceCore.h"            // spec v25 §7 — the right-mouse pull probe's second rung
#include "Gameplay/TraceMelee.h"           // TraceMelee::RequestEquipIfDifferent (spec v13 §2)
#include "Gameplay/TraceParry.h"           // spec v25 §7 — Trace.Input.VerifyRightMouse's carrier-only proof
#include "Gameplay/TraceWeaponComponent.h" // RequestReload (spec v16 §1 — the R bind)
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

/**
 * SPEC v17 §6. The opt-in switch for the input assets. 1 (default) prefers /Game/Trace/Input;
 * 0 forces the C++ constructors even when the assets are present and valid.
 *
 * Read once per controller, in BuildInputData, so flipping it mid-match does nothing until the next
 * possession — deliberately: swapping the action objects out from under live delegate bindings is
 * how you get a controller whose keys are bound to actions nothing maps to.
 */
static TAutoConsoleVariable<int32> CVarTraceInputUseAssets(
	TEXT("Trace.Input.UseAssets"),
	1,
	TEXT("Spec v17 s6. 1: load IA_*/IMC_Trace from /Game/Trace/Input when they are present (default).\n")
	TEXT("0: always construct the Enhanced Input data in C++. -TraceNoInputAssets does the same from\n")
	TEXT("the command line, which is the only form guaranteed to be in effect before the first\n")
	TEXT("controller builds its input."),
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

/**
 * SPEC v17 §6 — where the input assets live and whether we are allowed to use them.
 *
 * A NAMED namespace, after this file, not an anonymous one: UBT compiles this module as a unity
 * build, and two anonymous namespaces in the same translation unit are one namespace. Naming it is
 * how this project keeps `Scripts/check-jumbo-build-collisions.py` quiet by construction rather
 * than by luck. (The anonymous block above predates the rule and is left alone; renaming it would
 * touch the look/move modifier helpers, which is not this pass's business.)
 */
namespace TracePlayerControllerInput
{
	/** Generated by Scripts/generate-input-assets.py. Spec v17 §7 puts everything under /Game/Trace. */
	const TCHAR* const AssetDirectory = TEXT("/Game/Trace/Input");

	/** The one mapping context, as a full object path. */
	FString ContextAssetPath()
	{
		return FString::Printf(TEXT("%s/IMC_Trace.IMC_Trace"), AssetDirectory);
	}

	FString ActionAssetPath(const TCHAR* AssetName)
	{
		return FString::Printf(TEXT("%s/%s.%s"), AssetDirectory, AssetName, AssetName);
	}

	/**
	 * Rule §0.1's toggle. On by default, so a project that HAS the assets uses them.
	 *
	 * Two ways to turn it off, and both exist for a reason. The cvar is what a developer flips from
	 * the console mid-session, but a cvar set through -ExecCmds arrives AFTER BeginPlay, which is
	 * exactly when the decision is made — so the command line switch is the one an automated run
	 * has to use, and it is read once and cached because the command line cannot change.
	 */
	bool AreInputAssetsEnabled()
	{
		// The command line cannot change, so it is read once. -TraceNoInputAssets WINS over the cvar:
		// it is the form an automated run uses precisely because it is already in effect before the
		// first controller exists.
		static const bool bForcedOffByCommandLine =
			FParse::Param(FCommandLine::Get(), TEXT("TraceNoInputAssets"));

		return !bForcedOffByCommandLine && (CVarTraceInputUseAssets.GetValueOnGameThread() != 0);
	}

	// =============================================================================================
	// SPEC v25 §7 + §2 — ONE BUTTON, TWO VERBS: PARRY AND THE TURNOVER CORE-PULL
	// =============================================================================================
	//
	// §7 puts PARRY on the right mouse button. §2 puts the turnover CORE-PULL on the right mouse
	// button as well ("players from the opposite team can hold right mouse while hovering over the
	// core to pull it to them"). Both notes are in the same demo, neither yields, so this file has
	// to say how they share the button.
	//
	// *** THE PRECEDENCE, STATED ONCE, HERE: THERE IS NONE, BY CONSTRUCTION. ***
	//
	// The press is delivered to BOTH verbs and each is refused by its own authority. They cannot
	// both accept, because their preconditions are exact opposites on one boolean:
	//
	//     PARRY  TraceParry::RequestParry refuses with ETraceParryRefusal::NotCarrying — parry has
	//            been carrier-only since spec v3 §3, "a parry mechanic for the core carrier".
	//     PULL   spec v25 §2 requires you to be NOT carrying, hovering the Core with line of sight,
	//            on the team that did NOT drop it, inside the 5 s turnover window. The first clause
	//            alone excludes every carrier.
	//
	// So "pulling happens when you are NOT carrying and are hovering a turned-over core; parry is
	// everything else" is not a rule this layer enforces — it FALLS OUT of the two gates, and the
	// input layer's only job is to not swallow either press on the way. A parry that eats the pull
	// and a pull that eats the parry are the same bug in two directions: an input handler deciding,
	// on a client, which of two server-authoritative verbs a press was for. This one decides
	// nothing, so it cannot decide wrong.
	//
	// NO LOCAL bIsCarrier GATE, deliberately rather than lazily. ATraceCharacter::DoPassPressed
	// already makes the argument for the pass — "a second copy of the rule here could only ever
	// disagree with the first" — and the replicated carrier flag is precisely the value that is
	// stale for a frame or two around a turnover, which is the only moment a pull is legal at all.
	//
	// ---------------------------------------------------------------------------------------------
	// WHY THIS IS A PROBE AND NOT A CALL
	// ---------------------------------------------------------------------------------------------
	// §2's gameplay half (ATraceCore / ATraceGameMode) is owned by another slice and lands in
	// parallel with this one. A hard call to an entry point that does not exist yet would make this
	// file's build depend on which slice lands first, and "the build is red because the other half
	// is not in yet" has cost this project real time before.
	//
	// So the rungs below are probed at COMPILE time, exactly as TraceBotPawnAPI in
	// AI/TraceBotController.cpp probes the pass: the same pattern, the same overload-ranking trick
	// (called with the literal 0, `int` beats `long` beats `...`), and the same hazard — a PROTECTED
	// entry point, or one under a different name, fails detection and falls through to the no-op
	// rung. Two things keep that visible instead of silent:
	//
	//   1. GetPullBinding() reports which rung compiled, and LogLiveInputMappings prints it by name
	//      at the end of its dump — which is what Trace.Input.VerifyAssets calls, so every asset
	//      verification run carries the answer whether or not anybody asked for it;
	//   2. Trace.Input.VerifyRightMouse FAILS LOUDLY when the rung is None, so a build where §2
	//      landed under a third name cannot pass its own verification.
	//
	// *** THE CONTRACT §2 HAS TO MEET *** — in preference order, either is fine, both must be PUBLIC:
	//
	//     void ATraceCharacter::DoPullPressed();                                    // preferred
	//     void ATraceCharacter::DoPullReleased();
	//
	//     void ATraceCore::RequestPullInput(bool bPressed, ATraceCharacter* Requester);
	//
	// The second is the exact shape of the existing RequestPassInput, which is why it is offered: the
	// pull is a held, cancellable hover with a server-side timer, i.e. the same shape as the pass, so
	// a §2 that mirrors the pass lands on that signature without being asked.

	/** Which entry point the right-mouse PULL actually bound to. Reported, never guessed at. */
	enum class EPullBinding : uint8
	{
		/** §2 has not landed, or landed under a third name. Right mouse is parry only. */
		None = 0,

		/** ATraceCharacter::DoPullPressed / DoPullReleased — the pawn-level verb. */
		Pawn = 1,

		/** ATraceCore::RequestPullInput(bool, ATraceCharacter*) — the Core-level verb. */
		Core = 2,
	};

	const TCHAR* LexPullBinding(EPullBinding Binding)
	{
		switch (Binding)
		{
		case EPullBinding::Pawn: return TEXT("ATraceCharacter::DoPullPressed/DoPullReleased");
		case EPullBinding::Core: return TEXT("ATraceCore::RequestPullInput");
		default:                 return TEXT("NONE - spec v25 s2 has not landed; RMB is parry only");
		}
	}

	// --- press ----------------------------------------------------------------------------------
	template <typename P> auto PullPressed(P* Pawn, int) -> decltype(Pawn->DoPullPressed(), EPullBinding())
	{
		Pawn->DoPullPressed();
		return EPullBinding::Pawn;
	}
	template <typename P> auto PullPressed(P* Pawn, long)
		-> decltype(ATraceCore::Get(Pawn->GetWorld())->RequestPullInput(true, Pawn), EPullBinding())
	{
		if (ATraceCore* TheCore = ATraceCore::Get(Pawn->GetWorld()))
		{
			TheCore->RequestPullInput(true, Pawn);
		}
		return EPullBinding::Core;
	}
	template <typename P> EPullBinding PullPressed(P*, ...)
	{
		return EPullBinding::None;
	}

	// --- release --------------------------------------------------------------------------------
	//
	// Delivered UNCONDITIONALLY at the call site, for the reason OnPassCompleted documents at length:
	// a release that is dropped leaves a held-input latch armed on the server, and here that would be
	// a pull ring that fills forever behind a pause menu. Suppressed input and death are exactly the
	// two cases where the release must still arrive.
	template <typename P> auto PullReleased(P* Pawn, int) -> decltype(Pawn->DoPullReleased(), EPullBinding())
	{
		Pawn->DoPullReleased();
		return EPullBinding::Pawn;
	}
	template <typename P> auto PullReleased(P* Pawn, long)
		-> decltype(ATraceCore::Get(Pawn->GetWorld())->RequestPullInput(false, Pawn), EPullBinding())
	{
		if (ATraceCore* TheCore = ATraceCore::Get(Pawn->GetWorld()))
		{
			TheCore->RequestPullInput(false, Pawn);
		}
		return EPullBinding::Core;
	}
	template <typename P> EPullBinding PullReleased(P*, ...)
	{
		return EPullBinding::None;
	}

	// --- which rung compiled, asked without pressing anything -------------------------------------
	//
	// A THIRD overload set rather than a trait, so the answer comes from the same overload-ranking
	// rules the two live sets use and cannot drift from them. The pawn pointer is never dereferenced:
	// every `decltype(...)` here is an UNEVALUATED operand, and the bodies only return a constant.
	template <typename P> auto ProbePullBinding(P* Pawn, int) -> decltype(Pawn->DoPullPressed(), EPullBinding())
	{
		return EPullBinding::Pawn;
	}
	template <typename P> auto ProbePullBinding(P* Pawn, long)
		-> decltype(ATraceCore::Get(Pawn->GetWorld())->RequestPullInput(true, Pawn), EPullBinding())
	{
		return EPullBinding::Core;
	}
	template <typename P> EPullBinding ProbePullBinding(P*, ...)
	{
		return EPullBinding::None;
	}

	/** Which rung this build compiled. Safe to call anywhere, at any time, on any machine. */
	EPullBinding GetPullBinding()
	{
		ATraceCharacter* const NeverDereferenced = nullptr;
		return ProbePullBinding(NeverDereferenced, 0);
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
		// SPEC v17 §6. Assets first, C++ second, and the choice is made exactly once per controller.
		// TryAdoptInputAssets assigns nothing unless it can assign everything, so a false here always
		// leaves a clean slate for the constructor path below.
		if (!TryAdoptInputAssets())
		{
			ConstructInputDataInCode();
		}
	}

	ApplyControlSettings();
}

// -------------------------------------------------------------------------------------------
// SPEC v17 §6 — the asset path.
//
// The assets are content, not configuration: they carry the identity of each action, its ValueType
// and (for Move) its accumulation behaviour, and a human can open them. What they deliberately do
// NOT carry is the player's key bindings — those live in UTraceUserSettings and are laid down by
// ApplyControlSettings over the top, exactly as they always were. That is why the migration cannot
// change how the game plays: the only thing that moved is where the ACTION OBJECTS come from.
// -------------------------------------------------------------------------------------------

bool ATracePlayerController::TryAdoptInputAssets()
{
	using namespace TracePlayerControllerInput;

	if (!AreInputAssetsEnabled())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] Input assets DISABLED (Trace.Input.UseAssets 0, or -TraceNoInputAssets). ")
			TEXT("Building the Enhanced Input data in C++ — this is the shipped fallback and the game ")
			TEXT("plays identically."),
			*GetName());
		return false;
	}

	// LOAD_NoWarn | LOAD_Quiet because "the assets have not been generated yet" is the ORDINARY
	// state of a fresh clone, not an error. The engine's own missing-package warning would read as a
	// fault; the Display line below says what actually happened and what to do about it.
	UInputMappingContext* AssetContext = LoadObject<UInputMappingContext>(
		nullptr, *ContextAssetPath(), nullptr, LOAD_NoWarn | LOAD_Quiet);

	if (AssetContext == nullptr)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] No input assets at %s — building the Enhanced Input data in C++ instead. ")
			TEXT("This is a supported configuration; run Scripts/generate-input-assets.py to author them."),
			*GetName(), *ContextAssetPath());
		return false;
	}

	// One row per action asset. A function-local struct on purpose: it exists for the length of this
	// function, it is never shared, and a file-scope one would be a name to collide with under the
	// unity build for no gain.
	struct FActionSlot
	{
		TObjectPtr<UInputAction>* Member;
		const TCHAR* AssetName;
		EInputActionValueType ValueType;
		EInputActionAccumulationBehavior Accumulation;
	};

	// THE ORDER AND THE VALUES ARE THE C++ TABLE IN ConstructInputDataInCode, COPIED. If the two ever
	// disagree the assets are rejected and the fallback runs, which is the whole safety argument:
	// this list is what "generated from the current action table" is checked against at runtime.
	const EInputActionAccumulationBehavior Highest = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
	const FActionSlot Slots[] =
	{
		{ &IA_Move,             TEXT("IA_Move"),             EInputActionValueType::Axis2D,  EInputActionAccumulationBehavior::Cumulative },
		{ &IA_Look,             TEXT("IA_Look"),             EInputActionValueType::Axis2D,  Highest },
		{ &IA_Jump,             TEXT("IA_Jump"),             EInputActionValueType::Boolean, Highest },
		{ &IA_Crouch,           TEXT("IA_Crouch"),           EInputActionValueType::Boolean, Highest },
		{ &IA_Fire,             TEXT("IA_Fire"),             EInputActionValueType::Boolean, Highest },
		{ &IA_Pass,             TEXT("IA_Pass"),             EInputActionValueType::Boolean, Highest },
		{ &IA_Dash,             TEXT("IA_Dash"),             EInputActionValueType::Boolean, Highest },
		{ &IA_Parry,            TEXT("IA_Parry"),            EInputActionValueType::Boolean, Highest },
		{ &IA_Scoreboard,       TEXT("IA_Scoreboard"),       EInputActionValueType::Boolean, Highest },
		{ &IA_EquipKnife,       TEXT("IA_EquipKnife"),       EInputActionValueType::Boolean, Highest },
		{ &IA_EquipGun,         TEXT("IA_EquipGun"),         EInputActionValueType::Boolean, Highest },
		{ &IA_Ability,          TEXT("IA_Ability"),          EInputActionValueType::Boolean, Highest },
		{ &IA_AbilitySecondary, TEXT("IA_AbilitySecondary"), EInputActionValueType::Boolean, Highest },
		{ &IA_Reload,           TEXT("IA_Reload"),           EInputActionValueType::Boolean, Highest },
		// SPEC v26 §1 — the Core pull's own action. Scripts/generate-input-assets.py writes it; a
		// checkout whose /Game/Trace/Input predates v26 is MISSING it, and that is handled the way
		// every other incomplete set is: TryAdoptInputAssets rejects the whole set and the C++
		// fallback runs, so the game plays identically until the assets are regenerated.
		{ &IA_PullCore,         TEXT("IA_PullCore"),         EInputActionValueType::Boolean, Highest },
		// SPEC v28 §10 — the melee bind. Same story as IA_PullCore above: a checkout whose
		// /Game/Trace/Input predates v28 is MISSING it, TryAdoptInputAssets rejects the whole set, and
		// the C++ fallback below builds an identical action, so the game plays the same either way.
		{ &IA_Melee,            TEXT("IA_Melee"),            EInputActionValueType::Boolean, Highest },
	};

	// Resolve everything into a scratch list FIRST. Nothing is written to a member until the whole
	// set has passed, so a rejected load leaves the controller exactly as the C++ path expects to
	// find it: all null.
	const int32 SlotCount = static_cast<int32>(UE_ARRAY_COUNT(Slots));

	TArray<UInputAction*> Resolved;
	Resolved.Reserve(SlotCount);

	int32 Rejections = 0;
	for (const FActionSlot& Slot : Slots)
	{
		UInputAction* AssetAction = LoadObject<UInputAction>(
			nullptr, *ActionAssetPath(Slot.AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet);

		if (AssetAction == nullptr)
		{
			++Rejections;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] Input asset %s is missing while %s exists — the set is incomplete."),
				*GetName(), Slot.AssetName, *ContextAssetPath());
			continue;
		}

		// A WRONG ValueType IS THE SILENT FAILURE THIS WHOLE CHECK EXISTS FOR. An Axis2D action read
		// as a Boolean does not error; it delivers zeroes, so the player simply cannot move and
		// nothing anywhere says why. Same for Move's accumulation: TakeHighestAbsoluteValue makes
		// W+S walk backwards and counter-strafing impossible, which is a bug this project has had.
		if (AssetAction->ValueType != Slot.ValueType)
		{
			++Rejections;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] Input asset %s has ValueType %d, the game needs %d. Regenerate with ")
				TEXT("Scripts/generate-input-assets.py."),
				*GetName(), Slot.AssetName,
				static_cast<int32>(AssetAction->ValueType), static_cast<int32>(Slot.ValueType));
			continue;
		}

		if (AssetAction->AccumulationBehavior != Slot.Accumulation)
		{
			++Rejections;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] Input asset %s has AccumulationBehavior %d, the game needs %d. Regenerate ")
				TEXT("with Scripts/generate-input-assets.py."),
				*GetName(), Slot.AssetName,
				static_cast<int32>(AssetAction->AccumulationBehavior), static_cast<int32>(Slot.Accumulation));
			continue;
		}

		Resolved.Add(AssetAction);
	}

	if (Rejections > 0 || Resolved.Num() != SlotCount)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[%s] %d of %d input assets were rejected — falling back to the C++ input path. ")
			TEXT("The game plays exactly as it did before the assets existed."),
			*GetName(), Rejections, SlotCount);
		return false;
	}

	// A COPY, NOT THE ASSET. See the file header: ApplyControlSettings calls UnmapAll()/MapKey() on
	// this object on every settings change, and doing that to a loaded .uasset dirties it, persists
	// past PIE, and would be shared by every local player on the machine.
	InputMapping = DuplicateObject<UInputMappingContext>(AssetContext, this, TEXT("IMC_Trace"));
	if (InputMapping == nullptr)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[%s] Could not duplicate %s — falling back to the C++ input path."),
			*GetName(), *ContextAssetPath());
		return false;
	}

	// The source asset carries RF_Public|RF_Standalone (everything saved in a package does).
	// Inherited by a per-controller duplicate, RF_Standalone would keep one dead context alive for
	// the life of the process for every controller ever spawned.
	InputMapping->ClearFlags(RF_Public | RF_Standalone);

	for (int32 Index = 0; Index < Resolved.Num(); ++Index)
	{
		*Slots[Index].Member = Resolved[Index];
	}

	bUsingInputAssets = true;

	UE_LOG(LogTraceGame, Display,
		TEXT("[%s] Enhanced Input loaded from ASSETS: %d actions + %s (%d default mapping(s) in the ")
		TEXT("asset, about to be replaced by the player's own binds from TraceUserSettings.ini)."),
		*GetName(), Resolved.Num(), *ContextAssetPath(), AssetContext->GetMappings().Num());

	return true;
}

void ATracePlayerController::ConstructInputDataInCode()
{
	{
		// Everything below is outered to `this` (or to the mapping context) AND referenced by a
		// UPROPERTY, so the whole object graph survives GC for as long as the controller does.
		InputMapping = NewObject<UInputMappingContext>(this, TEXT("IMC_Trace"));

		auto MakeAction = [this](const TCHAR* ActionName, EInputActionValueType ValueType) -> UInputAction*
		{
			UInputAction* Action = NewObject<UInputAction>(this, ActionName);
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
		// Spec v13 §2, the two direct-select binds — the whole weapon input model as of spec v15 §5,
		// which deleted IA_SwapWeapon. Boolean like every other button here.
		IA_EquipKnife = MakeAction(TEXT("IA_EquipKnife"), EInputActionValueType::Boolean);
		IA_EquipGun   = MakeAction(TEXT("IA_EquipGun"),   EInputActionValueType::Boolean);
		// SPEC v14 §5 — the ability binds.
		IA_Ability          = MakeAction(TEXT("IA_Ability"),          EInputActionValueType::Boolean);
		IA_AbilitySecondary = MakeAction(TEXT("IA_AbilitySecondary"), EInputActionValueType::Boolean);
		// SPEC v16 §1 — "R to reload". Boolean like every other button here.
		IA_Reload           = MakeAction(TEXT("IA_Reload"),           EInputActionValueType::Boolean);
		// SPEC v26 §1 — the Core pull, now its own bind. Boolean like every other button here, and a
		// SEPARATE action from IA_Parry on purpose: two actions is what produces two rebindable rows.
		IA_PullCore         = MakeAction(TEXT("IA_PullCore"),         EInputActionValueType::Boolean);
		// SPEC v28 §10 — the melee bind (right mouse by default). Boolean like every other button.
		IA_Melee            = MakeAction(TEXT("IA_Melee"),            EInputActionValueType::Boolean);

		// Cumulative accumulation is REQUIRED for opposing keys to cancel. UInputAction defaults to
		// TakeHighestAbsoluteValue, and UEnhancedPlayerInput::ProcessActionMappingEvent merges with
		// `if (Abs(Modified[C]) >= Abs(Merged[C])) Merged[C] = Modified[C];` — note the `>=`, so on a
		// tie the LAST mapping added to the context wins. With the mapping order below that made W+S
		// walk backwards and A+D strafe right, and counter-strafing impossible. The engine's own docs
		// name WASD as the motivating case for Cumulative.
		IA_Move->AccumulationBehavior = EInputActionAccumulationBehavior::Cumulative;
	}

	bUsingInputAssets = false;

	UE_LOG(LogTraceGame, Display,
		TEXT("[%s] Enhanced Input CONSTRUCTED IN C++ (16 actions + IMC_Trace). This is the fallback ")
		TEXT("path and it is fully supported — behaviour is identical to the asset path."),
		*GetName());
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

	/**
	 * Maps EVERY key the player has on @p Bind to @p Action, skipping slots they have left empty.
	 *
	 * *** SPEC v28 §3c — THIS IS WHERE THE SECOND BIND BECOMES REAL. *** A slot the settings page can
	 * edit but the mapping context never sees is a bind that draws and does nothing, so this loop is
	 * the whole of the runtime half.
	 *
	 * TWO MAPPINGS ON ONE BOOLEAN ACTION IS A SUPPORTED SHAPE, not a trick. UEnhancedPlayerInput
	 * merges an action's mappings with "take the highest absolute value", so the action is down when
	 * EITHER key is down and comes up when the last of them is released — which is what a player who
	 * has bound parry to Q and to mouse 4 means. Started still fires once, on the first key down,
	 * because the trigger is evaluated on the merged value and not per mapping.
	 *
	 * No explicit triggers: an action with no trigger uses the implicit "down" trigger, which gives us
	 * Started on press, Triggered while held and Completed on release. That is exactly the shape the
	 * handlers below expect.
	 */
	auto MapButton = [this, &UserSettings](UInputAction* Action, ETraceInputAction Bind)
	{
		if (Action == nullptr)
		{
			return;
		}

		TArray<FKey> Keys;
		UserSettings.GetKeys(Bind, Keys);
		for (const FKey& Key : Keys)
		{
			InputMapping->MapKey(Action, Key);
		}
	};

	// --- Move: four 1D keys -> (X = strafe, +right), (Y = forward, +forward) --------------------
	//
	// MapKey returns a reference *into* the context's mapping array, so it is invalidated by the
	// next MapKey call. Each binding therefore gets its own scope and uses the reference
	// immediately — never cache one.
	//
	// SPEC v28 §3c: each direction walks ITS OWN SLOT LIST, so a player who puts strafe-left on both A
	// and the left arrow gets two mappings with the same modifier stack — which is the same thing two
	// keys on a Boolean action means one block down, expressed on an axis.
	{
		TArray<FKey> Keys;

		UserSettings.GetKeys(ETraceInputAction::MoveForward, Keys);
		for (const FKey& Key : Keys)
		{
			// Forward. 1D X -> Y.
			FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, Key);
			Mapping.Modifiers.Add(MakeSwizzleXToY(InputMapping));
		}

		UserSettings.GetKeys(ETraceInputAction::MoveBack, Keys);
		for (const FKey& Key : Keys)
		{
			// Backward. 1D X -> Y, then inverted.
			FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, Key);
			Mapping.Modifiers.Add(MakeSwizzleXToY(InputMapping));
			Mapping.Modifiers.Add(MakeNegate(InputMapping));
		}

		UserSettings.GetKeys(ETraceInputAction::MoveLeft, Keys);
		for (const FKey& Key : Keys)
		{
			// Strafe left — X, inverted.
			FEnhancedActionKeyMapping& Mapping = InputMapping->MapKey(IA_Move, Key);
			Mapping.Modifiers.Add(MakeNegate(InputMapping));
		}

		UserSettings.GetKeys(ETraceInputAction::MoveRight, Keys);
		for (const FKey& Key : Keys)
		{
			// Strafe right — raw X, no modifiers at all.
			InputMapping->MapKey(IA_Move, Key);
		}
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
	MapButton(IA_Jump,       ETraceInputAction::Jump);
	MapButton(IA_Crouch,     ETraceInputAction::Crouch);
	MapButton(IA_Fire,       ETraceInputAction::Fire);
	MapButton(IA_Pass,       ETraceInputAction::Pass);
	MapButton(IA_Dash,       ETraceInputAction::Dash);
	MapButton(IA_Parry,      ETraceInputAction::Parry);
	MapButton(IA_Scoreboard, ETraceInputAction::Scoreboard);
	// Spec v13 §2. Mapped through the same KeyFor/MapButton path as everything else, so the player's
	// rebind of "1" is honoured on the next settings change without a restart — and so an action the
	// player has deliberately UNBOUND gets no mapping at all rather than a dead one.
	MapButton(IA_EquipKnife, ETraceInputAction::EquipKnife);
	MapButton(IA_EquipGun,   ETraceInputAction::EquipGun);
	// SPEC v14 §5. Through the same KeyFor/MapButton path as everything else, so a player who
	// rebinds E in the options screen is rebinding the ability and not just the label — and so
	// UTraceAbilityInputRelay's ConfigId lookup and this mapping can never disagree about the key.
	MapButton(IA_Ability,          ETraceInputAction::Ability);
	MapButton(IA_AbilitySecondary, ETraceInputAction::AbilitySecondary);
	// SPEC v16 §1. Through the same KeyFor/MapButton path as everything else, so a player who rebinds
	// R in the options screen is rebinding the reload and not just its label — and so an action they
	// deliberately UNBOUND gets no mapping at all rather than a dead one. The clip still reloads
	// itself when it empties; unbinding R costs the manual reload only.
	MapButton(IA_Reload,           ETraceInputAction::Reload);
	// SPEC v26 §1 — "Make parry and pull core two separate binds in the settings menu." Its own row in
	// the action table, its own key (default F), its own mapping. Two mappings on ONE action would
	// have been the shortcut and it is not the item: the keybind page lists ACTIONS, so a second key
	// on IA_Parry would be a bind the player could neither see nor change independently.
	//
	// NOTHING HERE FORBIDS THE TWO SHARING A KEY. UTraceUserSettings::SetKey steals a key from
	// whoever else held it, so the options screen cannot produce that state — but a hand-edited
	// TraceUserSettings.ini can, and this maps whatever the table says either way. The tiebreak for
	// that case lives in OnParryStarted; see the note there.
	MapButton(IA_PullCore,         ETraceInputAction::PullCore);
	// *** SPEC v28 §10 — "Melee should be bound to right click by default." ***
	//
	// Through the same KeyFor/MapButton path as every other button, which is the whole reason melee
	// is a table row rather than a hardcoded EKeys::RightMouseButton somewhere in this file: it is
	// on the settings page, it is rebindable, and an action the player deliberately UNBINDS gets no
	// mapping at all rather than a dead one.
	//
	// THE THREE-WAY CONTENTION ON THIS BUTTON IS RESOLVED, AND NOT HERE. Melee, the Core pull and
	// the parry all wanted right mouse across §3, §10 and v25 §7. The parry LEFT (v28 §3d: Q + the
	// thumb mouse button, with the "ParryPull" -> "ParryKeys" migration that stops a returning
	// player putting it back). The pull keeps its own bind (F) AND rides this one under §10's
	// precedence rule, which is a state test inside TraceMelee::HandleMeleeInput, not a mapping.
	// So exactly one action maps the button. Trace.Input.VerifyRightMouse asserts that.
	MapButton(IA_Melee,            ETraceInputAction::Melee);

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

	// SPEC v26 §1 — the Core pull, on its own bind at last. A HOLD, so it gets the full
	// Started/Completed/Canceled shape for the reason IA_Pass documents: spec v25 §2's rule is
	// "releasing cancels", and a release edge that never arrives leaves a pull ring filling on the
	// server behind a pause menu or a lost window focus.
	EIC->BindAction(IA_PullCore, ETriggerEvent::Started,   this, &ATracePlayerController::OnPullCoreStarted);
	EIC->BindAction(IA_PullCore, ETriggerEvent::Completed, this, &ATracePlayerController::OnPullCoreCompleted);
	EIC->BindAction(IA_PullCore, ETriggerEvent::Canceled,  this, &ATracePlayerController::OnPullCoreCompleted);

	// SPEC v28 §10 — the melee bind. Started AND Completed AND Canceled, and both edges are required:
	// the press may have gone to the Core PULL, which is a HOLD, so a dropped release leaves a ring
	// filling on the server. TraceMelee::HandleMeleeInput forwards every release to the Core whichever
	// verb the press took, so there is no state to remember here.
	EIC->BindAction(IA_Melee, ETriggerEvent::Started,   this, &ATracePlayerController::OnMeleeStarted);
	EIC->BindAction(IA_Melee, ETriggerEvent::Completed, this, &ATracePlayerController::OnMeleeCompleted);
	EIC->BindAction(IA_Melee, ETriggerEvent::Canceled,  this, &ATracePlayerController::OnMeleeCompleted);

	// Spec v13 §2, direct select — and, since spec v15 §5 deleted IA_SwapWeapon, the only weapon
	// binds there are. PRESS EDGE ONLY. Every other button in this class binds Completed and
	// Canceled too, for the symmetry argument above; these must not. There is no held state to
	// release, and a Completed binding would send a second equip request on key-up — which the
	// idempotence guard in HandleDirectEquip would swallow, but a binding that only works because
	// something downstream ignores it is a binding waiting to be a bug.
	EIC->BindAction(IA_EquipKnife, ETriggerEvent::Started, this, &ATracePlayerController::OnEquipKnifeStarted);
	EIC->BindAction(IA_EquipGun,   ETriggerEvent::Started, this, &ATracePlayerController::OnEquipGunStarted);

	// SPEC v16 §1, the reload bind. PRESS EDGE ONLY, for the same reason the two above are: there is
	// no held state to release, and a Completed binding would fire a second request on key-up that
	// only works out because RequestReload happens to refuse a reload while one is running.
	EIC->BindAction(IA_Reload, ETriggerEvent::Started, this, &ATracePlayerController::OnReloadStarted);

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

		// SPEC v18 §1c — "sometimes the movement feels like it takes a second to register inputs".
		//
		// THE SYMMETRIC HALF OF THE RELEASE ABOVE, and the measured half of §1c. Going IN, this
		// function synthesises the release edges that are about to be swallowed. Coming OUT it has to
		// synthesise the PRESS edges that were swallowed, because Enhanced Input only fires Started on
		// a transition: a key that was already down when the overlay opened is still down now, and
		// will produce no further Started event until the player physically releases it and presses it
		// again. Held AXES recover on their own (IA_Move is bound on Triggered, which re-fires every
		// frame the key is down) — buttons do not, which is exactly why the complaint is about
		// movement feeling fine and actions feeling dead.
		//
		// STILL-HELD ONLY. Nothing is buffered and nothing is replayed from history: the question
		// asked is "is this key physically down at the instant input comes back?", so a jump pressed
		// and released while browsing a menu is correctly discarded, and a finger that is on the key
		// right now gets the press it is making. Anything else would be a menu handing the player
		// actions they took twenty seconds ago.
		//
		// THE HOLD-SHAPED ACTIONS ONLY, and the omissions are deliberate. Fire, Jump, Crouch, Pass,
		// Parry, the secondary ability and the scoreboard are all "while held" — a key down means the
		// player wants the thing NOW, and that is what they would get had they pressed a frame later.
		// Dash, Reload, the two equips and the primary ability are TAPS: a resting finger on one of
		// those keys is not a request, and firing them here would spend a 35 s ability or a full clip
		// on somebody who was leaning on a key while they read a menu.
		RedeliverHeldPressEdges();
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

/**
 * SPEC v18 §1c — the A/B arm for the held-press re-delivery.
 *
 * 0 restores the behaviour exactly as it shipped before: a press edge made before a menu opened is
 * swallowed and never comes back, so a player still holding the key when the overlay closes has to
 * release it and press it again. That is the RED arm — the reproduction of the user's "sometimes the
 * movement feels like it takes a second to register inputs" — and it exists because this project's
 * standing rule is that a harness which cannot go red is not evidence.
 *
 * Not `ECVF_Cheat`: it changes no gameplay rule, only whether one class of input is dropped, and a
 * playtester who dislikes the new behaviour should be able to turn it off without a cheat-enabled
 * build.
 */
static int32 GTraceRedeliverHeldPressEdges = 1;
static FAutoConsoleVariableRef CVarTraceRedeliverHeldPressEdges(
	TEXT("Trace.Input.RedeliverHeldOnRestore"),
	GTraceRedeliverHeldPressEdges,
	TEXT("Spec v18 sec 1c. 1 (default): when a menu hands gameplay input back, the press edge of every "
	     "HOLD-shaped action whose key is still physically down is re-delivered, because Enhanced Input "
	     "only fires Started on a transition and that transition already happened behind the overlay. "
	     "0 is the RED arm: the press stays swallowed and the player must release and press again."),
	ECVF_Default);

void ATracePlayerController::RedeliverHeldPressEdges()
{
	if (GTraceRedeliverHeldPressEdges == 0)
	{
		// The RED arm. Logged rather than silent, so a run that measures nothing cannot be mistaken
		// for a run in which there was nothing to measure.
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] Input restored: held press edges NOT re-delivered "
			     "(Trace.Input.RedeliverHeldOnRestore 0 — the pre-v18 behaviour)."),
			*GetName());
		return;
	}

	// bGameInputSuppressed is already false by the time this runs — that is required, not incidental.
	// Every handler below early-returns while it is true, so calling them any earlier would be a
	// no-op that looked like a fix.
	if (bGameInputSuppressed || !IsLocalController())
	{
		return;
	}

	const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();

	// The key is asked of the SAME table ApplyControlSettings maps from, so a rebound key is honoured
	// here without this function knowing anything about defaults. An unbound action has an invalid
	// key, which IsInputKeyDown would answer nonsense for, hence the validity test.
	//
	// SPEC v28 §3c — ANY of the action's keys. A player holding parry on the thumb button while the
	// pause menu is up would otherwise come back with the press swallowed, because the primary slot
	// (Q) is not the key their finger is on. GetKeys returns only the VALID slots, so an unbound
	// action asks IsInputKeyDown nothing at all.
	auto IsHeld = [this, &UserSettings](ETraceInputAction Action)
	{
		TArray<FKey> Keys;
		UserSettings.GetKeys(Action, Keys);
		for (const FKey& Key : Keys)
		{
			if (IsInputKeyDown(Key))
			{
				return true;
			}
		}
		return false;
	};

	int32 Redelivered = 0;

	// Ordered as the player would experience them: the two that change where the pawn IS come first,
	// so a player who came out of the menu already holding forward-and-crouch is sliding on the same
	// frame they are moving, rather than one frame later.
	if (IsHeld(ETraceInputAction::Crouch))          { OnCrouchStarted();           ++Redelivered; }
	if (IsHeld(ETraceInputAction::Jump))            { OnJumpStarted();             ++Redelivered; }
	if (IsHeld(ETraceInputAction::Fire))            { OnFireStarted();             ++Redelivered; }
	if (IsHeld(ETraceInputAction::Pass))            { OnPassStarted();             ++Redelivered; }
	if (IsHeld(ETraceInputAction::Parry))           { OnParryStarted();            ++Redelivered; }
	// SPEC v26 §1. THE PULL IS HOLD-SHAPED AND THEREFORE BELONGS IN THIS LIST — it is the clearest
	// case of the whole mechanism: the player is holding a key over a turned-over Core, opens the
	// pause menu, closes it, and without this their finger is on the button while the server thinks
	// they let go. (Before v26 it rode the Parry row above; splitting the actions splits the
	// re-delivery too, or the new bind would be the one control a menu could silently eat.)
	if (IsHeld(ETraceInputAction::PullCore))        { OnPullCoreStarted();         ++Redelivered; }
	// SPEC v28 §10. THE MELEE BIND IS HOLD-SHAPED FOR EXACTLY ONE REASON and it is the one that
	// matters: its press may go to the Core PULL. A player holding right mouse over a turned-over
	// Core, opening the pause menu and closing it again is the same failure the PullCore row above
	// describes, reached through a different button. The swing half is press-edge only and simply
	// re-swings if it is off cooldown, which is what a held melee button means anyway.
	if (IsHeld(ETraceInputAction::Melee))           { OnMeleeStarted();            ++Redelivered; }
	if (IsHeld(ETraceInputAction::AbilitySecondary)){ OnAbilitySecondaryStarted(); ++Redelivered; }
	if (IsHeld(ETraceInputAction::Scoreboard))      { OnScoreboardStarted();       ++Redelivered; }

	// Printed even at zero, and that is the point: "nobody was holding anything" and "the re-delivery
	// did not run" are different facts, and a line that only appears on success cannot tell them
	// apart. One line per menu close either way.
	UE_LOG(LogTraceGame, Display,
		TEXT("[%s] Input restored: re-delivered %d held press edge(s) that the overlay swallowed "
		     "(spec v18 §1c). Held axes recover on their own; buttons do not."),
		*GetName(), Redelivered);
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

		// =========================================================================================
		// SPEC v26 §1 — THE PRECEDENCE RULE, DEMOTED TO A TIEBREAK.
		// =========================================================================================
		//
		// "Make parry and pull core two separate binds in the settings menu [...] keep the precedence
		// rule only as a tiebreak if a player binds them to the same key on purpose."
		//
		// v25 §7 + §2 put both verbs on right mouse and dispatched BOTH from this handler, every
		// press, on the argument that their gates are exact opposites on "am I carrying the Core" and
		// so at most one could ever accept. That argument has not stopped being true — it is just no
		// longer needed, because the pull has its own action and its own handler
		// (OnPullCoreStarted) and its own default key.
		//
		// SO THIS LINE IS NOW CONDITIONAL, AND THE CONDITION IS THE ONLY THING v26 CHANGES HERE. When
		// the two actions resolve to DIFFERENT keys — the shipped state — the parry handler does the
		// parry and nothing else. When a player has deliberately put both on one key, the old
		// behaviour is restored exactly: one press, both verbs, the gates decide.
		//
		// WHY THE CONDITION IS NEEDED AT ALL, given that Enhanced Input maps the two actions
		// independently and would fire both handlers off one shared key anyway. Because relying on
		// that would be relying on UInputAction::bConsumeInput's cross-context consumption semantics
		// to stay the way they are, for a case that only exists because a player hand-edited an .ini.
		// The explicit test is three lines, is a local fact this file owns, and cannot be changed by
		// an engine upgrade. DispatchCorePull de-duplicates by frame, so a build where BOTH the
		// shared-key handlers fire still sends exactly one press.
		if (DoParryAndPullShareAKey())
		{
			DispatchCorePull(/*bPressed=*/true);
		}
	}
}

void ATracePlayerController::OnParryCompleted()
{
	// Intentionally not gated on bGameInputSuppressed, and GetPawn rather than GetLivingCharacter, for
	// the reason OnPassCompleted spells out: a release that is dropped leaves the server-side hold
	// latched. The PARRY half has no held state to release — the window is a fixed duration owned by
	// the trail component — but the shared-key case below is a hold and must still be cancelled.
	if (ATraceCharacter* TraceChar = GetPawn<ATraceCharacter>())
	{
		TraceChar->DoParryReleased();

		// SPEC v26 §1. The tiebreak's release half. Same condition as the press, and it MUST be the
		// same condition: a release delivered on a key the press was never delivered on is harmless,
		// but a press delivered without its release is a latched hold, so the asymmetry that matters
		// is the one this avoids.
		if (DoParryAndPullShareAKey())
		{
			DispatchCorePull(/*bPressed=*/false);
		}
	}
}

// =================================================================================================
// SPEC v26 §1 — THE CORE PULL'S OWN BIND
// =================================================================================================

bool ATracePlayerController::DoParryAndPullShareAKey() const
{
	const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();

	// SPEC v28 §3b/§3c — ANY key of the parry against ANY key of the pull. With two slots each, "do
	// they share a key" is an intersection and no longer a comparison, and it is now a state a player
	// can reach from the options page rather than only by hand-editing the .ini: the two actions'
	// exclusion groups are disjoint (Carrying vs NotCarrying), so UTraceUserSettings::SetKey lets
	// them keep one key between them on purpose. That makes this tiebreak load-bearing again.
	//
	// ActionUsesKey ignores invalid keys, so two unbound actions cannot compare equal here — which
	// they would under a plain FKey == FKey, silently turning the parry handler into a pull
	// dispatcher for a player who had unbound both.
	TArray<FKey> ParryKeys;
	UserSettings.GetKeys(ETraceInputAction::Parry, ParryKeys);
	for (const FKey& Key : ParryKeys)
	{
		if (UserSettings.ActionUsesKey(ETraceInputAction::PullCore, Key))
		{
			return true;
		}
	}
	return false;
}

void ATracePlayerController::DispatchCorePull(bool bPressed)
{
	// ONE DISPATCH PER FRAME PER EDGE. Two call sites can reach this on the same frame — IA_PullCore's
	// own handler, and OnParryStarted's shared-key tiebreak — and on a client each dispatch is a
	// separate ServerSetPullInput RPC. The server's latch is a set-membership test and would absorb
	// the duplicate, but "it works because something downstream ignores it" is exactly the shape of
	// binding this file refuses elsewhere (see the equip binds' press-edge-only note).
	//
	// GFrameCounter and not a world time: this is a de-duplication of two synchronous calls inside one
	// frame's input processing, so the frame IS the right unit, and it needs no world.
	uint64& Stamp = bPressed ? LastPullPressFrame : LastPullReleaseFrame;
	if (Stamp == GFrameCounter && GFrameCounter != 0)
	{
		return;
	}
	Stamp = GFrameCounter;

	// The PRESS is a request and is refused while gameplay input is suppressed. The RELEASE is a
	// cancel and is delivered unconditionally, through GetPawn rather than GetLivingCharacter — the
	// asymmetry OnPassCompleted argues for at length, and it bites hardest here: opening the pause
	// menu mid-pull suppresses input, dying mid-pull makes the pawn non-living, and both are cases
	// where a dropped release leaves a ring filling on the server with nobody holding the button.
	if (bPressed)
	{
		if (bGameInputSuppressed)
		{
			return;
		}

		if (ATraceCharacter* TraceChar = GetLivingCharacter())
		{
			++DebugPullPressCount;
			if (InputLogLevel() >= 1)
			{
				UE_LOG(LogTraceGame, Display, TEXT("INPUT Pull Core pressed #%d"), DebugPullPressCount);
			}
			TracePlayerControllerInput::PullPressed(TraceChar, 0);
		}
		return;
	}

	if (ATraceCharacter* TraceChar = GetPawn<ATraceCharacter>())
	{
		TracePlayerControllerInput::PullReleased(TraceChar, 0);
	}
}

void ATracePlayerController::OnPullCoreStarted()
{
	DispatchCorePull(/*bPressed=*/true);
}

void ATracePlayerController::OnPullCoreCompleted()
{
	DispatchCorePull(/*bPressed=*/false);
}

// -------------------------------------------------------------------------------------------
// SPEC v28 §10 — THE MELEE BIND (right mouse by default)
//
// The two handlers below are deliberately three lines each. Everything interesting is in
// TraceMelee::HandleMeleeInput: the precedence against the Core pull, the pull dispatch (including
// the client relay), the swing's refusals and the debug logging. Duplicating any of it here would
// give the input slice a second opinion about a server-authoritative verb, which is the mistake
// OnParryStarted's comment spends a paragraph refusing to make.
// -------------------------------------------------------------------------------------------

void ATracePlayerController::OnMeleeStarted()
{
	// The PRESS is a request, and is refused while a menu owns input — same gate as every other
	// press in this file.
	if (bGameInputSuppressed)
	{
		return;
	}

	if (ATraceCharacter* TraceChar = GetLivingCharacter())
	{
		const TraceMelee::EMeleeInputResult Result = TraceMelee::HandleMeleeInput(TraceChar, /*bPressed=*/true);

		if (InputLogLevel() >= 1)
		{
			const TCHAR* What =
				(Result == TraceMelee::EMeleeInputResult::Swing)    ? TEXT("SWING") :
				(Result == TraceMelee::EMeleeInputResult::CorePull) ? TEXT("CORE PULL (the circle is on screen)") :
				                                                      TEXT("refused");
			UE_LOG(LogTraceGame, Display, TEXT("INPUT Melee pressed -> %s"), What);
		}
	}
}

void ATracePlayerController::OnMeleeCompleted()
{
	// The RELEASE is a cancel and is delivered unconditionally, through GetPawn rather than
	// GetLivingCharacter. Opening the pause menu mid-pull suppresses input; dying mid-pull makes the
	// pawn non-living. Those are the two cases where a dropped release leaves a pull ring filling on
	// the server with nobody holding the button — the exact asymmetry OnPassCompleted and
	// DispatchCorePull both argue for.
	if (ATraceCharacter* TraceChar = GetPawn<ATraceCharacter>())
	{
		TraceMelee::HandleMeleeInput(TraceChar, /*bPressed=*/false);
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

void ATracePlayerController::OnReloadStarted()
{
	// FIRST LINE, before any gate, for the reason DebugEquipPressCount's declaration gives: this
	// counts "the R key reached a bound delegate", which Trace.Ammo.BindTest needs to be able to ask
	// separately from "a reload started". A probe that could only see the outcome would report a
	// dead binding and a correctly-refused reload identically.
	++DebugReloadPressCount;

	if (bGameInputSuppressed)
	{
		return;
	}

	// SPEC v16 §1, "R to reload". This handler makes NO decision of its own — see the header. It does
	// not even ask whether the clip is full, because UTraceWeaponComponent::RequestReload has to
	// answer that for the automatic reload and for the bots anyway, and two copies of "is a reload
	// legal" is how a predicted state and an authoritative one come to disagree.
	//
	// GetLivingCharacter() rather than GetPawn(): a dead player's R press is not a reload request, and
	// mouse1 is already their "put me back in" button.
	if (ATraceCharacter* TraceChar = GetLivingCharacter())
	{
		if (UTraceWeaponComponent* Weapon = TraceChar->Weapon)
		{
			const bool bStarted = Weapon->RequestReload();
			if (InputLogLevel() >= 1)
			{
				UE_LOG(LogTraceGame, Display, TEXT("INPUT Reload pressed: started=%d (clip %d/%d)"),
					bStarted ? 1 : 0, Weapon->GetClipAmmo(), Weapon->GetClipSize());
			}
		}
	}
}

#if !UE_BUILD_SHIPPING
/**
 * Set only by `Trace.V13.Hotkeys toggle`, the red arm at the bottom of this file. While it is true,
 * the direct-select handler below routes to the UNGUARDED equip so the harness can watch its own
 * assertions fail. Cleared again when the probe finishes.
 *
 * File-scope and distinctively named rather than an anonymous-namespace static: UBT compiles this
 * module as a unity/jumbo build, where two files' anonymous namespaces become one.
 */
static bool GTraceForceUnguardedEquip = false;
#endif

void ATracePlayerController::HandleDirectEquip(bool bWantKnife, const TCHAR* ActionLabel)
{
	// FIRST LINE, before any gate: this counts "the key reached a bound delegate", which is a
	// different question from "the weapon changed" and is the one the v13 §2 harness must be able
	// to ask. See the counter's declaration for the vacuous test it exists to prevent.
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
	// RequestEquip (the UNGUARDED entry point) is deliberately UNCHANGED and still costs a pullout on
	// a redundant request. Nothing binds a key to it any more — spec v15 §5 deleted the toggle — but
	// it is still the console's swap verb and, more usefully, it is what Trace.V13.Hotkeys' RED ARM
	// substitutes below to prove this test can fail. The two verbs want opposite things from a repeat
	// press and the component offers both, which is why this is a second function rather than an edit
	// to the first. Read UTraceWeaponComponent::RequestEquipIfDifferent for the full argument,
	// including why it is correct mid-pullout.
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
#if !UE_BUILD_SHIPPING
	// THE RED ARM, and it is one line on purpose. `Trace.V13.Hotkeys toggle` sets this and the
	// handler then calls the UNGUARDED equip — same key, same binding, same handler, same press
	// counter, one gate different. Before spec v15 §5 the red arm reached the unguarded path by
	// pressing the SwapWeapon key instead, which was a weaker A/B: it changed the action, the
	// binding, the handler and the counter all at once, so a failure could not be attributed to the
	// guard. Deleting the toggle bind forced the better version.
	const bool bEquipped = GTraceForceUnguardedEquip
		? TraceMelee::RequestEquip(TraceChar, Desired, &Refusal)
		: TraceMelee::RequestEquipIfDifferent(TraceChar, Desired, &Refusal);
#else
	const bool bEquipped = TraceMelee::RequestEquipIfDifferent(TraceChar, Desired, &Refusal);
#endif

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
	LogAction(TEXT("IA_EquipKnife"), IA_EquipKnife);
	LogAction(TEXT("IA_EquipGun  "), IA_EquipGun);
	LogAction(TEXT("IA_Reload    "), IA_Reload);
	LogAction(TEXT("IA_PullCore  "), IA_PullCore);
	LogAction(TEXT("IA_Melee     "), IA_Melee);

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
		// SPEC v28 §3c — the WHOLE binding, both slots. A diagnostic that printed only the primary
		// would show a parry on "Q" while the player was pressing the thumb button and getting a parry,
		// which is precisely the kind of half-truth this dump exists to prevent.
		UE_LOG(LogTraceGame, Display, TEXT("INPUTDIAG [%s]   bind %-16s -> %-30s [%s]"),
			Context, Info.ConfigId, *UserSettings.DescribeBinding(Info.Action),
			*LexTraceInputStates(Info.States));
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

void ATracePlayerController::LogLiveInputMappings(const TCHAR* Context) const
{
	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTMAP [%s] controller=%s source=%s mappings=%d"),
		Context, *GetName(),
		bUsingInputAssets ? TEXT("ASSETS (/Game/Trace/Input)") : TEXT("C++ (NewObject fallback)"),
		(InputMapping != nullptr) ? InputMapping->GetMappings().Num() : -1);

	if (InputMapping == nullptr)
	{
		return;
	}

	int32 Index = 0;
	for (const FEnhancedActionKeyMapping& Mapping : InputMapping->GetMappings())
	{
		// The modifier CLASSES, not their values: the swizzle/negate shape is what makes W "forward"
		// rather than "strafe", and it is the part a faithful migration has to reproduce exactly.
		FString Modifiers;
		for (const UInputModifier* Modifier : Mapping.Modifiers)
		{
			if (Modifier == nullptr)
			{
				continue;
			}
			if (!Modifiers.IsEmpty())
			{
				Modifiers += TEXT("+");
			}
			Modifiers += Modifier->GetClass()->GetName();
		}

		UE_LOG(LogTraceGame, Display, TEXT("INPUTMAP [%s]   %02d %-22s <- %-18s %s"),
			Context, Index,
			(Mapping.Action != nullptr) ? *Mapping.Action->GetName() : TEXT("<null action>"),
			*Mapping.Key.GetFName().ToString(),
			Modifiers.IsEmpty() ? TEXT("-") : *Modifiers);
		++Index;
	}

	// SPEC v25 §7 + §2. The right mouse button carries TWO verbs and only one of them appears in the
	// list above: the pull is dispatched from the parry handler and has no mapping of its own. So the
	// dump would otherwise be a complete and completely misleading picture of that button. Printed
	// every time this dump runs, including when the answer is NONE, because "the pull is not wired
	// yet" and "the pull is wired and quiet" are different facts and a line that only appeared on
	// success could not tell them apart.
	const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();
	// SPEC v28 §3d: this line used to be about the right mouse button, and the right mouse button is
	// no longer where any of these verbs live — parry moved to Q + the thumb button so §10's melee
	// could have it. What the line is FOR is unchanged: naming, in one place, which physical buttons
	// the three overlapping Core verbs are on, because that is the question every report about them
	// turns out to be.
	UE_LOG(LogTraceGame, Display,
		TEXT("INPUTMAP [%s]   Core verbs: PARRY on %s (bind '%s'), PULL CORE on %s -> %s, THROW on %s ")
		TEXT("(mouse 1 throws while carrying regardless)."),
		Context,
		*UserSettings.DescribeBinding(ETraceInputAction::Parry),
		TraceInputActions::Info(ETraceInputAction::Parry).ConfigId,
		*UserSettings.DescribeBinding(ETraceInputAction::PullCore),
		TracePlayerControllerInput::LexPullBinding(TracePlayerControllerInput::GetPullBinding()),
		*UserSettings.DescribeBinding(ETraceInputAction::Pass));
}

void ATracePlayerController::GetLiveMappedActions(TArray<const UInputAction*>& OutActions) const
{
	OutActions.Reset();

	if (InputMapping == nullptr)
	{
		return;
	}

	for (const FEnhancedActionKeyMapping& Mapping : InputMapping->GetMappings())
	{
		OutActions.Add(Mapping.Action);
	}
}

// =================================================================================================
// Trace.Input.VerifyAssets — SPEC v17 §6, the evidence half.
//
// §6's claim is that the generated IA_*/IMC_Trace assets were produced FROM the current action
// table, so the defaults are unchanged. That is a claim about two things agreeing, and the only
// honest way to check it is to read both and compare:
//
//   1. Are the assets there at all? "No" is a PASS with a different verdict — the C++ fallback is a
//      supported configuration, not a failure, and a command that shouted about a fresh clone would
//      teach people to ignore it.
//   2. Does every IA_* asset carry the ValueType and AccumulationBehavior the C++ path would have
//      given it? This is the check that catches the silent killer: an Axis2D action saved as a
//      Boolean delivers zeroes forever and nothing else in the engine says a word.
//   3. Does IMC_Trace's mapping list still match the SHIPPED DEFAULTS in TraceInputActions::All()?
//      The keys come out of that table at runtime, so a mismatch here does not break the game — it
//      means the asset is a stale PICTURE of the binds, which is its own kind of lie and is exactly
//      what happens when somebody changes a default and forgets to regenerate.
//   4. If a live local controller exists: which path did it actually take, and is the action object
//      bound to its handlers the ASSET or a NewObject'd copy? Everything above can pass on assets
//      the running game is ignoring.
// =================================================================================================

namespace TracePlayerControllerInput
{
	ATracePlayerController* FindLocalTraceController()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			UWorld* World = WorldContext.World();
			if (World == nullptr || (WorldContext.WorldType != EWorldType::Game && WorldContext.WorldType != EWorldType::PIE))
			{
				continue;
			}

			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				if (ATracePlayerController* Candidate = Cast<ATracePlayerController>(It->Get()))
				{
					if (Candidate->IsLocalController())
					{
						return Candidate;
					}
				}
			}
		}
		return nullptr;
	}

	void VerifyInputAssets()
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[InputAssets] ===== spec v17 s6: are the input assets present, valid and faithful? ====="));

		UE_LOG(LogTraceGame, Display, TEXT("[InputAssets]   toggle: Trace.Input.UseAssets=%d -TraceNoInputAssets=%d -> assets %s"),
			CVarTraceInputUseAssets.GetValueOnGameThread(),
			FParse::Param(FCommandLine::Get(), TEXT("TraceNoInputAssets")) ? 1 : 0,
			AreInputAssetsEnabled() ? TEXT("ENABLED") : TEXT("DISABLED"));

		UInputMappingContext* AssetContext = LoadObject<UInputMappingContext>(
			nullptr, *ContextAssetPath(), nullptr, LOAD_NoWarn | LOAD_Quiet);

		if (AssetContext == nullptr)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[InputAssets] VERDICT: NO ASSETS at %s. The game is on the C++ fallback, which is a ")
				TEXT("supported configuration and plays identically. Run Scripts/generate-input-assets.py ")
				TEXT("to author them."),
				*ContextAssetPath());
			return;
		}

		// ---- 2. every action asset, against the C++ table ----------------------------------------
		struct FExpectedAction
		{
			const TCHAR* AssetName;
			EInputActionValueType ValueType;
			EInputActionAccumulationBehavior Accumulation;
		};

		const EInputActionAccumulationBehavior Highest = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
		const FExpectedAction ExpectedActions[] =
		{
			{ TEXT("IA_Move"),             EInputActionValueType::Axis2D,  EInputActionAccumulationBehavior::Cumulative },
			{ TEXT("IA_Look"),             EInputActionValueType::Axis2D,  Highest },
			{ TEXT("IA_Jump"),             EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Crouch"),           EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Fire"),             EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Pass"),             EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Dash"),             EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Parry"),            EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Scoreboard"),       EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_EquipKnife"),       EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_EquipGun"),         EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Ability"),          EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_AbilitySecondary"), EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Reload"),           EInputActionValueType::Boolean, Highest },
			// SPEC v26 §1 — the Core pull's own action. A checkout whose /Game/Trace/Input predates
			// v26 fails this row, which is the whole point: it says "re-run
			// Scripts/generate-input-assets.py" rather than letting the new bind be silently absent
			// from the assets while the C++ fallback quietly carries it.
			{ TEXT("IA_PullCore"),         EInputActionValueType::Boolean, Highest },
			{ TEXT("IA_Melee"),            EInputActionValueType::Boolean, Highest },
		};

		int32 Failures = 0;
		int32 ActionsOk = 0;
		for (const FExpectedAction& Expected : ExpectedActions)
		{
			const UInputAction* AssetAction = LoadObject<UInputAction>(
				nullptr, *ActionAssetPath(Expected.AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet);

			if (AssetAction == nullptr)
			{
				++Failures;
				UE_LOG(LogTraceGame, Error, TEXT("[InputAssets]   MISSING  %s"), Expected.AssetName);
				continue;
			}

			const bool bTypeOk = (AssetAction->ValueType == Expected.ValueType);
			const bool bAccumOk = (AssetAction->AccumulationBehavior == Expected.Accumulation);
			if (bTypeOk && bAccumOk)
			{
				++ActionsOk;
				UE_LOG(LogTraceGame, Display, TEXT("[InputAssets]   ok       %-22s valueType=%d accumulation=%d"),
					Expected.AssetName, static_cast<int32>(AssetAction->ValueType),
					static_cast<int32>(AssetAction->AccumulationBehavior));
			}
			else
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[InputAssets]   WRONG    %-22s valueType=%d (want %d) accumulation=%d (want %d) — ")
					TEXT("the controller will REJECT this set and fall back to C++."),
					Expected.AssetName,
					static_cast<int32>(AssetAction->ValueType), static_cast<int32>(Expected.ValueType),
					static_cast<int32>(AssetAction->AccumulationBehavior), static_cast<int32>(Expected.Accumulation));
			}
		}

		// ---- 3. the mapping list, against the shipped defaults ------------------------------------
		//
		// Sentinel note: ETraceInputAction::Count in the Bind column means "not a rebindable action",
		// which is true of exactly the two mouse-look axes — they are not on the rebind list because a
		// mouse axis has no key to change, only a sensitivity.
		struct FExpectedMapping
		{
			const TCHAR* ActionName;
			ETraceInputAction Bind;
			FKey FixedKey;
			int32 ModifierCount;
			/**
			 * SPEC v28 §3c — which of the action's shipped slots this mapping is. 0 for every row that
			 * ships with one key; §3d's parry is the only action with a row for slot 1.
			 */
			int32 Slot;
		};

		const FExpectedMapping ExpectedMappings[] =
		{
			{ TEXT("IA_Move"),             ETraceInputAction::MoveForward,      FKey(),         1, 0 },
			{ TEXT("IA_Move"),             ETraceInputAction::MoveBack,         FKey(),         2, 0 },
			{ TEXT("IA_Move"),             ETraceInputAction::MoveLeft,         FKey(),         1, 0 },
			{ TEXT("IA_Move"),             ETraceInputAction::MoveRight,        FKey(),         0, 0 },
			{ TEXT("IA_Look"),             ETraceInputAction::Count,            EKeys::MouseX,  1, 0 },
			{ TEXT("IA_Look"),             ETraceInputAction::Count,            EKeys::MouseY,  2, 0 },
			{ TEXT("IA_Jump"),             ETraceInputAction::Jump,             FKey(),         0, 0 },
			{ TEXT("IA_Crouch"),           ETraceInputAction::Crouch,           FKey(),         0, 0 },
			{ TEXT("IA_Fire"),             ETraceInputAction::Fire,             FKey(),         0, 0 },
			// *** SPEC v25 §7: THERE IS NO IA_Pass ROW ANY MORE. ***  It used to sit here, on the
			// right mouse button. The throw now ships UNBOUND (TraceInputActions::All()), and an
			// unbound action produces no mapping at all — ApplyControlSettings' MapButton skips an
			// invalid key, and generate-input-assets.py cannot write a mapping to a key that does not
			// exist. So the asset must not contain one either, and this table is what says so.
			// IA_Pass itself is untouched and still in ExpectedActions above: the ACTION exists and is
			// still bound to its handlers, it simply starts with no key on it.
			{ TEXT("IA_Dash"),             ETraceInputAction::Dash,             FKey(),         0, 0 },
			// SPEC v25 §7 put this row on the RIGHT MOUSE BUTTON; SPEC v28 §3d moves it to Q and gives
			// it a SECOND mapping on the thumb mouse button. Nothing here says either key, and that is
			// the design — the expected key is read live from TraceInputActions::Info(Parry) below, so
			// moving a default is one edit in the action table and never two. What DOES have to be
			// stated is that there are now two rows for this action, because the count is the thing
			// that catches an asset regenerated by an older script.
			{ TEXT("IA_Parry"),            ETraceInputAction::Parry,            FKey(),         0, 0 },
			{ TEXT("IA_Parry"),            ETraceInputAction::Parry,            FKey(),         0, 1 },
			{ TEXT("IA_Scoreboard"),       ETraceInputAction::Scoreboard,       FKey(),         0, 0 },
			{ TEXT("IA_EquipKnife"),       ETraceInputAction::EquipKnife,       FKey(),         0, 0 },
			{ TEXT("IA_EquipGun"),         ETraceInputAction::EquipGun,         FKey(),         0, 0 },
			{ TEXT("IA_Ability"),          ETraceInputAction::Ability,          FKey(),         0, 0 },
			{ TEXT("IA_AbilitySecondary"), ETraceInputAction::AbilitySecondary, FKey(),         0, 0 },
			{ TEXT("IA_Reload"),           ETraceInputAction::Reload,           FKey(),         0, 0 },
			// SPEC v26 §1 — the Core pull's own mapping. Its expected key is read live from
			// TraceInputActions::Info(PullCore).DefaultKey() like every row above, so changing the
			// default is one edit in the action table and never two.
			{ TEXT("IA_PullCore"),         ETraceInputAction::PullCore,         FKey(),         0, 0 },
			{ TEXT("IA_Melee"),            ETraceInputAction::Melee,            FKey(),         0, 0 },
		};

		const int32 ExpectedMappingCount = static_cast<int32>(UE_ARRAY_COUNT(ExpectedMappings));
		const TArray<FEnhancedActionKeyMapping>& AssetMappings = AssetContext->GetMappings();
		if (AssetMappings.Num() != ExpectedMappingCount)
		{
			++Failures;
			UE_LOG(LogTraceGame, Error,
				TEXT("[InputAssets]   IMC_Trace has %d mapping(s), the action table wants %d. Regenerate ")
				TEXT("with Scripts/generate-input-assets.py."),
				AssetMappings.Num(), ExpectedMappingCount);
		}
		else
		{
			for (int32 Index = 0; Index < AssetMappings.Num(); ++Index)
			{
				const FEnhancedActionKeyMapping& Actual = AssetMappings[Index];
				const FExpectedMapping& Expected = ExpectedMappings[Index];

				// The DEFAULT key from the action table — deliberately not the player's current bind.
				// The asset is the shipped default; a player who rebinds Dash has not made the asset
				// wrong, and a check that used their live bind would fail for every rebinding player.
				// SPEC v28 §3c — the shipped key for THIS SLOT. DefaultKeyAlt is null-checked because
				// sixteen of the seventeen rows ship with one key and say so with &Default_None.
				FKey WantKey = Expected.FixedKey;
				if (Expected.Bind != ETraceInputAction::Count)
				{
					const FTraceInputActionInfo& BindInfo = TraceInputActions::Info(Expected.Bind);
					WantKey = (Expected.Slot == 0)
						? BindInfo.DefaultKey()
						: ((BindInfo.DefaultKeyAlt != nullptr) ? BindInfo.DefaultKeyAlt() : FKey());
				}

				const bool bActionOk = (Actual.Action != nullptr) && Actual.Action->GetName().Equals(Expected.ActionName);
				const bool bKeyOk = (Actual.Key == WantKey);
				const bool bModifiersOk = (Actual.Modifiers.Num() == Expected.ModifierCount);

				if (bActionOk && bKeyOk && bModifiersOk)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[InputAssets]   ok       %02d %-22s <- %-18s %d modifier(s)"),
						Index, Expected.ActionName, *WantKey.GetFName().ToString(), Expected.ModifierCount);
				}
				else
				{
					++Failures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[InputAssets]   WRONG    %02d asset says %s <- %s (%d modifier(s)); the action table ")
						TEXT("says %s <- %s (%d). The ASSET IS A STALE PICTURE — the game still plays the table."),
						Index,
						(Actual.Action != nullptr) ? *Actual.Action->GetName() : TEXT("<null>"),
						*Actual.Key.GetFName().ToString(), Actual.Modifiers.Num(),
						Expected.ActionName, *WantKey.GetFName().ToString(), Expected.ModifierCount);
				}
			}
		}

		// ---- 4. what the RUNNING game actually did ------------------------------------------------
		//
		// THE VACUITY GUARD. Everything above reads assets off disk and would pass identically on a
		// build whose controller ignores them entirely — which is precisely the failure mode this
		// migration can have. So ask the live controller which path it took, and prove the object its
		// delegates are bound to IS the asset (pointer identity, not a name match: two objects called
		// "IA_Move" is exactly what the fallback produces).
		if (const ATracePlayerController* LiveController = FindLocalTraceController())
		{
			const UInputAction* AssetMove = LoadObject<UInputAction>(
				nullptr, *ActionAssetPath(TEXT("IA_Move")), nullptr, LOAD_NoWarn | LOAD_Quiet);

			const bool bLiveOnAssets = LiveController->IsUsingInputAssets();

			TArray<const UInputAction*> LiveActions;
			LiveController->GetLiveMappedActions(LiveActions);
			const bool bBoundToAsset = (AssetMove != nullptr) && LiveActions.Contains(AssetMove);

			UE_LOG(LogTraceGame, Display,
				TEXT("[InputAssets]   live controller '%s': usingAssets=%d, its IA_Move IS the asset object=%d"),
				*LiveController->GetName(), bLiveOnAssets ? 1 : 0, bBoundToAsset ? 1 : 0);

			if (AreInputAssetsEnabled() && Failures == 0 && !(bLiveOnAssets && bBoundToAsset))
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[InputAssets]   The assets are present and valid, the toggle is on, and the live ")
					TEXT("controller is STILL not using them. The load path is broken."));
			}

			LiveController->LogLiveInputMappings(TEXT("VerifyAssets"));
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[InputAssets]   no local ATracePlayerController — this run checked the FILES only. ")
				TEXT("Run it again in a live match to prove the game is using them."));
		}

#define TRACE_INPUTASSETS_VERDICT_ARGS \
	(Failures == 0) ? TEXT("THE ASSETS MATCH THE C++ TABLE") : TEXT("THE ASSETS AND THE C++ TABLE DISAGREE"), \
	ActionsOk, static_cast<int32>(UE_ARRAY_COUNT(ExpectedActions)), AssetMappings.Num(), Failures

#define TRACE_INPUTASSETS_VERDICT_TEXT \
	TEXT("[InputAssets] VERDICT: %s. %d/%d action asset(s) valid, %d mapping(s) in IMC_Trace, %d failure(s).")

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_INPUTASSETS_VERDICT_TEXT, TRACE_INPUTASSETS_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_INPUTASSETS_VERDICT_TEXT, TRACE_INPUTASSETS_VERDICT_ARGS);
		}

#undef TRACE_INPUTASSETS_VERDICT_ARGS
#undef TRACE_INPUTASSETS_VERDICT_TEXT
	}

	// =============================================================================================
	// Trace.Input.VerifyRightMouse — SPEC v25 §7, the evidence half.
	//
	// §7's two claims are "right mouse is parry" and "right mouse no longer throws the Core", and
	// §7's warning is that the button now also carries §2's pull, so "a parry that eats the pull, or
	// a pull that eats the parry, is a real bug". Three separate things to be shown, and a passing
	// build has to show all three:
	//
	//   1. WHERE THE BUTTON POINTS. The resolved bind for PARRY is the right mouse button, the
	//      resolved bind for the THROW is not (it is UNBOUND by default), and no OTHER action holds
	//      the button either. Read out of the live UTraceUserSettings — the same table
	//      ApplyControlSettings maps from — not out of the defaults table, so a stale
	//      TraceUserSettings.ini that still says `Pass=RightMouseButton` FAILS here instead of being
	//      discovered in a playtest. That is the failure mode this whole item is exposed to.
	//
	//   2. THAT THE PULL IS WIRED AT ALL. §2 lands in parallel and the dispatch is a compile-time
	//      probe, so the one thing that must never pass quietly is "the probe found nothing".
	//
	//   3. THAT THE TWO VERBS CANNOT BOTH FIRE. Stated as an argument in the code, but ARGUED FROM A
	//      MEASUREMENT here: with a live local pawn that is NOT carrying, TraceParry::RequestParry is
	//      asked for real and must refuse with NotCarrying. That call is side-effect free precisely
	//      BECAUSE it refuses — no window opens, no RPC is sent — which is why it is safe to make
	//      from a console command mid-match, and why the carrying case is reported rather than
	//      exercised (asking it there WOULD open a real parry window and that is not a diagnostic's
	//      business).
	//
	// What this command does NOT cover: a human physically holding the right button over a
	// turned-over Core. That is delivery plus §2's gameplay, and it belongs to a match, not a
	// console.
	// =============================================================================================
	void VerifyRightMouse()
	{
		UTraceUserSettings& Settings = UTraceUserSettings::Get();
		const FKey RMB = EKeys::RightMouseButton;

		UE_LOG(LogTraceGame, Display,
			TEXT("[RightMouse] ===== spec v25 s7 + v26 s1: right mouse is the PARRY, never the throw, and the ")
			TEXT("CORE-PULL is now its own separate bind ====="));

		int32 Failures = 0;

		// ---- 1. where the button points ---------------------------------------------------------
		//
		// *** SPEC v28 §3d REVERSES HALF OF v25 §7 AND THIS BLOCK REVERSES WITH IT. ***
		//
		// v25 §7 asked for "parry on right click" and this command proved it. v28 §3d asks for parry on
		// "BOTH Q and the thumb mouse button" — and it asks for that BECAUSE v28 §10 is putting MELEE on
		// right click, which parry cannot share (both are live while carrying nothing in the melee's case
		// and while carrying in the parry's, and melee is not even in the rebind table, so there is no
		// exclusion group to appeal to). Leaving the old assertion here would have failed the build for
		// doing exactly what the new note says.
		//
		// WHAT SURVIVES UNCHANGED is the half that was never about which button: the THROW must not be on
		// right mouse, and no rebindable action may quietly be sitting on it.
		const bool bParryOnQ = Settings.ActionUsesKey(ETraceInputAction::Parry, EKeys::Q);
		const bool bParryOnThumb = Settings.ActionUsesKey(ETraceInputAction::Parry, EKeys::ThumbMouseButton);
		const bool bThrowOffRMB = !Settings.ActionUsesKey(ETraceInputAction::Pass, RMB);

		Failures += bThrowOffRMB ? 0 : 1;

		// Display and Error are separate calls, not a ternary verbosity: UE_LOG's verbosity is a token
		// the macro pastes into a compile-time category check, not a value.
		//
		// The parry's own keys are REPORTED and not failed on, because a player is allowed to rebind
		// their parry and this command has to stay useful on a real machine. The shipped-default check
		// that CAN fail lives in Trace.Keys.VerifyV28, where it reads the table rather than the player.
		if (bParryOnQ && bParryOnThumb)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[RightMouse]   ok       PARRY (\'%s\') is on %s - spec v28 s3d\'s Q + thumb pair."),
				TraceInputActions::Info(ETraceInputAction::Parry).ConfigId,
				*Settings.DescribeBinding(ETraceInputAction::Parry));
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[RightMouse]   note     PARRY is on %s, not spec v28 s3d\'s Q + thumb mouse pair. That is a ")
				TEXT("rebind on this machine, not a failure - the shipped defaults are checked by ")
				TEXT("Trace.Keys.VerifyV28."),
				*Settings.DescribeBinding(ETraceInputAction::Parry));
		}

		if (bThrowOffRMB)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[RightMouse]   ok       THROW (\'%s\') is on \'%s\' - NOT the right mouse button. Mouse 1 still ")
				TEXT("throws while carrying; that path is ATraceCharacter::DoFirePressed and is untouched."),
				TraceInputActions::Info(ETraceInputAction::Pass).ConfigId, *Settings.DescribeBinding(ETraceInputAction::Pass));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[RightMouse]   WRONG    THROW is STILL on the right mouse button. Spec v25 s7 removes exactly ")
				TEXT("this bind, and a stale \'Pass=RightMouseButton\' line in TraceUserSettings.ini is how it survives."));
		}

		// *** SPEC v28 §10 — MELEE NOW HOLDS THE BUTTON, AND EXACTLY ONE ACTION MAY. ***
		//
		// This block used to assert that the button was EMPTY. That was the correct assertion for as
		// long as the melee verb had no row in the action table, which is the state the §3 and §10
		// slices each left it in — §3 vacated the button and could not add the row (the verb was §10's
		// file), §10 built the verb and could not bind it (the table is §3's file). The integrator added
		// ETraceInputAction::Melee, so the assertion inverts: MELEE must hold it, and nothing else may.
		//
		// Both halves are failures, not notes, and for the same reason as before: a second holder means
		// one press does two things, and that is found in a playtest rather than in a build. The parry
		// is the specific one being guarded against — a hand-edited .ini goes through RefreshFromConfig,
		// which does not steal, so a file that still says the parry is on right mouse would produce
		// exactly the collision §3d's "ParryPull" -> "ParryKeys" migration exists to prevent.
		const bool bMeleeOnRMB = Settings.ActionUsesKey(ETraceInputAction::Melee, RMB);
		Failures += bMeleeOnRMB ? 0 : 1;

		if (bMeleeOnRMB)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[RightMouse]   ok       MELEE (\'%s\') is on %s - spec v28 s10\'s default. The verb is ")
				TEXT("TraceMelee::HandleMeleeInput, and the Core-pull precedence inside it is ATraceCore::CanPullNow, ")
				TEXT("which is the SAME call ATraceHUD makes to decide whether to draw the circle."),
				TraceInputActions::Info(ETraceInputAction::Melee).ConfigId,
				*Settings.DescribeBinding(ETraceInputAction::Melee));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[RightMouse]   WRONG    MELEE is on %s, NOT the right mouse button. Spec v28 s10 says \"Melee ")
				TEXT("should be bound to right click by default\"; a stale TraceUserSettings.ini line ")
				TEXT("\'Melee=<something>\' is how that survives a correct table."),
				*Settings.DescribeBinding(ETraceInputAction::Melee));
		}

		int32 OtherHolders = 0;
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			if (Info.Action == ETraceInputAction::Melee || !Settings.ActionUsesKey(Info.Action, RMB))
			{
				continue;
			}
			++OtherHolders;
			++Failures;
			UE_LOG(LogTraceGame, Error,
				TEXT("[RightMouse]   WRONG    \'%s\' (%s) ALSO holds the RIGHT MOUSE BUTTON, which spec v28 s10 gives ")
				TEXT("to the MELEE. One press would run two verbs. Note this is a collision no exclusion group can ")
				TEXT("resolve on its own: melee\'s group is NOT CARRYING, so the check would refuse a NotCarrying ")
				TEXT("action but permits a Carrying one - and the parry is Carrying."),
				Info.ConfigId, Info.DisplayName);
		}
		if (OtherHolders == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[RightMouse]   ok       the RIGHT MOUSE BUTTON is held by MELEE ALONE: none of the other %d ")
				TEXT("rebindable actions holds it in either slot."),
				TraceInputActions::All().Num() - 1);
		}

		// ---- 1b. SPEC v26 §1 — ARE PARRY AND PULL ACTUALLY TWO BINDS? ---------------------------
		//
		// "Make parry and pull core two separate binds in the settings menu." Three things have to be
		// true for that sentence to have landed, and all three are read out of the LIVE settings —
		// the same table ApplyControlSettings maps from — rather than out of the defaults, so a
		// hand-edited .ini that has re-merged them FAILS here instead of being discovered in a match.
		{
			const FKey PullKey = Settings.GetKey(ETraceInputAction::PullCore);
			const int32 PullRow = TraceInputActions::All().IndexOfByPredicate(
				[](const FTraceInputActionInfo& Info) { return Info.Action == ETraceInputAction::PullCore; });

			// (i) the pull has a row at all — which is also what puts it on the keybind page, since
			//     the options screen's rebind list IS this table walked in order.
			if (PullRow == INDEX_NONE)
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[RightMouse]   WRONG    there is no PULL CORE row in the action table, so spec v26 s1\'s ")
					TEXT("second bind cannot appear on the keybind page at all."));
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[RightMouse]   ok       PULL CORE (\'%s\', row %d of %d) is on the keybind page and resolves to %s"),
					TraceInputActions::Info(ETraceInputAction::PullCore).ConfigId, PullRow + 1,
					TraceInputActions::All().Num(), *Settings.DescribeBinding(ETraceInputAction::PullCore));
			}

			// (ii) it is BOUND. An unbound pull is a legal state a player may choose, but it is not the
			//      shipped one, and shipping it unbound would be the item silently not landing.
			if (!PullKey.IsValid())
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[RightMouse]   WRONG    PULL CORE is UNBOUND. Spec v26 s1 asks for a distinct action AND a ")
					TEXT("default; F is the shipped one."));
			}

			// (iii) the two are on DIFFERENT keys. Sharing one is legal and handled — OnParryStarted
			//       keeps v25's precedence rule as a tiebreak for exactly that, and since spec v28 §3b the
			//       options page will now PRODUCE that state on request (their exclusion groups are
			//       disjoint) instead of it only being reachable by hand-editing the .ini. Still reported,
			//       because it is not the shipped two-bind default.
			// The same intersection ATracePlayerController::DoParryAndPullShareAKey computes at runtime,
			// recomputed here rather than called, because that method is private and this is a
			// file-scope diagnostic — and because a diagnostic that asked the code under test whether it
			// agreed with itself would be worth nothing.
			bool bShared = false;
			{
				TArray<FKey> ParryKeys;
				Settings.GetKeys(ETraceInputAction::Parry, ParryKeys);
				for (const FKey& Key : ParryKeys)
				{
					if (Settings.ActionUsesKey(ETraceInputAction::PullCore, Key))
					{
						bShared = true;
						break;
					}
				}
			}
			if (bShared)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[RightMouse]   SHARED   PARRY (%s) and PULL CORE (%s) have a key in common. Supported: the ")
					TEXT("v25 s2 precedence rule is applied as a TIEBREAK (one press, both verbs, the authoritative ")
					TEXT("gates decide), and spec v28 s3b allows it because parry needs the Core and the pull needs ")
					TEXT("not to have it. It is not the shipped default."),
					*Settings.DescribeBinding(ETraceInputAction::Parry),
					*Settings.DescribeBinding(ETraceInputAction::PullCore));
			}
			else if (PullKey.IsValid())
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[RightMouse]   ok       PARRY (%s) and PULL CORE (%s) are TWO SEPARATE BINDS - spec v26 s1."),
					*Settings.DescribeBinding(ETraceInputAction::Parry),
					*Settings.DescribeBinding(ETraceInputAction::PullCore));
			}
		}

		// ---- 2. is the s2 pull actually wired? --------------------------------------------------
		const EPullBinding Binding = GetPullBinding();
		if (Binding == EPullBinding::None)
		{
			++Failures;
			UE_LOG(LogTraceGame, Error,
				TEXT("[RightMouse]   NOT WIRED  the spec v25 s2 CORE-PULL found no entry point. The input half is ")
				TEXT("in and the press is being dispatched, but it lands nowhere. ATraceCharacter must expose a ")
				TEXT("PUBLIC DoPullPressed()/DoPullReleased(), or ATraceCore a PUBLIC ")
				TEXT("RequestPullInput(bool, ATraceCharacter*). See EPullBinding in TracePlayerController.cpp."));
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("[RightMouse]   ok       the CORE-PULL is dispatched to %s"),
				LexPullBinding(Binding));
		}

		// ---- 3. the two verbs cannot both fire --------------------------------------------------
		const ATracePlayerController* Live = FindLocalTraceController();
		ATraceCharacter* LocalPawn = (Live != nullptr) ? Live->GetPawn<ATraceCharacter>() : nullptr;

		if (LocalPawn == nullptr)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[RightMouse]   no local pawn - the mutual-exclusion half was NOT measured this run. Run it ")
				TEXT("again in a live match."));
		}
		else
		{
			const ATraceCore* TheCore = ATraceCore::Get(LocalPawn->GetWorld());
			const bool bCarrying = (TheCore != nullptr) && (TheCore->GetCarrier() == LocalPawn);

			if (bCarrying)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[RightMouse]   local pawn IS CARRYING: PARRY is the legal verb and the PULL is refused ")
					TEXT("(s2 requires you not to be carrying). NOT exercised - asking RequestParry here would ")
					TEXT("open a real parry window, which a diagnostic may not do. Run this again without the Core."));
			}
			else
			{
				// The real gate, asked for real. A refusal is side-effect free by construction: no
				// window is opened and no RPC is sent, which is exactly why this is the case that can
				// be measured rather than asserted.
				ETraceParryRefusal Refusal = ETraceParryRefusal::None;
				const bool bParried = TraceParry::RequestParry(LocalPawn, &Refusal);
				const bool bRefusedForCarrying = !bParried && (Refusal == ETraceParryRefusal::NotCarrying);

				if (bRefusedForCarrying)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[RightMouse]   ok       local pawn is NOT carrying: TraceParry refused with '%s', so ")
						TEXT("this press cannot become a parry and is free for the pull. That is the whole ")
						TEXT("precedence - the two gates are opposites on 'am I the carrier', so at most one can ")
						TEXT("ever accept and neither can eat the other."),
						LexToString(Refusal));
				}
				else
				{
					++Failures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[RightMouse]   WRONG    local pawn is NOT carrying, yet TraceParry returned %d with ")
						TEXT("refusal '%s'. Parry is supposed to be carrier-only (spec v3 s3) - if it is not, the ")
						TEXT("parry and the pull CAN both be legal on one press and s7's precedence argument is void."),
						bParried ? 1 : 0, LexToString(Refusal));
				}
			}
		}

// SPEC v28 §3d CHANGED WHAT A PASS MEANS HERE, AGAIN. v25 §7's claim was "right mouse is parry";
// v26 §1's was "right mouse is the parry ALONE and the pull is its own bind"; v28 §3d's is that the
// button is EMPTY — parry has moved to Q + the thumb mouse button so §10's melee can have it — while
// the throw is still off it and the pull is still wired somewhere. Every key is PRINTED rather than
// asserted, because the player is allowed to rebind all of them and this command reads the live
// table; the assertions are the two things no rebind may produce (a throw on right mouse, anything
// at all on right mouse) plus the pull being wired at all.
#define TRACE_RIGHTMOUSE_VERDICT_ARGS \
	(Failures == 0) ? TEXT("RIGHT MOUSE IS FREE FOR THE MELEE, AND THE CORE VERBS ARE WHERE THEY BELONG") \
	                : TEXT("THE RIGHT MOUSE BUTTON IS WRONG"), \
	*Settings.DescribeBinding(ETraceInputAction::Parry), \
	*Settings.DescribeBinding(ETraceInputAction::Pass), \
	*Settings.DescribeBinding(ETraceInputAction::PullCore), \
	LexPullBinding(Binding), Failures

#define TRACE_RIGHTMOUSE_VERDICT_TEXT \
	TEXT("[RightMouse] VERDICT: %s. parry='%s' throw='%s' pullKey='%s' pull=%s, %d failure(s).")

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_RIGHTMOUSE_VERDICT_TEXT, TRACE_RIGHTMOUSE_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_RIGHTMOUSE_VERDICT_TEXT, TRACE_RIGHTMOUSE_VERDICT_ARGS);
		}

#undef TRACE_RIGHTMOUSE_VERDICT_ARGS
#undef TRACE_RIGHTMOUSE_VERDICT_TEXT
	}

	FAutoConsoleCommand CmdVerifyRightMouse(
		TEXT("Trace.Input.VerifyRightMouse"),
		TEXT("Spec v25 s7 + v26 s1 + v28 s3d. Proves the right mouse button is held by NO rebindable action ")
		TEXT("(so s10's melee may take it), that the Core throw is not on it, that ")
		TEXT("no third action holds it, that spec v25 s2's core-pull found an entry point, and - with a live ")
		TEXT("non-carrying pawn - that the parry gate really does refuse a non-carrier, which is what makes ")
		TEXT("the parry and the pull mutually exclusive on one press."),
		FConsoleCommandDelegate::CreateStatic(&VerifyRightMouse));

	FAutoConsoleCommand CmdVerifyInputAssets(
		TEXT("Trace.Input.VerifyAssets"),
		TEXT("Spec v17 s6. Compares /Game/Trace/Input's IA_*/IMC_Trace assets against the C++ action ")
		TEXT("table (ValueType, accumulation, default keys, modifier shape) and reports whether the ")
		TEXT("live controller is actually using them or is on the C++ fallback."),
		FConsoleCommandDelegate::CreateStatic(&VerifyInputAssets));
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

	// SPEC v26 §9 — Bodyshot / Headshot, client-side, and this is the one place they can be.
	//
	// NO NEW RPC IS ADDED. ClientNotifyHit is the EXISTING server->shooter confirmation and it runs
	// only on the shooter's own machine, which is exactly what "played locally, no RPC" means: the
	// wire traffic that carries the fact already exists and the sound rides the same frame as the
	// hitmarker the player sees. Putting it on the server's resolver instead would either be silent
	// for the shooter or need a second RPC.
	//
	// TraceAudio::Play's client-side gate still runs: `this` is a local APlayerController on the
	// shooter's machine and nowhere else, so a listen-server host does not hear its bots' hits.
	TraceAudio::Play(this, (Zone == ETraceHitZone::Head) ? TraceSoundEvents::Headshot : TraceSoundEvents::Bodyshot);

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
//       THE RED ARM. Identical keys, identical sequence, identical assertions — but the handler is
//       switched to the UNGUARDED equip (UTraceWeaponComponent::RequestEquip, which re-anchors the
//       pullout on a redundant request BY DESIGN). Steps 3 and 5 fail on it: the redundant press
//       restarts the 0.2 s pullout, which is precisely the behaviour §2 asked to be rid of.
//       Same build, same run, same key, same binding, same handler, ONE GATE DIFFERENT.
//
//       SPEC v15 §5 IMPROVED THIS ARM BY DELETING SOMETHING. The old red arm sent the presses to
//       the SwapWeapon toggle's own key and handler, so it changed the action, the binding, the
//       handler and the press counter all at once and a failure could not be pinned on the guard.
//       With the toggle bind gone there was nothing to send them to, and the honest A/B — flip one
//       flag, change nothing else — is what replaced it.
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

		/**
		 * World time the next step is allowed to run, i.e. a GAP FROM THE PREVIOUS STEP rather than a
		 * deadline measured from the start.
		 *
		 * THIS IS A CORRECTION, and the failure it fixes is the one this file's header already warns
		 * about in another form: a test that passes for the wrong reason, or fails for one. The steps
		 * used to gate on absolute offsets (T > 0.35, 0.45, 0.53, ...) from the probe's own start. One
		 * hitch — and the frames right after a match starts are nothing but hitches — put T past ALL of
		 * them at once, so six steps ran on six consecutive frames with a single frame of pullout decay
		 * between them and three assertions about a 0.2 s timer failed against perfectly correct
		 * behaviour. Measured: remaining fell 0.200 -> 0.176 -> 0.148 -> 0.120, a flat 0.028 per step,
		 * which is one frame and not the 0.08-0.12 s each gap claims.
		 *
		 * Gaps cannot be skipped by a hitch: however late a step runs, the next one is still a real
		 * interval later.
		 */
		double NextStepTime = 0.0;

		/** Waits @p Gap seconds from now before the next step. Called as each step completes. */
		void Advance(double Now, double Gap)
		{
			++Step;
			NextStepTime = Now + Gap;
		}

		/** Press counter sampled at the moment of the last synthetic press. */
		int32 CountAtPress = -1;

		/**
		 * THE RED ARM. When true the direct-select handler is routed to the UNGUARDED equip for the
		 * duration of the probe, against the identical checks. See the file-block header.
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

		// THE ONLY DIFFERENCE BETWEEN THE ARMS. Same key, same binding, same handler; the handler's
		// equip call is the one thing that changes. Cleared again when the probe finishes, so a red
		// run cannot leave the build behaving like the bug it was demonstrating.
		GTraceForceUnguardedEquip = bToggleArm;

		// Both arms press the SAME key: spec v13 §2's "1".
		const ETraceInputAction KnifeAction = ETraceInputAction::EquipKnife;
		const TCHAR* const KnifeLabel = bToggleArm ? TEXT("EquipKnife [RED ARM: unguarded]") : TEXT("EquipKnife");

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

				// ...AND WAIT FOR GAMEPLAY INPUT TO BE LIVE. Spec v14 §3's character-select screen is up
				// for the first frames of every match: it PAUSES the world and sets
				// bGameInputSuppressed, so HandleDirectEquip bumps its press counter and then returns
				// without equipping anything. Measured before this check existed: "pressing EquipKnife
				// (key One)" was immediately followed by "[CharSelect] Requesting ROCCO" — the 1 key was
				// picking a character, not drawing a knife — and four assertions failed for a reason
				// that had nothing to do with what they assert.
				//
				// THE PRESS COUNTER DOES NOT CATCH THIS, which is worth stating because it is exactly
				// the vacuity the counter was added to prevent. It is bumped on the handler's FIRST
				// line, ahead of the suppression gate on purpose, so it answers "did the key reach a
				// bound delegate?" and not "did anything happen?". While the select screen is up the
				// honest answer to the first is yes and to the second is no.
				if (PC->IsGameInputSuppressed())
				{
					return true;
				}

				// Whichever handler this arm is exercising, this is the count of presses that have
				// reached a bound delegate. Comparing it either side of a synthetic press is what
				// makes a swallowed key a FAILURE rather than a silent pass.
				const int32 PressCount = PC->GetDebugEquipPressCount();

				// WORLD TIME, NOT WALL CLOCK, and that is a correction rather than a preference.
				// Every value this probe asserts on — TraceMelee::GetDeployRemaining, the 0.2 s pullout
				// — is measured in WORLD seconds, so scheduling the steps off FPlatformTime::Seconds
				// silently assumed the two run at the same rate. On a machine with other headless rigs
				// on it they do not: measured at 0.026 s of world time elapsing across 0.080 s of wall
				// clock, which put step 3's "the pullout has had time to tick down" check a third of the
				// way to where it thought it was and failed three assertions about behaviour that was
				// entirely correct. One clock for the schedule and the assertions, and frame rate stops
				// being able to decide the verdict.
				const double Now = World->GetTimeSeconds();
				if (Probe->StartTime < 0.0)
				{
					Probe->StartTime = Now;
					UE_LOG(LogTraceGame, Display,
						TEXT("[V13.Hotkeys] start on %s, arm=%s. EquipKnife=%s EquipGun=%s | pullout=%.3fs hold=%.2fs"),
						*GetNameSafe(Pawn),
						Probe->bToggleArm ? TEXT("UNGUARDED (RED ARM — these checks are EXPECTED to fail)") : TEXT("direct select"),
						*UTraceUserSettings::DescribeKey(UTraceUserSettings::Get().GetKey(ETraceInputAction::EquipKnife)),
						*UTraceUserSettings::DescribeKey(UTraceUserSettings::Get().GetKey(ETraceInputAction::EquipGun)),
						TraceMelee::GetSwapSeconds(), V13PressHoldSeconds);
				}

				const bool bKnife = TraceMelee::IsKnifeEquipped(Pawn);
				const float Deploy = TraceMelee::GetDeployRemaining(Pawn);

				switch (Probe->Step)
				{
				case 0:
					// Baseline: put the GUN in hand with the "2" key, so the run starts from a known
					// weapon whatever the pawn spawned with.
					V13PressBoundKey(World, ETraceInputAction::EquipGun, TEXT("EquipGun"));
					// 0.35 s: comfortably clear of the 0.2 s pullout the baseline press costs.
					Probe->Advance(Now, 0.35);
					break;

				case 1:
					if (Now >= Probe->NextStepTime)
					{
						Probe->Check(!bKnife, TEXT("step 1: '2' put the GUN in hand"));
						Probe->Check(FMath::IsNearlyZero(Deploy, 1e-3f),
							FString::Printf(TEXT("step 1: baseline pullout finished (remaining=%.3f)"), Deploy));
						Probe->CountAtPress = PressCount;
						V13PressBoundKey(World, KnifeAction, KnifeLabel);
						// 0.10 s: long enough for the press to be delivered and the equip applied, far
						// short of the 0.2 s pullout it starts — step 2 must land MID-pullout.
						Probe->Advance(Now, 0.10);
					}
					break;

				case 2:
					if (Now >= Probe->NextStepTime)
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
						// 0.08 s: still inside the pullout, and enough decay to be measurable.
						Probe->Advance(Now, 0.08);
					}
					break;

				case 3:
					if (Now >= Probe->NextStepTime)
					{
						Probe->Check(PressCount > Probe->CountAtPress,
							FString::Printf(TEXT("step 3: the REDUNDANT mid-pullout press reached the handler (count %d -> %d)"),
								Probe->CountAtPress, PressCount));
						Probe->Check(bKnife, TEXT("step 3: still the KNIFE — a repeat press did not put it away again"));
						Probe->Check(Deploy < Probe->DeployBeforeRedundant,
							FString::Printf(TEXT("step 3: pullout NOT restarted — remaining fell %.3f -> %.3f"),
								Probe->DeployBeforeRedundant, Deploy));
						// 0.20 s, up from the 0.12 the absolute schedule allowed: step 4 asserts the
						// pullout has RUN OUT, and the pullout is 0.2 s from the step-2 press, so 0.12
						// only ever cleared it because the old deadlines were measured from a common
						// origin. From here it has to be a full pullout plus slack.
						Probe->Advance(Now, 0.20);
					}
					break;

				case 4:
					if (Now >= Probe->NextStepTime)
					{
						Probe->Check(FMath::IsNearlyZero(Deploy, 1e-3f),
							FString::Printf(TEXT("step 4: pullout ran to completion (remaining=%.3f)"), Deploy));
						// And again with the weapon fully up: still nothing.
						Probe->CountAtPress = PressCount;
						V13PressBoundKey(World, KnifeAction, KnifeLabel);
						Probe->Advance(Now, 0.10);
					}
					break;

				case 5:
					if (Now >= Probe->NextStepTime)
					{
						Probe->Check(PressCount > Probe->CountAtPress,
							FString::Printf(TEXT("step 5: the third press reached the handler (count %d -> %d)"),
								Probe->CountAtPress, PressCount));
						Probe->Check(bKnife, TEXT("step 5: still the KNIFE after a third press"));
						Probe->Check(FMath::IsNearlyZero(Deploy, 1e-3f),
							FString::Printf(TEXT("step 5: NO new pullout from the redundant press (remaining=%.3f)"), Deploy));
						V13PressBoundKey(World, ETraceInputAction::EquipGun, TEXT("EquipGun"));
						Probe->Advance(Now, 0.10);
					}
					break;

				case 6:
					if (Now >= Probe->NextStepTime)
					{
						Probe->Check(!bKnife, TEXT("step 6: '2' switched back to the GUN"));
						Probe->Check(Deploy > 0.f,
							FString::Printf(TEXT("step 6: a genuine change DOES cost a pullout (remaining=%.3f)"), Deploy));
						Probe->Advance(Now, 0.0);
					}
					break;

				default:
					// Put the handler back before reporting, whichever arm ran. A red run that left the
					// build routing "1" through the unguarded equip would turn a demonstration into the
					// very regression it was demonstrating.
					GTraceForceUnguardedEquip = false;

					if (Probe->Failures == 0)
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[V13.Hotkeys] RESULT: PASS — %d checks (arm=%s). 1 = knife, 2 = gun, direct select, "
							     "every press verified to have reached the handler, and a redundant press costs no pullout."),
							Probe->Passes, Probe->bToggleArm ? TEXT("unguarded") : TEXT("direct select"));
					}
					else
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[V13.Hotkeys] RESULT: FAIL — %d passed, %d FAILED (arm=%s%s)."),
							Probe->Passes, Probe->Failures,
							Probe->bToggleArm ? TEXT("unguarded") : TEXT("direct select"),
							Probe->bToggleArm ? TEXT(" — THIS IS THE RED ARM AND FAILING IS THE POINT") : TEXT(""));
					}
					return false;   // done
				}

				return true;
			}), 0.f);
	}

	FAutoConsoleCommand CmdV13Hotkeys(
		TEXT("Trace.V13.Hotkeys"),
		TEXT("Dev only. Spec v13 §2: press the bound EquipKnife/EquipGun keys through the real input pipeline and prove direct select does not re-trigger the pullout. Pass 'toggle' for the RED ARM, which sends the SAME presses through the UNGUARDED equip and is expected to FAIL."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const bool bToggleArm = (Args.Num() > 0) && Args[0].Equals(TEXT("toggle"), ESearchCase::IgnoreCase);
			V13RunHotkeyProbe(bToggleArm);
		}));
}

// =================================================================================================
// Trace.Ammo.BindTest — spec v16 §1's "R to reload", through the real input pipeline
//
// WHY A SEPARATE COMMAND FROM Trace.Ammo.Test. That one proves the RELOAD works by calling
// UTraceWeaponComponent::RequestReload directly, which is the right way to test the rules (full clip,
// mid-reload, carrying, an ability clip) and says nothing whatsoever about whether a key is wired to
// it. Everything between "the player pressed R" and "RequestReload was called" — the action table
// row, the mapping context, the Enhanced Input binding, the handler — is invisible to it.
//
// SO THIS PRESSES THE KEY. Trace.SimInput on whatever UTraceUserSettings currently has bound to
// ETraceInputAction::Reload, exactly as the v13 §2 hotkey probe does for 1 and 2. A rebind, an
// unbound action, a mapping that never reached the subsystem and a handler that was never bound all
// fail it.
//
// ITS CONTROL IS AN UNBOUND KEY, and that is what stops it being vacuous. A probe that only pressed
// R and watched the counter rise could not tell "R is bound to reload" from "the counter rises on
// any key at all" — so it first presses a key nothing claims and requires the counter NOT to move.
// The press counter is deliberately incremented on the handler's first line, before every gate, so
// "the key arrived" and "a reload started" are two separate observations rather than one.
// =================================================================================================

namespace TraceReloadBindTest
{
	/** Same 0.02 s synthetic hold the v13 probe uses; the gaps between steps are what matter. */
	constexpr float PressHoldSeconds = 0.02f;

	/** A key nothing in TraceInputActions::All() claims. Pressed as the control. */
	FKey UnclaimedKey()
	{
		return EKeys::K;
	}

	struct FProbe
	{
		int32 Step = 0;
		double NextStepTime = 0.0;
		int32 Passes = 0;
		int32 Failures = 0;

		int32 CountBeforeControl = 0;
		int32 CountAfterControl = 0;
		int32 CountBeforeReal = 0;
		int32 ClipBeforePress = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition)
			{
				++Passes;
				UE_LOG(LogTraceGame, Display, TEXT("[Ammo.BindTest]   PASS  %s"), *What);
			}
			else
			{
				++Failures;
				UE_LOG(LogTraceGame, Error, TEXT("[Ammo.BindTest]   FAIL  %s"), *What);
			}
		}
	};

	void PressKey(UWorld* World, const FKey& Key, const TCHAR* Label)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[Ammo.BindTest]   pressing %s (key %s)"),
			Label, *Key.GetFName().ToString());
		GEngine->Exec(World, *FString::Printf(TEXT("Trace.SimInput %s %.2f"),
			*Key.GetFName().ToString(), PressHoldSeconds));
	}

	void Run()
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
			UE_LOG(LogTraceGame, Warning, TEXT("[Ammo.BindTest] no game world yet."));
			return;
		}

		TSharedPtr<FProbe> Probe = MakeShared<FProbe>();

		UE_LOG(LogTraceGame, Display,
			TEXT("[Ammo.BindTest] ===== spec v16 §1: 'R to reload', pressed through the real pipeline. Reload is "
			     "bound to %s. Control: %s, which no action claims. ====="),
			*UTraceUserSettings::DescribeKey(UTraceUserSettings::Get().GetKey(ETraceInputAction::Reload)),
			*UTraceUserSettings::DescribeKey(UnclaimedKey()));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Probe, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}
			const double Now = FPlatformTime::Seconds();
			if (Now < Probe->NextStepTime)
			{
				return true;
			}

			ATracePlayerController* PC = Cast<ATracePlayerController>(TickWorld->GetFirstPlayerController());
			ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
			UTraceWeaponComponent* Weapon = (Pawn != nullptr) ? Pawn->Weapon : nullptr;

			if (PC == nullptr || Pawn == nullptr || Weapon == nullptr || !Pawn->IsAlive()
				|| Pawn->IsCarrier() || Weapon->IsKnifeEquipped())
			{
				if (Probe->Step == 0)
				{
					return true;   // still waiting for a usable pawn; safe at launch
				}
				UE_LOG(LogTraceGame, Error,
					TEXT("[Ammo.BindTest] VERDICT: INVALID — the local pawn stopped being usable mid-probe."));
				return false;
			}

			// ...AND WAIT FOR GAMEPLAY INPUT TO BE LIVE, which is the same trap the v13 §2 probe
			// documents at length a few hundred lines above — and which this probe walked straight into
			// on its first run. Spec v14 §3's character-select screen is up for the first seconds of
			// every match: it sets bGameInputSuppressed, so OnReloadStarted bumps its press counter and
			// then returns without asking for anything.
			//
			// MEASURED, before this gate existed: "the bound key REACHES OnReloadStarted (count 0 -> 1)"
			// passed and "a reload is actually running" failed — a perfectly correct build reported as
			// a broken bind. The press counter cannot catch this and is not meant to: it answers "did
			// the key reach a bound delegate", and while the select screen is up the honest answer to
			// that is yes and to "did anything happen" is no.
			if (PC->IsGameInputSuppressed())
			{
				if (Probe->Step == 0)
				{
					return true;
				}
				UE_LOG(LogTraceGame, Error,
					TEXT("[Ammo.BindTest] VERDICT: INVALID — gameplay input was suppressed mid-probe (a menu or "
					     "the character-select screen came up), so a swallowed press cannot be told from a "
					     "broken bind."));
				return false;
			}

			switch (Probe->Step)
			{
			case 0:
				// A PARTIAL CLIP, or the reload would be refused for a reason that has nothing to do
				// with the key and the whole probe would measure the full-clip rule instead.
				for (int32 Index = 0; Index < 5; ++Index)
				{
					Weapon->DebugConsumeRound();
				}
				Probe->CountBeforeControl = PC->GetDebugReloadPressCount();
				PressKey(TickWorld, UnclaimedKey(), TEXT("the CONTROL key"));
				Probe->Step = 1;
				Probe->NextStepTime = Now + 0.30;
				return true;

			case 1:
				Probe->CountAfterControl = PC->GetDebugReloadPressCount();
				Probe->Check(Probe->CountAfterControl == Probe->CountBeforeControl,
					FString::Printf(TEXT("CONTROL: an unclaimed key does NOT reach the reload handler "
					                     "(count %d -> %d)"),
						Probe->CountBeforeControl, Probe->CountAfterControl));
				Probe->Check(!Weapon->IsReloading(),
					TEXT("CONTROL: ...and starts no reload"));

				Probe->CountBeforeReal = PC->GetDebugReloadPressCount();
				Probe->ClipBeforePress = Weapon->GetClipAmmo();
				PressKey(TickWorld, UTraceUserSettings::Get().GetKey(ETraceInputAction::Reload),
					TEXT("the bound RELOAD key"));
				Probe->Step = 2;
				Probe->NextStepTime = Now + 0.30;
				return true;

			case 2:
			{
				const int32 CountNow = PC->GetDebugReloadPressCount();
				Probe->Check(CountNow > Probe->CountBeforeReal,
					FString::Printf(TEXT("the bound key REACHES ATracePlayerController::OnReloadStarted "
					                     "(count %d -> %d)"),
						Probe->CountBeforeReal, CountNow));

				// The reload is 0.5 s and this step runs 0.30 s after the press, so it must still be
				// running — which also rules out "it finished, therefore nothing happened".
				Probe->Check(Weapon->IsReloading(),
					FString::Printf(TEXT("...and a reload is actually running %.3fs later (%.3fs left, clip was "
					                     "%d/%d)"),
						0.30, Weapon->GetReloadRemaining(), Probe->ClipBeforePress, Weapon->GetClipSize()));

				Probe->Step = 3;
				Probe->NextStepTime = Now + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.25;
				return true;
			}

			default:
				Probe->Check(Weapon->GetClipAmmo() == Weapon->GetClipSize(),
					FString::Printf(TEXT("...and it refills the clip (%d/%d)"),
						Weapon->GetClipAmmo(), Weapon->GetClipSize()));

				if (Probe->Failures == 0)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[Ammo.BindTest] VERDICT: PASS — %d checks, 0 failed."),
						Probe->Passes);
				}
				else
				{
					UE_LOG(LogTraceGame, Error, TEXT("[Ammo.BindTest] VERDICT: *** FAIL *** — %d passed, %d FAILED."),
						Probe->Passes, Probe->Failures);
				}
				return false;
			}
		}));
	}

	FAutoConsoleCommand CmdAmmoBindTest(
		TEXT("Trace.Ammo.BindTest"),
		TEXT("Dev only. Spec v16 §1: press the bound RELOAD key through the real Enhanced Input pipeline and "
		     "prove it reaches the handler and starts a reload. Presses an UNCLAIMED key first as the control, "
		     "so the run cannot pass by responding to any key at all."),
		FConsoleCommandDelegate::CreateStatic(&Run));
}

#endif // !UE_BUILD_SHIPPING
