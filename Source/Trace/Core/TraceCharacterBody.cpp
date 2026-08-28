// Trace — what a Trace character IS MADE OF: the Mannequin dress-up, the per-character body mesh
// and its retargeted animation, and the team colours painted onto both. See TraceCharacter.h for
// the pawn and Core/TraceCharacter.cpp for everything it DOES.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT. RESTRUCTURE tranche D3 moved this block out of
// TraceCharacter.cpp unchanged. The audit's argument for the cut is the same one that makes it
// safe: NOTHING IN THE GAMEPLAY PATH READS ANY OF IT. Hitscan, lag compensation and the melee arc
// all reason about the CAPSULE — contract §7, restated on ConfigureVisualMesh in the other file —
// so which mesh is drawn, which anim blueprint is playing and what colour the shell is cannot move
// a bullet. A reader chasing a hit-registration bug never has to open this file, and a reader
// changing how Rocco looks cannot accidentally change where he can be shot.
//
// Everything here is a member of ATraceCharacter, or a helper private to this file. The state
// (BodyMeshYaw, AppliedBodyCharacterId, the MIDs, the team-colour retry timer) still lives on the
// one pawn and these functions still reach it directly: the split costs nothing in access and adds
// no indirection. The constant tables both files read are in Core/TraceCharacterInternal.h.
//
// FOUR THINGS LIVE HERE, in the order they run for a pawn:
//   1. the ART-AVAILABILITY check, which answers "why am I looking at capsules" BEFORE any pawn
//      exists, and the process-wide status the HUD banner draws from;
//   2. SetupCharacterVisuals(), which dresses every pawn in Epic's Mannequin at
//      PostInitializeComponents() — far too early to know WHO it is;
//   3. the PER-CHARACTER BODY, which is the answer to that, applied later from the events that
//      carry the character id, plus the bone-name translation the rest of the game asks for;
//   4. the TEAM COLOURS, which paint whichever of those two bodies ended up drawn.

#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterInternal.h"      // the measured layout/asset constant tables

#include "Animation/AnimClassInterface.h"     // GetTargetSkeleton(): does this ABP fit this rig?
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"                 // DoesPackageExist (the art first-run check)
#include "Misc/Parse.h"
#include "TimerManager.h"                     // the team-colour retry timer

#include "Core/TraceCharacterRoster.h"        // the per-character body mesh path and its yaw
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"               // IsCarrier(): the carrier wears white
#include "Gameplay/TraceWeaponComponent.h"     // the knife the third-person body has to hold
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// Character art availability — the "why am I looking at capsules" answer
// =================================================================================================
//
// A bug report arrived asking for the human character models that were already implemented. The
// models were fine; the REPORTER'S MACHINE had never run Scripts/import-mannequin.sh, and the only
// evidence of that was a single Warning line, emitted after the pawn had already spawned, in a log
// that nobody reads while playtesting. The models are not the defect. The SILENT DEGRADE is.
//
// Three things therefore happen now, and all three are needed:
//   1. the check runs BEFORE any pawn exists (VerifyCharacterArtInstalled, from ATraceHUD::BeginPlay),
//      by asking the package store rather than by loading anything;
//   2. it logs a boxed banner at Error verbosity — Warning has been suppressed in this project's
//      logs before, and this is the one message that must survive that;
//   3. the status is queryable, so ATraceHUD can keep a warning on screen for the whole session.
namespace
{
	/** Process-wide, because the answer is a property of the INSTALL, not of any one pawn. */
	ETraceCharacterArtStatus GCharacterArtStatus = ETraceCharacterArtStatus::Unknown;

	/** So the banner is printed once per distinct status rather than once per pawn (ten per match). */
	ETraceCharacterArtStatus GLoggedCharacterArtStatus = ETraceCharacterArtStatus::Unknown;

	/**
	 * "ROCCO (/Game/Trace/Characters/Rocco/SK_Rocco.SK_Rocco)" — WHICH character's body is missing and
	 * where we looked. The enum has no room for a payload and "a character mesh is missing" without
	 * saying which one is a message that gets re-read rather than acted on. Written by
	 * TraceCharacterBody::ReportMissingBodyMesh immediately before it reports the status.
	 */
	FString GMissingBodyMeshDetail;

	/** Same idea, for the anim class: "ROCCO: <path> did not load." Filled by ReportMissingBodyAnim. */
	FString GMissingBodyAnimDetail;

	/** "/Game/.../SKM_Manny_Simple.SKM_Manny_Simple" -> "/Game/.../SKM_Manny_Simple". */
	FString PackageNameOf(const TCHAR* ObjectPath)
	{
		return FPackageName::ObjectPathToPackageName(FString(ObjectPath));
	}

	/**
	 * Records the status and, on a CHANGE, says so at a verbosity that cannot be missed.
	 *
	 * Error rather than Warning for the two broken states. That is not shouting for its own sake:
	 * this project has twice concluded a working mechanic was dead because its log line was filtered,
	 * and the whole purpose of this message is to be the thing that is still visible when everything
	 * else has been turned down.
	 */
	void ReportCharacterArtStatus(ETraceCharacterArtStatus NewStatus)
	{
		// *** ONE PAWN'S GOOD NEWS DOES NOT CANCEL ANOTHER'S BAD NEWS. ***
		// Every pawn reports as it dresses, and until per-character bodies landed all ten of them were
		// answering the same question — "is the Mannequin installed" — so the last writer was always
		// right. CharacterBodyMeshMissing is about ONE character, so the nine Mannequin pawns that
		// dress after it would each report Ok and wipe the warning off the screen. Nothing else is
		// suppressed: a broken Mannequin install (MeshMissing) is more fundamental than a missing
		// body and still overwrites this, because it is true of every pawn including that one.
		if (NewStatus == ETraceCharacterArtStatus::Ok
			&& (GCharacterArtStatus == ETraceCharacterArtStatus::CharacterBodyMeshMissing
				|| GCharacterArtStatus == ETraceCharacterArtStatus::CharacterBodyAnimMissing))
		{
			return;
		}

		GCharacterArtStatus = NewStatus;
		if (GLoggedCharacterArtStatus == NewStatus)
		{
			return;
		}
		GLoggedCharacterArtStatus = NewStatus;

		switch (NewStatus)
		{
		case ETraceCharacterArtStatus::Ok:
			UE_LOG(LogTraceGame, Display,
				TEXT("[CharacterArt] OK — %s and %s both resolved. Characters are Epic's Mannequin with ")
				TEXT("head, limbs and the ABP_Unarmed run cycle."),
				TraceCharacterAssets::MannequinMesh, TraceCharacterAssets::UnarmedAnimClass);
			break;

		case ETraceCharacterArtStatus::MeshMissing:
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			UE_LOG(LogTraceGame, Error, TEXT("  CHARACTER ART IS NOT INSTALLED ON THIS MACHINE."));
			UE_LOG(LogTraceGame, Error, TEXT("  Every player will be drawn as a coloured capsule with a ball"));
			UE_LOG(LogTraceGame, Error, TEXT("  for a head, with NO running animation. This is a missing"));
			UE_LOG(LogTraceGame, Error, TEXT("  import, NOT the intended look of the game."));
			UE_LOG(LogTraceGame, Error, TEXT(""));
			UE_LOG(LogTraceGame, Error, TEXT("  FIX IT WITH ONE COMMAND, from the project root:"));
			UE_LOG(LogTraceGame, Error, TEXT("      %s"), TraceCharacterAssets::ImportCommand);
			UE_LOG(LogTraceGame, Error, TEXT(""));
			UE_LOG(LogTraceGame, Error, TEXT("  It copies Epic's Mannequin out of YOUR OWN UE 5.8 install into"));
			UE_LOG(LogTraceGame, Error, TEXT("  Content/Characters/Mannequins. The art is gitignored on"));
			UE_LOG(LogTraceGame, Error, TEXT("  purpose (126 MB of binaries), so a fresh clone always needs"));
			UE_LOG(LogTraceGame, Error, TEXT("  this once. Missing package: %s"), *PackageNameOf(TraceCharacterAssets::MannequinMesh));
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			break;

		case ETraceCharacterArtStatus::AnimMissing:
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			UE_LOG(LogTraceGame, Error, TEXT("  CHARACTER ART IS ONLY HALF INSTALLED."));
			UE_LOG(LogTraceGame, Error, TEXT("  The Mannequin mesh loaded but %s did not, so characters will"), TraceCharacterAssets::UnarmedAnimClass);
			UE_LOG(LogTraceGame, Error, TEXT("  stand in a fixed pose and never run."));
			UE_LOG(LogTraceGame, Error, TEXT("  FIX:  %s --force"), TraceCharacterAssets::ImportCommand);
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			break;

		case ETraceCharacterArtStatus::CharacterBodyMeshMissing:
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			UE_LOG(LogTraceGame, Error, TEXT("  A CHARACTER'S OWN BODY MESH IS NOT IMPORTED ON THIS MACHINE."));
			UE_LOG(LogTraceGame, Error, TEXT("  %s"), *GMissingBodyMeshDetail);
			UE_LOG(LogTraceGame, Error, TEXT("  That character's players are drawn as the MANNEQUIN instead."));
			UE_LOG(LogTraceGame, Error, TEXT("  Everybody else is unaffected and the match is playable."));
			UE_LOG(LogTraceGame, Error, TEXT(""));
			UE_LOG(LogTraceGame, Error, TEXT("  FIX IT, from the project root:"));
			UE_LOG(LogTraceGame, Error, TEXT("      ./Scripts/import-rocco.sh"));
			UE_LOG(LogTraceGame, Error, TEXT("  Character art is gitignored/LFS by design, so a fresh clone"));
			UE_LOG(LogTraceGame, Error, TEXT("  needs the import once."));
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			break;

		case ETraceCharacterArtStatus::CharacterBodyAnimMissing:
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			UE_LOG(LogTraceGame, Error, TEXT("  A CHARACTER'S BODY IS IMPORTED BUT NOT RETARGETED."));
			UE_LOG(LogTraceGame, Error, TEXT("  %s"), *GMissingBodyAnimDetail);
			UE_LOG(LogTraceGame, Error, TEXT("  That character's players are the right shape and the right"));
			UE_LOG(LogTraceGame, Error, TEXT("  size, and are FROZEN IN THEIR BIND POSE — arms out, sliding"));
			UE_LOG(LogTraceGame, Error, TEXT("  around the arena. Nothing else is affected, and a still"));
			UE_LOG(LogTraceGame, Error, TEXT("  screenshot of one standing looks entirely correct, which is"));
			UE_LOG(LogTraceGame, Error, TEXT("  exactly why this says so at Error."));
			UE_LOG(LogTraceGame, Error, TEXT(""));
			UE_LOG(LogTraceGame, Error, TEXT("  FIX IT, from the project root:"));
			UE_LOG(LogTraceGame, Error, TEXT("      ./Scripts/retarget-rocco.sh"));
			UE_LOG(LogTraceGame, Error, TEXT("  An anim blueprint is compiled against a SKELETON, so a body"));
			UE_LOG(LogTraceGame, Error, TEXT("  on a rig of its own needs its own retargeted copy of"));
			UE_LOG(LogTraceGame, Error, TEXT("  ABP_Unarmed. That script bakes it."));
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			break;

		case ETraceCharacterArtStatus::DisabledByCommandLine:
			UE_LOG(LogTraceGame, Display,
				TEXT("[CharacterArt] -TraceNoCharacterArt: the fallback primitives are being shown ON PURPOSE. ")
				TEXT("Relaunch without the switch for the Mannequin."));
			break;

		case ETraceCharacterArtStatus::NotRequired:
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterArt] Dedicated server: no character art is loaded or needed."));
			break;

		default:
			break;
		}
	}
}

void ATraceCharacter::VerifyCharacterArtInstalled()
{
	// Idempotent: the first caller decides, everybody after it is free. SetupCharacterVisuals() may
	// have got here first with a definitive answer (it actually tried to LOAD the assets, which beats
	// a package-store lookup), and must not be second-guessed by a cheaper test.
	if (GCharacterArtStatus != ETraceCharacterArtStatus::Unknown)
	{
		return;
	}

	if (IsRunningDedicatedServer())
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::NotRequired);
		return;
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoCharacterArt")))
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::DisabledByCommandLine);
		return;
	}

	// DoesPackageExist, not LoadObject: this runs during BeginPlay on the HUD, before the match has
	// drawn a frame, and the whole point is to answer the question without paying 126 MB for it.
	// Works in a cooked build too — the package store knows what was cooked in.
	const bool bMeshPresent = FPackageName::DoesPackageExist(PackageNameOf(TraceCharacterAssets::MannequinMesh));
	const bool bAnimPresent = FPackageName::DoesPackageExist(PackageNameOf(TraceCharacterAssets::UnarmedAnimClass));

	if (!bMeshPresent)
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::MeshMissing);
	}
	else if (!bAnimPresent)
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::AnimMissing);
	}
	else
	{
		// Deliberately NOT reported as Ok here. Both packages exist, but "exists" is not "loads and
		// has a skeleton": SetupCharacterVisuals() is the only thing that knows that, and it will set
		// the real status the moment the first pawn is dressed. Leaving it Unknown keeps the on-screen
		// warning off (Unknown is not a warned state) without claiming a success nobody has verified.
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterArt] Pre-flight OK: both Mannequin packages are present on disk. ")
			TEXT("Confirming on the first spawned character."));
	}
}

ETraceCharacterArtStatus ATraceCharacter::GetCharacterArtStatus()
{
	if (GCharacterArtStatus == ETraceCharacterArtStatus::Unknown)
	{
		VerifyCharacterArtInstalled();
	}
	return GCharacterArtStatus;
}

bool ATraceCharacter::GetCharacterArtWarning(FString& OutHeadline, FString& OutDetail)
{
	switch (GetCharacterArtStatus())
	{
	case ETraceCharacterArtStatus::MeshMissing:
		OutHeadline = TEXT("CHARACTER ART NOT INSTALLED");
		OutDetail = FString::Printf(TEXT("Players are placeholder shapes. Run  %s  from the project root, then relaunch."),
			TraceCharacterAssets::ImportCommand);
		return true;

	case ETraceCharacterArtStatus::CharacterBodyMeshMissing:
		OutHeadline = TEXT("CHARACTER BODY MESH NOT IMPORTED");
		OutDetail = FString::Printf(
			TEXT("%s is drawn as the Mannequin. Run  ./Scripts/import-rocco.sh  from the project root, then relaunch."),
			*GMissingBodyMeshDetail);
		return true;

	case ETraceCharacterArtStatus::CharacterBodyAnimMissing:
		OutHeadline = TEXT("CHARACTER BODY NOT RETARGETED");
		OutDetail = FString::Printf(
			TEXT("%s is frozen in its bind pose. Run  ./Scripts/retarget-rocco.sh  from the project root, then relaunch."),
			*GMissingBodyAnimDetail);
		return true;

	case ETraceCharacterArtStatus::AnimMissing:
		OutHeadline = TEXT("CHARACTER ANIMATIONS NOT INSTALLED");
		OutDetail = FString::Printf(TEXT("Players are posed but never move. Run  %s --force, then relaunch."),
			TraceCharacterAssets::ImportCommand);
		return true;

	case ETraceCharacterArtStatus::DisabledByCommandLine:
		// WARNED TOO, and that is the whole reason -TraceNoCharacterArt exists. The switch is there so
		// the fallback branch can be exercised on a machine where the import HAS been run; if it
		// exercised everything about the fallback EXCEPT the warning, the warning would be the one
		// part of this fix that no automated run could ever photograph. Different wording, so nobody
		// mistakes a deliberate test for a broken install.
		OutHeadline = TEXT("CHARACTER ART DISABLED (-TraceNoCharacterArt)");
		OutDetail = TEXT("This is the simulated missing-import state. Relaunch without the switch for the Mannequin.");
		return true;

	default:
		// Ok, Unknown and NotRequired draw nothing.
		OutHeadline.Reset();
		OutDetail.Reset();
		return false;
	}
}

namespace
{
	// MOVED WITH ITS ONLY CALLER (RESTRUCTURE D3). This clamp used to sit in TraceCharacter.cpp's
	// helper namespace beside ResolveAimRotation and ConfigureVisualMesh; every one of its three call
	// sites is in ApplyTeamColors() below, so it came here rather than being shared across two files
	// for no reader's benefit.

	FLinearColor SanitizeTint(const FLinearColor& InColor)
	{
		return FLinearColor(
			FMath::Clamp(InColor.R, 0.f, 1.f),
			FMath::Clamp(InColor.G, 0.f, 1.f),
			FMath::Clamp(InColor.B, 0.f, 1.f),
			1.f);
	}
}

// =================================================================================================
// Dressing a pawn: Epic's Mannequin, applied before anybody knows who this is
// =================================================================================================

void ATraceCharacter::SetupCharacterVisuals()
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp == nullptr)
	{
		return;
	}

	// A dedicated server renders nothing and never builds a pose. Loading 126 MB of character art
	// there would be pure cost — and the capsule, which is what the server actually simulates and
	// tests against, is unaffected either way.
	if (IsRunningDedicatedServer())
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::NotRequired);
		MeshComp->SetVisibility(false);
		return;
	}

	// -TraceNoCharacterArt exists so the not-imported path can actually be TESTED, on a machine where
	// the import has been run, without deleting anyone's Content folder out from under a parallel
	// session. The fallback is the branch every fresh clone takes; leaving it unexercised until a
	// new hire hits it is how "optional asset" handling quietly rots.
	const bool bForceNoArt = FParse::Param(FCommandLine::Get(), TEXT("TraceNoCharacterArt"));

	// LoadSynchronous on a soft pointer: the first pawn pays for the load, every pawn after it gets
	// the already-resident object back. Ten characters spawn at match start, so this happens once.
	USkeletalMesh* LoadedMesh = (bForceNoArt || CharacterMeshAsset.IsNull())
		? nullptr
		: CharacterMeshAsset.LoadSynchronous();

	if (LoadedMesh == nullptr)
	{
		// The whole point of the soft reference. No crash, no invisible player: show the primitives
		// and say exactly what to run. Banner-logged once per process rather than once per pawn,
		// because ten identical screenfuls of this would bury everything else in the log — and
		// mirrored onto the HUD by ATraceHUD::DrawArtWarning(), because a log line demonstrably was
		// not enough (this is the bug that got reported as "the character models were not replaced").
		ReportCharacterArtStatus(bForceNoArt
			? ETraceCharacterArtStatus::DisabledByCommandLine
			: ETraceCharacterArtStatus::MeshMissing);

		static bool bWarnedMissingMesh = false;
		if (!bWarnedMissingMesh)
		{
			bWarnedMissingMesh = true;
			UE_LOG(LogTraceGame, Warning, TEXT("%s (looked for %s)"),
				TraceCharacterAssets::MissingImportHint, TraceCharacterAssets::MannequinMesh);
		}

		bUsingSkeletalMesh = false;
		MeshComp->SetVisibility(false);
		if (FallbackBodyMesh != nullptr) { FallbackBodyMesh->SetVisibility(true); }
		if (FallbackHeadMesh != nullptr) { FallbackHeadMesh->SetVisibility(true); }
		ApplyTeamColors();
		return;
	}

	MeshComp->SetSkeletalMeshAsset(LoadedMesh);
	MeshComp->SetVisibility(true);

	bUsingSkeletalMesh = true;
	if (FallbackBodyMesh != nullptr) { FallbackBodyMesh->SetVisibility(false); }
	if (FallbackHeadMesh != nullptr) { FallbackHeadMesh->SetVisibility(false); }

	// The anim blueprint is loaded separately and is separately optional: a character with a mesh
	// but no anim instance is a T-pose, which is ugly but still playable and still correctly
	// team-coloured. That is a better failure than refusing to draw the player.
	UClass* LoadedAnimClass = (bForceNoArt || CharacterAnimClass.IsNull())
		? nullptr
		: CharacterAnimClass.LoadSynchronous();
	if (LoadedAnimClass != nullptr)
	{
		MeshComp->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		MeshComp->SetAnimInstanceClass(LoadedAnimClass);

		// The ONLY place in the codebase that can honestly say the art is fine: both objects loaded,
		// the mesh is attached and the anim blueprint is instanced. Everything else is a prediction.
		ReportCharacterArtStatus(ETraceCharacterArtStatus::Ok);
	}
	else
	{
		ReportCharacterArtStatus(bForceNoArt
			? ETraceCharacterArtStatus::DisabledByCommandLine
			: ETraceCharacterArtStatus::AnimMissing);

		static bool bWarnedMissingAnim = false;
		if (!bWarnedMissingAnim)
		{
			bWarnedMissingAnim = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("Character mesh loaded but its anim blueprint did not (%s). Characters will be ")
				TEXT("posed but not animated; re-run Scripts/import-mannequin.sh --force."),
				TraceCharacterAssets::UnarmedAnimClass);
		}
	}

	// Slot count is only knowable once the mesh is attached, so the MIDs are built from here.
	ApplyTeamColors();
}

// =================================================================================================
// THE PER-CHARACTER BODY — what OTHER players see when somebody picks Rocco
// =================================================================================================
//
// SetupCharacterVisuals() above dresses every pawn in the Mannequin. It runs from
// PostInitializeComponents(), which is far too early to know WHO this pawn is: the character id is
// not on the pawn, it is forwarded by ATracePlayerState from a UTraceAbilityComponent that is
// attached to the PLAYERSTATE — a third actor-ish object that replicates on its own schedule. So on
// any machine that did not spawn this pawn there are three independent arrivals (pawn, player state,
// ability component) and no guaranteed order between them.
//
// Everything below exists to make that ordering irrelevant. See UpdateCharacterBodyMesh()'s
// declaration for the argument in full; the short version is that this is a POLL with a remembered
// answer, re-asserted from every path that could learn something new, and free when nothing did.
//
// THE THREE THINGS A BODY SWAP HAS TO CARRY WITH IT, none of which are the mesh asset:
//   1. THE YAW, AND IT IS MEASURED PER RIG RATHER THAN ASSUMED. Epic's Mannequin is authored facing
//      +Y and needs -90; so are the ten generated bodies (PIPELINE_DESIGN.md §3.1), so they take the
//      same -90 — but they take it because import_characters.py MEASURED it on each imported asset,
//      not because it was copied. The proof that copying is the wrong habit is the rig that used to
//      be in this row: the hand-modelled RoccoTest.fbx was authored facing +X and needed 0, and the
//      Mannequin's number on it stood him sideways to the direction he was shooting.
//   2. THE ANIM BLUEPRINT, WHICH IS PER SKELETON — NOT PER CHARACTER, AND THE DIFFERENCE IS THE
//      WHOLE POINT OF THE SHARED RIG. An anim blueprint is compiled against a SKELETON asset, so a
//      body on a rig of its own cannot run ABP_Unarmed and is drawn frozen in its bind pose. All ten
//      generated bodies are bound to ONE skeleton (SK_TraceBody_Skeleton, Mannequin bone names,
//      identical joint positions), so ONE retarget bake serves all ten and every roster row names the
//      same class, ABP_Unarmed_Body. The compatibility check stays anyway: a body that is the right
//      shape and frozen is invisible to a screenshot.
//   3. THE THINGS BOLTED TO BONES. The third-person knife hangs off "hand_r" — which the generated
//      rigs carry precisely because §3.2 chose Mannequin names over the old FBX's suffixed ones, and
//      which the Mannequin has too. UTraceWeaponComponent is told to rebuild against the new body.
namespace TraceCharacterBody
{
	static_assert(TraceCharacterLayout::MeshYaw == -90.f,
		"ATraceCharacter::BodyMeshYaw's header default is written as the literal -90 because "
		"TraceCharacterLayout is PRIVATE to this .cpp family - it now lives in TraceCharacterInternal.h, "
		"which TraceCharacter.h deliberately does not include, because a public header that dragged the "
		"whole constant table behind it would recompile the module on every measurement change. So the "
		"literal stays and THIS ASSERT is what keeps the two honest: if MeshYaw moves, that default "
		"moves with it.");
	static_assert(TraceCharacterRoster::NoneId == 0,
		"ATraceCharacter::AppliedBodyCharacterId's header default is written as the literal 0 for the "
		"same reason. If NoneId moves, that default moves with it.");

	/**
	 * ONE RIG'S NAME FOR A MANNEQUIN BONE.
	 *
	 * Not a per-character table, deliberately: this is a property of the SKELETON, several characters
	 * may share one rig, and the lookup is "ask the mesh which of these names it actually has", which
	 * cannot be wrong about a mesh it is holding. Adding a rig means adding its names to a row.
	 *
	 * THE TRAILING "1" IS NOT A TYPO. RoccoTest.fbx ships TWO armatures using the same 24 bone names,
	 * so the FBX translator uniquified the second one it reached — every bone on the imported skeleton
	 * is "RightHand1", "Hips1", "Head1". It is deterministic (it follows the file's node order) and it
	 * is what the asset on disk actually contains. The un-suffixed spellings are listed beside them so
	 * a re-export with a single armature keeps working without a code change.
	 */
	struct FBodyBoneAliases
	{
		/** What the game asks for, in the Mannequin's vocabulary. */
		const TCHAR* MannequinName;

		/** Everything else that has ever meant the same joint, most likely first. Null-terminated. */
		const TCHAR* Aliases[4];
	};

	const FBodyBoneAliases BoneAliasTable[] =
	{
		{ TEXT("hand_r"),     { TEXT("RightHand1"),     TEXT("RightHand"),     TEXT("mixamorig:RightHand"),     nullptr } },
		{ TEXT("hand_l"),     { TEXT("LeftHand1"),      TEXT("LeftHand"),      TEXT("mixamorig:LeftHand"),      nullptr } },
		{ TEXT("lowerarm_r"), { TEXT("RightForeArm1"),  TEXT("RightForeArm"),  TEXT("mixamorig:RightForeArm"),  nullptr } },
		{ TEXT("lowerarm_l"), { TEXT("LeftForeArm1"),   TEXT("LeftForeArm"),   TEXT("mixamorig:LeftForeArm"),   nullptr } },
		{ TEXT("foot_r"),     { TEXT("RightFoot1"),     TEXT("RightFoot"),     TEXT("mixamorig:RightFoot"),     nullptr } },
		{ TEXT("foot_l"),     { TEXT("LeftFoot1"),      TEXT("LeftFoot"),      TEXT("mixamorig:LeftFoot"),      nullptr } },
		{ TEXT("head"),       { TEXT("Head1"),          TEXT("Head"),          TEXT("mixamorig:Head"),          nullptr } },
		{ TEXT("pelvis"),     { TEXT("Hips1"),          TEXT("Hips"),          TEXT("mixamorig:Hips"),          nullptr } },
	};

	/**
	 * @return the name @p MeshComp actually has for @p Wanted, or NAME_None if this rig has no
	 *         equivalent at all. Asks the mesh rather than the character, so it is correct for
	 *         whatever is on the component at the moment it is called.
	 */
	FName ResolveBoneName(const USkeletalMeshComponent* MeshComp, FName Wanted)
	{
		if (MeshComp == nullptr || Wanted.IsNone())
		{
			return NAME_None;
		}

		// The Mannequin, and the common case: one lookup and out. A mesh component with no skeletal
		// mesh at all (the not-imported fallback) answers false here and falls through to answer None,
		// which is what every caller already guards for.
		if (MeshComp->DoesSocketExist(Wanted))
		{
			return Wanted;
		}

		for (const FBodyBoneAliases& Row : BoneAliasTable)
		{
			if (Wanted != FName(Row.MannequinName))
			{
				continue;
			}

			for (const TCHAR* const Alias : Row.Aliases)
			{
				if (Alias == nullptr)
				{
					break;
				}
				const FName AliasName(Alias);
				if (MeshComp->DoesSocketExist(AliasName))
				{
					return AliasName;
				}
			}
			break;   // the Mannequin name appears once; no other row can match
		}

		return NAME_None;
	}

	/** Fills the banner's payload and reports, so a missing body says WHICH body. Once per process. */
	void ReportMissingBodyMesh(uint8 CharacterId, const FString& MeshPath)
	{
		GMissingBodyMeshDetail = FString::Printf(TEXT("%s: %s did not load."),
			*TraceCharacterRoster::NameFor(CharacterId), *MeshPath);
		ReportCharacterArtStatus(ETraceCharacterArtStatus::CharacterBodyMeshMissing);
	}

	/** The same, for the anim class that was supposed to drive that body. */
	void ReportMissingBodyAnim(uint8 CharacterId, const FString& Detail)
	{
		GMissingBodyAnimDetail = FString::Printf(TEXT("%s: %s"),
			*TraceCharacterRoster::NameFor(CharacterId), *Detail);
		ReportCharacterArtStatus(ETraceCharacterArtStatus::CharacterBodyAnimMissing);
	}

#if !UE_BUILD_SHIPPING
	/**
	 * *** THE RED ARM FOR Trace.Characters.BodyMesh, AND IT IS A REAL BUG, NOT A MUTILATION. ***
	 *
	 * At 1 the per-character body is applied ONLY from the lifecycle events — BeginPlay,
	 * PossessedBy, OnRep_PlayerState — and never from the poll. That is not a broken version of this
	 * feature, it is the OBVIOUS version of it, the one a reasonable person writes first and the one
	 * this project's own history says gets written: hook the events, apply once, done.
	 *
	 * It fails because THE CHARACTER ID DOES NOT ARRIVE WITH ANY OF THOSE EVENTS. It arrives on a
	 * UTraceAbilityComponent that replicates separately from the PlayerState, which replicates
	 * separately from the pawn — and it can land tens of seconds later than all three: a player who
	 * lets the select screen time out is auto-assigned at the deadline, twenty-five seconds after
	 * their pawn spawned (measured, in a listen-server run). Every hook has long since fired against
	 * "no character", so the body stays a Mannequin for the rest of the match.
	 *
	 * MEASURED, so the claim is the one the run makes and not the one that sounded right: with this
	 * at 1 on a connecting client, Trace.Characters.BodyMesh reported
	 *     FAIL TraceCharacter_0  role=Autonomous  says=ROCCO  applied=MANNEQUIN  draws=SKM_Manny_Simple
	 *     RESULT: *** FAIL *** - 1 pawn(s) are drawing a body their character did not ask for
	 * and the same run with it at 0 reported PASS for all ten, INCLUDING a role=Simulated pawn drawing
	 * SK_Rocco. Which pawn goes red depends on the ordering that machine happened to get — the rule is
	 * "whichever pawn learns its character after the last hook fired", not "the remote ones".
	 * A harness whose red arm and green arm agree is not measuring its rule.
	 */
	/**
	 * *** THE RED ARM FOR THE ANIM HALF OF Trace.Characters.BodyMesh, AND IT IS THE STATE THIS
	 * FEATURE WAS ACTUALLY IN. ***
	 *
	 * At 1, ApplyBodyAnimInstance ignores the character's own anim class and offers every pawn
	 * CharacterAnimClass — Epic's ABP_Unarmed — exactly as it did before this pass. On a Mannequin
	 * that changes nothing. On Rocco it reproduces the reported bug precisely: the class is bound to
	 * a skeleton whose bones SK_Rocco does not have, the compatibility check rejects it, and the pawn
	 * is drawn in its BIND POSE, gliding.
	 *
	 * It is here because the anim half of this feature is the half a screenshot cannot fail. A
	 * standing Rocco in a bind pose and a standing Rocco mid-idle are the same picture; the
	 * difference is only visible in motion, or in a harness that reads the anim class off the
	 * component. So the harness reads it — and this switch is how the harness is proved to be
	 * capable of going red at all.
	 */
	TAutoConsoleVariable<int32> CVarBodyAnimIgnore(
		TEXT("Trace.Characters.BodyAnimIgnore"),
		0,
		TEXT("Trace, dev only. 1 = ignore each character's own retargeted anim class and offer every pawn\n")
		TEXT("Epic's ABP_Unarmed, as this game did before the retarget existed. This is the RED ARM: it puts\n")
		TEXT("Rocco back in his bind pose, and Trace.Characters.BodyMesh must report FAIL while it is on."),
		ECVF_Cheat);

	TAutoConsoleVariable<int32> CVarBodyMeshEventsOnly(
		TEXT("Trace.Characters.BodyMeshEventsOnly"),
		0,
		TEXT("Trace, dev only. 1 = apply the per-character body ONLY from BeginPlay/PossessedBy/OnRep_PlayerState\n")
		TEXT("and never from the per-frame re-assert. This is the RED ARM: it restores the late-arrival bug,\n")
		TEXT("and Trace.Characters.BodyMesh must report FAIL on remotely-replicated pawns while it is on."),
		ECVF_Cheat);

	/**
	 * *** THE RED ARM FOR THE MATERIAL HALF OF Trace.Characters.BodyMesh, AND IT IS THE BUG THIS
	 * PASS FIXED. ***
	 *
	 * At 1, ApplyCharacterBodyMesh swaps the mesh WITHOUT dropping the component's material
	 * overrides — exactly what the code did before, and exactly what a reasonable person writes,
	 * because "set the mesh" sounds total and the overrides are invisible in the header of the
	 * component. They are not cleared by the engine: USkinnedMeshComponent::SetSkinnedAssetAndUpdate
	 * clears skin-weight profiles and morph targets and nothing else, and an override is returned in
	 * preference to the asset's own material for that slot.
	 *
	 * The result is per-slot and partial, which is why it survived three reviews: the Mannequin has
	 * two slots and SK_Rocco has ten, so Manny -> Rocco leaves ONLY sections 0 and 1 in M_Mannequin
	 * and the other eight correct. And it is invisible in a still frame, because ApplyTeamColors
	 * writes the same team tint into every MID whatever its parent — a wrongly-parented slot is the
	 * right colour. So the evidence has to be a comparison against the asset, which is what the
	 * material column does.
	 */
	TAutoConsoleVariable<int32> CVarBodyMeshKeepOverrides(
		TEXT("Trace.Characters.BodyMeshKeepOverrides"),
		0,
		TEXT("Trace, dev only. 1 = do not clear the component's material overrides when the body mesh is\n")
		TEXT("swapped. This is the RED ARM: it restores the stale-override bug, so a pawn that has changed\n")
		TEXT("body draws some slots in the PREVIOUS body's materials, and Trace.Characters.BodyMesh must\n")
		TEXT("report FAIL on the material column while it is on."),
		ECVF_Cheat);
#endif

	/** 0 in a shipping build: the red arm is a dev-only switch and the fix is unconditional there. */
	static bool ShouldKeepStaleMaterialOverrides()
	{
#if !UE_BUILD_SHIPPING
		return CVarBodyMeshKeepOverrides.GetValueOnGameThread() != 0;
#else
		return false;
#endif
	}
}

FName ATraceCharacter::ResolveBodyBoneName(FName MannequinBoneName) const
{
	return TraceCharacterBody::ResolveBoneName(GetMesh(), MannequinBoneName);
}

void ATraceCharacter::UpdateCharacterBodyMesh(bool bIsPoll)
{
	// A dedicated server draws nothing and never builds a pose, so it loads no character art at all —
	// the same rule, for the same reason, as SetupCharacterVisuals(). The capsule is what the server
	// simulates and it is identical for every character.
	if (IsRunningDedicatedServer())
	{
		return;
	}

	USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp == nullptr)
	{
		return;
	}

#if !UE_BUILD_SHIPPING
	if (bIsPoll && TraceCharacterBody::CVarBodyMeshEventsOnly.GetValueOnGameThread() != 0)
	{
		return;
	}
#else
	(void)bIsPoll;
#endif

	// THE REPLICATED SELECTION IS THE SOURCE OF TRUTH — the same value the ability framework, the
	// select screen and the scoreboard read, so the body on screen cannot disagree with the character
	// being simulated. Null PlayerState, unreplicated ability component and "still choosing" all
	// answer the same thing here, and it is the right answer: NoneId, i.e. the Mannequin.
	uint8 DesiredId = TraceCharacterRoster::NoneId;
	if (const ATracePlayerState* TracePS = GetPlayerState<ATracePlayerState>())
	{
		const uint8 Selected = TracePS->GetSelectedCharacter();
		if (TraceCharacterRoster::IsValidId(Selected))
		{
			DesiredId = Selected;
		}
	}

	// *** THIS BRANCH IS WHY A POLL IS AFFORDABLE. *** Ten pawns, every frame, on every machine, and
	// the steady state is one integer compare each. Note that the id is recorded even when the apply
	// FAILED (see ApplyCharacterBodyMesh), so a missing import costs one LoadSynchronous for the life
	// of the pawn rather than one per frame.
	if (DesiredId == AppliedBodyCharacterId)
	{
		return;
	}

	ApplyCharacterBodyMesh(DesiredId);
}

bool ATraceCharacter::ApplyCharacterBodyMesh(uint8 CharacterId)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp == nullptr)
	{
		return false;
	}

	// -TraceNoCharacterArt is how the not-imported path gets TESTED on a machine where the import HAS
	// been run. It has to cover the per-character bodies too, or the switch would stop simulating a
	// fresh clone the moment one player picked Rocco.
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoCharacterArt")))
	{
		AppliedBodyCharacterId = CharacterId;
		return false;
	}

	// EMPTY PATH MEANS THE MANNEQUIN — NoneId, and every pawn between spawn and lock-in. Find()
	// returns null for NoneId, which lands in the same place. Since PIPELINE_DESIGN.md §9.1 all ten
	// roster rows name a body, so the empty case is no longer "nine of the ten characters": it is a
	// pawn that has not chosen yet. A character whose asset is missing takes the branch below and
	// fails the LOAD, which is a different and separately reported thing.
	FString WantedPath;
	FString WantedAnimPath;
	float WantedYaw = TraceCharacterLayout::MeshYaw;
	if (const TraceCharacterRoster::FTraceCharacterEntry* Row = TraceCharacterRoster::Find(CharacterId))
	{
		if (Row->BodyMeshPath != nullptr && Row->BodyMeshPath[0] != TEXT('\0'))
		{
			WantedPath = Row->BodyMeshPath;
			WantedYaw = Row->BodyMeshYaw;
			// READ ONLY ALONGSIDE THE MESH, deliberately. An anim class without the body it was
			// retargeted for is not a half-fix, it is a class bound to a skeleton this pawn is not
			// wearing — ApplyBodyAnimInstance would reject it and the pawn would end up in a bind
			// pose it did not need. A row with a mesh and no anim class is the honest bind-pose case
			// and is reported as one.
			WantedAnimPath = (Row->BodyAnimClassPath != nullptr) ? Row->BodyAnimClassPath : TEXT("");
		}
	}

	// RECORDED BEFORE THE WORK, ON PURPOSE. "We have already dealt with this id" is what makes the
	// poll free, and it has to be true of a FAILED apply too — otherwise a character whose mesh is not
	// imported would attempt a synchronous load every frame for the whole match.
	AppliedBodyCharacterId = CharacterId;

	USkeletalMesh* WantedMesh = nullptr;
	if (WantedPath.IsEmpty())
	{
		WantedMesh = CharacterMeshAsset.IsNull() ? nullptr : CharacterMeshAsset.LoadSynchronous();
		if (WantedMesh == nullptr)
		{
			// The Mannequin itself is not installed. SetupCharacterVisuals() has already said so, at
			// Error, with the command to run, and has switched this pawn to the fallback shapes — so
			// there is nothing to add and nothing to undo.
			return false;
		}
	}
	else
	{
		// LoadSynchronous on the soft path: the first Rocco pawn pays, every one after it gets the
		// resident object back. This is a character SWITCH or a spawn, not a per-frame path.
		WantedMesh = Cast<USkeletalMesh>(FSoftObjectPath(WantedPath).TryLoad());
		if (WantedMesh == nullptr)
		{
			// The whole point of the reference being soft: this pawn keeps the Mannequin it is already
			// wearing, the match carries on, and the missing import is reported through the SAME
			// machinery a missing Mannequin uses — banner at Error, and a line on the HUD for the rest
			// of the session, because a log line demonstrably was not enough.
			TraceCharacterBody::ReportMissingBodyMesh(CharacterId, WantedPath);
			return false;
		}
	}

	const bool bMeshChanged = (MeshComp->GetSkeletalMeshAsset() != WantedMesh);
	if (bMeshChanged)
	{
		// CLEARED BEFORE THE SWAP, AND THIS IS LOAD-BEARING. ApplyTeamColors() has already wrapped the
		// OLD body's slots in MIDs, and a MID installed by CreateDynamicMaterialInstance lives in the
		// component's OverrideMaterials — which SetSkeletalMeshAsset does NOT touch
		// (USkinnedMeshComponent::SetSkinnedAssetAndUpdate clears skin-weight profiles and morph
		// targets, and nothing else). An override outlives the mesh that justified it and is returned
		// in preference to the new asset's own material, per slot index. Left in place, Manny -> Rocco
		// would draw Rocco's first two sections in M_Mannequin through Rocco's UVs, and the switch back
		// would put Rocco's material on the Mannequin. The team tint hides it — every MID gets the same
		// colour whatever its parent — which is exactly why it has to be dropped here rather than
		// noticed later. Trace.Characters.BodyMeshKeepOverrides is the red arm that puts it back.
		if (!TraceCharacterBody::ShouldKeepStaleMaterialOverrides())
		{
			MeshComp->EmptyOverrideMaterials();
		}
		MeshComp->SetSkeletalMeshAsset(WantedMesh);
	}

	BodyMeshYaw = WantedYaw;

	// Written here AND read every frame by UpdateCrouchPresentation, which recomposes this rotation
	// whenever the slide lean moves. Setting it once would survive exactly until the first slide.
	// Straight yaw rather than the leaned composition because a body swap happens at spawn or at a
	// character switch, both of which are standing still; the next lean tick restores the pose.
	MeshComp->SetRelativeRotation(FRotator(0.f, BodyMeshYaw, 0.f));
	MeshComp->SetRelativeLocation(FVector(0.f, 0.f, TraceCharacterLayout::MeshOffsetZ));

	// A pawn that was showing the fallback primitives (art half-installed) and then picks a character
	// whose body IS imported gets the body. Rare, but the alternative is a pawn holding two looks.
	MeshComp->SetVisibility(true);
	bUsingSkeletalMesh = true;
	if (FallbackBodyMesh != nullptr) { FallbackBodyMesh->SetVisibility(false); }
	if (FallbackHeadMesh != nullptr) { FallbackHeadMesh->SetVisibility(false); }

	ApplyBodyAnimInstance(MeshComp, WantedMesh, CharacterId, WantedAnimPath);

	// The MIDs are per-slot and the slot COUNT is a property of the mesh (the Mannequin has 2,
	// SK_Rocco has 10), so the team colour has to be rebuilt from scratch against the new body.
	ApplyTeamColors();

	if (bMeshChanged && Weapon != nullptr)
	{
		// The third-person knife is attached to a BONE BY NAME. Changing the asset under it leaves the
		// attachment pointing at a name the new rig may not have, which is not a crash — it is a knife
		// lying at the pawn's feet. The component rebuilds it against whatever this rig calls "hand_r".
		Weapon->NotifyBodyMeshChanged();
	}

	UE_LOG(LogTraceGame, Verbose, TEXT("%s body -> %s (character %s, yaw %.0f)"),
		*GetNameSafe(this), *GetNameSafe(WantedMesh), *TraceCharacterRoster::NameFor(CharacterId), BodyMeshYaw);

	return true;
}

void ATraceCharacter::ApplyBodyAnimInstance(USkeletalMeshComponent* MeshComp, USkeletalMesh* ForMesh,
	uint8 CharacterId, const FString& BodyAnimClassPath)
{
	if (MeshComp == nullptr || ForMesh == nullptr)
	{
		return;
	}

	// *** AN ANIM BLUEPRINT BELONGS TO A SKELETON, NOT TO A MESH, AND THAT IS WHY THIS TAKES A PATH.
	// *** ABP_Unarmed is compiled against the Mannequin's SKELETON ASSET. Hand it to a pawn wearing a
	// body built on any other skeleton and it does not animate it badly, it does not animate it at
	// all — which is what put the first bespoke body in the game into a bind pose. A row that names a
	// body therefore also names a class built for THAT body's skeleton: all ten generated bodies share
	// SK_TraceBody_Skeleton, so all ten name the one class baked onto it, ABP_Unarmed_Body — Epic's
	// own blend space retargeted and baked to sequences by Scripts/retarget_body.py, so a generated
	// character runs the same walk cycle everybody else does.
	//
	// EMPTY PATH IS STILL THE NORMAL ANSWER FOR A PAWN ON THE MANNEQUIN: no roster row, an
	// unresolvable body, or a clone that has not run the import all land on CharacterAnimClass, which
	// is what this function used unconditionally before there was a second rig in the game.
	FString AnimClassPath = BodyAnimClassPath;
#if !UE_BUILD_SHIPPING
	if (TraceCharacterBody::CVarBodyAnimIgnore.GetValueOnGameThread() != 0)
	{
		// The red arm. See the cvar: this is not a mutilation, it is the previous version of this
		// function, and it is what put Rocco in a bind pose in the first place.
		AnimClassPath.Reset();
	}
#endif

	UClass* LoadedAnimClass = nullptr;
	bool bWantedCharacterClass = false;
	if (!AnimClassPath.IsEmpty())
	{
		bWantedCharacterClass = true;
		// Soft by construction, exactly like the mesh path beside it: a clone that has run the import
		// and not the retarget has the body and not this. TryLoadClass rather than a hard reference so
		// that machine loses one character's animation instead of failing to start.
		LoadedAnimClass = FSoftClassPath(AnimClassPath).TryLoadClass<UAnimInstance>();
		if (LoadedAnimClass == nullptr)
		{
			TraceCharacterBody::ReportMissingBodyAnim(CharacterId,
				FString::Printf(TEXT("%s did not load."), *AnimClassPath));
		}
	}
	else
	{
		LoadedAnimClass = CharacterAnimClass.IsNull() ? nullptr : CharacterAnimClass.LoadSynchronous();
	}

	// THE SKELETON CHECK SURVIVES THE FIX, and it is worth saying why. Both sides are now expected to
	// agree, so this should never fire — but the state it catches is a pawn of exactly the right
	// shape, size, colour and position that simply never moves, which is invisible to a screenshot
	// and to every other check in this file. Asking costs one pointer compare. Handing the class over
	// regardless is not a crash either: the engine notices, clears the instance and logs once per
	// pawn per spawn. This lets the game say the useful thing instead.
	USkeleton* const MeshSkeleton = ForMesh->GetSkeleton();
	USkeleton* AnimSkeleton = nullptr;
	if (LoadedAnimClass != nullptr)
	{
		if (IAnimClassInterface* const AnimClassInterface = IAnimClassInterface::GetFromClass(LoadedAnimClass))
		{
			AnimSkeleton = AnimClassInterface->GetTargetSkeleton();
		}
	}

	// A NULL AnimSkeleton means "could not tell" (not an anim blueprint generated class, or a class
	// that does not carry its target). Try it and let the engine decide, which is exactly what this
	// code did before there was anything to check — no behaviour change on the path that works.
	const bool bCompatible = (LoadedAnimClass != nullptr)
		&& (AnimSkeleton == nullptr || MeshSkeleton == nullptr || AnimSkeleton == MeshSkeleton);

	if (bCompatible)
	{
		MeshComp->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		MeshComp->SetAnimInstanceClass(LoadedAnimClass);
		return;
	}

	// Bind pose, and SAID OUT LOUD. A body that is the right shape and frozen is the single most
	// deniable failure in this whole feature.
	MeshComp->SetAnimationMode(EAnimationMode::AnimationCustomMode);
	MeshComp->SetAnimInstanceClass(nullptr);

	if (bWantedCharacterClass && LoadedAnimClass != nullptr)
	{
		// The class loaded and is bound to the WRONG skeleton — a stale retarget, or a path pointing
		// at somebody else's rig. Different failure from "not retargeted yet", same visible result,
		// so it goes through the same banner with its own sentence.
		TraceCharacterBody::ReportMissingBodyAnim(CharacterId,
			FString::Printf(TEXT("%s drives skeleton '%s', but that body is skinned to '%s'."),
				*AnimClassPath, *GetNameSafe(AnimSkeleton), *GetNameSafe(MeshSkeleton)));
	}

	static bool bWarnedIncompatibleAnim = false;
	if (!bWarnedIncompatibleAnim)
	{
		bWarnedIncompatibleAnim = true;
		UE_LOG(LogTraceGame, Warning,
			TEXT("[CharacterArt] %s is skinned to skeleton '%s', which is not the one %s drives ('%s'). ")
			TEXT("That body is drawn in its BIND POSE and will not animate. A character with a rig of ")
			TEXT("its own needs its own retargeted anim class: ./Scripts/retarget-rocco.sh bakes Rocco's."),
			*GetNameSafe(ForMesh), *GetNameSafe(MeshSkeleton),
			bWantedCharacterClass ? *AnimClassPath : *CharacterAnimClass.ToString(),
			*GetNameSafe(AnimSkeleton));
	}
}

// =================================================================================================
// Presentation
// =================================================================================================

void ATraceCharacter::ApplyTeamColors()
{
	// A dedicated server cooks no material shaders and renders nothing, so there is neither a need
	// nor a guarantee that the basic-shape material resolves there.
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	const FLinearColor TeamColor = TraceTeamColor(GetTeam());

	// Carrier reads as "lit up". Pull hard toward white so the Core holder is unmistakable at arena
	// distance, but keep enough hue to tell which team is holding it.
	// Blended per component on purpose. FMath::Lerp<T> expands to `A + Alpha * (B - A)`, i.e. it
	// needs `float * FLinearColor`; whether that free operator exists has moved around across the
	// 5.x line, and UI/TraceHUD.cpp hand-rolls its own colour lerp for exactly this reason. Doing it
	// component-wise costs nothing and cannot be wrong on any engine version.
	FLinearColor BodyColor = TeamColor;
	if (bIsCarrier)
	{
		constexpr float CarrierWhiteBlend = 0.72f;
		BodyColor = FLinearColor(
			FMath::Lerp(TeamColor.R, 1.f, CarrierWhiteBlend),
			FMath::Lerp(TeamColor.G, 1.f, CarrierWhiteBlend),
			FMath::Lerp(TeamColor.B, 1.f, CarrierWhiteBlend),
			1.f);
	}
	FLinearColor HeadColor = bIsCarrier ? FLinearColor::White : (TeamColor * 1.4f);

	// See EmissiveNormal: a no-op on the stock Mannequin materials, kept so a material with a live
	// emissive would light up for free. The carrier is made distinct by the near-white tint above,
	// which is the part that has been confirmed to work.
	float EmissivePower = bIsCarrier
		? TraceCharacterLayout::EmissiveCarrier
		: TraceCharacterLayout::EmissiveNormal;

	if (bDeadPresentation)
	{
		// Dim rather than hide: seeing where someone died is useful information, and it makes the
		// respawn delay legible without any UI. The glow goes out entirely, which reads instantly as
		// "that one is not a threat".
		BodyColor *= 0.2f;
		HeadColor *= 0.2f;
		EmissivePower = TraceCharacterLayout::EmissiveDead;
	}

	if (bUsingSkeletalMesh)
	{
		ApplyColorToSkeletalMesh(SanitizeTint(BodyColor), EmissivePower);
	}
	else
	{
		ApplyColorToMesh(FallbackBodyMesh, FallbackBodyMID, SanitizeTint(BodyColor));
		ApplyColorToMesh(FallbackHeadMesh, FallbackHeadMID, SanitizeTint(HeadColor));
	}

	// The gun's light channels wear the team colour too. It is the only part of your own kit you can
	// see in first person, so it is the only thing that answers "which side am I on" without opening
	// the scoreboard — and it means a spectator switching between players sees the change instantly.
	// Team::None replicates as grey-cyan, which is also the correct read: the team is not known yet.
	if (ViewModelNeonMID != nullptr)
	{
		const FLinearColor NeonColor = (GetTeam() == ETraceTeam::None)
			? FLinearColor(0.55f, 0.88f, 1.f)
			: TeamColor;
		ViewModelNeonMID->SetVectorParameterValue(TEXT("Color"), NeonColor);
		ViewModelNeonMID->SetScalarParameterValue(TEXT("Glow"), 2.4f);
		// Fallback path (BasicShapeMaterial, no Glow input): the best available approximation of an
		// unlit strip is a bright matte albedo. Same trade ATraceArenaBuilder::MakeNeonMID makes.
		ViewModelNeonMID->SetVectorParameterValue(TEXT("Color"), NeonColor);
		ViewModelNeonMID->SetVectorParameterValue(TEXT("BaseColor"), NeonColor);
	}

	// The skid streak is deliberately BRIGHTER than the suit and not whitened for the carrier: it is
	// a movement tell, and it has to be readable across the arena on a near-black floor.
	if (SlideSkidMesh != nullptr)
	{
		if (SlideSkidMID == nullptr)
		{
			UMaterialInterface* Parent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : BasicShapeMaterial.Get();
			if (Parent != nullptr)
			{
				SlideSkidMID = SlideSkidMesh->CreateDynamicMaterialInstance(0, Parent);
			}
		}
		if (SlideSkidMID != nullptr)
		{
			SlideSkidMID->SetVectorParameterValue(TEXT("Color"), TeamColor);
			SlideSkidMID->SetVectorParameterValue(TEXT("BaseColor"), TeamColor);
			SlideSkidMID->SetScalarParameterValue(TEXT("Glow"), TraceCharacterLayout::SkidGlow);
		}
	}
}

void ATraceCharacter::ApplyColorToSkeletalMesh(const FLinearColor& InColor, float InEmissivePower)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp == nullptr || MeshComp->GetSkeletalMeshAsset() == nullptr)
	{
		return;
	}

	USkeletalMesh* const MIDSourceMesh = MeshComp->GetSkeletalMeshAsset();
	const int32 NumSlots = MeshComp->GetNumMaterials();

	// Built once and then reused, for the same reason as the static-mesh MIDs below: this runs on
	// every team change, carrier change, death and poll tick, and a fresh MID per call would leave a
	// trail of garbage. CreateDynamicMaterialInstance(Index) with no parent wraps whatever material
	// is already in the slot, so Manny keeps his own textures and normal maps — this tints the
	// existing material, it does not replace it with flat colour.
	//
	// KEYED ON THE MESH, not only on the slot count. A MID wraps ONE asset's material, so the honest
	// question is "were these built for the body we are wearing", and two different bodies can agree
	// on slot count (any future pair of bespoke rigs) while sharing none of their materials. The count
	// stays in the test because a mesh can be reimported with more sections under the same pointer.
	if (CharacterMIDs.Num() != NumSlots || CharacterMIDsMesh.Get() != MIDSourceMesh)
	{
		CharacterMIDs.Reset(NumSlots);
		for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
		{
			CharacterMIDs.Add(MeshComp->CreateDynamicMaterialInstance(SlotIndex));
		}
		CharacterMIDsMesh = MIDSourceMesh;
	}

	for (UMaterialInstanceDynamic* MID : CharacterMIDs)
	{
		if (MID == nullptr)
		{
			continue;
		}

		// "Paint Tint" and "EmissivePower" are M_Mannequin's own parameters, read out of the asset
		// rather than guessed at. Setting a parameter a material does not have is a silent no-op, so
		// the extra generic names in this loop cost nothing and keep this working if the mesh is ever
		// swapped for one built on a different material — which is exactly what the generated bodies
		// are: "AccentGlow" below and the four vector names after it are M_TraceBodySuit/Glow/Accent's,
		// and every one of them is a no-op on the Mannequin.
		MID->SetVectorParameterValue(TraceCharacterAssets::PaintTintParam, InColor);
		MID->SetScalarParameterValue(TraceCharacterAssets::EmissivePowerParam, InEmissivePower);

		// AccentGlow: the generated bodies' accent-trim brightness. Same state scalar as
		// EmissivePower (8 normal / 30 carrier / 0 dead) so the accent dims with death and
		// brightens with the carrier read; the accent HUE lives on the per-character MI and
		// is deliberately NOT set here (see PIPELINE_DESIGN.md §4). No-op on the Mannequin.
		MID->SetScalarParameterValue(TEXT("AccentGlow"), InEmissivePower);

		MID->SetVectorParameterValue(TEXT("Color"), InColor);
		MID->SetVectorParameterValue(TEXT("BaseColor"), InColor);
		MID->SetVectorParameterValue(TEXT("Tint"), InColor);
		MID->SetVectorParameterValue(TEXT("EmissiveColor"), InColor);
	}
}

void ATraceCharacter::ApplyColorToMesh(UStaticMeshComponent* InMesh, TObjectPtr<UMaterialInstanceDynamic>& InOutMID, const FLinearColor& InColor)
{
	if (InMesh == nullptr || BasicShapeMaterial == nullptr)
	{
		return;
	}

	// Created once and then reused. This function runs on every team change, every carrier change,
	// every death and on a poll timer — allocating a fresh material instance each time would leave a
	// trail of them for the GC.
	if (InOutMID == nullptr)
	{
		InOutMID = InMesh->CreateDynamicMaterialInstance(0, BasicShapeMaterial);
	}

	if (InOutMID == nullptr)
	{
		return;
	}

	// BasicShapeMaterial's parameter is "Color"; "BaseColor" is set as well because setting a
	// parameter that does not exist is a silent no-op, and this way the code survives being pointed
	// at a differently-named material later.
	InOutMID->SetVectorParameterValue(TEXT("Color"), InColor);
	InOutMID->SetVectorParameterValue(TEXT("BaseColor"), InColor);
}

void ATraceCharacter::PollTeamColors()
{
	++TeamColorAttempts;
	ApplyTeamColors();

	// Stop as soon as the team is known, or give up. The OnRep hooks are the primary path; this
	// exists because the pawn, its PlayerState and that PlayerState's Team replicate independently
	// and no single callback is guaranteed to fire last.
	if (GetTeam() != ETraceTeam::None || TeamColorAttempts >= TraceCharacterLayout::MaxTeamColorAttempts)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(TeamColorTimerHandle);
		}
	}
}
