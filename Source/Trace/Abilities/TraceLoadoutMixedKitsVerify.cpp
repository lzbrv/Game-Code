// Trace — Trace.Loadout.MixedKits: does every ability in a MIXED loadout actually do its job?
//
// =================================================================================================
// THE BUG CLASS THIS PROVES GONE
//
// Since the loadout screen, a player's three abilities can come from three different kits, and a
// player who locks in a loadout has NO character id. A lot of code still answered "which abilities
// does this player have" with a question that only ever described ONE kit:
//
//   * UTraceAbilityComponent::GetAbilitySetFor / GetAbilitySetAs<T> — the kit on E, nothing else;
//   * GetCharacterId() — the face, which a loadout-only player does not have at all.
//
// Every assertion below equips a loadout whose E comes from a DIFFERENT kit than the ability being
// measured (or equips the kit for a DIFFERENT ability than the one being measured), through
// UTraceAbilityComponent::ApplyLoadout, with the character id left at None, and then asks the
// shipped code path the game itself uses:
//
//   DAMAGE   ModifyDamageThroughPassives   CUSTOM STEEL (Chut passive) under Rocco's E, and not
//                                          under Chut's E alone; VISTECH PADDING (Slimeball passive)
//                                          under Rocco's E, and not under Slimeball's E alone.
//   TRAITS   TraceAbilityTraits            OVERLOAD and ACROBATICS (Lily movement), Mortimer's dash
//                                          passive, and not QUAKE alone.
//   SLIDE    GetSlideJumpWindowSpeedBonusFor  CARBON SLIDERS (Elle movement) under Rocco's E.
//   REFUND   UTraceOysterPoisonComponent::ApplyTo  PICKLER on E with no character id refunds E;
//                                          Oyster's jar passive under somebody else's E does not.
//   GOO      UTraceSlimeStickSubsystem     STICKY GLOVES with no character id gets its goo component.
//   HUD      the corner's draw record      SPEED BOOST (Rocco passive) and SUSPENDED (Mace movement)
//                                          under Chut's E; CLOAKED + the owner band from SHIMMER
//                                          (Elle passive) and from Oyster's DASH CLOAK under Rocco's E.
//
// THE RED RUN. Every starred assertion was run against the code as it was before this fix (the
// production files reverted, this file kept), and every one of them failed there. The commit message
// records that output. A harness that has never been seen to fail is not evidence.
//
// WHAT IT TOUCHES. The local player's loadout (restored at the end), their E cooldown, a poison on
// one enemy bot (it expires by itself in four seconds), and a stuck flag on a staged kit (cleared).
// Run it on a listen server or standalone, after the team screen is closed:
//     -TraceExecAt=10 -TraceExec="Trace.Teams.Close" -TraceExec2At=16 -TraceExec2="Trace.Loadout.MixedKits"
// =================================================================================================

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Abilities/Characters/TraceAbilitySetElle.h"
#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Abilities/Characters/TraceAbilitySetRocco.h"
#include "Abilities/Characters/TraceAbilitySetSlimeball.h"
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Abilities/Characters/TraceSlimewall.h"
#include "Containers/Ticker.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Gameplay/TraceMelee.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "UI/TraceHUD.h"
#include "UnrealClient.h"                 // FScreenshotRequest

namespace TraceLoadoutMixedKitsVerify
{
	struct FRun
	{
		int32 Step = 0;
		double NextRealTime = 0.0;
		int32 TicksLeft = 40000;

		TWeakObjectPtr<ATracePlayerController> PC;
		TWeakObjectPtr<ATraceHUD> Hud;

		FTraceLoadout OriginalLoadout;
		bool bGooPresentAtStart = false;

		int32 Checks = 0;
		int32 Failures = 0;
	};

	ATracePlayerController* FindLocalAuthorityPC(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr
			|| (!WorldPtr->IsNetMode(NM_ListenServer) && !WorldPtr->IsNetMode(NM_Standalone)))
		{
			// Staging is AUTHORITATIVE (ApplyLoadout, ApplyTo) and the HUD is LOCAL; only these two
			// net modes are both.
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			if (ATracePlayerController* Candidate = Cast<ATracePlayerController>(It->Get()))
			{
				if (Candidate->IsLocalController())
				{
					return Candidate;
				}
			}
		}
		return nullptr;
	}

	ATraceCharacter* PawnOf(const FRun& Run)
	{
		const ATracePlayerController* Controller = Run.PC.Get();
		return (Controller != nullptr) ? Cast<ATraceCharacter>(Controller->GetPawn()) : nullptr;
	}

	UTraceAbilityComponent* AbilitiesOf(const FRun& Run)
	{
		const ATracePlayerController* Controller = Run.PC.Get();
		return (Controller != nullptr) ? UTraceAbilityComponent::Get(Controller->PlayerState) : nullptr;
	}

	void Check(FRun& Run, bool bOk, const FString& Claim)
	{
		++Run.Checks;
		if (bOk)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[MixedKits]   ok    %s"), *Claim);
		}
		else
		{
			++Run.Failures;
			UE_LOG(LogTraceGame, Error, TEXT("[MixedKits]   FAIL  %s"), *Claim);
		}
	}

	FTraceLoadout Make(ETraceAbilityId Movement, ETraceAbilityId Passive, ETraceAbilityId Activated)
	{
		FTraceLoadout Out;
		Out.Set(ETraceLoadoutSlot::Movement, Movement);
		Out.Set(ETraceLoadoutSlot::Passive, Passive);
		Out.Set(ETraceLoadoutSlot::Activated, Activated);
		return Out;
	}

	/** Equips @p Wanted on the local player through the builder, with no character id. */
	bool Equip(FRun& Run, const FTraceLoadout& Wanted)
	{
		UTraceAbilityComponent* Abilities = AbilitiesOf(Run);
		if (Abilities == nullptr)
		{
			return false;
		}
		Abilities->ApplyLoadout(Wanted);
		UE_LOG(LogTraceGame, Display, TEXT("[MixedKits] equipped %s (character id %s)"),
			*TraceLoadoutToString(Abilities->GetLoadout()), TraceCharacterIdToString(Abilities->GetCharacterId()));
		return Abilities->GetLoadout() == Wanted && Abilities->GetCharacterId() == ETraceCharacterId::None;
	}

	/** Knife-from-the-front damage the local pawn would deal, through the shipped pipeline. */
	float FrontKnifeDamage(ATraceCharacter* Instigator, float Base)
	{
		FTraceAbilityDamageContext Context;
		Context.Instigator = Instigator;
		Context.Target = nullptr;   // no target: only the INSTIGATOR's kits are being measured
		Context.Cause = TraceMelee::GetKnifeKillCause();
		Context.bMelee = true;
		return UTraceAbilityComponent::ModifyDamageThroughPassives(Base, Context);
	}

	/** A body shot the local pawn would take, through the shipped pipeline. */
	float BodyShotTaken(ATraceCharacter* Target, float Base)
	{
		FTraceAbilityDamageContext Context;
		Context.Instigator = nullptr;   // no instigator: only the TARGET's kits are being measured
		Context.Target = Target;
		Context.Cause = FName(TEXT("Bullet"));
		return UTraceAbilityComponent::ModifyDamageThroughPassives(Base, Context);
	}

	ATraceCharacter* FindEnemyVictim(UWorld* WorldPtr, const ATraceCharacter* Local)
	{
		if (WorldPtr == nullptr || Local == nullptr)
		{
			return nullptr;
		}
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate != nullptr && Candidate != Local && Candidate->IsAlive()
				&& Candidate->GetTeam() != ETraceTeam::None && Candidate->GetTeam() != Local->GetTeam()
				&& !UTraceAbilityComponent::IsCarrier(Candidate))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** The pure checks: damage, traits, the slide bonus and the Pickler refund. One frame. */
	void RunImmediateChecks(FRun& Run, UWorld* WorldPtr)
	{
		ATraceCharacter* Pawn = PawnOf(Run);
		UTraceAbilityComponent* Abilities = AbilitiesOf(Run);
		const UTraceSettings& Settings = UTraceSettings::Get();

		// ---- DAMAGE: CUSTOM STEEL ----------------------------------------------------------------
		const float BaseKnife = 30.f;
		Check(Run, !FMath::IsNearlyEqual(Settings.ChutKnifeFrontDamage, BaseKnife),
			TEXT("(precondition) CUSTOM STEEL's front knife differs from the base, so the checks below can tell"));

		Equip(Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::CustomSteel, ETraceAbilityId::Ripple));
		const float WithSteel = FrontKnifeDamage(Pawn, BaseKnife);
		Check(Run, FMath::IsNearlyEqual(WithSteel, Settings.ChutKnifeFrontDamage),
			FString::Printf(TEXT("*** CUSTOM STEEL under Rocco's E: the front knife does %.0f (got %.0f) ***"),
				Settings.ChutKnifeFrontDamage, WithSteel));

		Equip(Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::Blasters, ETraceAbilityId::Chud));
		const float ChudOnly = FrontKnifeDamage(Pawn, BaseKnife);
		Check(Run, FMath::IsNearlyEqual(ChudOnly, BaseKnife),
			FString::Printf(TEXT("*** CHUD without CUSTOM STEEL: the front knife stays %.0f (got %.0f) ***"),
				BaseKnife, ChudOnly));

		// ---- DAMAGE: VISTECH PADDING ---------------------------------------------------------------
		const float BaseShot = 20.f;
		const float Reduced = BaseShot * (1.f - FMath::Clamp(Settings.SlimeballStuckDamageReduction, 0.f, 1.f));
		Check(Run, Reduced < BaseShot - 0.01f,
			TEXT("(precondition) VISTECH PADDING reduces a body shot at all, so the checks below can tell"));

		Equip(Run, Make(ETraceAbilityId::StickyGloves, ETraceAbilityId::VistechPadding, ETraceAbilityId::Ripple));
		if (UTraceAbilitySetSlimeball* Slime = Cast<UTraceAbilitySetSlimeball>(
			Abilities->GetAbilitySetForSlot(ETraceLoadoutSlot::Movement)))
		{
			Slime->DebugSetStuck(true);
			const float Taken = BodyShotTaken(Pawn, BaseShot);
			Check(Run, FMath::IsNearlyEqual(Taken, Reduced),
				FString::Printf(TEXT("*** VISTECH PADDING under Rocco's E, stuck: a %.0f body shot lands as %.1f (got %.1f) ***"),
					BaseShot, Reduced, Taken));
			Slime->DebugSetStuck(false);
		}
		else
		{
			Check(Run, false, TEXT("VISTECH PADDING: no Slimeball kit was built for STICKY GLOVES"));
		}

		Equip(Run, Make(ETraceAbilityId::StickyGloves, ETraceAbilityId::Blasters, ETraceAbilityId::Slimewall));
		if (UTraceAbilitySetSlimeball* Slime = Cast<UTraceAbilitySetSlimeball>(
			Abilities->GetAbilitySetForSlot(ETraceLoadoutSlot::Movement)))
		{
			Slime->DebugSetStuck(true);
			const float Taken = BodyShotTaken(Pawn, BaseShot);
			Check(Run, FMath::IsNearlyEqual(Taken, BaseShot),
				FString::Printf(TEXT("*** SLIMEWALL + STICKY GLOVES without VISTECH PADDING, stuck: the shot stays %.0f (got %.1f) ***"),
					BaseShot, Taken));
			Slime->DebugSetStuck(false);
		}
		else
		{
			Check(Run, false, TEXT("SLIMEWALL case: no Slimeball kit was built for STICKY GLOVES"));
		}

		// ---- TRAITS: Lily's movement abilities, Mortimer's passive ---------------------------------
		Equip(Run, Make(ETraceAbilityId::Overload, ETraceAbilityId::Blasters, ETraceAbilityId::Ripple));
		const int32 ExtraCharges = TraceAbilityTraits::GetExtraDashCharges(Pawn);
		Check(Run, Settings.LilyExtraDashCharges > 0 && ExtraCharges == FMath::Clamp(Settings.LilyExtraDashCharges, 0, 5),
			FString::Printf(TEXT("*** OVERLOAD under Rocco's E: %d extra dash charge(s) (got %d) ***"),
				Settings.LilyExtraDashCharges, ExtraCharges));

		Equip(Run, Make(ETraceAbilityId::Acrobatics, ETraceAbilityId::Blasters, ETraceAbilityId::Ripple));
		const float WallScale = TraceAbilityTraits::GetWallJumpMomentumScale(Pawn);
		Check(Run, Settings.LilyWallJumpMomentumBonus > 0.f && WallScale > 1.001f,
			FString::Printf(TEXT("*** ACROBATICS under Rocco's E: wall jumps carry more (scale %.3f) ***"), WallScale));

		Equip(Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::MortimerLoad, ETraceAbilityId::Ripple));
		const float DashScale = TraceAbilityTraits::GetDashDistanceScale(Pawn);
		const float HoldScale = TraceAbilityTraits::GetThrowChargeHoldScale(Pawn);
		Check(Run, DashScale < 0.999f && FMath::IsNearlyEqual(DashScale, FMath::Clamp(Settings.MortimerDashDistanceScale, 0.05f, 4.f)),
			FString::Printf(TEXT("*** Mortimer's dash passive under Rocco's E: dash reach x%.2f (got x%.2f) ***"),
				Settings.MortimerDashDistanceScale, DashScale));
		Check(Run, HoldScale > 1.001f,
			FString::Printf(TEXT("Mortimer's dash passive under Rocco's E: the throw charges longer (x%.2f)"), HoldScale));

		Equip(Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::Blasters, ETraceAbilityId::Quake));
		const float QuakeDash = TraceAbilityTraits::GetDashDistanceScale(Pawn);
		Check(Run, FMath::IsNearlyEqual(QuakeDash, 1.f),
			FString::Printf(TEXT("*** QUAKE without Mortimer's passive: a normal dash (got x%.2f) ***"), QuakeDash));

		// ---- SLIDE: CARBON SLIDERS -----------------------------------------------------------------
		Equip(Run, Make(ETraceAbilityId::CarbonSliders, ETraceAbilityId::Blasters, ETraceAbilityId::Ripple));
		const float GlobalBonus = 1.375f;
		const float SlideBonus = UTraceAbilityComponent::GetSlideJumpWindowSpeedBonusFor(Pawn, GlobalBonus);
		Check(Run, Settings.ElleSlideJumpGainBonus > 0.f && SlideBonus > GlobalBonus + 0.001f,
			FString::Printf(TEXT("*** CARBON SLIDERS under Rocco's E: a well-timed slide jump beats %.3f (got %.3f) ***"),
				GlobalBonus, SlideBonus));

		// ---- REFUND: PICKLER with no character id --------------------------------------------------
		ATraceCharacter* Victim = FindEnemyVictim(WorldPtr, Pawn);
		if (Victim == nullptr)
		{
			Check(Run, false, TEXT("PICKLER refund: no living enemy to poison (run with bots)"));
		}
		else
		{
			Equip(Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::Blasters, ETraceAbilityId::Pickler));
			Abilities->DebugSetActivatedCooldown(20.f);
			const float Before = Abilities->GetActivatedCooldownRemaining();
			UTraceOysterPoisonComponent::ApplyTo(Victim, Abilities);
			const float After = Abilities->GetActivatedCooldownRemaining();
			Check(Run, Before > 10.f && After < 0.5f,
				FString::Printf(TEXT("*** PICKLER on E with no character id: poisoning '%s' refunds E (%.1fs -> %.1fs) ***"),
					*GetNameSafe(Victim), Before, After));

			Equip(Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::PickleJar, ETraceAbilityId::Chud));
			Abilities->DebugSetActivatedCooldown(20.f);
			const float JarBefore = Abilities->GetActivatedCooldownRemaining();
			UTraceOysterPoisonComponent::ApplyTo(Victim, Abilities);
			const float JarAfter = Abilities->GetActivatedCooldownRemaining();
			Check(Run, JarBefore > 10.f && JarAfter > 10.f,
				FString::Printf(TEXT("Oyster's jar passive under Chut's E: a poison does NOT reset somebody else's E (%.1fs -> %.1fs)"),
					JarBefore, JarAfter));
			Abilities->DebugSetActivatedCooldown(0.f);
		}
	}

	/**
	 * Photographs the frame (UI included — the corner is a Slate widget) and returns the draw record
	 * the HUD wrote for it. The assertions read the RECORD, which is what the corner reports it
	 * emitted, not the gameplay state that fed it.
	 */
	ATraceHUD::FFxHudDrawRecord Shot(ATraceHUD& Hud, const TCHAR* Tag)
	{
		const FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots")
			/ FString::Printf(TEXT("MixedKits_%s_pid%d.png"), Tag, FPlatformProcess::GetCurrentProcessId()));
		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(FullPath));
		FScreenshotRequest::RequestScreenshot(FullPath, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[MixedKits] shot requested: %s"), *FullPath);

		Hud.LogFxHudDrawRecord(Tag);
		return Hud.GetFxHudDrawRecord();
	}

	bool Tick(TSharedPtr<FRun> Run);

	void Schedule(TSharedPtr<FRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + static_cast<double>(DelaySeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return (--Run->TicksLeft) > 0;
				}
				return Tick(Run);
			}), 0.f);
	}

	void Finish(FRun& Run)
	{
		if (UTraceAbilityComponent* Abilities = AbilitiesOf(Run))
		{
			Abilities->ApplyLoadout(Run.OriginalLoadout);
		}

		if (Run.Failures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[MixedKits] VERDICT: PASS. %d check(s). Every ability in a mixed loadout does its job, "
				     "whichever kit is on E and with no character id."), Run.Checks);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[MixedKits] VERDICT: FAIL. %d of %d check(s) failed."),
				Run.Failures, Run.Checks);
		}
	}

	bool Tick(TSharedPtr<FRun> Run)
	{
		ATracePlayerController* Controller = Run->PC.Get();
		ATraceHUD* HudPtr = Run->Hud.Get();
		UWorld* WorldPtr = (Controller != nullptr) ? Controller->GetWorld() : nullptr;
		ATraceCharacter* Pawn = PawnOf(*Run);
		UTraceAbilityComponent* Abilities = AbilitiesOf(*Run);

		if (Controller == nullptr || HudPtr == nullptr || WorldPtr == nullptr || Pawn == nullptr || Abilities == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[MixedKits] VERDICT: FAIL. Aborted at step %d: the local player, pawn or HUD went away."),
				Run->Step);
			return false;
		}

		const int32 ThisStep = Run->Step++;
		float NextDelay = 0.3f;

		switch (ThisStep)
		{
		case 0:
			// ---- GOO: STICKY GLOVES with no character id --------------------------------------------
			Run->bGooPresentAtStart = (Pawn->FindComponentByClass<UTraceSlimeStickFxComponent>() != nullptr);
			Equip(*Run, Make(ETraceAbilityId::StickyGloves, ETraceAbilityId::Blasters, ETraceAbilityId::Ripple));
			NextDelay = 0.6f;   // twelve polls of the stick subsystem
			break;

		case 1:
			if (Run->bGooPresentAtStart)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[MixedKits] the pawn already had the goo component before this run; the goo check "
					     "cannot discriminate and is skipped. Run it on a fresh match."));
			}
			else
			{
				Check(*Run, Pawn->FindComponentByClass<UTraceSlimeStickFxComponent>() != nullptr,
					TEXT("*** STICKY GLOVES with no character id: the stick goo is attached to the pawn ***"));
			}

			// ---- the same-frame checks --------------------------------------------------------------
			RunImmediateChecks(*Run, WorldPtr);

			// ---- HUD: SPEED BOOST (Rocco passive) and SUSPENDED (Mace movement) under Chut's E -------
			Equip(*Run, Make(ETraceAbilityId::Suspend, ETraceAbilityId::Blasters, ETraceAbilityId::Chud));
			if (UTraceCharacterAbilitySet* RoccoKit = Abilities->GetAbilitySetForSlot(ETraceLoadoutSlot::Passive))
			{
				// Rocco's OWN kill hook, the one the kill router calls.
				RoccoKit->OnKill(nullptr, FName(TEXT("Bullet")), /*bHeadshot=*/true);
				RoccoKit->OnKill(nullptr, FName(TEXT("Bullet")), /*bHeadshot=*/true);
			}
			Pawn->LaunchCharacter(FVector(0.f, 0.f, 1600.f), true, true);
			NextDelay = 0.3f;
			break;

		case 2:
			// Re-launched on the same tick as the press, for the reason TraceHUDV16Shots gives: a
			// headless frame can be long enough for the first hop to have landed.
			Pawn->LaunchCharacter(FVector(0.f, 0.f, 1600.f), true, true);
			if (UTraceCharacterAbilitySet* MaceKit = Abilities->GetAbilitySetForSlot(ETraceLoadoutSlot::Movement))
			{
				UE_LOG(LogTraceGame, Display, TEXT("[MixedKits] SUSPEND pressed=%d"),
					static_cast<int32>(MaceKit->OnSecondaryPressed()));
			}
			NextDelay = 0.2f;
			break;

		// Each SHOT step only photographs and asserts, and the next scene is staged on a LATER tick:
		// the screenshot is written at the end of the frame, so staging in the same tick would put
		// the next scene in this scene's photograph.
		case 3:
		{
			const ATraceHUD::FFxHudDrawRecord Record = Shot(*HudPtr, TEXT("suspend_blasters_chud"));
			Check(*Run, Record.ChipText.Contains(TEXT("SPEED BOOST")),
				TEXT("*** HUD: BLASTERS under Chut's E draws the SPEED BOOST chip ***"));
			Check(*Run, Record.ChipText.Contains(TEXT("SUSPENDED")),
				TEXT("*** HUD: SUSPEND under Chut's E draws the SUSPENDED chip ***"));
			NextDelay = 0.2f;
			break;
		}

		case 4:
			if (UTraceCharacterAbilitySet* MaceKit = Abilities->GetAbilitySetForSlot(ETraceLoadoutSlot::Movement))
			{
				MaceKit->OnSecondaryReleased();
			}

			// ---- HUD: CLOAKED from SHIMMER (Elle passive) under Rocco's E -------------------------
			Equip(*Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::Shimmer, ETraceAbilityId::Ripple));
			if (UTraceAbilitySetElle* ElleKit = Cast<UTraceAbilitySetElle>(
				Abilities->GetAbilitySetForSlot(ETraceLoadoutSlot::Passive)))
			{
				ElleKit->DebugStartCloak(2.5f);
			}
			NextDelay = 0.4f;
			break;

		case 5:
		{
			const ATraceHUD::FFxHudDrawRecord Record = Shot(*HudPtr, TEXT("shimmer_ripple"));
			Check(*Run, Record.ChipText.Contains(TEXT("CLOAKED")),
				TEXT("*** HUD: SHIMMER under Rocco's E draws the CLOAKED chip ***"));
			Check(*Run, Record.Vignettes.Contains(TEXT("CLOAK")),
				TEXT("*** HUD: SHIMMER under Rocco's E draws the owner's cloak band ***"));
			NextDelay = 0.2f;
			break;
		}

		case 6:
			// ---- HUD: CLOAKED from Oyster's DASH CLOAK (passive) under Rocco's E ------------------
			// Through the shipped hooks: a dash ends, then a jump, which is what the passive answers.
			//
			// BLINK on movement, not JET BOOTS, on purpose. Jump presses are offered to the kits in
			// turn and the first one to USE the press ends the offer, so a movement kit that spends
			// the jump (Rocco's second jump in the air) keeps it from ever reaching the passive. That
			// is a real limit of the dash cloak in mixed loadouts and is reported separately; this
			// check is about the HUD, so it uses a movement ability that never takes the jump.
			Equip(*Run, Make(ETraceAbilityId::Blink, ETraceAbilityId::DashCloak, ETraceAbilityId::Ripple));
			Abilities->NotifyDashEnded(/*bReachedFullDistance=*/true);
			Abilities->HandleJumpPressed();
			NextDelay = 0.2f;
			break;

		case 7:
		{
			const ATraceHUD::FFxHudDrawRecord Record = Shot(*HudPtr, TEXT("dashcloak_blink_ripple"));
			Check(*Run, Record.ChipText.Contains(TEXT("CLOAKED")),
				TEXT("*** HUD: Oyster's DASH CLOAK draws the CLOAKED chip ***"));
			Check(*Run, Record.Vignettes.Contains(TEXT("CLOAK")),
				TEXT("*** HUD: Oyster's DASH CLOAK draws the owner's cloak band ***"));
			NextDelay = 0.3f;   // let the photograph land before the loadout is put back
			break;
		}

		case 8:
			Finish(*Run);
			return false;

		default:
			return false;
		}

		Schedule(Run, NextDelay);
		return false;
	}

	void Start(UWorld* WorldPtr)
	{
		ATracePlayerController* Controller = FindLocalAuthorityPC(WorldPtr);
		ATraceHUD* HudPtr = (Controller != nullptr) ? Cast<ATraceHUD>(Controller->GetHUD()) : nullptr;
		ATracePlayerState* LocalState = (Controller != nullptr) ? Controller->GetPlayerState<ATracePlayerState>() : nullptr;
		UTraceAbilityComponent* Abilities = (LocalState != nullptr) ? UTraceAbilityComponent::Get(LocalState) : nullptr;
		const ATraceCharacter* Pawn = (Controller != nullptr) ? Cast<ATraceCharacter>(Controller->GetPawn()) : nullptr;

		if (Controller == nullptr || HudPtr == nullptr || LocalState == nullptr || Abilities == nullptr
			|| Pawn == nullptr || !Pawn->IsAlive())
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[MixedKits] VERDICT: FAIL. Needs a standalone or listen-server match with a living local "
				     "pawn and the match HUD (run on /Game/Maps/Arena after Trace.Teams.Close)."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[MixedKits] ===== every ability in a mixed loadout, whichever kit is on E ===== pawn=%s"),
			*GetNameSafe(Pawn));

		// Out of the select screen, the way LOCK IN leaves it, so the corner is drawn (the HUD hides
		// it while a full-screen page owns the screen) and the 4 Hz poll does not reopen it.
		if (Abilities->GetCharacterId() != ETraceCharacterId::None)
		{
			Abilities->ServerSetCharacter(ETraceCharacterId::None);
		}
		LocalState->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/true);
		LocalState->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);

		TSharedPtr<FRun> Run = MakeShared<FRun>();
		Run->PC = Controller;
		Run->Hud = HudPtr;
		Run->OriginalLoadout = Abilities->GetLoadout();
		Schedule(Run, 0.3f);
	}

	FAutoConsoleCommandWithWorld CmdMixedKits(
		TEXT("Trace.Loadout.MixedKits"),
		TEXT("Equips mixed loadouts on the local player (character id None, E from a different kit) and ")
		TEXT("proves each ability still works through the shipped path: damage passives, movement traits, ")
		TEXT("the slide bonus, the Pickler refund, the stick goo, and the HUD's status chips and cloak band."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&Start));
}

#endif // !UE_BUILD_SHIPPING
