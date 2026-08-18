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
	 * *** SPEC v25 §7 — PARRY IS THE RIGHT MOUSE BUTTON. *** Verbatim: "Remove the default bind of
	 * right clicking to throw the core while carrying it. Make the default key bind for parrying
	 * right click mouse."
	 *
	 * This reverses the v3 §3 reasoning that put parry on Q, and the reversal is sound because the
	 * premise has gone: v3 refused right mouse because "RIGHT MOUSE IS ALREADY PASS, and overloading
	 * it onto the carrier's own pass button would make the two mechanics unusable together". The
	 * same note that moves parry here takes the throw off this button (see Default_Pass below), so
	 * the collision v3 was avoiding no longer exists.
	 *
	 * WHAT ELSE THIS BUTTON NOW DOES, AND WHY IT IS NOT A SECOND COLLISION. Spec v25 §2 makes right
	 * mouse the Core-PULL input during a turnover. Parry and pull are the same physical button and
	 * are dispatched from the same handler (ATracePlayerController::OnParryStarted), because they
	 * can never both be legal at the same instant:
	 *
	 *     PARRY requires you to BE CARRYING the Core   (ETraceParryRefusal::NotCarrying)
	 *     PULL  requires you to NOT be carrying it, and to be hovering a turned-over Core with line
	 *           of sight, on the team that did NOT drop it, inside the 5 s window
	 *
	 * Those two predicates are mutually exclusive on `is this pawn the carrier`, so the press is
	 * delivered to both verbs and at most one authoritative gate can accept it. Neither eats the
	 * other; there is no priority to get wrong. See OnParryStarted for the full argument.
	 */
	FKey Default_Parry()       { return EKeys::RightMouseButton; }
	FKey Default_Fire()        { return EKeys::LeftMouseButton; }
	/**
	 * *** SPEC v25 §7 — THE THROW HAS NO DEFAULT KEY OF ITS OWN. *** It used to be right mouse; the
	 * note removes that bind and does not name a replacement, so this returns an INVALID FKey and
	 * the row ships UNBOUND.
	 *
	 * THE THROW IS NOT LOST, WHICH IS THE WHOLE REASON "NONE" IS THE RIGHT ANSWER RATHER THAN A
	 * CONSOLATION KEY. Mouse 1 already throws/passes the Core while carrying, in BOTH modes and
	 * from before this note:
	 *
	 *     goals mode     ATraceCharacter::DoFirePressed -> DoPassPressed -> the charged throw
	 *                    (the HUD's own carry caption is "LMB  -  THROW")
	 *     endzones mode  the same call, driving the 0.5 s hover-hold pass
	 *
	 * So the carrier's throw button is unchanged and is the one the HUD has always taught. This row
	 * is a SECOND, optional route to the same verb, and inventing a default for it — Q, freed by
	 * parry moving off it, was the obvious candidate — would have added a control nobody asked for,
	 * put a live key on a row the player has never needed, and made the freed key unavailable for
	 * the next thing. The row stays on the options screen so anyone who wants a dedicated throw key
	 * can bind one; it simply starts empty.
	 *
	 * AN INVALID DEFAULT IS A SUPPORTED STATE, not a special case. RefreshFromConfig seeds the table
	 * with it, ApplyControlSettings' MapButton skips invalid keys so no mapping is created,
	 * IsAtDefaults compares FKey() to FKey() and agrees, RedeliverHeldPressEdges' IsHeld already
	 * tests Key.IsValid() first, and DescribeKey prints "UNBOUND". Every one of those paths existed
	 * for ClearKey; this default only reaches them one step earlier.
	 */
	FKey Default_Pass()        { return FKey(); }
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
	/**
	 * SPEC v16 §1, verbatim: "R to reload."
	 *
	 * R is NAMED BY THE DOC, so there is no key to choose — only a collision to check. Claimed
	 * already: WASD, Space, LeftCtrl, LeftShift, Q, mouse1, mouse2, Tab, 1, 2, E, V. R is free, so
	 * this default steals nothing on a first run (SetKey's stealing rule would log it if it did), and
	 * it is the genre convention besides.
	 */
	FKey Default_Reload()           { return EKeys::R; }
	/**
	 * SPEC v26 §1, verbatim: "Make parry and pull core two separate binds in the settings menu."
	 *
	 * NO KEY IS NAMED BY THE NOTE, so this one is a choice and it is argued rather than asserted.
	 * Claimed already: WASD, Space, LeftControl, LeftShift, mouse1, mouse2, Tab, 1, 2, E, V, R.
	 *
	 * F WINS ON TWO COUNTS. It is unclaimed — spec v15 §5 vacated it when it deleted the SwapWeapon
	 * toggle — and "F is the key you press to grab the thing you are looking at" is the single
	 * strongest convention this genre has, which matters for a verb whose whole interaction is
	 * "hover the turned-over Core and hold".
	 *
	 * Q WAS THE OTHER CANDIDATE and was passed over on purpose. Spec v25 §7 freed it when parry moved
	 * to right mouse, so it is available and it is close to WASD. But the pull is a HOLD taken while
	 * still steering, and Q asks the ring finger to leave A; F is under an index finger that is
	 * already resting on D. Q also carries a decade of "that is the ability key" muscle memory from
	 * other games in this genre, and this action is not an ability.
	 *
	 * RIGHT MOUSE IS DELIBERATELY NOT THE DEFAULT, even though that is where v25 §2 put the pull.
	 * The note asks for TWO binds; shipping both on one key would satisfy the letter (two rows on the
	 * page) and none of the intent. Parry keeps the button v25 §7 explicitly named for it.
	 *
	 * SetKey's stealing rule would log it if this default took a key from another action on a first
	 * run. It does not.
	 */
	FKey Default_PullCore()         { return EKeys::F; }
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
		// *** SPEC v25 §7. BOTH ConfigIds ON THESE TWO ROWS ARE DELIBERATELY NEW STRINGS. ***
		//
		// Read this before "tidying" them back. Changing a DEFAULT key does nothing at all for anyone
		// who has ever saved a setting, because Save() -> FlattenToConfig() writes a line for EVERY
		// action, so a returning player's TraceUserSettings.ini already contains
		//
		//     KeyBindings=Parry=Q
		//     KeyBindings=Pass=RightMouseButton
		//
		// and RefreshFromConfig honours both, over the top of whatever this table says. The owner's
		// own machine has exactly that file. Ship the new defaults under the old ids and right mouse
		// still throws the Core, parry is still on Q, and the note reads as un-done.
		//
		// So the ids move with the meaning, which is this codebase's established migration and not a
		// new idea: ETraceInputAction::Parry's own comment records "Boost" -> "Parry" doing precisely
		// this in spec v3, and spec v15's "SwapWeapon" removal relies on the same drop rule. A line
		// naming a ConfigId the table no longer has is DISCARDED by RefreshFromConfig (see the parse
		// loop) and the action falls back to its shipped default. Trace.Settings.VerifyBinds prints
		// every dropped line by name, so this is checkable rather than assertable.
		//
		// WHAT A RETURNING PLAYER LOSES: a hand-rebound parry or throw key, once. That is the price
		// of the note landing at all, it is paid a single time, and it is loud in the log.
		//
		// The new ids also say what the buttons now DO. "ParryPull" is one button with two verbs
		// (spec v25 §2's Core pull rides the parry bind); "ThrowCore" is the mode-neutral name for
		// the verb mouse 1 already performs while carrying.
		//
		// NOT SAFE TO RENAME, for contrast, and this is why the rule is per-row rather than blanket:
		// "Ability", "AbilitySecondary" and "Reload" are looked up BY STRING outside this file
		// (UTraceAbilityInputRelay, ATraceHUD's ability and reload rows). Neither of these two is.
		// *** SPEC v26 §1 RENAMES THE LABEL AND NOT THE ID. *** "Make parry and pull core two separate
		// binds in the settings menu": the pull moved out to its own row at the bottom of this table,
		// so this row is the parry alone and the "/ PULL CORE" half of its label is now a lie.
		//
		// The ConfigId stays "ParryPull". Every id migration costs a returning player their hand-rebound
		// key exactly once (RefreshFromConfig drops any line naming an id this table no longer has), v25
		// §7 already spent that cost to put parry on this button, and spending it again to make the
		// string prettier would silently un-rebind everybody who has touched their parry since — for no
		// behaviour change at all. DisplayName is persisted nowhere and is therefore free to correct.
		{ ETraceInputAction::Parry,       TEXT("ParryPull"),   TEXT("PARRY"),        &Default_Parry       },
		{ ETraceInputAction::Fire,        TEXT("Fire"),        TEXT("FIRE"),         &Default_Fire        },
		{ ETraceInputAction::Pass,        TEXT("ThrowCore"),   TEXT("THROW / PASS CORE"), &Default_Pass  },
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
		// SPEC v16 §1, "R to reload". Same reasoning as every row above: the options screen's rebind
		// list IS this table walked in order, so an action missing from here is an action the player
		// cannot see or rebind however well it is wired up in the controller.
		{ ETraceInputAction::Reload,           TEXT("Reload"),           TEXT("RELOAD"),            &Default_Reload           },
		// SPEC v26 §1 — "Make parry and pull core two separate binds in the settings menu."
		//
		// THIS LINE IS WHAT PUTS THE PULL ON THE KEYBIND PAGE, and nothing else does. The options
		// screen's rebind list IS this table walked in order (FTraceOptionsMenu::RebuildRows), so an
		// action that is not here is an action the player cannot see or rebind however well it is
		// wired up in the controller. That is the same sentence the EquipKnife, Ability and Reload
		// rows above already carry, and it is the whole reason each of them exists.
		//
		// LAST IN THE LIST, hence last on the page, because ETraceInputAction is append-only (see the
		// enumerator's comment). PULL CORE sitting under RELOAD rather than next to PARRY is the price
		// of never renumbering the runtime table, and it is the right trade: the ordering is cosmetic,
		// a renumber is a live bug.
		{ ETraceInputAction::PullCore,         TEXT("PullCore"),         TEXT("PULL CORE"),         &Default_PullCore         },
	};

	static_assert(static_cast<int32>(ETraceInputAction::Count) == 17,
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
	// for FIRE and — since spec v25 §7 — RightMouseButton for PARRY (see the table at the top of
	// this file; it was PASS until v25 moved the throw off the button), so a rule that rejected
	// mouse buttons would have refused to load the game's own default bindings on the first run —
	// which is not what happens. Trace.VerifyBindableKeys prints the verdict for every mouse button
	// in the build so this can be checked rather than argued about.
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
		//
		// SPEC v25 §7 MOVED WHICH ACTION THE RIGHT BUTTON BELONGS TO — it was PASS, it is now PARRY —
		// so the snapshot below is taken over the WHOLE table rather than over a hand-written list of
		// the two victims. A list of names is exactly what went stale here, and a restore that misses
		// an action leaves the player's controls damaged by a diagnostic (Trace.V10.RebindFire learned
		// the same lesson the same way; see its comment).
		TMap<ETraceInputAction, FKey> Before;
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			Before.Add(Info.Action, Settings.GetKey(Info.Action));
		}
		const FKey Saved = Settings.GetKey(ETraceInputAction::Dash);

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

		// Put everything back, including whichever actions SetKey stole the two buttons from. Dash
		// last, so restoring a displaced action cannot take the button back off it.
		//
		// ClearKey for the invalid case rather than SetKey: SetKey REFUSES an invalid key by design
		// (an unparseable .ini line must never be able to wipe a binding), so an action that was
		// legitimately UNBOUND before this ran — which since spec v25 §7 is the shipped state of the
		// throw row — could not be restored through SetKey at all.
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			if (Info.Action == ETraceInputAction::Dash)
			{
				continue;
			}

			const FKey Was = Before[Info.Action];
			if (Settings.GetKey(Info.Action) == Was)
			{
				continue;
			}

			if (Was.IsValid())
			{
				Settings.SetKey(Info.Action, Was);
			}
			else
			{
				Settings.ClearKey(Info.Action);
			}
		}
		Settings.SetKey(ETraceInputAction::Dash, Saved);

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

		// A WARNING RATHER THAN A FAILURE SINCE SPEC v26 §1, and only because the fixture arm below now
		// supplies the discrimination this file could not. Before v26 there was no other source of it,
		// so "your file cannot tell the two loaders apart" was correctly fatal; now it is a statement
		// about the machine the command was run on, and failing a verification because the person
		// running it never rebound anything would be noise. If the fixture arm does not run or is
		// itself vacuous, this becomes a failure again — see the flag's use below.
		bool bFileCannotDiscriminate = false;
		if (PositionalWouldDiffer == 0)
		{
			bFileCannotDiscriminate = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[VerifyBinds]   this FILE cannot discriminate: reading it BY POSITION would have ")
				TEXT("produced the same bindings as reading it by ConfigId, because its lines happen to sit ")
				TEXT("in table order. The v26 s1 fixture arm below supplies the discrimination instead."));
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[VerifyBinds]   discriminating: a by-POSITION read of this file would have put %d of %d ")
				TEXT("action(s) on the wrong key. It did not, which is the claim."),
				PositionalWouldDiffer, Table.Num());
		}

		// ---- SPEC v26 §1 — THE APPEND-ONLY PROOF, ON A FIXTURE THIS COMMAND BUILDS ITSELF ----------
		//
		// §1 adds ETraceInputAction::PullCore. Appending an enumerator is safe ONLY because the file is
		// keyed by ConfigId string and never by position, and the guard above can only test that claim
		// as well as whoever is running it happens to have rebound their keys. On the machine this was
		// written on it reported VACUOUS — every line sat in table order — so the strongest check in
		// the command proved nothing about the very change that most needed it.
		//
		// So this arm stops depending on the player's file. It writes a fixture whose lines are
		// deliberately OUT OF TABLE ORDER, onto distinctive keys, including one line for the brand-new
		// PullCore and one naming an action that no longer exists, then runs the REAL loader
		// (RefreshFromConfig, the same call Get() makes on first use) over it.
		//
		// IN MEMORY ONLY, AND RESTORED. The fixture is written into UTraceUserSettings::KeyBindings —
		// the member RefreshFromConfig actually parses — and NOT into GConfig. That is a correction,
		// not a shortcut: the first version of this arm called GConfig->SetArray and every fixture line
		// came back as the shipped default, because RefreshFromConfig re-parses the MEMBER array that
		// LoadConfig populated at class load and never re-reads the config cache. Writing the member is
		// both the thing that works and the thing that cannot touch disk: nothing here calls Save(),
		// and the original array is put back and re-loaded before the command returns. That matters
		// more than usual — this can be run mid-match from the console.
		int32 FixtureDiscriminates = 0;
		{
			struct FFixtureLine { const TCHAR* ConfigId; const TCHAR* KeyName; };

			// REVERSED relative to the table, and every key non-default, so a positional loader cannot
			// accidentally agree. "SwapWeapon" is the dead id spec v15 §5 removed and is here to prove
			// the drop path still drops rather than shifting everything after it by one.
			const FFixtureLine Fixture[] =
			{
				{ TEXT("PullCore"),         TEXT("Nine")  },   // the new row — LAST in the table, FIRST here
				{ TEXT("SwapWeapon"),       TEXT("Seven") },   // dead id: must be DROPPED, not shifted
				{ TEXT("Reload"),           TEXT("Eight") },
				{ TEXT("AbilitySecondary"), TEXT("Six")   },
				{ TEXT("ParryPull"),        TEXT("Five")  },
				{ TEXT("MoveForward"),      TEXT("Four")  },   // row 0 of the table — LAST here
			};

			TArray<FString> FixtureLines;
			for (const FFixtureLine& Line : Fixture)
			{
				FixtureLines.Add(FString::Printf(TEXT("%s=%s"), Line.ConfigId, Line.KeyName));
			}

			const TArray<FString> SavedKeyBindings = Settings.KeyBindings;
			Settings.KeyBindings = FixtureLines;
			Settings.RefreshFromConfig();

			int32 FixtureFailures = 0;

			for (int32 Line = 0; Line < UE_ARRAY_COUNT(Fixture); ++Line)
			{
				const FString Id(Fixture[Line].ConfigId);
				const FKey Wanted(Fixture[Line].KeyName);

				const int32 Index = Table.IndexOfByPredicate(
					[&Id](const FTraceInputActionInfo& Info) { return Id.Equals(Info.ConfigId, ESearchCase::IgnoreCase); });

				if (Index == INDEX_NONE)
				{
					// The dead id. Nobody may be holding its key — that is what "dropped" means, as
					// opposed to "shifted onto the neighbour".
					const bool bNobodyHasIt = Table.IndexOfByPredicate(
						[&Settings, &Wanted](const FTraceInputActionInfo& Info)
						{ return Settings.GetKey(Info.Action) == Wanted; }) == INDEX_NONE;
					if (!bNobodyHasIt)
					{
						++FixtureFailures;
						UE_LOG(LogTraceGame, Error,
							TEXT("[VerifyBinds]   v26 s1 FIXTURE: the dead id '%s' was not dropped — something ")
							TEXT("inherited '%s'."), *Id, Fixture[Line].KeyName);
					}
					continue;
				}

				const FKey Actual = Settings.GetKey(static_cast<ETraceInputAction>(Index));
				if (Actual != Wanted)
				{
					++FixtureFailures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[VerifyBinds]   v26 s1 FIXTURE: '%s' (%s) asked for '%s' and resolved to '%s'. ")
						TEXT("Appending PullCore RENUMBERED the table."),
						*Id, Table[Index].DisplayName, Fixture[Line].KeyName, *Actual.GetFName().ToString());
				}

				// Would a positional loader have got this line wrong? It must, for at least one line,
				// or the fixture is as vacuous as the file was.
				if (Table.IsValidIndex(Line) && Index != Line)
				{
					++FixtureDiscriminates;
				}
			}

			if (FixtureDiscriminates == 0)
			{
				++FixtureFailures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[VerifyBinds]   v26 s1 FIXTURE is VACUOUS: its lines are in table order after all."));
			}

			Failures += FixtureFailures;

			if (FixtureFailures == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[VerifyBinds]   ok       v26 s1 APPEND-ONLY PROOF: a %d-line out-of-order fixture "
					     "(PullCore first, MoveForward last, one dead id) loaded through the real "
					     "RefreshFromConfig and every line landed on the action it NAMES. %d of them would "
					     "have landed on the wrong action under a by-position read, so this fixture can tell "
					     "the two loaders apart. The player's file on disk was not touched."),
					static_cast<int32>(UE_ARRAY_COUNT(Fixture)), FixtureDiscriminates);
			}

			// Put the player's own lines back and re-load them, so nothing after this command — and in
			// particular no later Save() by the options screen — can see the fixture.
			Settings.KeyBindings = SavedKeyBindings;
			Settings.RefreshFromConfig();
		}

		// The vacuity guard's verdict, decided once BOTH sources of discrimination have been tried.
		// Neither one on its own is enough: a file in table order proves nothing, and a fixture that
		// failed to run leaves the renumber claim untested.
		if (bFileCannotDiscriminate && FixtureDiscriminates == 0)
		{
			++Failures;
			UE_LOG(LogTraceGame, Error,
				TEXT("[VerifyBinds]   VACUOUS: neither the player's file nor the v26 s1 fixture could tell a ")
				TEXT("by-ConfigId loader from a by-position one, so this run says nothing about renumbering."));
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
