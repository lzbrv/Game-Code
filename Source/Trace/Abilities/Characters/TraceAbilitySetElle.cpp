// Trace — Elle. See the header for the clause-by-clause reading of spec v18 §2, for the two
// [ASSUMPTION]s the user is most likely to reverse, and for the one place SNAP does not fit the
// ability framework cleanly.

#include "Abilities/Characters/TraceAbilitySetElle.h"

#include "Camera/CameraActor.h"          // the FX parade's observer — a bare AActor has no root
#include "Camera/PlayerCameraManager.h"   // Trace.Elle.PortalShot frames the two mouths against the FOV
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
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"                 // FScreenshotRequest — the FX parade aims its own frames

#include "Materials/MaterialInstanceDynamic.h"

#include "Abilities/Characters/TraceElleGate.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"                // ElleCloak / ElleDecloak — World, server, at the two edges
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceFxBurst.h"           // TraceFxLoopBudget — the §1.4 attach choke point
#include "Gameplay/TraceFxShapes.h"
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
		FName(TEXT("EmissivePower")),       // M_Mannequin, and M_TraceBodyGlow (the team panels)
		FName(TEXT("EmissiveStrength")),    // M_TraceSurface
		FName(TEXT("Glow")),                // M_TraceNeon
		// M_TraceBodyAccent (generated bodies). Killing the glow IS the whole cloak effect for the
		// accent, which is why "AccentColor" is deliberately NOT in ColourParams above: the hue has
		// to survive so that restore needs no memory of what it was. Restore is the same
		// ApplyTeamColors() every other parameter here comes back through — see
		// ATraceCharacter::ApplyColorToSkeletalMesh, which writes this scalar from the same
		// 8/30/0 state value it writes EmissivePower from (PIPELINE_DESIGN.md §4.4).
		FName(TEXT("AccentGlow"))
	};

	/** The hook a translucent character material would pick up for free. No-op on every material today. */
	const FName OpacityParam(TEXT("Opacity"));

	// =============================================================================================
	// FX_AUDIO_PLAN §2.5 — THE TWO CLOAK SWEEPS
	// =============================================================================================

	/** Bible §2's semantic Cloak hue, #C9D8ED. NOT Elle's orchid accent: the state beats the owner. */
	const FLinearColor CloakSilver(0.60f, 0.70f, 0.85f, 1.f);

	/** §2.5: both sweeps run for 0.3 s. */
	constexpr float SweepSeconds = 0.3f;

	/** §2.5: "one flat ring (cylinder shell r 50 uu, h 4 uu)". */
	constexpr float SweepRadiusUU = 50.f;
	constexpr float SweepHeightUU = 4.f;

	/**
	 * §2.5's two intensities, and the asymmetry is the design: 0.5 going IN, 0.35 coming OUT, because
	 * "decloak must not out-shout the player it reveals". The reveal is information the enemy is owed;
	 * it is not a firework.
	 */
	constexpr float SweepOnIntensity = 0.5f;
	constexpr float SweepOffIntensity = 0.35f;

	/** Head and feet, uu relative to the capsule CENTRE — a standing Trace pawn is 88 uu each way. */
	constexpr float SweepTopUU = 88.f;
	constexpr float SweepBottomUU = -88.f;

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
	// The sweep is silenced BEFORE the visual comes down, so a character swap does not flash a decloak
	// on somebody who is no longer Elle: clearing the wanted flag means the call below sees no edge.
	bCloakVisualWanted = false;
	DetachCloakSweep();

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

	// The ring belonged to the pawn that is being destroyed on this same path; forget it rather than
	// trying to detach from it, exactly as the dim is forgotten rather than restored.
	bCloakVisualWanted = false;
	CloakSweep = nullptr;
	CloakSweepMID = nullptr;
	CloakSweepPawn = nullptr;
	bCloakSweepRunning = false;
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

	// §8.9: no FX component survives its pawn — and a decloak flash on a corpse would be a lie about
	// where she went. Both are settled before the visual half runs.
	bCloakVisualWanted = false;
	DetachCloakSweep();

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

	bCloakVisualWanted = false;
	DetachCloakSweep();

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
	// THE SWEEP IS TICKED FIRST, so the tick on which the on-sweep FINISHES is the tick the dim lands
	// — §2.5's "body dim applies at sweep end" with no frame of bright pawn in between.
	TickCloakSweep(DeltaSeconds);

	const bool bWantCloak = IsCloaked();

	// bCloakVisualWanted is in the condition as well as the other two, and it has to be: while the
	// on-sweep is delaying the dim, bCloakVisualApplied is still false, so a cloak that ENDS inside
	// those 0.3 s would otherwise never reach ApplyCloakVisual(false) — leaving the wanted flag set
	// and eating the NEXT cloak's rising edge.
	if (bWantCloak || bCloakVisualApplied || bCloakVisualWanted)
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

	// Read ONCE, here, and used by the cloak expiry below as well as by the two SNAP blocks: the
	// scratch pad is a reference, so a second binding further down would be the same object under a
	// second name and an invitation to write through one while reading the other.
	const FTraceAbilityNetState& Current = State();

	// *** THE GUARD IS THE FLAG, NOT IsCloaked(), AND THE DIFFERENCE IS A DEAD BRANCH. ***
	//
	// IsCloaked() is "the flag is up AND the deadline has not passed". Nesting the expiry test inside
	// it therefore asked whether the deadline had passed at a point where it provably had not, so
	// EndCloak(TEXT("expired")) — the way a cloak ends in almost every real match — was UNREACHABLE.
	// Nothing looked broken, because everything a player or a bot reads goes through IsCloaked() and
	// that answer was always right; what actually happened is that the EffectActive bit stayed set,
	// with a deadline in the past, until the next death wipe or half time.
	//
	// It surfaced here because FX §5.1 puts ElleDecloak at EndCloak, and a decloak sound that only
	// fires when she picks the Core back up or dies is a sound nobody would ever hear. Fixing the
	// guard is the whole fix: the two branches inside are unchanged, and the Core-pickup branch keeps
	// its own IsCloaked() test so an already-expired cloak cannot be reported as "took the Core back".
	if ((Current.Flags & TraceAbilityFlags::EffectActive) != 0)
	{
		// §2 [ASSUMPTION]: picking the Core back up drops the cloak early. The reading is that the
		// cloak is a getaway, and a player who has the Core again is not getting away from anything.
		if (Settings.bElleCloakEndsOnCorePickup && IsCloaked() && ATraceCore::IsCoreHolder(GetCharacter()))
		{
			EndCloak(TEXT("took the Core back"));
		}
		else if (Now >= Current.EffectEndMatchTime)
		{
			EndCloak(TEXT("expired"));
		}
	}

	// ---- SERVER: SNAP's second-gate window lapsing ------------------------------------------------
	//
	// §2: "If no second gate inside 4 s the first expires." The GATE expires itself, on its own
	// deadline — what happens here is the COOLDOWN, which is charged in full from the first press so
	// that a fluffed cast is not a free gate every four seconds.
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

	// FX §5.1: ElleCloak is a WORLD event at the authority's own StartCloak. One play, multicast, at
	// her body — the cosmetic half is driven from the replicated flag on every machine, so a second
	// local play anywhere would be the §8.7 double-audio failure.
	if (const ATraceCharacter* MyPawn = GetCharacter())
	{
		TraceAudio::Play(MyPawn, TraceSoundEvents::ElleCloak);
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Elle] CLOAK up for %.1fs (%s): opacity %.2f, ends at match time %.2f."),
		Duration, Why, UTraceSettings::Get().ElleCloakOpacity, EndAt);
}

void UTraceAbilitySetElle::EndCloak(const TCHAR* Why)
{
	// THE FLAG, not IsCloaked(), for the reason written out at the expiry test in TickAbilities: an
	// EXPIRED cloak is one whose flag is still up and whose deadline has passed, and IsCloaked() is
	// false for exactly that state — so this guard used to refuse the one call that most needed to
	// get through. Idempotent either way: with the bit already clear this is still a no-op.
	if (!HasAuthority() || (State().Flags & TraceAbilityFlags::EffectActive) == 0)
	{
		return;
	}

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::EffectActive);
	Writable.EffectEndMatchTime = 0.f;
	MarkStateDirty();

	// AND THE DECLOAK IS AUDIBLE ON PURPOSE (§2.5, §5.1): it is information the people hunting her are
	// owed. It is quieter than the cloak (-10 dBFS against -12 at render) for the same reason the
	// decloak SWEEP is quieter than the cloak sweep — a reveal must not out-shout the player it
	// reveals. Not played for a death: OnPawnDied's EndCloak runs after the pawn is already dead and
	// the death burst is the sound of that moment.
	if (const ATraceCharacter* MyPawn = GetCharacter())
	{
		if (MyPawn->IsAlive())
		{
			TraceAudio::Play(MyPawn, TraceSoundEvents::ElleDecloak);
		}
	}

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

	// ---- FX §2.5: THE SWEEPS, ON THE EDGE AND ONLY ON THE EDGE ----------------------------------
	//
	// This function is called at 20 Hz for the whole cloak (see the re-push comment in TickAbilities),
	// so anything that fires unconditionally here fires sixty times per cloak. The edge is against the
	// last value asked for, and a DEAD pawn is never asked: a sweep on a corpse is a lie about where
	// she went, and OnPawnDied comes through here.
	const bool bAlive = (MyPawn != nullptr) && MyPawn->IsAlive();
	if (bAlive && bCloakOn != bCloakVisualWanted)
	{
		StartCloakSweep(bCloakOn);
	}
	bCloakVisualWanted = bAlive && bCloakOn;

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

	// *** §2.5: "body dim (shipped treatment) applies at SWEEP END." ***
	//
	// The ring is the transition and the dim is the state, and running them together would put the
	// sweep on a pawn that had already gone dark — decoration on an event nobody can see happen. So
	// the dim is held for the 0.3 s the on-sweep runs.
	//
	// IT COSTS HER 0.3 s OF THE 3 s CLOAK, and that is a real, deliberate, spec-directed change rather
	// than an oversight: she is visible for those 0.3 s. bCloakVisualApplied stays FALSE across the
	// window on purpose — that accessor's contract is "the paint is on", and reporting it early would
	// make the one query that can distinguish a missed repaint start lying.
	if (bCloakSweepRunning && bCloakSweepIsOn)
	{
		return;
	}

	TraceAbilitySetElleFile::DimForCloak(MyPawn, UTraceSettings::Get().ElleCloakOpacity);
	bCloakVisualApplied = true;
	CloakVisualPawn = MyPawn;
}

// =================================================================================================
// FX §2.5 — the two cloak sweeps. EVERY MACHINE.
// =================================================================================================

void UTraceAbilitySetElle::StartCloakSweep(bool bCloakOn)
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr)
	{
		return;
	}

	// A dedicated server draws nothing and has no cooked shaders. The FACT still replicates from
	// there; only the paint is skipped, exactly as the dim already does it a few lines below.
	const UWorld* WorldPtr = GetWorld();
	if (WorldPtr != nullptr && WorldPtr->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// A second sweep replaces the first rather than stacking: cloak-on immediately followed by a death
	// or a Core pickup is a real sequence, and two rings travelling in opposite directions at once is
	// not a thing §2.5 describes.
	DetachCloakSweep();

	USceneComponent* AttachTo = MyPawn->GetRootComponent();
	if (AttachTo == nullptr)
	{
		return;
	}

	const float Intensity = bCloakOn
		? TraceAbilitySetElleFile::SweepOnIntensity
		: TraceAbilitySetElleFile::SweepOffIntensity;
	const float StartZ = bCloakOn ? TraceAbilitySetElleFile::SweepTopUU : TraceAbilitySetElleFile::SweepBottomUU;

	CloakSweep = TraceFxLoopBudget::AttachLoopPrimitive(MyPawn, AttachTo, UTraceFxShapes::GetCylinder(),
		TEXT("ElleCloakSweep"), TraceAbilitySetElleFile::CloakSilver, Intensity,
		FVector(0.f, 0.f, StartZ), TraceAbilitySetElleFile::SweepRadiusUU, CloakSweepMID);

	if (CloakSweep == nullptr)
	{
		return;   // refused (budget, or no additive material). Documented degradation: no ring.
	}

	// AttachLoopPrimitive scales every piece uniformly off its radius, which is right for a blob and
	// wrong for a flat ring. §2.5 asks for h 4 uu; the helper owns the budget and the material, the
	// caller owns the shape.
	const float XY = UTraceFxShapes::ShapeScaleForRadiusUU(TraceAbilitySetElleFile::SweepRadiusUU);
	CloakSweep->SetRelativeScale3D(FVector(XY, XY,
		UTraceFxShapes::ShapeScaleForLengthUU(TraceAbilitySetElleFile::SweepHeightUU)));

	CloakSweepPawn = MyPawn;
	CloakSweepElapsed = 0.f;
	bCloakSweepRunning = true;
	bCloakSweepIsOn = bCloakOn;

	// The travel summary starts here and is closed by TickCloakSweep. See GetLastSweepStartZ.
	LastSweepStartZ = static_cast<float>(CloakSweep->GetRelativeLocation().Z);
	LastSweepEndZ = LastSweepStartZ;
	bLastSweepWasCloakOn = bCloakOn;

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Elle] cloak sweep %s on %s: r %.0f uu, additive I %.2f, %.0f -> %.0f uu over %.1fs."),
		bCloakOn ? TEXT("ON (head->feet)") : TEXT("OFF (feet->head)"), *GetNameSafe(MyPawn),
		TraceAbilitySetElleFile::SweepRadiusUU, Intensity, StartZ,
		bCloakOn ? TraceAbilitySetElleFile::SweepBottomUU : TraceAbilitySetElleFile::SweepTopUU,
		TraceAbilitySetElleFile::SweepSeconds);
}

void UTraceAbilitySetElle::TickCloakSweep(float DeltaSeconds)
{
	if (!bCloakSweepRunning)
	{
		return;
	}

	ATraceCharacter* SweepPawn = CloakSweepPawn.Get();
	if (CloakSweep == nullptr || SweepPawn == nullptr || SweepPawn != GetCharacter())
	{
		// The pawn was replaced under us mid-sweep. Rule 1 of the router contract, and it applies to a
		// transient exactly as it applies to a loop.
		DetachCloakSweep();
		return;
	}

	CloakSweepElapsed += DeltaSeconds;
	const float Alpha = FMath::Clamp(CloakSweepElapsed / TraceAbilitySetElleFile::SweepSeconds, 0.f, 1.f);

	const float FromZ = bCloakSweepIsOn ? TraceAbilitySetElleFile::SweepTopUU : TraceAbilitySetElleFile::SweepBottomUU;
	const float ToZ   = bCloakSweepIsOn ? TraceAbilitySetElleFile::SweepBottomUU : TraceAbilitySetElleFile::SweepTopUU;

	CloakSweep->SetRelativeLocation(TraceFxLoopBudget::ClampToFootprint(
		FVector(0.f, 0.f, FMath::Lerp(FromZ, ToZ, Alpha))));

	// Read BACK off the component, not from the Lerp: the clamp above is allowed to move it, and a
	// summary of what the recipe asked for would not notice if it had.
	LastSweepEndZ = static_cast<float>(CloakSweep->GetRelativeLocation().Z);

	// MONOTONIC: the ring travels and its intensity only ever falls. §2.5 asks for I -> 0, and bible
	// §3.3's no-pulse rule is satisfied by construction rather than by promise.
	const float Peak = bCloakSweepIsOn
		? TraceAbilitySetElleFile::SweepOnIntensity
		: TraceAbilitySetElleFile::SweepOffIntensity;

	// AttachLoopPrimitive only ever hands back a piece whose blend resolved to Additive (it refuses
	// the opaque rungs outright), so this is the achieved blend and not a hopeful guess.
	UTraceFxShapes::SetGlow(CloakSweepMID, ETraceFxBlend::Additive,
		TraceAbilitySetElleFile::CloakSilver, TraceFxLoopBudget::ClampIntensity(Peak * (1.f - Alpha)));

	if (Alpha >= 1.f)
	{
		DetachCloakSweep();
	}
}

void UTraceAbilitySetElle::DetachCloakSweep()
{
	if (CloakSweep != nullptr)
	{
		TraceFxLoopBudget::DetachLoopPrimitive(CloakSweepPawn.Get(), CloakSweep);
	}
	CloakSweep = nullptr;
	CloakSweepMID = nullptr;
	CloakSweepPawn = nullptr;
	CloakSweepElapsed = 0.f;
	bCloakSweepRunning = false;
}

float UTraceAbilitySetElle::GetCloakSweepHeightUU() const
{
	return (CloakSweep != nullptr) ? static_cast<float>(CloakSweep->GetRelativeLocation().Z) : 0.f;
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

	// THE GAIN IS SCALED, THE MULTIPLIER IS NOT. The shipped global is 1.375000, i.e. 1 + 0.375000 of
	// gain (v8 §8's 1.3125 base, x v28 §5's 1.50 scale on the gain, x v26 §3a's 0.80 on the gain).
	// PATCH 28 ITEM 3 cuts her share of it from +40% to +30%: 0.375000 x 1.30 = 0.487500, so Elle's
	// is 1.487500 (it was 1.525000). Scaling the whole multiplier would give 1.7875 — an ability that
	// would beat DashSpeed off a fast slide, which is the inversion spec v9 §7 already refused for
	// everybody.
	//
	// NOT ONE OF THOSE NUMBERS IS TYPED BELOW, which is why Patch 28 item 3 was a one-knob edit: the
	// global arrives as a parameter and Elle's share is the live property.
	//
	// Never floored below the global: a passive that says "+30%" must not be able to punish her if
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

					// *** THE CLOCK RESTARTS HERE, AND UNTIL v23 IT DID NOT. ***
					//
					// PhaseStartReal was still the start of phase 2, so the wait below expired
					// SettleSeconds early and cut the walk short — the key was still held down when
					// the second press went in. Measured on this build: 238 uu and 252 uu of
					// separation across two runs, either side of the 260 uu minimum, so the second
					// mouth was refused BY DESIGN and the command reported "*** FAIL *** the two
					// mouths are 238 uu apart" for a product that was working perfectly. A harness
					// that cries wolf costs exactly as much as one that cannot go red. With the
					// restart she covers ~450 uu and the assertion measures the ability again.
					State->PhaseStartReal = NowReal;

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

	// =============================================================================================
	// Trace.Elle.PortalShot — DEMO 20 ITEM 4's EVIDENCE, POSED FOR THE CAMERA
	//
	// "Elle's portal is invisible" is a report about a SCREEN, so the only thing that can close it is
	// a picture taken from where the player sits. Trace.Elle.SnapPressTest already presses the real
	// key twice and pairs a portal, but it deliberately leaves Elle standing INSIDE the second mouth
	// (that is the assertion it exists to make), and from in there the first mouth is exactly 90° off
	// her nose and out of frame. A photograph of one mouth cannot answer "BOTH mouths visible".
	//
	// So this does the same two presses and then does the one thing the press test must not: it backs
	// her straight out of the mouth she is standing in, which puts the mouth she left ahead of her and
	// the first mouth ahead and to one side. Then it prints, for each of her own gates, the beads it
	// DRAWS, its colour, and its angle off the camera's forward vector against the camera's own FOV —
	// so the claim "both mouths are in this frame" is checkable against the log rather than being an
	// opinion about a JPEG. It asserts nothing and passes nothing; it poses and it reports.
	//
	// Everything it does happens inside the shipped 8 s pair lifetime, on the shipped knobs. Red arm:
	// Trace.Elle.GateVisible 0 — same run, same log, beads drawn falls to 0 and the photograph of the
	// same two mouths is of an empty floor.
	// =============================================================================================

	struct FPortalShotState
	{
		int32 Phase = 0;
		double PhaseStartReal = 0.0;
		double AcquireDeadline = 0.0;

		/**
		 * How long to hold the strafe key that separates the two mouths.
		 *
		 * 0.6 s at the shipped 800 uu/s walk clears the 260 uu minimum separation with room to
		 * spare. Too short and Snap REFUSES the second mouth for being on top of the first — a
		 * correct refusal that photographs identically to a broken ability.
		 */
		float WalkSeconds = 0.6f;
		/**
		 * How long to hold the back-up key that gets her OUT of the second mouth and looking at both.
		 *
		 * The camera has to end up further back than the mouths are apart, or the first mouth sits
		 * outside a 90 deg FOV and the frame answers half the question. 1.3 s is roughly 800 uu.
		 */
		float BackUpSeconds = 1.3f;

		TWeakObjectPtr<UTraceAbilityComponent> Elle;
		TWeakObjectPtr<APlayerState> ElleState;
	};

	/** Every gate this player owns, described from the camera. The log the screenshot is judged with. */
	/**
	 * TURN AND LOOK AT THE PORTAL, which is what a player does and what the fixture kept forgetting.
	 *
	 * Where the practice range drops Elle, and which way she is facing when it does, varies run to
	 * run: three consecutive runs of this command put the two mouths at 8 deg, 41 deg and 45 deg off
	 * the camera's nose, and the last of those is a photograph of a floor pad with the portal off the
	 * edge of the frame. The gates were identical in all three. Evidence that depends on where the
	 * spawn happened to point is not evidence, so the camera is aimed at the midpoint of the two
	 * mouths before the frame is taken — the same mouse movement a player would make, on the same
	 * camera, from the same chair. Nothing about the gates is touched.
	 */
	void AimAtOwnPortal(UWorld* WorldPtr, const APlayerState* Placer)
	{
		APlayerController* PC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		const APlayerCameraManager* Cam = (PC != nullptr) ? PC->PlayerCameraManager : nullptr;
		if (Cam == nullptr)
		{
			return;
		}

		FVector Sum = FVector::ZeroVector;
		int32 Count = 0;
		for (TActorIterator<ATraceElleGate> It(WorldPtr); It; ++It)
		{
			const ATraceElleGate* Gate = *It;
			if (Gate != nullptr && IsValid(Gate)
				&& (Placer == nullptr || Gate->GetSourcePlayerState() == Placer))
			{
				Sum += Gate->GetMouthLocation();
				++Count;
			}
		}
		if (Count == 0)
		{
			return;
		}

		const FVector Midpoint = Sum / static_cast<double>(Count);
		const FRotator Look = (Midpoint - Cam->GetCameraLocation()).Rotation();

		// Yaw only, and roll zeroed. Pitching to the mouths would tip the horizon and make the frame
		// look staged; the mouths are on the floor and a level look already holds them.
		PC->SetControlRotation(FRotator(0.f, Look.Yaw, 0.f));
	}

	void LogPortalShotFraming(UWorld* WorldPtr, const APlayerState* Placer)
	{
		const APlayerController* PC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		const APlayerCameraManager* Cam = (PC != nullptr) ? PC->PlayerCameraManager : nullptr;
		if (Cam == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ELLESHOT] no camera on this machine — nothing to frame."));
			return;
		}

		const FVector CamLocation = Cam->GetCameraLocation();
		const FVector CamForward = Cam->GetCameraRotation().Vector();
		const float HalfFOV = Cam->GetFOVAngle() * 0.5f;

		int32 Mine = 0;
		int32 Paired = 0;
		int32 DrawnBeads = 0;
		int32 InFrame = 0;

		for (TActorIterator<ATraceElleGate> It(WorldPtr); It; ++It)
		{
			const ATraceElleGate* Gate = *It;
			if (Gate == nullptr || !IsValid(Gate)
				|| (Placer != nullptr && Gate->GetSourcePlayerState() != Placer))
			{
				continue;
			}

			const FVector Mouth = Gate->GetMouthLocation();
			const FVector ToMouth = Mouth - CamLocation;
			const float Distance = static_cast<float>(ToMouth.Size());
			// A literal rather than KINDA_SMALL_NUMBER: the legacy math macros were re-spelled
			// UE_KINDA_SMALL_NUMBER during the 5.x line and the old names are a deprecation shim,
			// which is a C4996 waiting to happen on Windows. A uu is a centimetre; 1 is plenty.
			const float OffAxisDeg = (Distance > 1.f)
				? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
					static_cast<float>(FVector::DotProduct(ToMouth / Distance, CamForward)), -1.f, 1.f)))
				: 0.f;

			// Against the HORIZONTAL half-FOV, which is the generous axis and the one the two mouths
			// are separated along. A mouth is a 130 uu radius ring, so its EDGE is in frame a good way
			// past the point its centre leaves — this stays the conservative test and says so.
			const bool bInFrame = (OffAxisDeg <= HalfFOV);

			++Mine;
			Paired += Gate->IsPaired() ? 1 : 0;
			DrawnBeads += Gate->GetDrawnBeadCount();
			InFrame += bInFrame ? 1 : 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("[ELLESHOT]   mouth at (%s): paired=%d beads DRAWN=%d | %.0f uu from the camera, "
				     "%.1f deg off centre (half-FOV %.1f) -> %s"),
				*Mouth.ToCompactString(), Gate->IsPaired() ? 1 : 0, Gate->GetDrawnBeadCount(),
				Distance, OffAxisDeg, HalfFOV, bInFrame ? TEXT("IN FRAME") : TEXT("out of frame"));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLESHOT] READY: %d of her own mouths standing, %d paired, %d in frame, %d beads drawn in "
			     "total. Camera at (%s) looking (%s). A screenshot taken now is the answer to 'Elle's portal "
			     "is invisible'; with Trace.Elle.GateVisible 0 the same run draws 0 beads."),
			Mine, Paired, InFrame, DrawnBeads,
			*CamLocation.ToCompactString(), *CamForward.ToCompactString());
	}

	void RunEllePortalShot(const TArray<FString>& Args)
	{
		UWorld* WorldPtr = FindLocalGameWorldForPressTest();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ELLESHOT] no local game world with a player in it — this poses a CAMERA, so it needs a "
				     "machine somebody is sitting at."));
			return;
		}
		if (WorldPtr->IsPaused())
		{
			if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
			{
				FirstPC->SetPause(false);
			}
		}

		TSharedPtr<FPortalShotState> State = MakeShared<FPortalShotState>();
		State->WalkSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 0.1f, 3.f) : State->WalkSeconds;
		State->BackUpSeconds = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.f, 3.f) : State->BackUpSeconds;
		State->PhaseStartReal = FPlatformTime::Seconds();
		State->AcquireDeadline = State->PhaseStartReal + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLESHOT] ===== DEMO 20 item 4. Two presses of the real E key, then back out of the second "
			     "mouth so BOTH are in one frame. walk=%.2fs backUp=%.2fs. ====="),
			State->WalkSeconds, State->BackUpSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: stage, exactly as the press test stages ------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Local = FindLocalAbilityComponent(TickWorld);
				if (Local != nullptr && Local->GetCharacterId() != ETraceCharacterId::Elle)
				{
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
				const ATraceCharacter* EllePawn = (Local != nullptr) ? Local->GetOwningCharacter() : nullptr;
				const ATracePlayerController* LocalTracePC =
					Cast<ATracePlayerController>(TickWorld->GetFirstPlayerController());
				const bool bInputLive = (LocalTracePC != nullptr) && !LocalTracePC->IsGameInputSuppressed()
					&& !TickWorld->IsPaused();

				if (ElleSet == nullptr || EllePawn == nullptr || !EllePawn->IsAlive() || !bInputLive)
				{
					if (NowReal > State->AcquireDeadline)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[ELLESHOT] ABORTED: could not stage Elle (set=%d livingPawn=%d inputLive=%d)."),
							(ElleSet != nullptr) ? 1 : 0, (EllePawn != nullptr && EllePawn->IsAlive()) ? 1 : 0,
							bInputLive ? 1 : 0);
						return false;
					}
					return true;
				}

				State->Elle = Local;
				State->ElleState = Local->GetOwningPlayerState();
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 1: press 1, then start the strafe that separates the mouths ---------------
			if (State->Phase == 1)
			{
				PressAbilityKey(TickWorld);
				if (APlayerController* LocalPC = TickWorld->GetFirstPlayerController())
				{
					LocalPC->ConsoleCommand(FString::Printf(TEXT("Trace.SimInput D %.2f"), State->WalkSeconds),
						/*bWriteToLog=*/false);
				}
				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 2: press 2 once she has stopped, which pairs them -------------------------
			//
			// The wait is the walk plus a beat of deceleration: the second mouth is placed where she
			// COMES TO REST, and a mouth placed mid-slide is one she has already coasted out of.
			if (State->Phase == 2)
			{
				if ((NowReal - State->PhaseStartReal) < static_cast<double>(State->WalkSeconds) + 0.55)
				{
					return true;
				}
				PressAbilityKey(TickWorld);
				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: BACK OUT OF THE MOUTH SHE IS STANDING IN --------------------------------
			//
			// Backwards, not forwards or sideways, and that is the whole trick: it leaves the mouth she
			// just placed dead ahead and the first mouth ahead and to the side she walked from, so one
			// frame holds both. It is also a free demonstration of the Demo 17 step-in edge — she walks
			// OUT of a live gate and is not teleported, because leaving is not entering.
			if (State->Phase == 3)
			{
				if ((NowReal - State->PhaseStartReal) < 0.35)
				{
					return true;   // let the pair form and replicate before moving her
				}
				if (APlayerController* LocalPC = TickWorld->GetFirstPlayerController())
				{
					LocalPC->ConsoleCommand(FString::Printf(TEXT("Trace.SimInput S %.2f"), State->BackUpSeconds),
						/*bWriteToLog=*/false);
				}
				State->Phase = 4;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 4: hold still and describe the shot ----------------------------------------
			if ((NowReal - State->PhaseStartReal) < static_cast<double>(State->BackUpSeconds) + 0.4)
			{
				return true;
			}

			AimAtOwnPortal(TickWorld, State->ElleState.Get());
			LogPortalShotFraming(TickWorld, State->ElleState.Get());
			return false;
		}));
	}

	FAutoConsoleCommand CmdEllePortalShot(
		TEXT("Trace.Elle.PortalShot"),
		TEXT("DEMO 20 item 4. Trace.Elle.PortalShot [WalkSeconds] [BackUpSeconds] — presses the real E key twice "
		     "to open a portal, then backs out of the second mouth so BOTH mouths are in one frame, and logs each "
		     "mouth's drawn-bead count and angle off camera so a screenshot can be checked against the log. Pair "
		     "up ~1.5 s after it starts; shoot within the 8 s pair lifetime. Red arm: Trace.Elle.GateVisible 0."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunEllePortalShot));
}

// =================================================================================================
// Trace.Elle.FxTest — FX §2.5's FIVE ELEMENTS, STAGED, MEASURED AND PHOTOGRAPHED
//
// ===================================================================================================
// THE TWO THINGS A SCREENSHOT CANNOT PROVE, AND HOW THEY ARE PROVED INSTEAD
// ===================================================================================================
//
// §2.5 asks for two effects whose whole content is MOTION — a ring that travels head-to-feet in 0.3 s
// and a bead ring that turns at 12 deg/s. A photograph of either is a photograph of a ring, and a
// fixture that spawned one and reported "not null" would have measured its own call.
//
// So both are measured with TWO SAMPLES and a comparison:
//   the cloak sweep  -> GetCloakSweepHeightUU() twice, and the second must be LOWER going in and
//                       HIGHER coming out. That is the direction claim, which is the claim §2.5
//                       actually makes ("head->feet" / "feet->head"), rather than "a ring existed".
//   the portal ring  -> GetRingSpinDegrees() twice a second apart, against 12 deg/s.
//
// The rest — the cast ring, the teleport flash at BOTH mouths, the expiry dim-out — are read off live
// objects (an instance count, a world iterator, the fade alpha) and photographed while held.
//
// FIXED FILENAMES (W4KitsB_Elle_*.png), so this run takes the release capture lock.
// =================================================================================================

namespace TraceAbilitySetElleFile
{
	struct FElleFxState
	{
		int32 Stage = 0;
		double StageStartReal = 0.0;
		int32 Passed = 0;
		int32 Failed = 0;
		int32 Shots = 0;

		TWeakObjectPtr<UTraceAbilitySetElle> Elle;
		TWeakObjectPtr<ACameraActor> Observer;

		float CloakSweepZ1 = 0.f;
		float CloakSweepZ2 = 0.f;
		int32 CloakSweepSamples = 0;
		float CloakTravelStart = 0.f;
		float CloakTravelEnd = 0.f;
		bool  bCloakSweepSeen = false;
		bool  bDimLandedAfterSweep = false;

		float DecloakSweepZ1 = 0.f;
		float DecloakSweepZ2 = 0.f;
		int32 DecloakSweepSamples = 0;
		float DecloakTravelStart = 0.f;
		float DecloakTravelEnd = 0.f;
		bool  bDecloakSweepSeen = false;

		FVector MouthA = FVector::ZeroVector;
		FVector MouthB = FVector::ZeroVector;
		bool  bPairPlaced = false;
		int32 PairBeads = 0;
		int32 CastRingsPlaying = 0;
		float Spin1 = 0.f;
		float Spin2 = 0.f;
		double SpinSampledAt1 = 0.0;
		double SpinSampledAt2 = 0.0;
		int32 TeleportBursts = 0;
		float MinFadeSeen = 1.f;
		bool  bFadeShot = false;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; UE_LOG(LogTraceGame, Display, TEXT("[ELLEFX]   ok   %s"), *What); }
			else            { ++Failed; UE_LOG(LogTraceGame, Error,   TEXT("[ELLEFX]   FAIL %s"), *What); }
		}

		void Advance(int32 Next) { Stage = Next; StageStartReal = FPlatformTime::Seconds(); }
		double In() const { return FPlatformTime::Seconds() - StageStartReal; }

		/** Is this one of the two mouths THIS parade placed? See the filter's comment in stage 7. */
		bool IsMyMouth(const FVector& Where) const
		{
			return FVector::Dist(Where, MouthA) < 400.f || FVector::Dist(Where, MouthB) < 400.f;
		}
	};

	UTraceAbilitySetElle* MakeLocalPlayerIntoElle(UWorld* WorldPtr)
	{
		UTraceAbilityComponent* Comp = FindLocalAbilityComponent(WorldPtr);
		if (Comp == nullptr)
		{
			return nullptr;
		}
		if (Comp->GetCharacterId() != ETraceCharacterId::Elle)
		{
			Comp->ServerSetCharacter(ETraceCharacterId::Elle);
		}
		return Comp->GetAbilitySetAs<UTraceAbilitySetElle>();
	}

	/**
	 * A camera to watch her from: a purely local view change, so no pawn is moved.
	 *
	 * ACameraActor and NOT a bare AActor — AActor has no root component, so SpawnActor's transform
	 * has nothing to write to and the "observer" sits at the world origin reporting (0,0,0). It does
	 * not fail loudly; it produces convincing frames of somewhere nobody was.
	 */
	ACameraActor* PlaceElleObserver(UWorld* WorldPtr, const FVector& At, const FVector& LookAt)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		ACameraActor* Observer = WorldPtr->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), At,
			(LookAt - At).Rotation(), Params);
		if (Observer != nullptr)
		{
			if (APlayerController* PC = WorldPtr->GetFirstPlayerController())
			{
				PC->SetViewTargetWithBlend(Observer, 0.f);
			}
		}
		return Observer;
	}

	void AimElleObserver(AActor* Observer, const FVector& At, const FVector& LookAt)
	{
		if (Observer != nullptr)
		{
			Observer->SetActorLocation(At);
			Observer->SetActorRotation((LookAt - At).Rotation());
		}
	}

	void ShootElleFrame(FElleFxState& State, const TCHAR* Tag)
	{
		++State.Shots;
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots")
			/ FString::Printf(TEXT("W4KitsB_Elle_%02d_%s.png"), State.Shots, Tag));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[ELLEFX] Screenshot requested: %s"), *Path);
	}

	/** Only the ones standing at THIS parade's two mouths — a bot Elle's pair would otherwise count. */
	int32 CountElleTeleportBursts(UWorld* WorldPtr, const FVector& MouthA, const FVector& MouthB)
	{
		int32 Count = 0;
		for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
		{
			const ATraceFxBurst* Burst = *It;
			if (!IsValid(Burst) || Burst->GetBurstType() != ETraceFxBurstType::ElleTeleport)
			{
				continue;
			}
			const FVector Where = Burst->GetActorLocation();
			if (FVector::Dist(Where, MouthA) < 400.f || FVector::Dist(Where, MouthB) < 400.f)
			{
				++Count;
			}
		}
		return Count;
	}

	void RunElleFxTest()
	{
		UWorld* WorldPtr = FindLocalGameWorldForPressTest();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[ELLEFX] no local game world."));
			return;
		}

		UTraceAbilitySetElle* Elle = MakeLocalPlayerIntoElle(WorldPtr);
		if (Elle == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[ELLEFX] the local player has no ability component."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLEFX] ===== FX §2.5 parade: cloak sweep, decloak sweep, snap cast ring, paired-ring "
			     "rotation, teleport flash at both mouths, expiry dim-out ====="));

		TSharedRef<FElleFxState> State = MakeShared<FElleFxState>();
		State->Elle = Elle;
		State->StageStartReal = FPlatformTime::Seconds();
		TWeakObjectPtr<UWorld> WeakWorld(WorldPtr);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			UTraceAbilitySetElle* Kit = State->Elle.Get();
			if (TickWorld == nullptr || Kit == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[ELLEFX] the world or the kit went away mid-run."));
				return false;
			}
			ATraceCharacter* Pawn = Kit->GetCharacter();
			if (Pawn == nullptr)
			{
				return true;
			}

			switch (State->Stage)
			{
			case 0:
				if (State->In() > 1.0)
				{
					// Behind and slightly above: the sweep travels the LENGTH of her, so the camera has
					// to see the whole silhouette or the direction claim is unphotographable.
					State->Observer = PlaceElleObserver(TickWorld,
						Pawn->GetActorLocation() - Pawn->GetActorForwardVector() * 240.f + FVector(0.f, 0.f, 40.f),
						Pawn->GetActorLocation());
					Kit->DebugStartCloak(2.5f);
					State->Advance(1);
				}
				break;

			case 1:
				// SAMPLED EVERY TICK WHILE IT IS ALIVE, never at two fixed delays. The sweep is 0.3 s
				// long and this ticker is at the mercy of the frame rate: the first version of this
				// fixture asked for a second reading 0.14 s later, got it 0.31 s later — after the ring
				// had been detached — and read the accessor's "no ring" ZERO as travel. It printed
				// "z 73 -> 0 uu: it travelled head to feet" about a component that no longer existed.
				if (Kit->IsCloakSweepPlaying())
				{
					const float Z = Kit->GetCloakSweepHeightUU();
					if (!State->bCloakSweepSeen)
					{
						State->bCloakSweepSeen = true;
						State->CloakSweepZ1 = Z;
						ShootElleFrame(*State, TEXT("CloakSweep"));
					}
					State->CloakSweepZ2 = Z;
					++State->CloakSweepSamples;
					return true;
				}

				if (State->bCloakSweepSeen || State->In() > 1.5)
				{
					State->CloakTravelStart = Kit->GetLastSweepStartZ();
					State->CloakTravelEnd = Kit->GetLastSweepEndZ();
					UE_LOG(LogTraceGame, Display,
						TEXT("[ELLEFX] cloak sweep: travel summary z %.0f -> %.0f uu (%d live sample(s) caught, "
						     "z %.0f -> %.0f), dim applied=%d."),
						State->CloakTravelStart, State->CloakTravelEnd, State->CloakSweepSamples,
						State->CloakSweepZ1, State->CloakSweepZ2, Kit->IsCloakVisualApplied() ? 1 : 0);
					State->Advance(3);
				}
				break;

			case 2:
				State->Advance(3);   // folded into stage 1; kept so the stage numbers below do not move
				break;

			case 3:
				if (State->In() > 0.45)
				{
					// §2.5: the dim lands at SWEEP END, so this is the assertion that the delay is a
					// delay and not a removal.
					State->bDimLandedAfterSweep = Kit->IsCloakVisualApplied() && !Kit->IsCloakSweepPlaying();
					ShootElleFrame(*State, TEXT("CloakDim"));

					// A 0.05 s cloak, so it EXPIRES on its own — the shipped route out, through
					// EndCloak, which is where FX §5.1 hangs ElleDecloak. Calling a debug "stop" would
					// have proved the sweep while skipping the sound and the state clear with it.
					Kit->DebugStartCloak(0.05f);
					State->Advance(4);
				}
				break;

			case 4:
				if (Kit->IsCloakSweepPlaying())
				{
					const float Z = Kit->GetCloakSweepHeightUU();
					if (!State->bDecloakSweepSeen)
					{
						State->bDecloakSweepSeen = true;
						State->DecloakSweepZ1 = Z;
						ShootElleFrame(*State, TEXT("DecloakSweep"));
					}
					State->DecloakSweepZ2 = Z;
					++State->DecloakSweepSamples;
					return true;
				}

				if (State->bDecloakSweepSeen || State->In() > 1.5)
				{
					State->DecloakTravelStart = Kit->GetLastSweepStartZ();
					State->DecloakTravelEnd = Kit->GetLastSweepEndZ();
					UE_LOG(LogTraceGame, Display,
						TEXT("[ELLEFX] decloak sweep: travel summary z %.0f -> %.0f uu (%d live sample(s) caught, "
						     "z %.0f -> %.0f)."),
						State->DecloakTravelStart, State->DecloakTravelEnd, State->DecloakSweepSamples,
						State->DecloakSweepZ1, State->DecloakSweepZ2);
					State->Advance(6);
				}
				break;

			case 5:
				State->Advance(6);   // folded into stage 4
				break;

			case 6:
			{
				if (State->In() < 0.40)
				{
					break;
				}

				// BOTH MOUTHS ARE PLACED AWAY FROM HER, deliberately: PairWith seeds the re-entry
				// lockout for anybody standing in a mouth as it opens, so a pair placed under her feet
				// cannot be stepped into for a second and the teleport stage below would measure the
				// lockout instead of the flash.
				const FVector Side = FVector::CrossProduct(Pawn->GetActorForwardVector(), FVector::UpVector)
					.GetSafeNormal();
				State->MouthA = Pawn->GetActorLocation() + Side * 340.f;
				State->MouthB = State->MouthA + Pawn->GetActorForwardVector() * 420.f;
				State->bPairPlaced = Kit->DebugPlaceGatePair(State->MouthA, State->MouthB);

				const FVector Mid = 0.5f * (State->MouthA + State->MouthB);
				AimElleObserver(State->Observer.Get(), Mid + Side * 700.f + FVector(0.f, 0.f, 260.f), Mid);
				State->Advance(7);
				break;
			}

			case 7:
				if (State->In() > 0.10)
				{
					State->PairBeads = 0;
					State->CastRingsPlaying = 0;
					State->Spin1 = 0.f;
					for (TActorIterator<ATraceElleGate> It(TickWorld); It; ++It)
					{
						const ATraceElleGate* Gate = *It;
						if (!IsValid(Gate) || !Gate->IsPaired() || !State->IsMyMouth(Gate->GetMouthLocation()))
						{
							// BOTS RUN ELLE TOO. A census of every gate in the arena counts another
							// Elle's live pair as this fixture's evidence — measured, one run reported
							// 240 beads and 4 teleport bursts for a two-mouth pair. The filter is the
							// two points this parade itself asked for.
							continue;
						}
						State->PairBeads += Gate->GetDrawnBeadCount();
						State->CastRingsPlaying += Gate->IsCastRingPlaying() ? 1 : 0;
						State->Spin1 = Gate->GetRingSpinDegrees();
						UE_LOG(LogTraceGame, Display,
							TEXT("[ELLEFX] gate at (%s): beads DRAWN=%d, cast ring playing=%d, spin %.2f deg, "
							     "fade %.2f."),
							*Gate->GetMouthLocation().ToCompactString(), Gate->GetDrawnBeadCount(),
							Gate->IsCastRingPlaying() ? 1 : 0, Gate->GetRingSpinDegrees(),
							Gate->GetExpiryFadeAlpha());
					}
					State->SpinSampledAt1 = FPlatformTime::Seconds();
					ShootElleFrame(*State, TEXT("GatePairCastRing"));
					State->Advance(8);
				}
				break;

			case 8:
				if (State->In() > 1.20)
				{
					for (TActorIterator<ATraceElleGate> It(TickWorld); It; ++It)
					{
						const ATraceElleGate* Gate = *It;
						if (IsValid(Gate) && Gate->IsPaired() && State->IsMyMouth(Gate->GetMouthLocation()))
						{
							State->Spin2 = Gate->GetRingSpinDegrees();
						}
					}
					State->SpinSampledAt2 = FPlatformTime::Seconds();
					ShootElleFrame(*State, TEXT("GatePairSpun"));

					// STEP IN. She is 340 uu from mouth A, i.e. outside it on the previous server look,
					// so this is a genuine step-in EDGE and not a proximity poll.
					Pawn->TeleportTo(State->MouthA, Pawn->GetActorRotation(), false, true);
					State->Advance(9);
				}
				break;

			case 9:
				if (State->In() > 0.35)
				{
					State->TeleportBursts = CountElleTeleportBursts(TickWorld, State->MouthA, State->MouthB);
					UE_LOG(LogTraceGame, Display,
						TEXT("[ELLEFX] after the step-in: %d live ElleTeleport burst(s); she is at (%s)."),
						State->TeleportBursts, *Pawn->GetActorLocation().ToCompactString());
					ShootElleFrame(*State, TEXT("TeleportFlash"));
					State->Advance(10);
				}
				break;

			case 10:
			{
				// The dim-out: poll until a gate reports itself fading, or give up after the pair's own
				// lifetime plus a margin. Reported either way — an un-measured claim is a failed one.
				bool bAnyGate = false;
				for (TActorIterator<ATraceElleGate> It(TickWorld); It; ++It)
				{
					const ATraceElleGate* Gate = *It;
					if (IsValid(Gate) && State->IsMyMouth(Gate->GetMouthLocation()))
					{
						bAnyGate = true;
						State->MinFadeSeen = FMath::Min(State->MinFadeSeen, Gate->GetExpiryFadeAlpha());
					}
				}
				// KEEP POLLING TO THE END. Stopping at the first sub-1 reading reports whatever the
				// first sample happened to catch (0.91 on the run that motivated this note) and calls
				// it a dim-out; the honest number is the LOWEST the gates ever got to before they were
				// destroyed, which is what says whether they closed or blinked.
				if (State->MinFadeSeen < 1.f && !State->bFadeShot)
				{
					State->bFadeShot = true;
					// RE-AIMED FIRST. The step-in teleported her, and possessing/repossessing a pawn
					// hands the view target back to it — so without this the "expiry fade" frame is a
					// first-person photograph of wherever she landed, which was the solo4 run's one
					// useless frame.
					const FVector Mid = 0.5f * (State->MouthA + State->MouthB);
					const FVector At = Mid + FVector(0.f, 700.f, 260.f);
					AimElleObserver(State->Observer.Get(), At, Mid);
					if (APlayerController* PC = TickWorld->GetFirstPlayerController())
					{
						PC->SetViewTargetWithBlend(State->Observer.Get(), 0.f);
					}
					ShootElleFrame(*State, TEXT("ExpiryFade"));
				}
				if (bAnyGate && State->In() < 14.0)
				{
					return true;
				}
				State->Advance(11);
				break;
			}

			case 11:
			{
				if (APlayerController* PC = TickWorld->GetFirstPlayerController())
				{
					PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
				}
				if (AActor* Obs = State->Observer.Get())
				{
					Obs->Destroy();
				}

				const double SpinWindow = FMath::Max(0.01, State->SpinSampledAt2 - State->SpinSampledAt1);
				const float SpinRate = static_cast<float>((State->Spin2 - State->Spin1) / SpinWindow);

				UE_LOG(LogTraceGame, Display, TEXT("[ELLEFX] ===== verdict ====="));
				State->Check(State->bCloakSweepSeen, TEXT("a cloak-on sweep ring was attached to the pawn"));
				State->Check(State->CloakTravelEnd < State->CloakTravelStart - 20.f,
					FString::Printf(TEXT("*** it travelled HEAD -> FEET: the ring's own travel summary says "
					                     "z %.0f uu -> %.0f uu (the %d live sample(s) this fixture happened to "
					                     "catch said %.0f -> %.0f) ***"),
						State->CloakTravelStart, State->CloakTravelEnd, State->CloakSweepSamples,
						State->CloakSweepZ1, State->CloakSweepZ2));
				State->Check(State->bDimLandedAfterSweep,
					TEXT("the body dim landed AT SWEEP END (§2.5), not before it"));
				State->Check(State->bDecloakSweepSeen, TEXT("a decloak sweep ring was attached to the pawn"));
				State->Check(State->DecloakTravelEnd > State->DecloakTravelStart + 20.f,
					FString::Printf(TEXT("*** it travelled FEET -> HEAD: the ring's own travel summary says "
					                     "z %.0f uu -> %.0f uu (the %d live sample(s) said %.0f -> %.0f) ***"),
						State->DecloakTravelStart, State->DecloakTravelEnd, State->DecloakSweepSamples,
						State->DecloakSweepZ1, State->DecloakSweepZ2));
				// 120 = two mouths x three rings x twenty beads, and it only reaches 120 if BOTH ring
				// components report — which is the assertion that the waist ring did not fall out of
				// the count when §2.5 moved it into its own component.
				State->Check(State->bPairPlaced && State->PairBeads == 120,
					FString::Printf(TEXT("the pair is standing and DRAWN across both ring components "
					                     "(%d beads of the 120 two full mouths carry)"), State->PairBeads));
				State->Check(State->CastRingsPlaying >= 1,
					FString::Printf(TEXT("the snap cast ring was still expanding when the pair was photographed "
					                     "(%d gate(s))"), State->CastRingsPlaying));
				// §2.5's number, restated here rather than reached for: ATraceElleGate's constants are
				// file-local by design, and a harness that shared them could not tell a wrong constant
				// from a wrong rate. The tolerance is wide because the sample window is real time on a
				// headless machine, not match time.
				constexpr float ExpectedSpinDegPerSecond = 12.f;
				State->Check(FMath::Abs(SpinRate - ExpectedSpinDegPerSecond) < 6.f,
					FString::Printf(TEXT("*** the paired gate's middle ring TURNED: %.2f -> %.2f deg over %.2fs "
					                     "= %.1f deg/s against the %.0f deg/s §2.5 asks for ***"),
						State->Spin1, State->Spin2, SpinWindow, SpinRate, ExpectedSpinDegPerSecond));
				State->Check(State->TeleportBursts == 2,
					FString::Printf(TEXT("the teleport lit BOTH mouths: %d live ElleTeleport burst(s)"),
						State->TeleportBursts));
				State->Check(State->MinFadeSeen < 0.5f,
					FString::Printf(TEXT("the gates DIMMED OUT rather than vanishing — they got at least halfway "
					                     "down before they were destroyed (lowest fade alpha seen %.2f)"),
						State->MinFadeSeen));

				UE_LOG(LogTraceGame, Display, TEXT("[ELLEFX] ===== %d passed, %d failed, %d frame(s) ====="),
					State->Passed, State->Failed, State->Shots);
				if (State->Failed == 0)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[ELLEFX] VERDICT: PASS"));
				}
				else
				{
					UE_LOG(LogTraceGame, Error, TEXT("[ELLEFX] VERDICT: *** FAIL *** (%d)"), State->Failed);
				}
				return false;
			}

			default:
				return false;
			}

			return true;
		}), 0.05f);
	}

	FAutoConsoleCommand CmdElleFxTest(
		TEXT("Trace.Elle.FxTest"),
		TEXT("FX §2.5, staged and photographed: cloaks and decloaks her and measures the sweep's DIRECTION "
		     "from two samples of the live ring, places a real pair and measures the middle ring's rotation "
		     "rate, steps into a mouth and counts the ElleTeleport bursts at both, and watches the expiry "
		     "dim-out. Drives the local pawn and writes FIXED filenames — take the capture lock."),
		FConsoleCommandDelegate::CreateStatic(&RunElleFxTest));
}

// =================================================================================================
// Trace.Elle.GateWatch — THE CLIENT HALF. Decides nothing, drives nothing, MEASURES.
//
// Trace.Elle.FxTest needs authority: it places the pair and moves the pawn. That makes it exactly the
// wrong tool for the question this tranche has to answer — "is any of this visible on a machine that
// is not the server?" — and it is the same trap Trace.Elle.SnapPressTest's own header describes
// (a harness that only ever ran where the answer was guaranteed).
//
// So this runs ANYWHERE and asks only what a client can know: are the replicated gates DRAWN here, is
// the paired middle ring turning here, did the ElleTeleport bursts arrive here. It points this
// machine's camera at the pair — a purely local view change — and takes frames while they are alive.
//
// FIXED FILENAMES (W4KitsB_Client_*.png): take the capture lock.
// =================================================================================================

namespace TraceAbilitySetElleFile
{
	void RunElleGateWatch(UWorld* WorldPtr, float Seconds)
	{
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[GATEWATCH] no world."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[GATEWATCH] watching for %.0fs on netmode=%d (0 standalone, 2 listen, 3 client)."),
			Seconds, static_cast<int32>(WorldPtr->GetNetMode()));

		struct FWatchState
		{
			double EndReal = 0.0;
			bool bSeenGate = false;
			bool bViewed = false;
			int32 Shots = 0;
			double NextShotReal = 0.0;
			int32 BestBeads = 0;
			int32 BestPaired = 0;
			int32 CastRingsSeen = 0;
			int32 MostTeleportBursts = 0;
			float MinFade = 1.f;
			float SpinFirst = 0.f;
			double SpinFirstAt = 0.0;
			float SpinLast = 0.f;
			double SpinLastAt = 0.0;
			bool bSpinSampled = false;
			int32 PairedWhenSampled = 0;
		};

		TSharedRef<FWatchState> W = MakeShared<FWatchState>();
		W->EndReal = FPlatformTime::Seconds() + FMath::Clamp(Seconds, 1.f, 120.f);
		TWeakObjectPtr<UWorld> WeakWorld(WorldPtr);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([W, WeakWorld](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}

			int32 Beads = 0;
			int32 Paired = 0;
			int32 CastRings = 0;
			FVector MouthSum = FVector::ZeroVector;
			int32 Mouths = 0;
			float Spin = 0.f;

			for (TActorIterator<ATraceElleGate> It(TickWorld); It; ++It)
			{
				const ATraceElleGate* Gate = *It;
				if (!IsValid(Gate))
				{
					continue;
				}
				Beads += Gate->GetDrawnBeadCount();
				Paired += Gate->IsPaired() ? 1 : 0;
				CastRings += Gate->IsCastRingPlaying() ? 1 : 0;
				MouthSum += Gate->GetMouthLocation();
				++Mouths;
				W->MinFade = FMath::Min(W->MinFade, Gate->GetExpiryFadeAlpha());
				if (Gate->IsPaired())
				{
					Spin = Gate->GetRingSpinDegrees();
				}
			}

			int32 Bursts = 0;
			for (TActorIterator<ATraceFxBurst> It(TickWorld); It; ++It)
			{
				if (IsValid(*It) && It->GetBurstType() == ETraceFxBurstType::ElleTeleport)
				{
					++Bursts;
				}
			}
			W->MostTeleportBursts = FMath::Max(W->MostTeleportBursts, Bursts);
			W->BestBeads = FMath::Max(W->BestBeads, Beads);
			W->BestPaired = FMath::Max(W->BestPaired, Paired);
			W->CastRingsSeen = FMath::Max(W->CastRingsSeen, CastRings);

			if (Paired >= 1)
			{
				// THE BASELINE RESETS WHEN THE PAIR CHANGES. A 40 s window outlives an 8 s pair, so a
				// rate taken across two different pairs is arithmetic about two unrelated actors —
				// measured, one run reported 3.2 deg/s that way while the same build measured 12.0
				// deg/s within a single pair.
				if (!W->bSpinSampled || Paired != W->PairedWhenSampled || Spin < W->SpinLast - 1.f)
				{
					W->bSpinSampled = true;
					W->PairedWhenSampled = Paired;
					W->SpinFirst = Spin;
					W->SpinFirstAt = FPlatformTime::Seconds();
				}
				W->SpinLast = Spin;
				W->SpinLastAt = FPlatformTime::Seconds();
			}

			if (Mouths > 0 && !W->bSeenGate)
			{
				W->bSeenGate = true;
				UE_LOG(LogTraceGame, Display,
					TEXT("[GATEWATCH] *** FIRST FRAME WITH A GATE *** %d mouth(s), %d paired, beads DRAWN=%d, "
					     "cast ring(s) expanding=%d, spin %.2f deg."),
					Mouths, Paired, Beads, CastRings, Spin);
			}

			if (Mouths > 0 && !W->bViewed)
			{
				const FVector Mid = MouthSum / static_cast<float>(Mouths);
				if (APlayerController* PC = TickWorld->GetFirstPlayerController())
				{
					// Local view only: no pawn is moved, so nothing the server owns is touched and no
					// correction can fight it. Same trick Trace.Mace.RopeProbeWatch uses.
					FActorSpawnParameters Params;
					Params.ObjectFlags |= RF_Transient;
					const FVector At = Mid + FVector(0.f, 700.f, 320.f);
					// ACameraActor, not AActor: AActor has no root component, so a spawn transform has
					// nothing to write to and the "observer" reports (0,0,0) for ever. Photographed —
					// join2's client gate frames are the arena seen from its own centre at floor level.
					if (ACameraActor* Observer = TickWorld->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), At,
						(Mid - At).Rotation(), Params))
					{
						PC->SetViewTargetWithBlend(Observer, 0.f);
						W->bViewed = true;
						W->NextShotReal = FPlatformTime::Seconds() + 0.3;
						UE_LOG(LogTraceGame, Display,
							TEXT("[GATEWATCH] camera -> the pair's midpoint (%s), from (%s)."),
							*Mid.ToCompactString(), *At.ToCompactString());
					}
				}
			}

			// SPACED ACROSS THE WHOLE WINDOW, not bunched into the first four seconds: the teleport
			// flash is a 1.2 s actor somewhere inside 40 s, and four frames taken in a row at the start
			// photograph the same instant four times.
			if (W->bViewed && W->Shots < 6 && FPlatformTime::Seconds() >= W->NextShotReal)
			{
				const FString Path = FPaths::ConvertRelativePathToFull(
					FPaths::ProjectSavedDir() / TEXT("Screenshots")
					/ FString::Printf(TEXT("W4KitsB_Client_%02d.png"), W->Shots + 1));
				FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
				++W->Shots;
				W->NextShotReal = FPlatformTime::Seconds() + ((W->Shots < 2) ? 1.1 : 6.0);
				UE_LOG(LogTraceGame, Display,
					TEXT("[GATEWATCH] Screenshot requested: %s  (beads DRAWN=%d, paired=%d, bursts=%d)"),
					*Path, Beads, Paired, Bursts);
			}

			if (FPlatformTime::Seconds() < W->EndReal)
			{
				return true;
			}

			const double Window = FMath::Max(0.01, W->SpinLastAt - W->SpinFirstAt);
			const float Rate = static_cast<float>((W->SpinLast - W->SpinFirst) / Window);
			UE_LOG(LogTraceGame, Display,
				TEXT("[GATEWATCH] VERDICT on this machine: gates seen=%d | most beads DRAWN=%d | most paired=%d "
				     "| cast rings caught expanding=%d | most live ElleTeleport bursts=%d | middle-ring spin "
				     "%.2f -> %.2f deg over %.1fs = %.1f deg/s | lowest expiry fade %.2f | %d frame(s)."),
				W->bSeenGate ? 1 : 0, W->BestBeads, W->BestPaired, W->CastRingsSeen, W->MostTeleportBursts,
				W->SpinFirst, W->SpinLast, Window, Rate, W->MinFade, W->Shots);
			return false;
		}), 0.f);
	}

	FAutoConsoleCommandWithWorld CmdElleGateWatch(
		TEXT("Trace.Elle.GateWatch"),
		TEXT("Dev only, runs ANYWHERE and is meant for a CLIENT. Watches for 40 s and reports what THIS "
		     "machine can see of Elle's gates — drawn beads, the paired middle ring's rotation rate, the cast "
		     "ring, the expiry fade and the live ElleTeleport bursts — then photographs the pair. Writes "
		     "FIXED filenames; take the capture lock."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			RunElleGateWatch(World, 40.f);
		}));
}

#endif   // !UE_BUILD_SHIPPING
