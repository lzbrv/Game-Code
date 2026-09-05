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

	// =============================================================================================
	// THE NETWORK COMPATIBILITY VALUE — MADE EXPLICIT, BECAUSE THE DEFAULT IS NOT PORTABLE
	// =============================================================================================
	//
	// A client whose network version disagrees with the server's is refused during the handshake,
	// and the message the player gets ("CLIENT AND SERVER ARE RUNNING DIFFERENT BUILDS") does not
	// say WHICH of the several inputs disagreed. This project plays Mac-against-Windows, so it is
	// worth knowing exactly what that value is made of.
	//
	// WHAT THE ENGINE WOULD DO ON ITS OWN. FNetworkVersion::GetLocalNetworkVersion (UE 5.8,
	// Engine/Source/Runtime/Core/Private/Misc/NetworkVersion.cpp:221) CRCs this string:
	//
	//     "<ProjectName> <ProjectVersion>, NetCL: <n>, EngineNetworkVersion: <e>, GameNetworkVersion: <g>"
	//
	// Of those five terms, THREE COME FROM THE ENGINE INSTALL AND NOT FROM THIS REPOSITORY:
	//
	//   NetCL                 ENGINE_NET_VERSION is 0 in 5.8 (NetworkVersion.h:12), so this falls
	//                         through to BuildSettings::GetCompatibleChangelist() — the
	//                         "CompatibleChangelist" field of <engine>/Engine/Build/Build.version.
	//                         It is 55116800 on this machine's UE 5.8.1. It is a property of the
	//                         INSTALLED ENGINE, not of the game: a friend on 5.8.0, on a source
	//                         build, or on a launcher hotfix Epic shipped a different number in,
	//                         computes a different value from the same commit of this repository.
	//   EngineNetworkVersion  FEngineNetworkCustomVersion::LatestVersion — an engine header constant.
	//   GameNetworkVersion    FGameNetworkCustomVersion::LatestVersion — likewise.
	//
	// So the engine default is "these two machines will agree if their engine installs are
	// byte-identical", which is a hope, not a guarantee, and it is the specific hope that produces
	// an evening of "connection failed" with no diagnosis. THIS PROJECT DOES NOT RELY ON IT.
	//
	// WHAT WE DO INSTEAD. GetNetVersionString() below is built from exactly two things, BOTH OF
	// WHICH LIVE IN THIS REPOSITORY and are therefore identical on every machine that checked out
	// the same commit:
	//
	//     NetProtocolVersion                 the constant immediately below
	//     ProjectVersion                     Config/DefaultGame.ini, [GeneralProjectSettings]
	//
	// InstallNetVersionOverride() binds that to FNetworkVersion::GetLocalNetworkVersionOverride, the
	// engine's own supported hook (NetworkVersion.h:54-56), so every handshake uses it.
	//
	// *** WHAT THIS TRADES AWAY, SAID PLAINLY. *** The engine's default refuses a connection between
	// two DIFFERENT ENGINE VERSIONS, because the engine terms differ. Ours does not: two builds of
	// the same commit on 5.8 and on some future 5.9 would now agree on the version number and then
	// disagree about the wire format, which fails later and less clearly. That is an acceptable
	// trade for a project pinned to one engine version by Trace.uproject's EngineAssociation and
	// played by five people this weekend — and it is why NetProtocolVersion exists: BUMP IT when
	// the engine version moves or when anything about the replicated surface changes, and old
	// clients are refused again, by us, on purpose.
	//
	// FCrc::StrCrc32 IS SAFE TO USE ACROSS PLATFORMS AND THAT IS NOT AN ASSUMPTION. TCHAR is 2 bytes
	// on Windows and 4 on macOS, so a naive string hash would differ by platform. Crc.h:StrCrc32
	// widens every character to 32 bits before folding it in and says why in its own comment: "we
	// always want to treat every CRC as if it was based on 4 byte chars, even if it's less, because
	// we want consistency between equivalent strings with different character types." That is also
	// why the engine's own default network version works cross-platform at all.

	/**
	 * BUMP THIS to refuse older clients. It is the only thing in the compatibility value that a
	 * human sets.
	 *
	 * Bump it when: the engine version changes, a replicated property is added/removed/reordered in
	 * a way old clients cannot read, an RPC signature changes, or a cooked-content change would make
	 * two builds disagree about the world. Do NOT bump it for a fix that both sides can carry.
	 */
	// 1 -> 2, Demo 31: ATracePlayerController gained two replicated properties for the team-select
	// session. That is exactly the "a replicated property is added" case above, and the reason it is
	// bumped here rather than left alone is a hazard the verification pass caught: the compatibility
	// value is built from THIS constant and ProjectVersion, neither of which a feature commit
	// touches, so a Windows build made before Demo 31 and a Mac build made after it would both print
	// NET 51920028, agree, connect — and then disagree about the replicated layout of the actor the
	// client owns. Refusing at the handshake with a version message is a far better failure than a
	// session that connects and then behaves inexplicably.
	inline constexpr int32 NetProtocolVersion = 2;

	/**
	 * The exact string GetNetVersionChecksum() CRCs, e.g. "trace netproto 1, project 0.1.0".
	 *
	 * Printed as well as the checksum everywhere the checksum is printed, so a mismatch tells you
	 * WHICH term differs instead of only that something did. Lower-case already: the engine's own
	 * path lower-cases before hashing and this matches it, so the two are comparable by eye.
	 */
	TRACE_API FString GetNetVersionString();

	/** CRC32 of GetNetVersionString(). This is the number the handshake compares. */
	TRACE_API uint32 GetNetVersionChecksum();

	/** "NET 3F9A1C2E" — the eight hex digits a player can read off a title screen and compare. */
	TRACE_API FString GetNetVersionLabel();

	/**
	 * Binds GetNetVersionChecksum() to FNetworkVersion::GetLocalNetworkVersionOverride.
	 *
	 * Idempotent, and called once from the game module's StartupModule so that it is in place before
	 * any net driver exists in every configuration and every target (client, listen host, dedicated
	 * server, editor PIE). Invalidates the engine's cached checksum on the way out, because
	 * GetLocalNetworkVersion caches its answer on first use and binding after that first use would
	 * otherwise be silently ignored.
	 */
	TRACE_API void InstallNetVersionOverride();
}

/**
 * Which alphabet a field will accept — UI plan WP2.3.
 *
 * ONE FIELD CLASS, TWO ALPHABETS, and the mode is a property of the OPENING rather than of the
 * class: the join prompt and the CALL SIGN row want the same caret, the same blink, the same paste
 * and the same repeat, and differ only in which characters are legal. A second class would have
 * been a second copy of every one of those behaviours.
 *
 *   Address   — letters, digits, '.', ':', '-', '_'. What ClientTravel can be handed. The default,
 *               so the join prompt's Begin() call is unchanged.
 *   CallSign  — letters, digits, SPACE, '-', '_', '.'. A name, not a URL: ':' and ';' are refused
 *               because they are the two characters an address wants and a name never does, and
 *               because a call sign carrying a colon reads as a truncated endpoint everywhere it
 *               is printed (kill feed, scoreboard, log lines).
 */
enum class ETraceTextCharset : uint8
{
	Address = 0,
	CallSign
};

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
 * the .cpp). That table is deliberately restricted to what a field may contain — see
 * ETraceTextCharset — which also means a stray key press can never corrupt the field.
 *
 * The host must not route its own bindings while IsActive(); the title screen checks it exactly the
 * way it already checks FTraceOptionsMenu::IsOpen(), and so does the CALL SIGN row on the settings
 * page (FTraceOptionsMenu::Tick).
 */
class TRACE_API FTraceTextEntry
{
public:
	/** The address field's cap, and the default for any caller that does not state one. */
	static constexpr int32 DefaultMaxLength = 64;

	/**
	 * Starts editing, with the caret at the end of @p InitialText.
	 *
	 * @param InCharset    which alphabet is legal. Defaults to Address — zero-diff for the join prompt.
	 * @param InMaxLength  hard cap on the text, in characters. Defaults to the 64 an address gets;
	 *                     WP2.3 passes 16 for a call sign. Clamped to at least 1, because a field
	 *                     that can hold nothing is a field that silently eats every key press.
	 */
	void Begin(const FString& InitialText, ETraceTextCharset InCharset = ETraceTextCharset::Address,
		int32 InMaxLength = DefaultMaxLength);

	/** Which alphabet this opening is accepting. Meaningless while !IsActive(). */
	ETraceTextCharset GetCharset() const { return Charset; }

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
	/**
	 * Appends @p Char at the caret if the field has room and the character is legal for the charset
	 * this opening was Begun with.
	 *
	 * @return true when the character actually landed. The caller uses that to decide whether the
	 *         keystroke counts as an edit — see the caret blink in Poll.
	 */
	bool InsertChar(TCHAR Char);

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

	/** WP2.3 — which alphabet the current opening accepts. Set by Begin, never guessed at. */
	ETraceTextCharset Charset = ETraceTextCharset::Address;

	/**
	 * WP2.3 — the cap, per OPENING rather than per class.
	 *
	 * It was a `static constexpr int32 MaxLength = 64` until the call sign needed 16. An instance
	 * field is what lets one field class serve both without either caller having to trim the string
	 * afterwards — and trimming afterwards is exactly the bug it avoids: a 40-character paste into a
	 * 16-character name would otherwise be accepted here, drawn here, and silently truncated by the
	 * settings accessor, so the row and the game would disagree about the player's own name.
	 */
	int32 MaxLength = DefaultMaxLength;

	static constexpr float RepeatDelay = 0.40f;
	static constexpr float RepeatInterval = 0.045f;
};
