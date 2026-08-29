// Trace — the ten characters, as DATA.
//
// Spec v14 §3 asks for a select screen that shows "each one's movement, passive and activated
// ability so a player can choose meaningfully". That means the prose is a shipped asset, not
// decoration, and it must live in exactly one place: the select screen draws it, the game mode logs
// from it, and the ability framework can read it rather than writing a second table of names.
//
// DELIBERATELY PLAIN C++ — no UCLASS, no UENUM, nothing reflected. Same shape and same reasoning as
// UI/TraceMatchOptions.h: this header is included by the game mode, the player state and the HUD, so
// keeping UnrealHeaderTool out of it keeps the include free.
//
// *** THE ID SPACE IS ETraceCharacterId's (spec v14 §5, EXTENDED BY v18 §2 AND v19 §3). ***
//     None = 0, Rocco = 1, Chut = 2, Mace = 3, Oyster = 4, X = 5,
//     Roxie = 6, Elle = 7, Slimeball = 8, Mortimer = 9, Lily = 10
// Every id in this project — the replicated byte on ATracePlayerState, the argument to
// UTraceAbilityComponent::ServerSetCharacter, the number a console command takes — is that value.
// This file stores it as a uint8 rather than as the enum ONLY so that the character-select slice
// does not have to include the ability framework's header to draw a menu; the two are the same
// number and the static_asserts at the top of TraceCharacterRoster.cpp are where that agreement is asserted
// once the enum exists. Do not introduce a second numbering.
//
// SPEC v18 §2 TOOK THE ROSTER FROM FIVE TO EIGHT and SPEC v19 §3 TOOK IT TO TEN. Nothing in the shape
// of this file changed — still one table, still in id order, still Count entries — which is the reason
// each change was three constants and some rows rather than a migration. Per-team uniqueness still
// holds; with 5 players a side and 10 characters there is comfortable slack, and that is fine.

#pragma once

#include "CoreMinimal.h"
#include "Math/Color.h"

namespace TraceCharacterRoster
{
	/**
	 * ETraceCharacterId::None — the default characterless Mannequin.
	 *
	 * Who ends up here: mode A (frozen by spec v14 §2), every player when the "disable characters"
	 * setting is off, and anybody a full team roster cannot serve. NOT bots — this used to say "bots
	 * are always this", which spec v15 §2 reverses: bots pick like everybody else, they simply pick
	 * after the humans on their team and are assigned rather than asked.
	 */
	inline constexpr uint8 NoneId = 0;

	/** ETraceCharacterId::Rocco. */
	inline constexpr uint8 FirstId = 1;

	/**
	 * ETraceCharacterId::Lily — the LAST character, whichever one that currently is.
	 *
	 * *** THIS IS THE CONSTANT THAT MOVES EVERY TIME A CHARACTER IS ADDED. *** It was 5 (X) until
	 * spec v18 §2 appended Roxie, Elle and Slimeball, and 8 until spec v19 §3 appended Mortimer and
	 * Lily. Everything that walks the roster walks
	 * FirstId..LastId — the select screen, the asset loader, Trace.VerifyCharacterData, the player
	 * state's RPC validation — so leaving it behind does not produce a small bug, it produces a
	 * character nobody can pick and an asset table that is silently one row short (and therefore, by
	 * the all-or-none rule below, an entire roster that falls back to C++ values).
	 *
	 * The static_asserts at the top of TraceCharacterRoster.cpp turn exactly that mistake into a
	 * compile error. Do not delete them.
	 */
	inline constexpr uint8 LastId = 10;

	/** Selectable characters. NOT the size of the enum — None is not selectable. 8 -> 10 in v19 §3. */
	inline constexpr int32 Count = 10;

	/**
	 * One character's shipped description.
	 *
	 * The three ability strings are the spec's own words, compressed to fit a card. They are ASCII
	 * only and upper case on purpose: every string in this project's UI is drawn with the engine's
	 * built-in BITMAP fonts, whose glyph pages do not cover an em dash or a curly quote — those come
	 * out as a box or as nothing at all, and that has already bitten the title screen once.
	 */
	struct FTraceCharacterEntry
	{
		/** ETraceCharacterId as a uint8. */
		uint8 Id = NoneId;

		const TCHAR* Name = TEXT("");

		/** Spec §6 "Movement:". */
		const TCHAR* Movement = TEXT("");

		/** Spec §6 "Passive:". */
		const TCHAR* Passive = TEXT("");

		/** The activated ability's own name — Ripple, Chud, Spike, Pickler, Sting. */
		const TCHAR* ActivatedName = TEXT("");

		/** Spec §6 "Activated:". This is the one bound to E. */
		const TCHAR* Activated = TEXT("");

		/** Cooldown in seconds, for the card. The ability framework owns the number that is ENFORCED. */
		float ActivatedCooldown = 20.f;

		/**
		 * Card accent. Deliberately NOT a team colour: the select screen is the one place in this
		 * game where the two teams look identical (enemies may mirror your pick), so the cards have
		 * to be told apart by character rather than by side.
		 */
		FLinearColor Accent = FLinearColor::White;

		/**
		 * THE BODY EVERY OTHER PLAYER SEES — a LoadObject path to a USkeletalMesh, or EMPTY.
		 *
		 * EMPTY MEANS "EPIC'S MANNEQUIN", AND SINCE DEMO 29 ITEM 1 ALL TEN ROWS ARE EMPTY AGAIN.
		 * They named generated bodies between PIPELINE_DESIGN.md §9.1 and Demo 29; the ten strings are
		 * shelved in comments in TraceCharacterRoster.cpp, which also carries the measurement of why
		 * (a retarget bake that moved sixteen locomotion sequences' root travel into the pelvis). The
		 * Mannequin is not a degraded state — a character with no bespoke body is drawn exactly as
		 * this game has always drawn everybody — and it is also what an UNRESOLVABLE path falls back
		 * to, which is why this stayed a string: a clone that has not run
		 * Scripts/import-characters.sh must fall back rather than fail.
		 *
		 * SOFT BY CONSTRUCTION — it is a STRING here and a TSoftObjectPtr on the asset. A hard
		 * reference would make that clone fail to CONSTRUCT rather than fall back, which is the same
		 * reason ATraceCharacter::CharacterMeshAsset is soft. A path that does not resolve is
		 * reported through ETraceCharacterArtStatus, not crashed on.
		 */
		const TCHAR* BodyMeshPath = TEXT("");

		/**
		 * Yaw, in degrees, that turns THIS mesh's authored forward onto the actor's +X.
		 *
		 * *** MEASURED PER RIG, NOT COPIED — AND THE FACT THAT EVERY ROW READS -90 IS A RESULT,
		 * NOT A DEFAULT. *** Epic's Mannequin is authored facing +Y and therefore needs -90
		 * (TraceCharacterLayout::MeshYaw, and the default here); the ten generated bodies shelved by
		 * Demo 29 item 1 are authored facing +Y too (PIPELINE_DESIGN.md §3.1) and measured the same
		 * -90, which is why the rows keep the number while their mesh paths are empty — it is not
		 * READ while BodyMeshPath is empty, and it is the right answer either way. The rig that proves the
		 * rule is the one that is gone: the hand-modelled RoccoTest.fbx was authored facing +X and
		 * needed 0, and copying the Mannequin's -90 onto it turned him sideways — the single most
		 * likely way this feature looks broken. The number for a new mesh comes from measuring
		 * `Z x (LeftHand - RightHand)` on the imported asset (Scripts/import_characters.py's
		 * facing_heading(), which FAILS an import that measures outside its expected value); run on
		 * the Mannequin that formula reproduces -90, which is how the measurement is known to be right.
		 */
		float BodyMeshYaw = -90.f;

		/**
		 * THE ANIM BLUEPRINT CLASS THAT DRIVES BodyMesh — a LoadClass path ending in "_C", or EMPTY.
		 *
		 * EMPTY MEANS "WHATEVER ATraceCharacter::CharacterAnimClass RESOLVED TO", i.e. Epic's
		 * ABP_Unarmed, which is the right answer for every row whose BodyMeshPath is also empty.
		 *
		 * *** THIS FIELD EXISTS BECAUSE AN ANIM BLUEPRINT BELONGS TO A SKELETON, NOT TO A MESH. ***
		 * A row that names a body on a rig of its own MUST name an anim class built for that rig, or
		 * the pawn is drawn in its bind pose: an ABP is compiled against one SKELETON ASSET, and
		 * Epic's ABP_Unarmed is compiled against the Mannequin's, so it has nothing to move on any
		 * other rig. It is a per-SKELETON field rather than a per-character one, which is why all ten
		 * rows carried the SAME string while the generated bodies were wired: they share
		 * SK_TraceBody_Skeleton, so one retarget bake (Scripts/retarget_body.py) produced the one
		 * class they all named. Demo 29 item 1 shelved that string with the mesh paths — the two are
		 * shelved and restored together, because either one alone is a bug.
		 *
		 * SOFT BY CONSTRUCTION, exactly like BodyMeshPath, and unresolvable for the same reason: a
		 * clone that has not run the import and the retarget does not have it. ApplyBodyAnimInstance
		 * checks the class's target skeleton against the mesh's before handing it over, so a stale
		 * path costs the pawn its animation and nothing else.
		 */
		const TCHAR* BodyAnimClassPath = TEXT("");
	};

	/**
	 * The table, in the doc's order: Rocco, Chut, Mace, Oyster, X, Roxie, Elle, Slimeball, Mortimer,
	 * Lily. Always Count entries.
	 *
	 * *** SINCE SPEC v17 §5 THIS MAY BE SERVED FROM ASSETS. *** See the "TWO SOURCES" block at the
	 * bottom of this header. Nothing about the shape changed: still Count entries, still in id
	 * order, still TCHAR pointers with process lifetime, so every caller is unaffected.
	 */
	TRACE_API const TArray<FTraceCharacterEntry>& All();

	/** Null for NoneId and for anything out of range. */
	TRACE_API const FTraceCharacterEntry* Find(uint8 Id);

	/** True for FirstId..LastId. NoneId is deliberately NOT valid — "no character" is not a character. */
	TRACE_API bool IsValidId(uint8 Id);

	/** "ROCCO" / ... / "MANNEQUIN" for NoneId, so a log line never prints a bare number. */
	TRACE_API FString NameFor(uint8 Id);

	// =============================================================================================
	// TWO SOURCES, ONE SHAPE — spec v17 §5, and rule §0.1 (opt-in, with a LIVE C++ fallback)
	// =============================================================================================
	//
	// The characters now also exist as UTraceCharacterDefinition assets under
	// /Game/Trace/Data/Characters, generated from the C++ table below by
	// Scripts/generate-data-assets.py. All() serves the ASSETS when ALL of them load and validate,
	// and the C++ table otherwise — asset missing, asset corrupt, an id out of place, or toggle off.
	//
	// IT IS ALL OF THEM OR NONE, deliberately. A roster that was seven assets and one C++ row would
	// be a half-migration: the select screen would look right and nobody could say which row came
	// from where. One decision, logged once, at the top of the log.
	//
	// *** THIS IS THE TRAP SPEC v18 §2 WALKS INTO. *** Adding a character adds a row to the C++ table
	// and a NEW asset that does not exist yet, so between those two commits the roster is 7-of-8 and
	// EVERY character silently reverts to its C++ values. It is not a crash and it is not a visible
	// difference (the assets are generated from the C++ table, so the two agree) — but it means the
	// asset path has stopped being exercised. Re-run Scripts/generate-data-assets.py in the SAME
	// change that adds the enumerator, and read the verdict from Trace.VerifyCharacterData.
	//
	// THE TOGGLE:  Trace.Data.UseCharacterAssets  0|1   (default 1; 0 forces the C++ table)
	// THE PROOF:   Trace.VerifyCharacterData            (field-by-field, and it can go red)
	// THE DUMP:    Trace.Data.DumpCharacters            (says which source, and prints every row)
	//
	// THE NUMBERS THE ASSETS DO NOT HOLD: everything UTraceSettings holds today still lives there
	// and only there. The assets carry a character's identity and the words on its card. See
	// Data/TraceCharacterDefinition.h for the full statement of who owns what.

	/** Which table All() is serving right now. */
	enum class ESource : uint8
	{
		/** The table compiled into TraceCharacterRoster.cpp. Always available. */
		CppTable = 0,

		/** The Count UTraceCharacterDefinition assets, all of them or none. */
		Assets = 1
	};

	/** Resolves the source on the first call, exactly as All() does. */
	TRACE_API ESource CurrentSource();

	/** "C++ table" / "assets (/Game/Trace/Data/Characters)". For logs and the HUD's dev overlay. */
	TRACE_API const TCHAR* CurrentSourceName();

	/**
	 * THE C++ TABLE, ALWAYS — whatever All() is currently serving.
	 *
	 * This is what makes rule §0.1 checkable rather than asserted: the verifier compares the loaded
	 * assets against this, and the fallback is not a code path that only runs when something is
	 * broken, it is a table that is always there and always readable.
	 */
	TRACE_API const TArray<FTraceCharacterEntry>& CppFallbackTable();

	/**
	 * Drop the resolved table so the next All() decides again. Called by the console commands and by
	 * the generator's verification pass; there is no reason for gameplay code to call it.
	 */
	TRACE_API void ForceReload();
}
