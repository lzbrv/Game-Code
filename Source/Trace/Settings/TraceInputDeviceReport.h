// Trace — D32-DINPUT: the input-device report.
//
// THE OWNER'S ASK, VERBATIM: "Add direct input support not just xinput".
//
// -------------------------------------------------------------------------------------------------
// WHY THE DIAGNOSTIC IS THE DELIVERABLE AND THE MAPPING IS NOT
// -------------------------------------------------------------------------------------------------
// "DirectInput" is a Windows-only concept. Unreal's built-in Windows pad path is XInput (the
// XInputDevice plugin, EnabledByDefault), which covers Xbox-style pads and almost nothing else. The
// engine's answer for everything else on Windows is the RawInput plugin, which this project now
// enables — Win64-only, Optional, see Trace.uproject and the block in Config/DefaultInput.ini.
//
// Enabling it makes a generic HID device's BUTTONS arrive as bindable keys called
// GenericUSBController_Button1..N. It does NOT choose which of a player's 20 unlabelled buttons is
// "jump". That mapping is per-device and cannot be guessed from macOS, where no DirectInput device
// can even be attached. So the useful thing to build was the instrument: a report that names the
// devices the engine can actually see and lists every gamepad key that has reached the process. With
// it, a player whose pad "does nothing" sends back a file, and the mapping takes minutes.
//
// -------------------------------------------------------------------------------------------------
// WHAT "REACHING THE GAME" MEANS HERE, PRECISELY
// -------------------------------------------------------------------------------------------------
// The witness is a Slate IInputProcessor. It sees events at the application layer, BEFORE the game
// viewport, and it always returns false, so it consumes nothing and changes no behaviour. That
// vantage point is deliberate, because it splits the two failure modes that both present as "my
// controller doesn't work":
//
//   * Device listed in section 1, no keys in section 3  -> the OS/engine sees the hardware but no
//     input is arriving. A driver, a permission, or a device the input backend never opened.
//   * Keys in section 3, but the game does nothing      -> input IS arriving and the problem is the
//     binding. Section 3 prints the key names, which is exactly what a bind needs.
//
// It does NOT prove Enhanced Input consumed the key. Nothing in this file claims it does.
//
// -------------------------------------------------------------------------------------------------
// IT HAS TO WORK IN SHIPPING, WHICH IS WHY IT WRITES A FILE
// -------------------------------------------------------------------------------------------------
// Scripts/package.sh and package.bat both default to -clientconfig=Shipping, so that is what the
// owner's friends run. In Shipping this engine defines ALLOW_CONSOLE to ALLOW_CONSOLE_IN_SHIPPING,
// which is 0, and NO_LOGGING to !USE_LOGGING_IN_SHIPPING, which is 1 (Engine/Source/Runtime/Core/
// Public/Misc/Build.h). A player therefore has NO console to type a command into and NO log file to
// read. A console-command-only diagnostic would have been useless to precisely the person who needs
// it.
//
// So the report is written to a text file with FFileHelper, which does not go through the log
// system and is compiled into every configuration:
//
//   Windows   %LOCALAPPDATA%\Trace\Saved\TraceInputDevices.txt
//   macOS     ~/Library/Application Support/Epic/Trace/Saved/TraceInputDevices.txt
//   (in-tree editor / -game runs: <repo>/Saved/TraceInputDevices.txt)
//
// The console command Trace.Input.Devices prints the same text and exists only in non-Shipping,
// because that is the only place a console exists to type it into.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "TraceInputDeviceReport.generated.h"

/**
 * Writes TraceInputDevices.txt and keeps it current.
 *
 * A GAME INSTANCE SUBSYSTEM for the same reason UTraceGamepadInputSubsystem is one: the thing being
 * observed is the local player's hardware, which outlives any single world, and a per-world owner
 * would lose the witnessed-key history on every map travel — i.e. exactly when a player is most
 * likely to be told "press every button and send me the file".
 */
UCLASS()
class TRACE_API UTraceInputDeviceReportSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The whole report as text. Safe to call at any time, including before any device exists. */
	static FString BuildReport();

	/** Absolute path of the file BuildReport is written to. */
	static FString GetReportPath();

	/**
	 * Writes the report now.
	 *
	 * @param OutPath  Receives the absolute path written to, whether or not the write succeeded.
	 * @return         True if the file was written.
	 */
	static bool WriteReport(FString& OutPath);

private:
	/** Rewrites the file when, and only when, something in it has changed. See the 2 s note in the .cpp. */
	FTSTicker::FDelegateHandle TickHandle;
};
