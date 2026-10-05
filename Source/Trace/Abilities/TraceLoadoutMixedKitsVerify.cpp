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
//                                          (Elle passive) and from Oyster's DASH CLOAK under Rocco's E
//                                          (JET BOOTS on movement, through a real jump).
//
// THE RED RUN. Every starred assertion was run against the code as it was before this fix (the
// production files reverted, this file kept), and every one of them failed there. The commit message
// records that output. A harness that has never been seen to fail is not evidence.
//
// =================================================================================================
// AND THE MIRROR IMAGE: AN ABILITY YOU DID NOT PICK MUST NOT FIRE (the Demo 35 leak pass)
//
// A kit is built when ANY of its abilities is picked, and the component ticks and notifies it
// whatever it was picked for — so every effect has to check its OWN ability. Each leak below is a
// NEGATIVE check (the kit equipped for its other abilities only: the effect must not happen) beside a
// positive control (the ability equipped: it must), through the shipped path:
//
//   BASH       a real dash with a player held in reach: CUSTOM STEEL + CHUD knocks nobody, publishes
//              no dash window and never lights the armed tell; BASH knocks and publishes.
//   X-MECHS    a player held on the bee orbit: LEECH + STING marks nobody and draws no swarm;
//              X-MECHS draws the swarm and marks.
//   ROCKJUMP   the V row: MODDED without ROCKJUMP (WIRERIGS on V) draws no "[V] ROCKET"; ROCKJUMP does.
//   OVERLOAD   a real dash: a Core carrier without OVERLOAD refills the second charge in the normal
//              duration + cooldown; OVERLOAD (not carrying) still refills it at half rate.
//   BLINK      the legacy mantle's trait: QMECH + QUAKE without BLINK is not allowed to mantle;
//              BLINK is. (Trace.Mortimer.MantleTest presses the real key at a real ledge for it.)
//   JAR JUMP   retired by Demo 35: a jump off his own jar on RILLA CANS + PICKLER is an ordinary
//              jump and leaves the jar; with Trace.Demo35.LegacyJarJump 1 it launches and breaks it.
//
// Victims are TEAM-MATES with friendly fire forced on for the holds (restored after), so the player
// being held next to the local pawn is not also shooting it.
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
#include "Abilities/Characters/TraceAbilitySetChut.h"
#include "Abilities/Characters/TraceAbilitySetElle.h"
#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Abilities/Characters/TraceAbilitySetOyster.h"
#include "Abilities/Characters/TraceAbilitySetRocco.h"
#include "Abilities/Characters/TraceAbilitySetSlimeball.h"
#include "Abilities/Characters/TraceAbilitySetX.h"
#include "Abilities/Characters/TraceOysterJar.h"
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Abilities/Characters/TraceSlimewall.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Gameplay/TraceFxBurst.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceMelee.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Movement/TraceCharacterMovementComponent.h"
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

		/**
		 * The engine frame the next step was scheduled on. A step never runs on that same frame: the
		 * core ticker runs a ticker added during its own pass in that same pass, so a "next frame"
		 * re-schedule with no delay would otherwise spin inside one frame with the world stopped —
		 * and a hold that stops the world is a hold in which nothing can happen, which reads as a pass.
		 */
		uint64 ScheduledFrame = 0;

		/** Real time the match was first seen live by the leak scenes' wait, 0 until then. */
		double LiveSince = 0.0;
		double LiveWaitUntil = 0.0;

		TWeakObjectPtr<ATracePlayerController> PC;
		TWeakObjectPtr<ATraceHUD> Hud;

		FTraceLoadout OriginalLoadout;
		bool bGooPresentAtStart = false;

		int32 Checks = 0;
		int32 Failures = 0;

		// ---- the leak checks: one staged scene at a time ----------------------------------------
		/** The player held next to the local pawn: a team-mate, with friendly fire forced on. */
		TWeakObjectPtr<ATraceCharacter> Victim;
		/** Real-time end of the current hold / watch, or of a wait for the ground. */
		double PhaseUntil = 0.0;
		/** Real time the pawn was first seen on the ground in the current wait, 0 while airborne. */
		double GroundedSince = 0.0;

		bool  bDashSeen = false;
		bool  bDashingFlagSeen = false;
		bool  bAccentLiftSeen = false;
		TWeakObjectPtr<UTraceAbilitySetChut> ChutKit;

		/**
		 * TWO WITNESSES FOR "A KNOCK LANDED", because neither is enough alone. The kit's per-dash victim
		 * list is the direct one, but BASH's own OnDashEnded clears it, so it is sampled every frame and
		 * its peak kept — and at a low frame rate the bash and the dash's end can share a frame. The
		 * ChutBash burst TryBash spawns at the victim lives 1.2 s, so a NEW one near the pawn after the
		 * dash is the second witness and cannot be missed between frames.
		 */
		int32 MaxBashedSeen = 0;
		TArray<TWeakObjectPtr<ATraceFxBurst>> BashBurstsBefore;

		/**
		 * UP TO THREE DASHES PER BASH SCENE. Chut's 20 Hz poll measures dash progress from the tick
		 * that first SAW the dash, so at a low frame rate a whole dash can pass without a poll tick
		 * inside the end window: one dash is a coin toss for the poll, which is the route the leak ran
		 * on. The negative scene dashes three times and must knock nobody on any of them; the control
		 * stops at its first knock.
		 */
		int32 DashTries = 0;
		int32 KnocksSeen = 0;

		float PeakZ = 0.f;
		TWeakObjectPtr<ATraceOysterJar> Jar;
		bool  bJarSpawned = false;

		/** Switches the run flips, as it found them. RestoreSwitches puts every one of them back. */
		bool  bSwitchesSaved = false;
		int32 SavedFakeCarrier = 0;
		int32 SavedLegacyJarJump = 0;
		bool  bSavedFriendlyFire = false;
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

	/**
	 * A living TEAM-MATE of @p Local who is not carrying the Core, or null. See the file header.
	 *
	 * One with no X kit first: with friendly fire on for the holds, a team-mate's own X-MECHS bees
	 * would be a second source of marks, and the X scenes must only ever see the local pawn's.
	 */
	ATraceCharacter* FindTeammateVictim(UWorld* WorldPtr, const ATraceCharacter* Local)
	{
		if (WorldPtr == nullptr || Local == nullptr || Local->GetTeam() == ETraceTeam::None)
		{
			return nullptr;
		}
		ATraceCharacter* Fallback = nullptr;
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate != nullptr && Candidate != Local && Candidate->IsAlive()
				&& Candidate->GetTeam() == Local->GetTeam() && !UTraceAbilityComponent::IsCarrier(Candidate))
			{
				if (UTraceAbilityComponent::FindEquippedSetFor<UTraceAbilitySetX>(Candidate) == nullptr)
				{
					return Candidate;
				}
				Fallback = (Fallback != nullptr) ? Fallback : Candidate;
			}
		}
		return Fallback;
	}

	/** The held player for the current scene, re-found if the last one died or took the Core. */
	ATraceCharacter* LiveVictim(FRun& Run, UWorld* WorldPtr, const ATraceCharacter* Local)
	{
		ATraceCharacter* Current = Run.Victim.Get();
		if (Current == nullptr || !Current->IsAlive() || UTraceAbilityComponent::IsCarrier(Current))
		{
			Current = FindTeammateVictim(WorldPtr, Local);
			if (Current == nullptr)
			{
				Current = FindEnemyVictim(WorldPtr, Local);   // a team of one: an enemy is the only choice
			}
			Run.Victim = Current;
		}
		return Current;
	}

	/** Teleports @p Who to @p Location at rest. The hold re-places every frame, so a walk is undone. */
	void HoldAt(ATraceCharacter* Who, const FVector& Location)
	{
		if (Who == nullptr)
		{
			return;
		}
		Who->SetActorLocation(Location, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		if (UTraceCharacterMovementComponent* WhoMove = Who->GetTraceMovement())
		{
			WhoMove->Velocity = FVector::ZeroVector;
		}
	}

	/** Keeps the local pawn alive through a scene: a run that ends in its own death proves nothing. */
	void KeepAlive(ATraceCharacter* Local)
	{
		if (Local != nullptr && Local->IsAlive() && Local->Health != nullptr)
		{
			Local->Health->ResetHealth();
		}
	}

	/** How many bee swarms are orbiting @p Host on this machine. */
	int32 CountSwarmsOn(UWorld* WorldPtr, const ATraceCharacter* Host)
	{
		int32 Found = 0;
		if (WorldPtr != nullptr)
		{
			for (TActorIterator<ATraceBeeSwarm> It(WorldPtr); It; ++It)
			{
				if (*It != nullptr && It->Host.Get() == Host)
				{
					++Found;
				}
			}
		}
		return Found;
	}

	int32 GetSwitch(const TCHAR* Name, int32 Fallback)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return (Var != nullptr) ? Var->GetInt() : Fallback;
	}

	/** Sets an int console variable; false when this build does not register it. */
	bool SetSwitch(const TCHAR* Name, int32 Value)
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		if (Var == nullptr)
		{
			return false;
		}
		Var->Set(Value, ECVF_SetByConsole);
		return true;
	}

	void SetFriendlyFire(bool bOn)
	{
		if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
		{
			Mutable->bFriendlyFire = bOn;
		}
	}

	/** Records every switch the leak scenes flip, once, so any exit can put them back. */
	void SaveSwitches(FRun& Run)
	{
		if (Run.bSwitchesSaved)
		{
			return;
		}
		Run.SavedFakeCarrier = GetSwitch(TEXT("Trace.MoveKitFakeCarrier"), 0);
		Run.SavedLegacyJarJump = GetSwitch(TEXT("Trace.Demo35.LegacyJarJump"), 0);
		Run.bSavedFriendlyFire = UTraceSettings::Get().bFriendlyFire;
		Run.bSwitchesSaved = true;
	}

	void RestoreSwitches(FRun& Run)
	{
		if (!Run.bSwitchesSaved)
		{
			return;
		}
		SetSwitch(TEXT("Trace.MoveKitFakeCarrier"), Run.SavedFakeCarrier);
		SetSwitch(TEXT("Trace.Demo35.LegacyJarJump"), Run.SavedLegacyJarJump);
		SetFriendlyFire(Run.bSavedFriendlyFire);
	}

	/**
	 * One frame of a real dash for the BASH scenes: the held player sits just clear of the dashing
	 * pawn's capsule, beside it and a little ahead, and the frame's evidence is sampled off the
	 * shipped state: is he dashing, is the dash window on the wire for the kit, is the armed tell lit,
	 * how many has this dash bashed.
	 *
	 * WHERE, AND WHY. The hold is placed before the frame's movement, and the pawn then dashes ~55-165
	 * uu (60-20 fps) before either bash route looks. Beside him at 72 uu and 60 uu ahead keeps the
	 * victim inside the 130 uu reach after any of those moves (and on the poll's swept segment), and
	 * never in the dash's way, so the dash is not blocked into a different test.
	 */
	void SampleBashFrame(FRun& Run, UWorld* WorldPtr, ATraceCharacter* Local)
	{
		KeepAlive(Local);
		const UTraceCharacterMovementComponent* LocalMove = (Local != nullptr) ? Local->GetTraceMovement() : nullptr;
		if (Local == nullptr || LocalMove == nullptr)
		{
			return;
		}
		Run.bDashSeen = Run.bDashSeen || LocalMove->IsDashing();

		const float Reach = FMath::Max(1.f, UTraceSettings::Get().ChutBashRadiusUU);
		float CapsuleRadius = 34.f;
		if (const UCapsuleComponent* Capsule = Local->GetCapsuleComponent())
		{
			CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		}
		const float Lateral = 2.f * CapsuleRadius + 4.f;              // just clear of both capsules
		const float Lead = FMath::Min(60.f, Reach * 0.45f);           // half a frame of dash, ahead
		const FVector Ahead = LocalMove->IsDashing()
			? LocalMove->GetDashDirection().GetSafeNormal2D() : Local->GetActorForwardVector().GetSafeNormal2D();
		const FVector Side = FVector::CrossProduct(FVector::UpVector, Ahead).GetSafeNormal();
		HoldAt(LiveVictim(Run, WorldPtr, Local), Local->GetActorLocation() + Ahead * Lead + Side * Lateral);

		if (const UTraceAbilitySetChut* Kit = Run.ChutKit.Get())
		{
			Run.bDashingFlagSeen = Run.bDashingFlagSeen || ((Kit->State().Flags & TraceChutFlags::Dashing) != 0);
			Run.bAccentLiftSeen = Run.bAccentLiftSeen || Kit->DebugIsAccentLifted();
			Run.MaxBashedSeen = FMath::Max(Run.MaxBashedSeen, Kit->GetBashedThisDashCount());
		}
	}

	/** Every ChutBash burst alive in the world right now. */
	void CollectBashBursts(UWorld* WorldPtr, TArray<TWeakObjectPtr<ATraceFxBurst>>& Out)
	{
		Out.Reset();
		if (WorldPtr == nullptr)
		{
			return;
		}
		for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
		{
			if (*It != nullptr && It->GetBurstType() == ETraceFxBurstType::ChutBash)
			{
				Out.Add(*It);
			}
		}
	}

	/** ChutBash bursts that were not in @p Before and sit within @p Radius of @p Where. */
	int32 CountNewBashBurstsNear(UWorld* WorldPtr, const TArray<TWeakObjectPtr<ATraceFxBurst>>& Before,
	                             const FVector& Where, float Radius)
	{
		TArray<TWeakObjectPtr<ATraceFxBurst>> Now;
		CollectBashBursts(WorldPtr, Now);
		int32 Fresh = 0;
		for (const TWeakObjectPtr<ATraceFxBurst>& Entry : Now)
		{
			const ATraceFxBurst* BurstActor = Entry.Get();
			if (BurstActor != nullptr && !Before.Contains(Entry)
				&& FVector::Dist(BurstActor->GetActorLocation(), Where) <= Radius)
			{
				++Fresh;
			}
		}
		return Fresh;
	}

	/**
	 * One frame of an X scene: the held player stands ON the bee orbit at the swarm's height, so every
	 * bee passes through him once a revolution (five bees at 240 deg/s: one every 0.3 s).
	 */
	void HoldOnBeeOrbit(FRun& Run, UWorld* WorldPtr, ATraceCharacter* Local)
	{
		KeepAlive(Local);
		if (Local == nullptr)
		{
			return;
		}
		const FVector Centre = TraceXBees::GetSwarmCentre(Local);
		HoldAt(LiveVictim(Run, WorldPtr, Local),
			Centre + Local->GetActorForwardVector().GetSafeNormal2D() * TraceXBees::GetOrbitRadiusUU());
	}

	/**
	 * The jar-jump scenes need him STOOD on the ground for a few ability ticks first: the jump poll
	 * finds the jump as a ground-to-air edge, so a jump on the frame he lands has no edge to find and
	 * would read as "no launch" on any build. Returns true once he has been grounded long enough.
	 */
	bool SettledOnGround(FRun& Run, const ATraceCharacter* Local)
	{
		const UTraceCharacterMovementComponent* LocalMove = (Local != nullptr) ? Local->GetTraceMovement() : nullptr;
		const double NowReal = FPlatformTime::Seconds();
		if (LocalMove == nullptr || !LocalMove->IsMovingOnGround())
		{
			Run.GroundedSince = 0.0;
			return false;
		}
		if (Run.GroundedSince <= 0.0)
		{
			Run.GroundedSince = NowReal;
		}
		return (NowReal - Run.GroundedSince) >= 0.3;
	}

	/** Drops one jar of his at his feet (the only one: older jars are cleared) and jumps. */
	void JumpOffOwnJar(FRun& Run, ATraceCharacter* Local, UTraceAbilityComponent* LocalAbilities)
	{
		Run.Jar = nullptr;
		Run.bJarSpawned = false;
		Run.PeakZ = 0.f;
		UTraceAbilitySetOyster* Oyster = (LocalAbilities != nullptr)
			? LocalAbilities->FindEquippedSet<UTraceAbilitySetOyster>() : nullptr;
		if (Oyster == nullptr || Local == nullptr)
		{
			return;
		}
		Oyster->DebugDestroyAllJars();
		float HalfHeight = 88.f;
		if (const UCapsuleComponent* Capsule = Local->GetCapsuleComponent())
		{
			HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		}
		Run.Jar = Oyster->DebugSpawnJarAt(Local->GetActorLocation() - FVector(0.f, 0.f, HalfHeight), /*bPickler=*/false);
		Run.bJarSpawned = Run.Jar.IsValid();
		Local->Jump();
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
				FString::Printf(TEXT("*** SLUDGE + STICKY GLOVES without VISTECH PADDING, stuck: the shot stays %.0f (got %.1f) ***"),
					BaseShot, Taken));
			Slime->DebugSetStuck(false);
		}
		else
		{
			Check(Run, false, TEXT("SLUDGE case: no Slimeball kit was built for STICKY GLOVES"));
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

		// ---- LEAK: ROCKJUMP's V row ----------------------------------------------------------------
		// The E kit is asked first, so MODDED on E used to answer for V whatever V was: a permanent
		// "[V] ROCKET" over WIRERIGS here, READY forever, while V did WIRERIGS' job.
		{
			float Remaining = 0.f;
			float Duration = 0.f;
			FString Label;
			Equip(Run, Make(ETraceAbilityId::Suspend, ETraceAbilityId::Blasters, ETraceAbilityId::Modded));
			const bool bRowWithout = Abilities->GetSecondaryCooldownDisplay(Remaining, Duration, Label);
			Check(Run, !bRowWithout,
				FString::Printf(TEXT("*** MODDED without ROCKJUMP (WIRERIGS on V): no V cooldown row (got row=%d label '%s') ***"),
					bRowWithout ? 1 : 0, *Label));

			Label.Reset();
			Equip(Run, Make(ETraceAbilityId::RockJump, ETraceAbilityId::Blasters, ETraceAbilityId::Ripple));
			const bool bRowWith = Abilities->GetSecondaryCooldownDisplay(Remaining, Duration, Label);
			Check(Run, bRowWith && Label == TEXT("ROCKET"),
				FString::Printf(TEXT("(control) ROCKJUMP under Rocco's E: the [V] ROCKET row is drawn (got row=%d label '%s')"),
					bRowWith ? 1 : 0, *Label));
		}

		// ---- LEAK: BLINK's legacy mantle -------------------------------------------------------------
		// The mantle is the movement ability BLINK replaced; with Trace.Demo35.LegacyMantle on, a
		// Mortimer kit picked only for QMECH or QUAKE used to get it too. The trait is the first thing
		// TryMantle asks; Trace.Mortimer.MantleTest presses the real key at a real ledge for the same rule.
		Check(Run, Settings.bMortimerCanMantle,
			TEXT("(precondition) bMortimerCanMantle is on, so the BLINK control below can say yes"));
		Equip(Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::MortimerLoad, ETraceAbilityId::Quake));
		Check(Run, !TraceAbilityTraits::IsMantleAllowed(Pawn),
			TEXT("*** QMECH + QUAKE without BLINK: the retired mantle is not allowed for this loadout ***"));
		Equip(Run, Make(ETraceAbilityId::Blink, ETraceAbilityId::Blasters, ETraceAbilityId::Ripple));
		Check(Run, TraceAbilityTraits::IsMantleAllowed(Pawn),
			TEXT("(control) BLINK under Rocco's E: the legacy mantle trait answers for BLINK"));
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
		Run->ScheduledFrame = GFrameCounter;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime || GFrameCounter == Run->ScheduledFrame)
				{
					return (--Run->TicksLeft) > 0;
				}
				return Tick(Run);
			}), 0.f);
	}

	void Finish(FRun& Run)
	{
		RestoreSwitches(Run);
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
			RestoreSwitches(*Run);
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
				UE_LOG(LogTraceGame, Display, TEXT("[MixedKits] WIRERIGS (suspend) pressed=%d"),
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
				TEXT("*** HUD: WIRERIGS under Chut's E draws the SUSPENDED chip ***"));
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
			// Through the shipped hooks: a dash ends, then a REAL jump through the jump binding —
			// the passive answers a jump that happened (OnJumpPerformed), not a key that went down.
			//
			// JET BOOTS on movement, and it is the robust choice rather than the risky one now. The
			// pawn may still be in the air from the SUSPEND scene: there a plain movement kit's press
			// would be no jump at all (no wall, no second jump) and correctly no cloak, while JET
			// BOOTS' second jump is a jump, and since the cloak listens to jumps rather than to the
			// first-consumer press offer, Rocco spending the press no longer hides it from the
			// passive. On the ground the engine's own jump does the same. Trace.Oyster.DashCloakVerify
			// is where each of those routes is proven separately.
			Equip(*Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::DashCloak, ETraceAbilityId::Ripple));
			Abilities->NotifyDashEnded(/*bReachedFullDistance=*/true);
			if (!Controller->DebugPressJump())
			{
				Check(*Run, false, TEXT("VISISPURS scene: the jump press reached the pawn (input not suppressed)"));
			}
			NextDelay = 0.2f;
			break;

		case 7:
		{
			const ATraceHUD::FFxHudDrawRecord Record = Shot(*HudPtr, TEXT("dashcloak_jetboots_ripple"));
			Check(*Run, Record.ChipText.Contains(TEXT("CLOAKED")),
				TEXT("*** HUD: Oyster's VISISPURS draws the CLOAKED chip ***"));
			Check(*Run, Record.Vignettes.Contains(TEXT("CLOAK")),
				TEXT("*** HUD: Oyster's VISISPURS draws the owner's cloak band ***"));
			NextDelay = 0.3f;   // let the photograph land before the loadout is put back
			break;
		}

		// =========================================================================================
		// THE LEAK SCENES (see the file header). Each NEGATIVE first — the kit equipped for its other
		// abilities only — then its control. Every scene is judged on a later tick than it was staged.
		// =========================================================================================

		case 8:
		{
			// ---- FIRST, A LIVE MATCH. Locking in (Start) ends the warm-up, and the half then starts
			// and puts every pawn back on its spawn and every health and mark back to new — in the
			// middle of a scene if it is allowed to. So the scenes wait for the match to be in play
			// and settled, and run inside the half.
			const ATraceGameState* TraceGS = WorldPtr->GetGameState<ATraceGameState>();
			const bool bLive = (TraceGS != nullptr) && TraceGS->TraceMatchState == ETraceMatchState::InProgress
				&& !TraceGS->IsHalfTimeBreak();
			const double NowReal = FPlatformTime::Seconds();
			if (Run->LiveWaitUntil <= 0.0)
			{
				Run->LiveWaitUntil = NowReal + 60.0;
			}
			if (!bLive || Run->LiveSince <= 0.0 || (NowReal - Run->LiveSince) < 2.0)
			{
				Run->LiveSince = bLive ? ((Run->LiveSince > 0.0) ? Run->LiveSince : NowReal) : 0.0;
				if (NowReal > Run->LiveWaitUntil)
				{
					Check(*Run, false, TEXT("(precondition) the match went live within 60 s, so the leak scenes could run"));
					Run->Step = 23;
					NextDelay = 0.1f;
					break;
				}
				Run->Step = ThisStep;
				NextDelay = 0.25f;
				break;
			}
			UE_LOG(LogTraceGame, Display, TEXT("[MixedKits] the match is live (%.1fs); the leak scenes run in play."),
				NowReal - Run->LiveSince);

			// ---- OVERLOAD: a Core carrier WITHOUT it refills the second charge at the normal rate ----
			// Trace.MoveKitFakeCarrier gives the local pawn the carrier's pool (base + the carrier's
			// extra charge) without a Core. 1.5 s first: the cloak scene's screenshot costs one long
			// frame under -RenderOffScreen, and a dash inside it begins and ends in one step.
			SaveSwitches(*Run);
			SetSwitch(TEXT("Trace.MoveKitFakeCarrier"), 1);
			Equip(*Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::Blasters, ETraceAbilityId::Ripple));
			NextDelay = 1.5f;
			break;
		}

		case 9:
		case 11:
		{
			// One real dash from a FULL pool, so the refill clock is started by this dash and nothing else.
			UTraceCharacterMovementComponent* LocalMove = Pawn->GetTraceMovement();
			if (LocalMove == nullptr)
			{
				Check(*Run, false, TEXT("the local pawn has a Trace movement component"));
				break;
			}
			for (int32 RefundTries = 0; RefundTries < 4 && LocalMove->GetDashCharges() < LocalMove->GetMaxDashCharges(); ++RefundTries)
			{
				LocalMove->RefundDashCharge();
			}
			const int32 WantPool = FMath::Max(1, UTraceSettings::Get().BaseDashCharges)
				+ ((ThisStep == 9) ? FMath::Max(0, UTraceSettings::Get().CarrierExtraDashCharges)
				                   : TraceAbilityTraits::GetExtraDashCharges(Pawn));
			Check(*Run, LocalMove->GetMaxDashCharges() == WantPool && LocalMove->GetDashCharges() == WantPool,
				FString::Printf(TEXT("(precondition) %s: the pool is full at %d before the dash (got %d of %d)"),
					(ThisStep == 9) ? TEXT("fake carrier, no OVERLOAD") : TEXT("OVERLOAD, not carrying"),
					WantPool, LocalMove->GetDashCharges(), LocalMove->GetMaxDashCharges()));
			Pawn->DoDash();
			NextDelay = 0.1f;
			break;
		}

		case 10:
		case 12:
		{
			const UTraceCharacterMovementComponent* LocalMove = Pawn->GetTraceMovement();
			const float NormalWindow = FMath::Max(0.01f, UTraceSettings::Get().DashDuration)
				+ FMath::Max(0.f, UTraceSettings::Get().DashCooldown);
			const float RefillLeft = (LocalMove != nullptr) ? LocalMove->GetDashCooldownRemaining() : -1.f;
			const int32 Held = (LocalMove != nullptr) ? LocalMove->GetDashCharges() : -1;
			const int32 Pool = (LocalMove != nullptr) ? LocalMove->GetMaxDashCharges() : -1;
			Check(*Run, Held >= 1 && Held == Pool - 1,
				FString::Printf(TEXT("(precondition) the dash spent exactly one charge (%d of %d left)"), Held, Pool));

			if (ThisStep == 10)
			{
				// The clock is a few frames old, so it reads a little UNDER the window; the old rule
				// started it at twice the window, which no elapsed time brings under this line.
				Check(*Run, RefillLeft > 0.5f * NormalWindow && RefillLeft <= NormalWindow + 0.05f,
					FString::Printf(TEXT("*** a Core carrier WITHOUT OVERLOAD (JET BOOTS/BLASTERS/RIPPLE) at 1 of 2 refills in "
					                     "duration + cooldown = %.2fs (clock reads %.2fs) ***"),
						NormalWindow, RefillLeft));

				SetSwitch(TEXT("Trace.MoveKitFakeCarrier"), Run->SavedFakeCarrier);
				Check(*Run, !UTraceAbilityComponent::IsCarrier(Pawn),
					TEXT("(precondition) the local pawn is not really carrying the Core, so OVERLOAD's charge is in the pool"));
				Equip(*Run, Make(ETraceAbilityId::Overload, ETraceAbilityId::Blasters, ETraceAbilityId::Ripple));
				NextDelay = 0.3f;
				break;
			}

			const float SlowWindow = NormalWindow * FMath::Max(1.f, UTraceSettings::Get().LilyExtraDashRechargeScale);
			Check(*Run, RefillLeft > NormalWindow + 0.25f && RefillLeft <= SlowWindow + 0.05f,
				FString::Printf(TEXT("(control) OVERLOAD, not carrying, at 1 of 2: its extra charge still refills at half rate "
				                     "(%.2fs window, clock reads %.2fs)"),
					SlowWindow, RefillLeft));

			// ---- X-MECHS: LEECH + STING without it draw no swarm and sting nobody ---------------
			// Friendly fire ON for the holds (see the file header), and only for the holds.
			SetFriendlyFire(true);
			Equip(*Run, Make(ETraceAbilityId::Leech, ETraceAbilityId::Blasters, ETraceAbilityId::Sting));
			Check(*Run, CountSwarmsOn(WorldPtr, Pawn) == 0,
				FString::Printf(TEXT("*** LEECH + STING without X-MECHS: no bee swarm is drawn around him (%d) ***"),
					CountSwarmsOn(WorldPtr, Pawn)));
			if (ATraceCharacter* Held0 = LiveVictim(*Run, WorldPtr, Pawn))
			{
				if (Held0->Health != nullptr)
				{
					Held0->Health->ClearVulnerable();
				}
			}
			Run->PhaseUntil = FPlatformTime::Seconds() + 1.2;
			NextDelay = 0.f;
			break;
		}

		case 13:
		case 14:
		{
			HoldOnBeeOrbit(*Run, WorldPtr, Pawn);
			if (FPlatformTime::Seconds() < Run->PhaseUntil)
			{
				Run->Step = ThisStep;   // hold another frame
				NextDelay = 0.f;
				break;
			}

			ATraceCharacter* Held = Run->Victim.Get();
			const bool bMarked = (Held != nullptr) && (Held->Health != nullptr) && Held->Health->IsVulnerable();
			Check(*Run, Held != nullptr, TEXT("(precondition) a living player was held on the bee orbit"));

			if (ThisStep == 13)
			{
				Check(*Run, !bMarked,
					FString::Printf(TEXT("*** LEECH + STING without X-MECHS: %s, held ON the bee orbit for 1.2 s, is NOT marked "
					                     "VULNERABLE ***"), *GetNameSafe(Held)));

				Equip(*Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::XMechs, ETraceAbilityId::Ripple));
				Check(*Run, CountSwarmsOn(WorldPtr, Pawn) == 1,
					FString::Printf(TEXT("(control) X-MECHS under Rocco's E: the bee swarm is drawn (%d)"),
						CountSwarmsOn(WorldPtr, Pawn)));
				if (Held != nullptr && Held->Health != nullptr)
				{
					Held->Health->ClearVulnerable();
				}
				Run->PhaseUntil = FPlatformTime::Seconds() + 1.2;
				NextDelay = 0.f;
				break;
			}

			Check(*Run, bMarked,
				FString::Printf(TEXT("(control) X-MECHS under Rocco's E: %s, held on the same orbit, IS marked VULNERABLE"),
					*GetNameSafe(Held)));
			if (Held != nullptr && Held->Health != nullptr)
			{
				Held->Health->ClearVulnerable();
			}

			// ---- BASH: CUSTOM STEEL + CHUD without it knock nobody and light no tell -------------
			Equip(*Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::CustomSteel, ETraceAbilityId::Chud));
			Run->ChutKit = Abilities->FindEquippedSet<UTraceAbilitySetChut>();
			NextDelay = 0.3f;
			break;
		}

		case 15:
		case 17:
		{
			// A full pool and one real dash through the shipping entry point, with the held player
			// beside him from the first frame.
			if (UTraceCharacterMovementComponent* LocalMove = Pawn->GetTraceMovement())
			{
				for (int32 RefundTries = 0; RefundTries < 4 && LocalMove->GetDashCharges() < LocalMove->GetMaxDashCharges(); ++RefundTries)
				{
					LocalMove->RefundDashCharge();
				}
			}
			if (Run->DashTries == 0)
			{
				// A new scene: its evidence starts empty and gathers over every dash it makes.
				Run->bDashSeen = false;
				Run->bDashingFlagSeen = false;
				Run->bAccentLiftSeen = false;
				Run->KnocksSeen = 0;
			}
			Run->MaxBashedSeen = 0;
			CollectBashBursts(WorldPtr, Run->BashBurstsBefore);
			SampleBashFrame(*Run, WorldPtr, Pawn);
			Pawn->DoDash();
			Run->PhaseUntil = FPlatformTime::Seconds() + 0.6;
			NextDelay = 0.f;
			break;
		}

		case 16:
		case 18:
		{
			SampleBashFrame(*Run, WorldPtr, Pawn);
			if (FPlatformTime::Seconds() < Run->PhaseUntil)
			{
				Run->Step = ThisStep;   // watch another frame
				NextDelay = 0.f;
				break;
			}

			const UTraceAbilitySetChut* Kit = Run->ChutKit.Get();
			// The dash covers ~600 uu and the knock lands in its last 35%, beside the pawn; 700 uu around
			// where he ends up holds every burst this dash can have made and none from across the arena.
			const int32 FreshBursts = CountNewBashBurstsNear(WorldPtr, Run->BashBurstsBefore, Pawn->GetActorLocation(), 700.f);
			Run->KnocksSeen += FMath::Max(Run->MaxBashedSeen, FreshBursts);
			++Run->DashTries;
			UE_LOG(LogTraceGame, Display,
				TEXT("[MixedKits] %s dash %d: peak victims-this-dash %d, new ChutBash bursts near him %d (knocks so far %d)."),
				(ThisStep == 16) ? TEXT("no-BASH") : TEXT("BASH"), Run->DashTries, Run->MaxBashedSeen, FreshBursts, Run->KnocksSeen);
			if (Run->KnocksSeen == 0 && Run->DashTries < 3)
			{
				Run->Step = ThisStep - 1;   // another dash: see DashTries
				NextDelay = 0.2f;
				break;
			}
			const int32 Bashed = Run->KnocksSeen;
			const int32 Dashes = Run->DashTries;
			Run->DashTries = 0;
			Check(*Run, Kit != nullptr && Run->bDashSeen,
				FString::Printf(TEXT("(precondition) a Chut kit was equipped (%d) and a real dash ran (%d) with %s held in reach"),
					(Kit != nullptr) ? 1 : 0, Run->bDashSeen ? 1 : 0, *GetNameSafe(Run->Victim.Get())));

			if (ThisStep == 16)
			{
				Check(*Run, Bashed == 0,
					FString::Printf(TEXT("*** CUSTOM STEEL + CHUD without BASH: the end of the dash knocks NOBODY (%d knock(s) in %d dash(es)) ***"),
						Bashed, Dashes));
				Check(*Run, !Run->bDashingFlagSeen,
					TEXT("*** CUSTOM STEEL + CHUD without BASH: no bash window is published (the Dashing flag never reached the wire) ***"));
				Check(*Run, !Run->bAccentLiftSeen,
					TEXT("*** CUSTOM STEEL + CHUD without BASH: the 'bash armed' accent never lights ***"));

				Equip(*Run, Make(ETraceAbilityId::JetBoots, ETraceAbilityId::Bash, ETraceAbilityId::Ripple));
				Run->ChutKit = Abilities->FindEquippedSet<UTraceAbilitySetChut>();
				NextDelay = 0.3f;
				break;
			}

			Check(*Run, Bashed >= 1,
				FString::Printf(TEXT("(control) BASH under Rocco's E: the end of the dash knocks the held player (%d knock(s) in %d dash(es))"),
					Bashed, Dashes));
			Check(*Run, Run->bDashingFlagSeen,
				TEXT("(control) BASH under Rocco's E: the bash window is published for the armed tell"));
			UE_LOG(LogTraceGame, Display, TEXT("[MixedKits] BASH control: armed accent seen on a sampled frame = %d "
				"(the window is ~63 ms and frame-sampled, so this one is reported, not asserted)."),
				Run->bAccentLiftSeen ? 1 : 0);
			SetFriendlyFire(Run->bSavedFriendlyFire);

			// ---- THE JAR JUMP Demo 35 retired: RILLA CANS + PICKLER, no character id -----------------
			Equip(*Run, Make(ETraceAbilityId::None, ETraceAbilityId::PickleJar, ETraceAbilityId::Pickler));
			Run->GroundedSince = 0.0;
			Run->PhaseUntil = FPlatformTime::Seconds() + 6.0;
			NextDelay = 0.f;
			break;
		}

		case 19:
		case 21:
		{
			KeepAlive(Pawn);
			if (!SettledOnGround(*Run, Pawn))
			{
				if (FPlatformTime::Seconds() > Run->PhaseUntil)
				{
					Check(*Run, false, TEXT("(precondition) he stood on the ground so a jump off his own jar could be tried"));
					Run->Step = 23;   // straight to the verdict
					NextDelay = 0.1f;
					break;
				}
				Run->Step = ThisStep;
				NextDelay = 0.f;
				break;
			}
			JumpOffOwnJar(*Run, Pawn, Abilities);
			Run->PhaseUntil = FPlatformTime::Seconds() + 0.3;
			NextDelay = 0.f;
			break;
		}

		case 20:
		case 22:
		{
			if (const UTraceCharacterMovementComponent* LocalMove = Pawn->GetTraceMovement())
			{
				Run->PeakZ = FMath::Max(Run->PeakZ, static_cast<float>(LocalMove->Velocity.Z));
			}
			if (FPlatformTime::Seconds() < Run->PhaseUntil)
			{
				Run->Step = ThisStep;
				NextDelay = 0.f;
				break;
			}

			const float PlainJumpZ = (Pawn->GetTraceMovement() != nullptr) ? Pawn->GetTraceMovement()->JumpZVelocity : 0.f;
			const float LaunchZ = UTraceSettings::Get().OysterJarJumpZVelocity;
			const bool bJarStanding = Run->Jar.IsValid();
			Check(*Run, Run->bJarSpawned && Run->PeakZ > 0.5f * PlainJumpZ,
				FString::Printf(TEXT("(precondition) a jar of his was at his feet (%d) and he jumped (peak Z %.0f uu/s)"),
					Run->bJarSpawned ? 1 : 0, Run->PeakZ));

			if (ThisStep == 20)
			{
				Check(*Run, Run->PeakZ < 0.5f * (PlainJumpZ + LaunchZ),
					FString::Printf(TEXT("*** RILLA CANS + PICKLER: a jump off his own jar is an ORDINARY jump (peak Z %.0f; a plain "
					                     "jump is %.0f, the retired launch %.0f) ***"), Run->PeakZ, PlainJumpZ, LaunchZ));
				Check(*Run, bJarStanding,
					TEXT("*** RILLA CANS + PICKLER: the jump leaves his jar standing (no self-made poison burst) ***"));

				const bool bSwitchFound = SetSwitch(TEXT("Trace.Demo35.LegacyJarJump"), 1);
				UE_LOG(LogTraceGame, Display, TEXT("[MixedKits] jar-jump control: Trace.Demo35.LegacyJarJump 1 (%s)."),
					bSwitchFound ? TEXT("set") : TEXT("NOT REGISTERED in this build"));
				Run->GroundedSince = 0.0;
				Run->PhaseUntil = FPlatformTime::Seconds() + 6.0;
				NextDelay = 0.f;
				break;
			}

			Check(*Run, Run->PeakZ >= LaunchZ - 1.f,
				FString::Printf(TEXT("(control) Trace.Demo35.LegacyJarJump 1: the same jump launches him (peak Z %.0f of %.0f)"),
					Run->PeakZ, LaunchZ));
			Check(*Run, !bJarStanding && Run->bJarSpawned,
				TEXT("(control) Trace.Demo35.LegacyJarJump 1: the same jump breaks the jar"));
			SetSwitch(TEXT("Trace.Demo35.LegacyJarJump"), Run->SavedLegacyJarJump);
			NextDelay = 0.3f;
			break;
		}

		case 23:
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
		TEXT("the slide bonus, the Pickler refund, the stick goo, and the HUD's status chips and cloak band. Then the ")
		TEXT("mirror image: an ability you did NOT pick must not fire (BASH's knock and tell, X-MECHS' bees and swarm, ")
		TEXT("ROCKJUMP's V row, OVERLOAD's slow refill on a carrier, BLINK's legacy mantle, the retired jar jump), each ")
		TEXT("beside a control with the ability equipped. About 25 s."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&Start));
}

#endif // !UE_BUILD_SHIPPING
