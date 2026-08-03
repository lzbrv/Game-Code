// Trace — synthetic input harness (debug only).
//
// WHY THIS EXISTS
//
// Every bot in this project calls ATraceCharacter::DoMove / DoFirePressed / DoDash directly. Not
// one of them goes anywhere near Enhanced Input. So a run with 1900 bot kills in it proves exactly
// nothing about whether a human can move or shoot: the whole chain
//
//     OS key -> FSlateApplication -> SViewport -> FSceneViewport
//         -> UGameViewportClient::InputKey -> APlayerController::InputKey
//         -> UEnhancedPlayerInput::InputKey -> mapping context -> trigger evaluation
//         -> UEnhancedInputComponent binding -> ATracePlayerController::OnFireStarted
//         -> ATraceCharacter::DoFirePressed -> UTraceWeaponComponent::StartFire -> FireOnce
//         -> ServerFire -> lag-compensated hitscan -> ClientNotifyHit
//
// was, before this file, exercised only by a person with a keyboard — whose bug report ("I cannot
// shoot") comes with no stack trace and no log line.
//
// This harness injects key and mouse events into that chain so an automated, headless run can
// prove or disprove the whole thing from a log.
//
// HOW FAR UP THE CHAIN IT REACHES, AND WHY
//
// The injection point is UGameViewportClient::InputKey / InputAxis — precisely the call
// FSceneViewport::OnKeyDown makes after Slate has routed a real keystroke to the game viewport
// widget. Everything from there inward is byte-for-byte the real path.
//
// Going one level higher, to FSlateApplication::ProcessKeyDownEvent, is implemented below but
// compiled out: FSlateApplication, FKeyEvent and FPointerEvent are exported by the Slate and
// SlateCore modules, and the Trace module lists neither in Trace.Build.cs. It links against Engine
// only, so those symbols are unresolved at link time (measured: 11 undefined arm64 symbols,
// including FSlateApplication::CurrentApplication and the FInputEvent vtable). Adding
// "Slate", "SlateCore" and "ApplicationCore" to PublicDependencyModuleNames is the entire fix —
// then set TRACE_HARNESS_WITH_SLATE to 1 in the .cpp and the Slate path compiles and runs.
//
// What the missing level would additionally cover: Slate keyboard-focus routing and viewport mouse
// capture. Neither is invisible here — Trace.InputDiag reports the viewport's capture mode and
// IgnoreInput() flag directly — but a focus bug specifically cannot be reproduced by this harness.
// Do not read a passing self-test as proof that a real, unfocused, backgrounded window works.
//
// COMMANDS
//
//   Trace.InputDiag
//       One-shot dump of the whole Enhanced Input setup: PlayerInput class, InputComponent class
//       and binding count, whether the subsystem accepted our mapping context, every action's
//       ValueType, plus viewport capture/ignore state.
//
//   Trace.SimInput <Key> [Seconds] [viewport|controller]
//       Press a key, hold it, release it. Key is any FKey name (W, A, S, D, SpaceBar, LeftShift,
//       Tab, LeftMouseButton) or the aliases LMB / RMB / Space / Shift. Default hold 0.5s.
//       "viewport" (default) enters at UGameViewportClient::InputKey; "controller" skips the
//       viewport and enters at APlayerController::InputKey, which is how you tell a viewport
//       problem (IgnoreInput, capture, no local player for the input device) apart from an
//       Enhanced Input problem.
//
//   Trace.SimAxis <MouseX|MouseY> <Delta> [viewport|controller]
//       One analog mouse sample, for the Look action.
//
//   Trace.InputSelfTest [SettleSeconds] [viewport|controller]
//       The actual proof. Waits for a living local pawn, then runs a scripted sequence and prints
//       a PASS/FAIL line per assertion at Display level under LogTraceGame:
//
//         - diagnostics, and the static preconditions (local controller, local player, viewport
//           not ignoring input, an input device that resolves to a local player)
//         - W held for 1.2s, with the pawn's location sampled either side
//         - a 20-sample mouse sweep, checked against the control rotation
//         - an enemy placed down a traced-clear line from our own muzzle, then LMB held for 3s,
//           checked against tracers emitted AND against server-sent hit confirmations
//         - then it KILLS the pawn and runs the whole sequence again on the replacement, because
//           "input dies after the first death" is the failure mode that matches the bug report
//
//       Safe to pass through -ExecCmds at launch: it self-schedules until the world is ready.
//
// USAGE
//
//   UnrealEditor Trace.uproject /Game/Maps/Arena -game -windowed -resx=1280 -resy=720 \
//       -ExecCmds="Trace.InputSelfTest" -abslog=/tmp/selftest.log
//
// Nothing here ships in a Shipping build (see the UE_BUILD_SHIPPING guard in the .cpp).

#pragma once

#include "CoreMinimal.h"
