// Trace — one character, as an asset a human can open (spec v17 §5).
//
// ===================================================================================================
// WHAT THIS IS, AND — MORE IMPORTANTLY — WHAT IT IS NOT
// ===================================================================================================
//
// This asset carries a character's IDENTITY AND ITS CARD: the id, the name, the accent colour, and
// the three ability slots' words plus the cooldown the select screen and the HUD ring PRINT.
//
// IT DOES NOT CARRY TUNING. Ripple's dash speed, Chud's 30%, the Spike's 6600 uu range, Oyster's jar
// count, X's bee count — every number the game ENFORCES stays exactly where it is today, on
// UTraceSettings (Project Settings ▸ Game ▸ Trace, category "Abilities|<Character>"), which is
// config-backed, live-editable during PIE and covered knob-by-knob by Trace.VerifyKnobs. Moving any
// of it here would create two places to change one number, which is the specific hazard spec v17 §5
// tells us to resolve rather than create. The SettingsCategory field below exists to point the
// person who opens this asset at the page that holds the real numbers.
//
// THE ONE NUMBER THAT LIVES IN BOTH PLACES IS THE ACTIVATED COOLDOWN, and it did before this pass
// too: Core/TraceCharacterRoster.cpp hardcoded it for the card while the ability set read the
// enforced value from UTraceSettings. This pass does not remove that duplication (that would be a
// redesign, and §0 forbids one), it makes it LOUD: Trace.VerifyCharacterData compares the two and
// FAILS when they disagree. See Data/TraceCharacterDataVerify.cpp.
//
// ===================================================================================================
// HOW THESE ASSETS ARE MADE
// ===================================================================================================
//
//     Scripts/generate-data-assets.py   (via the editor's pythonscript commandlet)
//
// The script creates one package per character and then calls CopyFallbackValues() below, which reads the
// C++ table in Core/TraceCharacterRoster.cpp. The generator therefore CANNOT invent a number: there
// is no character data in the Python at all. MatchesFallback() is the proof in the other direction —
// it re-reads a saved asset and compares it, field by field, with the same C++ table.
//
// A HAND EDIT TO ONE OF THESE ASSETS IS OVERWRITTEN BY THE NEXT --force REGENERATE. That is the
// point of the generator, not a defect; the user-facing notes say so.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Math/Color.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId — THE id space, see the roster header

#include "TraceCharacterDefinition.generated.h"

/**
 * One of the three ability slots spec v14 §6 gives every character: movement, passive, activated.
 *
 * WHY FString AND NOT FText. Every string here is drawn by the Canvas HUD with the engine's built-in
 * BITMAP fonts, whose glyph pages do not cover an em dash or a curly quote, so the shipped prose is
 * deliberately ASCII and upper case (see TraceCharacterRoster.h). FText would add a localisation
 * namespace and key to every generated asset, which is churn in a file we regenerate and diff, for a
 * localisation pass this project has not asked for. If Trace is ever localised, this is the change
 * to make — and it is a one-field-at-a-time change, not a rewrite.
 */
USTRUCT(BlueprintType)
struct FTraceAbilitySlotDefinition
{
	GENERATED_BODY()

	/**
	 * The ability's own name — "RIPPLE", "CHUD", "SPIKE", "PICKLER", "STING".
	 * EMPTY for the movement and passive slots, because §6 never named those and the card does not
	 * print a name for them. Empty is a legitimate value here, not a missing one.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ability Slot")
	FString DisplayName;

	/** The card's prose for this slot — spec §6's own words, trimmed to fit a card. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ability Slot", meta = (MultiLine = "true"))
	FString Description;

	/**
	 * *** DISPLAY ONLY. THIS NUMBER ENFORCES NOTHING. ***
	 *
	 * The select screen prints it ("ACTIVATED [E] - RIPPLE - 20s CD") and the HUD's cooldown ring
	 * uses it as the meter's denominator. What actually gates the E key is
	 * UTraceCharacterAbilitySet::GetActivatedCooldownSeconds(), which reads UTraceSettings.
	 * 0 on the movement and passive slots — neither has a cooldown.
	 *
	 * Trace.VerifyCharacterData FAILS if this disagrees with the enforced value.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ability Slot",
		meta = (ClampMin = "0.0", ClampMax = "180.0", UIMin = "0.0", UIMax = "60.0"))
	float CooldownSeconds = 0.f;
};

/**
 * A character, as data.
 *
 * UPrimaryDataAsset rather than plain UDataAsset so the Asset Manager can find and cook these by
 * type without anything referencing them: GetPrimaryAssetId() below pins the type name to
 * "TraceCharacter" so a Project Settings ▸ Asset Manager entry can be written down once and stay
 * true. Nothing in the shipped game depends on the Asset Manager today — the roster loads these by
 * package path — but a cooked build will, and a type that changes name later is a silent cook miss.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Trace Character Definition"))
class TRACE_API UTraceCharacterDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// =============================================================================================
	// IDENTITY
	// =============================================================================================

	/**
	 * THE id space, ETraceCharacterId's, the same number the player state replicates as a uint8.
	 * There is no second numbering anywhere in this project and this asset does not start one.
	 *
	 * Not BlueprintReadOnly on purpose: ETraceCharacterId is a plain UENUM() and marking a property
	 * of a non-BlueprintType enum Blueprint-visible is a UHT error. The details panel does not care.
	 */
	UPROPERTY(EditAnywhere, Category = "Identity")
	ETraceCharacterId CharacterId = ETraceCharacterId::None;

	/** "ROCCO". Upper case ASCII — see FTraceAbilitySlotDefinition's note on the bitmap fonts. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identity")
	FString DisplayName;

	/**
	 * The card's accent, and the colour of the HUD's ability row. DELIBERATELY NOT A TEAM COLOUR:
	 * on the select screen both teams look identical (an enemy may mirror your pick), so a team
	 * tint there would carry no information at all.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identity")
	FLinearColor AccentColor = FLinearColor::White;

	// =============================================================================================
	// THE THREE SLOTS
	// =============================================================================================

	/** Spec §6 "Movement:". Piggybacks on jump / dash; no cooldown of its own. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Abilities")
	FTraceAbilitySlotDefinition MovementSlot;

	/** Spec §6 "Passive:". Always on; no cooldown. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Abilities")
	FTraceAbilitySlotDefinition PassiveSlot;

	/** Spec §6 "Activated:". The one bound to E. Its CooldownSeconds is the printed one — read it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Abilities")
	FTraceAbilitySlotDefinition ActivatedSlot;

	// =============================================================================================
	// THE SIGNPOST — this is how "two places to change one number" gets resolved
	// =============================================================================================

	/**
	 * The UTraceSettings category that holds this character's ENFORCED numbers, e.g.
	 * "Abilities|Rocco". Nothing reads this at runtime. It is here so that the designer who opens
	 * DA_Character_Rocco looking for the ripple's speed is told, in the asset itself, that it is not
	 * in the asset and exactly which settings page to open instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Where The Enforced Numbers Live")
	FName SettingsCategory;

	// =============================================================================================
	// QUERIES
	// =============================================================================================

	/**
	 * "Is this asset safe to put on the select screen?" Checks the id is a real character, that the
	 * name and the three descriptions are non-empty, and that the accent is finite. The roster
	 * refuses the WHOLE asset table if any one character fails, and says which — a roster that is
	 * four assets and one C++ row is a half-migration nobody can reason about.
	 */
	bool IsUsable(FString& OutWhyNot) const;

	/** FPrimaryAssetId("TraceCharacter", "DA_Character_Rocco"). Stable, and written down in docs. */
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;

	/** "TraceCharacter". The Asset Manager type name; one place, so a doc can quote it. */
	static FName PrimaryAssetTypeName();

	// =============================================================================================
	// THE CONVENTION THE ROSTER AND THE GENERATOR BOTH OBEY
	// =============================================================================================

	/** "/Game/Trace/Data/Characters" — spec v17 §7's folder, with no trailing slash. */
	static const TCHAR* CharactersPackageRoot();

	/** "DA_Character_Rocco". RENAMING ONE OF THESE MAKES THE ROSTER FALL BACK TO C++. */
	static FString AssetNameFor(ETraceCharacterId Id);

	/** "/Game/Trace/Data/Characters/DA_Character_Rocco.DA_Character_Rocco" — a LoadObject path. */
	static FString ObjectPathFor(ETraceCharacterId Id);

	/** "/Game/Trace/Data/Characters/DA_Character_Rocco" — a package path. */
	static FString PackagePathFor(ETraceCharacterId Id);

	// =============================================================================================
	// THE GENERATOR'S TWO ENTRY POINTS — called from Scripts/generate-data-assets.py, nowhere else
	// =============================================================================================

	/**
	 * Fill @p Target from the C++ table in Core/TraceCharacterRoster.cpp. THIS is why the generator
	 * script contains no character data: every value it writes comes through here.
	 *
	 * @param CharacterIdValue  TraceCharacterRoster::FirstId..LastId (1..8 as of spec v18 §2), as an
	 *                          int, because a plain UENUM is awkward to name from Python and an int
	 *                          cannot be mis-imported.
	 * @return false, with nothing written, for an id outside that range.
	 */
	UFUNCTION(BlueprintCallable, Category = "Trace|Data",
		meta = (DisplayName = "Copy Fallback Values From C++ Table"))
	static bool CopyFallbackValues(UTraceCharacterDefinition* Target, int32 CharacterIdValue);

	/**
	 * The round-trip proof, in one call: re-read a saved asset and compare it FIELD BY FIELD with
	 * the C++ table it was generated from. Strings compare exactly (case and punctuation included),
	 * floats compare EXACTLY — no epsilon, because a value that survived a save should be the same
	 * bits, and an epsilon here would hide precisely the drift this is looking for.
	 *
	 * NOT a UFUNCTION, deliberately. Unreal's Python glue collapses "bool return + one out param"
	 * into "-> str or None" — it treats the bool as a success flag and THROWS THE STRING AWAY on
	 * failure, which would silently discard the one thing the generator needs to print. Python calls
	 * DescribeFallbackMismatch() below instead. (Measured, not assumed: the binding's own docstring
	 * reads `X.matches_fallback(definition) -> str or None`.)
	 *
	 * @param OutMismatch  the first difference found, named, with both values. Empty on success.
	 */
	static bool MatchesFallback(UTraceCharacterDefinition* Definition, FString& OutMismatch);

	/**
	 * The same comparison, shaped for Python and Blueprint: EMPTY STRING means the asset matches the
	 * C++ table; anything else is the first difference, named, with both values.
	 */
	UFUNCTION(BlueprintCallable, Category = "Trace|Data",
		meta = (DisplayName = "Describe Difference From C++ Table"))
	static FString DescribeFallbackMismatch(UTraceCharacterDefinition* Definition);
};
