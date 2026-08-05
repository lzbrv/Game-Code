// Trace — multiplayer entry-point support. See TraceNetworking.h.

#include "UI/TraceNetworking.h"

#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreMisc.h"
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
	case ENetworkFailure::OutdatedClient:
		return TEXT("YOUR BUILD IS OLDER THAN THE SERVER'S. REBUILD AND TRY AGAIN.");
	case ENetworkFailure::OutdatedServer:
		return TEXT("THE SERVER'S BUILD IS OLDER THAN YOURS. THE HOST MUST REBUILD.");
	case ENetworkFailure::PendingConnectionFailure:
		return TEXT("COULD NOT REACH THAT ADDRESS.");
	case ENetworkFailure::NetGuidMismatch:
	case ENetworkFailure::NetChecksumMismatch:
		return TEXT("CLIENT AND SERVER ARE RUNNING DIFFERENT BUILDS.");
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
}

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
		};
		return Table;
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

void FTraceTextEntry::Begin(const FString& InitialText)
{
	bActive = true;
	bSubmitted = false;
	bCancelled = false;
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

void FTraceTextEntry::InsertChar(TCHAR Char)
{
	if (Text.Len() >= MaxLength)
	{
		return;
	}

	Text.InsertAt(Caret, Char);
	++Caret;
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
				if (FChar::IsAlnum(Char) || Char == TEXT('.') || Char == TEXT(':')
					|| Char == TEXT('-') || Char == TEXT('_'))
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
			InsertChar(bShift ? Entry.Shifted : Entry.Plain);
			LastEditTime = Now;
		}
	}
}
