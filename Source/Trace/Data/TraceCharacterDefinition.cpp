// Trace — one character, as an asset. See Data/TraceCharacterDefinition.h for who owns which number.

#include "Data/TraceCharacterDefinition.h"

#include "Core/TraceCharacterRoster.h"
#include "Trace.h"

// The file's own namespace, not an anonymous one: UBT compiles this module as a unity/jumbo build,
// two files that each say `namespace { ... }` become ONE namespace, and this project has already
// lost a Windows build to exactly that (see Scripts/check-jumbo-build-collisions.py).
namespace TraceCharacterDefinitionFile
{
	/** Every ASCII-only, upper-case string this asset carries, in one place for the error messages. */
	bool IsBlank(const FString& Value)
	{
		return Value.TrimStartAndEnd().IsEmpty();
	}

	/**
	 * Exact float comparison, said out loud so nobody "fixes" it into a nearly-equal later.
	 *
	 * Every float here makes exactly one trip: a literal in the C++ table, written into a .uasset by
	 * the generator, read back by the loader. That trip is lossless — the value is serialised as the
	 * same 32-bit float it started as — so ANY difference is a real difference: a hand edit, a
	 * regenerate against changed code, or a corrupted package. An epsilon would hide the 20.0 -> 20.1
	 * hand edit, which is the single most likely way this asset silently stops matching the game.
	 */
	bool SameFloat(float A, float B)
	{
		return A == B;
	}

	bool SameColor(const FLinearColor& A, const FLinearColor& B)
	{
		return SameFloat(A.R, B.R) && SameFloat(A.G, B.G) && SameFloat(A.B, B.B) && SameFloat(A.A, B.A);
	}
}

// =================================================================================================
// The convention: one known name per character, in one known folder
// =================================================================================================

const TCHAR* UTraceCharacterDefinition::CharactersPackageRoot()
{
	return TEXT("/Game/Trace/Data/Characters");
}

FName UTraceCharacterDefinition::PrimaryAssetTypeName()
{
	return FName(TEXT("TraceCharacter"));
}

FString UTraceCharacterDefinition::AssetNameFor(ETraceCharacterId Id)
{
	if (Id == ETraceCharacterId::None || Id >= ETraceCharacterId::Count)
	{
		return FString();
	}

	// "Rocco", not "ROCCO": TraceCharacterIdToString is the project's one stable, never-localised
	// spelling of a character id and it is already what console commands and logs use. The card's
	// upper-case name is a separate, display-only string.
	return FString::Printf(TEXT("DA_Character_%s"), TraceCharacterIdToString(Id));
}

FString UTraceCharacterDefinition::PackagePathFor(ETraceCharacterId Id)
{
	const FString LeafName = AssetNameFor(Id);
	return LeafName.IsEmpty() ? FString() : FString::Printf(TEXT("%s/%s"), CharactersPackageRoot(), *LeafName);
}

FString UTraceCharacterDefinition::ObjectPathFor(ETraceCharacterId Id)
{
	const FString LeafName = AssetNameFor(Id);
	return LeafName.IsEmpty()
		? FString()
		: FString::Printf(TEXT("%s/%s.%s"), CharactersPackageRoot(), *LeafName, *LeafName);
}

FPrimaryAssetId UTraceCharacterDefinition::GetPrimaryAssetId() const
{
	// The base class would derive the type from the nearest native parent, which is right but not
	// WRITTEN DOWN anywhere; a cook rule in Project Settings has to name a type exactly, and a type
	// name nobody stated is a type name that changes. So it is stated, once, here.
	return FPrimaryAssetId(PrimaryAssetTypeName(), GetFName());
}

// =================================================================================================
// Validation — the roster's admission test
// =================================================================================================

bool UTraceCharacterDefinition::IsUsable(FString& OutWhyNot) const
{
	OutWhyNot.Reset();

	const int32 IdValue = static_cast<int32>(CharacterId);
	if (IdValue < static_cast<int32>(TraceCharacterRoster::FirstId)
		|| IdValue > static_cast<int32>(TraceCharacterRoster::LastId))
	{
		OutWhyNot = FString::Printf(
			TEXT("CharacterId is %d; it must be %d..%d (None is not a character)."),
			IdValue, static_cast<int32>(TraceCharacterRoster::FirstId), static_cast<int32>(TraceCharacterRoster::LastId));
		return false;
	}

	if (TraceCharacterDefinitionFile::IsBlank(DisplayName))
	{
		OutWhyNot = TEXT("DisplayName is empty; the select screen and the kill feed both print it.");
		return false;
	}

	// The three descriptions are the whole reason the select screen exists (spec v14 §3: "show each
	// one's movement, passive and activated ability so a player can choose meaningfully"). A blank
	// one draws an empty card section, which reads as a broken game rather than as a missing string.
	if (TraceCharacterDefinitionFile::IsBlank(MovementSlot.Description))
	{
		OutWhyNot = TEXT("MovementSlot.Description is empty.");
		return false;
	}
	if (TraceCharacterDefinitionFile::IsBlank(PassiveSlot.Description))
	{
		OutWhyNot = TEXT("PassiveSlot.Description is empty.");
		return false;
	}
	if (TraceCharacterDefinitionFile::IsBlank(ActivatedSlot.Description))
	{
		OutWhyNot = TEXT("ActivatedSlot.Description is empty.");
		return false;
	}
	if (TraceCharacterDefinitionFile::IsBlank(ActivatedSlot.DisplayName))
	{
		OutWhyNot = TEXT("ActivatedSlot.DisplayName is empty; the HUD's ability row prints it.");
		return false;
	}

	// The HUD divides by this. Entry->ActivatedCooldown feeds the cooldown ring's denominator, and
	// the drawing code clamps at 0.01 rather than dividing by zero — but a zero here would still make
	// the ring meaningless, and it can only come from a bad edit.
	if (!(ActivatedSlot.CooldownSeconds > 0.f))
	{
		OutWhyNot = FString::Printf(
			TEXT("ActivatedSlot.CooldownSeconds is %.4f; it must be greater than zero (the HUD ring divides by it)."),
			ActivatedSlot.CooldownSeconds);
		return false;
	}

	if (!FMath::IsFinite(AccentColor.R) || !FMath::IsFinite(AccentColor.G)
		|| !FMath::IsFinite(AccentColor.B) || !FMath::IsFinite(AccentColor.A))
	{
		OutWhyNot = TEXT("AccentColor has a non-finite channel.");
		return false;
	}

	return true;
}

// =================================================================================================
// The generator's two entry points
// =================================================================================================

bool UTraceCharacterDefinition::CopyFallbackValues(UTraceCharacterDefinition* Target, int32 CharacterIdValue)
{
	if (Target == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[CharacterData] CopyFallbackValues called with a null target."));
		return false;
	}

	if (CharacterIdValue < static_cast<int32>(TraceCharacterRoster::FirstId)
		|| CharacterIdValue > static_cast<int32>(TraceCharacterRoster::LastId))
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[CharacterData] CopyFallbackValues: %d is not a character id (%d..%d)."),
			CharacterIdValue, static_cast<int32>(TraceCharacterRoster::FirstId),
			static_cast<int32>(TraceCharacterRoster::LastId));
		return false;
	}

	// *** DELIBERATELY CppFallbackTable(), NOT All() OR Find(). ***
	//
	// All() reads whatever the roster resolved to, and if the assets already exist that would make
	// this asset -> asset: the generator would faithfully copy a hand edit forward and the round-trip
	// proof would then pass while proving nothing at all. The C++ table is the origin, and the
	// generator reads the origin.
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Fallback = TraceCharacterRoster::CppFallbackTable();
	const int32 Index = CharacterIdValue - static_cast<int32>(TraceCharacterRoster::FirstId);
	if (!Fallback.IsValidIndex(Index) || Fallback[Index].Id != static_cast<uint8>(CharacterIdValue))
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[CharacterData] CopyFallbackValues: the C++ fallback table is out of id order at index %d."), Index);
		return false;
	}

	const TraceCharacterRoster::FTraceCharacterEntry& Row = Fallback[Index];

	Target->CharacterId = static_cast<ETraceCharacterId>(CharacterIdValue);
	Target->DisplayName = Row.Name;
	Target->AccentColor = Row.Accent;

	// An EMPTY path becomes a NULL soft pointer, not a soft pointer to "" — the two are the same
	// thing to the runtime but not to a diff, and null is what the details panel shows as "None".
	// Nothing is loaded here: assigning a soft pointer never touches the package, which is exactly
	// what lets the generator run on a machine where the character art was never imported.
	Target->BodyMesh = (Row.BodyMeshPath != nullptr && Row.BodyMeshPath[0] != TEXT('\0'))
		? TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(Row.BodyMeshPath))
		: TSoftObjectPtr<USkeletalMesh>();
	Target->BodyMeshYaw = Row.BodyMeshYaw;
	Target->BodyAnimClass = (Row.BodyAnimClassPath != nullptr && Row.BodyAnimClassPath[0] != TEXT('\0'))
		? TSoftClassPtr<UAnimInstance>(FSoftClassPath(Row.BodyAnimClassPath))
		: TSoftClassPtr<UAnimInstance>();

	Target->MovementSlot.DisplayName.Reset();     // §6 never named the movement ability
	Target->MovementSlot.Description = Row.Movement;
	Target->MovementSlot.CooldownSeconds = 0.f;

	Target->PassiveSlot.DisplayName.Reset();      // nor the passive
	Target->PassiveSlot.Description = Row.Passive;
	Target->PassiveSlot.CooldownSeconds = 0.f;

	Target->ActivatedSlot.DisplayName = Row.ActivatedName;
	Target->ActivatedSlot.Description = Row.Activated;
	Target->ActivatedSlot.CooldownSeconds = Row.ActivatedCooldown;

	// "Abilities|Rocco" — the exact UPROPERTY Category string on UTraceSettings, so the person who
	// reads it in the details panel can type it into the settings page's search box and land.
	Target->SettingsCategory = FName(*FString::Printf(TEXT("Abilities|%s"),
		TraceCharacterIdToString(Target->CharacterId)));

	Target->MarkPackageDirty();
	return true;
}

bool UTraceCharacterDefinition::MatchesFallback(UTraceCharacterDefinition* Definition, FString& OutMismatch)
{
	OutMismatch.Reset();

	if (Definition == nullptr)
	{
		OutMismatch = TEXT("the definition is null");
		return false;
	}

	const int32 IdValue = static_cast<int32>(Definition->CharacterId);
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Fallback = TraceCharacterRoster::CppFallbackTable();
	const int32 Index = IdValue - static_cast<int32>(TraceCharacterRoster::FirstId);

	if (!Fallback.IsValidIndex(Index) || Fallback[Index].Id != static_cast<uint8>(IdValue))
	{
		OutMismatch = FString::Printf(TEXT("CharacterId %d has no row in the C++ table"), IdValue);
		return false;
	}

	const TraceCharacterRoster::FTraceCharacterEntry& Row = Fallback[Index];

	// One field per line, first difference wins, and the message always carries BOTH values. A
	// verification that says "mismatch" without saying what to what is a verification that gets
	// re-run rather than acted on.
	auto SameString = [&OutMismatch](const TCHAR* FieldLabel, const FString& Asset, const TCHAR* Cpp) -> bool
	{
		if (Asset.Equals(FString(Cpp), ESearchCase::CaseSensitive))
		{
			return true;
		}
		OutMismatch = FString::Printf(TEXT("%s: asset \"%s\" != C++ \"%s\""), FieldLabel, *Asset, Cpp);
		return false;
	};

	if (!SameString(TEXT("DisplayName"),               Definition->DisplayName,               Row.Name))          { return false; }
	if (!SameString(TEXT("MovementSlot.Description"),  Definition->MovementSlot.Description,  Row.Movement))      { return false; }
	if (!SameString(TEXT("PassiveSlot.Description"),   Definition->PassiveSlot.Description,   Row.Passive))       { return false; }
	if (!SameString(TEXT("ActivatedSlot.DisplayName"), Definition->ActivatedSlot.DisplayName, Row.ActivatedName)) { return false; }
	if (!SameString(TEXT("ActivatedSlot.Description"), Definition->ActivatedSlot.Description, Row.Activated))     { return false; }

	if (!TraceCharacterDefinitionFile::SameFloat(Definition->ActivatedSlot.CooldownSeconds, Row.ActivatedCooldown))
	{
		OutMismatch = FString::Printf(TEXT("ActivatedSlot.CooldownSeconds: asset %.6f != C++ %.6f"),
			Definition->ActivatedSlot.CooldownSeconds, Row.ActivatedCooldown);
		return false;
	}

	if (!TraceCharacterDefinitionFile::SameColor(Definition->AccentColor, Row.Accent))
	{
		OutMismatch = FString::Printf(TEXT("AccentColor: asset %s != C++ %s"),
			*Definition->AccentColor.ToString(), *Row.Accent.ToString());
		return false;
	}

	// The body, compared as a PATH STRING and never loaded. This runs on machines where the mesh is
	// not imported (that is the whole reason the reference is soft), so a comparison that resolved
	// the pointer would report a drift that is really just an absent file.
	{
		const FString AssetBodyMesh = Definition->BodyMesh.IsNull()
			? FString()
			: Definition->BodyMesh.ToSoftObjectPath().ToString();
		if (!SameString(TEXT("BodyMesh"), AssetBodyMesh, Row.BodyMeshPath))
		{
			return false;
		}
	}

	if (!TraceCharacterDefinitionFile::SameFloat(Definition->BodyMeshYaw, Row.BodyMeshYaw))
	{
		OutMismatch = FString::Printf(TEXT("BodyMeshYaw: asset %.6f != C++ %.6f"),
			Definition->BodyMeshYaw, Row.BodyMeshYaw);
		return false;
	}

	// The anim class, compared the same way and for the same reason: a path string, never loaded.
	{
		const FString AssetBodyAnim = Definition->BodyAnimClass.IsNull()
			? FString()
			: Definition->BodyAnimClass.ToSoftObjectPath().ToString();
		if (!SameString(TEXT("BodyAnimClass"), AssetBodyAnim, Row.BodyAnimClassPath))
		{
			return false;
		}
	}

	// The slots that must stay empty. If a future pass gives the movement ability a name, this line
	// is the one that will say so, rather than the card quietly growing a caption.
	if (!Definition->MovementSlot.DisplayName.IsEmpty() || !Definition->PassiveSlot.DisplayName.IsEmpty())
	{
		OutMismatch = FString::Printf(
			TEXT("Movement/Passive slot DisplayName should be empty (C++ has no name for either); got \"%s\" / \"%s\""),
			*Definition->MovementSlot.DisplayName, *Definition->PassiveSlot.DisplayName);
		return false;
	}

	if (!TraceCharacterDefinitionFile::SameFloat(Definition->MovementSlot.CooldownSeconds, 0.f)
		|| !TraceCharacterDefinitionFile::SameFloat(Definition->PassiveSlot.CooldownSeconds, 0.f))
	{
		OutMismatch = FString::Printf(
			TEXT("Movement/Passive slot CooldownSeconds should be 0; got %.6f / %.6f"),
			Definition->MovementSlot.CooldownSeconds, Definition->PassiveSlot.CooldownSeconds);
		return false;
	}

	return true;
}

FString UTraceCharacterDefinition::DescribeFallbackMismatch(UTraceCharacterDefinition* Definition)
{
	FString Mismatch;
	if (MatchesFallback(Definition, Mismatch))
	{
		// Empty is the PASS, and it is empty on purpose: a caller that forgets to check gets a blank
		// line rather than a cheerful "OK" it might print next to a failure.
		return FString();
	}

	return Mismatch.IsEmpty() ? TEXT("mismatched, but the comparison returned no detail") : Mismatch;
}
