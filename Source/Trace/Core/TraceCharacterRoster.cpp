// Trace — the five characters, as data. See TraceCharacterRoster.h.

#include "Core/TraceCharacterRoster.h"

#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId — asserted against, never used as a type here

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

namespace
{
	/**
	 * The table itself.
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
	 */
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& GetTable()
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
}

const TArray<TraceCharacterRoster::FTraceCharacterEntry>& TraceCharacterRoster::All()
{
	return GetTable();
}

const TraceCharacterRoster::FTraceCharacterEntry* TraceCharacterRoster::Find(uint8 Id)
{
	if (!IsValidId(Id))
	{
		return nullptr;
	}

	// The table is authored in id order and asserted so below, which makes this an index rather than
	// a search. If the assertion ever fires, fix the TABLE — do not turn this into a linear scan,
	// because the id is also an array index in the select screen's card layout.
	const TArray<FTraceCharacterEntry>& Table = GetTable();

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
