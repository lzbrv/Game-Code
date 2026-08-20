// =================================================================================================
// TraceStatsDump.cpp — spec v29 §4, "every single stat in the game and its value", as a CSV.
//
// Read TraceStatsDump.h first: it states what this is, why it is a CSV and not a Google Sheet, and
// the one rule the implementation obeys — NOTHING IN THIS FILE IS A TYPED-IN GAME VALUE.
//
// There are exactly three kinds of number in the output and each says which it is in its Source
// column:
//
//   1. A KNOB. Found by walking a UCLASS with TFieldIterator and read out of its config-populated
//      CDO. This file names no knob to list it; it lists whatever is there. Add a knob and it
//      appears, rename one and it appears under the new name, delete one and it leaves.
//
//   2. A NUMBER THAT IS NOT ON A SETTINGS PAGE. The select-card cooldowns (roster / DA_Character_*
//      assets), the cooldown the ability set actually charges (a virtual on the CDO), the
//      engine-owned movement fields (MaxWalkSpeed and friends, which live on
//      UCharacterMovementComponent and are pushed there from WalkSpeed at BeginPlay). Spec §4 says
//      to include these and say where they came from rather than omit them, so each one names its
//      source in the sheet.
//
//   3. A DERIVED NUMBER. RPM from an interval, the effective air caps from base x scale, the
//      knife-profile speed from WalkSpeed x multiplier, shots-to-kill from health / damage. Every
//      one is computed HERE, AT DUMP TIME, from bases fetched by name — never stored. That is the
//      project's standing rule (a value that modifies a base must move when the base moves)
//      applied to the spreadsheet: this sheet cannot go stale relative to the knobs, because it
//      holds no copy of them.
//
//      When a base a derived row needs is missing (somebody renamed it), the cell reads
//      "<MISSING KNOB: Name>" and the run reports it. It does not quietly print zero.
//
// WHY THE SETTINGS PAGES ARE REACHED BY /Script PATH AND NOT BY #include. Same reasoning as
// FKnobSpec::OwnerPath in TraceSettings.cpp: this file must keep compiling while somebody else has
// a settings header open, and a stat dump that can break the build is a stat dump people delete.
// The cost is that a renamed CLASS reports as missing instead of failing to compile — which is why
// FStatsDumpReport::MissingClasses exists and why the summary line goes red on it.
// =================================================================================================

#if !UE_BUILD_SHIPPING

#include "Debug/TraceStatsDump.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

#include "Trace.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Core/TraceCharacterRoster.h"

// Named after the file, per the Windows jumbo-build rule (Scripts/check-jumbo-build-collisions.py).
namespace TraceStatsDump
{
	// =============================================================================================
	// THE SECTIONS — the "group it so a human can read it" half of spec §4
	//
	// A sheet with 700 rows in declaration order is a data dump, not a breakdown. The first column
	// is the grouping, the sort is by these indices, and rows keep their declaration order inside a
	// section (a stable sort), so Movement reads top to bottom the way the header does.
	// =============================================================================================
	namespace Sec
	{
		constexpr int32 About      = 0;
		constexpr int32 Key        = 5;
		constexpr int32 Movement   = 10;
		constexpr int32 Weapons    = 20;
		constexpr int32 Smg        = 21;
		constexpr int32 Recoil     = 22;
		constexpr int32 Tracer     = 23;
		constexpr int32 Damage     = 30;
		constexpr int32 Melee      = 35;
		constexpr int32 Health     = 40;
		constexpr int32 Parry      = 45;
		constexpr int32 Core       = 50;
		constexpr int32 Match      = 55;
		constexpr int32 Characters = 60;
		constexpr int32 AbilityFw  = 61;

		/** 62..71 — one per character, in roster order, so Rocco's block precedes Chut's. */
		constexpr int32 AbilityBase = 62;

		constexpr int32 Bots       = 80;
		constexpr int32 Trail      = 85;
		constexpr int32 Ui         = 88;
		constexpr int32 Audio      = 90;
		constexpr int32 Net        = 92;
		constexpr int32 GameMode   = 94;
		/** 89 so it sits directly under UI and HUD: the crosshair page is what a reader looks for. */
		constexpr int32 Player     = 89;
		constexpr int32 EngineOwned= 98;
		constexpr int32 Other      = 999;
	}

	struct FStatRow
	{
		int32 SectionIndex = Sec::Other;
		FString Section;
		FString Category;
		FString Stat;
		FString DisplayName;
		FString Value;
		FString Unit;
		FString Type;
		FString Source;
		FString Description;
	};

	// =============================================================================================
	// Text helpers
	// =============================================================================================

	/**
	 * One CSV cell. ALWAYS quoted, and the quote doubled inside.
	 *
	 * Unconditional quoting rather than "quote only when it contains a comma" on purpose: the
	 * descriptions here are prose full of commas, semicolons, quotes and the occasional newline, and
	 * the failure mode of getting it wrong is not a crash — it is a spreadsheet where one row's
	 * columns are silently shifted by one and every reader believes it. Trace.VerifyStats re-parses
	 * the file afterwards precisely because that failure is invisible by eye.
	 */
	static FString Cell(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\r\n"), TEXT(" "));
		Out.ReplaceInline(TEXT("\r"), TEXT(" "));
		Out.ReplaceInline(TEXT("\n"), TEXT(" "));
		Out.ReplaceInline(TEXT("\t"), TEXT(" "));
		Out.ReplaceInline(TEXT("\""), TEXT("\"\""));
		return FString::Printf(TEXT("\"%s\""), *Out);
	}

	/** Collapse a doc comment into one readable line, and cap it so a cell stays a cell. */
	static FString Tidy(const FString& In, int32 MaxLength = 420)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\r"), TEXT(" "));
		Out.ReplaceInline(TEXT("\n"), TEXT(" "));
		Out.ReplaceInline(TEXT("\t"), TEXT(" "));

		// Typographic characters the doc comments use freely. Harmless in UTF-8, but a spreadsheet
		// imported on a machine that guesses the encoding wrong turns them into mojibake, and
		// mojibake in the middle of a sentence reads as corruption of the whole file.
		Out.ReplaceInline(TEXT("—"), TEXT("-"));
		Out.ReplaceInline(TEXT("–"), TEXT("-"));
		Out.ReplaceInline(TEXT("‘"), TEXT("'"));
		Out.ReplaceInline(TEXT("’"), TEXT("'"));
		Out.ReplaceInline(TEXT("“"), TEXT("\""));
		Out.ReplaceInline(TEXT("”"), TEXT("\""));
		Out.ReplaceInline(TEXT(" "), TEXT(" "));

		while (Out.Contains(TEXT("  ")))
		{
			Out.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		Out.TrimStartAndEndInline();

		if (Out.Len() > MaxLength)
		{
			FString Clipped = Out.Left(MaxLength);
			int32 LastSpace = INDEX_NONE;
			if (Clipped.FindLastChar(TEXT(' '), LastSpace) && LastSpace > MaxLength / 2)
			{
				Clipped = Clipped.Left(LastSpace);
			}
			Out = Clipped + TEXT(" ...");
		}
		return Out;
	}

	/** %.6g, so 800 prints as "800" and 1.446875 keeps its digits. */
	static FString Number(double Value)
	{
		if (!FMath::IsFinite(Value))
		{
			return TEXT("<not finite>");
		}
		return FString::Printf(TEXT("%.6g"), Value);
	}

	// =============================================================================================
	// Metadata. Guarded on WITH_METADATA, which is what FField actually gates GetMetaData on.
	//
	// The editor target has it; a cooked game target does not. Without it every Description and
	// Category cell would be empty, so the fallbacks below are not decoration: SectionFromName()
	// keeps the grouping working from the property NAME alone, and the sheet says so in its About
	// block rather than looking mysteriously empty.
	// =============================================================================================

	static FString Meta(const FProperty* Prop, const TCHAR* Key)
	{
#if WITH_METADATA
		if (Prop != nullptr && Prop->HasMetaData(Key))
		{
			return Prop->GetMetaData(Key);
		}
#endif
		return FString();
	}

	static bool HasMetadataSupport()
	{
#if WITH_METADATA
		return true;
#else
		return false;
#endif
	}

	// =============================================================================================
	// Units
	//
	// Two sources, in this order:
	//   1. The property's own DisplayName, when it ends in a short parenthetical. This project
	//      writes them already — "Reaction Time (s)", "Aim Error Base (deg)", "Sight Range (uu)",
	//      "Knife Ground Speed (x walk)". That is the AUTHOR's unit and it beats any guess.
	//   2. A name heuristic, ordered most-specific first. It is a heuristic and the sheet does not
	//      pretend otherwise — an unrecognised name gets an empty unit rather than a wrong one.
	// =============================================================================================

	static FString UnitFromDisplayName(const FString& DisplayName)
	{
		if (DisplayName.IsEmpty())
		{
			return FString();
		}

		// Strip the "[v12 §3]"-style spec references this project appends after the unit.
		FString Working = DisplayName;
		int32 BracketAt = INDEX_NONE;
		if (Working.FindChar(TEXT('['), BracketAt))
		{
			Working = Working.Left(BracketAt);
		}
		Working.TrimEndInline();

		if (!Working.EndsWith(TEXT(")")))
		{
			return FString();
		}

		int32 OpenAt = INDEX_NONE;
		if (!Working.FindLastChar(TEXT('('), OpenAt))
		{
			return FString();
		}

		FString Inner = Working.Mid(OpenAt + 1, Working.Len() - OpenAt - 2).TrimStartAndEnd();

		// A parenthetical that contains a bracket of its own is a SENTENCE, not a unit -
		// "(fraction; velocity is sqrt(1+this))" grabbed "1+this)" before this line existed.
		if (Inner.Contains(TEXT("(")) || Inner.Contains(TEXT(")")))
		{
			return FString();
		}

		// "(uu/s, applied to Roxie)" and "(s, 0 = off)" lead with the unit and then explain it.
		int32 BreakAt = INDEX_NONE;
		if (Inner.FindChar(TEXT(','), BreakAt) || Inner.FindChar(TEXT(';'), BreakAt))
		{
			Inner = Inner.Left(BreakAt).TrimStartAndEnd();
		}

		// A spec reference, not a unit: "(v9 §7, x1.10)", "(v26 §3)".
		if (Inner.Contains(TEXT("§")) || (Inner.Len() >= 2 && Inner[0] == TEXT('v') && FChar::IsDigit(Inner[1])))
		{
			return FString();
		}

		// 10 characters is the line between a unit and a parenthetical remark.
		if (Inner.Len() == 0 || Inner.Len() > 10)
		{
			return FString();
		}

		// THE FIRST WORD MUST ITSELF BE A UNIT.
		//
		// This is the rule that separates "(x walk)", "(uu/s per s)" and "(fraction)" - which are
		// units - from "(lateral, per s)", "(ABSOLUTE, not a fraction ...)", "(no ground)" and
		// "(A/B test)", which are the author emphasising something and which produced units reading
		// "lateral" and "ABSOLUTE" until this check existed. A rejected parenthetical falls through
		// to the name heuristic, which gets both of those right.
		//
		// Anything containing a digit, a slash or a hyphen is taken as a unit unconditionally:
		// "0-1", "uu/s", "deg/s", "m/s^2" are all self-evidently dimensional.
		static const TCHAR* UnitWords[] = { TEXT("s"), TEXT("ms"), TEXT("uu"), TEXT("deg"), TEXT("HP"),
			TEXT("Hz"), TEXT("px"), TEXT("x"), TEXT("%"), TEXT("fraction"), TEXT("rounds"),
			TEXT("count"), TEXT("points"), TEXT("shots") };

		int32 SpaceAt = INDEX_NONE;
		const FString FirstWord = Inner.FindChar(TEXT(' '), SpaceAt) ? Inner.Left(SpaceAt) : Inner;

		// A hyphen or a digit is dimensional on sight - "0-1", "-1 to 1".
		bool bFirstWordIsAUnit = FirstWord.Contains(TEXT("-"));
		for (int32 CharIndex = 0; !bFirstWordIsAUnit && CharIndex < FirstWord.Len(); ++CharIndex)
		{
			bFirstWordIsAUnit = FChar::IsDigit(FirstWord[CharIndex]);
		}

		// A slash is a RATIO, and a ratio is a unit only when its numerator is one: "uu/s" and
		// "deg/s" qualify, "(A/B test)" on the scoring-mode knob does not.
		int32 SlashAt = INDEX_NONE;
		const FString Numerator = FirstWord.FindChar(TEXT('/'), SlashAt) ? FirstWord.Left(SlashAt) : FirstWord;

		for (const TCHAR* Word : UnitWords)
		{
			if (bFirstWordIsAUnit)
			{
				break;
			}
			bFirstWordIsAUnit = Numerator.Equals(Word, ESearchCase::IgnoreCase);
		}

		return bFirstWordIsAUnit ? Inner : FString();
	}

	static FString UnitFromName(const FString& StatName, const FProperty* Prop)
	{
		if (CastField<FBoolProperty>(Prop) != nullptr)
		{
			return TEXT("on/off");
		}

		const FString Name = StatName;
		auto Has = [&Name](const TCHAR* Needle) { return Name.Contains(Needle, ESearchCase::IgnoreCase); };

		// Most specific first. Order is the whole correctness of this function: "SlideJumpWindow-
		// SpeedBonus" contains both "Speed" and "Bonus" and is a MULTIPLIER, so multipliers are
		// tested before speeds.
		if (Has(TEXT("RPM")))                                                    return TEXT("rounds/min");
		// Screen-space UI numbers are pixels, not world units, and calling them "uu" would be a
		// wrong unit rather than a missing one. Colour is excluded on purpose - it falls through.
		if (Has(TEXT("Crosshair")) && (Has(TEXT("Size")) || Has(TEXT("Thickness"))
			|| Has(TEXT("Gap")) || Has(TEXT("Length")) || Has(TEXT("Outline"))))  return TEXT("px");
		if (Has(TEXT("Volume")))                                                 return TEXT("0-1");
		if (Has(TEXT("Sensitivity")))                                            return TEXT("x (multiplier)");
		if (Has(TEXT("FieldOfView")))                                            return TEXT("deg");
		if (Has(TEXT("NormalZ")))                                                return TEXT("cos, -1 to 1");
		if (Has(TEXT("Bounce")) || Has(TEXT("Restitution")))                     return TEXT("0-1");
		if (Has(TEXT("Fraction")) || Has(TEXT("Opacity")) || Has(TEXT("Chance"))
			|| Has(TEXT("Alpha")) || Has(TEXT("Aggression")))                    return TEXT("0-1");
		if (Has(TEXT("Multiplier")) || Has(TEXT("Scale")) || Has(TEXT("Bonus"))
			|| Has(TEXT("Retention")) || Has(TEXT("Bias")) || Has(TEXT("Exponent"))
			|| Has(TEXT("Strength")) || Has(TEXT("Gain")))                       return TEXT("x (multiplier)");
		if (Has(TEXT("Degrees")) || Has(TEXT("Angle")) || Has(TEXT("Cone"))
			|| Has(TEXT("Fov")) || Has(TEXT("Pitch")) || Has(TEXT("Yaw")))       return TEXT("deg");
		if (Has(TEXT("Seconds")) || Has(TEXT("Duration")) || Has(TEXT("Cooldown"))
			|| Has(TEXT("Delay")) || Has(TEXT("Interval")) || Has(TEXT("Window"))
			|| Has(TEXT("Lifetime")) || Has(TEXT("Lockout")) || Has(TEXT("Grace"))
			|| Has(TEXT("Linger")) || Has(TEXT("Timeout")))                      return TEXT("s");
		// Friction is deliberately absent: in UE it is a dimensionless coefficient, not an
		// acceleration, and labelling it uu/s^2 would be a WRONG unit rather than a missing one.
		if (Has(TEXT("Acceleration")) || Has(TEXT("Deceleration")))              return TEXT("uu/s^2");
		if (Has(TEXT("Speed")) || Has(TEXT("Velocity")) || Has(TEXT("Impulse"))
			|| Has(TEXT("Launch")) || Has(TEXT("Knockback")))                    return TEXT("uu/s");
		if (Has(TEXT("UU")) || Has(TEXT("Radius")) || Has(TEXT("Range"))
			|| Has(TEXT("Distance")) || Has(TEXT("Height")) || Has(TEXT("Width"))
			|| Has(TEXT("Length")) || Has(TEXT("Spacing")) || Has(TEXT("Reach")))return TEXT("uu");
		if (Has(TEXT("Damage")) || Has(TEXT("Health")))                          return TEXT("HP");
		if (Has(TEXT("Index")))                                                  return TEXT("index");
		if (Has(TEXT("Color")) || Has(TEXT("Colour")))                           return TEXT("RGBA 0-1");

		if (CastField<FIntProperty>(Prop) != nullptr)
		{
			if (Has(TEXT("Count")) || Has(TEXT("Max")) || Has(TEXT("Min"))
				|| Has(TEXT("Num")) || Has(TEXT("Size")) || Has(TEXT("Charges"))
				|| Has(TEXT("Lead")) || Has(TEXT("Score")) || Has(TEXT("PerTeam"))
				|| Has(TEXT("PerMatch")) || Has(TEXT("Samples"))
				|| Has(TEXT("Boosts")))                                          return TEXT("count");
		}
		return FString();
	}

	static FString UnitFor(const FString& StatName, const FString& DisplayName, const FProperty* Prop)
	{
		// Two types answer for themselves before the DisplayName is consulted at all, because
		// whatever their author wrote in brackets it is not a unit:
		//   * a bool is on/off, always;
		//   * an ENUM's value is an enumerator NAME, not a quantity, so it has no unit. ScoringMode
		//     is written "Scoring Mode (A/B test)" and briefly reported its unit as "A/B test".
		if (CastField<FBoolProperty>(Prop) != nullptr)
		{
			return TEXT("on/off");
		}
		const bool bIsEnum = (CastField<FEnumProperty>(Prop) != nullptr)
			|| (CastField<FByteProperty>(Prop) != nullptr && CastField<FByteProperty>(Prop)->Enum != nullptr);
		if (bIsEnum)
		{
			return FString();
		}

		const FString FromAuthor = UnitFromDisplayName(DisplayName);
		return FromAuthor.IsEmpty() ? UnitFromName(StatName, Prop) : FromAuthor;
	}

	// =============================================================================================
	// Sections
	// =============================================================================================

	/** The roster index (0-based, Rocco = 0) of a character NAME, or INDEX_NONE. */
	static int32 RosterIndexForName(const FString& CharacterName)
	{
		const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Table = TraceCharacterRoster::All();
		for (int32 Index = 0; Index < Table.Num(); ++Index)
		{
			if (CharacterName.Equals(Table[Index].Name, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/**
	 * Map a UPROPERTY Category ("Movement|Slide", "Abilities|Roxie", "Combat|SMG") onto one of the
	 * human sections. Prefix matching, most specific first.
	 */
	static void SectionFromCategory(const FString& Category, int32& OutIndex, FString& OutName)
	{
		auto Is = [&Category](const TCHAR* Prefix) { return Category.StartsWith(Prefix, ESearchCase::IgnoreCase); };

		if (Is(TEXT("Abilities|Framework"))) { OutIndex = Sec::AbilityFw; OutName = TEXT("Abilities - Framework"); return; }
		if (Is(TEXT("Abilities|")))
		{
			const FString Who = Category.RightChop(FCString::Strlen(TEXT("Abilities|"))).TrimStartAndEnd();
			const int32 RosterIndex = RosterIndexForName(Who);
			OutIndex = (RosterIndex != INDEX_NONE) ? (Sec::AbilityBase + RosterIndex) : Sec::AbilityFw;
			OutName = FString::Printf(TEXT("Abilities - %s"), *Who);
			return;
		}
		if (Is(TEXT("Abilities")))    { OutIndex = Sec::AbilityFw;  OutName = TEXT("Abilities - Framework"); return; }
		if (Is(TEXT("Movement")))     { OutIndex = Sec::Movement;   OutName = TEXT("Movement"); return; }
		if (Is(TEXT("Combat|SMG")))   { OutIndex = Sec::Smg;        OutName = TEXT("Weapons - SMG"); return; }
		if (Is(TEXT("Combat|Recoil"))){ OutIndex = Sec::Recoil;     OutName = TEXT("Weapons - Recoil"); return; }
		if (Is(TEXT("Combat")))       { OutIndex = Sec::Weapons;    OutName = TEXT("Weapons - Pistol and shared"); return; }
		if (Is(TEXT("Weapon")))       { OutIndex = Sec::Weapons;    OutName = TEXT("Weapons - Pistol and shared"); return; }
		if (Is(TEXT("Tracer")))       { OutIndex = Sec::Tracer;     OutName = TEXT("Weapons - Tracer and bullet VFX"); return; }
		if (Is(TEXT("Damage")) || Is(TEXT("Zone")))
		                              { OutIndex = Sec::Damage;     OutName = TEXT("Damage and hit zones"); return; }
		if (Is(TEXT("Melee")) || Is(TEXT("Knife")))
		                              { OutIndex = Sec::Melee;      OutName = TEXT("Melee (Knife)"); return; }
		if (Is(TEXT("Health")) || Is(TEXT("Regen")))
		                              { OutIndex = Sec::Health;     OutName = TEXT("Health and regeneration"); return; }
		if (Is(TEXT("Parry")))        { OutIndex = Sec::Parry;      OutName = TEXT("Parry"); return; }
		if (Is(TEXT("Core")))         { OutIndex = Sec::Core;       OutName = TEXT("Core (Mode B)"); return; }
		if (Is(TEXT("Match")))        { OutIndex = Sec::Match;      OutName = TEXT("Match rules"); return; }
		// FTraceBotProfile's members carry their own categories - Reaction, Aim, Burst, Tempo,
		// Engagement - and every one of them is a BOT number. Without these five lines the bot
		// profiles scatter into "Other" and the reader has to know that to find them.
		if (Is(TEXT("Bots")) || Is(TEXT("Reaction")) || Is(TEXT("Aim"))
			|| Is(TEXT("Burst")) || Is(TEXT("Tempo")) || Is(TEXT("Engagement")))
		                              { OutIndex = Sec::Bots;       OutName = TEXT("Bots and AI"); return; }
		if (Is(TEXT("Trail")))        { OutIndex = Sec::Trail;      OutName = TEXT("Trail and VFX"); return; }
		if (Is(TEXT("HUD")) || Is(TEXT("UI")) || Is(TEXT("Crosshair")) || Is(TEXT("Menu")))
		                              { OutIndex = Sec::Ui;         OutName = TEXT("UI and HUD"); return; }
		if (Is(TEXT("Audio")) || Is(TEXT("Sound")))
		                              { OutIndex = Sec::Audio;      OutName = TEXT("Audio"); return; }
		if (Is(TEXT("Net")))          { OutIndex = Sec::Net;        OutName = TEXT("Networking"); return; }

		OutIndex = Sec::Other;
		OutName = TEXT("Other / uncategorised");
	}

	/**
	 * Last-resort grouping from the property NAME, for a build with no metadata (WITH_METADATA off)
	 * where every Category string is empty. Deliberately crude: it exists so the sheet still groups
	 * rather than becoming 700 rows of "Other".
	 */
	static void SectionFromName(const FString& StatName, int32& OutIndex, FString& OutName)
	{
		auto Has = [&StatName](const TCHAR* Needle) { return StatName.Contains(Needle, ESearchCase::IgnoreCase); };

		if (Has(TEXT("Smg")))                                { OutIndex = Sec::Smg;      OutName = TEXT("Weapons - SMG"); return; }
		if (Has(TEXT("Recoil")))                             { OutIndex = Sec::Recoil;   OutName = TEXT("Weapons - Recoil"); return; }
		if (Has(TEXT("Tracer")))                             { OutIndex = Sec::Tracer;   OutName = TEXT("Weapons - Tracer and bullet VFX"); return; }
		if (Has(TEXT("Bot")))                                { OutIndex = Sec::Bots;     OutName = TEXT("Bots and AI"); return; }
		if (Has(TEXT("Core")) || Has(TEXT("Goal")))          { OutIndex = Sec::Core;     OutName = TEXT("Core (Mode B)"); return; }
		if (Has(TEXT("Trail")) || Has(TEXT("Ghost")))        { OutIndex = Sec::Trail;    OutName = TEXT("Trail and VFX"); return; }
		if (Has(TEXT("Parry")))                              { OutIndex = Sec::Parry;    OutName = TEXT("Parry"); return; }
		if (Has(TEXT("Damage")) || Has(TEXT("Head")) || Has(TEXT("Hip")))
		                                                     { OutIndex = Sec::Damage;   OutName = TEXT("Damage and hit zones"); return; }
		if (Has(TEXT("Knife")) || Has(TEXT("Melee")) || Has(TEXT("Stab")))
		                                                     { OutIndex = Sec::Melee;    OutName = TEXT("Melee (Knife)"); return; }
		if (Has(TEXT("Slide")) || Has(TEXT("Dash")) || Has(TEXT("Jump"))
			|| Has(TEXT("Walk")) || Has(TEXT("Air")) || Has(TEXT("Wall")))
		                                                     { OutIndex = Sec::Movement; OutName = TEXT("Movement"); return; }
		OutIndex = Sec::Other;
		OutName = TEXT("Other / uncategorised");
	}

	/**
	 * A readable Category for a property whose author gave it none.
	 *
	 * The per-player pages (UTraceUserSettings, UTraceGameUserSettings) are written as plain
	 * `UPROPERTY(config)` with no editor Category, because they are never shown in Project Settings —
	 * so every crosshair, mouse and keybind row came out of the walk with an EMPTY Category cell.
	 * That is the one column a reader groups a spreadsheet by, and a blank there makes the whole
	 * per-player block unsortable. This names the group from the property name instead.
	 *
	 * It is only ever consulted for a blank; an author-written Category always wins, and the SECTION
	 * is decided before this runs so nothing here can move a row into a different section.
	 */
	static FString CategoryFromName(const FString& StatName)
	{
		auto Has = [&StatName](const TCHAR* Needle) { return StatName.Contains(Needle, ESearchCase::IgnoreCase); };

		if (Has(TEXT("Crosshair")))                                { return TEXT("Crosshair"); }
		if (Has(TEXT("Mouse")) || Has(TEXT("Sensitivity")) || Has(TEXT("Invert")))
		                                                           { return TEXT("Mouse and look"); }
		if (Has(TEXT("KeyBinding")) || Has(TEXT("Bind")))          { return TEXT("Keybinds"); }
		if (Has(TEXT("FieldOfView")) || Has(TEXT("Fov")) || Has(TEXT("Resolution"))
			|| Has(TEXT("Screen")) || Has(TEXT("Vsync")) || Has(TEXT("Detect")))
		                                                           { return TEXT("Video"); }
		if (Has(TEXT("Class")) || Has(TEXT("Builder")))            { return TEXT("Spawned classes"); }
		return TEXT("Uncategorised");
	}

	// =============================================================================================
	// Reading one property
	// =============================================================================================

	/** Structs from this module get expanded member by member; engine math types are exported whole. */
	static bool ShouldExpandStruct(const FStructProperty* AsStruct)
	{
		if (AsStruct == nullptr || AsStruct->Struct == nullptr)
		{
			return false;
		}
		// "/Script/Trace" only. FLinearColor and FVector have UPROPERTYs too, and expanding them
		// would turn one readable "(R=1.0,G=0.55,...)" cell into four rows of noise; FTraceBotProfile
		// is the opposite case - one cell would be a 40-field blob nobody can read.
		const UPackage* Pkg = AsStruct->Struct->GetOutermost();
		return (Pkg != nullptr) && Pkg->GetName().Equals(TEXT("/Script/Trace"));
	}

	static FString ValueOf(const FProperty* Prop, const void* Container, int32 ArrayIndex = 0)
	{
		if (Prop == nullptr || Container == nullptr)
		{
			return TEXT("<unreadable>");
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container, ArrayIndex);

		if (const FBoolProperty* AsBool = CastField<FBoolProperty>(Prop))
		{
			return AsBool->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
		}

		// Enums before numerics: an `enum class : uint8` is an FEnumProperty, and a UENUM with no
		// explicit underlying type lands as an FByteProperty WITH an Enum - both must print the
		// enumerator NAME, because that is what DefaultGame.ini round-trips.
		const bool bIsEnum = (CastField<FEnumProperty>(Prop) != nullptr)
			|| (CastField<FByteProperty>(Prop) != nullptr && CastField<FByteProperty>(Prop)->Enum != nullptr);

		if (!bIsEnum)
		{
			if (const FNumericProperty* AsNumeric = CastField<FNumericProperty>(Prop))
			{
				if (AsNumeric->IsFloatingPoint())
				{
					return Number(AsNumeric->GetFloatingPointPropertyValue(ValuePtr));
				}
				return FString::Printf(TEXT("%lld"), AsNumeric->GetSignedIntPropertyValue(ValuePtr));
			}
		}

		FString Exported;
		Prop->ExportText_InContainer(ArrayIndex, Exported, Container, Container, nullptr, PPF_None);
		if (Exported.Len() > 300)
		{
			Exported = Exported.Left(300) + TEXT(" ...");
		}
		return Exported;
	}

	/** "Range 0-3." appended to a description, when the author clamped the knob. */
	static FString RangeSuffix(const FProperty* Prop)
	{
		const FString ClampMin = Meta(Prop, TEXT("ClampMin"));
		const FString ClampMax = Meta(Prop, TEXT("ClampMax"));
		if (!ClampMin.IsEmpty() || !ClampMax.IsEmpty())
		{
			return FString::Printf(TEXT(" [allowed range: %s to %s]"),
				ClampMin.IsEmpty() ? TEXT("-") : *ClampMin,
				ClampMax.IsEmpty() ? TEXT("-") : *ClampMax);
		}
		return FString();
	}

	// =============================================================================================
	// Walking one settings page
	// =============================================================================================

	struct FSettingsPage
	{
		/** /Script path, so this file includes no settings header. See the file header. */
		const TCHAR* ClassPath;

		/** What goes in the Source column, i.e. "where do I change this". */
		const TCHAR* SourceLabel;

		/**
		 * Forced section for every row on this page, or Sec::Other to derive it from each
		 * property's Category. UTraceSettings is the only page that spans sections.
		 */
		int32 ForcedSection;
		const TCHAR* ForcedSectionName;
	};

	/** Resolve the object whose values should be read for a class. */
	static const UObject* ContainerFor(const UClass* InClass)
	{
		if (InClass == nullptr)
		{
			return nullptr;
		}

		// UGameUserSettings has a live singleton that is NOT the CDO - reading the CDO there would
		// print the shipped defaults while the player is running something else, i.e. a plausible
		// wrong number, which is the one output this file must never produce.
		if (InClass->IsChildOf(UGameUserSettings::StaticClass()) && GEngine != nullptr)
		{
			if (UGameUserSettings* LiveSettings = GEngine->GetGameUserSettings())
			{
				if (LiveSettings->IsA(InClass))
				{
					return LiveSettings;
				}
			}
		}
		return InClass->GetDefaultObject();
	}

	static void AddPropertyRow(TArray<FStatRow>& Rows, const FProperty* Prop, const void* Container,
		const FString& NamePrefix, const FString& SourceLabel, int32 ForcedSection,
		const FString& ForcedSectionName, const FString& CategoryOverride, int32 ArrayIndex)
	{
		FStatRow Row;

		Row.Stat = NamePrefix + Prop->GetName();
		if (Prop->ArrayDim > 1)
		{
			Row.Stat += FString::Printf(TEXT("[%d]"), ArrayIndex);
		}

		Row.DisplayName = Meta(Prop, TEXT("DisplayName"));
		Row.Category    = CategoryOverride.IsEmpty() ? Meta(Prop, TEXT("Category")) : CategoryOverride;
		Row.Value       = ValueOf(Prop, Container, ArrayIndex);
		Row.Type        = Prop->GetCPPType();
		Row.Unit        = UnitFor(Row.Stat, Row.DisplayName, Prop);

		const bool bConfig = Prop->HasAnyPropertyFlags(CPF_Config);
		Row.Source = bConfig
			? SourceLabel
			: FString::Printf(TEXT("%s -- NOT `config`: DefaultGame.ini cannot reach this one"), *SourceLabel);

		Row.Description = Tidy(Meta(Prop, TEXT("ToolTip"))) + RangeSuffix(Prop);
		Row.Description.TrimStartAndEndInline();

		if (ForcedSection != Sec::Other)
		{
			Row.SectionIndex = ForcedSection;
			Row.Section = ForcedSectionName;
		}
		else if (!Row.Category.IsEmpty())
		{
			SectionFromCategory(Row.Category, Row.SectionIndex, Row.Section);
		}
		else
		{
			SectionFromName(Row.Stat, Row.SectionIndex, Row.Section);
		}

		// AFTER the section is settled, never before - see CategoryFromName.
		if (Row.Category.IsEmpty())
		{
			Row.Category = CategoryFromName(Row.Stat);
		}

		Rows.Add(MoveTemp(Row));
	}

	/**
	 * Emit every UPROPERTY on one class, expanding this module's own structs one level.
	 * @return the number of rows added.
	 */
	static int32 WalkClass(TArray<FStatRow>& Rows, const UClass* InClass, const UObject* Container,
		const FSettingsPage& Page, int32& OutPropertyCount, int32& OutConfigCount)
	{
		const int32 RowsBefore = Rows.Num();
		OutPropertyCount = 0;
		OutConfigCount = 0;

		const FString SourceLabel(Page.SourceLabel);
		const FString ForcedName(Page.ForcedSectionName != nullptr ? Page.ForcedSectionName : TEXT(""));

		for (TFieldIterator<FProperty> PropIt(InClass, EFieldIterationFlags::IncludeDeprecated); PropIt; ++PropIt)
		{
			const FProperty* Prop = *PropIt;
			if (Prop == nullptr)
			{
				continue;
			}
			++OutPropertyCount;
			if (Prop->HasAnyPropertyFlags(CPF_Config))
			{
				++OutConfigCount;
			}

			const FStructProperty* AsStruct = CastField<FStructProperty>(Prop);
			if (ShouldExpandStruct(AsStruct))
			{
				// One level only. Two would put FLinearColor's channels in the sheet as rows.
				const FString Prefix = Prop->GetName() + TEXT(".");
				const FString OuterCategory = Meta(Prop, TEXT("Category"));
				const void* StructPtr = Prop->ContainerPtrToValuePtr<void>(Container);

				for (TFieldIterator<FProperty> MemberIt(AsStruct->Struct, EFieldIterationFlags::IncludeDeprecated);
					MemberIt; ++MemberIt)
				{
					const FProperty* Member = *MemberIt;
					if (Member == nullptr)
					{
						continue;
					}
					for (int32 Index = 0; Index < FMath::Max(1, Member->ArrayDim); ++Index)
					{
						AddPropertyRow(Rows, Member, StructPtr, Prefix, SourceLabel,
							Page.ForcedSection, ForcedName, OuterCategory, Index);
					}
				}
				continue;
			}

			for (int32 Index = 0; Index < FMath::Max(1, Prop->ArrayDim); ++Index)
			{
				AddPropertyRow(Rows, Prop, Container, FString(), SourceLabel,
					Page.ForcedSection, ForcedName, FString(), Index);
			}
		}

		return Rows.Num() - RowsBefore;
	}

	// =============================================================================================
	// Reading one knob by name — the only way a DERIVED row gets a base
	// =============================================================================================

	struct FKnobSource
	{
		const UClass* OwnerClass = nullptr;
		const UObject* Container = nullptr;
	};

	/**
	 * @return true when @p Name exists on @p Src as a number. False records the miss in @p Report so
	 *         a renamed base is reported rather than silently becoming a zero in the sheet.
	 */
	static bool ReadNumber(const FKnobSource& Src, const TCHAR* Name, double& OutValue,
		FStatsDumpReport& Report)
	{
		OutValue = 0.0;
		if (Src.OwnerClass == nullptr || Src.Container == nullptr)
		{
			Report.MissingKnobs.AddUnique(FString::Printf(TEXT("%s (owning class absent)"), Name));
			return false;
		}
		const FProperty* Prop = Src.OwnerClass->FindPropertyByName(FName(Name));
		const FNumericProperty* AsNumeric = CastField<FNumericProperty>(Prop);
		if (AsNumeric == nullptr)
		{
			Report.MissingKnobs.AddUnique(FString::Printf(TEXT("%s (on %s)"), Name, *GetNameSafe(Src.OwnerClass)));
			return false;
		}
		const void* ValuePtr = AsNumeric->ContainerPtrToValuePtr<void>(Src.Container);
		OutValue = AsNumeric->IsFloatingPoint()
			? AsNumeric->GetFloatingPointPropertyValue(ValuePtr)
			: static_cast<double>(AsNumeric->GetSignedIntPropertyValue(ValuePtr));
		return true;
	}

	static bool ReadBool(const FKnobSource& Src, const TCHAR* Name, bool& OutValue,
		FStatsDumpReport& Report)
	{
		OutValue = false;
		const FProperty* Prop = (Src.OwnerClass != nullptr) ? Src.OwnerClass->FindPropertyByName(FName(Name)) : nullptr;
		const FBoolProperty* AsBool = CastField<FBoolProperty>(Prop);
		if (AsBool == nullptr || Src.Container == nullptr)
		{
			Report.MissingKnobs.AddUnique(FString::Printf(TEXT("%s (bool)"), Name));
			return false;
		}
		OutValue = AsBool->GetPropertyValue_InContainer(Src.Container);
		return true;
	}

	// =============================================================================================
	// Row builders for the non-knob material
	// =============================================================================================

	static void AddRow(TArray<FStatRow>& Rows, int32 SectionIndex, const TCHAR* SectionName,
		const FString& Category, const FString& Stat, const FString& Value, const TCHAR* Unit,
		const TCHAR* Type, const FString& Source, const FString& Description)
	{
		FStatRow Row;
		Row.SectionIndex = SectionIndex;
		Row.Section = SectionName;
		Row.Category = Category;
		Row.Stat = Stat;
		Row.Value = Value;
		Row.Unit = Unit;
		Row.Type = Type;
		Row.Source = Source;
		Row.Description = Description;
		Rows.Add(MoveTemp(Row));
	}

	/** A number this file worked out from other numbers. The formula goes in the Source column. */
	static void AddDerived(TArray<FStatRow>& Rows, const FString& Stat, bool bHaveInputs, double Value,
		const TCHAR* Unit, const FString& Formula, const FString& Description)
	{
		AddRow(Rows, Sec::Key, TEXT("Key numbers (computed live)"), TEXT("Derived"), Stat,
			bHaveInputs ? Number(Value) : FString::Printf(TEXT("<MISSING KNOB - see %s>"), *Formula),
			Unit, TEXT("derived"),
			FString::Printf(TEXT("DERIVED at dump time: %s"), *Formula),
			Description);
	}

	// =============================================================================================
	// The build
	// =============================================================================================

	static void BuildAboutSection(TArray<FStatRow>& Rows, const FString& CsvPath)
	{
		const TCHAR* SectionName = TEXT("About this sheet");
		const FString Cat = TEXT("Provenance");

		AddRow(Rows, Sec::About, SectionName, Cat, TEXT("Generated at (UTC)"),
			FDateTime::UtcNow().ToString(), TEXT(""), TEXT("text"),
			TEXT("Trace.DumpStats"),
			TEXT("This file is GENERATED FROM THE RUNNING GAME. Do not hand-edit it - re-run Trace.DumpStats instead, or your edit is gone at the next dump."));

		AddRow(Rows, Sec::About, SectionName, Cat, TEXT("Generated by"),
			TEXT("Trace.DumpStats (Source/Trace/Debug/TraceStatsDump.cpp)"), TEXT(""), TEXT("text"),
			TEXT("Trace.DumpStats"),
			TEXT("Console command, dev builds only. `Trace.DumpStats <path>` writes elsewhere; `Trace.VerifyStats` re-parses the written file column by column."));

		// PROJECT-RELATIVE, not the absolute path. docs/TraceStats.csv is committed and read on
		// GitHub, and "/Users/<whoever>/trace/docs/..." in a committed file is both noise and a
		// small leak of whose machine happened to run the dump.
		FString WrittenTo = CsvPath;
		FPaths::MakePathRelativeTo(WrittenTo, *FPaths::ProjectDir());
		if (WrittenTo.StartsWith(TEXT("..")))
		{
			WrittenTo = CsvPath;   // outside the project - an absolute path is the honest answer
		}

		AddRow(Rows, Sec::About, SectionName, Cat, TEXT("Written to"),
			WrittenTo, TEXT(""), TEXT("text"), TEXT("Trace.DumpStats"),
			TEXT("Saved/Stats/TraceStats.csv by default. docs/TraceStats.csv is the committed copy, produced by the same command with an explicit path."));

		AddRow(Rows, Sec::About, SectionName, Cat, TEXT("How to open this in Google Sheets"),
			TEXT("File > Import > Upload, then Replace spreadsheet"), TEXT(""), TEXT("text"),
			TEXT("n/a"),
			TEXT("We cannot create a Google Sheet from the build machine - there is no authenticated Drive access - so this is the one-click import instead. GitHub also renders it as a sortable table with no download."));

		AddRow(Rows, Sec::About, SectionName, Cat, TEXT("Where these values come from"),
			TEXT("the live CDOs, after Config/DefaultGame.ini has been applied"), TEXT(""), TEXT("text"),
			TEXT("n/a"),
			TEXT("NOT the C++ header defaults. Config/DefaultGame.ini beats a header default in this project, so a value read from a header can be wrong; every value here is read back out of the object the game plays from."));

		AddRow(Rows, Sec::About, SectionName, Cat, TEXT("Descriptions and categories available"),
			HasMetadataSupport() ? TEXT("yes") : TEXT("no - metadata is compiled out of this target"),
			TEXT(""), TEXT("text"), TEXT("WITH_METADATA"),
			TEXT("UPROPERTY doc comments and Category strings are editor-only metadata. In a cooked target they do not exist, so the Description and Category columns come out empty and the grouping falls back to a name heuristic."));

		AddRow(Rows, Sec::About, SectionName, Cat, TEXT("Roster source"),
			TraceCharacterRoster::CurrentSourceName(), TEXT(""), TEXT("text"),
			TEXT("TraceCharacterRoster::CurrentSource()"),
			TEXT("Which table the character cards came from - the DA_Character_* assets, or the C++ fallback table in Core/TraceCharacterRoster.cpp. It is all of them or none, by design."));

		AddRow(Rows, Sec::About, SectionName, Cat, TEXT("Reading the Source column"),
			TEXT("knob = editable in Project Settings and DefaultGame.ini; anything else says where it lives"),
			TEXT(""), TEXT("text"), TEXT("n/a"),
			TEXT("Rows marked DERIVED are computed at dump time from the knobs above them and are stored nowhere - change the base and the derived number moves with it."));
	}

	static void BuildCharacterSections(TArray<FStatRow>& Rows, FStatsDumpReport& Report)
	{
		const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Table = TraceCharacterRoster::All();
		const FString RosterSource = FString::Printf(TEXT("roster card (%s) -- NOT a knob"),
			TraceCharacterRoster::CurrentSourceName());

		for (int32 Index = 0; Index < Table.Num(); ++Index)
		{
			const TraceCharacterRoster::FTraceCharacterEntry& Entry = Table[Index];
			const FString Who(Entry.Name);
			const FString Cat = FString::Printf(TEXT("Characters|%s"), *Who);

			UClass* SetClass = UTraceCharacterAbilitySet::FindClassFor(static_cast<ETraceCharacterId>(Entry.Id));
			const UTraceCharacterAbilitySet* SetCdo = (SetClass != nullptr)
				? Cast<UTraceCharacterAbilitySet>(SetClass->GetDefaultObject())
				: nullptr;

			AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
				FString::Printf(TEXT("%s.ActivatedAbilityName"), *Who), FString(Entry.ActivatedName),
				TEXT(""), TEXT("text"), RosterSource,
				TEXT("The name the select card and the HUD print for the E ability."));

			AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
				FString::Printf(TEXT("%s.ActivatedCooldown_PRINTED"), *Who), Number(Entry.ActivatedCooldown),
				TEXT("s"), TEXT("float"), RosterSource,
				TEXT("DISPLAY ONLY. What the select screen prints and what the HUD cooldown ring uses as its denominator. It enforces nothing."));

			if (SetCdo != nullptr)
			{
				const float Enforced = SetCdo->GetActivatedCooldownSeconds();
				AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
					FString::Printf(TEXT("%s.ActivatedCooldown_ENFORCED"), *Who), Number(Enforced),
					TEXT("s"), TEXT("float"),
					FString::Printf(TEXT("%s::GetActivatedCooldownSeconds() on the CDO -- NOT a knob, it READS one"), *SetClass->GetName()),
					TEXT("What the ability framework actually charges when E is pressed. This is the number that matters in a match; the printed one above is only what the card says."));

				const bool bAgree = FMath::IsNearlyEqual(Enforced, Entry.ActivatedCooldown, 0.01f);
				AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
					FString::Printf(TEXT("%s.CooldownCardMatchesEnforced"), *Who),
					bAgree ? FString(TEXT("yes")) : FString::Printf(TEXT("NO - card %s s, game charges %s s"),
						*Number(Entry.ActivatedCooldown), *Number(Enforced)),
					TEXT(""), TEXT("bool"), TEXT("comparison of the two rows above"),
					TEXT("Trace.VerifyCharacterData fails the build's own check when these disagree. It is in the sheet so a designer sees the disagreement without running a command."));

				AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
					FString::Printf(TEXT("%s.AbilitySetClass"), *Who), SetClass->GetName(),
					TEXT(""), TEXT("text"), TEXT("UTraceCharacterAbilitySet::FindClassFor (reflection)"),
					TEXT("The C++ class that implements this character. Its tuning lives in the Abilities section for this character."));
			}
			else
			{
				AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
					FString::Printf(TEXT("%s.AbilitySetClass"), *Who), TEXT("<not implemented>"),
					TEXT(""), TEXT("text"), TEXT("UTraceCharacterAbilitySet::FindClassFor (reflection)"),
					TEXT("No UTraceCharacterAbilitySet subclass claims this character id, so it plays as a Mannequin with a name and its E does nothing."));
				Report.MissingClasses.AddUnique(FString::Printf(TEXT("ability set for %s"), *Who));
			}

			AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
				FString::Printf(TEXT("%s.MovementAbility"), *Who), Tidy(Entry.Movement),
				TEXT(""), TEXT("text"), RosterSource, TEXT("Card text for the movement slot. No cooldown."));

			AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
				FString::Printf(TEXT("%s.PassiveAbility"), *Who), Tidy(Entry.Passive),
				TEXT(""), TEXT("text"), RosterSource, TEXT("Card text for the passive slot. Always on, no cooldown."));

			AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
				FString::Printf(TEXT("%s.ActivatedAbility"), *Who), Tidy(Entry.Activated),
				TEXT(""), TEXT("text"), RosterSource, TEXT("Card text for the E ability."));

			AddRow(Rows, Sec::Characters, TEXT("Characters - cards and enforced cooldowns"), Cat,
				FString::Printf(TEXT("%s.AccentColor"), *Who),
				FString::Printf(TEXT("(R=%.3f,G=%.3f,B=%.3f,A=%.3f)"),
					Entry.Accent.R, Entry.Accent.G, Entry.Accent.B, Entry.Accent.A),
				TEXT("RGBA 0-1"), TEXT("FLinearColor"), RosterSource,
				TEXT("Card accent and the colour of the HUD ability row. Deliberately not a team colour."));
		}
	}

	static void BuildEngineOwnedSection(TArray<FStatRow>& Rows, UWorld* WorldPtr)
	{
		const TCHAR* SectionName = TEXT("Engine-owned movement fields (live)");
		const FString Cat = TEXT("Movement|Engine-owned");

		// Prefer a LIVE pawn: MaxWalkSpeed is pushed into the engine's own field from
		// UTraceSettings::WalkSpeed at BeginPlay and re-pushed on a live edit, so the CDO's value is
		// the constructor seed and the pawn's is what the physics step is clamping against. Printing
		// the seed and calling it "movement speed" is exactly the kind of plausible-and-wrong number
		// this sheet exists to stop.
		const UCharacterMovementComponent* Moves = nullptr;
		const UCapsuleComponent* Capsule = nullptr;
		FString Provenance;

		if (WorldPtr != nullptr)
		{
			for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
			{
				const APlayerController* Controller = It->Get();
				const ACharacter* Pawn = (Controller != nullptr) ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
				if (Pawn != nullptr)
				{
					Moves = Pawn->GetCharacterMovement();
					Capsule = Pawn->GetCapsuleComponent();
					Provenance = FString::Printf(TEXT("LIVE pawn %s (engine-owned field, pushed from the knobs)"),
						*Pawn->GetName());
					break;
				}
			}
		}

		if (Moves == nullptr)
		{
			// No pawn (a menu-only or commandlet run). Fall back to the class defaults and SAY SO,
			// rather than printing a number that looks live.
			const UClass* PawnClass = FindObject<UClass>(nullptr, TEXT("/Script/Trace.TraceCharacter"));
			const ACharacter* PawnCdo = (PawnClass != nullptr) ? Cast<ACharacter>(PawnClass->GetDefaultObject()) : nullptr;
			if (PawnCdo != nullptr)
			{
				Moves = PawnCdo->GetCharacterMovement();
				Capsule = PawnCdo->GetCapsuleComponent();
			}
			Provenance = TEXT("ATraceCharacter CDO (constructor seed - NO live pawn in this run, so this is the default and not the played value)");
		}

		if (Moves == nullptr)
		{
			AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("<unavailable>"), TEXT("no character class in this build"),
				TEXT(""), TEXT("text"), TEXT("n/a"),
				TEXT("Neither a live pawn nor an ATraceCharacter CDO could be reached, so the engine-owned movement fields are not in this dump."));
			return;
		}

		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("MaxWalkSpeed"), Number(Moves->MaxWalkSpeed),
			TEXT("uu/s"), TEXT("float"), Provenance,
			TEXT("THE number the physics step clamps ground speed against - i.e. 'movement speed'. It is not a knob: UTraceSettings::WalkSpeed is copied into it, so change WalkSpeed, not this."));
		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("MaxWalkSpeedCrouched"), Number(Moves->MaxWalkSpeedCrouched),
			TEXT("uu/s"), TEXT("float"), Provenance, TEXT("Half of MaxWalkSpeed, set alongside it."));
		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("JumpZVelocity"), Number(Moves->JumpZVelocity),
			TEXT("uu/s"), TEXT("float"), Provenance, TEXT("Upward launch speed on a jump. Apex height is derived in the key-numbers section."));
		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("GravityScale"), Number(Moves->GravityScale),
			TEXT("x (multiplier)"), TEXT("float"), Provenance, TEXT("Multiplies world gravity (980 uu/s^2) for this pawn."));
		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("AirControl"), Number(Moves->AirControl),
			TEXT("0-1"), TEXT("float"), Provenance, TEXT("Fraction of ground acceleration usable in the air."));
		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("MaxAcceleration"), Number(Moves->MaxAcceleration),
			TEXT("uu/s^2"), TEXT("float"), Provenance, TEXT("How hard the pawn accelerates toward its input direction."));
		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("BrakingDecelerationWalking"), Number(Moves->BrakingDecelerationWalking),
			TEXT("uu/s^2"), TEXT("float"), Provenance, TEXT("How hard it stops on the ground with no input."));
		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("GroundFriction"), Number(Moves->GroundFriction),
			TEXT(""), TEXT("float"), Provenance, TEXT("Ground friction coefficient; with braking deceleration it decides how sharp a stop feels."));
		AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("MaxStepHeight"), Number(Moves->MaxStepHeight),
			TEXT("uu"), TEXT("float"), Provenance, TEXT("Tallest ledge the pawn walks straight up without a jump."));

		if (Capsule != nullptr)
		{
			AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("CapsuleHalfHeight"),
				Number(Capsule->GetUnscaledCapsuleHalfHeight()), TEXT("uu"), TEXT("float"), Provenance,
				TEXT("Half the pawn's standing height. The hit-zone fractions in the Damage section are fractions of TWICE this."));
			AddRow(Rows, Sec::EngineOwned, SectionName, Cat, TEXT("CapsuleRadius"),
				Number(Capsule->GetUnscaledCapsuleRadius()), TEXT("uu"), TEXT("float"), Provenance,
				TEXT("Pawn collision radius."));
		}
	}

	/**
	 * The headline block. Every one of these is arithmetic over values fetched by name a few lines
	 * earlier - there is not one literal game number in here, which is what makes it impossible for
	 * this block to disagree with the rest of the sheet.
	 */
	static void BuildKeyNumbers(TArray<FStatRow>& Rows, const FKnobSource& Game, const FKnobSource& Dmg,
		UWorld* WorldPtr, FStatsDumpReport& Report)
	{
		double WalkSpeed = 0.0, KnifeMul = 0.0, CarrierMul = 0.0;
		const bool bWalk    = ReadNumber(Game, TEXT("WalkSpeed"), WalkSpeed, Report);
		const bool bKnife   = ReadNumber(Game, TEXT("KnifeMoveSpeedMultiplier"), KnifeMul, Report);
		const bool bCarrier = ReadNumber(Game, TEXT("CarrierSpeedMultiplier"), CarrierMul, Report);

		AddDerived(Rows, TEXT("Movement speed - base walk"), bWalk, WalkSpeed, TEXT("uu/s"),
			TEXT("UTraceSettings.WalkSpeed"),
			TEXT("The base every other movement number in this game is expressed against."));

		AddDerived(Rows, TEXT("Movement speed - guns stowed (knife profile)"), bWalk && bKnife,
			WalkSpeed * KnifeMul, TEXT("uu/s"),
			TEXT("WalkSpeed x KnifeMoveSpeedMultiplier"),
			TEXT("The speed boost for having no gun out. Stored as a MULTIPLIER of WalkSpeed, not as an absolute, so it tracks the base - retune WalkSpeed and this moves with it."));

		AddDerived(Rows, TEXT("Movement speed - carrying the Core"), bWalk && bCarrier,
			WalkSpeed * CarrierMul, TEXT("uu/s"),
			TEXT("WalkSpeed x CarrierSpeedMultiplier"),
			TEXT("The carrier is slightly faster. Also a multiplier of the base."));

		// --- the owner's second example: "roxie E cooldown" ---------------------------------------
		{
			UClass* RoxieClass = UTraceCharacterAbilitySet::FindClassFor(ETraceCharacterId::Roxie);
			const UTraceCharacterAbilitySet* RoxieCdo = (RoxieClass != nullptr)
				? Cast<UTraceCharacterAbilitySet>(RoxieClass->GetDefaultObject()) : nullptr;
			AddRow(Rows, Sec::Key, TEXT("Key numbers (computed live)"), TEXT("Derived"),
				TEXT("Roxie E (Modded) cooldown - what the game charges"),
				RoxieCdo != nullptr ? Number(RoxieCdo->GetActivatedCooldownSeconds())
				                    : FString(TEXT("<Roxie ability set not found>")),
				TEXT("s"), TEXT("float"),
				TEXT("UTraceAbilitySetRoxie::GetActivatedCooldownSeconds() on the CDO -- reads RoxieModdedCooldownSeconds"),
				TEXT("The owner's own example. The knob is RoxieModdedCooldownSeconds in the Abilities - ROXIE section; this row is what the framework actually charges after clamping, which is the number that matters."));
		}

		// --- the owner's third example: "pistol headshot dmg" -------------------------------------
		double HeadDmg = 0.0, BodyDmg = 0.0, LegDmg = 0.0, MaxHp = 0.0;
		const bool bHead = ReadNumber(Dmg,  TEXT("HeadDamage"), HeadDmg, Report);
		const bool bBody = ReadNumber(Dmg,  TEXT("BodyDamage"), BodyDmg, Report);
		const bool bLeg  = ReadNumber(Dmg,  TEXT("LegDamage"),  LegDmg,  Report);
		const bool bHp   = ReadNumber(Game, TEXT("MaxHealth"),  MaxHp,   Report);

		AddDerived(Rows, TEXT("Pistol damage - headshot"), bHead, HeadDmg, TEXT("HP"),
			TEXT("UTraceDamageSettings.HeadDamage"),
			TEXT("The owner's own example. A headshot is a kill outright, so this is held at or above MaxHealth."));
		AddDerived(Rows, TEXT("Pistol damage - body"), bBody, BodyDmg, TEXT("HP"),
			TEXT("UTraceDamageSettings.BodyDamage"), TEXT("Torso and arms."));
		AddDerived(Rows, TEXT("Pistol damage - legs"), bLeg, LegDmg, TEXT("HP"),
			TEXT("UTraceDamageSettings.LegDamage"), TEXT("Below the hips."));

		AddDerived(Rows, TEXT("Pistol shots to kill - body"), bBody && bHp && BodyDmg > 0.0,
			FMath::CeilToDouble(MaxHp / FMath::Max(1.0, BodyDmg)), TEXT("shots"),
			TEXT("ceil(MaxHealth / BodyDamage)"),
			TEXT("Computed, not typed: retune either number and this cell follows."));
		AddDerived(Rows, TEXT("Pistol shots to kill - legs"), bLeg && bHp && LegDmg > 0.0,
			FMath::CeilToDouble(MaxHp / FMath::Max(1.0, LegDmg)), TEXT("shots"),
			TEXT("ceil(MaxHealth / LegDamage)"), TEXT("Computed."));

		double FireInterval = 0.0;
		const bool bFire = ReadNumber(Game, TEXT("FireInterval"), FireInterval, Report);
		AddDerived(Rows, TEXT("Pistol fire rate"), bFire && FireInterval > 0.0, 60.0 / FMath::Max(0.0001, FireInterval),
			TEXT("rounds/min"), TEXT("60 / FireInterval"),
			TEXT("The knob is the INTERVAL in seconds; RPM is what a player feels. Kept derived so the two can never disagree."));

		double SmgInterval = 0.0;
		const bool bSmgFire = ReadNumber(Game, TEXT("SmgFireInterval"), SmgInterval, Report);
		AddDerived(Rows, TEXT("SMG fire rate (nominal, from the knob)"), bSmgFire && SmgInterval > 0.0,
			60.0 / FMath::Max(0.0001, SmgInterval), TEXT("rounds/min"), TEXT("60 / SmgFireInterval"),
			TEXT("NOMINAL. This is what the knob asks for, not necessarily what a held trigger measures - see the weapons section and the spec's open item on the measured rate."));

		double SmgHead = 0.0, SmgBody = 0.0, SmgLeg = 0.0;
		const bool bSh = ReadNumber(Game, TEXT("SmgHeadDamage"), SmgHead, Report);
		const bool bSb = ReadNumber(Game, TEXT("SmgBodyDamage"), SmgBody, Report);
		const bool bSl = ReadNumber(Game, TEXT("SmgLegDamage"),  SmgLeg,  Report);
		AddDerived(Rows, TEXT("SMG damage - headshot (close)"), bSh, SmgHead, TEXT("HP"),
			TEXT("UTraceSettings.SmgHeadDamage"), TEXT("Inside the falloff range."));
		AddDerived(Rows, TEXT("SMG damage - body (close)"), bSb, SmgBody, TEXT("HP"),
			TEXT("UTraceSettings.SmgBodyDamage"), TEXT("Inside the falloff range."));
		AddDerived(Rows, TEXT("SMG damage - legs (close)"), bSl, SmgLeg, TEXT("HP"),
			TEXT("UTraceSettings.SmgLegDamage"), TEXT("Inside the falloff range."));
		AddDerived(Rows, TEXT("SMG shots to kill - body (close)"), bSb && bHp && SmgBody > 0.0,
			FMath::CeilToDouble(MaxHp / FMath::Max(1.0, SmgBody)), TEXT("shots"),
			TEXT("ceil(MaxHealth / SmgBodyDamage)"), TEXT("Computed."));

		// --- spec v29 2d: the SMG's damage past 800 uu ------------------------------------------
		// The far numbers are their own knobs, so the interesting cells are the ones nobody stores:
		// how much damage the range costs, and what that does to the shots-to-kill count.
		{
			double FalloffStart = 0.0, FalloffRamp = 0.0;
			double FarHead = 0.0, FarBody = 0.0, FarLeg = 0.0;
			bool bFalloffOn = false;
			const bool bStart = ReadNumber(Game, TEXT("SmgFalloffStartUU"), FalloffStart, Report);
			const bool bRamp  = ReadNumber(Game, TEXT("SmgFalloffRampUU"),  FalloffRamp,  Report);
			const bool bFh    = ReadNumber(Game, TEXT("SmgFarHeadDamage"),  FarHead,      Report);
			const bool bFb    = ReadNumber(Game, TEXT("SmgFarBodyDamage"),  FarBody,      Report);
			const bool bFl    = ReadNumber(Game, TEXT("SmgFarLegDamage"),   FarLeg,       Report);
			const bool bOn    = ReadBool(Game,   TEXT("bSmgDamageFalloff"), bFalloffOn,   Report);

			AddRow(Rows, Sec::Key, TEXT("Key numbers (computed live)"), TEXT("Derived"),
				TEXT("SMG damage falloff - shape"),
				(bOn && !bFalloffOn) ? FString(TEXT("OFF - the SMG deals its close numbers at every range"))
					: (!bStart || !bRamp) ? FString(TEXT("<MISSING KNOB - SmgFalloffStartUU / SmgFalloffRampUU>"))
					: (FalloffRamp <= 0.0)
						? FString::Printf(TEXT("CLIFF at %s uu"), *Number(FalloffStart))
						: FString::Printf(TEXT("RAMP from %s uu to %s uu"), *Number(FalloffStart),
							*Number(FalloffStart + FalloffRamp)),
				TEXT(""), TEXT("derived"),
				TEXT("DERIVED at dump time: bSmgDamageFalloff, SmgFalloffStartUU, SmgFalloffRampUU"),
				TEXT("A ramp width of 0 uu IS the cliff - one knob covers both shapes, so reading SmgFalloffRampUU is how you tell which one is shipping without reading the code."));

			AddDerived(Rows, TEXT("SMG damage - headshot (beyond falloff)"), bFh, FarHead, TEXT("HP"),
				TEXT("UTraceSettings.SmgFarHeadDamage"), TEXT("Past the falloff range. The pistol is unchanged at any range - only the SMG falls off."));
			AddDerived(Rows, TEXT("SMG damage - body (beyond falloff)"), bFb, FarBody, TEXT("HP"),
				TEXT("UTraceSettings.SmgFarBodyDamage"), TEXT("Past the falloff range."));
			AddDerived(Rows, TEXT("SMG damage - legs (beyond falloff)"), bFl, FarLeg, TEXT("HP"),
				TEXT("UTraceSettings.SmgFarLegDamage"), TEXT("Past the falloff range."));

			AddDerived(Rows, TEXT("SMG shots to kill - body (beyond falloff)"), bFb && bHp && FarBody > 0.0,
				FMath::CeilToDouble(MaxHp / FMath::Max(1.0, FarBody)), TEXT("shots"),
				TEXT("ceil(MaxHealth / SmgFarBodyDamage)"),
				TEXT("Computed. Compare with the close-range row above - the gap between those two counts IS the range penalty a player feels."));
			AddDerived(Rows, TEXT("SMG damage lost past falloff - body"), bSb && bFb && SmgBody > 0.0,
				100.0 * (SmgBody - FarBody) / FMath::Max(0.001, SmgBody), TEXT("%"),
				TEXT("100 x (SmgBodyDamage - SmgFarBodyDamage) / SmgBodyDamage"),
				TEXT("Percentage of body damage the range costs. Derived from the two knobs, so retuning either one moves this cell."));
		}

		// --- SMG sustain, the same three cells the pistol gets -----------------------------------
		{
			double SmgClip = 0.0, SmgReload = 0.0;
			const bool bSmgClip   = ReadNumber(Game, TEXT("SmgClipSize"), SmgClip, Report);
			const bool bSmgReload = ReadNumber(Game, TEXT("SmgReloadSeconds"), SmgReload, Report);

			AddDerived(Rows, TEXT("SMG - time to empty a full clip"),
				bSmgClip && bSmgFire && SmgInterval > 0.0, (SmgClip - 1.0) * SmgInterval, TEXT("s"),
				TEXT("(SmgClipSize - 1) x SmgFireInterval"),
				TEXT("Trigger-limited, ignoring reload. Nominal, from the interval knob."));
			AddDerived(Rows, TEXT("SMG - sustained DPS through a reload (body shots, inside falloff)"),
				bSmgClip && bSmgFire && bSmgReload && bSb && (SmgClip * SmgInterval + SmgReload) > 0.0,
				(SmgClip * SmgBody) / FMath::Max(0.001, SmgClip * SmgInterval + SmgReload), TEXT("HP/s"),
				TEXT("(SmgClipSize x SmgBodyDamage) / (SmgClipSize x SmgFireInterval + SmgReloadSeconds)"),
				TEXT("Moves when ANY of clip size, fire rate, body damage or reload time is retuned - which is why the 0.8 s -> 1.3 s reload change of spec v29 2c shows up here without anybody editing this sheet."));
		}

		// --- spec v29 2e: what MODDED actually changes, as multiples of the base -----------------
		{
			double RoxieRate = 0.0, RoxieRecoil = 0.0, RoxieDuration = 0.0, BaseKick = 0.0;
			bool bRecoilOn = false;
			const bool bRr  = ReadNumber(Game, TEXT("RoxieModdedFireRateMultiplier"), RoxieRate, Report);
			const bool bRx  = ReadNumber(Game, TEXT("RoxieModdedRecoilScale"), RoxieRecoil, Report);
			const bool bRd  = ReadNumber(Game, TEXT("RoxieModdedDurationSeconds"), RoxieDuration, Report);
			const bool bBk  = ReadNumber(Game, TEXT("RecoilPitchPerShot"), BaseKick, Report);
			const bool bRe  = ReadBool(Game,   TEXT("bRecoilEnabled"), bRecoilOn, Report);

			// The multiplier is a RATE multiplier and DIVIDES the interval - that is the standing
			// rule as it is written in TraceSettings.cpp, and getting it backwards would print 364
			// RPM for a gun that fires at 990. Both weapons go through the one seam.
			const double RoxiePistol = (bFire && bRr && FireInterval > 0.0 && RoxieRate > 0.0)
				? 60.0 / (FireInterval / RoxieRate) : 0.0;
			const double RoxieSmg = (bSmgFire && bRr && SmgInterval > 0.0 && RoxieRate > 0.0)
				? 60.0 / (SmgInterval / RoxieRate) : 0.0;

			AddDerived(Rows, TEXT("Roxie MODDED - pistol fire rate while it is up"),
				bFire && bRr && FireInterval > 0.0 && RoxieRate > 0.0, RoxiePistol, TEXT("rounds/min"),
				TEXT("60 / (FireInterval / RoxieModdedFireRateMultiplier)"),
				TEXT("The multiplier is a RATE multiplier on the base and DIVIDES the interval, so it moves if the pistol's fire rate is ever retuned. It is not an RPM and not an interval."));
			AddDerived(Rows, TEXT("Roxie MODDED - SMG fire rate while it is up"),
				bSmgFire && bRr && SmgInterval > 0.0 && RoxieRate > 0.0, RoxieSmg, TEXT("rounds/min"),
				TEXT("60 / (SmgFireInterval / RoxieModdedFireRateMultiplier)"),
				TEXT("Same seam, other gun. No ability knows which weapon is in her hands - it scales whatever is."));
			AddDerived(Rows, TEXT("Roxie MODDED - first-shot recoil kick"),
				bBk && bRx, BaseKick * RoxieRecoil, TEXT("deg"),
				TEXT("RecoilPitchPerShot x RoxieModdedRecoilScale"),
				TEXT("Spec v29 2e. Recoil is OFF for everyone else, and MODDED re-adds it as a MULTIPLE of the base kick rather than as its own number of degrees - retune RecoilPitchPerShot and her kick follows."));
			AddRow(Rows, Sec::Key, TEXT("Key numbers (computed live)"), TEXT("Derived"),
				TEXT("Recoil for everybody else"),
				(bRe && bRecoilOn) ? FString(TEXT("ON")) : FString(TEXT("OFF - a perfectly static muzzle")),
				TEXT(""), TEXT("bool"), TEXT("DERIVED at dump time: bRecoilEnabled"),
				TEXT("Recoil was removed globally in spec v22 and stays removed. The row above is Roxie's trade for the higher fire rate, and only while MODDED is up."));
			AddDerived(Rows, TEXT("Roxie MODDED - rounds fired in one activation (SMG, held trigger)"),
				bRd && RoxieSmg > 0.0, FMath::FloorToDouble(RoxieDuration * RoxieSmg / 60.0) + 1.0,
				TEXT("rounds"), TEXT("floor(RoxieModdedDurationSeconds x effective SMG RPM / 60) + 1"),
				TEXT("Computed from the duration and the effective rate, so it follows either one. Ignores the reload, which ends MODDED anyway when bRoxieModdedEndsOnReload is true."));
		}

		double AirSoft = 0.0, AirHard = 0.0, AirScale = 0.0;
		const bool bSoft  = ReadNumber(Game, TEXT("AirStrafeSoftCapSpeed"), AirSoft, Report);
		const bool bHard  = ReadNumber(Game, TEXT("AirStrafeHardCapSpeed"), AirHard, Report);
		const bool bScale = ReadNumber(Game, TEXT("AirStrafeAsymptoteScale"), AirScale, Report);
		AddDerived(Rows, TEXT("Air strafe soft cap - EFFECTIVE"), bSoft && bScale, AirSoft * AirScale,
			TEXT("uu/s"), TEXT("AirStrafeSoftCapSpeed x AirStrafeAsymptoteScale"),
			TEXT("The base and the scale are separate knobs; the product is what the game plays at and it appears nowhere else. Reading the base alone is how a double-applied scale hides."));
		AddDerived(Rows, TEXT("Air strafe hard cap - EFFECTIVE"), bHard && bScale, AirHard * AirScale,
			TEXT("uu/s"), TEXT("AirStrafeHardCapSpeed x AirStrafeAsymptoteScale"), TEXT("Same shape as the soft cap."));

		double SlideDur = 0.0, SlideScale = 0.0;
		const bool bSlide  = ReadNumber(Game, TEXT("SlideDuration"), SlideDur, Report);
		const bool bSlideS = ReadNumber(Game, TEXT("SlideMaxLengthScale"), SlideScale, Report);
		AddDerived(Rows, TEXT("Slide duration - EFFECTIVE"), bSlide && bSlideS, SlideDur * SlideScale,
			TEXT("s"), TEXT("SlideDuration x SlideMaxLengthScale"), TEXT("Base x scale, as shipped."));

		double WallWindow = 0.0, WallWindowScale = 0.0, WallRetain = 0.0, WallRetainScale = 0.0;
		const bool bWw  = ReadNumber(Game, TEXT("WallJumpWindowSeconds"), WallWindow, Report);
		const bool bWws = ReadNumber(Game, TEXT("WallJumpWindowScale"), WallWindowScale, Report);
		const bool bWr  = ReadNumber(Game, TEXT("WallJumpSpeedRetention"), WallRetain, Report);
		const bool bWrs = ReadNumber(Game, TEXT("WallJumpMomentumScale"), WallRetainScale, Report);
		AddDerived(Rows, TEXT("Wall jump window - EFFECTIVE"), bWw && bWws, WallWindow * WallWindowScale,
			TEXT("s"), TEXT("WallJumpWindowSeconds x WallJumpWindowScale"), TEXT("Base x scale."));
		AddDerived(Rows, TEXT("Wall jump speed retention - EFFECTIVE"), bWr && bWrs, WallRetain * WallRetainScale,
			TEXT("x (multiplier)"), TEXT("WallJumpSpeedRetention x WallJumpMomentumScale"), TEXT("Base x scale."));

		double GoalFraction = 0.0;
		const bool bGoal = ReadNumber(Game, TEXT("GoalWidthFieldFraction"), GoalFraction, Report);
		AddDerived(Rows, TEXT("Goal mouth width"), bGoal, GoalFraction * 9600.0, TEXT("uu"),
			TEXT("GoalWidthFieldFraction x 9600 uu field width"),
			TEXT("The knob is a FRACTION of the field so the goal tracks the arena if the arena is resized. 9600 uu is the shipped field width."));

		double ThrowSpeed = 0.0, MassScale = 0.0;
		const bool bThrow = ReadNumber(Game, TEXT("CoreThrowSpeed"), ThrowSpeed, Report);
		const bool bMass  = ReadNumber(Game, TEXT("CoreMassScale"), MassScale, Report);
		AddDerived(Rows, TEXT("Core throw speed - EFFECTIVE after weight"), bThrow && bMass && MassScale > 0.0,
			ThrowSpeed / FMath::Sqrt(FMath::Max(0.01, MassScale)), TEXT("uu/s"),
			TEXT("CoreThrowSpeed / sqrt(CoreMassScale)"),
			TEXT("The number a designer types is not the speed the Core flies at - the weight model divides it. This is the flown speed."));

		double ClipSize = 0.0, ReloadSecs = 0.0;
		const bool bClip   = ReadNumber(Game, TEXT("ClipSize"), ClipSize, Report);
		const bool bReload = ReadNumber(Game, TEXT("ReloadSeconds"), ReloadSecs, Report);
		AddDerived(Rows, TEXT("Pistol - time to empty a full clip"), bClip && bFire && FireInterval > 0.0,
			(ClipSize - 1.0) * FireInterval, TEXT("s"),
			TEXT("(ClipSize - 1) x FireInterval"),
			TEXT("Trigger-limited, ignoring reload. ClipSize-1 because the first round costs no interval."));
		AddDerived(Rows, TEXT("Pistol - sustained DPS through a reload (body shots)"),
			bClip && bFire && bReload && bBody && (ClipSize * FireInterval + ReloadSecs) > 0.0,
			(ClipSize * BodyDmg) / FMath::Max(0.001, ClipSize * FireInterval + ReloadSecs), TEXT("HP/s"),
			TEXT("(ClipSize x BodyDamage) / (ClipSize x FireInterval + ReloadSeconds)"),
			TEXT("Computed. The one number that moves when ANY of clip size, fire rate, body damage or reload time is retuned."));

		// Jump apex, from the engine-owned fields rather than a knob.
		{
			const UCharacterMovementComponent* Moves = nullptr;
			if (WorldPtr != nullptr)
			{
				for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
				{
					const APlayerController* Controller = It->Get();
					const ACharacter* Pawn = (Controller != nullptr) ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
					if (Pawn != nullptr)
					{
						Moves = Pawn->GetCharacterMovement();
						break;
					}
				}
			}
			if (Moves == nullptr)
			{
				const UClass* PawnClass = FindObject<UClass>(nullptr, TEXT("/Script/Trace.TraceCharacter"));
				const ACharacter* PawnCdo = (PawnClass != nullptr) ? Cast<ACharacter>(PawnClass->GetDefaultObject()) : nullptr;
				Moves = (PawnCdo != nullptr) ? PawnCdo->GetCharacterMovement() : nullptr;
			}
			if (Moves != nullptr)
			{
				const double Gravity = 980.0 * FMath::Max(0.01f, Moves->GravityScale);
				AddDerived(Rows, TEXT("Jump apex height"), true,
					(static_cast<double>(Moves->JumpZVelocity) * Moves->JumpZVelocity) / (2.0 * Gravity),
					TEXT("uu"), TEXT("JumpZVelocity^2 / (2 x 980 x GravityScale)"),
					TEXT("How high a standing jump reaches. Derived from the engine-owned fields, so it follows a gravity or jump-speed change without anybody remembering to update it."));
			}
		}
	}

	// =============================================================================================
	// WriteStatsCsv
	// =============================================================================================

	static UWorld* BestWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	FString DefaultCsvPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Stats") / TEXT("TraceStats.csv"));
	}

	bool WriteStatsCsv(const FString& InPath, FStatsDumpReport& OutReport)
	{
		OutReport = FStatsDumpReport();

		FString CsvPath = InPath.TrimStartAndEnd();
		if (CsvPath.IsEmpty())
		{
			CsvPath = DefaultCsvPath();
		}
		else if (FPaths::IsRelative(CsvPath))
		{
			CsvPath = FPaths::ProjectDir() / CsvPath;
		}
		CsvPath = FPaths::ConvertRelativePathToFull(CsvPath);

		// -----------------------------------------------------------------------------------------
		// THE PAGES. Adding a settings class to the game means adding ONE LINE here; adding a knob
		// to an existing class means nothing at all.
		// -----------------------------------------------------------------------------------------
		static const FSettingsPage Pages[] =
		{
			{ TEXT("/Script/Trace.TraceSettings"),
			  TEXT("UTraceSettings - Project Settings > Game > Trace Gameplay; [/Script/Trace.TraceSettings] in Config/DefaultGame.ini"),
			  Sec::Other, nullptr },

			{ TEXT("/Script/Trace.TraceDamageSettings"),
			  TEXT("UTraceDamageSettings - Project Settings > Game > Trace Damage Zones"),
			  Sec::Damage, TEXT("Damage and hit zones") },

			{ TEXT("/Script/Trace.TraceMeleeSettings"),
			  TEXT("UTraceMeleeSettings - Project Settings > Game > Trace Melee (Knife); [/Script/Trace.TraceMeleeSettings] in DefaultGame.ini"),
			  Sec::Melee, TEXT("Melee (Knife)") },

			{ TEXT("/Script/Trace.TraceHealthSettings"),
			  TEXT("UTraceHealthSettings - Project Settings > Game > Trace Health (Regeneration)"),
			  Sec::Health, TEXT("Health and regeneration") },

			{ TEXT("/Script/Trace.TraceAudioSettings"),
			  TEXT("UTraceAudioSettings - Project Settings > Game > Trace Audio"),
			  Sec::Audio, TEXT("Audio") },

			{ TEXT("/Script/Trace.TraceGameMode"),
			  TEXT("ATraceGameMode CDO - [/Script/Trace.TraceGameMode] in Config/DefaultGame.ini"),
			  Sec::GameMode, TEXT("Game mode") },

			{ TEXT("/Script/Trace.TraceUserSettings"),
			  TEXT("UTraceUserSettings - PER-PLAYER, saved to Saved/Config/.../TraceUserSettings.ini. NOT shipped tuning"),
			  Sec::Player, TEXT("Player settings (per-player: crosshair, mouse, keybinds, video)") },

			{ TEXT("/Script/Trace.TraceGameUserSettings"),
			  TEXT("UTraceGameUserSettings - PER-PLAYER video/gameplay, saved to GameUserSettings.ini. NOT shipped tuning"),
			  Sec::Player, TEXT("Player settings (per-player: crosshair, mouse, keybinds, video)") },
		};

		TArray<FStatRow> Rows;
		Rows.Reserve(1024);

		BuildAboutSection(Rows, CsvPath);

		FKnobSource GameplaySource;
		FKnobSource DamageSource;

		for (const FSettingsPage& Page : Pages)
		{
			UClass* PageClass = FindObject<UClass>(nullptr, Page.ClassPath);
			if (PageClass == nullptr)
			{
				OutReport.MissingClasses.Add(Page.ClassPath);
				UE_LOG(LogTraceGame, Warning,
					TEXT("[DumpStats] settings class '%s' does not exist in this build - its knobs are NOT in the sheet."),
					Page.ClassPath);
				continue;
			}

			const UObject* PageContainer = ContainerFor(PageClass);
			if (PageContainer == nullptr)
			{
				OutReport.MissingClasses.Add(Page.ClassPath);
				continue;
			}

			const bool bIsPrimary = FCString::Stricmp(Page.ClassPath, TEXT("/Script/Trace.TraceSettings")) == 0;
			if (bIsPrimary)
			{
				GameplaySource.OwnerClass = PageClass;
				GameplaySource.Container = PageContainer;
			}
			else if (FCString::Stricmp(Page.ClassPath, TEXT("/Script/Trace.TraceDamageSettings")) == 0)
			{
				DamageSource.OwnerClass = PageClass;
				DamageSource.Container = PageContainer;
			}

			int32 PropertyCount = 0;
			int32 ConfigCount = 0;
			const int32 Added = WalkClass(Rows, PageClass, PageContainer, Page, PropertyCount, ConfigCount);

			if (bIsPrimary)
			{
				OutReport.TraceSettingsPropertyCount = PropertyCount;
				OutReport.TraceSettingsConfigCount = ConfigCount;
			}
			else
			{
				OutReport.OtherSettingsRowCount += Added;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[DumpStats]   %-46s %4d properties (%d config) -> %d rows"),
				Page.ClassPath, PropertyCount, ConfigCount, Added);
		}

		UWorld* WorldPtr = BestWorld();

		const int32 BeforeNonKnob = Rows.Num();
		BuildCharacterSections(Rows, OutReport);
		BuildEngineOwnedSection(Rows, WorldPtr);
		OutReport.NonKnobRowCount = Rows.Num() - BeforeNonKnob;

		const int32 BeforeDerived = Rows.Num();
		BuildKeyNumbers(Rows, GameplaySource, DamageSource, WorldPtr, OutReport);
		OutReport.DerivedRowCount = Rows.Num() - BeforeDerived;

		// Stable, so rows keep their declaration order inside a section. Declaration order is the
		// order the header reads in, which is the order the person who wrote the knobs thinks in.
		Rows.StableSort([](const FStatRow& A, const FStatRow& B)
		{
			return A.SectionIndex < B.SectionIndex;
		});

		// -----------------------------------------------------------------------------------------
		// Serialise
		// -----------------------------------------------------------------------------------------
		FString Csv;
		Csv.Reserve(Rows.Num() * 320);
		Csv += TEXT("Section,Category,Stat,Display Name,Value,Unit,Type,Source,Description\n");

		TSet<FString> DistinctSections;
		for (const FStatRow& Row : Rows)
		{
			DistinctSections.Add(Row.Section);
			Csv += Cell(Row.Section);      Csv += TEXT(",");
			Csv += Cell(Row.Category);     Csv += TEXT(",");
			Csv += Cell(Row.Stat);         Csv += TEXT(",");
			Csv += Cell(Row.DisplayName);  Csv += TEXT(",");
			Csv += Cell(Row.Value);        Csv += TEXT(",");
			Csv += Cell(Row.Unit);         Csv += TEXT(",");
			Csv += Cell(Row.Type);         Csv += TEXT(",");
			Csv += Cell(Row.Source);       Csv += TEXT(",");
			Csv += Cell(Row.Description);  Csv += TEXT("\n");
		}

		OutReport.RowCount = Rows.Num();
		OutReport.SectionCount = DistinctSections.Num();

		// Saved/Stats does not exist on a fresh clone, and a writer that fails on a missing directory
		// would make the first run of the command look like a code failure.
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CsvPath), /*Tree=*/true);

		// UTF-8 with NO byte-order mark. Sheets reads that correctly on import, and a BOM shows up as
		// a stray character in the first header cell of GitHub's CSV table renderer.
		if (!FFileHelper::SaveStringToFile(Csv, *CsvPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogTraceGame, Error, TEXT("[DumpStats] could not write '%s'."), *CsvPath);
			return false;
		}

		OutReport.Path = CsvPath;
		return true;
	}

	// =============================================================================================
	// VerifyStatsCsv — read it back and PARSE it
	// =============================================================================================

	/**
	 * Quote-aware split of one CSV line into its fields, unescaping doubled quotes.
	 *
	 * This is a SECOND implementation of the format, deliberately: it is written from the CSV rules
	 * rather than from Cell(), so a bug in Cell() shows up here as a wrong field count instead of
	 * being reproduced symmetrically and cancelling out. A verifier that shares the writer's code
	 * cannot fail when the writer is wrong, which is the whole point of running it.
	 *
	 * @return false when the line ends inside an open quote - a structurally broken row.
	 */
	static bool SplitCsvLine(const FString& Line, TArray<FString>& OutFields)
	{
		OutFields.Reset();

		FString Current;
		bool bInQuotes = false;

		for (int32 CharIndex = 0; CharIndex < Line.Len(); ++CharIndex)
		{
			const TCHAR Ch = Line[CharIndex];
			if (Ch == TEXT('"'))
			{
				if (bInQuotes && (CharIndex + 1) < Line.Len() && Line[CharIndex + 1] == TEXT('"'))
				{
					Current.AppendChar(TEXT('"'));   // an escaped quote inside a field
					++CharIndex;
					continue;
				}
				bInQuotes = !bInQuotes;
				continue;
			}
			if (Ch == TEXT(',') && !bInQuotes)
			{
				OutFields.Add(Current);
				Current.Reset();
				continue;
			}
			Current.AppendChar(Ch);
		}
		OutFields.Add(Current);

		return !bInQuotes;
	}

	bool VerifyStatsCsv(const FString& InPath, FString& OutSummary)
	{
		FString CsvPath = InPath.TrimStartAndEnd();
		if (CsvPath.IsEmpty())
		{
			CsvPath = DefaultCsvPath();
		}
		else if (FPaths::IsRelative(CsvPath))
		{
			CsvPath = FPaths::ProjectDir() / CsvPath;
		}
		CsvPath = FPaths::ConvertRelativePathToFull(CsvPath);

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *CsvPath))
		{
			OutSummary = FString::Printf(TEXT("could not read '%s'"), *CsvPath);
			return false;
		}
		if (Lines.Num() < 2)
		{
			OutSummary = FString::Printf(TEXT("'%s' has %d line(s) - no data"), *CsvPath, Lines.Num());
			return false;
		}

		TArray<FString> Fields;
		const bool bHeaderOk = SplitCsvLine(Lines[0], Fields);
		const int32 HeaderFields = bHeaderOk ? Fields.Num() : -1;

		// The Stat column's index, found by NAME rather than assumed to be 2, so reordering the
		// columns later cannot turn this check into a check of the wrong column.
		int32 StatColumn = INDEX_NONE;
		int32 SectionColumn = INDEX_NONE;
		for (int32 ColumnIndex = 0; ColumnIndex < Fields.Num(); ++ColumnIndex)
		{
			if (Fields[ColumnIndex].Equals(TEXT("Stat"), ESearchCase::IgnoreCase))    { StatColumn = ColumnIndex; }
			if (Fields[ColumnIndex].Equals(TEXT("Section"), ESearchCase::IgnoreCase)) { SectionColumn = ColumnIndex; }
		}

		int32 DataRows = 0;
		int32 BadWidth = 0;
		int32 Unterminated = 0;
		int32 EmptyStat = 0;
		int32 EmptySection = 0;
		int32 MissingKnobCells = 0;

		for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
		{
			const FString& Line = Lines[LineIndex];
			if (Line.IsEmpty())
			{
				continue;
			}
			++DataRows;

			if (!SplitCsvLine(Line, Fields))
			{
				++Unterminated;
				continue;
			}
			if (Fields.Num() != HeaderFields)
			{
				++BadWidth;
				if (BadWidth <= 5)
				{
					UE_LOG(LogTraceGame, Error, TEXT("[VerifyStats] line %d has %d fields, header has %d: %s"),
						LineIndex + 1, Fields.Num(), HeaderFields, *Line.Left(160));
				}
				continue;
			}
			if (StatColumn != INDEX_NONE && Fields[StatColumn].TrimStartAndEnd().IsEmpty())
			{
				++EmptyStat;
			}
			if (SectionColumn != INDEX_NONE && Fields[SectionColumn].TrimStartAndEnd().IsEmpty())
			{
				++EmptySection;
			}
			for (const FString& Field : Fields)
			{
				if (Field.Contains(TEXT("<MISSING KNOB")))
				{
					++MissingKnobCells;
					break;
				}
			}
		}

		const bool bPass = (BadWidth == 0) && (Unterminated == 0) && (HeaderFields == 9)
			&& (EmptyStat == 0) && (EmptySection == 0) && (DataRows > 0);
		OutSummary = FString::Printf(
			TEXT("%d data rows, %d columns, %d width mismatches, %d unterminated quotes, %d empty stat names, "
			     "%d empty sections, %d rows with a missing-knob cell"),
			DataRows, HeaderFields, BadWidth, Unterminated, EmptyStat, EmptySection, MissingKnobCells);

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[VerifyStats] VERDICT: PASS - %s (%s)"), *OutSummary, *CsvPath);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[VerifyStats] VERDICT: FAIL - %s (%s)"), *OutSummary, *CsvPath);
		}
		return bPass;
	}

	// =============================================================================================
	// Console commands
	// =============================================================================================

	static void CmdDumpStatsImpl(const TArray<FString>& Args)
	{
		const FString Wanted = (Args.Num() > 0) ? Args[0] : FString();

		FStatsDumpReport Report;
		UE_LOG(LogTraceGame, Display, TEXT("[DumpStats] ===== spec v29 4: every stat in the game, generated ====="));

		if (!WriteStatsCsv(Wanted, Report))
		{
			UE_LOG(LogTraceGame, Error, TEXT("[DumpStats] VERDICT: FAIL - nothing written."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[DumpStats] %d rows in %d sections -> %s"),
			Report.RowCount, Report.SectionCount, *Report.Path);
		UE_LOG(LogTraceGame, Display,
			TEXT("[DumpStats]   UTraceSettings: %d UPROPERTYs, %d of them `config` (the knob registry). "
			     "Other settings pages: %d rows. Not-a-knob rows (roster cards, ability-set CDOs, engine fields): %d. Derived: %d."),
			Report.TraceSettingsPropertyCount, Report.TraceSettingsConfigCount,
			Report.OtherSettingsRowCount, Report.NonKnobRowCount, Report.DerivedRowCount);

		for (const FString& Missing : Report.MissingClasses)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[DumpStats]   MISSING CLASS: %s"), *Missing);
		}
		for (const FString& Missing : Report.MissingKnobs)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[DumpStats]   MISSING KNOB (a derived row could not be computed): %s"), *Missing);
		}

		if (Report.MissingKnobs.Num() == 0 && Report.MissingClasses.Num() == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[DumpStats] VERDICT: PASS - every settings page resolved and every derived row found its base."));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[DumpStats] VERDICT: INCOMPLETE - %d missing class(es), %d missing knob(s). "
				     "The sheet was still written; the affected cells say so instead of printing a wrong number."),
				Report.MissingClasses.Num(), Report.MissingKnobs.Num());
		}
	}

	static void CmdVerifyStatsImpl(const TArray<FString>& Args)
	{
		FString Summary;
		VerifyStatsCsv((Args.Num() > 0) ? Args[0] : FString(), Summary);
	}

	static FAutoConsoleCommand CmdDumpStats(
		TEXT("Trace.DumpStats"),
		TEXT("Dev only. Write every stat in the game to a CSV (default Saved/Stats/TraceStats.csv). Optional argument: a path, absolute or project-relative."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdDumpStatsImpl));

	static FAutoConsoleCommand CmdVerifyStats(
		TEXT("Trace.VerifyStats"),
		TEXT("Dev only. Re-read a stats CSV and prove it is a well-formed table - quote-aware, every row the header's width."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdVerifyStatsImpl));
}

#endif // !UE_BUILD_SHIPPING
