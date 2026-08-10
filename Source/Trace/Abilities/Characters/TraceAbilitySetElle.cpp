// Trace — Elle. See the header for the clause-by-clause reading of spec v18 §2, for the two
// [ASSUMPTION]s the user is most likely to reverse, and for the one place SNAP does not fit the
// ability framework cleanly.

#include "Abilities/Characters/TraceAbilitySetElle.h"

#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/Characters/TraceElleGate.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE RED ARMS.
//
// Each removes ONE of Elle's abilities and nothing else, so Trace.Elle.Verify can be made to FAIL on
// a build that is otherwise identical. A harness whose red arm cannot reproduce is a harness whose
// green means nothing.
//
// The teleport has its own arm on the gate actor (Trace.Elle.SnapTeleportEnabled) rather than being
// folded into SnapEnabled, because "the gates were never placed" and "the gates were placed and moved
// nobody" are different failures with different fixes, and one switch covering both would let either
// hide behind the other.
//
// NONE of them touches the carrier rule. That has exactly one arm and it is the framework's
// (Trace.Ability.CarrierImmune), which is what makes Trace.Elle.CarrierTest's red half unambiguous.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarElleCloakEnabled(
	TEXT("Trace.Elle.CloakEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = passing or throwing the Core cloaks Elle for 3 s. 0 = the trigger "
	     "is detected and logged but no cloak is raised, so every cloak assertion in Trace.Elle.Verify "
	     "must go red. Never ship 0."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarElleSnapEnabled(
	TEXT("Trace.Elle.SnapEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = pressing E places a SNAP gate. 0 = the press charges the full "
	     "cooldown and places nothing, so every gate assertion in Trace.Elle.Verify must go red. "
	     "Never ship 0."),
	ECVF_Cheat);

namespace TraceAbilitySetElleFile
{
	/**
	 * How dark a cloaked Elle's body is drawn, as a multiplier on the cloak opacity.
	 *
	 * *** READ THIS BEFORE JUDGING THE CLOAK'S LOOK. ***
	 *
	 * §2 asks for "semi-transparent". NOTHING IN THIS PROJECT IS TRANSLUCENT: M_TraceSurface and
	 * M_TraceNeon are both authored BLEND_OPAQUE (Scripts/generate_content.py sets it explicitly, with
	 * a comment about why), and Epic's M_Mannequin — which is what a dressed pawn actually wears — is
	 * opaque too. A blend mode is a property of the MATERIAL, not of an instance, so no amount of
	 * parameter-setting from C++ can make an opaque material see-through. A translucent character
	 * material is a content change (a new parent in generate_content.py plus a regenerate), and
	 * content generation is not this pass's slice.
	 *
	 * So the cloak does the two things that DO work through an opaque material, and both of them are
	 * real reductions in how findable she is on this arena:
	 *
	 *   1. every colour parameter goes to near-black scaled by ElleCloakOpacity, and every emissive
	 *      parameter goes to zero. On a Tron field of black surfaces and bright neon, a team-coloured
	 *      glowing pawn is the brightest thing in a lane and an unlit black one is close to a
	 *      silhouette;
	 *   2. she stops casting a shadow, which is the other half of how a player is actually spotted
	 *      here — the floor is near-mirror and a shadow on it reads before the pawn does.
	 *
	 * AND it pushes an "Opacity" scalar anyway, which is a silent no-op on every material in the game
	 * today and becomes the whole ability the day a translucent character material lands. That is the
	 * codebase's own convention (see ApplyColorToSkeletalMesh: "setting a parameter a material does
	 * not have is a silent no-op"), not a stub pretending to work.
	 */
	constexpr float CloakBodyDarkness = 0.10f;

	/**
	 * Names every material family in this project uses for the same two ideas. Setting a parameter a
	 * material does not have is free, so listing them all is what keeps the cloak working across the
	 * Mannequin, the generated parents and the BasicShapes fallback without a branch per case.
	 */
	const FName ColourParams[] =
	{
		FName(TEXT("Paint Tint")),      // M_Mannequin's own — read out of the asset, not guessed
		FName(TEXT("Tint")),            // M_TraceSurface / M_TraceNeon
		FName(TEXT("Color")),           // M_TraceNeon / BasicShapeMaterial
		FName(TEXT("BaseColor")),       // M_TraceSurface
		FName(TEXT("EmissiveColor"))
	};

	const FName EmissiveParams[] =
	{
		FName(TEXT("EmissivePower")),       // M_Mannequin
		FName(TEXT("EmissiveStrength")),    // M_TraceSurface
		FName(TEXT("Glow"))                 // M_TraceNeon
	};

	/** The hook a translucent character material would pick up for free. No-op on every material today. */
	const FName OpacityParam(TEXT("Opacity"));

	/** The pawn's THIRD-PERSON body, i.e. the parts other players see. Not the first-person viewmodel. */
	void GatherBodyMeshes(ATraceCharacter* Pawn, TArray<UMeshComponent*, TInlineAllocator<3>>& Out)
	{
		Out.Reset();
		if (Pawn == nullptr)
		{
			return;
		}
		if (UMeshComponent* Skeletal = Pawn->GetMesh())
		{
			Out.Add(Skeletal);
		}
		if (Pawn->FallbackBodyMesh != nullptr)
		{
			Out.Add(Pawn->FallbackBodyMesh);
		}
		if (Pawn->FallbackHeadMesh != nullptr)
		{
			Out.Add(Pawn->FallbackHeadMesh);
		}
	}

	void DimForCloak(ATraceCharacter* Pawn, float Opacity)
	{
		TArray<UMeshComponent*, TInlineAllocator<3>> Meshes;
		GatherBodyMeshes(Pawn, Meshes);

		const float Clamped = FMath::Clamp(Opacity, 0.f, 1.f);
		const FVector Dim(CloakBodyDarkness * Clamped, CloakBodyDarkness * Clamped, (CloakBodyDarkness + 0.03f) * Clamped);

		for (UMeshComponent* Mesh : Meshes)
		{
			Mesh->SetScalarParameterValueOnMaterials(OpacityParam, Clamped);
			for (const FName& Param : EmissiveParams)
			{
				Mesh->SetScalarParameterValueOnMaterials(Param, 0.f);
			}
			for (const FName& Param : ColourParams)
			{
				Mesh->SetVectorParameterValueOnMaterials(Param, Dim);
			}
			Mesh->SetCastShadow(false);
		}
	}

	void RestoreFromCloak(ATraceCharacter* Pawn)
	{
		TArray<UMeshComponent*, TInlineAllocator<3>> Meshes;
		GatherBodyMeshes(Pawn, Meshes);

		for (UMeshComponent* Mesh : Meshes)
		{
			Mesh->SetScalarParameterValueOnMaterials(OpacityParam, 1.f);
			Mesh->SetCastShadow(true);
		}

		// The COLOUR is restored by asking the pawn to redo it, not by remembering what it was.
		// ATraceCharacter::ApplyTeamColors() is idempotent, is the one definition of a pawn's colours,
		// and already runs on every team change, carrier change and death — so restoring through it
		// means a cloak that ends during a team swap cannot hand back the wrong team's colour.
		if (Pawn != nullptr)
		{
			Pawn->ApplyTeamColors();
		}
	}
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetElle::OnUnequipped()
{
	// Cosmetics first: the pawn survives the character change, so a dimmed body would survive it too.
	ApplyCloakVisual(false);
	DestroyGates();
	LocalFirstGatePredictionEnd = 0.f;
}

void UTraceAbilitySetElle::OnPawnSpawned()
{
	// A fresh pawn has never been dimmed. Forget the old one rather than trying to restore it — it is
	// being destroyed by ATraceGameMode::RestartPlayerFresh on this same path.
	bCloakVisualApplied = false;
	CloakVisualPawn = nullptr;
	bHeldCoreLastTick = false;
}

void UTraceAbilitySetElle::OnPawnDied()
{
	// A cloak on a corpse is meaningless, and the corpse's materials are about to be hidden anyway.
	// Ending it on the server is what makes every OTHER machine restore too — the visual is driven
	// from the replicated fact, never from a local guess.
	if (HasAuthority())
	{
		EndCloak(TEXT("died"));
	}

	ApplyCloakVisual(false);
	bHeldCoreLastTick = false;

	// THE GATES OUTLIVE HER, deliberately, and this is the same [ASSUMPTION] Rocco's Ripple already
	// ships: §2 says the pair "expires after 8 s" and says nothing about their author. A pair that
	// vanished the instant Elle was shot would make the ability strictly worse for the team it was
	// laid for, and the choke point answers entry from her PlayerState, which survives her pawn.
}

void UTraceAbilitySetElle::OnHalfTime()
{
	// The framework has already cleared the cooldown and Reset() the net state, so IsCloaked() is
	// already false. What is left is the world actors and the cosmetic half, which is exactly what
	// this hook is documented to be for.
	DestroyGates();
	ApplyCloakVisual(false);
	bHeldCoreLastTick = false;
	LocalFirstGatePredictionEnd = 0.f;
}

// =================================================================================================
// Tick
// =================================================================================================

void UTraceAbilitySetElle::TickAbilities(float DeltaSeconds)
{
	// ---- EVERY MACHINE: the cosmetic half, driven from the REPLICATED fact ------------------------
	//
	// Re-pushed every tick while the cloak is up rather than once on the edge. ATraceCharacter::
	// ApplyTeamColors() runs on every carrier change — which is to say, on the very frame a pass
	// completes — and it would otherwise repaint her at full brightness for the rest of the cloak.
	// Re-pushing at 20 Hz costs a handful of parameter writes and cannot lose that race by more than
	// one tick.
	const bool bWantCloak = IsCloaked();
	if (bWantCloak || bCloakVisualApplied)
	{
		ApplyCloakVisual(bWantCloak);
	}

	if (!HasAuthority())
	{
		return;
	}

	const float Now = MatchTimeNow();
	const UTraceSettings& Settings = UTraceSettings::Get();

	// ---- SERVER: the cloak's trigger, its early-out and its expiry --------------------------------
	TickCloakTrigger();

	if (IsCloaked())
	{
		// §2 [ASSUMPTION]: picking the Core back up drops the cloak early. The reading is that the
		// cloak is a getaway, and a player who has the Core again is not getting away from anything.
		if (Settings.bElleCloakEndsOnCorePickup && ATraceCore::IsCoreHolder(GetCharacter()))
		{
			EndCloak(TEXT("took the Core back"));
		}
		else if (Now >= State().EffectEndMatchTime)
		{
			EndCloak(TEXT("expired"));
		}
	}

	// ---- SERVER: SNAP's second-gate window lapsing ------------------------------------------------
	//
	// §2: "If no second gate inside 4 s the first expires." The GATE expires itself, on its own
	// deadline — what happens here is the COOLDOWN, which is charged in full from the first press so
	// that a fluffed cast is not a free gate every four seconds.
	const FTraceAbilityNetState& Current = State();
	if ((Current.Flags & TraceAbilityFlags::AuxActive) != 0 && Now >= Current.AuxEndMatchTime)
	{
		const float ReadyAt = GetSnapReadyMatchTime();

		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::AuxActive);
		Writable.AuxEndMatchTime = ReadyAt;
		MarkStateDirty();

		FirstGate = nullptr;

		UE_LOG(LogTraceGame, Log,
			TEXT("[Elle] SNAP window lapsed with no second gate — the first expires and the full %.0fs "
			     "cooldown is charged from the first press (ready at match time %.2f)."),
			Settings.ElleSnapCooldownSeconds, ReadyAt);
	}

	// ---- SERVER: the pair's bookkeeping bit -------------------------------------------------------
	if ((Current.Flags & TraceAbilityFlags::Charged) != 0
		&& !FirstGate.IsValid() && !SecondGate.IsValid())
	{
		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::Charged);
		MarkStateDirty();
	}
}

// =================================================================================================
// PASSIVE 1 — THE CLOAK
// =================================================================================================

void UTraceAbilitySetElle::TickCloakTrigger()
{
	ATraceCharacter* MyPawn = GetCharacter();
	const bool bHoldsCoreNow = ATraceCore::IsCoreHolder(MyPawn);

	// THE EDGE, AND WHY IT IS QUALIFIED RATHER THAN BARE.
	//
	// "Right after passing or throwing" is not "right after ceasing to hold". A carrier stops holding
	// the Core when they are shot, when they fall out of the world, when half time parks it and when
	// an interception takes it — and cloaking her for three seconds every time somebody kills her
	// would be a different, much stronger, and unasked-for ability.
	//
	// So the edge is qualified by two facts that are both true of a pass and of a throw and of
	// nothing else: SHE IS STILL ALIVE, and the Core went somewhere voluntary — to a living team-mate
	// (a completed pass, ETraceCoreGrantReason::Pass) or LOOSE in the world (a mode-B throw).
	if (bHeldCoreLastTick && !bHoldsCoreNow && MyPawn != nullptr && MyPawn->IsAlive())
	{
		const ATraceCore* CoreActor = ATraceCore::Get(GetWorld());
		if (CoreActor != nullptr)
		{
			const ATraceCharacter* NewHolder = CoreActor->GetCarrier();

			const bool bPassed = (NewHolder != nullptr) && (NewHolder != MyPawn) && NewHolder->IsAlive()
				&& (NewHolder->GetTeam() != ETraceTeam::None)
				&& (NewHolder->GetTeam() == MyPawn->GetTeam());

			const bool bThrew = CoreActor->IsLoose();

			if (bPassed || bThrew)
			{
				StartCloak(bPassed ? TEXT("passed the Core") : TEXT("threw the Core"));
			}
			else
			{
				// Worth a line: "why did Elle not cloak" is otherwise unanswerable from a log, and the
				// three ways to lose the Core involuntarily all land here.
				UE_LOG(LogTraceGame, Verbose,
					TEXT("[Elle] lost the Core without passing or throwing it (newHolder=%s loose=%d) — no cloak."),
					*GetNameSafe(NewHolder), CoreActor->IsLoose() ? 1 : 0);
			}
		}
	}

	bHeldCoreLastTick = bHoldsCoreNow;
}

void UTraceAbilitySetElle::StartCloak(const TCHAR* Why)
{
	if (!HasAuthority())
	{
		return;
	}

	if (CVarElleCloakEnabled.GetValueOnAnyThread() == 0)
	{
		// RED ARM: the trigger fired and is logged, and no cloak goes up. Everything else about the
		// two arms is identical, so a cloak assertion that stays green under this is measuring
		// something other than the cloak.
		UE_LOG(LogTraceGame, Warning, TEXT("[Elle] CLOAK SUPPRESSED by Trace.Elle.CloakEnabled 0 (%s)."), Why);
		return;
	}

	const float Duration = FMath::Max(0.f, UTraceSettings::Get().ElleCloakDurationSeconds);
	const float EndAt = MatchTimeNow() + Duration;

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags |= TraceAbilityFlags::EffectActive;
	Writable.EffectEndMatchTime = EndAt;
	MarkStateDirty();

	UE_LOG(LogTraceGame, Log,
		TEXT("[Elle] CLOAK up for %.1fs (%s): opacity %.2f, ends at match time %.2f."),
		Duration, Why, UTraceSettings::Get().ElleCloakOpacity, EndAt);
}

void UTraceAbilitySetElle::EndCloak(const TCHAR* Why)
{
	if (!HasAuthority() || !IsCloaked())
	{
		return;
	}

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::EffectActive);
	Writable.EffectEndMatchTime = 0.f;
	MarkStateDirty();

	UE_LOG(LogTraceGame, Verbose, TEXT("[Elle] CLOAK down (%s)."), Why);
}

bool UTraceAbilitySetElle::IsCloaked() const
{
	const FTraceAbilityNetState& Current = State();
	return (Current.Flags & TraceAbilityFlags::EffectActive) != 0
		&& MatchTimeNow() < Current.EffectEndMatchTime;
}

float UTraceAbilitySetElle::GetCloakEndMatchTime() const
{
	return IsCloaked() ? State().EffectEndMatchTime : 0.f;
}

void UTraceAbilitySetElle::ApplyCloakVisual(bool bCloakOn)
{
	ATraceCharacter* MyPawn = GetCharacter();
	ATraceCharacter* PreviouslyDimmed = CloakVisualPawn.Get();

	// Restore first when the cloak is coming down, and ALSO when the pawn has changed underneath us:
	// a respawn between two ability ticks would otherwise leave a pawn dimmed with nothing left
	// holding the flag that says so.
	if (bCloakVisualApplied && (!bCloakOn || PreviouslyDimmed != MyPawn))
	{
		if (PreviouslyDimmed != nullptr)
		{
			TraceAbilitySetElleFile::RestoreFromCloak(PreviouslyDimmed);
		}
		bCloakVisualApplied = false;
		CloakVisualPawn = nullptr;
	}

	if (!bCloakOn || MyPawn == nullptr)
	{
		return;
	}

	// A dedicated server draws nothing, has no materials worth writing and would be doing this for
	// ten pawns. The FACT still replicates from there; only the paint is skipped.
	const UWorld* WorldPtr = GetWorld();
	if (WorldPtr != nullptr && WorldPtr->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	TraceAbilitySetElleFile::DimForCloak(MyPawn, UTraceSettings::Get().ElleCloakOpacity);
	bCloakVisualApplied = true;
	CloakVisualPawn = MyPawn;
}

#if !UE_BUILD_SHIPPING
void UTraceAbilitySetElle::DebugStartCloak(float Seconds)
{
	if (!HasAuthority())
	{
		return;
	}

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags |= TraceAbilityFlags::EffectActive;
	Writable.EffectEndMatchTime = MatchTimeNow() + FMath::Max(0.f, Seconds);
	MarkStateDirty();
}

bool UTraceAbilitySetElle::DebugPlaceGatePair(const FVector& MouthA, const FVector& MouthB)
{
	if (!HasAuthority())
	{
		return false;
	}

	DestroyGates();

	const float PairExpire = MatchTimeNow() + FMath::Max(0.f, UTraceSettings::Get().ElleSnapPairLifetimeSeconds);

	ATraceElleGate* GateA = PlaceGate(MouthA, PairExpire, /*bSecondOfPair=*/false);
	ATraceElleGate* GateB = PlaceGate(MouthB, PairExpire, /*bSecondOfPair=*/true);
	if (GateA == nullptr || GateB == nullptr)
	{
		DestroyGates();
		return false;
	}

	FirstGate = GateA;
	SecondGate = GateB;
	GateA->PairWith(GateB, PairExpire);

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::AuxActive);
	Writable.Flags |= TraceAbilityFlags::Charged;
	Writable.AuxLocation = MouthA;
	MarkStateDirty();

	return true;
}
#endif

// =================================================================================================
// PASSIVE 2 — the well-timed slide-jump gain
//
// NOT WIRED IN. See the header: the movement component has no per-pawn seam for this number and
// Movement/ is another agent's file this pass. This function is the whole of Elle's half, and it is
// deliberately a pure function of the shipped global so that the one line which eventually calls it
// cannot introduce a second opinion about what "the bonus" is.
// =================================================================================================

float UTraceAbilitySetElle::GetSlideJumpWindowSpeedBonusForElle(const AActor* Actor, float GlobalWellTimedBonus)
{
	// Everybody who is not Elle keeps the number they have. This is what makes the passive ELLE ONLY
	// — spec v18 §4's "do not regress ... slide-jump 1.446875 (Elle changes only her own)".
	const UTraceCharacterAbilitySet* const Set = UTraceAbilityComponent::GetAbilitySetFor(Actor);
	if (Set == nullptr || Set->GetCharacterId() != ETraceCharacterId::Elle)
	{
		return GlobalWellTimedBonus;
	}

	// THE GAIN IS SCALED, THE MULTIPLIER IS NOT. 1.446875 is 1 + 0.446875 of gain; +40% of the gain is
	// 0.446875 x 1.40 = 0.625, so Elle's is 1.625. Scaling the whole multiplier would give 2.025 — an
	// ability more than twice the size of the one §2 asked for, and one that would beat DashSpeed off
	// a fast slide, which is the inversion spec v9 §7 already refused for everybody.
	//
	// Never floored below the global: a passive that says "+40%" must not be able to punish her if
	// somebody sets the bonus negative.
	const float Gain = FMath::Max(0.f, GlobalWellTimedBonus - 1.f);
	const float Scaled = 1.f + Gain * (1.f + FMath::Max(0.f, UTraceSettings::Get().ElleSlideJumpGainBonus));

	return FMath::Max(GlobalWellTimedBonus, Scaled);
}

float UTraceAbilitySetElle::ModifySlideJumpWindowSpeedBonus(float InWellTimedBonus) const
{
	// THE SEAM THE MOVEMENT COMPONENT ACTUALLY CALLS (v18 §2 integration pass). It reaches this
	// through UTraceAbilityComponent::GetSlideJumpWindowSpeedBonusFor(), so Movement/ never learns
	// Elle's name — the same shape as GetDashHitSweepRadius() and Chut.
	//
	// It forwards to the static above rather than repeating the arithmetic, so the number the harness
	// measures and the number a slide-jump actually launches at have ONE definition and cannot drift.
	// The static's own "is this actor Elle?" test is redundant on this path (we are Elle, or this
	// override would not be running) and is left in place because the harness calls it with other
	// pawns on purpose, to prove the passive is Elle-only.
	return GetSlideJumpWindowSpeedBonusForElle(GetCharacter(), InWellTimedBonus);
}

// =================================================================================================
// ACTIVATED — SNAP
// =================================================================================================

float UTraceAbilitySetElle::GetSnapReadyMatchTime() const
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	const FTraceAbilityNetState& Current = State();

	if ((Current.Flags & TraceAbilityFlags::AuxActive) == 0)
	{
		return Current.AuxEndMatchTime;
	}

	// Mid-cast: AuxEndMatchTime is the END OF THE 4 s WINDOW, not a ready time. Derive what the ready
	// time WILL be, so that a press landing in the up-to-50 ms gap between the window lapsing and the
	// ability tick noticing is refused rather than handed a free gate.
	const float Window = FMath::Max(0.1f, Settings.ElleSnapSecondGateWindowSeconds);
	const float FirstPressAt = Current.AuxEndMatchTime - Window;

	return FMath::Max(Current.AuxEndMatchTime, FirstPressAt + FMath::Max(0.f, Settings.ElleSnapCooldownSeconds));
}

bool UTraceAbilitySetElle::IsAwaitingSecondGate() const
{
	const float Now = MatchTimeNow();
	const FTraceAbilityNetState& Current = State();

	if ((Current.Flags & TraceAbilityFlags::AuxActive) != 0)
	{
		return Now < Current.AuxEndMatchTime;
	}

	// CLIENT PREDICTION, and it is load-bearing rather than a nicety. The replicated flag above is one
	// round trip behind the press that set it, and during that round trip a remote client that
	// believed the cast was over would grey its own cooldown ring out — making the SECOND GATE
	// unreachable for anybody who is not the host. Zero on a server, where the state is the truth.
	return Now < LocalFirstGatePredictionEnd;
}

bool UTraceAbilitySetElle::CanActivate(FText& OutReason) const
{
	// The completion press is ALWAYS legal inside the window. It is the second half of one cast, not
	// a second cast — the same reading Mace's spike reactivation ships with.
	if (IsAwaitingSecondGate())
	{
		return true;
	}

	const float Remaining = GetSnapReadyMatchTime() - MatchTimeNow();
	if (Remaining > 0.f)
	{
		// Reached mainly after a FLUFFED cast (gate A placed, no second gate inside the window), where
		// the framework's own cooldown is genuinely zero because press 1 charges nothing.
		//
		// THE HUD AND THIS FUNCTION USED TO DISAGREE HERE and no longer do: the v18 §2 integration pass
		// added GetCharacterOwnedCooldownRemaining() below, which
		// UTraceAbilityComponent::GetActivatedCooldownRemaining() folds into the number the ring draws.
		// The ring counts the same seconds this refusal counts. OutReason is still written, for
		// whatever eventually surfaces a refusal in words.
		OutReason = FText::FromString(FString::Printf(TEXT("SNAP recharging (%.0fs)"), FMath::CeilToFloat(Remaining)));
		return false;
	}

	return true;
}

float UTraceAbilitySetElle::GetActivatedCooldownSeconds() const
{
	// Read at the point of use, never cached — the whole settings page is live-editable during PIE.
	const float Full = FMath::Max(0.f, UTraceSettings::Get().ElleSnapCooldownSeconds);

	// CDO-SAFE, AND THAT IS A REQUIREMENT RATHER THAN DEFENSIVENESS. Trace.VerifyCharacterData
	// section D asks the CLASS DEFAULT OBJECT for the enforced cooldown and compares it against the
	// 35 s the select card prints. A CDO has no component and no state, so the honest answer there is
	// the published number.
	if (GetAbilityComponent() == nullptr)
	{
		return Full;
	}

	// PRESS 1 CHARGES NOTHING, so that press 2 can reach ActivateAbility() at all — the framework
	// checks the cooldown before it asks the character anything. See the header for why this is the
	// only route available from inside a character file.
	if (IsAwaitingSecondGate())
	{
		return 0.f;
	}

	// PRESS 2 CHARGES THE REMAINDER, measured from press 1, so the ring counts 35 s down from the
	// start of the cast rather than restarting it at the end.
	const float Remaining = GetSnapReadyMatchTime() - MatchTimeNow();
	if (Remaining > 0.f)
	{
		return FMath::Min(Remaining, Full);
	}

	return Full;
}

float UTraceAbilitySetElle::GetCharacterOwnedCooldownRemaining() const
{
	// WHAT THE HUD RING HAS TO DRAW, AND WHY THE FRAMEWORK CANNOT KNOW IT ON ITS OWN. Press 1 of a
	// SNAP cast charges no framework cooldown (GetActivatedCooldownSeconds above returns 0 while the
	// window is open, or press 2 could never reach the ability). So after a cast that is never
	// completed the framework's timer is zero while CanActivate() refuses for up to 31 s — a lit
	// button that does nothing. This is the number that closes that gap.
	//
	// ZERO WHILE THE 4 S WINDOW IS OPEN, deliberately: the ability really is available then, and it is
	// the ONLY thing the player should be doing. Greying the ring mid-cast would tell them the
	// opposite of the truth at the one moment it matters.
	if (IsAwaitingSecondGate())
	{
		return 0.f;
	}

	// CDO-safe for the same reason GetActivatedCooldownSeconds() is: the verifier asks the class
	// default object, which has no component and therefore no cast in progress.
	if (GetAbilityComponent() == nullptr)
	{
		return 0.f;
	}

	return FMath::Max(0.f, GetSnapReadyMatchTime() - MatchTimeNow());
}

ATraceElleGate* UTraceAbilitySetElle::GetFirstGate() const  { return FirstGate.Get(); }
ATraceElleGate* UTraceAbilitySetElle::GetSecondGate() const { return SecondGate.Get(); }

ATraceElleGate* UTraceAbilitySetElle::PlaceGate(const FVector& Mouth, float ExpireMatchTime, bool bSecondOfPair)
{
	UWorld* WorldPtr = GetWorld();
	ATraceCharacter* MyPawn = GetCharacter();
	if (WorldPtr == nullptr || MyPawn == nullptr)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Instigator = MyPawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	ATraceElleGate* Gate = WorldPtr->SpawnActor<ATraceElleGate>(
		ATraceElleGate::StaticClass(), Mouth, FRotator::ZeroRotator, SpawnParams);
	if (Gate == nullptr)
	{
		return nullptr;
	}

	APlayerState* SourceState = (GetAbilityComponent() != nullptr)
		? GetAbilityComponent()->GetOwningPlayerState()
		: nullptr;

	Gate->InitialiseGate(SourceState, MyPawn->GetTeam(), Mouth,
		UTraceSettings::Get().ElleSnapGateRadiusUU, ExpireMatchTime, bSecondOfPair);

	return Gate;
}

void UTraceAbilitySetElle::DestroyGates()
{
	if (ATraceElleGate* Gate = FirstGate.Get())
	{
		Gate->Destroy();
	}
	if (ATraceElleGate* Gate = SecondGate.Get())
	{
		Gate->Destroy();
	}
	FirstGate = nullptr;
	SecondGate = nullptr;
}

bool UTraceAbilitySetElle::ActivateAbility()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr)
	{
		return false;   // a fizzle: no pawn. Do not charge for it.
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Now = MatchTimeNow();
	const float Window = FMath::Max(0.1f, Settings.ElleSnapSecondGateWindowSeconds);

	const bool bCompletingPair = IsAwaitingSecondGate();

	if (!HasAuthority())
	{
		// PREDICTED HALF. Nothing is spawned locally — the gates are replicated actors, and a locally
		// spawned one would put two mouths on one machine, both drawing and one of them a lie.
		//
		// What IS predicted is WHICH PRESS THIS WAS, because that is the input to
		// GetActivatedCooldownSeconds() and therefore to whether this client's cooldown ring greys
		// out. Predicting it wrong on press 1 is what would make the second gate unreachable for
		// every player who is not the host.
		LocalFirstGatePredictionEnd = bCompletingPair ? 0.f : (Now + Window);
		return true;
	}

	if (CVarElleSnapEnabled.GetValueOnAnyThread() == 0)
	{
		// RED ARM: charge the FULL cooldown, place nothing, touch no state. Everything else about the
		// two arms is identical.
		return true;
	}

	const FVector Mouth = MyPawn->GetActorLocation();

	// ---- PRESS 1: "places a portal gate where she stands" ----------------------------------------
	if (!bCompletingPair || !FirstGate.IsValid())
	{
		// Belt and braces: a pair still standing from an earlier cast is torn down rather than
		// orphaned, so "at most two gates exist" is a property of the code rather than of the timing.
		DestroyGates();

		const float WindowEnd = Now + Window;

		ATraceElleGate* GateA = PlaceGate(Mouth, WindowEnd, /*bSecondOfPair=*/false);
		if (GateA == nullptr)
		{
			return false;   // the world refused the spawn: a fizzle, and she is not charged for it
		}
		FirstGate = GateA;

		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags |= TraceAbilityFlags::AuxActive;
		Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::Charged);
		Writable.AuxEndMatchTime = WindowEnd;
		Writable.AuxLocation = Mouth;
		MarkStateDirty();

		UE_LOG(LogTraceGame, Log,
			TEXT("[Elle] SNAP gate 1 placed by %s at (%s), radius %.0f uu. Press again within %.1fs for the "
			     "second gate; the window closes at match time %.2f."),
			*GetNameSafe(MyPawn), *Mouth.ToCompactString(), Settings.ElleSnapGateRadiusUU, Window, WindowEnd);

		return true;
	}

	// ---- PRESS 2: "reactivate within 4 s to place a second gate, and then players teleport" -------
	const float PairExpire = Now + FMath::Max(0.f, Settings.ElleSnapPairLifetimeSeconds);

	ATraceElleGate* GateB = PlaceGate(Mouth, PairExpire, /*bSecondOfPair=*/true);
	if (GateB == nullptr)
	{
		return false;
	}
	SecondGate = GateB;

	// PairWith rewrites BOTH deadlines to the pair's, and seeds the re-entry lockout for anybody
	// standing in either mouth — including Elle, who is stood in this one by construction.
	FirstGate->PairWith(GateB, PairExpire);

	const float ReadyAt = GetSnapReadyMatchTime();

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::AuxActive);
	Writable.Flags |= TraceAbilityFlags::Charged;
	Writable.AuxEndMatchTime = ReadyAt;
	MarkStateDirty();

	UE_LOG(LogTraceGame, Log,
		TEXT("[Elle] SNAP PAIR live: (%s) <-> (%s), radius %.0f uu, both expire at match time %.2f (%.1fs). "
		     "Usable by both teams: %d. A carrier may use one voluntarily: %d. Cooldown ready at %.2f."),
		*FirstGate->GetMouthLocation().ToCompactString(), *Mouth.ToCompactString(),
		Settings.ElleSnapGateRadiusUU, PairExpire, Settings.ElleSnapPairLifetimeSeconds,
		Settings.bElleSnapUsableByBothTeams ? 1 : 0, Settings.bElleSnapCarrierMayUseGate ? 1 : 0, ReadyAt);

	return true;
}
