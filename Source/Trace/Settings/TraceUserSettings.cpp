// Trace — player control settings implementation. See TraceUserSettings.h.

#include "Settings/TraceUserSettings.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Trace.h"                      // LogTraceGame
#include "UObject/UObjectGlobals.h"     // GetMutableDefault

// =================================================================================================
// The action table
//
// DefaultKey is a function pointer rather than an FKey value because EKeys' statics are initialised
// during module startup, and a namespace-scope FKey table would be built from them at static-init
// time with no ordering guarantee. A call is free and cannot be wrong.
// =================================================================================================

namespace
{
	FKey Default_MoveForward() { return EKeys::W; }
	FKey Default_MoveBack()    { return EKeys::S; }
	FKey Default_MoveLeft()    { return EKeys::A; }
	FKey Default_MoveRight()   { return EKeys::D; }
	FKey Default_Jump()        { return EKeys::SpaceBar; }
	FKey Default_Crouch()      { return EKeys::LeftControl; }
	FKey Default_Dash()        { return EKeys::LeftShift; }
	/**
	 * Parry (spec v3 §3). The spec suggested "right mouse or Q"; RIGHT MOUSE IS ALREADY PASS, and
	 * parry is a carrier-only ability, so overloading it onto the carrier's own pass button would
	 * make the two mechanics unusable together — exactly the pair a carrier needs most. Q it is:
	 * unclaimed, under the movement hand, reachable without leaving WASD.
	 */
	FKey Default_Parry()       { return EKeys::Q; }
	FKey Default_Fire()        { return EKeys::LeftMouseButton; }
	FKey Default_Pass()        { return EKeys::RightMouseButton; }
	FKey Default_Scoreboard()  { return EKeys::Tab; }
}

const TArray<FTraceInputActionInfo>& TraceInputActions::All()
{
	// Function-local static: built on first use, after EKeys is up, and never rebuilt.
	static const TArray<FTraceInputActionInfo> Table =
	{
		{ ETraceInputAction::MoveForward, TEXT("MoveForward"), TEXT("MOVE FORWARD"), &Default_MoveForward },
		{ ETraceInputAction::MoveBack,    TEXT("MoveBack"),    TEXT("MOVE BACK"),    &Default_MoveBack    },
		{ ETraceInputAction::MoveLeft,    TEXT("MoveLeft"),    TEXT("STRAFE LEFT"),  &Default_MoveLeft    },
		{ ETraceInputAction::MoveRight,   TEXT("MoveRight"),   TEXT("STRAFE RIGHT"), &Default_MoveRight   },
		{ ETraceInputAction::Jump,        TEXT("Jump"),        TEXT("JUMP"),         &Default_Jump        },
		{ ETraceInputAction::Crouch,      TEXT("Crouch"),      TEXT("CROUCH / SLIDE"), &Default_Crouch    },
		{ ETraceInputAction::Dash,        TEXT("Dash"),        TEXT("DASH"),         &Default_Dash        },
		{ ETraceInputAction::Parry,       TEXT("Parry"),       TEXT("PARRY"),        &Default_Parry       },
		{ ETraceInputAction::Fire,        TEXT("Fire"),        TEXT("FIRE"),         &Default_Fire        },
		{ ETraceInputAction::Pass,        TEXT("Pass"),        TEXT("PASS CORE"),    &Default_Pass        },
		{ ETraceInputAction::Scoreboard,  TEXT("Scoreboard"),  TEXT("SCOREBOARD"),   &Default_Scoreboard  },
	};

	static_assert(static_cast<int32>(ETraceInputAction::Count) == 11,
		"ETraceInputAction and TraceInputActions::All() have drifted apart. Add the new action to the "
		"table above, give it a ConfigId that will never change, and bind it in ATracePlayerController.");

	return Table;
}

const FTraceInputActionInfo& TraceInputActions::Info(ETraceInputAction Action)
{
	const TArray<FTraceInputActionInfo>& Table = All();
	const int32 Index = FMath::Clamp(static_cast<int32>(Action), 0, Table.Num() - 1);
	return Table[Index];
}

// =================================================================================================
// Lifecycle
// =================================================================================================

UTraceUserSettings::UTraceUserSettings()
{
	Bindings.SetNum(static_cast<int32>(ETraceInputAction::Count));
}

UTraceUserSettings& UTraceUserSettings::Get()
{
	UTraceUserSettings* Settings = GetMutableDefault<UTraceUserSettings>();

	// The config system fills the CDO's `config` properties at class load, but KeyBindings is a flat
	// string list that still has to be parsed into FKeys. Doing it lazily here means every entry
	// point — controller, title screen, pause menu — gets a ready object without any of them having
	// to know about an init order.
	if (!Settings->bLoaded)
	{
		Settings->RefreshFromConfig();
	}

	return *Settings;
}

FTraceUserSettingsChanged& UTraceUserSettings::OnChanged()
{
	static FTraceUserSettingsChanged Delegate;
	return Delegate;
}

// =================================================================================================
// Mouse
// =================================================================================================

float UTraceUserSettings::GetLookScaleX() const
{
	// Clamped rather than trusted: this value can arrive from a hand-edited .ini, and a zero would
	// silently disable looking while a huge one would spin the player on the first mouse event.
	return FMath::Clamp(MouseSensitivity, MinSensitivity, MaxSensitivity);
}

float UTraceUserSettings::GetLookScaleY() const
{
	const float Scale = GetLookScaleX()
		* FMath::Clamp(MouseSensitivityYScale, MinSensitivityYScale, MaxSensitivityYScale);

	// The sign IS the inversion. Folding it into the Scalar modifier rather than adding or removing
	// a Negate modifier keeps the mapping's modifier list a fixed shape, so a live rebuild only ever
	// changes numbers.
	return bInvertMouseY ? -Scale : Scale;
}

// =================================================================================================
// Bindings
// =================================================================================================

bool UTraceUserSettings::IsBindableKey(const FKey& Key)
{
	if (!Key.IsValid())
	{
		return false;
	}

	// Axes are what the Look mapping consumes; binding Dash to "MouseX" would fire continuously.
	// Gestures and touch are not reachable on this platform. Everything else — keyboard, mouse
	// buttons, wheel clicks, gamepad face buttons — is fair game.
	if (Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D() || Key.IsGesture() || Key.IsTouch())
	{
		return false;
	}

	// Escape is the universal "get me out of here" in this menu and must never become a game bind,
	// or the player can lock themselves out of the settings screen with the settings screen.
	if (Key == EKeys::Escape)
	{
		return false;
	}

	// EKeys::AnyKey is a real, valid, non-axis FKey that fires for EVERY key press. Measured: the
	// rebind capture walks this list and matches it before it ever reaches the key the player
	// actually pressed, so the first rebind produced "MOVE FORWARD -> ANY KEY" and every subsequent
	// key press moved the player forward. It is a wildcard, not a button.
	if (Key == EKeys::AnyKey)
	{
		return false;
	}

	return true;
}

FString UTraceUserSettings::DescribeKey(const FKey& Key)
{
	if (!Key.IsValid())
	{
		return TEXT("UNBOUND");
	}

	// GetDisplayName gives "Space Bar" / "Left Shift" / "Left Mouse Button", which is what a player
	// recognises; the internal FName would give "SpaceBar" and "LeftMouseButton".
	return Key.GetDisplayName().ToString().ToUpper();
}

FKey UTraceUserSettings::GetKey(ETraceInputAction Action) const
{
	const int32 Index = static_cast<int32>(Action);
	return Bindings.IsValidIndex(Index) ? Bindings[Index] : FKey();
}

void UTraceUserSettings::SetKey(ETraceInputAction Action, const FKey& Key)
{
	const int32 Index = static_cast<int32>(Action);
	if (!Bindings.IsValidIndex(Index) || !IsBindableKey(Key))
	{
		return;
	}

	// Steal the key from whoever else had it. See the header for why this beats refusing.
	for (int32 Other = 0; Other < Bindings.Num(); ++Other)
	{
		if (Other != Index && Bindings[Other] == Key)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[Settings] '%s' taken from %s and given to %s."),
				*DescribeKey(Key),
				TraceInputActions::Info(static_cast<ETraceInputAction>(Other)).DisplayName,
				TraceInputActions::Info(Action).DisplayName);
			Bindings[Other] = FKey();
		}
	}

	Bindings[Index] = Key;
	Save();
}

void UTraceUserSettings::ClearKey(ETraceInputAction Action)
{
	const int32 Index = static_cast<int32>(Action);
	if (!Bindings.IsValidIndex(Index) || !Bindings[Index].IsValid())
	{
		return;
	}

	UE_LOG(LogTraceGame, Display, TEXT("[Settings] %s unbound."), TraceInputActions::Info(Action).DisplayName);
	Bindings[Index] = FKey();
	Save();
}

void UTraceUserSettings::RefreshFromConfig()
{
	const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();

	Bindings.Reset();
	Bindings.SetNum(Table.Num());

	// Start from the shipped defaults, then let the .ini override entry by entry. A truncated or
	// partially corrupt file therefore degrades to "some defaults" rather than to "no controls".
	for (int32 Index = 0; Index < Table.Num(); ++Index)
	{
		Bindings[Index] = Table[Index].DefaultKey();
	}

	for (const FString& Entry : KeyBindings)
	{
		FString ConfigId;
		FString KeyName;
		if (!Entry.Split(TEXT("="), &ConfigId, &KeyName))
		{
			continue;
		}

		ConfigId.TrimStartAndEndInline();
		KeyName.TrimStartAndEndInline();

		const int32 Index = Table.IndexOfByPredicate(
			[&ConfigId](const FTraceInputActionInfo& Info) { return ConfigId.Equals(Info.ConfigId, ESearchCase::IgnoreCase); });

		if (Index == INDEX_NONE)
		{
			// An action that no longer exists. Dropping it silently is correct: the alternative is
			// refusing to load a file that a previous build wrote perfectly legitimately.
			continue;
		}

		// The explicit unbound marker, which is what a stolen key leaves behind.
		if (KeyName.IsEmpty() || KeyName.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			Bindings[Index] = FKey();
			continue;
		}

		const FKey Key(*KeyName);
		if (IsBindableKey(Key))
		{
			Bindings[Index] = Key;
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Settings] Ignoring '%s' for action '%s': not a bindable key on this platform. Keeping the default."),
				*KeyName, *ConfigId);
		}
	}

	bLoaded = true;
}

void UTraceUserSettings::FlattenToConfig()
{
	const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();

	KeyBindings.Reset(Table.Num());
	for (int32 Index = 0; Index < Table.Num() && Index < Bindings.Num(); ++Index)
	{
		// FKey::GetFName() is the stable serialisation name ("SpaceBar"), NOT the display name.
		// Writing the display name would produce a file that cannot be read back.
		const FKey& Key = Bindings[Index];
		KeyBindings.Add(FString::Printf(TEXT("%s=%s"),
			Table[Index].ConfigId,
			Key.IsValid() ? *Key.GetFName().ToString() : TEXT("None")));
	}
}

void UTraceUserSettings::ResetToDefaults()
{
	MouseSensitivity = DefaultSensitivity;
	MouseSensitivityYScale = DefaultSensitivityYScale;
	bInvertMouseY = false;

	KeyBindings.Reset();
	RefreshFromConfig();      // repopulates Bindings straight from the defaults table
	Save();
}

bool UTraceUserSettings::IsAtDefaults() const
{
	if (!FMath::IsNearlyEqual(MouseSensitivity, DefaultSensitivity)
		|| !FMath::IsNearlyEqual(MouseSensitivityYScale, DefaultSensitivityYScale)
		|| bInvertMouseY)
	{
		return false;
	}

	const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();
	for (int32 Index = 0; Index < Table.Num() && Index < Bindings.Num(); ++Index)
	{
		if (Bindings[Index] != Table[Index].DefaultKey())
		{
			return false;
		}
	}

	return true;
}

void UTraceUserSettings::Save()
{
	FlattenToConfig();

	// SaveConfig writes into GConfig's in-memory copy of the file. Without the explicit Flush the
	// values only reach disk at a clean shutdown — and this build is normally ended with pkill, so
	// "clean shutdown" is the case that does not happen.
	SaveConfig();
	if (GConfig != nullptr)
	{
		GConfig->Flush(false, GetClass()->GetConfigName());
	}

	// One listener today (ATracePlayerController) but broadcast unconditionally: the title screen
	// has no controller at all, and a future one must not have to be remembered here.
	OnChanged().Broadcast();

	UE_LOG(LogTraceGame, Display,
		TEXT("[Settings] Saved. sensitivity=%.2f yScale=%.2f invertY=%d -> %s"),
		MouseSensitivity, MouseSensitivityYScale, bInvertMouseY ? 1 : 0,
		*GetClass()->GetConfigName());
}
