// Trace — the five characters, as data. See TraceCharacterRoster.h.
//
// Since spec v17 §5 this file answers All() from ONE OF TWO PLACES: the five
// UTraceCharacterDefinition assets under /Game/Trace/Data/Characters, or the C++ table below. The
// C++ table is not dead code kept for sentiment — it is the fallback rule §0.1 requires, it is what
// the generator reads when it authors the assets, and it is what Trace.VerifyCharacterData compares
// the assets against. Deleting it would make the migration untestable and unbackable-out-of.

#include "Core/TraceCharacterRoster.h"

#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId — asserted against, never used as a type here
#include "Data/TraceCharacterDefinition.h"

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Trace.h"
#include "UObject/UObjectGlobals.h"

// ---------------------------------------------------------------------------------------------
// THE ID SPACE, ASSERTED RATHER THAN ASSUMED.
//
// This file stores character ids as plain uint8 so the UI slice can draw a menu without taking the
// reflected ability headers. That is only safe as long as the two numberings are the same numbering,
// and "the same numbering" is exactly the kind of agreement that survives right up until somebody
// inserts an enumerator. So it is checked at compile time, here, in the one translation unit that
// can see both — a static_assert cannot be argued with, cannot go stale, and costs nothing.
// ---------------------------------------------------------------------------------------------

static_assert(static_cast<uint8>(ETraceCharacterId::None)   == TraceCharacterRoster::NoneId,
	"TraceCharacterRoster::NoneId must equal ETraceCharacterId::None.");
static_assert(static_cast<uint8>(ETraceCharacterId::Rocco)  == TraceCharacterRoster::FirstId,
	"TraceCharacterRoster::FirstId must equal ETraceCharacterId::Rocco.");
static_assert(static_cast<uint8>(ETraceCharacterId::X)      == TraceCharacterRoster::LastId,
	"TraceCharacterRoster::LastId must equal ETraceCharacterId::X.");
static_assert(TraceCharacterCount == TraceCharacterRoster::Count,
	"The roster table and ETraceCharacterId disagree about how many characters there are.");

// The file's own namespace rather than an anonymous one. UBT compiles this module as a unity/jumbo
// build, so two files that each say `namespace { ... }` become one namespace with two definitions of
// everything they share a name with — GetTable() being about as collidable a name as exists. This
// project has already lost a Windows build that way; Scripts/check-jumbo-build-collisions.py gates
// on it and build.sh runs the check.
namespace TraceCharacterRosterFile
{
	/**
	 * THE C++ TABLE.
	 *
	 * Function-local static rather than a file-scope global: FLinearColor has a non-trivial
	 * constructor, and a file-scope TArray of them would run before main() in an order no standard
	 * guarantees. Everything in this file is read-only after first use, so the one-time initialisation
	 * is thread-safe by C++11 magic-statics.
	 *
	 * THE PROSE IS THE SPEC'S, TRIMMED TO FIT A CARD. Where a number was assumed rather than stated
	 * (Mace's and Oyster's cooldowns) the card still prints it, because a player choosing between
	 * five characters needs to compare them on the same axes — but see the report: those two are
	 * [ASSUMPTION] 20 s and the ability framework, not this table, is what actually enforces any of it.
	 *
	 * *** THIS IS THE ORIGIN OF THE GENERATED ASSETS. *** Scripts/generate-data-assets.py contains no
	 * character data at all; it calls UTraceCharacterDefinition::CopyFallbackValues, which reads this.
	 * Change a word here and regenerate, and the assets follow. Change a word in an asset and this
	 * does not follow — Trace.VerifyCharacterData is what tells you.
	 */
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& GetCppTable()
	{
		static const TArray<TraceCharacterRoster::FTraceCharacterEntry> Table =
		{
			{
				1, TEXT("ROCCO"),
				TEXT("A VERY SMALL SECOND JUMP. THE POINT IS THE INSTANT MIDAIR DIRECTION CHANGE, NOT THE HEIGHT."),
				TEXT("HEADSHOT KILLS GIVE +3% SPEED FOR 1S. STACKS, AND EACH KILL EXTENDS THE WHOLE BOOST."),
				TEXT("RIPPLE"),
				TEXT("DASH ON ITS OWN COOLDOWN, LEAVING A RIPPLE FOR 4S. ANYONE ON EITHER TEAM CAN RIDE IT, "
				     "INCLUDING THE CORE CARRIER, AND YOU CAN SHOOT WHILE RIDING."),
				20.f,
				FLinearColor(1.00f, 0.86f, 0.25f, 1.f)    // amber
			},
			{
				2, TEXT("CHUT"),
				TEXT("BASH. HITTING A PLAYER WITH THE END OF A STANDARD DASH KNOCKS THEM ALONG YOUR TRAVEL. "
				     "NO EFFECT ON THE CORE CARRIER."),
				TEXT("KNIFE DEALS 50 FROM THE FRONT INSTEAD OF 30. THE 60 DEGREE BACK ZONE STAYS AT 100."),
				TEXT("CHUD"),
				TEXT("TAKE 30% LESS DAMAGE FROM BODY SHOTS AND MELEES FOR 10S. A KNIFE KILL REFRESHES THE "
				     "TIMER. DOES NOT STACK."),
				20.f,
				FLinearColor(0.35f, 0.95f, 0.55f, 1.f)    // green
			},
			{
				3, TEXT("MACE"),
				TEXT("HOLD V IN THE AIR TO SUSPEND FOR UP TO 1.25S. NO GRAVITY, LATERAL MOVE CAPPED AT 550. "
				     "RELEASING V CANCELS INSTANTLY."),
				TEXT("+30% CORE MAGNET RADIUS. THE BASE IS 450, SO HERS IS 585."),
				TEXT("SPIKE"),
				TEXT("THROW A ROPED SPIKE. IT EMBEDS IN A WALL FOR 2S; PRESS AGAIN TO BE PULLED TO IT AT THE "
				     "MOMENTUM CEILING. ANY MOVEMENT INPUT CANCELS. YOU CAN SHOOT AND BE SHOT WHILE PULLED."),
				20.f,
				FLinearColor(0.65f, 0.55f, 1.00f, 1.f)    // violet
			},
			{
				4, TEXT("OYSTER"),
				TEXT("JUMPING WHILE STOOD ON ONE OF YOUR OWN JARS BREAKS IT AND BOOSTS YOU UPWARD."),
				TEXT("EVERY DASH LEAVES A POISON JAR, EVEN CARRYING THE CORE. AN ENEMY WHO TOUCHES ONE "
				     "BREAKS IT: 3 DAMAGE EVERY 0.5S FOR 4S AND -30% SPEED. JARS LAST 4S, MAX 3."),
				TEXT("PICKLER"),
				TEXT("LOB A JAR. ON LANDING IT DEALS 30 IN AN AREA AND PULLS NEARBY ENEMIES IN - THEN STAYS "
				     "ON THE GROUND AS A NORMAL JAR."),
				20.f,
				FLinearColor(0.30f, 0.85f, 0.95f, 1.f)    // cyan
			},
			{
				5, TEXT("X"),
				TEXT("+10% SPEED WHILE ANY ENEMY IS VULNERABLE."),
				TEXT("FIVE MECHANICAL BEES ORBIT YOU. AN ENEMY THEY TOUCH IS VULNERABLE FOR 2S AND TAKES "
				     "+25% DAMAGE FROM EVERYTHING. DOES NOT STACK; A NEW HIT RESETS THE TIMER."),
				TEXT("STING"),
				TEXT("LOAD THE 5 BEES INTO YOUR GUN. YOUR NEXT FIVE BULLETS APPLY VULNERABLE AT NORMAL "
				     "DAMAGE. THE BEES RESUME ORBITING ONCE ALL FIVE ARE FIRED."),
				25.f,
				FLinearColor(1.00f, 0.40f, 0.55f, 1.f)    // rose
			}
		};

		return Table;
	}

	// =============================================================================================
	// THE ASSET PATH
	// =============================================================================================

	/**
	 * The five entries built from the assets, and the strings they point INTO.
	 *
	 * FTraceCharacterEntry holds `const TCHAR*`, because it is read by a Canvas HUD that draws every
	 * frame and by a header that deliberately takes no dependency on UObject. Those pointers must
	 * outlive every draw, so the FStrings that own the characters live here, for the life of the
	 * process, and are never touched after the table is built.
	 *
	 * WHY THAT IS SAFE, stated rather than hoped: FString's characters are a heap allocation owned by
	 * a TArray (UE has no small-string optimisation), so the address of the first character does not
	 * move when the FString is moved. Even so, Strings is RESERVED TO ITS FINAL SIZE before the first
	 * Add, so nothing is ever moved at all. Both belts; the trousers stay up either way.
	 */
	struct FAssetRosterStorage
	{
		TArray<FString> Strings;
		TArray<TraceCharacterRoster::FTraceCharacterEntry> Entries;

		void Reset()
		{
			Entries.Reset();
			Strings.Reset();
		}
	};

	FAssetRosterStorage& AssetStorage()
	{
		static FAssetRosterStorage Storage;
		return Storage;
	}

	bool GResolved = false;
	TraceCharacterRoster::ESource GSource = TraceCharacterRoster::ESource::CppTable;

	void OnUseAssetsCVarChanged(IConsoleVariable* /*Variable*/);

	/**
	 * RULE §0.1's toggle. 1 (default) = use the assets when all five are present and valid;
	 * 0 = the C++ table, unconditionally, exactly as the game behaved before spec v17 §5.
	 *
	 * Default 1 rather than 0 because spec v17 §5 asks for "loads definitions from assets when
	 * present", the assets are generated FROM the C++ values, and Trace.VerifyCharacterData proves
	 * field by field that the two agree. The fallback is not the off-switch — the fallback is
	 * automatic and silent-except-in-the-log whenever the assets are missing or wrong. This toggle
	 * is the manual override for the day somebody needs to bisect a bug against the old path.
	 */
	TAutoConsoleVariable<int32> CVarUseCharacterAssets(
		TEXT("Trace.Data.UseCharacterAssets"),
		1,
		TEXT("Trace, spec v17 §5. 1 = the roster reads /Game/Trace/Data/Characters when all five assets\n")
		TEXT("load and validate; 0 = always use the C++ table compiled into TraceCharacterRoster.cpp.\n")
		TEXT("Either way the game plays identically — the assets are generated from the C++ values and\n")
		TEXT("Trace.VerifyCharacterData proves it. Changing this re-resolves the roster immediately."),
		FConsoleVariableDelegate::CreateStatic(&OnUseAssetsCVarChanged),
		ECVF_Default);

	/**
	 * Load and validate all five, or none.
	 *
	 * @param OutWhyNot  why the C++ table is being used, phrased for a log line a designer will read.
	 * @return true only when Storage holds five good entries in id order.
	 */
	bool TryBuildFromAssets(FAssetRosterStorage& Storage, FString& OutWhyNot)
	{
		Storage.Reset();

		// FIVE strings per character (name, movement, passive, activated name, activated), reserved
		// up front so no Add can ever reallocate — see FAssetRosterStorage's comment.
		Storage.Strings.Reserve(TraceCharacterRoster::Count * 5);
		Storage.Entries.Reserve(TraceCharacterRoster::Count);

		for (int32 IdValue = TraceCharacterRoster::FirstId; IdValue <= TraceCharacterRoster::LastId; ++IdValue)
		{
			const ETraceCharacterId TypedId = static_cast<ETraceCharacterId>(IdValue);
			const FString PackagePath = UTraceCharacterDefinition::PackagePathFor(TypedId);
			const FString ObjectPath  = UTraceCharacterDefinition::ObjectPathFor(TypedId);

			// Asked BEFORE LoadObject so that the ordinary "the assets have not been generated yet"
			// case produces one calm log line rather than five engine load warnings.
			if (!FPackageName::DoesPackageExist(PackagePath))
			{
				OutWhyNot = FString::Printf(TEXT("%s does not exist"), *PackagePath);
				Storage.Reset();
				return false;
			}

			UTraceCharacterDefinition* const Definition =
				LoadObject<UTraceCharacterDefinition>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);

			if (Definition == nullptr)
			{
				OutWhyNot = FString::Printf(TEXT("%s exists but did not load as a UTraceCharacterDefinition"), *ObjectPath);
				Storage.Reset();
				return false;
			}

			FString WhyUnusable;
			if (!Definition->IsUsable(WhyUnusable))
			{
				OutWhyNot = FString::Printf(TEXT("%s is not usable: %s"), *ObjectPath, *WhyUnusable);
				Storage.Reset();
				return false;
			}

			if (static_cast<int32>(Definition->CharacterId) != IdValue)
			{
				// The file name and the id inside it disagree. Refusing is the only safe answer: the
				// id is an array index in the select screen's card layout, so a table out of id order
				// would draw ROCCO's card and hand the player CHUT.
				OutWhyNot = FString::Printf(TEXT("%s holds CharacterId %d but its name says %d"),
					*ObjectPath, static_cast<int32>(Definition->CharacterId), IdValue);
				Storage.Reset();
				return false;
			}

			const int32 NameIndex      = Storage.Strings.Add(Definition->DisplayName);
			const int32 MovementIndex  = Storage.Strings.Add(Definition->MovementSlot.Description);
			const int32 PassiveIndex   = Storage.Strings.Add(Definition->PassiveSlot.Description);
			const int32 ActivatedNameIndex = Storage.Strings.Add(Definition->ActivatedSlot.DisplayName);
			const int32 ActivatedIndex = Storage.Strings.Add(Definition->ActivatedSlot.Description);

			TraceCharacterRoster::FTraceCharacterEntry Entry;
			Entry.Id                = static_cast<uint8>(IdValue);
			Entry.Name              = *Storage.Strings[NameIndex];
			Entry.Movement          = *Storage.Strings[MovementIndex];
			Entry.Passive           = *Storage.Strings[PassiveIndex];
			Entry.ActivatedName     = *Storage.Strings[ActivatedNameIndex];
			Entry.Activated         = *Storage.Strings[ActivatedIndex];
			Entry.ActivatedCooldown = Definition->ActivatedSlot.CooldownSeconds;
			Entry.Accent            = Definition->AccentColor;

			Storage.Entries.Add(Entry);
		}

		if (Storage.Entries.Num() != TraceCharacterRoster::Count)
		{
			OutWhyNot = FString::Printf(TEXT("built %d entries, expected %d"),
				Storage.Entries.Num(), TraceCharacterRoster::Count);
			Storage.Reset();
			return false;
		}

		OutWhyNot.Reset();
		return true;
	}

	/**
	 * Decide once, then answer from the decision.
	 *
	 * GAME THREAD ONLY, like everything that reads this table (the HUD's draw, the game mode's log
	 * lines, the select screen). LoadObject is game-thread work and the storage is not guarded.
	 */
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Resolve()
	{
		if (!GResolved)
		{
			// DO NOT LATCH BEFORE THE ENGINE EXISTS. Something could ask the roster for a name during
			// static initialisation or very early module startup, long before any package can be
			// loaded; answering "C++ table" is right for that call and WRONG to remember forever.
			if (GEngine == nullptr)
			{
				return GetCppTable();
			}

			GResolved = true;
			GSource = TraceCharacterRoster::ESource::CppTable;

			if (CVarUseCharacterAssets.GetValueOnGameThread() == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[CharacterData] Roster source: C++ table — Trace.Data.UseCharacterAssets is 0. "
					     "The five characters are exactly what they were before spec v17 §5."));
			}
			else
			{
				FString WhyNot;
				if (TryBuildFromAssets(AssetStorage(), WhyNot))
				{
					GSource = TraceCharacterRoster::ESource::Assets;
					UE_LOG(LogTraceGame, Display,
						TEXT("[CharacterData] Roster source: ASSETS — all %d loaded from %s. "
						     "Run Trace.VerifyCharacterData to compare them with the C++ table."),
						TraceCharacterRoster::Count, UTraceCharacterDefinition::CharactersPackageRoot());
				}
				else
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[CharacterData] Roster source: C++ table (FALLBACK) — %s. This is not an error: "
						     "the game plays exactly as it did before spec v17 §5. Run "
						     "Scripts/generate-data-assets.py to author the assets."),
						*WhyNot);
				}
			}
		}

		return (GSource == TraceCharacterRoster::ESource::Assets) ? AssetStorage().Entries : GetCppTable();
	}

	void OnUseAssetsCVarChanged(IConsoleVariable* /*Variable*/)
	{
		TraceCharacterRoster::ForceReload();
	}
}

const TArray<TraceCharacterRoster::FTraceCharacterEntry>& TraceCharacterRoster::All()
{
	return TraceCharacterRosterFile::Resolve();
}

const TArray<TraceCharacterRoster::FTraceCharacterEntry>& TraceCharacterRoster::CppFallbackTable()
{
	return TraceCharacterRosterFile::GetCppTable();
}

TraceCharacterRoster::ESource TraceCharacterRoster::CurrentSource()
{
	TraceCharacterRosterFile::Resolve();
	return TraceCharacterRosterFile::GSource;
}

const TCHAR* TraceCharacterRoster::CurrentSourceName()
{
	return (CurrentSource() == ESource::Assets)
		? TEXT("assets (/Game/Trace/Data/Characters)")
		: TEXT("C++ table (Core/TraceCharacterRoster.cpp)");
}

void TraceCharacterRoster::ForceReload()
{
	// Callers hold FTraceCharacterEntry pointers only for the duration of one draw or one log line,
	// and this runs from a console command, i.e. between frames on the game thread. Anything that
	// cached an entry ACROSS frames was already wrong — All() has never promised a stable address.
	TraceCharacterRosterFile::GResolved = false;
	TraceCharacterRosterFile::GSource = ESource::CppTable;
	TraceCharacterRosterFile::AssetStorage().Reset();
}

const TraceCharacterRoster::FTraceCharacterEntry* TraceCharacterRoster::Find(uint8 Id)
{
	if (!IsValidId(Id))
	{
		return nullptr;
	}

	// The table is authored in id order and asserted so below, which makes this an index rather than
	// a search. If the assertion ever fires, fix the TABLE — do not turn this into a linear scan,
	// because the id is also an array index in the select screen's card layout. (The asset loader
	// enforces the same order before it will serve a single row, for the same reason.)
	const TArray<FTraceCharacterEntry>& Table = All();

	const int32 Index = static_cast<int32>(Id) - static_cast<int32>(FirstId);
	if (!Table.IsValidIndex(Index))
	{
		return nullptr;
	}

	checkf(Table[Index].Id == Id,
		TEXT("TraceCharacterRoster: the table is out of id order at index %d (holds %d, expected %d)."),
		Index, static_cast<int32>(Table[Index].Id), static_cast<int32>(Id));

	return &Table[Index];
}

bool TraceCharacterRoster::IsValidId(uint8 Id)
{
	return Id >= FirstId && Id <= LastId;
}

FString TraceCharacterRoster::NameFor(uint8 Id)
{
	if (Id == NoneId)
	{
		// Never "NONE". This string ends up in a kill feed, a log line and on the select screen's
		// "you are playing as" footer, and "none" reads as a fault where "mannequin" reads as a
		// deliberate state — which is exactly what it is in mode A, with the characters toggle off,
		// and for anybody a full team roster could not serve. (It is no longer the bots' permanent
		// state: spec v15 §2 gives them characters too.)
		return TEXT("MANNEQUIN");
	}

	if (const FTraceCharacterEntry* Entry = Find(Id))
	{
		return Entry->Name;
	}

	return FString::Printf(TEXT("CHARACTER %d"), static_cast<int32>(Id));
}
