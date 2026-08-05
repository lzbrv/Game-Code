// Trace — the in-game multiplayer entry points: addresses, joining, and why a join failed.
//
// WHY THIS FILE EXISTS
// The reported bug was not a netcode bug. Two machines each pressed PLAY, each got a *standalone*
// match, and nothing was ever listening — `ATraceMenuHUD::StartMatch()` travelled to the arena with
// only `?difficulty=` and `?mode=` in the URL and no `?listen`. There was also no Host or Join UI
// anywhere in the project, and no address shown to anyone, and no message when a connection failed.
// So the collaborator could not tell a broken VPN from a broken game, because the game said nothing
// either way.
//
// Everything needed to fix that is small, shared by three screens (title, join prompt, in-match HUD)
// and has nothing to do with drawing, so it lives here rather than being spread across two HUDs:
//
//   * which local address to TELL PEOPLE     — TraceNet::GetPreferredLocalAddress()
//   * whether we can actually host on 7777   — TraceNet::IsListenPortAvailable()
//   * where we are connected right now       — TraceNet::DescribeConnection()
//   * the last address the player joined     — TraceNet::LoadLastJoinAddress() / Save...
//   * why the last attempt failed            — TraceNet::BindFailureHandlers() + GetLastFailure()
//   * typing an address on a Canvas          — FTraceTextEntry
//
// Plain C++, no UCLASS: none of it is reflected, replicated or Blueprint-facing, and both HUD
// headers already include non-UObject helpers (FTraceOptionsMenu) for exactly this reason.
//
// MODULE DEPENDENCIES: this uses ISocketSubsystem (module "Sockets"), which is a PUBLIC dependency
// of "Engine" and therefore already on this module's include and link path — Trace.Build.cs needs no
// change. ApplicationCore is deliberately NOT used; see FTraceTextEntry::Poll for how paste works
// without it.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Net/Core/Connection/NetEnums.h"   // ENetworkFailure
#include "Engine/EngineBaseTypes.h"         // ETravelFailure

class APlayerController;
class UWorld;

namespace TraceNet
{
	/**
	 * Unreal's default game port, and the only one anything in this project ever uses.
	 *
	 * It is UDP. The number appears in Scripts/run-listen-server.sh, in docs/NETWORKING.md and in the
	 * address every screen prints, so it is written down once, here.
	 */
	inline constexpr int32 DefaultPort = 7777;

	/**
	 * The address to hand somebody who wants to join this machine, without the port.
	 *
	 * Enumerates every local adapter and RANKS them, because "the first address the OS returns" is
	 * routinely loopback or a virtual bridge, and telling the host to share 127.0.0.1 is worse than
	 * telling them nothing. Preference order, highest first:
	 *
	 *   1. 100.64.0.0/10   — the CGNAT block Tailscale hands out. This team plays over Tailscale
	 *                        (docs/NETWORKING.md §4), so if a tailnet address exists it is
	 *                        essentially always the one that will work from another house.
	 *   2. RFC1918         — 192.168/16, 10/8, 172.16/12. The LAN case.
	 *   3. Anything else routable.
	 *   4. 169.254/16      — link-local. Better than nothing, rarely useful.
	 *   5. 127.0.0.0/8     — loopback. Only ever returned when there is literally nothing else, and
	 *                        it IS correct for two processes on one machine, which is how this
	 *                        feature was verified.
	 *
	 * IPv4 only, on purpose: the shipped net driver binds IPv4 by default here, and printing a v6
	 * address somebody then cannot connect to would recreate the exact confusion this fixes.
	 */
	TRACE_API FString GetPreferredLocalAddress();

	/** Every local IPv4 address, best first, for the diagnostic dump. Never empty (loopback at worst). */
	TRACE_API void GetLocalAddresses(TArray<FString>& OutAddresses);

	/** "100.101.102.103:7777" — GetPreferredLocalAddress() with the port the host will listen on. */
	TRACE_API FString GetHostEndpoint();

	/**
	 * Can we bind UDP @p Port right now?
	 *
	 * Checked BEFORE travelling, because a listen server that fails to bind does not fail loudly:
	 * UEngine::LoadMap logs one `LogNet: Error: LoadMap: failed to Listen(...)` line and then loads
	 * the map anyway, standalone. The match plays perfectly and is silently unjoinable — which is
	 * indistinguishable, from the player's chair, from the bug this whole pass is fixing.
	 */
	TRACE_API bool IsListenPortAvailable(int32 Port = DefaultPort);

	/**
	 * IsListenPortAvailable(DefaultPort), answered from a ~2 second cache.
	 *
	 * For the per-frame callers. Creating, binding and closing a UDP socket sixty times a second to
	 * answer a question whose answer changes about once an hour would be an odd way to spend a frame.
	 */
	TRACE_API bool IsDefaultPortFreeCached();

	/**
	 * Tidies whatever the player typed into something ClientTravel accepts.
	 *
	 * Strips whitespace and a pasted "open " / "unreal://" prefix, and appends ":7777" when no port
	 * was given. Returns empty for input with no host part at all.
	 */
	TRACE_API FString NormalizeJoinAddress(const FString& Raw);

	// ---- Remembering the address ------------------------------------------------------------------
	//
	// Stored in Saved/Config/<Platform>/GameUserSettings.ini under [/Script/Trace.Network]. That file
	// is per-machine, writable at runtime and outside source control — the same three properties that
	// make it right for a mouse sensitivity make it right for "the address I keep typing". It is
	// deliberately NOT Config/DefaultGame.ini: a tailnet address is nobody else's business and must
	// never appear in a diff.

	TRACE_API FString LoadLastJoinAddress();
	TRACE_API void SaveLastJoinAddress(const FString& Address);

	// ---- Connection state -------------------------------------------------------------------------

	/** What this process is: "HOSTING", "CLIENT", "OFFLINE". */
	enum class ERole : uint8
	{
		Offline = 0,
		Hosting,
		Client
	};

	/**
	 * Resolves the connection state from the world's actual net mode and net driver, never from what
	 * we intended.
	 *
	 * That distinction is the entire point. If the listen bind failed we are standalone, and the HUD
	 * must say OFFLINE rather than repeating a promise the process did not keep.
	 *
	 * @param OutEndpoint  the address to show: our own when hosting, the server's when a client.
	 * @param OutDetail    a short second line ("2 CONNECTED", "PORT 7777 WAS BUSY"), possibly empty.
	 */
	TRACE_API ERole DescribeConnection(const UWorld* World, FString& OutEndpoint, FString& OutDetail);

	// ---- Failure reporting ------------------------------------------------------------------------

	/**
	 * Subscribes to UEngine::OnNetworkFailure and OnTravelFailure exactly once per process.
	 *
	 * Bound to GEngine, which outlives every map, and never unbound: the handler captures nothing and
	 * only writes to the static store below, so there is no lifetime to manage and nothing to leak.
	 * That matters because a failed join DESTROYS the world that started it — the engine browses back
	 * to the default map — so a handler owned by the menu HUD would be torn down by the very event it
	 * exists to report. Called from both HUDs' BeginPlay; the second call does nothing.
	 */
	TRACE_API void BindFailureHandlers();

	/**
	 * The last connection failure, if there was one recently.
	 *
	 * Survives the travel back to the title screen, which is the whole reason it is a file-static and
	 * not a member: by the time anything can draw the message, the world that failed is gone.
	 *
	 * @param OutAgeSeconds  wall-clock seconds since it happened, so callers can fade it out.
	 * @return false when nothing has failed this session.
	 */
	TRACE_API bool GetLastFailure(FString& OutHeadline, FString& OutDetail, double& OutAgeSeconds);

	/** Records a failure by hand — used for the ones the engine does not raise (e.g. a busy port). */
	TRACE_API void ReportFailure(const FString& Headline, const FString& Detail);

	/** Forgets the last failure. Called when the player starts a fresh attempt. */
	TRACE_API void ClearFailure();

	/** Plain-English form of an engine failure code. "Connection timed out", not "ConnectionTimeout". */
	TRACE_API FString DescribeNetworkFailure(ENetworkFailure::Type FailureType);
	TRACE_API FString DescribeTravelFailure(ETravelFailure::Type FailureType);

	/** One Display line naming the net mode, the endpoint and every local address. */
	TRACE_API void LogNetworkDiagnostics(const UWorld* World, const TCHAR* Context);
}

/**
 * A one-line text field driven entirely by polled key state.
 *
 * WHY POLLING. The menu is Canvas-drawn and this project has no Slate widgets to focus, so there is
 * no character stream to read: UGameViewportClient::InputChar routes characters to the console and
 * nothing else — APlayerController has no InputChar at all. FTraceOptionsMenu already polls
 * WasInputKeyJustPressed for its rebind capture for the same reason, so this is the established
 * pattern in this codebase rather than a second one.
 *
 * The cost of polling is that the key -> character mapping has to be written down (see the table in
 * the .cpp). That table is deliberately restricted to what an address can contain — letters, digits,
 * dot, colon, hyphen, underscore — which also means a stray key press can never corrupt the field.
 *
 * The host must not route its own bindings while IsActive(); the title screen checks it exactly the
 * way it already checks FTraceOptionsMenu::IsOpen().
 */
class TRACE_API FTraceTextEntry
{
public:
	/** Starts editing, with the caret at the end of @p InitialText. */
	void Begin(const FString& InitialText);

	/** Stops editing and clears the pending submit/cancel edges. */
	void End();

	bool IsActive() const { return bActive; }

	/**
	 * Reads the keyboard for this frame. Call once per frame from the owning DrawHUD while active.
	 *
	 * Input is ignored for the remainder of the frame Begin() was called on: the Enter that opened
	 * the field is still "just pressed" and would otherwise submit it immediately — the same trap
	 * FTraceOptionsMenu::IgnoreInputBeforeFrame documents.
	 */
	void Poll(APlayerController* PC, float Now);

	/** True once, on the frame Enter was pressed. */
	bool ConsumeSubmit();

	/** True once, on the frame Escape was pressed. */
	bool ConsumeCancel();

	const FString& GetText() const { return Text; }
	void SetText(const FString& InText);

	/** Character index the caret sits before. Always within [0, Text.Len()]. */
	int32 GetCaret() const { return Caret; }

	/** True while the caret should be drawn — a 1.7 Hz square wave, restarted by every keystroke. */
	bool IsCaretVisible(float Now) const;

	/** Set for two seconds after a paste, so the panel can confirm it did something. */
	bool WasRecentlyPasted(float Now) const { return (Now - LastPasteTime) < 2.f; }

private:
	/** Appends @p Char at the caret if the field has room and the character is legal. */
	void InsertChar(TCHAR Char);

	void Backspace();
	void DeleteForward();

	/** Reads the OS clipboard without an ApplicationCore dependency. Empty when unavailable. */
	static FString ReadClipboard();

	bool bActive = false;
	FString Text;
	int32 Caret = 0;

	bool bSubmitted = false;
	bool bCancelled = false;

	uint64 IgnoreInputBeforeFrame = 0;

	/** Time of the last accepted keystroke; the caret blink phase is measured from it. */
	float LastEditTime = 0.f;
	float LastPasteTime = -1000.f;

	/** Held-key repeat for backspace only. Nothing else in an address is worth holding down. */
	float NextRepeatTime = 0.f;
	bool bRepeatArmed = false;

	static constexpr int32 MaxLength = 64;
	static constexpr float RepeatDelay = 0.40f;
	static constexpr float RepeatInterval = 0.045f;
};
