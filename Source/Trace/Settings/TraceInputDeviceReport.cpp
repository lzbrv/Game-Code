// Trace — D32-DINPUT: the input-device report. See TraceInputDeviceReport.h for why this exists.

#include "Settings/TraceInputDeviceReport.h"

#include "Trace.h"

#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "GenericPlatform/InputDeviceRegistry.h"
#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"
#include "Misc/CoreMiscDefines.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

// A NAMED namespace, not an anonymous one. Scripts/check-jumbo-build-collisions.py gates the build
// on this: two files each defining `namespace { ... Foo }` are legal apart and one redefinition once
// UBT concatenates them into a unity translation unit, which builds clean on macOS and fails on
// Windows with MSVC C2084. Naming it after the file is the fix that file's own docs prescribe.
namespace TraceInputDeviceReport
{
	/** One key the witness has seen since launch. */
	struct FWitnessedKey
	{
		int32  EventCount     = 0;
		double FirstSeenTime  = 0.0;
		double LastSeenTime   = 0.0;
		float  LastValue      = 0.0f;
		int32  LastDeviceId   = INDEX_NONE;
		int32  LastUserIndex  = INDEX_NONE;
		bool   bSawAnalog     = false;
		bool   bSawDigital    = false;
	};

	/** Bumped whenever the report's CONTENT would change. The ticker only rewrites the file on a bump. */
	static uint64 Generation = 1;

	/** Generation the file on disk was written from. Starts different from Generation to force one write. */
	static uint64 WrittenGeneration = 0;

	static TMap<FName, FWitnessedKey> WitnessedKeys;

	/**
	 * Count of NON-gamepad key events, deliberately WITHOUT their key names.
	 *
	 * The count answers the one question worth asking about the keyboard here — "is this machine
	 * delivering keyboard input to the game at all?" — and naming the keys would answer nothing more
	 * while turning a file the owner asks players to send him into a keylog. That is not a trade
	 * worth making for a diagnostic.
	 */
	static int64 NonGamepadKeyEvents = 0;

	/** True once any device-connection change has been observed, so the report can say "since launch". */
	static int32 ConnectionChangeCount = 0;

	static void Record(const FKey& Key, float Value, FInputDeviceId DeviceId, int32 UserIndex, bool bAnalog)
	{
		if (!Key.IsGamepadKey())
		{
			// Non-gamepad traffic is counted and discarded — see NonGamepadKeyEvents above.
			++NonGamepadKeyEvents;
			return;
		}

		const FName KeyName = Key.GetFName();
		const double Now = FPlatformTime::Seconds();

		FWitnessedKey* Existing = WitnessedKeys.Find(KeyName);
		if (!Existing)
		{
			Existing = &WitnessedKeys.Add(KeyName);
			Existing->FirstSeenTime = Now;

			// A key never seen before changes the report. A key seen 4000 times does not, which is
			// why the bump lives here and not below: a player holding a stick would otherwise rewrite
			// the file every 2 s for as long as they played.
			++Generation;
		}

		Existing->EventCount++;
		Existing->LastSeenTime  = Now;
		Existing->LastValue     = Value;
		Existing->LastDeviceId  = DeviceId.GetId();
		Existing->LastUserIndex = UserIndex;
		Existing->bSawAnalog   |= bAnalog;
		Existing->bSawDigital  |= !bAnalog;
	}

	/**
	 * Watches input at the Slate layer and consumes nothing.
	 *
	 * EVERY HANDLER RETURNS FALSE. In Slate that means "not handled, keep routing", so registering
	 * this processor cannot swallow a key, cannot change focus and cannot alter what the game
	 * receives. That is the entire safety argument for having it live in a Shipping build.
	 */
	class FWitness : public IInputProcessor
	{
	public:
		virtual ~FWitness() override = default;

		virtual void Tick(const float /*DeltaTime*/, FSlateApplication& /*SlateApp*/, TSharedRef<ICursor> /*Cursor*/) override
		{
			// Nothing to do per-frame: everything is event driven. Pure virtual, so it must exist.
		}

		virtual bool HandleKeyDownEvent(FSlateApplication& /*SlateApp*/, const FKeyEvent& InKeyEvent) override
		{
			Record(InKeyEvent.GetKey(), 1.0f, InKeyEvent.GetInputDeviceId(), static_cast<int32>(InKeyEvent.GetUserIndex()), /*bAnalog*/ false);
			return false;
		}

		virtual bool HandleAnalogInputEvent(FSlateApplication& /*SlateApp*/, const FAnalogInputEvent& InAnalogInputEvent) override
		{
			Record(InAnalogInputEvent.GetKey(), InAnalogInputEvent.GetAnalogValue(), InAnalogInputEvent.GetInputDeviceId(), static_cast<int32>(InAnalogInputEvent.GetUserIndex()), /*bAnalog*/ true);
			return false;
		}

		virtual const TCHAR* GetDebugName() const override { return TEXT("TraceInputDeviceWitness"); }
	};

	static TSharedPtr<FWitness> Witness;

	static const TCHAR* LexConnectionState(EInputDeviceConnectionState State)
	{
		switch (State)
		{
		case EInputDeviceConnectionState::Invalid:      return TEXT("Invalid");
		case EInputDeviceConnectionState::Unknown:      return TEXT("Unknown");
		case EInputDeviceConnectionState::Disconnected: return TEXT("Disconnected");
		case EInputDeviceConnectionState::Connected:    return TEXT("Connected");
		default:                                        return TEXT("<unhandled>");
		}
	}

	static void OnDeviceConnectionChanged(EInputDeviceConnectionState /*NewState*/, FPlatformUserId /*UserId*/, FInputDeviceId /*DeviceId*/)
	{
		++ConnectionChangeCount;
		++Generation;
	}

	/**
	 * Handle for the connection-change subscription.
	 *
	 * IPlatformInputDeviceMapper::OnInputDeviceConnectionChange is a STATIC member of the mapper, so
	 * it outlives every game instance. Subscribing with AddStatic and then trying to undo it with
	 * RemoveAll(this) would be a no-op — there is no `this` bound to a static delegate — and each new
	 * game instance would stack another subscription on the last one's corpse. Keep the handle.
	 */
	static FDelegateHandle ConnectionChangeHandle;
}

// -------------------------------------------------------------------------------------------------
// The report
// -------------------------------------------------------------------------------------------------

FString UTraceInputDeviceReportSubsystem::BuildReport()
{
	using namespace TraceInputDeviceReport;

	TStringBuilder<8192> Out;

	Out << TEXT("Trace - input device report\n");
	Out << TEXT("===========================\n\n");
	Out << TEXT("Written        : ") << *FDateTime::Now().ToString() << TEXT("\n");
	Out << TEXT("Platform       : ") << ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()) << TEXT("\n");
	Out << TEXT("Build config   : ") << LexToString(FApp::GetBuildConfiguration()) << TEXT("\n");
	Out << TEXT("Uptime         : ") << *FString::Printf(TEXT("%.1f s"), FPlatformTime::Seconds() - GStartTime) << TEXT("\n\n");

	// ---------------------------------------------------------------------------------------------
	// 1. Which input backends are actually running
	//
	// This is the section that proves or disproves the plugin change in Trace.uproject. RawInput is
	// Win64-only and Optional, so on macOS "RawInput module loaded: NO" is the CORRECT answer and not
	// a fault; on Windows a NO means the plugin did not build or did not load, and no amount of
	// per-device configuration below it will do anything.
	// ---------------------------------------------------------------------------------------------
	Out << TEXT("1. INPUT BACKENDS\n");
	Out << TEXT("   XInputDevice module loaded : ")
		<< (FModuleManager::Get().IsModuleLoaded(TEXT("XInputDevice")) ? TEXT("YES") : TEXT("NO"))
		<< TEXT("   (Windows Xbox-style pads; Windows only)\n");
	Out << TEXT("   RawInput module loaded     : ")
		<< (FModuleManager::Get().IsModuleLoaded(TEXT("RawInput")) ? TEXT("YES") : TEXT("NO"))
		<< TEXT("   (Windows generic HID / \"DirectInput\" devices; Windows only)\n");
	Out << TEXT("   GameInputBase module loaded: ")
		<< (FModuleManager::Get().IsModuleLoaded(TEXT("GameInputBase")) ? TEXT("YES") : TEXT("NO"))
		<< TEXT("   (not enabled by this project)\n");
	Out << TEXT("   On macOS all three are expected to read NO. The Mac pad path is HIDInputInterface,\n");
	Out << TEXT("   which is built into the engine's ApplicationCore and is not a plugin, so it has no\n");
	Out << TEXT("   module to report here - section 2 is where a Mac pad shows up.\n\n");

	// ---------------------------------------------------------------------------------------------
	// 2. What the engine's device mapper knows
	// ---------------------------------------------------------------------------------------------
	Out << TEXT("2. DEVICES THE ENGINE HAS MAPPED\n");

	if (FSlateApplication::IsInitialized())
	{
		Out << TEXT("   FSlateApplication::IsGamepadAttached() : ")
			<< (FSlateApplication::Get().IsGamepadAttached() ? TEXT("true") : TEXT("false")) << TEXT("\n");
	}
	else
	{
		Out << TEXT("   FSlateApplication::IsGamepadAttached() : <Slate not initialised>\n");
	}
	Out << TEXT("   Connection changes since launch        : ") << ConnectionChangeCount << TEXT("\n\n");

	IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();

	TArray<FInputDeviceId> Devices;
	Mapper.GetAllInputDevices(Devices);

	if (Devices.Num() == 0)
	{
		Out << TEXT("   (none - the mapper has never been told about any input device)\n");
	}
	// Named so the loop can label it. Every platform maps keyboard and mouse onto one device that no
	// backend registers a descriptor for; the first run of this report printed it as a bare
	// "Device 0 ... (no descriptor registered)" and reading it back, that looks like a fault rather
	// than the single most normal line in the file. Say what it is.
	const FInputDeviceId DefaultDevice = Mapper.GetDefaultInputDevice();

	for (const FInputDeviceId DeviceId : Devices)
	{
		const FPlatformUserId OwningUser = Mapper.GetUserForInputDevice(DeviceId);

		Out << TEXT("   Device ") << DeviceId.GetId()
			<< TEXT("  state=") << LexConnectionState(Mapper.GetInputDeviceConnectionState(DeviceId))
			<< TEXT("  user=") << (OwningUser.IsValid() ? OwningUser.GetInternalId() : INDEX_NONE)
			<< (DeviceId == DefaultDevice ? TEXT("  [default keyboard/mouse device, not a gamepad]") : TEXT(""));

		// The descriptor is where a device's NAME lives. What is in it depends entirely on which
		// backend registered the device — see the note printed below the list.
		const TOptional<FInputDeviceDescriptor> Descriptor = FInputDeviceRegistry::FindDescriptor(DeviceId);
		if (Descriptor.IsSet())
		{
			Out << TEXT("\n            backend=\"") << *Descriptor->InputDeviceName.ToString() << TEXT("\"")
				<< TEXT("  hardware=\"") << *Descriptor->HardwareDeviceIdentifier.ToString() << TEXT("\"");
		}
		else
		{
			Out << TEXT("\n            (no descriptor registered for this device)");
		}
		Out << TEXT("\n");
	}

	Out << TEXT("\n   HOW MUCH OF A NAME TO EXPECT, BY PLATFORM - this differs and it matters:\n");
	Out << TEXT("     Windows + RawInput : hardware= is the raw HID device path, which contains the\n");
	Out << TEXT("                          VID and PID (e.g. \\\\?\\HID#VID_046D&PID_C21F#...). That is\n");
	Out << TEXT("                          the identifier a per-device mapping is keyed on. Send it.\n");
	Out << TEXT("     Windows + XInput   : hardware= is a fixed XInput identifier. XInput does not\n");
	Out << TEXT("                          expose product names at all, so no name is available.\n");
	Out << TEXT("     macOS              : backend=\"HIDInputInterface\", hardware=\"Mac_HIDController\"\n");
	Out << TEXT("                          for EVERY pad. The engine hardcodes that string, so the\n");
	Out << TEXT("                          report cannot tell a DualSense from an arcade stick here.\n");
	Out << TEXT("                          The VID/PID does exist in the engine's own log line\n");
	Out << TEXT("                          \"Controller attached: VendorID 0x.., ProductID 0x..\" -\n");
	Out << TEXT("                          but that is a UE_LOG, so it is compiled out of Shipping.\n");
	Out << TEXT("                          On a Mac, run a Development build to get the VID/PID.\n\n");

	// ---------------------------------------------------------------------------------------------
	// 3. What has actually arrived
	// ---------------------------------------------------------------------------------------------
	Out << TEXT("3. GAMEPAD KEYS SEEN SINCE LAUNCH\n");
	Out << TEXT("   Observed at the Slate layer, before the game viewport. Presence here proves input\n");
	Out << TEXT("   is REACHING THE PROCESS. It does NOT prove the game acted on it - that is a\n");
	Out << TEXT("   binding question, and the key names below are what a binding needs.\n\n");

	if (WitnessedKeys.Num() == 0)
	{
		Out << TEXT("   (nothing - no gamepad key or axis has arrived)\n");
		Out << TEXT("   If a device is listed in section 2 and this is empty, press every button and\n");
		Out << TEXT("   move every stick, then look again. If it is STILL empty, the device is known to\n");
		Out << TEXT("   the mapper but its input is not being delivered.\n\n");
		Out << TEXT("   ON WINDOWS, TRY THIS FIRST: quit, plug the controller in, THEN launch the game.\n");
		Out << TEXT("   RawInput registers its catch-all device ONCE at startup and only succeeds if a\n");
		Out << TEXT("   matching device is ALREADY CONNECTED at that moment (FRawInputWindows'\n");
		Out << TEXT("   constructor, via RegisterInputDevice, which returns INDEX_NONE when nothing\n");
		Out << TEXT("   connected matches). Plug in after launch and there is no re-registration path,\n");
		Out << TEXT("   so the device is simply never listened to. This is a plugin limitation, not a\n");
		Out << TEXT("   fault in your controller, and it is the single most likely reason for a pad that\n");
		Out << TEXT("   shows up in section 2 and does nothing here.\n");
	}
	else
	{
		// Sorted so two reports from the same player are diffable.
		TArray<FName> SortedKeys;
		WitnessedKeys.GetKeys(SortedKeys);
		SortedKeys.Sort(FNameLexicalLess());

		Out << TEXT("   key                              events   last     analog? device user\n");
		for (const FName KeyName : SortedKeys)
		{
			const FWitnessedKey& Seen = WitnessedKeys.FindChecked(KeyName);
			const FKey Key(KeyName);

			// bIsAnalogKey is the key's own METADATA; Seen.bSawAnalog is how the value actually
			// arrived. The two disagreeing is not a printing quirk, it is the RawInput axis trap —
			// see the paragraph printed after this table.
			const bool bIsAnalogKey = Key.IsValid() && Key.IsAnalog();

			Out << *FString::Printf(TEXT("   %-32s %6d   %+6.3f  %-7s %6d %4d%s\n"),
				*KeyName.ToString(),
				Seen.EventCount,
				Seen.LastValue,
				bIsAnalogKey ? TEXT("axis") : TEXT("button"),
				Seen.LastDeviceId,
				Seen.LastUserIndex,
				(Seen.bSawAnalog && !bIsAnalogKey) ? TEXT("  <-- analog value on a non-axis key") : TEXT(""));
		}

		Out << TEXT("\n   A \"<--\" line above is the known RawInput axis defect, not a device fault:\n");
		Out << TEXT("   the RawInput plugin registers GenericUSBController_Axis1..24 with the flag\n");
		Out << TEXT("   FKeyDetails::GamepadKey only, never FKeyDetails::Axis1D, so FKey::IsAnalog() is\n");
		Out << TEXT("   false for them. UPlayerInput::InputKey gates its analog path on IsAnalog(), and\n");
		Out << TEXT("   its non-analog path switches on IE_Pressed/IE_Repeat/IE_Released/IE_DoubleClick\n");
		Out << TEXT("   with no default case - so an IE_Axis event on such a key is dropped and never\n");
		Out << TEXT("   reaches Enhanced Input. Buttons are unaffected. The fix is config, not code:\n");
		Out << TEXT("   see the RawInputSettings block in Config/DefaultInput.ini.\n");
	}

	Out << TEXT("\n   Non-gamepad key events (keyboard/mouse), counted but deliberately not named: ")
		<< NonGamepadKeyEvents << TEXT("\n");
	Out << TEXT("   A zero there while the game is plainly playable means this witness is not running,\n");
	Out << TEXT("   and section 3 as a whole should not be trusted.\n\n");

	Out << TEXT("Send this file whole. The two things that make a pad mappable in minutes are the\n");
	Out << TEXT("hardware= string in section 2 and the key names in section 3.\n");

	return FString(Out.ToString());
}

FString UTraceInputDeviceReportSubsystem::GetReportPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("TraceInputDevices.txt"));
}

bool UTraceInputDeviceReportSubsystem::WriteReport(FString& OutPath)
{
	OutPath = GetReportPath();

	// FFileHelper, NOT UE_LOG. Shipping defines NO_LOGGING (see the header), so anything routed
	// through the log system would vanish for exactly the players this file exists to help.
	//
	// ForceUTF8WithoutBOM, and it is not cosmetic. SaveStringToFile defaults to AutoDetect, which
	// writes UTF-16LE-with-BOM the moment the text contains one non-ASCII character. The first
	// version of this file did exactly that — an em dash in the prose was enough — and the result was
	// a report that a player cannot usefully paste into a chat window and that half of the standard
	// command-line tools render as spaced-out gibberish. The report text is also kept ASCII-only for
	// the same reason; if you add prose here, do not reach for a typographic dash.
	return FFileHelper::SaveStringToFile(BuildReport(), *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// -------------------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------------------

bool UTraceInputDeviceReportSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// A dedicated server has no Slate application to register a witness with and no local player
	// holding a controller. Creating it there would be a ticker and a file write for nothing.
	return !IsRunningDedicatedServer();
}

void UTraceInputDeviceReportSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	using namespace TraceInputDeviceReport;

	Super::Initialize(Collection);

	if (FSlateApplication::IsInitialized() && !Witness.IsValid())
	{
		Witness = MakeShared<FWitness>();

		// Index 0: ahead of everything, so a key consumed by a menu or by the game is still counted.
		// Safe precisely because every handler returns false — see the FWitness comment.
		FSlateApplication::Get().RegisterInputPreProcessor(Witness, 0);
	}

	if (!ConnectionChangeHandle.IsValid())
	{
		ConnectionChangeHandle = IPlatformInputDeviceMapper::Get()
			.GetOnInputDeviceConnectionChange().AddStatic(&OnDeviceConnectionChanged);
	}

	// Poll rather than write on every change. A pad connecting can fire several changes in a row and
	// the first gamepad key of a session arrives on an arbitrary frame; 2 s coalesces all of that into
	// one write while still being far faster than a player can plug in a pad and go looking for the
	// file. The Generation/WrittenGeneration pair is what stops it writing when nothing has changed.
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float /*Delta*/) -> bool
		{
			if (WrittenGeneration != Generation)
			{
				const uint64 Writing = Generation;

				FString Path;
				if (WriteReport(Path))
				{
					// Store what was actually written, not the generation as of now: a device change
					// landing mid-write would otherwise be marked written without being in the file.
					WrittenGeneration = Writing;
				}
			}
			return true;
		}),
		2.0f);
}

void UTraceInputDeviceReportSubsystem::Deinitialize()
{
	using namespace TraceInputDeviceReport;

	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	if (ConnectionChangeHandle.IsValid())
	{
		IPlatformInputDeviceMapper::Get().GetOnInputDeviceConnectionChange().Remove(ConnectionChangeHandle);
		ConnectionChangeHandle.Reset();
	}

	if (Witness.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(Witness);
	}
	Witness.Reset();

	Super::Deinitialize();
}

// -------------------------------------------------------------------------------------------------
// Console commands
//
// NON-SHIPPING ONLY, AND THAT IS NOT AN OVERSIGHT. In Shipping this engine defines ALLOW_CONSOLE to
// ALLOW_CONSOLE_IN_SHIPPING == 0, so there is no console to type into; a command there would be dead
// code that the release gate (Scripts/build.sh --prove-shipping) exists to keep out. The file write
// above is the Shipping-reachable half, and it runs in every configuration.
// -------------------------------------------------------------------------------------------------

#if !UE_BUILD_SHIPPING

namespace TraceInputDeviceReport
{
	static void CmdDevices()
	{
		UE_LOG(LogTraceGame, Display, TEXT("\n%s"), *UTraceInputDeviceReportSubsystem::BuildReport());

		FString Path;
		const bool bWrote = UTraceInputDeviceReportSubsystem::WriteReport(Path);
		UE_LOG(LogTraceGame, Display, TEXT("D32-DINPUT: report %s %s"),
			bWrote ? TEXT("written to") : TEXT("COULD NOT BE WRITTEN to"), *Path);
	}

	/**
	 * Proves section 3 of the report is LIVE, and can fail.
	 *
	 * Section 3 is the half of the report that a player's answer actually turns on, and until this
	 * existed nothing had ever demonstrated it recording anything: every run on this machine was
	 * headless with no pad attached, so "no gamepad key has arrived" was equally consistent with a
	 * working witness and with a witness that was never registered. That is a check that cannot fail,
	 * and it proves nothing.
	 *
	 * So this injects through FSlateApplication, which is ABOVE the preprocessor chain, and asserts on
	 * what came out. Two of the four arms are NEGATIVE CONTROLS that must NOT record, so a witness
	 * that indiscriminately recorded everything would fail this just as loudly as one that recorded
	 * nothing.
	 *
	 * It snapshots and restores the witness state, because leaving synthetic keys in a file the owner
	 * asks players to send him would be worse than having no test.
	 */
	static void Verify()
	{
		if (!FSlateApplication::IsInitialized())
		{
			UE_LOG(LogTraceGame, Error, TEXT("D32-DINPUT Verify: no Slate application; cannot inject. INCONCLUSIVE."));
			return;
		}

		// Snapshot.
		const TMap<FName, FWitnessedKey> SavedKeys = WitnessedKeys;
		const int64  SavedNonGamepad = NonGamepadKeyEvents;
		const uint64 SavedGeneration = Generation;

		const FModifierKeysState NoModifiers;

		// THE DEVICE ID MUST BE ONE THE MAPPER ACTUALLY KNOWS, and this is not a detail. The first
		// version of this test invented FInputDeviceId::CreateFromInternalId(7). The FKeyEvent
		// constructor that takes an FInputDeviceId resolves a Slate user index FROM it, an unmapped id
		// resolves to -1, and FSlateApplication::ProcessKeyDownEvent then dies on
		// "Assertion failed: UserIndex >= 0" (SlateApplication.cpp:4574) — which took the whole
		// headless run down with it. GetDefaultInputDevice() is mapped to the primary platform user on
		// every platform, so it resolves to a real user index.
		const FInputDeviceId SyntheticDevice = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
		int32 Failures = 0;

		auto Check = [&Failures](const TCHAR* What, bool bPassed)
		{
			UE_LOG(LogTraceGame, Display, TEXT("D32-DINPUT Verify: %-58s %s"), What, bPassed ? TEXT("PASS") : TEXT("FAIL"));
			if (!bPassed)
			{
				++Failures;
			}
		};

		// Arm 1, NEGATIVE CONTROL: before injecting anything, the key must be absent. Without this the
		// arm below would also "pass" against a witness pre-populated by a real pad.
		WitnessedKeys.Remove(EKeys::Gamepad_FaceButton_Bottom.GetFName());
		WitnessedKeys.Remove(EKeys::Gamepad_LeftX.GetFName());
		Check(TEXT("arm 1 (control): test keys absent before injection"),
			!WitnessedKeys.Contains(EKeys::Gamepad_FaceButton_Bottom.GetFName())
			&& !WitnessedKeys.Contains(EKeys::Gamepad_LeftX.GetFName()));

		// Arm 2: a gamepad BUTTON must be recorded, on the device id it was injected with.
		{
			const FKeyEvent Injected(EKeys::Gamepad_FaceButton_Bottom, NoModifiers, SyntheticDevice, false, 0, 0);
			FSlateApplication::Get().ProcessKeyDownEvent(Injected);

			const FWitnessedKey* Seen = WitnessedKeys.Find(EKeys::Gamepad_FaceButton_Bottom.GetFName());
			Check(TEXT("arm 2: gamepad button recorded, with its device id"),
				Seen != nullptr && Seen->EventCount >= 1 && Seen->bSawDigital && Seen->LastDeviceId == SyntheticDevice.GetId());
		}

		// Arm 3: an ANALOG event must be recorded WITH ITS VALUE. A witness that only handled key-down
		// would pass arm 2 and fail here, which is the point of separating them.
		{
			const FAnalogInputEvent Injected(EKeys::Gamepad_LeftX, NoModifiers, SyntheticDevice, false, 0, 0, 0.625f);
			FSlateApplication::Get().ProcessAnalogInputEvent(Injected);

			const FWitnessedKey* Seen = WitnessedKeys.Find(EKeys::Gamepad_LeftX.GetFName());
			Check(TEXT("arm 3: analog axis recorded, carrying its value"),
				Seen != nullptr && Seen->bSawAnalog && FMath::IsNearlyEqual(Seen->LastValue, 0.625f));
		}

		// Arm 4, NEGATIVE CONTROL: a KEYBOARD key must be counted and NOT named. This is the privacy
		// promise the report makes in writing, so it gets a test rather than a comment.
		{
			const int64 BeforeCount = NonGamepadKeyEvents;
			const FKeyEvent Injected(EKeys::A, NoModifiers, SyntheticDevice, false, 0, 0);
			FSlateApplication::Get().ProcessKeyDownEvent(Injected);

			Check(TEXT("arm 4 (control): keyboard key counted but never named"),
				NonGamepadKeyEvents == BeforeCount + 1 && !WitnessedKeys.Contains(EKeys::A.GetFName()));
		}

		// Restore, so the report a player sends is untouched by having run this.
		WitnessedKeys        = SavedKeys;
		NonGamepadKeyEvents  = SavedNonGamepad;
		Generation           = SavedGeneration;

		UE_LOG(LogTraceGame, Display, TEXT("D32-DINPUT Verify: %d failure(s). Witness state restored."), Failures);
	}

	static FAutoConsoleCommand CmdInputVerify(
		TEXT("Trace.Input.VerifyWitness"),
		TEXT("D32-DINPUT. Proves the input witness behind section 3 of the device report is actually "
			"recording: injects a gamepad button and a gamepad axis through FSlateApplication and "
			"asserts both were captured, plus two negative controls (the keys are absent beforehand, "
			"and a keyboard key is counted but never named). Restores everything it changes."),
		FConsoleCommandDelegate::CreateStatic(&Verify));

	static FAutoConsoleCommand CmdInputDevices(
		TEXT("Trace.Input.Devices"),
		TEXT("D32-DINPUT. Prints the input-device report and writes it to Saved/TraceInputDevices.txt: "
			"which input backend modules are loaded, every device the engine's device mapper has "
			"mapped with its backend and hardware identifier, and every gamepad key seen since launch. "
			"The file is written automatically in every build configuration, including Shipping, where "
			"this command does not exist because there is no console."),
		FConsoleCommandDelegate::CreateStatic(&CmdDevices));
}

#endif // !UE_BUILD_SHIPPING
