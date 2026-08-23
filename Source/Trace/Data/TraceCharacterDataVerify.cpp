// Trace — the evidence half of spec v17 §5.
//
// ===================================================================================================
// WHY THIS FILE EXISTS
// ===================================================================================================
//
// §5 asks for the character assets to be "generated from the CURRENT C++ values so nothing changes —
// prove the values round-trip identically". "Prove" is the operative word: a migration that moved
// every character into a .uasset and quietly rounded one cooldown would look completely healthy
// from the outside, and the first person to notice would be a player who felt that Rocco's ripple
// came back late.
//
//     Trace.VerifyCharacterData
//
// answers four questions in one run, in this order, because each one is only worth asking if the
// previous one passed:
//
//   A. WHICH SOURCE is the running game actually serving? (Not "which do we think"; which.)
//   B. ROUND TRIP — every saved asset, re-read from disk, compared field by field with the C++ table
//      it was generated from.
//   C. THE LIVE TABLE — what All() is handing the select screen and the HUD right now, compared with
//      the C++ table. B proves the files are right; C proves the thing the game is DRAWING is right,
//      and those are different claims.
//   D. THE ONE NUMBER THAT LIVES IN TWO PLACES — the activated cooldown. The asset (and, before this
//      pass, a hardcoded literal) carries the number the card PRINTS and the HUD ring divides by;
//      UTraceSettings carries the number the game ENFORCES. They agreed before this pass and they
//      must keep agreeing. This section is what makes a disagreement loud instead of invisible.
//
// HOW TO MAKE IT GO RED (it is not evidence if it cannot):
//   * open Content/Trace/Data/Characters/DA_Character_Rocco in the editor, change the activated
//     cooldown from 20 to 21, save, then run the command: B and D both fail and name the field.
//   * or change one word of the card prose: B fails and prints both strings.
//   * or run with the assets deleted: A says C++ fallback, B reports NOT PRESENT, and the verdict is
//     an honest "the fallback is in force" rather than a pass.
//
// HEADLESS:
//   Scripts/run-listen-server.sh -- -TraceExec=Trace.VerifyCharacterData|Trace.Data.DumpCharacters
//
// ===================================================================================================

#include "CoreMinimal.h"

#include "Abilities/TraceAbilityTypes.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Core/TraceCharacterRoster.h"
#include "Data/TraceCharacterDefinition.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Trace.h"
#include "UObject/UObjectGlobals.h"

// Named after the file. The module is a unity/jumbo build and this project has already lost a
// Windows build to four verify files that each defined the same helper in `namespace { }`.
namespace TraceCharacterDataVerify
{
	/** The same checklist shape the other verify files use, so a FAIL is impossible to skim past. */
	struct FDataChecklist
	{
		int32 Passed = 0;
		int32 Failed = 0;
		int32 Skipped = 0;

		void Check(bool bCondition, const FString& What, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterData] %s  %-34s  |  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What, *Detail);
		}

		void Skip(const FString& What, const FString& Detail)
		{
			++Skipped;
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterData] SKIP  %-34s  |  %s"), *What, *Detail);
		}
	};

	/** Every difference between two roster rows, named, or an empty string when they are identical. */
	FString DifferenceBetween(const TraceCharacterRoster::FTraceCharacterEntry& Live,
	                          const TraceCharacterRoster::FTraceCharacterEntry& Reference)
	{
		TArray<FString> Differences;

		auto CompareText = [&Differences](const TCHAR* Label, const TCHAR* A, const TCHAR* B)
		{
			// Both sides are non-null by construction (FTraceCharacterEntry defaults to TEXT("")),
			// but a null here would be a crash inside a verification, which is the least useful
			// place in the codebase to crash.
			const TCHAR* SafeA = (A != nullptr) ? A : TEXT("<null>");
			const TCHAR* SafeB = (B != nullptr) ? B : TEXT("<null>");
			if (FCString::Strcmp(SafeA, SafeB) != 0)
			{
				Differences.Add(FString::Printf(TEXT("%s (\"%s\" vs \"%s\")"), Label, SafeA, SafeB));
			}
		};

		if (Live.Id != Reference.Id)
		{
			Differences.Add(FString::Printf(TEXT("Id (%d vs %d)"),
				static_cast<int32>(Live.Id), static_cast<int32>(Reference.Id)));
		}

		CompareText(TEXT("Name"),          Live.Name,          Reference.Name);
		CompareText(TEXT("Movement"),      Live.Movement,      Reference.Movement);
		CompareText(TEXT("Passive"),       Live.Passive,       Reference.Passive);
		CompareText(TEXT("ActivatedName"), Live.ActivatedName, Reference.ActivatedName);
		CompareText(TEXT("Activated"),     Live.Activated,     Reference.Activated);

		// The body every other player sees. Compared as a PATH and never loaded — the mesh is
		// imported per developer and is legitimately absent here, which is exactly why the reference
		// is soft. An empty path on both sides means "Epic's Mannequin", which is nine of the ten.
		CompareText(TEXT("BodyMeshPath"),  Live.BodyMeshPath,  Reference.BodyMeshPath);

		// And the class that drives it. A body whose anim class drifted would still LOOK right in a
		// screenshot of a standing pawn, which is exactly why it is compared here rather than left
		// to be noticed by somebody watching a character walk.
		CompareText(TEXT("BodyAnimClassPath"), Live.BodyAnimClassPath, Reference.BodyAnimClassPath);

		if (Live.BodyMeshYaw != Reference.BodyMeshYaw)
		{
			Differences.Add(FString::Printf(TEXT("BodyMeshYaw (%.6f vs %.6f)"),
				Live.BodyMeshYaw, Reference.BodyMeshYaw));
		}

		// EXACT, not nearly-equal. See the note in TraceCharacterDefinition.cpp: the value makes one
		// lossless trip through a .uasset, so any difference at all is a real edit.
		if (Live.ActivatedCooldown != Reference.ActivatedCooldown)
		{
			Differences.Add(FString::Printf(TEXT("ActivatedCooldown (%.6f vs %.6f)"),
				Live.ActivatedCooldown, Reference.ActivatedCooldown));
		}

		if (Live.Accent.R != Reference.Accent.R || Live.Accent.G != Reference.Accent.G
			|| Live.Accent.B != Reference.Accent.B || Live.Accent.A != Reference.Accent.A)
		{
			Differences.Add(FString::Printf(TEXT("Accent (%s vs %s)"),
				*Live.Accent.ToString(), *Reference.Accent.ToString()));
		}

		return FString::Join(Differences, TEXT("; "));
	}

	/**
	 * The cooldown the game ENFORCES for @p Id, asked of the shipped function rather than re-derived.
	 *
	 * UTraceCharacterAbilitySet::GetActivatedCooldownSeconds() is what
	 * UTraceAbilityComponent::ServerActivateAbility calls, and every one of the overrides is a
	 * pure read of UTraceSettings — no pawn, no component, no world. That is what makes it safe to
	 * ask the CLASS DEFAULT OBJECT, which is the only instance available outside a live match, and
	 * asking it is the whole point: a table of knob names copied into this file would be a fourth
	 * copy of the number and would agree with the game right up until somebody renamed a knob.
	 *
	 * If a future character's override ever touches GetCharacter() this call becomes unsafe — it is a
	 * dev-only command, it would fail loudly here, and the fix is to give the ability set a
	 * CDO-safe cooldown accessor rather than to reintroduce the copied table.
	 *
	 * @return a negative number when no class claims @p Id (a Mannequin-with-a-name character).
	 */
	float EnforcedCooldownFor(ETraceCharacterId Id)
	{
		UClass* const SetClass = UTraceCharacterAbilitySet::FindClassFor(Id);
		if (SetClass == nullptr)
		{
			return -1.f;
		}

		const UTraceCharacterAbilitySet* const Defaults =
			Cast<UTraceCharacterAbilitySet>(SetClass->GetDefaultObject());

		return (Defaults != nullptr) ? Defaults->GetActivatedCooldownSeconds() : -1.f;
	}

	// =============================================================================================
	// Trace.VerifyCharacterData
	// =============================================================================================

	void VerifyCharacterData()
	{
		FDataChecklist List;

		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterData] ===== spec v17 §5: characters as assets, verified ====="));

		// ---- A. WHICH SOURCE ---------------------------------------------------------------------
		const TraceCharacterRoster::ESource Source = TraceCharacterRoster::CurrentSource();
		const bool bAssets = (Source == TraceCharacterRoster::ESource::Assets);

		IConsoleVariable* const Toggle = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Data.UseCharacterAssets"));
		const int32 ToggleValue = (Toggle != nullptr) ? Toggle->GetInt() : -1;

		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterData] A. SOURCE: %s   (Trace.Data.UseCharacterAssets = %d)"),
			TraceCharacterRoster::CurrentSourceName(), ToggleValue);

		// ---- B. ROUND TRIP, one asset at a time ---------------------------------------------------
		//
		// Deliberately independent of section A: this loads each package DIRECTLY, so it reports on
		// the files on disk even when the toggle is off and the game is running from C++. "The assets
		// are correct" and "the assets are in use" are separate facts and each is worth knowing.
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterData] B. ROUND TRIP — each saved asset vs the C++ table it was generated from"));

		int32 PresentAssets = 0;
		for (int32 IdValue = TraceCharacterRoster::FirstId; IdValue <= TraceCharacterRoster::LastId; ++IdValue)
		{
			const ETraceCharacterId TypedId = static_cast<ETraceCharacterId>(IdValue);
			const FString AssetLabel = UTraceCharacterDefinition::AssetNameFor(TypedId);
			const FString PackagePath = UTraceCharacterDefinition::PackagePathFor(TypedId);

			if (!FPackageName::DoesPackageExist(PackagePath))
			{
				List.Skip(AssetLabel, FString::Printf(
					TEXT("NOT PRESENT (%s) — the C++ fallback is what runs. Not a failure; run "
					     "Scripts/generate-data-assets.py to author it."), *PackagePath));
				continue;
			}

			++PresentAssets;

			UTraceCharacterDefinition* const Definition = LoadObject<UTraceCharacterDefinition>(
				nullptr, *UTraceCharacterDefinition::ObjectPathFor(TypedId), nullptr, LOAD_NoWarn | LOAD_Quiet);

			if (Definition == nullptr)
			{
				List.Check(false, AssetLabel, TEXT("the package exists but did not load as a UTraceCharacterDefinition"));
				continue;
			}

			FString WhyUnusable;
			if (!Definition->IsUsable(WhyUnusable))
			{
				List.Check(false, AssetLabel, FString::Printf(TEXT("not usable: %s"), *WhyUnusable));
				continue;
			}

			FString Mismatch;
			const bool bMatches = UTraceCharacterDefinition::MatchesFallback(Definition, Mismatch);
			List.Check(bMatches, AssetLabel, bMatches
				? FString::Printf(TEXT("identical to the C++ row (name, 3 descriptions, cooldown %.2fs, accent)"),
					Definition->ActivatedSlot.CooldownSeconds)
				: Mismatch);
		}

		// ---- C. THE LIVE TABLE --------------------------------------------------------------------
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterData] C. LIVE TABLE — what All() is handing the select screen and the HUD right now"));

		const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Live = TraceCharacterRoster::All();
		const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Cpp  = TraceCharacterRoster::CppFallbackTable();

		List.Check(Live.Num() == TraceCharacterRoster::Count,
			TEXT("live table size"),
			FString::Printf(TEXT("%d entries, expected %d"), Live.Num(), TraceCharacterRoster::Count));

		if (!bAssets)
		{
			// Same array, so a field-by-field comparison would be comparing a thing with itself and
			// printing a row of PASSes that mean nothing. Say what is actually true instead.
			List.Check(&Live == &Cpp, TEXT("live table IS the C++ table"),
				TEXT("the fallback is in force, so there is nothing to differ — this is the pre-v17 behaviour, exactly"));
		}
		else
		{
			for (int32 Index = 0; Index < FMath::Min(Live.Num(), Cpp.Num()); ++Index)
			{
				const FString Difference = DifferenceBetween(Live[Index], Cpp[Index]);
				List.Check(Difference.IsEmpty(),
					FString::Printf(TEXT("live[%d] %s"), Index, Cpp[Index].Name),
					Difference.IsEmpty() ? TEXT("every field identical to the C++ row") : Difference);
			}
		}

		// ---- D. THE ONE NUMBER THAT LIVES IN TWO PLACES -------------------------------------------
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterData] D. PRINTED cooldown (asset/table, used by the card and the HUD ring) ")
			TEXT("vs ENFORCED cooldown (UTraceSettings, used by the E key)"));

		for (int32 Index = 0; Index < Live.Num(); ++Index)
		{
			const TraceCharacterRoster::FTraceCharacterEntry& Row = Live[Index];
			const ETraceCharacterId TypedId = static_cast<ETraceCharacterId>(Row.Id);
			const float Enforced = EnforcedCooldownFor(TypedId);

			if (Enforced < 0.f)
			{
				List.Skip(FString::Printf(TEXT("cooldown %s"), Row.Name),
					TEXT("no UTraceCharacterAbilitySet subclass claims this id, so nothing enforces a cooldown"));
				continue;
			}

			const bool bAgree = FMath::IsNearlyEqual(Row.ActivatedCooldown, Enforced, 0.001f);
			List.Check(bAgree, FString::Printf(TEXT("cooldown %s"), Row.Name),
				FString::Printf(TEXT("printed %.3fs, enforced %.3fs%s"),
					Row.ActivatedCooldown, Enforced,
					bAgree ? TEXT("") : TEXT("  <-- CHANGE BOTH: the asset's ActivatedSlot.CooldownSeconds "
					                         "AND the UTraceSettings knob in the character's Abilities|<Name> category")));
		}

		// ---- VERDICT -------------------------------------------------------------------------------
		if (List.Failed > 0)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CharacterData] VERDICT: *** FAIL *** — %d passed, %d FAILED, %d skipped. The assets and the "
				     "C++ values have drifted apart; the game is no longer playing the numbers this pass migrated."),
				List.Passed, List.Failed, List.Skipped);
		}
		else if (PresentAssets == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[CharacterData] VERDICT: FALLBACK IN FORCE — no character assets exist, so the game runs from "
				     "the C++ table exactly as it did before spec v17 §5 (%d passed, %d skipped). "
				     "Run Scripts/generate-data-assets.py to author them."),
				List.Passed, List.Skipped);
		}
		else if (PresentAssets < TraceCharacterRoster::Count)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CharacterData] VERDICT: PARTIAL — %d of %d assets exist. The roster is ALL-OR-NONE, so the "
				     "C++ table is what runs. Regenerate with Scripts/generate-data-assets.py."),
				PresentAssets, TraceCharacterRoster::Count);
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[CharacterData] VERDICT: PASS — %d checks, %d skipped. All %d assets round-trip identically to the "
				     "C++ table, the live roster matches it field for field, and every printed cooldown equals the "
				     "cooldown UTraceSettings enforces."),
				List.Passed, List.Skipped, PresentAssets);
		}
	}

	// =============================================================================================
	// Trace.Data.DumpCharacters — the human-readable half
	// =============================================================================================

	void DumpCharacters()
	{
		const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Table = TraceCharacterRoster::All();

		UE_LOG(LogTraceGame, Display, TEXT("[CharacterData] ===== the roster, as the game sees it ====="));
		UE_LOG(LogTraceGame, Display, TEXT("[CharacterData] source: %s"), TraceCharacterRoster::CurrentSourceName());

		for (const TraceCharacterRoster::FTraceCharacterEntry& Row : Table)
		{
			const float Enforced = EnforcedCooldownFor(static_cast<ETraceCharacterId>(Row.Id));

			UE_LOG(LogTraceGame, Display,
				TEXT("[CharacterData]   %d %-7s accent %s  activated \"%s\"  printed CD %.2fs  enforced CD %s"),
				static_cast<int32>(Row.Id), Row.Name, *Row.Accent.ToString(), Row.ActivatedName,
				Row.ActivatedCooldown,
				(Enforced >= 0.f) ? *FString::Printf(TEXT("%.2fs"), Enforced) : TEXT("<no ability set>"));

			// The body, on its own line because the path is long and because "(the Mannequin)" is the
			// answer for nine of the ten and should read as a normal state rather than as a blank.
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterData]     body %s  yaw %.1f"),
				(Row.BodyMeshPath != nullptr && Row.BodyMeshPath[0] != TEXT('\0'))
					? Row.BodyMeshPath : TEXT("(the Mannequin)"),
				Row.BodyMeshYaw);
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterData]     anim %s"),
				(Row.BodyAnimClassPath != nullptr && Row.BodyAnimClassPath[0] != TEXT('\0'))
					? Row.BodyAnimClassPath : TEXT("(ABP_Unarmed)"));
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterData]       MOVEMENT  %s"), Row.Movement);
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterData]       PASSIVE   %s"), Row.Passive);
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterData]       ACTIVATED %s"), Row.Activated);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterData] The numbers above are the ones a card PRINTS. Every number the game ENFORCES is on "
			     "UTraceSettings (Project Settings > Game > Trace, category Abilities|<Character>) — "
			     "Trace.DumpSettings prints those."));
	}

	void ReloadCharacters()
	{
		TraceCharacterRoster::ForceReload();
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterData] Roster dropped; it will re-resolve on the next read. New source: %s"),
			TraceCharacterRoster::CurrentSourceName());
	}
}

#if !UE_BUILD_SHIPPING
namespace TraceCharacterDataVerify
{
	FAutoConsoleCommand CmdVerifyCharacterData(
		TEXT("Trace.VerifyCharacterData"),
		TEXT("Dev only. spec v17 §5. Prove every character asset round-trips identically to the C++ table, that "
		     "the live roster matches it, and that every printed cooldown equals the one UTraceSettings enforces."),
		FConsoleCommandDelegate::CreateStatic(&VerifyCharacterData));

	FAutoConsoleCommand CmdDumpCharacters(
		TEXT("Trace.Data.DumpCharacters"),
		TEXT("Dev only. Log the roster the game is serving right now, and say whether it came from the assets or "
		     "from the C++ table."),
		FConsoleCommandDelegate::CreateStatic(&DumpCharacters));

	FAutoConsoleCommand CmdReloadCharacters(
		TEXT("Trace.Data.ReloadCharacters"),
		TEXT("Dev only. Forget the resolved roster so the next read re-decides between the assets and the C++ table. "
		     "Use it after regenerating the assets without restarting."),
		FConsoleCommandDelegate::CreateStatic(&ReloadCharacters));
}
#endif
