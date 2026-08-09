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
	/**
	 * Spec v13 §2, verbatim: "Change default keybinds for switching weapons to be: 1 (switch to
	 * knife) and 2 (switch to gun)."
	 *
	 * EKeys::One / EKeys::Two are the NUMBER ROW, not the numpad (that is EKeys::NumPadOne). The
	 * request is the genre convention — weapon slots on the top row — and the number row is what a
	 * player reaches for without thinking and what a laptop without a numpad still has.
	 *
	 * Both pass IsBindableKey: they are real buttons, not axes, not Escape and not AnyKey. Nothing
	 * else in the table claims a digit, so neither default steals a key from another action on a
	 * first run (SetKey's stealing rule would have logged it if it did).
	 *
	 * SPEC v15 §5: these two are now the ONLY weapon binds. The SwapWeapon toggle that used to sit
	 * above them — ConfigId "SwapWeapon", default F — is deleted, so F is unclaimed again and a
	 * player's saved `SwapWeapon=F` line is dropped on load like any other line naming an action
	 * that no longer exists. See the ETraceInputAction::EquipKnife comment for the full argument.
	 */
	FKey Default_EquipKnife()  { return EKeys::One; }
	FKey Default_EquipGun()    { return EKeys::Two; }
	/**
	 * SPEC v14 §5. E and V are NAMED BY THE DOC, so there is no key to choose here — only a collision
	 * to check. Taken already: WASD, Space, LeftCtrl, LeftShift, Q, mouse1, mouse2, Tab, 1, 2.
	 * Neither E nor V is claimed by any row above, so neither default steals a key on a first run
	 * (SetKey's stealing rule would log it if it did).
	 *
	 * Historical note worth keeping: E was the pre-v3 BOOST key. That action is gone and its ConfigId
	 * ("Boost") is not this one, so an old TraceUserSettings.ini's `Boost=E` line is dropped by
	 * RefreshFromConfig exactly as it is for Parry — nobody inherits an ability bound by accident.
	 */
	FKey Default_Ability()          { return EKeys::E; }
	FKey Default_AbilitySecondary() { return EKeys::V; }
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
		// SPEC v13 §2. These two rows exist for the options screen as much as for the game: the
		// rebind list IS this table, walked in order, so an action that is not here is an action the
		// player cannot see or rebind however well it is wired up in the controller. "Both new binds
		// must appear in the settings rebind list" is satisfied by these lines and by nothing else.
		//
		// SPEC v15 §5 DELETED THE `SwapWeapon` ROW that used to sit directly above these two. That is
		// also what removes "SWAP WEAPON" from the options screen's rebind list — the list is this
		// table walked in order and nothing else, so there is no second place to go and delete it.
		{ ETraceInputAction::EquipKnife,  TEXT("EquipKnife"),  TEXT("EQUIP KNIFE"),  &Default_EquipKnife  },
		{ ETraceInputAction::EquipGun,    TEXT("EquipGun"),    TEXT("EQUIP GUN"),    &Default_EquipGun    },
		// SPEC v14 §5. Same reasoning as the two rows above: the rebind list IS this table, so an
		// ability the player cannot see here is an ability they cannot rebind however well it is
		// wired in the controller. The ConfigIds are the two strings ATraceHUD and
		// UTraceAbilityInputRelay already search for by name — do not rename them.
		{ ETraceInputAction::Ability,          TEXT("Ability"),          TEXT("ABILITY"),           &Default_Ability          },
		{ ETraceInputAction::AbilitySecondary, TEXT("AbilitySecondary"), TEXT("ABILITY (SECONDARY)"), &Default_AbilitySecondary },
	};

	static_assert(static_cast<int32>(ETraceInputAction::Count) == 15,
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

	// SPEC v10 §8 — "Allow Mouse button 1 and mouse button 2 as keybinds in the settings menu."
	//
	// MOUSE BUTTONS ARE BINDABLE AND ALWAYS HAVE BEEN, and this comment exists so the next person to
	// read the note does not "fix" it here a second time. The shipped defaults are LeftMouseButton
	// for FIRE and RightMouseButton for PASS (see the table at the top of this file), so a rule that
	// rejected mouse buttons would have refused to load the game's own default bindings on the first
	// run — which is not what happens. Trace.VerifyBindableKeys prints the verdict for every mouse
	// button in the build so this can be checked rather than argued about.
	//
	// AXES ARE THE ONLY MOUSE INPUT REFUSED, and that is a different thing entirely. MouseX / MouseY
	// / Mouse2D / MouseWheelAxis are what the Look mapping consumes; binding Dash to "MouseX" would
	// fire it continuously for as long as the player looked around. MouseScrollUp and MouseScrollDown
	// are BUTTONS in UE's model, not axes, so a scroll click stays bindable — which is what a player
	// who wants dash on the wheel expects.
	//
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

// =================================================================================================
// Trace.VerifyBindableKeys — SPEC v10 §8, the evidence half.
//
// The note is "allow mouse button 1 and mouse button 2 as keybinds", i.e. a report that the rebind
// UI refuses them. There are exactly two places a key can be refused between a player pressing it
// and it becoming a binding:
//
//   1. VALIDATION — UTraceUserSettings::IsBindableKey, which is what builds the options menu's
//      capture list and what SetKey and RefreshFromConfig both gate on. If a key fails here it can
//      never be captured, never be loaded from the ini and never be set programmatically.
//   2. DELIVERY — whether the press reaches APlayerController::WasInputKeyJustPressed at all while
//      the options overlay is up, which is an input-routing question, not a settings one.
//
// This command answers (1) exhaustively and by name, so (2) is what is left over. It walks every key
// the engine knows, prints the verdict for the mouse set specifically, and then does a full
// round trip on LeftMouseButton and RightMouseButton — bind it, read it back, flatten it to the
// string the ini stores, re-parse it — because "IsBindableKey says yes" and "the binding survives a
// save and a load" are two different claims and the second is the one a player experiences.
//
// The bindings it touches are restored before it returns, so it is safe to run mid-session.
// =================================================================================================

namespace
{
	void VerifyBindableKeys()
	{
		UTraceUserSettings& Settings = UTraceUserSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("[BindableKeys] ===== spec v10 s8: which keys the rebind UI is allowed to capture ====="));

		// --- The mouse set, named one by one -----------------------------------------------------
		//
		// Listed explicitly rather than filtered out of GetAllKeys by IsMouseButton(), because the
		// question being answered is "is THIS key, the one in the note, bindable" and a filter that
		// silently returned an empty list would read as a pass.
		const TArray<FKey> MouseKeys =
		{
			EKeys::LeftMouseButton, EKeys::RightMouseButton, EKeys::MiddleMouseButton,
			EKeys::ThumbMouseButton, EKeys::ThumbMouseButton2,
			EKeys::MouseScrollUp, EKeys::MouseScrollDown,
			EKeys::MouseX, EKeys::MouseY, EKeys::MouseWheelAxis
		};

		int32 MouseButtonsBindable = 0;
		for (const FKey& Key : MouseKeys)
		{
			const bool bBindable = UTraceUserSettings::IsBindableKey(Key);
			const bool bAxis = Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D();

			if (bBindable && !bAxis)
			{
				++MouseButtonsBindable;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[BindableKeys]   %-20s bindable=%d  axis=%d  (%s)"),
				*Key.GetFName().ToString(), bBindable ? 1 : 0, bAxis ? 1 : 0,
				*UTraceUserSettings::DescribeKey(Key));
		}

		// --- The exclusions that must SURVIVE ------------------------------------------------------
		const bool bEscapeExcluded = !UTraceUserSettings::IsBindableKey(EKeys::Escape);
		const bool bAnyKeyExcluded = !UTraceUserSettings::IsBindableKey(EKeys::AnyKey);

		UE_LOG(LogTraceGame, Display,
			TEXT("[BindableKeys]   exclusions kept: Escape=%d AnyKey=%d (both must be 1)"),
			bEscapeExcluded ? 1 : 0, bAnyKeyExcluded ? 1 : 0);

		// --- The whole capture list, counted ------------------------------------------------------
		TArray<FKey> AllKeys;
		EKeys::GetAllKeys(AllKeys);

		int32 Capturable = 0;
		int32 CapturableMouse = 0;
		for (const FKey& Key : AllKeys)
		{
			if (!UTraceUserSettings::IsBindableKey(Key))
			{
				continue;
			}
			++Capturable;
			CapturableMouse += Key.IsMouseButton() ? 1 : 0;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[BindableKeys]   the options menu's capture list is %d of %d engine keys, %d of them mouse buttons."),
			Capturable, AllKeys.Num(), CapturableMouse);

		// --- The round trip -----------------------------------------------------------------------
		//
		// Dash is the victim on purpose: it is not one of the two actions that already OWN a mouse
		// button, so a pass here cannot be an accident of the defaults.
		const FKey Saved = Settings.GetKey(ETraceInputAction::Dash);
		const FKey SavedFire = Settings.GetKey(ETraceInputAction::Fire);
		const FKey SavedPass = Settings.GetKey(ETraceInputAction::Pass);

		int32 RoundTripsOk = 0;
		for (const FKey& Key : { EKeys::LeftMouseButton, EKeys::RightMouseButton })
		{
			Settings.SetKey(ETraceInputAction::Dash, Key);
			const FKey ReadBack = Settings.GetKey(ETraceInputAction::Dash);

			// Through the string form the ini actually stores, and back. This is where a key that
			// validates but does not SERIALISE would be caught.
			const FString Serialised = ReadBack.IsValid() ? ReadBack.GetFName().ToString() : TEXT("None");
			const FKey Reparsed(*Serialised);

			const bool bOk = (ReadBack == Key) && (Reparsed == Key) && UTraceUserSettings::IsBindableKey(Reparsed);
			RoundTripsOk += bOk ? 1 : 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("[BindableKeys]   DASH <- %-18s set=%d ini='%s' reparsed=%s -> %s"),
				*Key.GetFName().ToString(), (ReadBack == Key) ? 1 : 0, *Serialised,
				*Reparsed.GetFName().ToString(), bOk ? TEXT("ROUND TRIP OK") : TEXT("FAILED"));
		}

		// Put everything back, including the two actions SetKey may have stolen the button from.
		Settings.SetKey(ETraceInputAction::Dash, Saved);
		Settings.SetKey(ETraceInputAction::Fire, SavedFire);
		Settings.SetKey(ETraceInputAction::Pass, SavedPass);

		// Two calls rather than a ternary verbosity: UE_LOG's verbosity argument is a token the macro
		// pastes into a compile-time category check, not a value, so it cannot be an expression.
		const bool bPass = (MouseButtonsBindable >= 5) && bEscapeExcluded && bAnyKeyExcluded && (RoundTripsOk == 2);

#define TRACE_BINDABLE_VERDICT_ARGS \
	bPass ? TEXT("VALIDATION ACCEPTS MOUSE BUTTONS") : TEXT("VALIDATION IS REJECTING SOMETHING"), \
	MouseButtonsBindable, RoundTripsOk, (bEscapeExcluded && bAnyKeyExcluded) ? 1 : 0

#define TRACE_BINDABLE_VERDICT_TEXT \
	TEXT("[BindableKeys] VERDICT: %s. Mouse buttons bindable=%d/7, round trips=%d/2, Escape and ") \
	TEXT("AnyKey still excluded=%d. If this passes and the MENU still refuses a mouse click, the ") \
	TEXT("refusal is in DELIVERY (the press never reaching WasInputKeyJustPressed while the overlay ") \
	TEXT("is up), not in validation.")

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TRACE_BINDABLE_VERDICT_TEXT, TRACE_BINDABLE_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_BINDABLE_VERDICT_TEXT, TRACE_BINDABLE_VERDICT_ARGS);
		}

#undef TRACE_BINDABLE_VERDICT_ARGS
#undef TRACE_BINDABLE_VERDICT_TEXT
	}

	// =============================================================================================
	// Trace.Settings.VerifyBinds — SPEC v15 §5, the evidence half.
	//
	// §5 deletes ETraceInputAction::SwapWeapon, which RENUMBERS every enumerator below it. The claim
	// is that a player's existing TraceUserSettings.ini survives that unharmed, because the file is
	// keyed by ConfigId STRING and never by position. That claim is cheap to write and has been
	// wrong in this codebase before, so this command checks it against the file that is actually on
	// disk rather than against the in-memory table:
	//
	//   1. It reads the RAW `KeyBindings` lines back out of GConfig — the same strings the .ini
	//      holds — instead of trusting UTraceUserSettings::KeyBindings, which any Save() in the run
	//      would already have rewritten from the current table.
	//   2. For every line naming an action that still exists, it asserts the resolved binding IS the
	//      key that line names. That is what "every other bind still lands on the right action"
	//      means, and a renumber would break it wholesale.
	//   3. For every line naming an action that no longer exists — `SwapWeapon=F`, and pre-v3
	//      `Boost=E` — it asserts the line was DROPPED and names it. Dropped, not defaulted-over:
	//      the check below proves no surviving action inherited that key.
	//   4. For every action the file does NOT mention, it asserts the shipped default is in place.
	//
	// Together those four exhaust the file: every line is either honoured or explicitly discarded,
	// and every action is either from the file or from the defaults. There is no third outcome for a
	// renumber to hide in.
	// =============================================================================================
	void VerifyBinds()
	{
		UTraceUserSettings& Settings = UTraceUserSettings::Get();
		const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();

		UE_LOG(LogTraceGame, Display,
			TEXT("[VerifyBinds] ===== spec v15 s5: does a pre-v15 TraceUserSettings.ini still load correctly? ====="));

		// Straight out of the config cache, so this reports the FILE and not our own idea of it.
		TArray<FString> RawLines;
		const FString Section = UTraceUserSettings::StaticClass()->GetPathName();
		const FString Filename = UTraceUserSettings::StaticClass()->GetConfigName();
		if (GConfig != nullptr)
		{
			GConfig->GetArray(*Section, TEXT("KeyBindings"), RawLines, Filename);
		}

		UE_LOG(LogTraceGame, Display, TEXT("[VerifyBinds]   file '%s' section '%s' holds %d KeyBindings line(s)."),
			*Filename, *Section, RawLines.Num());

		int32 Failures = 0;
		int32 Honoured = 0;
		int32 Dropped = 0;
		TSet<FString> NamedIds;

		for (const FString& Line : RawLines)
		{
			FString ConfigId;
			FString KeyName;
			if (!Line.Split(TEXT("="), &ConfigId, &KeyName))
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[VerifyBinds]   '%s' is not 'Id=Key'; skipped."), *Line);
				continue;
			}
			ConfigId.TrimStartAndEndInline();
			KeyName.TrimStartAndEndInline();
			NamedIds.Add(ConfigId.ToLower());

			const int32 Index = Table.IndexOfByPredicate(
				[&ConfigId](const FTraceInputActionInfo& Info) { return ConfigId.Equals(Info.ConfigId, ESearchCase::IgnoreCase); });

			if (Index == INDEX_NONE)
			{
				++Dropped;
				UE_LOG(LogTraceGame, Display,
					TEXT("[VerifyBinds]   DROPPED  '%s' — no action by that ConfigId any more. This is the case ")
					TEXT("spec v15 s5 is about; a 'SwapWeapon=F' line from a pre-v15 build lands here."),
					*Line);
				continue;
			}

			const FKey Wanted(*KeyName);
			const FKey Actual = Settings.GetKey(static_cast<ETraceInputAction>(Index));
			const bool bWantsUnbound = KeyName.IsEmpty() || KeyName.Equals(TEXT("None"), ESearchCase::IgnoreCase);
			const bool bOk = bWantsUnbound ? !Actual.IsValid() : (Actual == Wanted);

			if (bOk)
			{
				++Honoured;
				UE_LOG(LogTraceGame, Display, TEXT("[VerifyBinds]   ok       %-18s -> %-16s (%s)"),
					*ConfigId, *KeyName, Table[Index].DisplayName);
			}
			else
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[VerifyBinds]   WRONG    %-18s asked for '%s' but %s resolved to '%s' — the file has been ")
					TEXT("read BY POSITION somewhere."),
					*ConfigId, *KeyName, Table[Index].DisplayName, *Actual.GetFName().ToString());
			}
		}

		// Everything the file did not mention must be sitting on its shipped default. This is the half
		// that catches a dropped line quietly leaking its key onto a neighbour.
		for (int32 Index = 0; Index < Table.Num(); ++Index)
		{
			if (NamedIds.Contains(FString(Table[Index].ConfigId).ToLower()))
			{
				continue;
			}

			const FKey Actual = Settings.GetKey(static_cast<ETraceInputAction>(Index));
			const FKey Default = Table[Index].DefaultKey();
			if (Actual == Default)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[VerifyBinds]   default  %-18s -> %s"),
					Table[Index].ConfigId, *Actual.GetFName().ToString());
			}
			else
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[VerifyBinds]   WRONG    %s is not named in the file, so it should be its default '%s', but it is '%s'."),
					Table[Index].ConfigId, *Default.GetFName().ToString(), *Actual.GetFName().ToString());
			}
		}

		// ---- THE VACUITY GUARD, and it is the most important line in this command --------------------
		//
		// Everything above passes trivially on a file whose bindings are all defaults, or whose lines
		// happen to sit in table order: by-ConfigId and by-position agree there, so the run would prove
		// nothing about the renumber it exists to be worried about. So COUNT the disagreement. This is
		// what a positional loader — the bug — would have produced from these very lines, and if it is
		// identical to what we got, the fixture cannot tell the two loaders apart and this run is not
		// evidence.
		int32 PositionalWouldDiffer = 0;
		for (int32 Line = 0; Line < RawLines.Num() && Line < Table.Num(); ++Line)
		{
			FString ConfigId;
			FString KeyName;
			if (!RawLines[Line].Split(TEXT("="), &ConfigId, &KeyName))
			{
				continue;
			}
			KeyName.TrimStartAndEndInline();

			const FKey AsPositional(*KeyName);
			if (Settings.GetKey(static_cast<ETraceInputAction>(Line)) != AsPositional)
			{
				++PositionalWouldDiffer;
			}
		}

		if (PositionalWouldDiffer == 0)
		{
			++Failures;
			UE_LOG(LogTraceGame, Error,
				TEXT("[VerifyBinds]   VACUOUS: reading this file BY POSITION would have produced the same ")
				TEXT("bindings as reading it by ConfigId, so it cannot tell a correct loader from a ")
				TEXT("renumbered one. Use a file whose binds are non-default and out of table order."));
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[VerifyBinds]   discriminating: a by-POSITION read of this file would have put %d of %d ")
				TEXT("action(s) on the wrong key. It did not, which is the claim."),
				PositionalWouldDiffer, Table.Num());
		}

		// The removed action, by name. Stated separately because "SwapWeapon is gone" is the actual
		// requirement and an empty ini would otherwise let every check above pass vacuously.
		const bool bSwapGone = Table.IndexOfByPredicate(
			[](const FTraceInputActionInfo& Info) { return FCString::Stricmp(Info.ConfigId, TEXT("SwapWeapon")) == 0; }) == INDEX_NONE;
		if (!bSwapGone)
		{
			++Failures;
			UE_LOG(LogTraceGame, Error, TEXT("[VerifyBinds]   SwapWeapon is STILL in the action table — spec v15 s5 removes it."));
		}

#define TRACE_VERIFYBINDS_ARGS \
	(Failures == 0) ? TEXT("THE PRE-v15 FILE LOADS CORRECTLY") : TEXT("A BINDING LANDED ON THE WRONG ACTION"), \
	Honoured, Dropped, Table.Num(), bSwapGone ? 1 : 0, Failures

#define TRACE_VERIFYBINDS_TEXT \
	TEXT("[VerifyBinds] VERDICT: %s. %d line(s) honoured, %d stale line(s) dropped, %d action(s) in the ") \
	TEXT("table, SwapWeapon removed=%d, %d failure(s).")

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_VERIFYBINDS_TEXT, TRACE_VERIFYBINDS_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_VERIFYBINDS_TEXT, TRACE_VERIFYBINDS_ARGS);
		}

#undef TRACE_VERIFYBINDS_ARGS
#undef TRACE_VERIFYBINDS_TEXT
	}

	FAutoConsoleCommand CmdVerifyBinds(
		TEXT("Trace.Settings.VerifyBinds"),
		TEXT("Spec v15 s5. Reads the raw KeyBindings lines back out of TraceUserSettings.ini and proves ")
		TEXT("every line naming a live action resolved to that action's key, every line naming a dead one ")
		TEXT("(SwapWeapon, Boost) was dropped, and every unmentioned action is on its shipped default."),
		FConsoleCommandDelegate::CreateStatic(&VerifyBinds));

	FAutoConsoleCommand CmdVerifyBindableKeys(
		TEXT("Trace.VerifyBindableKeys"),
		TEXT("Spec v10 s8. Prints whether every mouse button passes UTraceUserSettings::IsBindableKey, ")
		TEXT("whether Escape and AnyKey are still excluded, and round-trips LeftMouseButton and ")
		TEXT("RightMouseButton through a real binding and the ini's string form. Restores the bindings it ")
		TEXT("touches."),
		FConsoleCommandDelegate::CreateStatic(&VerifyBindableKeys));
}
