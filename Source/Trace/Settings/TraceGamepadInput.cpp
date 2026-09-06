// Trace — D31-PAD: the gamepad's mapping context, and D32-PADMENU: the vocabulary every
// pad-driven menu screen reads. See TraceGamepadInput.h for both arguments.

#include "Settings/TraceGamepadInput.h"

#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Misc/CoreMiscDefines.h"
#include "UnrealClient.h"

#include "Settings/TraceUserSettings.h"
#include "Trace.h"

// =================================================================================================
// Plumbing
//
// A NAMED namespace, not an anonymous one: this module is compiled as a unity/jumbo build and two
// files that each open an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
// =================================================================================================

namespace TraceGamepadInputImpl
{
	/** Where the pad's IA_ assets live, mirroring ATracePlayerController's own path. */
	const TCHAR* const AssetDirectory = TEXT("/Game/Trace/Input");

	/**
	 * The two sticks, and the ONLY two mappings in this file that are not a per-action button.
	 *
	 * Named constants rather than literals at the two use sites because Trace.Pad.Verify asserts
	 * against them: a check that hardcoded its own idea of which key MOVE is on would pass while the
	 * context mapped something else.
	 */
	FKey MoveStick() { return EKeys::Gamepad_Left2D; }
	FKey LookStick() { return EKeys::Gamepad_Right2D; }

	UInputModifierDeadZone* MakeRadialDeadZone(UObject* Outer, float Lower)
	{
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(Outer);
		DeadZone->LowerThreshold = Lower;
		DeadZone->UpperThreshold = 1.f;

		// RADIAL AND NOT AXIAL, and this is the difference between a stick that feels round and one
		// that feels like a plus sign. Axial tests each component against the threshold on its own, so
		// a stick pushed diagonally at 0.19 on both axes reads as dead while a stick pushed straight
		// at 0.21 on one reads as live — the dead region is a SQUARE, and a player circling a target
		// feels the corners. Radial tests the magnitude, which is the shape the stick physically has.
		//
		// The engine also RESCALES what survives, so the first live sample is 0.0 rather than the
		// threshold: without that, crossing the dead zone would snap the view by the dead-zone value.
		DeadZone->Type = EDeadZoneType::Radial;
		return DeadZone;
	}

	UInputModifierScalar* MakeScalar(UObject* Outer, float X, float Y)
	{
		UInputModifierScalar* Scalar = NewObject<UInputModifierScalar>(Outer);
		Scalar->Scalar = FVector(X, Y, 1.f);
		return Scalar;
	}

	/**
	 * The stick's response curve. SQUARE, on both axes.
	 *
	 * WHY A CURVE AT ALL: a stick has roughly 1.4 cm of throw and has to cover both "track a target
	 * that is drifting one degree per second" and "spin 180". A linear map cannot do both — pick a
	 * rate fast enough for the turn and the first half-millimetre of the tracking motion is already
	 * five degrees. Squaring spends most of the stick's travel on the slow half, which is where a
	 * player's fingers actually live, and keeps full deflection at the full rate.
	 *
	 * EXPONENT 2 AND NOT 3. Three is the other common answer and it makes the centre so soft that
	 * small corrections feel unresponsive on a worn stick, which reads as input lag. Two is the value
	 * that survives on a pad whose sticks are not new.
	 *
	 * NOT A SETTING. It is a shape, not a quantity — a player who wants "faster" reaches for the rate
	 * slider, and a curve exponent on a settings page is a control nobody can predict the effect of.
	 */
	UInputModifierResponseCurveExponential* MakeSquareCurve(UObject* Outer)
	{
		UInputModifierResponseCurveExponential* Curve = NewObject<UInputModifierResponseCurveExponential>(Outer);
		Curve->CurveExponent = FVector(2.f, 2.f, 1.f);
		return Curve;
	}

	/** The first local player controller in any game world. Null on a dedicated server. */
	APlayerController* FindLocalController(const UGameInstance* GameInstance)
	{
		if (GameInstance == nullptr)
		{
			return nullptr;
		}

		const UWorld* World = GameInstance->GetWorld();
		if (World == nullptr)
		{
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* const PC = It->Get();

			// IsLocalController, because on a listen server this iterator also walks the remote
			// players' controllers — and a remote controller has no local player and no input at all.
			if (PC != nullptr && PC->IsLocalController())
			{
				return PC;
			}
		}
		return nullptr;
	}
}

// =================================================================================================
// D32-PADMENU — the pad's menu vocabulary, implemented once
//
// See the block at the bottom of TraceGamepadInput.h for why these live here rather than being
// written out four times, and for the button table itself. Nothing below is compiled out of
// Shipping: these ARE the feature, not a check on it.
// =================================================================================================

namespace TracePadMenu
{
	const FKey& ConfirmKey() { return EKeys::Gamepad_FaceButton_Bottom; }
	const FKey& BackKey()    { return EKeys::Gamepad_FaceButton_Right; }
	const FKey& AltKey()     { return EKeys::Gamepad_FaceButton_Left; }

	bool IsEnabled()
	{
		return UTraceUserSettings::Get().bPadEnabled;
	}

	bool HasSeenPad(const UObject* WorldContext)
	{
		const UTraceGamepadInputSubsystem* const Pad = UTraceGamepadInputSubsystem::Get(WorldContext);
		return (Pad != nullptr) && Pad->HasSeenGamepadInput();
	}

	/**
	 * "Is this direction being asked for", over a D-pad key and its left-stick twin.
	 *
	 * Byte-for-byte the predicate FTraceOptionsMenu::PollNavigation's local `PadDown` lambda uses,
	 * and Trace.Pad.MenuVerify asserts the two key LISTS still agree. See the header.
	 */
	static bool DirDown(const APlayerController* PC, const FKey& DPad, const FKey& Stick)
	{
		if (PC == nullptr || !IsEnabled())
		{
			return false;
		}
		return PC->IsInputKeyDown(DPad) || PC->IsInputKeyDown(Stick)
			|| PC->WasInputKeyJustPressed(DPad) || PC->WasInputKeyJustPressed(Stick);
	}

	int32 NavX(const APlayerController* PC)
	{
		int32 Dir = 0;
		if (DirDown(PC, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right)) { Dir += 1; }
		if (DirDown(PC, EKeys::Gamepad_DPad_Left,  EKeys::Gamepad_LeftStick_Left))  { Dir -= 1; }
		return Dir;
	}

	int32 NavY(const APlayerController* PC)
	{
		int32 Dir = 0;
		if (DirDown(PC, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_LeftStick_Down)) { Dir += 1; }
		if (DirDown(PC, EKeys::Gamepad_DPad_Up,   EKeys::Gamepad_LeftStick_Up))   { Dir -= 1; }
		return Dir;
	}

	static bool ButtonPressed(const APlayerController* PC, const FKey& Key)
	{
		return PC != nullptr && IsEnabled() && PC->WasInputKeyJustPressed(Key);
	}

	bool ConfirmPressed(const APlayerController* PC) { return ButtonPressed(PC, ConfirmKey()); }
	bool BackPressed(const APlayerController* PC)    { return ButtonPressed(PC, BackKey()); }
	bool AltPressed(const APlayerController* PC)     { return ButtonPressed(PC, AltKey()); }

	bool RisingEdge(const APlayerController* PC, const FKey& Key, bool& bWasDown)
	{
		const bool bDown = (PC != nullptr) && PC->IsInputKeyDown(Key);

		// RECORDED BEFORE THE GATE IS APPLIED, deliberately. If the state were only tracked while
		// controller input was on, a player who switched it on with a button already held would get an
		// edge out of the switch itself.
		const bool bEdge = bDown && !bWasDown;
		bWasDown = bDown;

		return bEdge && IsEnabled();
	}

	bool StepRepeat(int32 Dir, int32& LastDir, float& NextTime, float Now)
	{
		if (Dir == 0)
		{
			// RESET, not "leave it alone". Without this, releasing the D-pad and pressing the SAME
			// direction again inside the repeat window would be read as a still-held direction and
			// would silently swallow the second press.
			LastDir = 0;
			return false;
		}

		if (Dir != LastDir)
		{
			// Direction just became held: act immediately, then wait out the longer first delay.
			LastDir = Dir;
			NextTime = Now + RepeatDelay;
			return true;
		}

		if (Now >= NextTime)
		{
			NextTime = Now + RepeatInterval;
			return true;
		}

		return false;
	}
}

// =================================================================================================
// Lifecycle
// =================================================================================================

UTraceGamepadInputSubsystem* UTraceGamepadInputSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = (WorldContext != nullptr)
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull)
		: nullptr;

	const UGameInstance* GameInstance = (World != nullptr) ? World->GetGameInstance() : nullptr;
	return (GameInstance != nullptr) ? GameInstance->GetSubsystem<UTraceGamepadInputSubsystem>() : nullptr;
}

void UTraceGamepadInputSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// EVERY FRAME (a delay of 0), and the two jobs justify it separately. Re-applying the context is
	// an idempotent HasMappingContext test that does nothing on all but a handful of frames — and it
	// has to run on all of them, because a travel gives the local player a brand-new UPlayerInput
	// that has never heard of this context, and there is no event for "your mapping contexts were
	// just thrown away". The MENU/START edge simply cannot be sampled at any lower rate without
	// losing taps.
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UTraceGamepadInputSubsystem::Tick), 0.f);

	// The pad's mappings carry the player's own numbers (rate, dead zones, inversion) and the player's
	// own button choices, so every settings change has to reach this the way it reaches
	// ATracePlayerController::ApplyControlSettings. See UTraceUserSettings::Save.
	SettingsChangedHandle = UTraceUserSettings::OnChanged().AddUObject(
		this, &UTraceGamepadInputSubsystem::ApplyPadSettings);

	UE_LOG(LogTraceGame, Display,
		TEXT("[Pad] Gamepad input subsystem up. The pad's mapping context is applied as soon as a ")
		TEXT("local player exists and is never removed, so a controller paired mid-session needs no ")
		TEXT("detection and cannot disable the keyboard."));
}

void UTraceGamepadInputSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	if (SettingsChangedHandle.IsValid())
	{
		UTraceUserSettings::OnChanged().Remove(SettingsChangedHandle);
		SettingsChangedHandle.Reset();
	}

	// Deliberately NOT removing the context from the local player: the local player is torn down with
	// the game instance this subsystem belongs to, and reaching into it during shutdown is how a
	// half-destroyed UPlayerInput gets dereferenced.
	PadContext = nullptr;
	AppliedTo = nullptr;

	Super::Deinitialize();
}

APlayerController* UTraceGamepadInputSubsystem::LocalController() const
{
	return TraceGamepadInputImpl::FindLocalController(GetGameInstance());
}

// =================================================================================================
// Resolving the actions
// =================================================================================================

bool UTraceGamepadInputSubsystem::ResolveInputActions()
{
	Actions.Reset();

	// ---- Source 1: the objects the player's input is ACTUALLY using -----------------------------
	//
	// See the header. Whichever of ATracePlayerController's two paths built the actions, this list
	// holds the very objects its handlers are bound to, so a name matched here is exact by
	// construction rather than by convention.
	if (const APlayerController* const PC = LocalController())
	{
		if (const UEnhancedPlayerInput* const PlayerInput = Cast<UEnhancedPlayerInput>(PC->PlayerInput))
		{
			// GetEnhancedActionMappingsView and NOT GetEnhancedActionMappings: the array itself is
			// PROTECTED in 5.8 and the const view over it is the public accessor. Same contents —
			// this player's flattened mapping list, holding the very UInputAction objects the
			// controller bound its handlers to, whichever of its two paths created them.
			for (const FEnhancedActionKeyMapping& Mapping : PlayerInput->GetEnhancedActionMappingsView())
			{
				if (Mapping.Action != nullptr)
				{
					// const_cast: MapKey takes a const UInputAction*, but the map that holds them is
					// declared over UInputAction* so UPROPERTY reflection can root them. Nothing here
					// ever mutates an action — see ATracePlayerController's header on why sharing them
					// is correct.
					Actions.Add(Mapping.Action->GetFName(),
						const_cast<UInputAction*>(ToRawPtr(Mapping.Action)));
				}
			}
		}
	}

	// ---- Source 2: the assets, for anything source 1 could not name -----------------------------
	//
	// One action reaches this every time on a default install: THROW / PASS CORE ships unbound on the
	// keyboard (spec v25 §7), so IA_Pass has no mapping and cannot appear above. It still needs
	// resolving, because a player is free to put it on a pad button.
	const TCHAR* const Wanted[] =
	{
		TEXT("IA_Move"), TEXT("IA_Look"), TEXT("IA_Jump"), TEXT("IA_Crouch"), TEXT("IA_Fire"),
		TEXT("IA_Pass"), TEXT("IA_Dash"), TEXT("IA_Parry"), TEXT("IA_Scoreboard"),
		TEXT("IA_EquipKnife"), TEXT("IA_EquipGun"), TEXT("IA_EquipSmg"), TEXT("IA_Ability"),
		TEXT("IA_AbilitySecondary"), TEXT("IA_Reload"), TEXT("IA_PullCore"), TEXT("IA_Melee"),
		TEXT("IA_Inspect"),
	};

	int32 FromAssets = 0;
	for (const TCHAR* Name : Wanted)
	{
		if (Actions.Contains(FName(Name)))
		{
			continue;
		}

		// LOAD_NoWarn | LOAD_Quiet for the reason ATracePlayerController gives: "the assets have not
		// been generated yet" is the ordinary state of a fresh clone, not an error.
		const FString Path = FString::Printf(TEXT("%s/%s.%s"),
			TraceGamepadInputImpl::AssetDirectory, Name, Name);

		if (UInputAction* Loaded = LoadObject<UInputAction>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Actions.Add(FName(Name), Loaded);
			++FromAssets;
		}
	}

	// MOVE AND LOOK ARE THE TWO THAT MATTER. Without them there is no stick, and a pad with buttons
	// but no sticks is not a playable controller — so this is the failure that gets reported, once.
	const bool bHaveSticks = Actions.Contains(FName(TEXT("IA_Move"))) && Actions.Contains(FName(TEXT("IA_Look")));

	if (!bHaveSticks && !bResolveFailureReported)
	{
		bResolveFailureReported = true;
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Pad] Could not resolve IA_Move / IA_Look, so no controller mappings can be built. ")
			TEXT("The keyboard and mouse are unaffected. This happens when the input assets under %s ")
			TEXT("are missing AND the C++ fallback has not run yet; Trace.Input.VerifyAssets says which."),
			TraceGamepadInputImpl::AssetDirectory);
	}
	else if (bHaveSticks && FromAssets > 0)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Pad] %d action(s) resolved from %s rather than from the live mapping list."),
			FromAssets, TraceGamepadInputImpl::AssetDirectory);
	}

	return bHaveSticks;
}

// =================================================================================================
// Building and applying the context
// =================================================================================================

void UTraceGamepadInputSubsystem::ApplyPadSettings()
{
	using namespace TraceGamepadInputImpl;

	APlayerController* const PC = LocalController();
	ULocalPlayer* const LocalPlayer = (PC != nullptr) ? PC->GetLocalPlayer() : nullptr;
	if (LocalPlayer == nullptr)
	{
		// Normal during early init and on a dedicated server. Tick() calls back.
		return;
	}

	UEnhancedInputLocalPlayerSubsystem* const Input =
		LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (Input == nullptr)
	{
		return;
	}

	const UTraceUserSettings& Settings = UTraceUserSettings::Get();

	// ---- The off switch --------------------------------------------------------------------------
	//
	// Removing the CONTEXT and not clearing its mappings, so that "off" is a state an outside
	// observer can see: HasMappingContext answers false, Trace.Pad.Status says so, and the negative
	// arm of Trace.Pad.Drive is a real removal rather than a table full of blanks.
	if (!Settings.bPadEnabled)
	{
		if (PadContext != nullptr && Input->HasMappingContext(PadContext))
		{
			Input->RemoveMappingContext(PadContext);
			Input->RequestRebuildControlMappings();
			UE_LOG(LogTraceGame, Display,
				TEXT("[Pad] Controller input is OFF (settings). The pad's mapping context has been ")
				TEXT("removed; the keyboard and mouse context is untouched."));
		}
		AppliedTo = nullptr;
		return;
	}

	if (!ResolveInputActions())
	{
		return;
	}

	// A CONTEXT OF OUR OWN, outered to this subsystem, never an asset. There is no IMC_TracePad.uasset
	// and there should not be: ApplyControlSettings' own comment explains that mapping a loaded asset
	// dirties it and shares it between local players, and this context is rebuilt on every settings
	// change for exactly the same reason its keyboard twin is.
	if (PadContext == nullptr)
	{
		PadContext = NewObject<UInputMappingContext>(this, TEXT("IMC_TracePad"));
	}

	// Torn down and rebuilt wholesale rather than edited in place — the same argument
	// ApplyControlSettings makes: a context with twenty mappings costs nothing to rebuild and the
	// result cannot be half-applied.
	PadContext->UnmapAll();

	auto Action = [this](const TCHAR* Name) -> UInputAction*
	{
		TObjectPtr<UInputAction>* Found = Actions.Find(FName(Name));
		return (Found != nullptr) ? Found->Get() : nullptr;
	};

	// ---- MOVE: the left stick, one Axis2D mapping ------------------------------------------------
	//
	// ONE MAPPING, WHERE THE KEYBOARD NEEDS FOUR. Gamepad_Left2D already delivers (X = right,
	// Y = forward), which IS IA_Move's contract ("X = strafe (+right), Y = forward (+forward)"), so
	// there is nothing to swizzle and nothing to negate — the four 1D keys and their modifier stacks
	// in ApplyControlSettings exist only because a keyboard key is one number.
	//
	// NO ScaleByDeltaTime HERE, and the asymmetry with LOOK below is the whole point of the pair:
	// IA_Move's value is a DIRECTION consumed by the movement component, which does its own
	// per-frame integration. Scaling it by delta time would make the pawn's speed a function of the
	// frame rate — the exact bug ScaleByDeltaTime is on the look mapping to PREVENT, applied to the
	// one axis where it is wrong.
	if (UInputAction* Move = Action(TEXT("IA_Move")))
	{
		FEnhancedActionKeyMapping& Mapping = PadContext->MapKey(Move, MoveStick());
		Mapping.Modifiers.Add(MakeRadialDeadZone(PadContext, Settings.GetPadMoveDeadzone()));
	}

	// ---- LOOK: the right stick -------------------------------------------------------------------
	//
	// THE MODIFIER ORDER IS THE FEATURE. Enhanced Input applies modifiers in list order, and every
	// one of these four is wrong in any other position:
	//
	//   1. DEAD ZONE first, on the RAW stick, because that is the only place the number means what
	//      the settings page says it means — "ignore deflection under 0.20". After a scalar it would
	//      be a threshold on degrees-per-second and would move every time the rate slider did.
	//   2. RESPONSE CURVE second, on the dead-zone's rescaled 0..1, so the curve spends its soft half
	//      on the part of the stick's travel the player's fingers actually use.
	//   3. SCALAR third: 0..1 of curve output becomes degrees per second, and the SIGN of Y is the
	//      invert setting (GetPadLookRateY), exactly as it is for the mouse.
	//   4. SCALE BY DELTA TIME last, and this is the one that is not optional. A stick is a RATE and a
	//      mouse is a DELTA: the mouse reports how far it moved this frame and is therefore already
	//      per-frame, while a held stick reports the same 1.0 every frame forever. Without this the
	//      turn speed is "220 degrees per FRAME", i.e. a function of frame rate — 13000 deg/s at
	//      60 fps — and ATracePlayerController::OnLookInput's spike guard would drop every sample as
	//      implausible, which reads as the stick doing nothing at all.
	if (UInputAction* Look = Action(TEXT("IA_Look")))
	{
		FEnhancedActionKeyMapping& Mapping = PadContext->MapKey(Look, LookStick());
		Mapping.Modifiers.Add(MakeRadialDeadZone(PadContext, Settings.GetPadLookDeadzone()));
		Mapping.Modifiers.Add(MakeSquareCurve(PadContext));
		Mapping.Modifiers.Add(MakeScalar(PadContext, Settings.GetPadLookRateX(), Settings.GetPadLookRateY()));
		Mapping.Modifiers.Add(NewObject<UInputModifierScaleByDeltaTime>(PadContext));
	}

	// ---- The buttons -----------------------------------------------------------------------------
	//
	// One row per action, read out of the player's pad table. An action they have deliberately
	// cleared gets NO mapping rather than a dead one — the same rule ApplyControlSettings states for
	// the keyboard, and the reason ClearPadKey exists as a separate entry point from SetPadKey.
	//
	// THE ACTION NAMES ARE THE ASSET NAMES, and the one row where the two vocabularies differ is
	// EquipGun / IA_EquipGun, whose ConfigId has been "PistolSlot" since spec v31 §1. That is why
	// this is a table and not a loop over TraceInputActions::All() with a string built from the enum.
	struct FPadButton
	{
		ETraceInputAction Bind;
		const TCHAR* ActionName;
	};

	static const FPadButton Buttons[] =
	{
		{ ETraceInputAction::Jump,             TEXT("IA_Jump")             },
		{ ETraceInputAction::Crouch,           TEXT("IA_Crouch")           },
		{ ETraceInputAction::Dash,             TEXT("IA_Dash")             },
		{ ETraceInputAction::Fire,             TEXT("IA_Fire")             },
		{ ETraceInputAction::Pass,             TEXT("IA_Pass")             },
		{ ETraceInputAction::Parry,            TEXT("IA_Parry")            },
		{ ETraceInputAction::Melee,            TEXT("IA_Melee")            },
		{ ETraceInputAction::PullCore,         TEXT("IA_PullCore")         },
		{ ETraceInputAction::Ability,          TEXT("IA_Ability")          },
		{ ETraceInputAction::AbilitySecondary, TEXT("IA_AbilitySecondary") },
		{ ETraceInputAction::Reload,           TEXT("IA_Reload")           },
		{ ETraceInputAction::Scoreboard,       TEXT("IA_Scoreboard")       },
		{ ETraceInputAction::EquipGun,         TEXT("IA_EquipGun")         },
		{ ETraceInputAction::EquipSmg,         TEXT("IA_EquipSmg")         },
		{ ETraceInputAction::EquipKnife,       TEXT("IA_EquipKnife")       },
		{ ETraceInputAction::Inspect,          TEXT("IA_Inspect")          },
	};

	// The four MOVE rows are absent on purpose and are not an omission: movement on a pad is the left
	// stick above. static_assert would be the honest guard but ETraceInputAction has no "is this a
	// move row" predicate to assert against, so the count is stated instead and Trace.Pad.Verify
	// checks that every action carrying a default pad button appears in this table.
	static_assert(UE_ARRAY_COUNT(Buttons) == static_cast<int32>(ETraceInputAction::Count) - 4,
		"Every action except the four keyboard MOVE rows needs a row in the pad button table, or its "
		"bind will draw on the controller page and map to nothing.");

	int32 Mapped = 0;
	for (const FPadButton& Button : Buttons)
	{
		const FKey Key = Settings.GetPadKey(Button.Bind);
		UInputAction* const Target = Action(Button.ActionName);
		if (!Key.IsValid() || Target == nullptr)
		{
			continue;
		}

		PadContext->MapKey(Target, Key);
		++Mapped;
	}

	// ---- Apply -----------------------------------------------------------------------------------
	//
	// Removed and re-added rather than left in place, because the object the subsystem holds is the
	// same one we just rewrote and Enhanced Input caches the resolved key->action table off it.
	// RequestRebuildControlMappings alone would be enough today, and the pair is cheap insurance
	// against a rebuild that keeps a stale copy.
	if (Input->HasMappingContext(PadContext))
	{
		Input->RemoveMappingContext(PadContext);
	}
	Input->AddMappingContext(PadContext, PadMappingPriority);
	Input->RequestRebuildControlMappings();

	// The same check ATracePlayerController::AddInputMappings makes and for the same reason:
	// AddMappingContext is a SILENT no-op when PlayerInput is not a UEnhancedPlayerInput, and a
	// project that overrides DefaultPlayerInputClass hits exactly that with no error anywhere.
	const bool bAccepted = Input->HasMappingContext(PadContext);
	AppliedTo = bAccepted ? LocalPlayer : nullptr;

	if (!bAccepted)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[Pad] Enhanced Input refused the controller mapping context — no gamepad input will ")
			TEXT("reach the pawn. PlayerInput is currently '%s'."),
			(PC->PlayerInput != nullptr) ? *PC->PlayerInput->GetClass()->GetName() : TEXT("<none>"));
		return;
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[Pad] Controller mappings applied: %d button(s) + 2 stick(s) at priority %d. ")
		TEXT("Look %.0f deg/s (pitch %.0f%s), dead zones look %.2f / move %.2f."),
		Mapped, PadMappingPriority,
		Settings.GetPadLookRateX(), FMath::Abs(Settings.GetPadLookRateY()),
		Settings.bPadInvertLookY ? TEXT(", INVERTED") : TEXT(""),
		Settings.GetPadLookDeadzone(), Settings.GetPadMoveDeadzone());
}

// =================================================================================================
// Tick
// =================================================================================================

bool UTraceGamepadInputSubsystem::Tick(float /*DeltaSeconds*/)
{
	APlayerController* const PC = LocalController();
	if (PC == nullptr)
	{
		return true;
	}

	ULocalPlayer* const LocalPlayer = PC->GetLocalPlayer();
	if (LocalPlayer == nullptr)
	{
		return true;
	}

	// ---- Keep the context applied ----------------------------------------------------------------
	//
	// AN IDEMPOTENT CHECK EVERY FRAME, NOT A ONE-SHOT AT STARTUP, and this is load-bearing rather
	// than defensive. A travel destroys the player controller and its UPlayerInput; the local player
	// survives, but every mapping context that was applied to the old PlayerInput is gone with it,
	// and there is no event to hang a re-apply on. ATracePlayerController solves the same problem by
	// calling AddInputMappings from BeginPlay every time; this subsystem has no BeginPlay, so it asks.
	if (UEnhancedInputLocalPlayerSubsystem* const Input =
			LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
	{
		const bool bWantContext = UTraceUserSettings::Get().bPadEnabled;
		const bool bHaveContext = (PadContext != nullptr) && Input->HasMappingContext(PadContext);

		if (bWantContext != bHaveContext || AppliedTo.Get() != LocalPlayer)
		{
			ApplyPadSettings();
		}
	}

	TickMenuButton(PC);

	// ---- The readout -----------------------------------------------------------------------------
	//
	// Latched, never cleared, and never a gate — see the header. It answers exactly one question for
	// the controller settings page: "has this machine ever seen a pad". A player who has paired
	// nothing and a player whose pad is not working need different advice, and without this line the
	// page cannot tell them apart.
	//
	// AnyKey is the cheap way to ask "did anything at all arrive", but it is a KEYBOARD-inclusive
	// wildcard, so the pad's own keys are polled instead: none of them can be produced by a keyboard.
	//
	// *** THE LIST IS COMPLETE, AND IT WAS NOT. *** It used to stop after D-pad UP and DOWN, with a
	// comment claiming the probes "cover every pad a player will pick up". D32-PADMENU disproved that
	// with a screenshot: a run that pressed only D-pad LEFT — a perfectly ordinary thing to do on a
	// two-plate screen whose choices are side by side — moved the highlight and still reported no
	// controller, so the team screen drew no pad captions for a player who was visibly using one.
	//
	// LEFT and RIGHT are now here, and so are the eight DIGITAL STICK keys. The sticks were thought to
	// be covered by the analog test below, and for a real pad they are; but the synthetic path — and
	// any platform layer that reports a flick as its digital key without an analog sample — produced
	// exactly the same silent miss.
	if (!bSeenGamepadInput)
	{
		static const FKey Probes[] =
		{
			EKeys::Gamepad_FaceButton_Bottom, EKeys::Gamepad_FaceButton_Right,
			EKeys::Gamepad_FaceButton_Left,   EKeys::Gamepad_FaceButton_Top,
			EKeys::Gamepad_Special_Left,      EKeys::Gamepad_Special_Right,
			EKeys::Gamepad_LeftShoulder,      EKeys::Gamepad_RightShoulder,
			EKeys::Gamepad_LeftTrigger,       EKeys::Gamepad_RightTrigger,
			EKeys::Gamepad_LeftThumbstick,    EKeys::Gamepad_RightThumbstick,
			EKeys::Gamepad_DPad_Up,           EKeys::Gamepad_DPad_Down,
			EKeys::Gamepad_DPad_Left,         EKeys::Gamepad_DPad_Right,
			EKeys::Gamepad_LeftStick_Up,      EKeys::Gamepad_LeftStick_Down,
			EKeys::Gamepad_LeftStick_Left,    EKeys::Gamepad_LeftStick_Right,
			EKeys::Gamepad_RightStick_Up,     EKeys::Gamepad_RightStick_Down,
			EKeys::Gamepad_RightStick_Left,   EKeys::Gamepad_RightStick_Right,
		};

		for (const FKey& Probe : Probes)
		{
			if (PC->IsInputKeyDown(Probe))
			{
				bSeenGamepadInput = true;
				break;
			}
		}

		// The sticks, which is how most players' first pad input arrives — nobody presses a button
		// before they have moved a stick. A threshold well above any dead zone, so a drifting stick on
		// a pad that IS connected still counts and a numerically noisy zero does not.
		if (!bSeenGamepadInput
			&& (FMath::Abs(PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX))  > 0.5f
			 || FMath::Abs(PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY))  > 0.5f
			 || FMath::Abs(PC->GetInputAnalogKeyState(EKeys::Gamepad_RightX)) > 0.5f
			 || FMath::Abs(PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY)) > 0.5f))
		{
			bSeenGamepadInput = true;
		}

		if (bSeenGamepadInput)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[Pad] A controller has been seen. Nothing changed as a result — the mappings were ")
				TEXT("already live and the keyboard still is; this line exists so the settings page can ")
				TEXT("say so."));
		}
	}

	return true;   // keep ticking
}

void UTraceGamepadInputSubsystem::TickMenuButton(APlayerController* PC)
{
	// *** MENU/START -> ESCAPE. See the header for why Escape is the one key it is safe to
	// synthesise: UTraceUserSettings::IsBindableKey refuses it explicitly, so no gameplay action can
	// ever be sitting on it, and the only three readers are the pause poll in ATraceHUD, the options
	// overlay's own cancel and the title screen's cancel bind. ***
	//
	// A REMEMBERED DOWN STATE RATHER THAN WasInputKeyJustPressed. This ticker runs from
	// FTSTicker::GetCoreTicker, which is not the player controller's tick, so the "just pressed" flag
	// may have been raised and cleared between two of our samples. A rising edge computed from our
	// own previous sample cannot miss one, and cannot fire twice for one press either.
	const bool bDown = PC->IsInputKeyDown(EKeys::Gamepad_Special_Right);
	const bool bRisingEdge = bDown && !bMenuButtonWasDown;
	bMenuButtonWasDown = bDown;

	if (!bRisingEdge || !UTraceUserSettings::Get().bPadEnabled)
	{
		return;
	}

	// Straight into APlayerController::InputKey, which is where UGameViewportClient::InputKey ends up
	// anyway. Going in at the viewport would add its fullscreen toggle, its console gate and its
	// input-device-to-local-player lookup to a press that has already been resolved to this
	// controller — three ways for the button to do nothing, for no gain.
	// INTERNAL ID 0, which is what Debug/TraceInputHarness.cpp measured desktop platforms to map the
	// keyboard and mouse to. The harness has to work harder — it walks ids 0..7 asking
	// GEngine->GetLocalPlayerFromInputDevice — because it enters at UGameViewportClient::InputKey,
	// which uses the device id to FIND a local player. We already have the controller, and
	// APlayerController::InputKey routes by nothing but itself.
	const FInputDeviceId Device = FInputDeviceId::CreateFromInternalId(0);

	const FInputKeyEventArgs Pressed(
		/*Viewport*/ nullptr, Device, EKeys::Escape, IE_Pressed,
		/*AmountDepressed*/ 1.f, /*bIsTouchEvent*/ false, FPlatformTime::Cycles64());

	const FInputKeyEventArgs Released(
		/*Viewport*/ nullptr, Device, EKeys::Escape, IE_Released,
		/*AmountDepressed*/ 0.f, /*bIsTouchEvent*/ false, FPlatformTime::Cycles64());

	// PRESS AND RELEASE IN ONE GO. Everything that reads Escape reads the PRESS edge
	// (WasInputKeyJustPressed), and a press with no release would leave the key latched down —
	// so a player holding MENU would have Escape stuck for the rest of the match.
	PC->InputKey(Pressed);
	PC->InputKey(Released);

	UE_LOG(LogTraceGame, Verbose, TEXT("[Pad] MENU/START -> Escape."));
}

UInputAction* UTraceGamepadInputSubsystem::FindResolvedAction(FName ActionName) const
{
	const TObjectPtr<UInputAction>* Found = Actions.Find(ActionName);
	return (Found != nullptr) ? Found->Get() : nullptr;
}

#if !UE_BUILD_SHIPPING

// =================================================================================================
// D31-PAD — verification
//
// Compiled out of Shipping, exactly like Debug/TraceInputHarness.cpp and for the same reason: these
// commands inject synthetic input and mutate the player's settings, and neither belongs in a build
// a player can run.
//
// *** WHAT EACH ONE PROVES, AND WHAT IT DOES NOT. ***
//
//   Trace.Pad.Status   Describes. Proves nothing; it is the thing you read when a check fails.
//   Trace.Pad.Verify   STATIC. The shipped layout has no duplicate buttons, every default passes
//                      IsBindablePadKey, no pad button has leaked into the keyboard table (and no
//                      keyboard key into the pad table), a rebind survives the .ini round trip, and
//                      the built context maps what the tables say. Runs without a pawn.
//   Trace.Pad.Drive    LIVE. Injects synthetic stick and button samples through the real input
//                      pipeline and reads the result back out of Enhanced Input and out of the
//                      player's control rotation. THREE OF ITS SIX ARMS ARE NEGATIVE CONTROLS —
//                      a stick inside the dead zone, a stick with the pad switched off, and a button
//                      that was released — because an arm that cannot fail proves nothing.
//
// A NAMED namespace, not an anonymous one — see the note at the top of this file.
// =================================================================================================

namespace TraceGamepadVerify
{
	int32 GFailures = 0;

	void Check(bool bCondition, const TCHAR* What, const FString& Detail)
	{
		if (bCondition)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[Pad]   ok       %s%s%s"),
				What, Detail.IsEmpty() ? TEXT("") : TEXT(" — "), *Detail);
		}
		else
		{
			++GFailures;
			UE_LOG(LogTraceGame, Error, TEXT("[Pad]   WRONG    %s%s%s"),
				What, Detail.IsEmpty() ? TEXT("") : TEXT(" — "), *Detail);
		}
	}

	/** Injects one analog sample for @p Key. Mirrors Debug/TraceInputHarness.cpp's InjectAxis. */
	bool InjectAxis(APlayerController* PC, const FKey& Key, float Value)
	{
		if (PC == nullptr)
		{
			return false;
		}

		FViewport* const Viewport =
			(GEngine != nullptr && GEngine->GameViewport != nullptr) ? GEngine->GameViewport->Viewport : nullptr;

		// NumSamples must be >= 1: UPlayerInput accumulates (Delta, NumSamples) pairs and a zero
		// sample count makes the axis read as untouched.
		const FInputKeyEventArgs Args(
			Viewport, FInputDeviceId::CreateFromInternalId(0), Key, Value,
			/*DeltaTime*/ 1.f / 60.f, /*NumSamples*/ 1, FPlatformTime::Cycles64());

		// THE VIEWPORT PATH FIRST, because it is the one a real pad takes — FSceneViewport hands the
		// sample to UGameViewportClient::InputAxis, which ends in PlayerController->InputKey. Falling
		// back to the controller skips the viewport's IgnoreInput gate and its device-to-local-player
		// lookup, which is worth having when there is no viewport at all (a -nullrhi run).
		if (GEngine != nullptr && GEngine->GameViewport != nullptr)
		{
			return GEngine->GameViewport->InputAxis(Args);
		}
		return PC->InputKey(Args);
	}

	/**
	 * Sends an explicit ZERO sample on all four stick axes.
	 *
	 * *** THIS IS NOT TIDYING UP, IT IS WHAT MAKES THE LAST TWO ARMS ABLE TO FAIL. ***
	 * UPlayerInput::EvaluateKeyMapState only copies an axis accumulator into RawValue when a sample
	 * arrived that frame (`if (SampleCountAccumulator > 0)`), and for a PAIRED key it copies only the
	 * components that were sampled. So an injected stick value is not "the value this frame", it is
	 * "the value until something else is injected" — and the first run of the keyboard arm below
	 * measured IA_Move at 2.000, which is the keyboard's 1.0 plus a full second of left stick left
	 * over from the arm three phases earlier. That arm would have passed with the keyboard
	 * completely dead.
	 *
	 * A real pad does not behave that way: the platform layer samples every connected pad every
	 * frame, zeros included. This is the synthetic path's artefact, and zeroing is the honest fix.
	 */
	void ZeroSticks(APlayerController* PC);

	bool InjectButton(APlayerController* PC, const FKey& Key, bool bPressed)
	{
		if (PC == nullptr)
		{
			return false;
		}

		FViewport* const Viewport =
			(GEngine != nullptr && GEngine->GameViewport != nullptr) ? GEngine->GameViewport->Viewport : nullptr;

		const FInputKeyEventArgs Args(
			Viewport, FInputDeviceId::CreateFromInternalId(0), Key,
			bPressed ? IE_Pressed : IE_Released, bPressed ? 1.f : 0.f,
			/*bIsTouchEvent*/ false, FPlatformTime::Cycles64());

		if (GEngine != nullptr && GEngine->GameViewport != nullptr)
		{
			return GEngine->GameViewport->InputKey(Args);
		}
		return PC->InputKey(Args);
	}

	void ZeroSticks(APlayerController* PC)
	{
		InjectAxis(PC, EKeys::Gamepad_LeftX,  0.f);
		InjectAxis(PC, EKeys::Gamepad_LeftY,  0.f);
		InjectAxis(PC, EKeys::Gamepad_RightX, 0.f);
		InjectAxis(PC, EKeys::Gamepad_RightY, 0.f);
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Pad.Status
	// ---------------------------------------------------------------------------------------------

	void Status()
	{
		const UTraceUserSettings& Settings = UTraceUserSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("[Pad] STATUS: controller input %s, look %.0f deg/s (pitch %.0f%s), dead zones ")
			TEXT("look %.2f / move %.2f, at defaults=%d."),
			Settings.bPadEnabled ? TEXT("ON") : TEXT("OFF"),
			Settings.GetPadLookRateX(), FMath::Abs(Settings.GetPadLookRateY()),
			Settings.bPadInvertLookY ? TEXT(", INVERTED") : TEXT(""),
			Settings.GetPadLookDeadzone(), Settings.GetPadMoveDeadzone(),
			Settings.IsPadAtDefaults() ? 1 : 0);

		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			const FKey Pad = Settings.GetPadKey(Info.Action);
			const FKey Shipped = (Info.DefaultPadKey != nullptr) ? Info.DefaultPadKey() : FKey();
			if (!Pad.IsValid() && !Shipped.IsValid())
			{
				continue;   // a MOVE row, or the throw. Neither has a pad button by design.
			}

			UE_LOG(LogTraceGame, Display, TEXT("[Pad]   %-22s %-14s (%s)  keyboard: %s"),
				Info.DisplayName, *UTraceUserSettings::DescribePadKey(Pad),
				(Pad == Shipped) ? TEXT("default") : TEXT("REBOUND"),
				*Settings.DescribeBinding(Info.Action));
		}

		UWorld* const World = (GEngine != nullptr) ? GEngine->GetCurrentPlayWorld() : nullptr;
		UTraceGamepadInputSubsystem* const Pad = UTraceGamepadInputSubsystem::Get(World);
		if (Pad == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Pad] STATUS: no gamepad subsystem — there is no game instance yet."));
			return;
		}

		const UInputMappingContext* const Context = Pad->GetPadContext();
		UE_LOG(LogTraceGame, Display,
			TEXT("[Pad] STATUS: context %s with %d mapping(s); a controller has %sbeen seen on this machine."),
			(Context != nullptr) ? TEXT("BUILT") : TEXT("NOT BUILT"),
			(Context != nullptr) ? Context->GetMappings().Num() : 0,
			Pad->HasSeenGamepadInput() ? TEXT("") : TEXT("NOT "));
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Pad.Verify — the static half
	// ---------------------------------------------------------------------------------------------

	void Verify()
	{
		GFailures = 0;
		UTraceUserSettings& Settings = UTraceUserSettings::Get();
		const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();

		UE_LOG(LogTraceGame, Display, TEXT("[Pad] VERIFY: the shipped controller layout and its persistence."));

		// ---- 1. Every shipped pad default is a real, bindable pad button -------------------------
		{
			int32 WithDefault = 0;
			int32 Bad = 0;
			for (const FTraceInputActionInfo& Info : Table)
			{
				const FKey Key = (Info.DefaultPadKey != nullptr) ? Info.DefaultPadKey() : FKey();
				if (!Key.IsValid())
				{
					continue;
				}
				++WithDefault;
				if (!UTraceUserSettings::IsBindablePadKey(Key))
				{
					++Bad;
					UE_LOG(LogTraceGame, Error, TEXT("[Pad]   %s ships on '%s', which IsBindablePadKey refuses."),
						Info.DisplayName, *Key.ToString());
				}
			}
			Check(Bad == 0 && WithDefault == static_cast<int32>(ETraceInputAction::Count) - 5,
				TEXT("every shipped pad default is a bindable pad button"),
				FString::Printf(TEXT("%d action(s) carry one; the five without are the four MOVE rows "
					"and THROW / PASS CORE, all three by design"), WithDefault));
		}

		// ---- 2. No two actions ship on the same button -------------------------------------------
		//
		// The layout comment claims "nothing here collides". This is the check that makes that a fact
		// rather than a claim, and it is exactly the check that would have caught L3 being given to
		// both CROUCH and a second verb while the comment still said otherwise.
		{
			TMap<FKey, const TCHAR*> Seen;
			int32 Collisions = 0;
			for (const FTraceInputActionInfo& Info : Table)
			{
				const FKey Key = (Info.DefaultPadKey != nullptr) ? Info.DefaultPadKey() : FKey();
				if (!Key.IsValid())
				{
					continue;
				}
				if (const TCHAR** Prior = Seen.Find(Key))
				{
					++Collisions;
					UE_LOG(LogTraceGame, Error, TEXT("[Pad]   '%s' is on BOTH %s and %s."),
						*UTraceUserSettings::DescribePadKey(Key), *Prior, Info.DisplayName);
				}
				else
				{
					Seen.Add(Key, Info.DisplayName);
				}
			}
			Check(Collisions == 0, TEXT("no two actions ship on one pad button"),
				FString::Printf(TEXT("%d distinct buttons used"), Seen.Num()));
		}

		// ---- 3. MENU/START is left unclaimed -----------------------------------------------------
		//
		// Not tidiness: TickMenuButton turns that button into the pause key, and an action bound to it
		// as well would fire in the same instant the menu opened.
		{
			bool bClaimed = false;
			for (const FTraceInputActionInfo& Info : Table)
			{
				const FKey Key = (Info.DefaultPadKey != nullptr) ? Info.DefaultPadKey() : FKey();
				bClaimed = bClaimed || (Key == EKeys::Gamepad_Special_Right);
			}
			Check(!bClaimed, TEXT("MENU/START is unclaimed, so it can be the pause button"), FString());
		}

		// ---- 4. The two PAGES partition the key space --------------------------------------------
		//
		// This is the item that stops one physical button from being entered into both tables, which
		// would map it in both contexts at once and leave one of the two rows silently dead. Asserted
		// in BOTH directions, and with the ACCEPT half as well as the REFUSE half, so it cannot pass
		// by refusing everything.
		{
			// The keybind page's filter. IsBindableKey — which the .ini loader uses — still accepts a
			// pad button, and that difference is the whole point: a saved line is honoured, a new
			// binding cannot be created. Both halves are checked.
			const bool bKeyboardPageRefusesPad =
				!UTraceUserSettings::IsBindableKeyboardKey(EKeys::Gamepad_FaceButton_Bottom)
				&& !UTraceUserSettings::IsBindableKeyboardKey(EKeys::Gamepad_RightTrigger);

			const bool bKeyboardPageAcceptsKeyboard =
				UTraceUserSettings::IsBindableKeyboardKey(EKeys::SpaceBar)
				&& UTraceUserSettings::IsBindableKeyboardKey(EKeys::LeftMouseButton);

			const bool bLoaderStillAcceptsPad =
				UTraceUserSettings::IsBindableKey(EKeys::Gamepad_FaceButton_Bottom);

			Check(bKeyboardPageRefusesPad && bKeyboardPageAcceptsKeyboard && bLoaderStillAcceptsPad,
				TEXT("the KEYBIND page takes keyboard and mouse only, and the loader still takes both"),
				FString::Printf(TEXT("page refuses pad=%d, page accepts keyboard=%d, ")
					TEXT("loader still honours a saved pad line=%d"),
					bKeyboardPageRefusesPad ? 1 : 0, bKeyboardPageAcceptsKeyboard ? 1 : 0,
					bLoaderStillAcceptsPad ? 1 : 0));

			const bool bPadRefusesKeyboard =
				!UTraceUserSettings::IsBindablePadKey(EKeys::SpaceBar)
				&& !UTraceUserSettings::IsBindablePadKey(EKeys::LeftMouseButton)
				&& !UTraceUserSettings::IsBindablePadKey(EKeys::Escape);

			// The sticks must be refused too, or a player could bind JUMP to "push the stick forward"
			// as an AXIS and jump for as long as they walked.
			const bool bPadRefusesAxes =
				!UTraceUserSettings::IsBindablePadKey(EKeys::Gamepad_LeftX)
				&& !UTraceUserSettings::IsBindablePadKey(EKeys::Gamepad_Right2D);

			// ...but the DIGITAL stick keys are buttons and must stay bindable, or "flick the stick" is
			// unavailable. Both halves are asserted, so this cannot pass by refusing everything.
			const bool bPadAcceptsButtons =
				UTraceUserSettings::IsBindablePadKey(EKeys::Gamepad_FaceButton_Bottom)
				&& UTraceUserSettings::IsBindablePadKey(EKeys::Gamepad_RightTrigger)
				&& UTraceUserSettings::IsBindablePadKey(EKeys::Gamepad_LeftStick_Up);

			Check(bPadRefusesKeyboard && bPadRefusesAxes && bPadAcceptsButtons,
				TEXT("the CONTROLLER page takes pad buttons only"),
				FString::Printf(TEXT("keyboard refused=%d, stick axes refused=%d, pad buttons accepted=%d"),
					bPadRefusesKeyboard ? 1 : 0, bPadRefusesAxes ? 1 : 0, bPadAcceptsButtons ? 1 : 0));
		}

		// ---- 5. A rebind survives the .ini round trip ---------------------------------------------
		//
		// The owner's requirement in as many words: "Rebinding must persist like the keyboard binds
		// do." Persisting means SURVIVING THE PARSE, so the value is written, the raw config lines are
		// re-read, and the table is rebuilt from them — not merely stored in memory.
		{
			const FKey Before = Settings.GetPadKey(ETraceInputAction::Jump);
			const FKey Probe = EKeys::Gamepad_DPad_Down;   // shipped on INSPECT, so this also steals

			Settings.SetPadKey(ETraceInputAction::Jump, Probe);
			const bool bTookIt = (Settings.GetPadKey(ETraceInputAction::Jump) == Probe);

			// JUMP and INSPECT are both live in the same states, so the steal must have happened.
			const bool bStole = !Settings.GetPadKey(ETraceInputAction::Inspect).IsValid();

			Settings.RefreshPadFromConfig();
			const bool bSurvived = (Settings.GetPadKey(ETraceInputAction::Jump) == Probe);

			Check(bTookIt && bSurvived, TEXT("a pad rebind is written, re-read and honoured"),
				FString::Printf(TEXT("set=%d, survived a reload from PadKeyBindings=%d"),
					bTookIt ? 1 : 0, bSurvived ? 1 : 0));
			Check(bStole, TEXT("the rebind took the button from the action that had it"),
				TEXT("INSPECT is left visibly UNBOUND rather than silently sharing D-PAD DOWN"));

			// ---- 5b. THE KEYBOARD IS UNTOUCHED, which is the whole reason for two tables ----------
			Check(Settings.GetKey(ETraceInputAction::Jump) == EKeys::SpaceBar,
				TEXT("rebinding a PAD button leaves the KEYBOARD bind alone"),
				FString::Printf(TEXT("JUMP is still %s on the keyboard"),
					*UTraceUserSettings::DescribeKey(Settings.GetKey(ETraceInputAction::Jump))));

			// Put it back. Deliberately through ResetPadToDefaults rather than by re-setting the two
			// keys: this also proves the reset row works, and it is the only way to un-steal INSPECT.
			Settings.ResetPadToDefaults();
			Check(Settings.GetPadKey(ETraceInputAction::Jump) == Before
				&& Settings.GetPadKey(ETraceInputAction::Inspect) == EKeys::Gamepad_DPad_Down,
				TEXT("ResetPadToDefaults restored both rows"),
				TEXT("this command has left the player's controller layout as it found it"));
		}

		// ---- 6. ResetToDefaults (the KEYBOARD page's reset) does not touch the pad ----------------
		//
		// The negative half of item 5b, and the one that would silently regress: the two resets sit on
		// different pages and each must own only its own page.
		{
			const FKey PadBefore = Settings.GetPadKey(ETraceInputAction::Fire);
			Settings.SetPadKey(ETraceInputAction::Fire, EKeys::Gamepad_FaceButton_Top);
			const FKey Rebound = Settings.GetPadKey(ETraceInputAction::Fire);

			Settings.ResetToDefaults();   // the CONTROLS page's reset

			Check(Settings.GetPadKey(ETraceInputAction::Fire) == Rebound
				&& Rebound == EKeys::Gamepad_FaceButton_Top,
				TEXT("the keyboard page's RESET leaves the controller page alone"),
				FString::Printf(TEXT("FIRE is still on %s after a full keyboard reset"),
					*UTraceUserSettings::DescribePadKey(Settings.GetPadKey(ETraceInputAction::Fire))));

			Settings.SetPadKey(ETraceInputAction::Fire, PadBefore);
			Settings.ResetPadToDefaults();
		}

		// ---- 7. The live context maps what the tables say -----------------------------------------
		{
			UWorld* const World = (GEngine != nullptr) ? GEngine->GetCurrentPlayWorld() : nullptr;
			UTraceGamepadInputSubsystem* const Pad = UTraceGamepadInputSubsystem::Get(World);
			const UInputMappingContext* const Context = (Pad != nullptr) ? Pad->GetPadContext() : nullptr;

			if (Context == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Pad]   skipped  the context has not been built — run this in a match, not on ")
					TEXT("a title screen with no local player. Items 1-6 above are world-independent."));
			}
			else
			{
				bool bMove = false;
				bool bLook = false;
				int32 Buttons = 0;
				int32 NonPadKeys = 0;

				for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
				{
					bMove = bMove || (Mapping.Key == TraceGamepadInputImpl::MoveStick());
					bLook = bLook || (Mapping.Key == TraceGamepadInputImpl::LookStick());

					if (!Mapping.Key.IsGamepadKey())
					{
						++NonPadKeys;
						UE_LOG(LogTraceGame, Error,
							TEXT("[Pad]   the pad context maps '%s', which is not a pad key at all."),
							*Mapping.Key.ToString());
					}
					else if (Mapping.Key != TraceGamepadInputImpl::MoveStick()
						&& Mapping.Key != TraceGamepadInputImpl::LookStick())
					{
						++Buttons;
					}
				}

				Check(bMove && bLook, TEXT("both sticks are mapped"),
					FString::Printf(TEXT("move=%d look=%d"), bMove ? 1 : 0, bLook ? 1 : 0));

				// THE COUNT IS COMPUTED, NOT LITERAL. A hardcoded 16 would start lying the moment a
				// player unbound one button, and would then have to be "fixed" by weakening the check.
				int32 Expected = 0;
				for (const FTraceInputActionInfo& Info : Table)
				{
					if (Info.Action != ETraceInputAction::MoveForward
						&& Info.Action != ETraceInputAction::MoveBack
						&& Info.Action != ETraceInputAction::MoveLeft
						&& Info.Action != ETraceInputAction::MoveRight
						&& Settings.GetPadKey(Info.Action).IsValid())
					{
						++Expected;
					}
				}

				Check(Buttons == Expected, TEXT("every bound pad button reached the context"),
					FString::Printf(TEXT("%d mapped, %d bound in the table"), Buttons, Expected));
				Check(NonPadKeys == 0, TEXT("the pad context contains no keyboard or mouse key"),
					TEXT("this is what stops a pad bind from stealing a key the mouse context also maps"));
			}
		}

		if (GFailures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[Pad] VERDICT: the shipped controller layout is collision-free, bindable, "
					"persistent across a reload, and independent of the keyboard's own reset. "
					"0 failure(s). This says NOTHING about whether a press moves the pawn — "
					"that is Trace.Pad.Drive."));
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[Pad] VERDICT: %d failure(s)."), GFailures);
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Pad.Drive — the live half
	//
	// *** THIS IS THE ONE THAT ANSWERS "DOES THE MAPPING ACTUALLY DRIVE THE GAME". *** Everything
	// above proves the tables agree with each other; a table can be perfect and map an action object
	// nothing is bound to, which looks exactly like a working feature until somebody picks up a pad.
	//
	// TWO OBSERVABLES PER ARM, ON PURPOSE:
	//   * the ACTION VALUE, read back out of UEnhancedPlayerInput. This proves the synthetic sample
	//     reached the mapping and came out the far side with the modifier stack applied. It does not
	//     depend on a pawn, on being alive, or on the camera.
	//   * the CONTROL ROTATION / PAWN LOCATION. This proves the value reached the GAME. It can fail
	//     for reasons that are not this feature's fault (a pause menu is up, the pawn is dead), so
	//     both are reported and the action value is the one that is asserted hardest.
	//
	// THREE OF THE ARMS ARE NEGATIVE CONTROLS and they are not decoration: a positive arm alone
	// cannot distinguish "the mapping works" from "something else is turning the camera".
	// ---------------------------------------------------------------------------------------------

	struct FDrive : public TSharedFromThis<FDrive>
	{
		enum class EPhase : uint8
		{
			Settle,          // no injection at all — the baseline, and the first negative control
			LookFull,        // stick hard right
			LookDeadZone,    // stick INSIDE the dead zone — negative control
			LookPadOff,      // stick hard right with the pad switched off — negative control
			MoveFull,        // stick hard forward
			ButtonHeld,      // A held down
			ButtonReleased,  // A released — negative control
			KeyboardMove,    // the KEYBOARD's move key, with the pad context still applied
			MouseLook,       // the MOUSE, with the pad context still applied
			Done,
		};

		static TSharedPtr<FDrive> Instance;

		EPhase Phase = EPhase::Settle;
		float PhaseSeconds = 0.f;
		bool bPhaseStarted = false;

		/**
		 * *** WHY EVERY PHASE THROWS AWAY ITS FIRST FEW FRAMES. ***
		 *
		 * This ticker runs from FTSTicker::GetCoreTicker, which fires EARLY in the engine's frame —
		 * before the player controller ticks and therefore before UPlayerInput has consumed the
		 * sample injected below. So on the first tick of a phase, the action value read back is still
		 * the PREVIOUS phase's: measured, the dead-zone arm's first sample carried 8.7 degrees left
		 * over from the full-deflection arm before it, and the released-button arm's first sample
		 * still said the button was down. Both read as feature bugs and were test bugs.
		 *
		 * A margin rather than a re-order, because the injection genuinely must happen every frame
		 * and there is no ordering of one tick that makes a value appear before the frame that
		 * produced it. 0.15 s is about four frames at the ~25 fps a headless offscreen run manages,
		 * and the phases are long enough that what is left is still most of each one.
		 */
		static constexpr float SettleMargin = 0.15f;

		/** False until PhaseSeconds passes SettleMargin; the snapshot below is taken at that moment. */
		bool bMeasuring = false;
		float MeasuredSeconds = 0.f;

		float StartYaw = 0.f;
		FVector StartLocation = FVector::ZeroVector;
		float PeakActionX = 0.f;
		float PeakActionY = 0.f;
		bool bSawActionTrue = false;

		bool bRestorePadEnabled = true;
		FTSTicker::FDelegateHandle Handle;

		static float DurationOf(EPhase InPhase)
		{
			switch (InPhase)
			{
			// A FULL SECOND OF SETTLE, and it is not padding: ATracePlayerController::OnLookInput
			// refuses every look event until IgnoreLookUntilTime, the window it opens after a spawn
			// or a mouse capture. Driving inside that window would fail for a reason that has nothing
			// to do with the pad.
			case EPhase::Settle:         return 1.00f;
			case EPhase::LookFull:       return 0.65f;
			case EPhase::LookDeadZone:   return 0.65f;
			case EPhase::LookPadOff:     return 0.65f;
			case EPhase::MoveFull:       return 0.75f;
			case EPhase::ButtonHeld:     return 0.40f;
			case EPhase::ButtonReleased: return 0.40f;
			case EPhase::KeyboardMove:   return 0.50f;
			case EPhase::MouseLook:      return 0.50f;
			default:                     return 0.f;
			}
		}

		static void Start()
		{
			Stop();

			GFailures = 0;
			Instance = MakeShared<FDrive>();
			Instance->Handle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateSP(Instance.ToSharedRef(), &FDrive::Tick), 0.f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[Pad] DRIVE: injecting synthetic stick, button, keyboard and mouse samples through ")
				TEXT("the real input pipeline. Eight arms; four of them are negative controls."));
		}

		static void Stop()
		{
			if (Instance.IsValid())
			{
				if (Instance->Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Instance->Handle);
				}
				Instance.Reset();
			}
		}

		bool Tick(float DeltaSeconds)
		{
			APlayerController* const PC = TraceGamepadInputImpl::FindLocalController(
				(GEngine != nullptr && GEngine->GetCurrentPlayWorld() != nullptr)
					? GEngine->GetCurrentPlayWorld()->GetGameInstance() : nullptr);

			UTraceGamepadInputSubsystem* const Pad = UTraceGamepadInputSubsystem::Get(
				(GEngine != nullptr) ? GEngine->GetCurrentPlayWorld() : nullptr);

			if (PC == nullptr || Pad == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Pad] DRIVE: no local player controller yet — nothing to drive. Run this in a ")
					TEXT("match."));
				Finish();
				return false;
			}

			UEnhancedPlayerInput* const PlayerInput = Cast<UEnhancedPlayerInput>(PC->PlayerInput);
			UInputAction* const Look = Pad->FindResolvedAction(FName(TEXT("IA_Look")));
			UInputAction* const Move = Pad->FindResolvedAction(FName(TEXT("IA_Move")));
			UInputAction* const Jump = Pad->FindResolvedAction(FName(TEXT("IA_Jump")));
			UInputAction* const Fire = Pad->FindResolvedAction(FName(TEXT("IA_Fire")));

			if (PlayerInput == nullptr || Look == nullptr || Move == nullptr || Jump == nullptr)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[Pad] DRIVE: the input actions could not be resolved (PlayerInput=%s). ")
					TEXT("Trace.Input.VerifyAssets says why."),
					(PC->PlayerInput != nullptr) ? *PC->PlayerInput->GetClass()->GetName() : TEXT("<none>"));
				++GFailures;
				Finish();
				return false;
			}

			if (!bPhaseStarted)
			{
				BeginPhase(PC, Pad);
			}

			// ---- The injection for this frame ----------------------------------------------------
			const UTraceUserSettings& Settings = UTraceUserSettings::Get();
			switch (Phase)
			{
			case EPhase::LookFull:
			case EPhase::LookPadOff:
				InjectAxis(PC, EKeys::Gamepad_RightX, 1.0f);
				break;

			case EPhase::LookDeadZone:
				// HALF THE DEAD ZONE. Deliberately not zero: a zero sample would prove only that
				// nothing happens when nothing is pushed, which is not the claim. The claim is that a
				// stick which IS deflected — as a worn stick permanently is — produces no turn.
				InjectAxis(PC, EKeys::Gamepad_RightX, Settings.GetPadLookDeadzone() * 0.5f);
				break;

			case EPhase::MoveFull:
				InjectAxis(PC, EKeys::Gamepad_LeftY, 1.0f);
				break;

			case EPhase::KeyboardMove:
				// Every frame, so the pad contributes a real, measured zero rather than whatever it
				// last carried. See ZeroSticks.
				ZeroSticks(PC);
				break;

			case EPhase::MouseLook:
				// *** THE OWNER'S CASE IS A PAD *IN ADDITION TO* A MOUSE. *** So the last two arms
				// drive the OTHER device with the pad's context still applied, which is the only way
				// to show that adding it took nothing away. Ten counts a frame at the shipped 1.5
				// deg/count is 15 degrees — well under OnLookInput's spike threshold.
				ZeroSticks(PC);
				InjectAxis(PC, EKeys::MouseX, 10.0f);
				break;

			default:
				break;
			}

			PhaseSeconds += DeltaSeconds;

			// The snapshot is taken when the margin expires, not when the phase started — see
			// SettleMargin. Everything measured below is measured over MeasuredSeconds.
			if (!bMeasuring && PhaseSeconds >= SettleMargin)
			{
				bMeasuring = true;
				MeasuredSeconds = 0.f;
				PeakActionX = 0.f;
				PeakActionY = 0.f;
				bSawActionTrue = false;
				StartYaw = PC->GetControlRotation().Yaw;
				StartLocation = (PC->GetPawn() != nullptr) ? PC->GetPawn()->GetActorLocation() : FVector::ZeroVector;
			}

			// ---- What came out the far side ------------------------------------------------------
			if (bMeasuring)
			{
				MeasuredSeconds += DeltaSeconds;

				const FVector2D LookValue = PlayerInput->GetActionValue(Look).Get<FVector2D>();
				const FVector2D MoveValue = PlayerInput->GetActionValue(Move).Get<FVector2D>();

				const bool bLookPhase = (Phase == EPhase::LookFull || Phase == EPhase::LookDeadZone
					|| Phase == EPhase::LookPadOff || Phase == EPhase::Settle
					|| Phase == EPhase::MouseLook);

				PeakActionX = FMath::Max(PeakActionX, FMath::Abs(bLookPhase ? LookValue.X : MoveValue.X));
				PeakActionY = FMath::Max(PeakActionY, FMath::Abs(bLookPhase ? LookValue.Y : MoveValue.Y));

				if (Phase == EPhase::KeyboardMove)
				{
					// The keyboard's OWN move key, read out of the player's binding table rather than
					// hardcoded as W: a check that assumed W would fail for a player who had rebound
					// it and would then be "fixed" by weakening it.
					PeakActionY = FMath::Max(PeakActionY, FMath::Abs(MoveValue.Y));
				}

				if (Phase == EPhase::ButtonHeld || Phase == EPhase::ButtonReleased)
				{
					bSawActionTrue = bSawActionTrue || PlayerInput->GetActionValue(Jump).Get<bool>();

					// A SECOND NEGATIVE CONTROL INSIDE THE POSITIVE ARM: pressing A must not fire.
					// Without it, a context that mapped every pad button to every action would pass.
					if (Fire != nullptr && PlayerInput->GetActionValue(Fire).Get<bool>())
					{
						++GFailures;
						UE_LOG(LogTraceGame, Error,
							TEXT("[Pad]   WRONG    holding A also triggered FIRE."));
					}
				}
			}

			if (PhaseSeconds >= DurationOf(Phase))
			{
				EndPhase(PC, Pad);
			}

			if (Phase == EPhase::Done)
			{
				Finish();
				return false;
			}
			return true;
		}

		void BeginPhase(APlayerController* PC, UTraceGamepadInputSubsystem* Pad)
		{
			bPhaseStarted = true;
			bMeasuring = false;
			PhaseSeconds = 0.f;
			MeasuredSeconds = 0.f;
			PeakActionX = 0.f;
			PeakActionY = 0.f;
			bSawActionTrue = false;
			StartYaw = PC->GetControlRotation().Yaw;
			StartLocation = (PC->GetPawn() != nullptr) ? PC->GetPawn()->GetActorLocation() : FVector::ZeroVector;

			if (Phase == EPhase::LookPadOff)
			{
				// THE NEGATIVE CONTROL'S ARM. Removing the CONTEXT, not blanking the table: this is the
				// state the OFF toggle on the settings page actually produces.
				UTraceUserSettings& Settings = UTraceUserSettings::Get();
				bRestorePadEnabled = Settings.bPadEnabled;
				Settings.bPadEnabled = false;
				Pad->ApplyPadSettings();
			}
			else if (Phase == EPhase::ButtonHeld)
			{
				InjectButton(PC, EKeys::Gamepad_FaceButton_Bottom, /*bPressed=*/true);
			}
			else if (Phase == EPhase::ButtonReleased)
			{
				InjectButton(PC, EKeys::Gamepad_FaceButton_Bottom, /*bPressed=*/false);
			}
			else if (Phase == EPhase::KeyboardMove)
			{
				ZeroSticks(PC);
				InjectButton(PC, UTraceUserSettings::Get().GetKey(ETraceInputAction::MoveForward), true);
			}
			else if (Phase == EPhase::MouseLook)
			{
				ZeroSticks(PC);
			}
		}

		void EndPhase(APlayerController* PC, UTraceGamepadInputSubsystem* Pad)
		{
			const UTraceUserSettings& Settings = UTraceUserSettings::Get();
			const float YawDelta = FMath::Abs(FRotator::NormalizeAxis(PC->GetControlRotation().Yaw - StartYaw));
			const float Moved = (PC->GetPawn() != nullptr)
				? FVector::Dist2D(PC->GetPawn()->GetActorLocation(), StartLocation) : 0.f;

			switch (Phase)
			{
			case EPhase::Settle:
				// NEGATIVE CONTROL 1: with nothing injected the view must be still. If this fails,
				// every positive arm below is measuring something other than the pad.
				Check(YawDelta < 1.0f && PeakActionX < UE_KINDA_SMALL_NUMBER,
					TEXT("[control] nothing injected, nothing moves"),
					FString::Printf(TEXT("yaw drifted %.2f deg, peak look action %.4f"), YawDelta, PeakActionX));
				break;

			case EPhase::LookFull:
			{
				// The expected turn: rate x the window actually measured, with the response curve at
				// full deflection = 1. MeasuredSeconds and not DurationOf, or the settle margin would
				// make the prediction 30% high and the tolerance below would be hiding it.
				const float Expected = Settings.GetPadLookRateX() * MeasuredSeconds;
				Check(PeakActionX > UE_KINDA_SMALL_NUMBER,
					TEXT("the right stick reaches IA_Look"),
					FString::Printf(TEXT("peak per-frame value %.3f deg"), PeakActionX));
				Check(YawDelta > Expected * 0.4f,
					TEXT("the right stick turns the view"),
					FString::Printf(TEXT("yaw moved %.1f deg in %.2f s; %.0f deg/s predicts ~%.1f"),
						YawDelta, MeasuredSeconds, Settings.GetPadLookRateX(), Expected));
				break;
			}

			case EPhase::LookDeadZone:
				// NEGATIVE CONTROL 2.
				Check(YawDelta < 1.0f && PeakActionX < UE_KINDA_SMALL_NUMBER,
					TEXT("[control] a stick INSIDE the dead zone turns nothing"),
					FString::Printf(TEXT("held at %.3f (dead zone %.2f): yaw moved %.2f deg, peak action %.4f"),
						Settings.GetPadLookDeadzone() * 0.5f, Settings.GetPadLookDeadzone(), YawDelta, PeakActionX));
				break;

			case EPhase::LookPadOff:
			{
				// NEGATIVE CONTROL 3, and the strongest of the three: the very same full-deflection
				// sample that turned the view two phases ago must now do nothing.
				Check(YawDelta < 1.0f && PeakActionX < UE_KINDA_SMALL_NUMBER,
					TEXT("[control] with CONTROLLER INPUT off, a full stick turns nothing"),
					FString::Printf(TEXT("yaw moved %.2f deg, peak action %.4f"), YawDelta, PeakActionX));

				UTraceUserSettings& Mutable = UTraceUserSettings::Get();
				Mutable.bPadEnabled = bRestorePadEnabled;
				Pad->ApplyPadSettings();
				Check(Pad->GetPadContext() != nullptr,
					TEXT("the pad context was rebuilt after the off arm"),
					TEXT("this command has left controller input as it found it"));
				break;
			}

			case EPhase::MoveFull:
				Check(PeakActionY > 0.5f,
					TEXT("the left stick reaches IA_Move"),
					FString::Printf(TEXT("peak forward value %.3f"), PeakActionY));
				// The pawn's own displacement is REPORTED and not asserted: it depends on being alive,
				// unblocked and not stood against a wall, none of which is this feature's business.
				UE_LOG(LogTraceGame, Display, TEXT("[Pad]   note     the pawn moved %.0f uu while the stick was forward."), Moved);
				break;

			case EPhase::ButtonHeld:
				Check(bSawActionTrue, TEXT("the A button reaches IA_Jump"),
					TEXT("read back from UEnhancedPlayerInput while the button was held"));
				break;

			case EPhase::ButtonReleased:
				// NEGATIVE CONTROL 4: the action must come back up. A mapping that latched would make
				// every arm above pass and would weld the player's jump down.
				Check(!bSawActionTrue, TEXT("[control] IA_Jump is false once A is released"), FString());
				break;

			// ---- The two arms that answer "does the pad break the keyboard and mouse" -------------
			//
			// THESE ARE THE HOT-PLUG PROOF, and they are the reason the arms above are not the whole
			// command. Everything before this shows a pad driving the game; the owner's actual
			// question is whether the OTHER device still does while it is. Both run with the pad's
			// mapping context applied and unmodified.
			case EPhase::KeyboardMove:
			{
				const FKey MoveKey = Settings.GetKey(ETraceInputAction::MoveForward);

				// BOUNDED ABOVE AS WELL AS BELOW. IA_Move accumulates Cumulatively, so a stick still
				// reading forward would add its own 1.0 and the lower bound alone would pass with the
				// keyboard dead — which is exactly what the first run of this arm did (2.000). The
				// sticks are held at a measured zero for the whole phase, so 1.0 is the keyboard's
				// and nothing else's.
				Check(PeakActionY > 0.5f && PeakActionY < 1.5f,
					TEXT("the KEYBOARD still moves while the pad's context is applied"),
					FString::Printf(TEXT("held '%s' with both sticks held at zero: IA_Move forward ")
						TEXT("peaked at %.3f (1.0 is the keyboard alone)"),
						*UTraceUserSettings::DescribeKey(MoveKey), PeakActionY));
				InjectButton(PC, MoveKey, /*bPressed=*/false);
				break;
			}

			case EPhase::MouseLook:
				// The sticks are held at a measured zero here too, so the turn is the mouse's. Without
				// that, the right stick's last sample from the pad-off arm would still be in
				// Gamepad_Right2D and this would pass with the mouse dead.
				Check(YawDelta > 5.f,
					TEXT("the MOUSE still looks while the pad's context is applied"),
					FString::Printf(TEXT("yaw moved %.1f deg over %.2f s of synthetic mouse motion, ")
						TEXT("with both sticks held at zero"), YawDelta, MeasuredSeconds));
				break;

			default:
				break;
			}

			Phase = static_cast<EPhase>(static_cast<uint8>(Phase) + 1);
			bPhaseStarted = false;
		}

		void Finish()
		{
			if (GFailures == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[Pad] DRIVE VERDICT: synthetic gamepad input reaches the mapping context, comes ")
					TEXT("out with the dead zone and the rate applied, and turns the player's view; it does ")
					TEXT("none of that inside the dead zone, with the pad switched off, or after the button ")
					TEXT("is released; and the keyboard and mouse still drive the game while the pad's ")
					TEXT("context is applied. 0 failure(s)."));
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[Pad] DRIVE VERDICT: %d failure(s)."), GFailures);
			}

			// Not Stop(): we are inside our own tick, and removing the ticker that is running is what
			// the `return false` from Tick already does. Only the shared pointer has to go.
			if (Instance.IsValid())
			{
				Instance->Handle.Reset();
				Instance.Reset();
			}
		}
	};

	TSharedPtr<FDrive> FDrive::Instance;

	// ---------------------------------------------------------------------------------------------
	// D32-PADMENU — Trace.Pad.Menu, the synthetic THUMB
	// ---------------------------------------------------------------------------------------------
	//
	// A scripted sequence of pad button presses, spaced over real frames.
	//
	// *** WHY A SEQUENCER AND NOT ONE COMMAND PER PRESS. ***
	// A press is only observable on the NEXT UPlayerInput::ProcessInputStack: UPlayerInput::InputKey
	// writes an EventAccumulator, and bDown / EventCounts — the two things WasInputKeyJustPressed and
	// IsInputKeyDown actually read — are filled from it when the player controller ticks. So a press
	// and its release inside one callback are a press no screen can see. And -TraceExec fires its
	// whole command list in ONE callback (see UI/TraceAutoShot.h), so "down, down, a" as three
	// console commands is three presses in one frame, i.e. zero presses.
	//
	// Each step therefore holds its button for HoldSeconds — long enough for several player-input
	// ticks, and SHORTER than TracePadMenu::RepeatDelay so one step is one move and never two — then
	// releases it and waits GapSeconds before the next.
	//
	// EVERY SCREEN THIS DRIVES IS DRIVEN THROUGH ITS OWN POLLING. Nothing here calls MoveSelection,
	// ActivateSelection, Confirm or a Server RPC. The only thing injected is a BUTTON, at
	// UGameViewportClient::InputKey — the very call FSceneViewport makes for a physical pad — so a
	// run that passes with the screen's polling disconnected is not a run this can produce.

	struct FPadMenuScript : TSharedFromThis<FPadMenuScript>
	{
		static TSharedPtr<FPadMenuScript> Instance;

		/** Long enough for several player-input ticks; shorter than TracePadMenu::RepeatDelay. */
		static constexpr float HoldSeconds = 0.20f;
		static constexpr float GapSeconds = 0.24f;

		/**
		 * WHERE THE CURRENT STEP IS. AN EXPLICIT STATE MACHINE, and the first version was not.
		 *
		 * That version inferred the state from an elapsed-time comparison and a bPressed flag, and it
		 * PRESSED THE SAME BUTTON FOREVER: after the release cleared the flag, the very next tick read
		 * "not pressed" and pressed again, so the step only advanced on a frame long enough to skip
		 * the whole hold-and-gap window in one go. Measured in the first title-screen run — the log
		 * shows `1/4 press / 1/4 release` eighteen times, and the index only moved when a screenshot
		 * flush stalled one frame for 1.1 s. Three named states cannot express that bug.
		 */
		enum class EStep : uint8
		{
			Press,     // inject the down edge on this tick
			Hold,      // wait out HoldSeconds, then inject the up edge
			Gap,       // wait out GapSeconds, then move to the next token
		};

		TArray<FKey> Keys;
		TArray<FString> Names;

		int32 Index = 0;
		float StepSeconds = 0.f;
		EStep Step = EStep::Press;
		FTSTicker::FDelegateHandle Handle;

		/** "down" -> Gamepad_DPad_Down. Returns an invalid key for a token that is not a button. */
		static FKey KeyForToken(const FString& Token)
		{
			const FString T = Token.ToLower();
			if (T == TEXT("up"))      { return EKeys::Gamepad_DPad_Up; }
			if (T == TEXT("down"))    { return EKeys::Gamepad_DPad_Down; }
			if (T == TEXT("left"))    { return EKeys::Gamepad_DPad_Left; }
			if (T == TEXT("right"))   { return EKeys::Gamepad_DPad_Right; }
			if (T == TEXT("lup"))     { return EKeys::Gamepad_LeftStick_Up; }
			if (T == TEXT("ldown"))   { return EKeys::Gamepad_LeftStick_Down; }
			if (T == TEXT("lleft"))   { return EKeys::Gamepad_LeftStick_Left; }
			if (T == TEXT("lright"))  { return EKeys::Gamepad_LeftStick_Right; }
			if (T == TEXT("a"))       { return TracePadMenu::ConfirmKey(); }
			if (T == TEXT("b"))       { return TracePadMenu::BackKey(); }
			if (T == TEXT("x"))       { return TracePadMenu::AltKey(); }
			if (T == TEXT("y"))       { return EKeys::Gamepad_FaceButton_Top; }
			if (T == TEXT("menu"))    { return EKeys::Gamepad_Special_Right; }
			if (T == TEXT("view"))    { return EKeys::Gamepad_Special_Left; }
			// "wait" is a deliberate no-key step: it is how a script gives a screen time to travel,
			// replicate or close between two presses.
			if (T == TEXT("wait"))    { return FKey(); }
			return FKey(NAME_None);
		}

		static void Start(const TArray<FString>& Args)
		{
			Stop();

			if (Args.Num() == 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[PadMenu] Trace.Pad.Menu needs at least one of: up down left right lup ldown ")
					TEXT("lleft lright a b x y menu view wait."));
				return;
			}

			TSharedRef<FPadMenuScript> Script = MakeShared<FPadMenuScript>();
			for (const FString& Arg : Args)
			{
				const FKey Key = KeyForToken(Arg);
				if (Key.GetFName() == NAME_None && !Arg.Equals(TEXT("wait"), ESearchCase::IgnoreCase))
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[PadMenu] '%s' is not a token I know; skipped."), *Arg);
					continue;
				}
				Script->Keys.Add(Key);
				Script->Names.Add(Key.IsValid() ? UTraceUserSettings::DescribePadKey(Key) : FString(TEXT("(wait)")));
			}

			if (Script->Keys.Num() == 0)
			{
				return;
			}

			Instance = Script;
			Script->Handle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateSP(Script, &FPadMenuScript::Tick), 0.f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[PadMenu] Driving %d synthetic pad press(es), %.2fs held / %.2fs apart, through "
					"UGameViewportClient::InputKey. Controller input is %s."),
				Script->Keys.Num(), HoldSeconds, GapSeconds,
				TracePadMenu::IsEnabled() ? TEXT("ON") : TEXT("OFF (nothing should respond)"));
		}

		static void Stop()
		{
			if (Instance.IsValid())
			{
				// A half-finished script must not leave a button latched down for the rest of the run.
				if (Instance->Step == EStep::Hold && Instance->Keys.IsValidIndex(Instance->Index)
					&& Instance->Keys[Instance->Index].IsValid())
				{
					InjectButton(CurrentController(), Instance->Keys[Instance->Index], /*bPressed=*/false);
				}
				if (Instance->Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Instance->Handle);
				}
				Instance.Reset();
			}
		}

		static APlayerController* CurrentController()
		{
			UWorld* const World = (GEngine != nullptr) ? GEngine->GetCurrentPlayWorld() : nullptr;
			return TraceGamepadInputImpl::FindLocalController((World != nullptr) ? World->GetGameInstance() : nullptr);
		}

		bool Tick(float DeltaSeconds)
		{
			APlayerController* const PC = CurrentController();
			if (PC == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[PadMenu] No local player controller; sequence abandoned."));
				Instance.Reset();
				return false;
			}

			switch (Step)
			{
			case EStep::Press:
				// The down edge goes in on its OWN tick and the clock starts at zero afterwards, so
				// the button is guaranteed to be down across at least one whole engine frame — which
				// is what UPlayerInput::ProcessInputStack needs before WasInputKeyJustPressed and
				// IsInputKeyDown can report it at all.
				if (Keys[Index].IsValid())
				{
					InjectButton(PC, Keys[Index], /*bPressed=*/true);
				}
				UE_LOG(LogTraceGame, Display, TEXT("[PadMenu] %d/%d  press    %s"),
					Index + 1, Keys.Num(), *Names[Index]);
				Step = EStep::Hold;
				StepSeconds = 0.f;
				return true;

			case EStep::Hold:
				StepSeconds += DeltaSeconds;
				if (StepSeconds < HoldSeconds)
				{
					return true;
				}
				if (Keys[Index].IsValid())
				{
					InjectButton(PC, Keys[Index], /*bPressed=*/false);
				}
				UE_LOG(LogTraceGame, Display, TEXT("[PadMenu] %d/%d  release  %s"),
					Index + 1, Keys.Num(), *Names[Index]);
				Step = EStep::Gap;
				StepSeconds = 0.f;
				return true;

			case EStep::Gap:
			default:
				StepSeconds += DeltaSeconds;
				if (StepSeconds < GapSeconds)
				{
					return true;
				}
				break;
			}

			++Index;
			StepSeconds = 0.f;
			Step = EStep::Press;

			if (!Keys.IsValidIndex(Index))
			{
				UE_LOG(LogTraceGame, Display, TEXT("[PadMenu] Sequence complete (%d press(es))."), Keys.Num());
				if (Instance.IsValid())
				{
					Instance->Handle.Reset();
					Instance.Reset();
				}
				return false;
			}
			return true;
		}
	};

	TSharedPtr<FPadMenuScript> FPadMenuScript::Instance;

	// ---------------------------------------------------------------------------------------------
	// D32-PADMENU — Trace.Pad.MenuVerify
	// ---------------------------------------------------------------------------------------------
	//
	// What the four menu screens actually depend on, asserted against the LIVE input pipeline.
	//
	// SEVEN OF ITS NINETEEN CHECKS ARE NEGATIVE CONTROLS, because a check that cannot fail proves
	// nothing. THREE WHOLE ARMS are one — nothing pressed at all, a released button, and the whole
	// thing with CONTROLLER INPUT switched off — and FOUR MORE sit INSIDE the positive arms, which is
	// the half that is easy to skip: A must not also read as BACK or as the second verb, and a D-pad
	// direction must move its own axis and NOT the other one. Without those four, a helper that
	// answered "any button" or "any direction" for everything would pass every positive arm.
	//
	// The pure StepRepeat arm is bounded above as well as below: a repeat clock that fired every
	// frame would pass a "did it move at all" check.

	struct FPadMenuVerify : TSharedFromThis<FPadMenuVerify>
	{
		enum class EPhase : uint8
		{
			Idle,          // nothing injected — negative control
			AHeld,         // A down
			AReleased,     // A up — negative control
			DownHeld,      // D-pad down
			RightHeld,     // D-pad right
			StickLeft,     // left stick's digital LEFT — the stick twin of the D-pad
			PadOff,        // A down with CONTROLLER INPUT off — negative control
			Done,
		};

		static TSharedPtr<FPadMenuVerify> Instance;
		static constexpr float PhaseSeconds = 0.35f;

		/**
		 * The first tick of a phase still reads the PREVIOUS phase's key state, because this ticker
		 * runs before the player controller does. Same margin, same reason, as FDrive::SettleMargin.
		 */
		static constexpr float SettleMargin = 0.12f;

		EPhase Phase = EPhase::Idle;
		float Seconds = 0.f;
		bool bEntered = false;
		bool bMeasuring = false;

		bool bSawConfirm = false;
		bool bSawBack = false;
		bool bSawAlt = false;
		int32 SawNavX = 0;
		int32 SawNavY = 0;
		bool bNavXWasNonZero = false;
		bool bNavYWasNonZero = false;

		bool bRestorePadEnabled = true;
		FTSTicker::FDelegateHandle Handle;

		static void Start()
		{
			if (Instance.IsValid())
			{
				return;
			}
			GFailures = 0;
			Instance = MakeShared<FPadMenuVerify>();
			Instance->Handle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateSP(Instance.ToSharedRef(), &FPadMenuVerify::Tick), 0.f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[PadMenu] VERIFY: the menu vocabulary every pad-driven screen reads. Seven arms, "
					"nineteen checks; seven of the checks are negative controls (three whole arms, and "
					"four more inside the positive ones)."));

			// ---- The pure half, run immediately: it needs no frames -----------------------------
			StaticChecks();
		}

		static void StaticChecks()
		{
			const FKey A = TracePadMenu::ConfirmKey();
			const FKey B = TracePadMenu::BackKey();
			const FKey X = TracePadMenu::AltKey();

			Check(A != B && B != X && A != X, TEXT("CONFIRM, BACK and the second verb are three different buttons"),
				FString::Printf(TEXT("%s / %s / %s"), *UTraceUserSettings::DescribePadKey(A),
					*UTraceUserSettings::DescribePadKey(B), *UTraceUserSettings::DescribePadKey(X)));

			Check(UTraceUserSettings::IsBindablePadKey(A) && UTraceUserSettings::IsBindablePadKey(B)
				&& UTraceUserSettings::IsBindablePadKey(X),
				TEXT("all three are real pad buttons"),
				TEXT("IsBindablePadKey accepts each one, so none of them is a keyboard key that would "
					"never arrive from a controller"));

			// MENU/START is the pause key (TickMenuButton). A menu whose CONFIRM was also pause would
			// activate a row and open the pause menu on one press.
			Check(A != EKeys::Gamepad_Special_Right && B != EKeys::Gamepad_Special_Right
				&& X != EKeys::Gamepad_Special_Right,
				TEXT("none of the three is MENU/START, which is the pause key"), FString());

			// A menu button that is also a NAV key would move the highlight and activate it at once.
			const FKey NavKeys[] =
			{
				EKeys::Gamepad_DPad_Up,        EKeys::Gamepad_DPad_Down,
				EKeys::Gamepad_DPad_Left,      EKeys::Gamepad_DPad_Right,
				EKeys::Gamepad_LeftStick_Up,   EKeys::Gamepad_LeftStick_Down,
				EKeys::Gamepad_LeftStick_Left, EKeys::Gamepad_LeftStick_Right,
			};
			bool bClash = false;
			for (const FKey& Nav : NavKeys)
			{
				bClash = bClash || (Nav == A) || (Nav == B) || (Nav == X);
			}
			Check(!bClash, TEXT("no menu button is also one of the eight navigation keys"),
				TEXT("4 D-pad keys + the left stick's 4 digital keys"));

			// ---- The repeat clock, bounded at BOTH ends ------------------------------------------
			{
				int32 LastDir = 0;
				float NextTime = 0.f;
				int32 Steps = 0;

				// One fresh press: exactly one step, however many frames it is held for.
				for (float T = 0.f; T < TracePadMenu::RepeatDelay * 0.9f; T += 0.02f)
				{
					Steps += TracePadMenu::StepRepeat(1, LastDir, NextTime, T) ? 1 : 0;
				}
				Check(Steps == 1, TEXT("a held direction steps ONCE before the repeat delay"),
					FString::Printf(TEXT("%d step(s) over %.2fs of holding, delay is %.2fs"),
						Steps, TracePadMenu::RepeatDelay * 0.9f, TracePadMenu::RepeatDelay));

				// Then it repeats — but at the stated interval, not every frame.
				const int32 Before = Steps;
				for (float T = TracePadMenu::RepeatDelay * 0.9f; T < TracePadMenu::RepeatDelay + 0.30f; T += 0.02f)
				{
					Steps += TracePadMenu::StepRepeat(1, LastDir, NextTime, T) ? 1 : 0;
				}
				const int32 Repeats = Steps - Before;
				const int32 Expected = FMath::FloorToInt(0.30f / TracePadMenu::RepeatInterval) + 1;
				Check(Repeats >= 1 && Repeats <= Expected + 1,
					TEXT("and then repeats at the stated interval, not every frame"),
					FString::Printf(TEXT("%d repeat(s) in the 0.30s after the delay; %.2fs interval "
						"predicts about %d, a per-frame clock would give 15"),
						Repeats, TracePadMenu::RepeatInterval, Expected));

				// Release and press the same direction again: a NEW press, not a swallowed repeat.
				int32 Again = 0;
				TracePadMenu::StepRepeat(0, LastDir, NextTime, 1.0f);
				Again += TracePadMenu::StepRepeat(1, LastDir, NextTime, 1.01f) ? 1 : 0;
				Check(Again == 1, TEXT("releasing and re-pressing the same direction steps again"),
					TEXT("without the LastDir reset this press lands inside the previous repeat window "
						"and is silently swallowed"));
			}
		}

		static float DurationOf(EPhase) { return PhaseSeconds; }

		bool Tick(float DeltaSeconds)
		{
			APlayerController* const PC = FPadMenuScript::CurrentController();
			if (PC == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[PadMenu] VERIFY: no local player controller."));
				Finish();
				return false;
			}

			if (!bEntered)
			{
				Enter(PC);
			}

			Seconds += DeltaSeconds;

			if (!bMeasuring && Seconds >= SettleMargin)
			{
				bMeasuring = true;
				bSawConfirm = false;
				bSawBack = false;
				bSawAlt = false;
				SawNavX = 0;
				SawNavY = 0;
				bNavXWasNonZero = false;
				bNavYWasNonZero = false;

				// *** THE INJECTION HAPPENS HERE, NOT AT PHASE ENTRY. ***
				// It used to happen at entry and the "A reaches CONFIRM" arm FAILED on the first
				// honest run — a false negative, and a clean example of why an arm that has never
				// been red is worth nothing. WasInputKeyJustPressed is true for exactly ONE
				// player-input tick, and a press injected at entry has that tick inside the settle
				// margin, which is the window this test throws away. Injecting at the moment
				// measurement opens puts the one tick that carries the edge inside the window that
				// looks for it. Phase ENTRY still does the tear-down of the previous phase (release
				// its key, flip the setting), which is what the margin is genuinely for.
				BeginMeasuring(PC);
			}

			if (bMeasuring)
			{
				bSawConfirm = bSawConfirm || TracePadMenu::ConfirmPressed(PC);
				bSawBack    = bSawBack    || TracePadMenu::BackPressed(PC);
				bSawAlt     = bSawAlt     || TracePadMenu::AltPressed(PC);

				const int32 X = TracePadMenu::NavX(PC);
				const int32 Y = TracePadMenu::NavY(PC);
				if (X != 0) { SawNavX = X; bNavXWasNonZero = true; }
				if (Y != 0) { SawNavY = Y; bNavYWasNonZero = true; }
			}

			if (Seconds >= DurationOf(Phase))
			{
				Leave(PC);
			}

			if (Phase == EPhase::Done)
			{
				Finish();
				return false;
			}
			return true;
		}

		/** Phase entry: undo the PREVIOUS phase. Nothing this phase is testing is injected here. */
		void Enter(APlayerController* PC)
		{
			bEntered = true;
			bMeasuring = false;
			Seconds = 0.f;

			switch (Phase)
			{
			case EPhase::AReleased: InjectButton(PC, TracePadMenu::ConfirmKey(), false); break;
			case EPhase::RightHeld: InjectButton(PC, EKeys::Gamepad_DPad_Down, false); break;
			case EPhase::StickLeft: InjectButton(PC, EKeys::Gamepad_DPad_Right, false); break;
			case EPhase::PadOff:
			{
				InjectButton(PC, EKeys::Gamepad_LeftStick_Left, false);
				UTraceUserSettings& Settings = UTraceUserSettings::Get();
				bRestorePadEnabled = Settings.bPadEnabled;
				// The state the OFF toggle on the CONTROLLER page actually produces. The field is
				// written directly rather than through Save(), because a Save here would rewrite the
				// player's .ini for the duration of a check.
				Settings.bPadEnabled = false;
				break;
			}
			default:
				break;
			}
		}

		/** The moment measurement opens: inject what THIS phase is about. See the call site. */
		void BeginMeasuring(APlayerController* PC)
		{
			switch (Phase)
			{
			case EPhase::AHeld:     InjectButton(PC, TracePadMenu::ConfirmKey(), true); break;
			case EPhase::DownHeld:  InjectButton(PC, EKeys::Gamepad_DPad_Down, true); break;
			case EPhase::RightHeld: InjectButton(PC, EKeys::Gamepad_DPad_Right, true); break;
			case EPhase::StickLeft: InjectButton(PC, EKeys::Gamepad_LeftStick_Left, true); break;

			// The negative control's positive half: A really is pressed, it is only the SETTING that
			// is off. An arm that injected nothing here would pass with the gate deleted.
			case EPhase::PadOff:    InjectButton(PC, TracePadMenu::ConfirmKey(), true); break;

			// Idle and AReleased inject nothing, on purpose: they are the two arms whose whole claim
			// is that nothing arrives.
			default:
				break;
			}
		}

		void Leave(APlayerController* PC)
		{
			switch (Phase)
			{
			case EPhase::Idle:
				Check(!bSawConfirm && !bSawBack && !bSawAlt && !bNavXWasNonZero && !bNavYWasNonZero,
					TEXT("[control] nothing pressed, nothing reads as pressed"),
					FString::Printf(TEXT("confirm=%d back=%d alt=%d navX=%d navY=%d"),
						bSawConfirm ? 1 : 0, bSawBack ? 1 : 0, bSawAlt ? 1 : 0,
						bNavXWasNonZero ? 1 : 0, bNavYWasNonZero ? 1 : 0));
				break;

			case EPhase::AHeld:
				Check(bSawConfirm, TEXT("A reaches CONFIRM"), TEXT("injected at the viewport, read back "
					"through the same TracePadMenu::ConfirmPressed every screen calls"));
				// NEGATIVE CONTROL INSIDE THE POSITIVE ARM: without it, a helper that answered "any
				// button" for all three would pass every arm above.
				Check(!bSawBack && !bSawAlt, TEXT("[control] A is not also BACK or the second verb"),
					FString::Printf(TEXT("back=%d alt=%d"), bSawBack ? 1 : 0, bSawAlt ? 1 : 0));
				Check(!bNavXWasNonZero && !bNavYWasNonZero,
					TEXT("[control] A moves no highlight"),
					FString::Printf(TEXT("navX=%d navY=%d"), SawNavX, SawNavY));
				break;

			case EPhase::AReleased:
				Check(!bSawConfirm, TEXT("[control] CONFIRM is false once A is released"), FString());
				break;

			case EPhase::DownHeld:
				Check(bNavYWasNonZero && SawNavY == 1, TEXT("D-pad DOWN is +1 on the vertical axis"),
					FString::Printf(TEXT("navY=%d"), SawNavY));
				Check(!bNavXWasNonZero, TEXT("[control] and moves nothing horizontally"),
					FString::Printf(TEXT("navX=%d"), SawNavX));
				break;

			case EPhase::RightHeld:
				Check(bNavXWasNonZero && SawNavX == 1, TEXT("D-pad RIGHT is +1 on the horizontal axis"),
					FString::Printf(TEXT("navX=%d"), SawNavX));
				Check(!bNavYWasNonZero, TEXT("[control] and moves nothing vertically"),
					FString::Printf(TEXT("navY=%d"), SawNavY));
				break;

			case EPhase::StickLeft:
				Check(bNavXWasNonZero && SawNavX == -1,
					TEXT("the LEFT STICK's digital keys drive the same axis as the D-pad"),
					FString::Printf(TEXT("navX=%d — this is what lets a player who never finds the "
						"D-pad still walk a menu"), SawNavX));
				break;

			case EPhase::PadOff:
			{
				Check(!bSawConfirm && !bNavXWasNonZero && !bNavYWasNonZero,
					TEXT("[control] with CONTROLLER INPUT off, a held A does nothing at all"),
					FString::Printf(TEXT("confirm=%d navX=%d navY=%d — the same gate TickMenuButton "
						"puts on MENU/START"), bSawConfirm ? 1 : 0, SawNavX, SawNavY));

				UTraceUserSettings& Settings = UTraceUserSettings::Get();
				Settings.bPadEnabled = bRestorePadEnabled;
				InjectButton(PC, TracePadMenu::ConfirmKey(), false);
				Check(TracePadMenu::IsEnabled() == bRestorePadEnabled,
					TEXT("CONTROLLER INPUT was put back the way it was found"),
					FString::Printf(TEXT("now %s"), bRestorePadEnabled ? TEXT("ON") : TEXT("OFF")));
				break;
			}

			default:
				break;
			}

			Phase = static_cast<EPhase>(static_cast<uint8>(Phase) + 1);
			bEntered = false;
		}

		void Finish()
		{
			if (GFailures == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[PadMenu] VERIFY VERDICT: the menu vocabulary holds. 0 failure(s)."));
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[PadMenu] VERIFY VERDICT: %d failure(s)."), GFailures);
			}

			if (Instance.IsValid())
			{
				Instance->Handle.Reset();
				Instance.Reset();
			}
		}
	};

	TSharedPtr<FPadMenuVerify> FPadMenuVerify::Instance;

	/**
	 * D32-PADMENU — Trace.Pad.Enable <0|1>.
	 *
	 * The CONTROLLER INPUT toggle, from the console, so the OFF case can be a NEGATIVE CONTROL on any
	 * screen rather than only inside Trace.Pad.MenuVerify's own arm. The title screen is the one that
	 * needs it: MenuVerify cannot be run there (its A arm would start a match), so without this there
	 * was no way to show that the same synthetic presses that walk the menu do nothing when a player
	 * has switched their pad off.
	 *
	 * IN MEMORY ONLY — it writes the field and re-applies the context, and deliberately does NOT
	 * Save(). A dev command that left a player's TraceUserSettings.ini with their controller disabled
	 * is a support ticket nobody would connect back to a verification run.
	 */
	FAutoConsoleCommand CmdPadEnable(
		TEXT("Trace.Pad.Enable"),
		TEXT("D32-PADMENU, dev only. Trace.Pad.Enable <0|1> - flips CONTROLLER INPUT for this session "
			"without saving it, so the OFF case can be used as a negative control."),
		FConsoleCommandWithArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args)
			{
				const bool bWant = (Args.Num() == 0) || (FCString::Atoi(*Args[0]) != 0);
				UTraceUserSettings::Get().bPadEnabled = bWant;

				UWorld* const World = (GEngine != nullptr) ? GEngine->GetCurrentPlayWorld() : nullptr;
				if (UTraceGamepadInputSubsystem* const Pad = UTraceGamepadInputSubsystem::Get(World))
				{
					Pad->ApplyPadSettings();
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[Pad] CONTROLLER INPUT is now %s (this session only; nothing was saved)."),
					bWant ? TEXT("ON") : TEXT("OFF"));
			}));

	FAutoConsoleCommand CmdPadMenu(
		TEXT("Trace.Pad.Menu"),
		TEXT("D32-PADMENU. Trace.Pad.Menu <token> [<token> ...] - drives the menus with synthetic pad "
			"button presses, one at a time, spaced over real frames. Tokens: up down left right (D-pad), "
			"lup ldown lleft lright (left stick), a b x y, menu, view, wait. Each is held 0.20s and "
			"released, which is one navigation step and never two. Nothing is called directly: the "
			"button goes in at UGameViewportClient::InputKey and every screen reads it through its own "
			"polling."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FPadMenuScript::Start));

	FAutoConsoleCommand CmdPadMenuVerify(
		TEXT("Trace.Pad.MenuVerify"),
		TEXT("D32-PADMENU. Proves the pad's MENU vocabulary against the live input pipeline: A is "
			"CONFIRM and only CONFIRM, B is BACK, the D-pad and the left stick drive the same two axes, "
			"the shared repeat clock steps once per press, and CONTROLLER INPUT off silences all of it. "
			"Seven of its nineteen checks are negative controls. Takes about 2.5s. Run it in a match, "
			"not on the title screen, where a synthetic A would start one."),
		FConsoleCommandDelegate::CreateStatic(&FPadMenuVerify::Start));

	FAutoConsoleCommand CmdPadStatus(
		TEXT("Trace.Pad.Status"),
		TEXT("D31-PAD. Prints the shipped and current controller layout, the analog settings, whether "
			"the pad's mapping context is built, and whether a controller has been seen on this machine."),
		FConsoleCommandDelegate::CreateStatic(&Status));

	FAutoConsoleCommand CmdPadVerify(
		TEXT("Trace.Pad.Verify"),
		TEXT("D31-PAD, static. Proves the shipped controller layout has no two actions on one button, "
			"that every default is a bindable pad button, that MENU/START is left free for pause, that "
			"the pad and keyboard tables cannot contain each other's keys, that a pad rebind survives "
			"the .ini round trip, and that neither page's RESET touches the other's binds. Restores "
			"everything it changes."),
		FConsoleCommandDelegate::CreateStatic(&Verify));

	FAutoConsoleCommand CmdPadDrive(
		TEXT("Trace.Pad.Drive"),
		TEXT("D31-PAD, live. Injects synthetic stick, button, keyboard and mouse samples through the "
			"real input pipeline and reads the result back out of Enhanced Input AND out of the "
			"control rotation. Four arms are negative controls (nothing injected, a stick inside the "
			"dead zone, a full stick with controller input switched off, a released button) and two "
			"prove the keyboard and the mouse still work while the pad's context is applied. Takes "
			"about 5 s, and must be run in a match rather than on the character-select screen, where "
			"gameplay input is suppressed and every look arm would read zero."),
		FConsoleCommandDelegate::CreateStatic(&FDrive::Start));
}

#endif // !UE_BUILD_SHIPPING
