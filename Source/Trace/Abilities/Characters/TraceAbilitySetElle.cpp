// Trace — Elle. See the header for the clause-by-clause reading of spec v18 §2, for the two
// [ASSUMPTION]s the user is most likely to reverse, and for the one place SNAP does not fit the
// ability framework cleanly.

#include "Abilities/Characters/TraceAbilitySetElle.h"

#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/Characters/TraceElleGate.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"
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
		// ...WITH ONE EXCEPTION, AND IT IS DEMO 17's. The second mouth may not land on top of the first.
		//
		// A player whose first press looks like it did nothing presses again ON THE SPOT, and that used
		// to produce two mouths inside one gate radius: a "portal" whose two ends are the same place,
		// which teleports you to where you already are and costs the full 35 s. Refusing costs her
		// NOTHING — TryActivate charges no cooldown for a false CanActivate, and the 4 s window keeps
		// running — so the cast is not wasted, it is waiting for her to move.
		//
		// Answered on the client too: AuxLocation is the replicated first mouth, so the owning client
		// refuses its own press instead of predicting a gate the server is about to decline.
		const float MinSeparation = FMath::Max(0.f, UTraceSettings::Get().ElleSnapMinimumMouthSeparationUU);
		const ATraceCharacter* MyPawn = GetCharacter();
		if (MinSeparation > 0.f && MyPawn != nullptr)
		{
			const float Separation = static_cast<float>(
				FVector::Dist(MyPawn->GetActorLocation(), FVector(State().AuxLocation)));
			if (Separation < MinSeparation)
			{
				OutReason = FText::FromString(FString::Printf(
					TEXT("SNAP: move %.0f uu further from the first gate"), MinSeparation - Separation));

				// SAID OUT LOUD, once per press, on the authority. "I pressed E and the second gate did
				// not appear" is otherwise unanswerable from a log — and nothing surfaces OutReason on
				// screen yet (see the note in the report), so this line is currently the only place the
				// reason exists at all.
				if (HasAuthority())
				{
					UE_LOG(LogTraceGame, Log,
						TEXT("[Elle] SNAP second gate REFUSED: she is %.0f uu from the first mouth and needs "
						     "%.0f. The cast is NOT charged and the window is still open — walk away and press "
						     "again."),
						Separation, MinSeparation);
				}
				return false;
			}
		}

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

#if !UE_BUILD_SHIPPING

// =================================================================================================
// Trace.Elle.SnapPressTest — SNAP AS A PLAYER ACTUALLY DRIVES IT, ON THE MACHINE THEY SIT AT
//
// ===================================================================================================
// WHY THIS EXISTS WHEN Trace.Elle.Verify ALREADY REPORTED SNAP "PROVEN"
// ===================================================================================================
//
// Demo 17, verbatim: "Elle's snap ability isn't opening a portal and isn't reactivatable, it doesn't
// work at all." Trace.Elle.Verify reported 20 passed / 0 failed on the same build. It was not lying
// about anything it measured; it was measuring somewhere the player never stands. Two gaps, and the
// second one is the one that mattered:
//
//   1. IT PRESSES A FUNCTION, NOT A KEY. It calls Elle->TryActivate() directly, so everything above
//      that function is untested: the E bind, ATracePlayerController::OnAbilityStarted (which drops
//      the press whole while game input is suppressed), TraceAbilityIntegration::IsEnabled() and
//      UTraceAbilityInputRelay::RouteActivatePressed. This command injects a REAL KEY through the
//      real input pipeline instead — the same injector Trace.SimInput uses.
//
//   2. *** IT ONLY EVER RAN ON THE SERVER, AND SNAP'S SECOND PRESS IS A PREDICTION. *** Everything
//      the harness asked about — GetFirstGate(), GetSecondGate() — is a SERVER-SIDE pointer that is
//      always null on a client, so the whole activated ability was unmeasured on the machine most
//      players are actually on. A listen-server host cannot find that class of bug about itself, which
//      is the same argument Trace.Elle.RemoteCloak already makes for the cloak.
//
// So this command judges from THE WORLD, not from Elle's private bookkeeping: it counts the gate
// actors that are actually standing, filtered to the ones this player placed, and asks whether they
// are paired and whether their rings were built. Those are exactly the two things the user's sentence
// is about — "opening a portal" and being able to see it — and they are answerable identically on a
// server and on a client.
//
// RED ARM: `Trace.Elle.SnapEnabled 0`. The key still arrives and the press is still accepted; nothing
// is placed. Every gate assertion below must go red, which is the shape of the reported failure.
// =================================================================================================

namespace TraceAbilitySetElleFile
{
	/** What is actually standing in the world, which is what the player can see. */
	struct FSnapGateCensus
	{
		/** Gates placed by the player under test. */
		int32 Mine = 0;
		/** ...of which have a partner, i.e. are half of a real portal rather than a lone marker. */
		int32 MinePaired = 0;
		/** Ring beads built across those gates on THIS machine. 0 = there is nothing to look at. */
		int32 MineBeads = 0;
		/** Every gate in the world, including other Elles'. Context for a confusing run. */
		int32 Total = 0;

		FString Describe() const
		{
			return FString::Printf(TEXT("mine=%d paired=%d beads=%d (all Elles: %d)"),
				Mine, MinePaired, MineBeads, Total);
		}
	};

	FSnapGateCensus CensusGates(UWorld* WorldPtr, const APlayerState* Placer)
	{
		FSnapGateCensus Out;
		if (WorldPtr == nullptr)
		{
			return Out;
		}

		for (TActorIterator<ATraceElleGate> It(WorldPtr); It; ++It)
		{
			const ATraceElleGate* Gate = *It;
			if (Gate == nullptr || !IsValid(Gate))
			{
				continue;
			}
			++Out.Total;

			// THE PLACER IS THE FILTER, and it has to be, because bots run Elle too: a run that counted
			// every gate in the arena would pass on a build where the player's own key did nothing at all
			// and a bot happened to cast in the same second. SourcePlayerState is replicated, so this
			// question has the same answer on a client as on the server.
			if (Placer != nullptr && Gate->GetSourcePlayerState() != Placer)
			{
				continue;
			}

			++Out.Mine;
			Out.MineBeads += Gate->GetDrawnBeadCount();
			if (Gate->IsPaired())
			{
				++Out.MinePaired;
			}
		}

		return Out;
	}

	struct FSnapPressState
	{
		int32 Phase = 0;
		double PhaseStartReal = 0.0;
		double AcquireDeadline = 0.0;
		float GapSeconds = 1.2f;
		/** How long to let a press land before reading the world. A client needs a round trip. */
		float SettleSeconds = 0.8f;

		TWeakObjectPtr<UTraceAbilityComponent> Elle;
		TWeakObjectPtr<APlayerState> ElleState;
		bool bAuthority = false;

		int32 PressesDelivered = 0;

		FSnapGateCensus AfterFirst;
		bool bAwaitingAfterFirst = false;
		float CooldownAfterFirst = -1.f;

		FSnapGateCensus AfterSecond;
		float CooldownAfterPair = -1.f;

		/** How far the fixture managed to get the two mouths apart, in uu. See the walk in phase 3. */
		float MouthSeparation = -1.f;

		/** Whether the movement key has been pressed for this cast's walk. See phase 3. */
		bool bWalkStarted = false;

		/** Cases the fixture could not stage. Reported apart from failures — they are not defects. */
		int32 Invalid = 0;

		/** Where she was left standing after the pair completed, and how far the pair moved her. */
		FVector RestPosition = FVector::ZeroVector;
		float DriftWhileStandingStill = -1.f;

		/**
		 * Whether she was actually INSIDE one of her own mouths when the stand-still watch began.
		 *
		 * The watch is meaningless otherwise: a player stood outside both mouths is not moved by either
		 * arm, so "she did not move" would be green on the build this fix exists to correct. Measured —
		 * a fixture that walked her too far coasted her out of her own gate and the red arm silently
		 * stopped reproducing.
		 */
		bool bRestingInsideOwnMouth = false;

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[ELLEPRESS]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	/**
	 * THE WORLD THIS MACHINE PLAYS IN — which is deliberately NOT "the authoritative world".
	 *
	 * Every other harness in this file asks for the server's world, and that is exactly the habit that
	 * left the whole activated ability unmeasured on a client. Here the only requirement is a game world
	 * with somebody sitting at it, because a key press needs a keyboard.
	 */
	UWorld* FindLocalGameWorldForPressTest()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetFirstPlayerController() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** The LOCAL player's ability component — the one the E key on this keyboard reaches. */
	UTraceAbilityComponent* FindLocalAbilityComponent(UWorld* WorldPtr)
	{
		const APlayerController* LocalPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		APlayerState* LocalState = (LocalPC != nullptr) ? LocalPC->PlayerState : nullptr;
		return (LocalState != nullptr) ? LocalState->FindComponentByClass<UTraceAbilityComponent>() : nullptr;
	}

	/**
	 * ONE PRESS OF E, THROUGH THE REAL KEY.
	 *
	 * Trace.SimInput injects through the engine's own input pipeline, so what this delivers is a key
	 * press and not a function call — which is the whole difference between this command and the one
	 * that reported PASS while the player had nothing.
	 */
	void PressAbilityKey(UWorld* WorldPtr)
	{
		APlayerController* LocalPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		if (LocalPC == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ELLEPRESS] no local player controller — a key cannot be pressed on a machine nobody is "
				     "sitting at."));
			return;
		}
		LocalPC->ConsoleCommand(TEXT("Trace.SimInput E 0.05"), /*bWriteToLog=*/false);
	}

	void RunElleSnapPressTest(const TArray<FString>& Args)
	{
		UWorld* WorldPtr = FindLocalGameWorldForPressTest();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ELLEPRESS] no local game world with a player in it — this command presses a KEY, so it "
				     "needs a machine somebody is sitting at (a host or a client, not a dedicated server)."));
			return;
		}
		if (WorldPtr->IsPaused())
		{
			if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
			{
				FirstPC->SetPause(false);
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ELLEPRESS] The world was PAUSED (the select screen does that). Unpaused, or every "
					     "measurement below would have been a zero that looks like a pass."));
			}
		}

		TSharedPtr<FSnapPressState> State = MakeShared<FSnapPressState>();
		State->GapSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 0.1f, 30.f) : 1.2f;
		State->SettleSeconds = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.1f, 5.f) : 0.8f;
		State->PhaseStartReal = FPlatformTime::Seconds();
		State->AcquireDeadline = State->PhaseStartReal + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLEPRESS] ===== Demo 17 item 1. Two presses of the REAL E KEY %.2fs apart on THIS machine "
			     "(netMode=%d), judged on the gates actually standing in the world. Red arm: "
			     "Trace.Elle.SnapEnabled 0. ====="),
			State->GapSeconds, static_cast<int32>(WorldPtr->GetNetMode()));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ELLEPRESS] ABORTED: the world went away."));
				return false;
			}

			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: stage -----------------------------------------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Local = FindLocalAbilityComponent(TickWorld);
				if (Local != nullptr && Local->GetCharacterId() != ETraceCharacterId::Elle)
				{
					// EACH MACHINE ASKS THE WAY IT IS ALLOWED TO. A client has no business writing
					// CharacterId, and calling ServerSetCharacter there logs a refusal and changes
					// nothing — so a client run would have staged nothing and reported INVALID forever.
					if (Local->GetOwner() != nullptr && Local->GetOwner()->HasAuthority())
					{
						Local->ServerSetCharacter(ETraceCharacterId::Elle);
					}
					else
					{
						Local->ServerRequestSetCharacter(ETraceCharacterId::Elle);
					}
				}

				UTraceAbilitySetElle* ElleSet = (Local != nullptr)
					? Local->GetAbilitySetAs<UTraceAbilitySetElle>() : nullptr;
				ATraceCharacter* EllePawn = (Local != nullptr) ? Local->GetOwningCharacter() : nullptr;

				// THE SELECT SCREEN SWALLOWS A PRESS, AND THAT IS THE FIXTURE'S PROBLEM, NOT THE GAME'S.
				// ATracePlayerController::OnAbilityStarted returns early while game input is suppressed,
				// which it is for as long as the screen is up — and the screen closes a beat AFTER the
				// staging above gives the player a character. A press delivered into that beat vanishes
				// and the run reads exactly like a dead ability. This was measured, not imagined: the
				// first version of this command reported a false FAIL for precisely that reason.
				const ATracePlayerController* LocalTracePC =
					Cast<ATracePlayerController>(TickWorld->GetFirstPlayerController());
				const bool bInputLive = (LocalTracePC != nullptr) && !LocalTracePC->IsGameInputSuppressed()
					&& !TickWorld->IsPaused();

				if (ElleSet == nullptr || EllePawn == nullptr || !EllePawn->IsAlive() || !bInputLive)
				{
					if (NowReal > State->AcquireDeadline)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[ELLEPRESS] VERDICT: INVALID — could not stage (elleSet=%d livingPawn=%d "
							     "inputLive=%d). Run it EARLY in a match, with characters ON, before a bot "
							     "team-mate claims Elle."),
							(ElleSet != nullptr) ? 1 : 0, (EllePawn != nullptr && EllePawn->IsAlive()) ? 1 : 0,
							bInputLive ? 1 : 0);
						return false;
					}
					return true;
				}

				State->Elle = Local;
				State->ElleState = Local->GetOwningPlayerState();
				State->bAuthority = (Local->GetOwner() != nullptr) && Local->GetOwner()->HasAuthority();
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			UTraceAbilityComponent* Elle = State->Elle.Get();
			UTraceAbilitySetElle* ElleSet = (Elle != nullptr)
				? Elle->GetAbilitySetAs<UTraceAbilitySetElle>() : nullptr;
			if (Elle == nullptr || ElleSet == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ELLEPRESS] ABORTED: Elle went away mid-test."));
				return false;
			}

			// ---- Phase 1: PRESS 1, on the key ---------------------------------------------------
			if (State->Phase == 1)
			{
				PressAbilityKey(TickWorld);
				++State->PressesDelivered;
				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 2: what the first press left in the world ----------------------------------
			if (State->Phase == 2)
			{
				if ((NowReal - State->PhaseStartReal) < static_cast<double>(State->SettleSeconds))
				{
					return true;   // a key event, then (on a client) a round trip, then an actor channel
				}

				State->AfterFirst = CensusGates(TickWorld, State->ElleState.Get());
				State->bAwaitingAfterFirst = ElleSet->IsAwaitingSecondGate();
				State->CooldownAfterFirst = Elle->GetActivatedCooldownRemaining();

				State->Phase = 3;
				return true;
			}

			// ---- Phase 3: WALK HER — on the movement key, not on a teleport — then PRESS 2 -------
			//
			// SHE HAS TO GO SOMEWHERE, and that is a statement about the ability rather than about the
			// fixture: since Demo 17 the second mouth is REFUSED on top of the first, because two mouths
			// inside one gate radius are not a portal. A harness that pressed twice on the spot would be
			// measuring the refusal.
			//
			// *** AND SHE HAS TO WALK, NOT BE TELEPORTED, OR THE CLIENT HALF OF THIS TEST IS A LIE. ***
			// The first version called TeleportTo, which is authoritative only on the server: run from a
			// client it moved the local pawn 780 uu while the SERVER still had her stood on gate A, so
			// the server refused the second mouth for being on top of the first and the run reported a
			// product failure that was entirely the fixture's. Holding the movement key is the player's
			// own path and replicates properly on both machines — and if she walks into a wall, the
			// separation below simply is not met and the run says INVALID instead of FAIL.
			if (State->Phase == 3)
			{
				if (!State->bWalkStarted)
				{
					State->bWalkStarted = true;
					if (APlayerController* LocalPC = TickWorld->GetFirstPlayerController())
					{
						// HALF the gap, so the second half is deceleration: she has to be STOOD STILL
						// when she places the second mouth, or she coasts out of it before the
						// stand-still watch below even starts and that assertion passes for the worst
						// possible reason — nobody was in a mouth in either arm. Measured: a 0.75-gap
						// walk left her outside her own mouth and made the red arm stop reproducing.
						LocalPC->ConsoleCommand(
							FString::Printf(TEXT("Trace.SimInput D %.2f"), State->GapSeconds * 0.5f),
							/*bWriteToLog=*/false);
					}
					return true;
				}

				if ((NowReal - State->PhaseStartReal) < static_cast<double>(State->GapSeconds))
				{
					return true;
				}

				if (const ATraceCharacter* EllePawn = Elle->GetOwningCharacter())
				{
					State->MouthSeparation = static_cast<float>(
						FVector::Dist(EllePawn->GetActorLocation(), FVector(Elle->GetNetState().AuxLocation)));
				}

				PressAbilityKey(TickWorld);
				++State->PressesDelivered;
				State->Phase = 4;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 4: what the second press left in the world ----------------------------------
			if (State->Phase == 4)
			{
				if ((NowReal - State->PhaseStartReal) < static_cast<double>(State->SettleSeconds))
				{
					return true;
				}

				State->AfterSecond = CensusGates(TickWorld, State->ElleState.Get());
				State->CooldownAfterPair = Elle->GetActivatedCooldownRemaining();

				if (const ATraceCharacter* EllePawn = Elle->GetOwningCharacter())
				{
					State->RestPosition = EllePawn->GetActorLocation();

					for (TActorIterator<ATraceElleGate> It(TickWorld); It; ++It)
					{
						const ATraceElleGate* Gate = *It;
						if (Gate != nullptr && IsValid(Gate)
							&& Gate->GetSourcePlayerState() == State->ElleState.Get()
							&& Gate->IsInsideMouth(EllePawn))
						{
							State->bRestingInsideOwnMouth = true;
							break;
						}
					}
				}

				State->Phase = 45;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 45: STAND STILL IN HER OWN MOUTH, WHICH IS THE OTHER HALF OF THE BUG ---------
			//
			// Elle is by construction standing in the second mouth she just placed. Before Demo 17 the
			// pair then threw her to the far mouth every time the 1 s re-entry lockout lapsed, for the
			// pair's whole 8 s life — measured at seven teleports in seven seconds. A gate now only fires
			// on the frame somebody STEPS IN, so a player who does not move is not moved.
			// Trace.Elle.SnapStepIn 0 restores the old poll and makes this assertion go red.
			if (State->Phase == 45)
			{
				if (const ATraceCharacter* EllePawn = Elle->GetOwningCharacter())
				{
					const float Drift =
						static_cast<float>(FVector::Dist(EllePawn->GetActorLocation(), State->RestPosition));
					State->DriftWhileStandingStill = FMath::Max(State->DriftWhileStandingStill, Drift);
				}

				// Long enough to cross the re-entry lockout several times over, and comfortably inside
				// the pair's 8 s so the gates are still standing while it is watched.
				const double Watch = FMath::Min(4.0, static_cast<double>(UTraceSettings::Get().ElleSnapPairLifetimeSeconds) - 1.0);
				if ((NowReal - State->PhaseStartReal) < Watch)
				{
					return true;
				}

				State->Phase = 5;
				return true;
			}

			// ---- Phase 5: the verdict --------------------------------------------------------------
			const UTraceSettings& Knobs = UTraceSettings::Get();

			UE_LOG(LogTraceGame, Display,
				TEXT("[ELLEPRESS] machine=%s | key presses delivered=%d | after press 1: %s awaiting=%d "
				     "cooldown=%.2fs | after press 2: %s cooldown=%.2fs"),
				State->bAuthority ? TEXT("SERVER/HOST") : TEXT("CLIENT"), State->PressesDelivered,
				*State->AfterFirst.Describe(), State->bAwaitingAfterFirst ? 1 : 0, State->CooldownAfterFirst,
				*State->AfterSecond.Describe(), State->CooldownAfterPair);

			State->Check(State->AfterFirst.Mine >= 1,
				TEXT("PRESSING E OPENS A GATE where she stands — the first half of 'isn't opening a portal'"));
			State->Check(State->AfterFirst.MineBeads > 0,
				FString::Printf(TEXT("...and it is actually DRAWN on this machine (%d ring beads), so there is "
				                     "something on screen to walk into"), State->AfterFirst.MineBeads));
			State->Check(State->bAwaitingAfterFirst,
				FString::Printf(TEXT("the %.1fs second-gate window is open after press 1, on this machine — the "
				                     "prediction a client needs or the second press is unreachable off the host"),
					Knobs.ElleSnapSecondGateWindowSeconds));
			State->Check(State->CooldownAfterFirst <= 0.01f,
				FString::Printf(TEXT("E reads READY after press 1 (%.2fs) — a cooling ring here is a second press "
				                     "the player will never make"), State->CooldownAfterFirst));

			// A WALK THAT WENT NOWHERE IS A BROKEN FIXTURE, NOT A BROKEN PORTAL. Since Demo 17 the second
			// mouth is refused inside the minimum separation, so if she could not get clear of the first
			// one — walked into a wall, was shot, was slowed — the three assertions below would be
			// measuring that refusal working, and reporting them as failures would be the worst kind of
			// red: a product defect that is not one.
			const bool bWalkedFarEnough = (State->AfterFirst.Mine >= 1)
				&& (State->MouthSeparation >= Knobs.ElleSnapMinimumMouthSeparationUU);
			if (!bWalkedFarEnough)
			{
				++State->Invalid;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ELLEPRESS]   INVALID  the fixture only got her %.0f uu from the first mouth, inside "
					     "the %.0f uu minimum, so the second press was refused BY DESIGN and 'is it "
					     "reactivatable' has no answer this run. Run it somewhere she can walk 300 uu."),
					State->MouthSeparation, Knobs.ElleSnapMinimumMouthSeparationUU);
			}

			State->Check(!bWalkedFarEnough || State->AfterSecond.Mine >= 2,
				TEXT("*** PRESSING E AGAIN INSIDE THE WINDOW IS REACTIVATABLE: a second mouth appears *** — the "
				     "half Demo 17 reported dead"));
			State->Check(!bWalkedFarEnough || State->AfterSecond.MinePaired >= 2,
				FString::Printf(TEXT("...and the two mouths are PAIRED (%d of %d), which is what makes them a "
				                     "portal rather than two markers"),
					State->AfterSecond.MinePaired, State->AfterSecond.Mine));
			State->Check(!bWalkedFarEnough || State->AfterSecond.MineBeads > State->AfterFirst.MineBeads,
				FString::Printf(TEXT("...and the second mouth is drawn too (%d beads, up from %d)"),
					State->AfterSecond.MineBeads, State->AfterFirst.MineBeads));
			State->Check(!bWalkedFarEnough || (State->AfterSecond.MinePaired >= 2
				&& State->CooldownAfterPair > (Knobs.ElleSnapCooldownSeconds - 6.f)
				&& State->CooldownAfterPair <= Knobs.ElleSnapCooldownSeconds + 0.01f),
				FString::Printf(TEXT("a COMPLETED cast then charges the %.0fs cooldown, and this machine knows it "
				                     "(%.2fs left)"), Knobs.ElleSnapCooldownSeconds, State->CooldownAfterPair));

			// QUALIFIED ON A GATE HAVING BEEN PLACED, and that is not pedantry — it is what the red arm
			// measured. Under Trace.Elle.SnapEnabled 0 nothing is placed, AuxLocation is never written and
			// the "separation" is Elle's distance from the world origin, which sailed past any threshold
			// and reported PASS in an arm where the whole ability was switched off.
			State->Check(bWalkedFarEnough,
				FString::Printf(TEXT("the two mouths are %.0f uu apart, i.e. at least the %.0f uu minimum — a "
				                     "'portal' whose ends are the same place is the other half of 'it doesn't "
				                     "work at all'"),
					State->MouthSeparation, Knobs.ElleSnapMinimumMouthSeparationUU));
			// QUALIFIED ON HER ACTUALLY BEING IN A MOUTH, and that is not pedantry: a player stood
			// outside both mouths is not moved by the old proximity poll either, so without this the
			// assertion goes green on the very build it exists to catch. It cost one red arm that
			// silently stopped reproducing to learn.
			if (bWalkedFarEnough && State->AfterSecond.MinePaired >= 2 && !State->bRestingInsideOwnMouth)
			{
				++State->Invalid;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ELLEPRESS]   INVALID  she came to rest OUTSIDE both of her own mouths, so 'standing "
					     "still does not move her' has no answer this run — neither the shipped rule nor the "
					     "old proximity poll moves somebody who is not in a gate."));
			}

			State->Check(!bWalkedFarEnough || !State->bRestingInsideOwnMouth
				|| (State->AfterSecond.MinePaired >= 2
				&& State->DriftWhileStandingStill >= 0.f
				&& State->DriftWhileStandingStill < (Knobs.ElleSnapGateRadiusUU * 0.5f)),
				FString::Printf(TEXT("*** STANDING STILL IN HER OWN MOUTH DOES NOT MOVE HER (%.0f uu of drift over "
				                     "%.0fs) *** — a gate takes you when you STEP IN. Trace.Elle.SnapStepIn 0 makes "
				                     "this FAIL and nothing else"),
					State->DriftWhileStandingStill,
					FMath::Min(4.f, Knobs.ElleSnapPairLifetimeSeconds - 1.f)));

			UE_LOG(LogTraceGame, Display,
				TEXT("[ELLEPRESS] ===== %d passed, %d failed, %d could not be staged (INVALID) ====="),
				State->Passed, State->Failed, State->Invalid);
			if (State->Failed == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[ELLEPRESS] VERDICT: PASS — two presses of the real key open a portal this machine can "
					     "see and step through."));
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[ELLEPRESS] VERDICT: *** FAIL *** (%d)"), State->Failed);
			}
			return false;
		}));
	}

	FAutoConsoleCommand CmdElleSnapPressTest(
		TEXT("Trace.Elle.SnapPressTest"),
		TEXT("DEMO 17 item 1. Presses the REAL E key twice, GapSeconds apart (default 1.2, settle 0.8), through "
		     "the real input pipeline, and reports from the WORLD whether a portal opened, whether it is drawn "
		     "on this machine and whether the second press paired it. Runs on a host OR a client — the client "
		     "half is the one Trace.Elle.Verify structurally cannot cover. Red arm: Trace.Elle.SnapEnabled 0."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunElleSnapPressTest));
}

#endif   // !UE_BUILD_SHIPPING
