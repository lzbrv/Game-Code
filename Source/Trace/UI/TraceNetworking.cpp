// Trace — multiplayer entry-point support. See TraceNetworking.h.

#include "UI/TraceNetworking.h"

#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"     // FAutoConsoleCommand — Trace.NetVersion
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreMisc.h"
#include "Misc/Crc.h"                    // FCrc::StrCrc32 — the compatibility checksum
#include "Misc/NetworkVersion.h"         // FNetworkVersion::GetLocalNetworkVersionOverride
#include "Misc/Parse.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Trace.h"                       // LogTraceGame

// CLIPBOARD PASTE — the one optional thing in this file.
//
// Ctrl/Cmd+V in the JOIN field needs FPlatformApplicationMisc::ClipboardPaste, which lives in the
// ApplicationCore module. Its HEADER resolves transitively through Engine, but the SYMBOL does not:
// the link fails unless "ApplicationCore" is named in Trace.Build.cs. It is, and this is the only
// reason it is.
//
// IF THAT DEPENDENCY IS EVER UNWANTED: set this to 0 and remove the module from Trace.Build.cs.
// Nothing else changes — ReadClipboard() returns an empty string, Ctrl+V becomes a no-op, and every
// other part of the JOIN flow (typing, backspace, the remembered address) is unaffected. That is
// deliberately a one-line decision rather than a refactor.
#define TRACE_HAS_CLIPBOARD 1

#if TRACE_HAS_CLIPBOARD
#	include "HAL/PlatformApplicationMisc.h"
#endif

namespace TraceNet
{

// =================================================================================================
// Local addresses
// =================================================================================================

namespace
{
	/** Config home for the remembered join address. Per-machine, runtime-writable, never in a diff. */
	const TCHAR* NetConfigSection = TEXT("/Script/Trace.Network");
	const TCHAR* LastJoinAddressKey = TEXT("LastJoinAddress");

	/**
	 * How much we want to hand this address to somebody in another house. Higher is better.
	 *
	 * See the long note on GetPreferredLocalAddress() for why the order is what it is. The values are
	 * spaced so a new class can be slotted between two existing ones without renumbering.
	 */
	int32 ScoreAddress(const FString& Address)
	{
		TArray<FString> Parts;
		Address.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/false);
		if (Parts.Num() != 4)
		{
			// Not a dotted quad: an IPv6 address, or something with a scope id. Deliberately last —
			// the shipped net driver binds IPv4 here, so printing a v6 address would send somebody
			// chasing a connection that was never going to work.
			return 1;
		}

		const int32 A = FCString::Atoi(*Parts[0]);
		const int32 B = FCString::Atoi(*Parts[1]);

		if (A == 127)                                  { return 10;  }   // loopback
		if (A == 169 && B == 254)                      { return 20;  }   // link-local, usually a dud
		if (A == 100 && B >= 64 && B <= 127)           { return 100; }   // Tailscale / CGNAT 100.64/10
		if (A == 10)                                   { return 80;  }   // RFC1918
		if (A == 192 && B == 168)                      { return 80;  }   // RFC1918
		if (A == 172 && B >= 16 && B <= 31)            { return 80;  }   // RFC1918
		if (A == 0 || A >= 224)                        { return 5;   }   // unspecified / multicast

		return 50;                                                        // routable, but unusual here
	}
}

namespace
{
	/**
	 * Adapter enumeration and the port probe are both cached for a couple of seconds.
	 *
	 * Not premature optimisation — a correctness-shaped one. Three separate title-screen elements ask
	 * for the host endpoint (the chip, the PLAY row, the selected-row blurb) and the HUD asks once a
	 * frame, so an uncached implementation would run GetLocalAdapterAddresses four times per frame
	 * and, in the offline branch, create/bind/close a UDP socket per frame as well. Adapters do not
	 * change on a human timescale; two seconds still picks up a VPN coming up while somebody sits on
	 * the title screen.
	 */
	constexpr double CacheSeconds = 2.0;

	TArray<FString> CachedAddresses;
	double CachedAddressesTime = -1000.0;

	bool bCachedPortFree = true;
	double CachedPortTime = -1000.0;

	/** The uncached enumeration. Only ever called through GetLocalAddresses(). */
	void EnumerateLocalAddresses(TArray<FString>& OutAddresses);
}

void GetLocalAddresses(TArray<FString>& OutAddresses)
{
	const double NowSeconds = FPlatformTime::Seconds();
	if ((NowSeconds - CachedAddressesTime) > CacheSeconds)
	{
		EnumerateLocalAddresses(CachedAddresses);
		CachedAddressesTime = NowSeconds;
	}

	OutAddresses = CachedAddresses;
}

namespace
{
void EnumerateLocalAddresses(TArray<FString>& OutAddresses)
{
	OutAddresses.Reset();

	if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{
		TArray<TSharedPtr<FInternetAddr>> Adapters;
		if (Sockets->GetLocalAdapterAddresses(Adapters))
		{
			for (const TSharedPtr<FInternetAddr>& Adapter : Adapters)
			{
				if (!Adapter.IsValid())
				{
					continue;
				}

				// ToString(false) = no port. The port is ours to choose and is appended by callers.
				const FString AsText = Adapter->ToString(/*bAppendPort=*/false);
				if (!AsText.IsEmpty())
				{
					OutAddresses.AddUnique(AsText);
				}
			}
		}
	}

	// Never hand a caller an empty list. Loopback is a real, working answer for the two-processes-
	// on-one-machine case, which is exactly how this feature is verified.
	if (OutAddresses.Num() == 0)
	{
		OutAddresses.Add(TEXT("127.0.0.1"));
	}

	// Stable-sorted so the order does not shuffle between frames and make the menu flicker.
	OutAddresses.StableSort([](const FString& Lhs, const FString& Rhs)
	{
		return ScoreAddress(Lhs) > ScoreAddress(Rhs);
	});
}
} // anonymous namespace

FString GetPreferredLocalAddress()
{
	TArray<FString> Addresses;
	GetLocalAddresses(Addresses);
	return Addresses[0];
}

FString GetHostEndpoint()
{
	return FString::Printf(TEXT("%s:%d"), *GetPreferredLocalAddress(), DefaultPort);
}

bool IsListenPortAvailable(int32 Port)
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr)
	{
		// No socket subsystem at all. Nothing useful to report; let the engine's own Listen() speak.
		return true;
	}

	const TSharedRef<FInternetAddr> BindAddr = Sockets->CreateInternetAddr();
	bool bIsValid = false;
	BindAddr->SetIp(TEXT("0.0.0.0"), bIsValid);
	BindAddr->SetPort(Port);

	// FUniqueSocket, so the probe socket is closed on every return path including the early ones.
	FUniqueSocket Probe = Sockets->CreateUniqueSocket(NAME_DGram, TEXT("TracePortProbe"), BindAddr->GetProtocolType());
	if (!Probe.IsValid())
	{
		return true;
	}

	return Probe->Bind(*BindAddr);
}

bool IsDefaultPortFreeCached()
{
	const double NowSeconds = FPlatformTime::Seconds();
	if ((NowSeconds - CachedPortTime) > CacheSeconds)
	{
		bCachedPortFree = IsListenPortAvailable(DefaultPort);
		CachedPortTime = NowSeconds;
	}
	return bCachedPortFree;
}

FString NormalizeJoinAddress(const FString& Raw)
{
	FString Address = Raw.TrimStartAndEnd();

	// Things people paste. "open 100.1.2.3" comes straight out of docs/NETWORKING.md §6, and
	// "unreal://" is what the engine itself prints in some log lines.
	if (Address.StartsWith(TEXT("open "), ESearchCase::IgnoreCase))
	{
		Address = Address.RightChop(5).TrimStartAndEnd();
	}
	if (Address.StartsWith(TEXT("unreal://"), ESearchCase::IgnoreCase))
	{
		Address = Address.RightChop(9).TrimStartAndEnd();
	}

	// Anything after a '?' is a URL option, and a join URL has no business carrying one.
	int32 OptionIndex = INDEX_NONE;
	if (Address.FindChar(TEXT('?'), OptionIndex))
	{
		Address = Address.Left(OptionIndex).TrimStartAndEnd();
	}

	if (Address.IsEmpty())
	{
		return FString();
	}

	// Append the default port when none was typed. An address with no port travels fine — the engine
	// substitutes its own default — but printing the port back to the player is how they learn what
	// the other machine has to allow through its firewall.
	int32 ColonIndex = INDEX_NONE;
	if (!Address.FindLastChar(TEXT(':'), ColonIndex))
	{
		Address = FString::Printf(TEXT("%s:%d"), *Address, DefaultPort);
	}
	else if (ColonIndex == Address.Len() - 1)
	{
		// Trailing colon, no digits after it: "100.1.2.3:".
		Address = FString::Printf(TEXT("%s%d"), *Address, DefaultPort);
	}

	return Address;
}

// =================================================================================================
// Remembering the address
// =================================================================================================

FString LoadLastJoinAddress()
{
	FString Address;
	if (GConfig != nullptr)
	{
		GConfig->GetString(NetConfigSection, LastJoinAddressKey, Address, GGameUserSettingsIni);
	}
	return Address;
}

void SaveLastJoinAddress(const FString& Address)
{
	if (GConfig == nullptr || Address.IsEmpty())
	{
		return;
	}

	GConfig->SetString(NetConfigSection, LastJoinAddressKey, *Address, GGameUserSettingsIni);

	// Flushed rather than left in GConfig's in-memory copy, for the same reason
	// UTraceUserSettings::Save() flushes: the usual way this build is closed is pkill, and a value
	// that only survives a clean exit would not survive a single playtest.
	GConfig->Flush(false, GGameUserSettingsIni);

	UE_LOG(LogTraceGame, Log, TEXT("[Net] Remembered join address '%s'."), *Address);
}

// =================================================================================================
// Connection state
// =================================================================================================

ERole DescribeConnection(const UWorld* World, FString& OutEndpoint, FString& OutDetail)
{
	OutEndpoint.Reset();
	OutDetail.Reset();

	if (World == nullptr)
	{
		return ERole::Offline;
	}

	const ENetMode NetMode = World->GetNetMode();
	UNetDriver* NetDriver = World->GetNetDriver();

	if (NetMode == NM_Client)
	{
		if (NetDriver != nullptr && NetDriver->ServerConnection != nullptr)
		{
			OutEndpoint = NetDriver->ServerConnection->LowLevelGetRemoteAddress(/*bAppendPort=*/true);
		}
		if (OutEndpoint.IsEmpty())
		{
			OutEndpoint = TEXT("SERVER");
		}
		return ERole::Client;
	}

	if (NetMode == NM_ListenServer || NetMode == NM_DedicatedServer)
	{
		// The port the driver ACTUALLY bound, not the one we asked for. They differ if 7777 was busy
		// and something upstream fell back, and the number a host reads off their screen has to be
		// the number that works.
		int32 Port = DefaultPort;
		if (NetDriver != nullptr && NetDriver->LocalAddr.IsValid() && NetDriver->LocalAddr->GetPort() != 0)
		{
			Port = NetDriver->LocalAddr->GetPort();
		}

		OutEndpoint = FString::Printf(TEXT("%s:%d"), *GetPreferredLocalAddress(), Port);

		const int32 Connected = (NetDriver != nullptr) ? NetDriver->ClientConnections.Num() : 0;
		OutDetail = (Connected == 1)
			? FString(TEXT("1 PLAYER CONNECTED"))
			: FString::Printf(TEXT("%d PLAYERS CONNECTED"), Connected);

		return ERole::Hosting;
	}

	// Standalone. Either a deliberate offline match, or — and this is the case worth naming — a
	// listen server that never came up at all, which the engine reports with one LogNet error and
	// then carries on as if nothing happened (UEngine::LoadMap does not abort on a failed Listen).
	//
	// Note that a merely BUSY port does not land here: UIpNetDriver walks up to the next free port
	// and hosts successfully, which the branch above reports with the real port. This is the harder
	// failure — no driver at all.
	//
	OutEndpoint = TEXT("OFFLINE");
	OutDetail = IsDefaultPortFreeCached()
		? FString()
		: FString::Printf(TEXT("UDP %d IS IN USE BY ANOTHER PROCESS"), DefaultPort);

	return ERole::Offline;
}

// =================================================================================================
// Failure reporting
//
// A silent failed join is the specific reason the collaborator could not tell a broken VPN from a
// broken game. Everything here exists to make sure a failure ends up on screen.
// =================================================================================================

namespace
{
	FString LastFailureHeadline;
	FString LastFailureDetail;
	double LastFailureTime = -1.0;
	bool bFailureHandlersBound = false;
}

void ReportFailure(const FString& Headline, const FString& Detail)
{
	LastFailureHeadline = Headline;

	// Truncated for the SCREEN only — the full string is in the log line below. Both banners centre
	// a single unwrapped line, and an engine error string can be a couple of hundred characters of
	// connection identifiers, which would run off both edges of the viewport.
	constexpr int32 MaxDetailChars = 140;
	LastFailureDetail = (Detail.Len() > MaxDetailChars)
		? (Detail.Left(MaxDetailChars - 3) + TEXT("..."))
		: Detail;

	LastFailureTime = FPlatformTime::Seconds();

	// Error, not Warning: this is always something the player asked for and did not get.
	UE_LOG(LogTraceGame, Error, TEXT("[Net] %s — %s"), *Headline, *Detail);
}

void ClearFailure()
{
	LastFailureHeadline.Reset();
	LastFailureDetail.Reset();
	LastFailureTime = -1.0;
}

bool GetLastFailure(FString& OutHeadline, FString& OutDetail, double& OutAgeSeconds)
{
	if (LastFailureTime < 0.0)
	{
		return false;
	}

	OutHeadline = LastFailureHeadline;
	OutDetail = LastFailureDetail;
	OutAgeSeconds = FPlatformTime::Seconds() - LastFailureTime;
	return true;
}

FString DescribeNetworkFailure(ENetworkFailure::Type FailureType)
{
	switch (FailureType)
	{
	case ENetworkFailure::NetDriverAlreadyExists:
		return TEXT("A NET DRIVER IS ALREADY RUNNING. RESTART THE GAME.");
	case ENetworkFailure::NetDriverCreateFailure:
		return TEXT("COULD NOT CREATE A NET DRIVER.");
	case ENetworkFailure::NetDriverListenFailure:
		return FString::Printf(TEXT("COULD NOT LISTEN ON UDP %d. ANOTHER COPY MAY ALREADY BE HOSTING."), DefaultPort);
	case ENetworkFailure::ConnectionLost:
		return TEXT("CONNECTION LOST.");
	case ENetworkFailure::ConnectionTimeout:
		return TEXT("CONNECTION TIMED OUT. CHECK THE ADDRESS, THE VPN, AND UDP 7777 ON THE HOST.");
	case ENetworkFailure::FailureReceived:
		return TEXT("THE SERVER REFUSED THE CONNECTION.");
	// THE FOUR VERSION-MISMATCH CODES NAME THE CHECK, because "different builds" is true and useless:
	// it does not say what to compare or where to look. The NET code is on the title screen of every
	// build, bottom-right, so the instruction is one a player can actually carry out.
	case ENetworkFailure::OutdatedClient:
		return FString::Printf(TEXT("BUILD MISMATCH. YOURS IS %s — COMPARE IT WITH THE HOST'S (TITLE SCREEN, BOTTOM RIGHT)."),
			*GetNetVersionLabel());
	case ENetworkFailure::OutdatedServer:
		return FString::Printf(TEXT("BUILD MISMATCH. YOURS IS %s — THE HOST'S TITLE SCREEN MUST SHOW THE SAME CODE."),
			*GetNetVersionLabel());
	case ENetworkFailure::PendingConnectionFailure:
		return TEXT("COULD NOT REACH THAT ADDRESS.");
	case ENetworkFailure::NetGuidMismatch:
	case ENetworkFailure::NetChecksumMismatch:
		return FString::Printf(TEXT("CLIENT AND SERVER ARE RUNNING DIFFERENT BUILDS. YOURS IS %s."),
			*GetNetVersionLabel());
	default:
		return TEXT("THE CONNECTION FAILED.");
	}
}

FString DescribeTravelFailure(ETravelFailure::Type FailureType)
{
	switch (FailureType)
	{
	case ETravelFailure::NoLevel:
	case ETravelFailure::LoadMapFailure:
		return TEXT("THE SERVER'S MAP COULD NOT BE LOADED.");
	case ETravelFailure::InvalidURL:
		return TEXT("THAT ADDRESS IS NOT VALID. USE  <ip>:7777.");
	case ETravelFailure::PackageMissing:
	case ETravelFailure::NoDownload:
		return TEXT("THE SERVER IS RUNNING CONTENT THIS BUILD DOES NOT HAVE.");
	case ETravelFailure::PackageVersion:
		return TEXT("CLIENT AND SERVER ARE RUNNING DIFFERENT BUILDS.");
	case ETravelFailure::PendingNetGameCreateFailure:
		return TEXT("COULD NOT START THE CONNECTION.");
	case ETravelFailure::ServerTravelFailure:
		return TEXT("THE SERVER FAILED TO CHANGE MAP.");
	case ETravelFailure::ClientTravelFailure:
		return TEXT("THIS CLIENT FAILED TO FOLLOW THE SERVER.");
	case ETravelFailure::CheatCommands:
		return TEXT("TRAVEL IS DISABLED BECAUSE CHEAT COMMANDS WERE USED.");
	default:
		return TEXT("TRAVEL FAILED.");
	}
}

void BindFailureHandlers()
{
	if (bFailureHandlersBound || GEngine == nullptr)
	{
		return;
	}
	bFailureHandlersBound = true;

	// Deliberately raw lambdas capturing nothing. See the header: the world that raised the failure
	// is usually being torn down as the delegate fires, so anything with an owner would be the wrong
	// shape here.
	GEngine->OnNetworkFailure().AddLambda(
		[](UWorld* /*FailedWorld*/, UNetDriver* FailedDriver, ENetworkFailure::Type FailureType, const FString& ErrorString)
		{
			// A dropped connection means two completely different things depending on which end you
			// are. On the CLIENT it means "you have been disconnected" and is alarming. On the SERVER
			// the engine raises the same code when a remote player times out — telling the host
			// "CONNECTION LOST" for somebody else's wifi would be actively misleading, and after a
			// pass spent making failures visible, a false alarm is the fastest way to teach people to
			// ignore the banner.
			//
			// ServerConnection is non-null only on a client, so its absence identifies the server.
			const bool bWeAreTheServer = (FailedDriver != nullptr) && (FailedDriver->ServerConnection == nullptr);
			const bool bPeerDropped =
				(FailureType == ENetworkFailure::ConnectionLost) || (FailureType == ENetworkFailure::ConnectionTimeout);

			const FString Headline = (bWeAreTheServer && bPeerDropped)
				? FString(TEXT("A PLAYER LEFT THE MATCH"))
				: DescribeNetworkFailure(FailureType);

			// The engine's own ErrorString is often empty and, when it is not, is aimed at a
			// programmer ("UNetConnection::Tick: Connection TIMED OUT"). Keep both: the readable
			// sentence for the player, the raw code for the log and the bug report.
			ReportFailure(Headline,
				FString::Printf(TEXT("%s%s%s"),
					ENetworkFailure::ToString(FailureType),
					ErrorString.IsEmpty() ? TEXT("") : TEXT(": "),
					*ErrorString));
		});

	GEngine->OnTravelFailure().AddLambda(
		[](UWorld* /*FailedWorld*/, ETravelFailure::Type FailureType, const FString& ErrorString)
		{
			ReportFailure(DescribeTravelFailure(FailureType),
				FString::Printf(TEXT("%s%s%s"),
					ETravelFailure::ToString(FailureType),
					ErrorString.IsEmpty() ? TEXT("") : TEXT(": "),
					*ErrorString));
		});

	UE_LOG(LogTraceGame, Log, TEXT("[Net] Network and travel failure handlers bound."));
}

void LogNetworkDiagnostics(const UWorld* World, const TCHAR* Context)
{
	FString Endpoint;
	FString Detail;
	const ERole Role = DescribeConnection(World, Endpoint, Detail);

	const TCHAR* RoleName =
		(Role == ERole::Hosting) ? TEXT("HOSTING") :
		(Role == ERole::Client)  ? TEXT("CLIENT")  : TEXT("OFFLINE");

	TArray<FString> Addresses;
	GetLocalAddresses(Addresses);

	// Display, not Log: this line is the first thing anyone will be asked for when a playtest fails
	// to connect, and it has to survive the default verbosity of a normal run.
	UE_LOG(LogTraceGame, Display, TEXT("[Net] %s: %s  endpoint=%s  %s  localAddrs=[%s]"),
		Context, RoleName, *Endpoint, *Detail, *FString::Join(Addresses, TEXT(", ")));

	// The compatibility value, on the SAME line anyone is already asked for. A mismatch between two
	// machines is the failure this line exists to make a five-second check rather than an evening.
	UE_LOG(LogTraceGame, Display, TEXT("[Net] compatibility: %s  (\"%s\")  — this must match on every machine in the session."),
		*GetNetVersionLabel(), *GetNetVersionString());
}

// =================================================================================================
// THE NETWORK COMPATIBILITY VALUE
//
// See the long block on NetProtocolVersion in TraceNetworking.h for why this exists and what it
// trades away. This file is only the mechanism.
// =================================================================================================

FString GetNetVersionString()
{
	// Computed ONCE. Not for speed — this is called a handful of times per process — but because the
	// value must not be able to change under a live connection. GConfig is fully loaded by the time
	// anything asks (the earliest caller is the game module's StartupModule, which runs at
	// LoadingPhase Default, after config init), and a static local makes "read it once" the only
	// possible behaviour rather than a convention someone has to keep.
	static const FString Value = []() -> FString
	{
		FString ProjectVersion;
		if (GConfig != nullptr)
		{
			GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"),
				TEXT("ProjectVersion"), ProjectVersion, GGameIni);
		}
		ProjectVersion.TrimStartAndEndInline();

		// AN EMPTY ProjectVersion MUST NOT SILENTLY BECOME A DIFFERENT VALUE ON ONE MACHINE. If the
		// ini read fails on one side only — a corrupt config, a staged build missing DefaultGame.ini
		// — the two machines would compute different checksums and the mismatch would be blamed on
		// the platform. A fixed sentinel makes that case AGREE (and shows up in the printed string,
		// so it is visible rather than invisible).
		if (ProjectVersion.IsEmpty())
		{
			ProjectVersion = TEXT("unset");
		}

		// Lower case, matching the engine's own GetLocalNetworkVersion, which does .ToLower() before
		// hashing. Keeping the same convention means the two strings are comparable by eye.
		return FString::Printf(TEXT("trace netproto %d, project %s"),
			NetProtocolVersion, *ProjectVersion.ToLower());
	}();

	return Value;
}

uint32 GetNetVersionChecksum()
{
	static const uint32 Checksum = FCrc::StrCrc32(*GetNetVersionString());
	return Checksum;
}

FString GetNetVersionLabel()
{
	return FString::Printf(TEXT("NET %08X"), GetNetVersionChecksum());
}

void InstallNetVersionOverride()
{
	// IDEMPOTENT. BindStatic on an already-bound delegate would just replace the same binding, but
	// saying so here is cheaper than making a reader work it out, and the early return also means a
	// second caller cannot invalidate a checksum that is already in use by a live connection.
	if (FNetworkVersion::GetLocalNetworkVersionOverride.IsBound())
	{
		return;
	}

	FNetworkVersion::GetLocalNetworkVersionOverride.BindStatic(&GetNetVersionChecksum);

	// *** THIS LINE IS LOAD-BEARING AND ITS ABSENCE WOULD BE INVISIBLE. ***
	// FNetworkVersion::GetLocalNetworkVersion caches its answer in a static the first time anything
	// asks (NetworkVersion.cpp:223 — "if (bHasCachedNetworkChecksum) return CachedNetworkChecksum;")
	// and only consults the delegate on a cache MISS. Anything that asked before this bind — and the
	// engine does ask, e.g. when building the user agent string — would have burned the DEFAULT
	// value in, and this override would then do nothing at all while appearing to be installed.
	FNetworkVersion::InvalidateNetworkChecksum();

	UE_LOG(LogTraceGame, Display,
		TEXT("[Net] compatibility value pinned by the project: %s  (\"%s\"). "
		     "Every machine in a session must print this same value; the engine's own default "
		     "would have mixed in the engine install's changelist and could differ between "
		     "a Mac and a Windows build of this same commit."),
		*GetNetVersionLabel(), *GetNetVersionString());
}

// -------------------------------------------------------------------------------------------------
// Trace.NetVersion — the console-side check.
//
// Registered unconditionally, including Shipping: IConsoleManager is compiled in there and this
// command reads state and prints, so there is no cheat surface to gate. The console itself is
// unavailable in a Shipping game, which is exactly why the value ALSO goes on the title screen
// (ATraceMenuHUD's version label) — that is the check a player can actually run.
// -------------------------------------------------------------------------------------------------
static FAutoConsoleCommand GTraceNetVersionCmd(
	TEXT("Trace.NetVersion"),
	TEXT("Print this build's network compatibility value. Two machines can only connect if they print the same one."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		UE_LOG(LogTraceGame, Display, TEXT("[Net] %s  (\"%s\")  protocol=%d  override=%s"),
			*TraceNet::GetNetVersionLabel(), *TraceNet::GetNetVersionString(),
			TraceNet::NetProtocolVersion,
			FNetworkVersion::GetLocalNetworkVersionOverride.IsBound() ? TEXT("installed") : TEXT("NOT INSTALLED"));

		// The engine's own answer as well, so the command proves the override is actually the thing
		// the handshake uses instead of merely asserting it. These two MUST agree; if they do not,
		// something asked for the network version before InstallNetVersionOverride() ran.
		const uint32 EngineSays = FNetworkVersion::GetLocalNetworkVersion();
		UE_LOG(LogTraceGame, Display, TEXT("[Net] engine handshake will send: NET %08X  %s"),
			EngineSays,
			(EngineSays == TraceNet::GetNetVersionChecksum())
				? TEXT("(matches — the override is in force)")
				: TEXT("*** DOES NOT MATCH THE OVERRIDE — the engine cached a value before startup ***"));
	}));

} // namespace TraceNet

// =================================================================================================
// FTraceTextEntry
// =================================================================================================

namespace
{
	/** One polled key and the characters it produces unshifted / shifted. */
	struct FTraceTextKey
	{
		FKey (*Key)();
		TCHAR Plain;
		TCHAR Shifted;
	};

	/**
	 * The whole typeable alphabet of this field.
	 *
	 * Restricted to what an address can legally contain — letters (hostnames), digits, '.', ':', '-'
	 * and '_'. That is not laziness: a field that physically cannot hold a '/' is a field that cannot
	 * be handed a string ClientTravel will choke on.
	 *
	 * ':' is produced by the SEMICOLON key whether or not shift is held. On a US layout ':' *is*
	 * shift+';', and ';' has no meaning in an address, so mapping the unshifted key to ':' as well
	 * costs nothing and quietly rescues anyone on a layout where shift is not detected.
	 *
	 * WP2.3 — THE TABLE IS SHARED; THE FILTER IS PER-CHARSET. The space bar is appended here (it is
	 * meaningless in an address and legal in a name) and the CallSign filter below is what decides
	 * which of these rows a given opening will actually accept. One table, so a key added for one
	 * alphabet cannot be missing from the other by accident, and one predicate, so the typed path and
	 * the pasted path cannot disagree about what a legal character is.
	 */
	const TArray<FTraceTextKey>& TextKeyTable()
	{
		static const TArray<FTraceTextKey> Table =
		{
			{ []{ return EKeys::A; }, TEXT('a'), TEXT('A') },
			{ []{ return EKeys::B; }, TEXT('b'), TEXT('B') },
			{ []{ return EKeys::C; }, TEXT('c'), TEXT('C') },
			{ []{ return EKeys::D; }, TEXT('d'), TEXT('D') },
			{ []{ return EKeys::E; }, TEXT('e'), TEXT('E') },
			{ []{ return EKeys::F; }, TEXT('f'), TEXT('F') },
			{ []{ return EKeys::G; }, TEXT('g'), TEXT('G') },
			{ []{ return EKeys::H; }, TEXT('h'), TEXT('H') },
			{ []{ return EKeys::I; }, TEXT('i'), TEXT('I') },
			{ []{ return EKeys::J; }, TEXT('j'), TEXT('J') },
			{ []{ return EKeys::K; }, TEXT('k'), TEXT('K') },
			{ []{ return EKeys::L; }, TEXT('l'), TEXT('L') },
			{ []{ return EKeys::M; }, TEXT('m'), TEXT('M') },
			{ []{ return EKeys::N; }, TEXT('n'), TEXT('N') },
			{ []{ return EKeys::O; }, TEXT('o'), TEXT('O') },
			{ []{ return EKeys::P; }, TEXT('p'), TEXT('P') },
			{ []{ return EKeys::Q; }, TEXT('q'), TEXT('Q') },
			{ []{ return EKeys::R; }, TEXT('r'), TEXT('R') },
			{ []{ return EKeys::S; }, TEXT('s'), TEXT('S') },
			{ []{ return EKeys::T; }, TEXT('t'), TEXT('T') },
			{ []{ return EKeys::U; }, TEXT('u'), TEXT('U') },
			{ []{ return EKeys::V; }, TEXT('v'), TEXT('V') },
			{ []{ return EKeys::W; }, TEXT('w'), TEXT('W') },
			{ []{ return EKeys::X; }, TEXT('x'), TEXT('X') },
			{ []{ return EKeys::Y; }, TEXT('y'), TEXT('Y') },
			{ []{ return EKeys::Z; }, TEXT('z'), TEXT('Z') },

			{ []{ return EKeys::Zero;  }, TEXT('0'), TEXT('0') },
			{ []{ return EKeys::One;   }, TEXT('1'), TEXT('1') },
			{ []{ return EKeys::Two;   }, TEXT('2'), TEXT('2') },
			{ []{ return EKeys::Three; }, TEXT('3'), TEXT('3') },
			{ []{ return EKeys::Four;  }, TEXT('4'), TEXT('4') },
			{ []{ return EKeys::Five;  }, TEXT('5'), TEXT('5') },
			{ []{ return EKeys::Six;   }, TEXT('6'), TEXT('6') },
			{ []{ return EKeys::Seven; }, TEXT('7'), TEXT('7') },
			{ []{ return EKeys::Eight; }, TEXT('8'), TEXT('8') },
			{ []{ return EKeys::Nine;  }, TEXT('9'), TEXT('9') },

			{ []{ return EKeys::NumPadZero;  }, TEXT('0'), TEXT('0') },
			{ []{ return EKeys::NumPadOne;   }, TEXT('1'), TEXT('1') },
			{ []{ return EKeys::NumPadTwo;   }, TEXT('2'), TEXT('2') },
			{ []{ return EKeys::NumPadThree; }, TEXT('3'), TEXT('3') },
			{ []{ return EKeys::NumPadFour;  }, TEXT('4'), TEXT('4') },
			{ []{ return EKeys::NumPadFive;  }, TEXT('5'), TEXT('5') },
			{ []{ return EKeys::NumPadSix;   }, TEXT('6'), TEXT('6') },
			{ []{ return EKeys::NumPadSeven; }, TEXT('7'), TEXT('7') },
			{ []{ return EKeys::NumPadEight; }, TEXT('8'), TEXT('8') },
			{ []{ return EKeys::NumPadNine;  }, TEXT('9'), TEXT('9') },

			{ []{ return EKeys::Period;     }, TEXT('.'), TEXT('.') },
			{ []{ return EKeys::Decimal;    }, TEXT('.'), TEXT('.') },
			{ []{ return EKeys::Semicolon;  }, TEXT(':'), TEXT(':') },
			{ []{ return EKeys::Colon;      }, TEXT(':'), TEXT(':') },
			{ []{ return EKeys::Hyphen;     }, TEXT('-'), TEXT('_') },
			{ []{ return EKeys::Subtract;   }, TEXT('-'), TEXT('_') },
			{ []{ return EKeys::Underscore; }, TEXT('_'), TEXT('_') },

			// WP2.3 — legal in a CALL SIGN only; IsLegalChar refuses it in Address mode, where a
			// space in a travel URL is a silent connection failure.
			{ []{ return EKeys::SpaceBar;   }, TEXT(' '), TEXT(' ') },
		};
		return Table;
	}

	/**
	 * WP2.3 — THE ONE definition of "legal character", asked by the typed path AND the pasted one.
	 *
	 * Two filters would be two answers: the shipped paste filter already listed its own five
	 * punctuation marks by hand, and a charset added later would have had to be remembered in both
	 * places. It is not remembered in both places — it is asked here.
	 */
	bool IsLegalChar(ETraceTextCharset Charset, TCHAR Char)
	{
		if (FChar::IsAlnum(Char))
		{
			return true;
		}

		if (Charset == ETraceTextCharset::CallSign)
		{
			// SPACE, '-', '_', '.'. ':' and ';' are refused — see ETraceTextCharset in the header.
			return Char == TEXT(' ') || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('.');
		}

		return Char == TEXT('.') || Char == TEXT(':') || Char == TEXT('-') || Char == TEXT('_');
	}

	bool IsShiftDown(const APlayerController* PC)
	{
		return PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift);
	}

	/** Ctrl on Windows/Linux, Command on macOS. Both are accepted everywhere; nothing else uses them. */
	bool IsPasteModifierDown(const APlayerController* PC)
	{
		return PC->IsInputKeyDown(EKeys::LeftControl) || PC->IsInputKeyDown(EKeys::RightControl)
			|| PC->IsInputKeyDown(EKeys::LeftCommand) || PC->IsInputKeyDown(EKeys::RightCommand);
	}
}

void FTraceTextEntry::Begin(const FString& InitialText, ETraceTextCharset InCharset, int32 InMaxLength)
{
	bActive = true;
	bSubmitted = false;
	bCancelled = false;

	// Set BEFORE the Left() below, or the very first thing this field does is apply the PREVIOUS
	// opening's cap to the new opening's text.
	Charset = InCharset;
	MaxLength = FMath::Max(1, InMaxLength);

	Text = InitialText.Left(MaxLength);
	Caret = Text.Len();
	LastEditTime = 0.f;
	bRepeatArmed = false;

	// The key that opened the field is still "just pressed" for the rest of this frame.
	IgnoreInputBeforeFrame = GFrameCounter + 1;
}

void FTraceTextEntry::End()
{
	bActive = false;
	bSubmitted = false;
	bCancelled = false;
	bRepeatArmed = false;
}

void FTraceTextEntry::SetText(const FString& InText)
{
	Text = InText.Left(MaxLength);
	Caret = FMath::Clamp(Caret, 0, Text.Len());
}

bool FTraceTextEntry::ConsumeSubmit()
{
	const bool bResult = bSubmitted;
	bSubmitted = false;
	return bResult;
}

bool FTraceTextEntry::ConsumeCancel()
{
	const bool bResult = bCancelled;
	bCancelled = false;
	return bResult;
}

bool FTraceTextEntry::IsCaretVisible(float Now) const
{
	// Phase measured from the last keystroke so the caret is always SOLID while the player is
	// actually typing, and only starts blinking once they stop. A caret that blinks out mid-keypress
	// reads as dropped input.
	return FMath::Fmod(FMath::Max(0.f, Now - LastEditTime), 1.0f) < 0.62f;
}

bool FTraceTextEntry::InsertChar(TCHAR Char)
{
	if (Text.Len() >= MaxLength)
	{
		return false;
	}

	// WP2.3 — the charset gate, at the ONE point every character enters the string. Both callers
	// (the key table and the paste loop) funnel here, so a key that is illegal for this opening
	// cannot arrive by either route. The space bar is in the shared table and is refused here for an
	// address; ':' is in the table and is refused here for a call sign.
	if (!IsLegalChar(Charset, Char))
	{
		return false;
	}

	Text.InsertAt(Caret, Char);
	++Caret;
	return true;
}

void FTraceTextEntry::Backspace()
{
	if (Caret <= 0)
	{
		return;
	}
	Text.RemoveAt(Caret - 1, 1);
	--Caret;
}

void FTraceTextEntry::DeleteForward()
{
	if (Caret >= Text.Len())
	{
		return;
	}
	Text.RemoveAt(Caret, 1);
}

FString FTraceTextEntry::ReadClipboard()
{
#if TRACE_HAS_CLIPBOARD
	FString Clipboard;
	FPlatformApplicationMisc::ClipboardPaste(Clipboard);
	return Clipboard;
#else
	return FString();
#endif
}

void FTraceTextEntry::Poll(APlayerController* PC, float Now)
{
	if (!bActive || PC == nullptr || GFrameCounter < IgnoreInputBeforeFrame)
	{
		return;
	}

	// ---- Commit and abandon --------------------------------------------------------------------
	if (PC->WasInputKeyJustPressed(EKeys::Enter))
	{
		bSubmitted = true;
		return;
	}
	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		bCancelled = true;
		return;
	}

	const bool bShift = IsShiftDown(PC);
	const bool bPasteModifier = IsPasteModifierDown(PC);

	// ---- Paste ---------------------------------------------------------------------------------
	//
	// Checked BEFORE the character table, because 'V' is in that table: without this, Cmd+V would
	// paste the clipboard AND type a 'v'.
	if (bPasteModifier && PC->WasInputKeyJustPressed(EKeys::V))
	{
		const FString Clipboard = ReadClipboard();
		if (!Clipboard.IsEmpty())
		{
			// One line only, and only the legal characters. People paste "  100.1.2.3:7777\n" out of
			// a terminal all the time, and a trailing newline in a travel URL is a silent failure.
			FString Cleaned;
			for (const TCHAR Char : Clipboard)
			{
				if (IsLegalChar(Charset, Char))
				{
					Cleaned.AppendChar(Char);
				}
				else if (Char == TEXT('\n') || Char == TEXT('\r'))
				{
					break;
				}
			}

			if (!Cleaned.IsEmpty())
			{
				for (const TCHAR Char : Cleaned)
				{
					InsertChar(Char);
				}
				LastPasteTime = Now;
				LastEditTime = Now;
			}
		}
		return;
	}

	// ---- Caret movement ------------------------------------------------------------------------
	if (PC->WasInputKeyJustPressed(EKeys::Left))
	{
		Caret = FMath::Max(0, Caret - 1);
		LastEditTime = Now;
	}
	if (PC->WasInputKeyJustPressed(EKeys::Right))
	{
		Caret = FMath::Min(Text.Len(), Caret + 1);
		LastEditTime = Now;
	}
	if (PC->WasInputKeyJustPressed(EKeys::Home) || PC->WasInputKeyJustPressed(EKeys::Up))
	{
		Caret = 0;
		LastEditTime = Now;
	}
	if (PC->WasInputKeyJustPressed(EKeys::End) || PC->WasInputKeyJustPressed(EKeys::Down))
	{
		Caret = Text.Len();
		LastEditTime = Now;
	}

	// ---- Deletion, with repeat -----------------------------------------------------------------
	//
	// Backspace is the one key here worth holding down: correcting a mistyped 15-character address
	// one discrete press at a time is exactly the sort of thing that makes a hand-rolled field feel
	// like a prototype.
	const bool bBackspaceDown = PC->IsInputKeyDown(EKeys::BackSpace);
	if (PC->WasInputKeyJustPressed(EKeys::BackSpace))
	{
		Backspace();
		LastEditTime = Now;
		bRepeatArmed = true;
		NextRepeatTime = Now + RepeatDelay;
	}
	else if (bBackspaceDown && bRepeatArmed && Now >= NextRepeatTime)
	{
		Backspace();
		LastEditTime = Now;
		NextRepeatTime = Now + RepeatInterval;
	}
	else if (!bBackspaceDown)
	{
		bRepeatArmed = false;
	}

	if (PC->WasInputKeyJustPressed(EKeys::Delete))
	{
		DeleteForward();
		LastEditTime = Now;
	}

	// ---- Characters ----------------------------------------------------------------------------
	for (const FTraceTextKey& Entry : TextKeyTable())
	{
		if (PC->WasInputKeyJustPressed(Entry.Key()))
		{
			// Only a key that actually LANDED restarts the caret's blink phase. A space bar pressed
			// into an address field is refused (WP2.3), and a refused key that still froze the caret
			// solid would look exactly like a key that had been accepted.
			if (InsertChar(bShift ? Entry.Shifted : Entry.Plain))
			{
				LastEditTime = Now;
			}
		}
	}
}
