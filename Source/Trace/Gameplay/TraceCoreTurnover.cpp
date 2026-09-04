// Trace — SPEC v25 §2, the Core's TURNOVER STATE MACHINE, its pull, and the loose-Core pickup that
// ends it. See TraceCore.h for the three-row table and TraceCore.cpp for everything else.
//
// WHAT IS IN HERE. The turnover window (register / announce / clear), the pull button and its
// server-side race, the pull travel, the loose pickup and the loose-Core reset. RESTRUCTURE tranche
// D2 moved it out of TraceCore.cpp verbatim: same banners, same essays, same order.
//
// EVERY DECISION IS STILL THE SERVER'S, which is the invariant this file exists to keep legible.
// The four replicated facts (TurnoverLockoutTeam, TurnoverStartServerTime, PullHolds, PullWinner)
// are written HERE AND NOWHERE ELSE — that was true when this code sat in the middle of a sixteen
// thousand line file and it is easier to check now that the writers are one screen apart. No client
// runs a clock of its own. Spec v25: "Do not let a client decide it won a race."
//
// Everything here is a member of ATraceCore; the state still lives on the one actor.

#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceCoreInternal.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"
#include "UObject/ObjectKey.h"
#include "Net/UnrealNetwork.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TraceMatchTypes.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceEndzone.h"
#include "Gameplay/TraceFxShapes.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "World/TraceArenaBuilder.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// The two team questions are shared with TraceCore.cpp, so both files ask them of the one
// implementation and every call site below reads exactly as it did when it lived there.
using namespace TraceCoreLocal;

// =================================================================================================
// SPEC v25 §2 — THE TURNOVER STATE MACHINE
//
// Read the table in TraceCore.h before changing anything here. Three rows, and the third is the
// first: once the lockout expires the turnover is CLEARED, because "nobody pulls, either team picks
// up, normal beam" is exactly what a Core that was never turned over already does.
//
// EVERY DECISION IS THE SERVER'S. The four replicated facts (TurnoverLockoutTeam,
// TurnoverStartServerTime, PullHolds, PullWinner) are written here and nowhere else, and no client
// runs a clock of its own — spec v25: "Do not let a client decide it won a race."
// =================================================================================================

float ATraceCore::GetPullHoldSeconds()        { return TraceModeBTuning::PullHoldSeconds(); }
float ATraceCore::GetTurnoverLockoutSeconds() { return TraceModeBTuning::TurnoverLockoutSeconds(); }
float ATraceCore::GetTurnoverBeamScale()      { return TraceModeBTuning::TurnoverBeamScale(); }

bool ATraceCore::IsTurnoverActive() const
{
	if (TurnoverLockoutTeam == ETraceTeam::None)
	{
		return false;
	}

	// The clock is asked here rather than trusted to have been cleared, so a client whose
	// TurnoverLockoutTeam = None has not yet arrived still stops showing the window on time. The
	// server clears it in ServerTickTurnover; this is what makes the two agree in between.
	return GetTurnoverSecondsRemaining() > 0.f;
}

float ATraceCore::GetTurnoverSecondsRemaining() const
{
	if (TurnoverLockoutTeam == ETraceTeam::None)
	{
		return 0.f;
	}

	const float Lockout = GetTurnoverLockoutSeconds();
	return FMath::Max(0.f, (TurnoverStartServerTime + Lockout) - GetServerTimeSeconds());
}

float ATraceCore::GetTurnoverAlpha() const
{
	if (TurnoverLockoutTeam == ETraceTeam::None)
	{
		return -1.f;
	}

	const float Lockout = GetTurnoverLockoutSeconds();
	if (Lockout <= 0.f)
	{
		return 1.f;
	}

	return FMath::Clamp((GetServerTimeSeconds() - TurnoverStartServerTime) / Lockout, 0.f, 1.f);
}

float ATraceCore::GetPullProgressFor(const AActor* Player) const
{
	if (Player == nullptr)
	{
		return -1.f;
	}

	const float Hold = GetPullHoldSeconds();
	const float Now = GetServerTimeSeconds();

	for (const FTraceCorePullHold& Entry : PullHolds)
	{
		if (Entry.Puller != Player)
		{
			continue;
		}

		// A zero hold time is "instant", not "divide by zero". It is reachable from the console and
		// from a misconfigured ini, and a NaN ring would be the loudest possible way to find out.
		if (Hold <= 0.f)
		{
			return 1.f;
		}

		return FMath::Clamp((Now - Entry.StartServerTime) / Hold, 0.f, 1.f);
	}

	return -1.f;
}

float ATraceCore::GetLocalPullProgress() const
{
	const UWorld* World = GetWorld();
	if (World == nullptr || PullHolds.Num() == 0)
	{
		return -1.f;   // The common case, and it costs nothing.
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* Controller = It->Get();
		if (Controller == nullptr || !Controller->IsLocalController())
		{
			continue;
		}

		const float Progress = GetPullProgressFor(Controller->GetPawn());
		if (Progress >= 0.f)
		{
			return Progress;
		}
	}

	return -1.f;
}

bool ATraceCore::CanPullNow(const ATraceCharacter* Puller, const TCHAR** OutReason) const
{
	// Every refusal names itself, for the same reason IsLegalPassTarget's do: "the ring never
	// appeared" is what a player experiences, and it has eight possible causes.
	const auto Refuse = [OutReason](const TCHAR* Why) -> bool
	{
		if (OutReason != nullptr)
		{
			*OutReason = Why;
		}
		return false;
	};

	if (OutReason != nullptr)
	{
		*OutReason = TEXT("legal");
	}

	// There was a "not goals mode" refusal here (spec v25 made the pull goals-only). Goals is the
	// only mode now, so it could never fire.
	if (!IsValid(Puller) || !Puller->IsAlive())
	{
		return Refuse(TEXT("no living puller"));
	}
	if (!bLoose)
	{
		return Refuse(TEXT("the Core is not loose"));
	}
	if (Puller == Carrier)
	{
		return Refuse(TEXT("the puller is holding the Core"));     // §7's precedence: carriers parry.
	}
	if (!IsTurnoverActive())
	{
		return Refuse(TEXT("no turnover window is open"));         // Row 1 and row 3: nobody pulls.
	}
	if (PullWinner != nullptr)
	{
		return Refuse(TEXT("a pull has already completed"));
	}

	// *** THE LOCKOUT IS ON THE TEAM THAT DROPPED IT, NOT ON THE INDIVIDUAL. *** Spec v25 is explicit,
	// and it is the whole difference between "the player who threw it away" and "their side": a
	// teammate of the thrower is refused here exactly as the thrower is.
	const ETraceTeam PullingTeam = GetTurnoverPullingTeam();
	if (PullingTeam == ETraceTeam::None)
	{
		return Refuse(TEXT("the turnover has no opposing team"));
	}
	if (Puller->GetTeam() != PullingTeam)
	{
		return Refuse(TEXT("on the team that dropped it - locked out"));
	}

	const FVector ViewLocation = Puller->GetPawnViewLocation();
	const FVector CoreCentre = LooseLocation;

	FVector ToCore = CoreCentre - ViewLocation;
	const double Distance = ToCore.Size();

	const float MaxRange = TraceModeBTuning::PullMaxRangeUU();
	if (MaxRange > 0.f && Distance > static_cast<double>(MaxRange))
	{
		return Refuse(TEXT("out of pull range"));
	}

	// LINE OF SIGHT, and deliberately the SAME channel and the same argument as the pass's
	// (IsLegalPassTarget): ECC_Visibility, because an object-type query matches the endzone trigger -
	// a QueryOnly box that responds to nothing but ECC_Pawn - and would make a Core lying inside a
	// zone unpullable for a reason nobody could see. Single ray, unlike the pass's three: the pass
	// probes a 176 uu pawn that ducks behind cover, this probes a 20 uu ball on the floor, and there
	// is no second point on it worth asking about.
	const UWorld* World = GetWorld();
	if (World != nullptr)
	{
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceCorePullLos), /*bTraceComplex=*/false);
		QueryParams.AddIgnoredActor(this);
		QueryParams.AddIgnoredActor(Puller);

		if (World->LineTraceTestByChannel(ViewLocation, CoreCentre, ECC_Visibility, QueryParams))
		{
			return Refuse(TEXT("no line of sight"));
		}
	}

	if (Distance <= CoreGeometryEpsilon)
	{
		return true;   // Standing on top of it. Nothing sensible to measure; accept.
	}

	ToCore /= Distance;

	const FVector AimDirection = Puller->GetAimDirection();
	const double Cosine = FVector::DotProduct(ToCore, AimDirection);
	if (Cosine <= 0.0)
	{
		return Refuse(TEXT("the Core is behind them"));
	}

	// (a) THE CONE - what makes a Core acquirable at all at range. A 20 uu orb subtends 0.14 deg at
	//     8000 uu, so a pure ray-through-the-ball test is unusable across this pitch.
	const double ConeDegrees = static_cast<double>(TraceModeBTuning::PullAimConeDegrees());
	if (Cosine >= FMath::Cos(FMath::DegreesToRadians(ConeDegrees)))
	{
		return true;
	}

	// (b) THE RAY THROUGH THE ORB - what makes it acquirable point-blank, where the cone has
	//     collapsed to a few uu. Perpendicular distance from the Core to the aim ray, against the
	//     DRAWN orb radius plus the slack, so the forgiveness is measured off the ball a player can
	//     actually see rather than off the larger sphere the flight sweeps with.
	const double PerpendicularDistance = Distance * FMath::Sqrt(FMath::Max(0.0, 1.0 - Cosine * Cosine));
	const double Threshold = TraceModeBVisibleOrbRadius + static_cast<double>(TraceModeBTuning::PullAimSlackUU());
	if (PerpendicularDistance <= Threshold)
	{
		return true;
	}

	return Refuse(TEXT("not hovering the Core"));
}

void ATraceCore::RegisterTurnover(ETraceTeam DroppingTeam, const FVector& Where, const TCHAR* Why)
{
	if (!HasAuthority() || DroppingTeam == ETraceTeam::None)
	{
		return;
	}

	TurnoverLockoutTeam = DroppingTeam;
	TurnoverStartServerTime = GetServerTimeSeconds();
	bTurnoverRegisteredThisFlight = true;

	// DEMO 29 §3(b). A fresh window counts its own refusals and gets its own log budget, so the count
	// printed on a refusal line is always about the lockout the reader is looking at.
	LockoutRefusalCount = 0;
	LastLockoutRefusalLogServerTime = -1.f;

	// *** SPEC v28 §2. THE CoreTurnover SOUND USED TO BE PLAYED FROM HERE, AND THAT WAS THE BUG. ***
	//
	// v26 §9 put it on this line because this function is called "RegisterTurnover". But this function
	// runs at the end of a LANDING: ServerSurfaceTurnover is its only shipping caller, and it fires
	// once a thrown Core has stopped bouncing, come to rest and stayed still for the settle. So the
	// sound announced the frame the five-second lockout OPENED and the other side became free to take
	// the Core — "when a team picks up a core which was locked out", in the owner's words — and it
	// announced nothing at all when a carrier was shot dead, because a kill never reaches here.
	//
	// It now fires from AnnounceTurnoverSound(), at the moment a team STOPS HOLDING THE CORE: the
	// throw that drops it, and the death that takes it. Trace.Audio.TurnoverEdge 0 puts it back on
	// this line, unchanged, which is the red arm. See AnnounceTurnoverSound.
	if (TraceModeBTuning::LegacyTurnoverSoundEdge())
	{
		TraceAudio::PlayAt(this, TraceSoundEvents::CoreTurnover, Where);
	}

#if !UE_BUILD_SHIPPING
	// Captured HERE, by the rule itself, for the same reason TakeLooseCore captures its take: by the
	// time Trace.ModeB.Verify's step 6/7/8 gets to judge, an enemy may already have pulled the Core
	// and cleared the window, and a scenario that polled for it would report a rule that had fired
	// as one that had not.
	if (bVerifyAwaitingTurnover)
	{
		bVerifyAwaitingTurnover = false;
		bVerifyTurnoverSeen = true;
		VerifyTurnoverLockedTeam = DroppingTeam;
	}
#endif

	// The Core is at rest on the surface it landed on and STAYS there. Zeroing the velocity is what
	// makes that true on the clients too: their dead reckoning is gated on a non-zero velocity, so a
	// Core left with the last frame's residual would keep drifting on every machine but this one.
	LooseVelocity = FVector::ZeroVector;
	LooseLocation = Where;
	bLooseAtRest = true;
	SetActorLocation(LooseLocation, false, nullptr, ETeleportType::TeleportPhysics);

	// THE LANDING LATCH IS CONSUMED HERE. It has done its job (spec v19 §1.5's settle, which is what
	// held possession until the ball had actually been drawn touching something) and it also gates the
	// pickup poll - leaving it set would make the Core unpickupable by ANYBODY for the rest of the
	// window, which is the opposite of what the turnover is for.
	ClearPendingTurnover();

	// A fresh window starts with a clean race: no holds carried over from before the landing, and no
	// stale winner. The latch (PullInputHeld) is deliberately NOT cleared - a player who was already
	// holding right mouse when it landed has their finger down, and the next tick will start their
	// 0.3 s honestly rather than demanding they let go and press again.
	PullHolds.Reset();
	PullWinner = nullptr;

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[ModeB] spec v25 §2: turnover window OPEN at %s (%s). Locked out: %s. Pull: %s, %.2fs hold."),
		*Where.ToCompactString(), Why, *TraceTeamName(DroppingTeam).ToString(),
		*TraceTeamName(TraceOpposingTeam(DroppingTeam)).ToString(), GetPullHoldSeconds());

	// The beam is the field-wide read of this whole mechanic, so it changes on the same frame the
	// window opens rather than on the next reconciliation tick.
	ApplyAttachment();
	UpdateVisuals();
}

void ATraceCore::AnnounceTurnoverSound(const FVector& Where, const ATraceCharacter* Loser, const TCHAR* Why)
{
	if (!HasAuthority())
	{
		return;
	}

	if (TraceModeBTuning::LegacyTurnoverSoundEdge())
	{
		return;   // RED ARM: the sound lives on RegisterTurnover's landing instead. See that function.
	}

	// ONE ANNOUNCEMENT PER LOSS. A carrier's death reaches this twice by design, not by accident:
	// ATraceGameMode::NotifyCharacterDied calls DropAt() from inside the health component's OnDeath
	// broadcast, and ATraceCore::OnHolderDeath is a second listener on the SAME broadcast. Both are
	// correct places to announce from — DropAt is the only one a disconnect reaches, OnHolderDeath is
	// the only one that knows about a killer — so the de-dup is here rather than a decision about
	// which of them "really" owns the event.
	//
	// KEYED ON THE PAWN AS WELL AS THE TIME. A window alone would swallow a genuine second turnover:
	// kill the carrier (sound), the killer takes the Core and throws it away 0.1 s later (a second,
	// different loss, by a different pawn) — that must be two sounds, and with the pawn in the key it
	// is. The window only ever collapses the two handlers of ONE pawn's ONE loss.
	const float Now = GetServerTimeSeconds();
	constexpr float SameEventWindow = 0.25f;

	if (Loser != nullptr
		&& LastTurnoverSoundLoser.Get() == Loser
		&& (Now - LastTurnoverSoundServerTime) < SameEventWindow
		&& (Now - LastTurnoverSoundServerTime) >= 0.f)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Audio] spec v28 §2: CoreTurnover already announced %.3fs ago for %s (%s); not doubling it."),
			Now - LastTurnoverSoundServerTime, *GetNameSafe(Loser), Why);
		return;
	}

	LastTurnoverSoundLoser = Loser;
	LastTurnoverSoundServerTime = Now;

	// GAME-SIDE, and the call site cannot choose otherwise: TraceSoundEvents' table declares
	// CoreTurnover as ETraceSoundSide::World and TraceAudio::PlayAt multicasts it from the authority.
	// `Where` is passed explicitly because a turnover happens at a POINT — the hand the Core left, or
	// the spot the carrier died on — and by the time this returns the Core may already be somewhere
	// else entirely.
	TraceAudio::PlayAt(this, TraceSoundEvents::CoreTurnover, Where);

	UE_LOG(LogTraceGame, Display,
		TEXT("[Audio] spec v28 §2: TURNOVER SOUND (game-side) at %s - %s lost the Core (%s)."),
		*Where.ToCompactString(), *GetNameSafe(Loser), Why);
}

void ATraceCore::ClearTurnover(const TCHAR* Why)
{
	if (TurnoverLockoutTeam == ETraceTeam::None && PullHolds.Num() == 0 && PullWinner == nullptr)
	{
		return;
	}

	if (TurnoverLockoutTeam != ETraceTeam::None)
	{
		// The refusal tally is DEMO 29 §3(b)'s: how many times this window actually turned somebody
		// away. Zero is the ordinary case (nobody went near it) and is not a failure; a non-zero
		// number is the rule caught doing its job, on the same line as the window it belongs to.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] spec v25 §2: turnover window CLOSED (%s) after %.2fs - %s is no longer locked ")
			TEXT("out and nobody may pull. %d pickup(s) refused while it was open."),
			Why, GetServerTimeSeconds() - TurnoverStartServerTime,
			*TraceTeamName(TurnoverLockoutTeam).ToString(), LockoutRefusalCount);
	}

	TurnoverLockoutTeam = ETraceTeam::None;
	TurnoverStartServerTime = 0.f;
	PullHolds.Reset();
	PullWinner = nullptr;

	ApplyAttachment();
	UpdateVisuals();
}

void ATraceCore::RequestPullInput(bool bPressed, ATraceCharacter* Requester)
{
	if (!IsValid(Requester))
	{
		return;
	}

	if (Requester->HasAuthority())
	{
		// The listen host's own player, and every bot. No round trip, and no prediction either: the
		// hold this starts is the server's, which is the only one there is.
		ServerApplyPullInput(Requester, bPressed);
		return;
	}

	// A CLIENT. It sends the BUTTON and nothing else - not a hold length, not a completion, not a
	// winner. See ATraceCorePullRelay for why the message cannot go on this actor.
	if (!Requester->IsLocallyControlled())
	{
		return;   // One machine speaks for one pawn.
	}

	if (ATraceCorePullRelay* Relay = ATraceCorePullRelay::Find(Requester->GetController()))
	{
		Relay->ServerSetPullInput(bPressed);
	}
	else
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeB] spec v25 §2: %s pressed pull but has no ATraceCorePullRelay yet (it replicates ")
			TEXT("shortly after PostLogin); the press is dropped rather than predicted."),
			*GetNameSafe(Requester));
	}
}

void ATraceCore::ServerApplyPullInput(ATraceCharacter* Requester, bool bPressed)
{
	if (!HasAuthority() || !IsValid(Requester))
	{
		return;
	}

	PullInputHeld.RemoveAll([](const TWeakObjectPtr<ATraceCharacter>& Entry)
	{
		return !Entry.IsValid();
	});

	const int32 Existing = PullInputHeld.IndexOfByPredicate([Requester](const TWeakObjectPtr<ATraceCharacter>& Entry)
	{
		return Entry.Get() == Requester;
	});

	if (bPressed)
	{
		if (Existing == INDEX_NONE)
		{
			PullInputHeld.Add(Requester);
		}
		return;
	}

	if (Existing != INDEX_NONE)
	{
		PullInputHeld.RemoveAt(Existing);
	}

	// *** RELEASING CANCELS. IT DOES NOT PAUSE. *** Spec v25 states it outright, and it is the rule a
	// 0.29 s release has to fail on: the entry is removed, so the next press starts a new hold at zero
	// rather than resuming 0.29 s of credit.
	CancelPullFor(Requester, TEXT("the button was released"), /*bAlsoClearLatch=*/false);
}

void ATraceCore::CancelPullFor(const ATraceCharacter* Puller, const TCHAR* Why, bool bAlsoClearLatch)
{
	if (!HasAuthority())
	{
		return;
	}

	const int32 Index = PullHolds.IndexOfByPredicate([Puller](const FTraceCorePullHold& Entry)
	{
		return Entry.Puller == Puller;
	});

	if (Index != INDEX_NONE)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeB] spec v25 §2: %s's pull CANCELLED at %.0f%% - %s."),
			*GetNameSafe(Puller),
			100.f * FMath::Clamp((GetServerTimeSeconds() - PullHolds[Index].StartServerTime)
				/ FMath::Max(KINDA_SMALL_NUMBER, GetPullHoldSeconds()), 0.f, 1.f),
			Why);

		PullHolds.RemoveAt(Index);
	}

	if (bAlsoClearLatch)
	{
		PullInputHeld.RemoveAll([Puller](const TWeakObjectPtr<ATraceCharacter>& Entry)
		{
			return !Entry.IsValid() || Entry.Get() == Puller;
		});
	}
}

void ATraceCore::ServerTickTurnover(float /*DeltaSeconds*/)
{
	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	// --- 1. THE WINDOW EXPIRES ON THE TEAM'S CLOCK, NOT ON ANY PLAYER'S. -------------------------
	//
	// Row 3 of the table. Clearing it is the whole of "after the 5 seconds are up, the opposite team
	// loses the pull ability and either team can pick up the core by running over it": no pull,
	// because CanPullNow refuses without a window; either team, because ServerTryLoosePickup's
	// lockout test is written against the same window; normal beam, because UpdateVisuals is.
	if (TurnoverLockoutTeam != ETraceTeam::None && GetTurnoverSecondsRemaining() <= 0.f)
	{
		ClearTurnover(TEXT("the lockout expired"));
	}

	// --- 2. Forget anybody who has left the match, so a dead pawn cannot hold a fill open. -------
	PullInputHeld.RemoveAll([](const TWeakObjectPtr<ATraceCharacter>& Entry)
	{
		const ATraceCharacter* Character = Entry.Get();
		return Character == nullptr || !IsValid(Character) || !Character->IsAlive();
	});

	// --- 3. Validate every live hold. CANCELS, never pauses. ------------------------------------
	for (int32 Index = PullHolds.Num() - 1; Index >= 0; --Index)
	{
		ATraceCharacter* Puller = PullHolds[Index].Puller;

		const bool bStillHolding = PullInputHeld.ContainsByPredicate(
			[Puller](const TWeakObjectPtr<ATraceCharacter>& Entry) { return Entry.Get() == Puller; });

		const TCHAR* Reason = TEXT("unknown");
		if (!bStillHolding)
		{
			CancelPullFor(Puller, TEXT("the button is no longer held"), /*bAlsoClearLatch=*/false);
		}
		else if (!CanPullNow(Puller, &Reason))
		{
			// Losing hover and losing line of sight arrive here identically, which is what the spec
			// asks for: "Losing either cancels the fill; it does not pause it."
			CancelPullFor(Puller, Reason, /*bAlsoClearLatch=*/false);
		}
	}

	// --- 4. Start a hold for anybody who is eligible and has not got one. ------------------------
	for (const TWeakObjectPtr<ATraceCharacter>& Entry : PullInputHeld)
	{
		ATraceCharacter* Puller = Entry.Get();
		if (Puller == nullptr)
		{
			continue;
		}

		const bool bAlreadyPulling = PullHolds.ContainsByPredicate(
			[Puller](const FTraceCorePullHold& Hold) { return Hold.Puller == Puller; });

		if (bAlreadyPulling || !CanPullNow(Puller))
		{
			continue;
		}

		FTraceCorePullHold& Added = PullHolds.AddDefaulted_GetRef();
		Added.Puller = Puller;
		Added.StartServerTime = Now;

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeB] spec v25 §2: %s (%s) started a pull on the turned-over Core."),
			*GetNameSafe(Puller), *TraceTeamName(Puller->GetTeam()).ToString());
	}

	// --- 5. FIRST TO COMPLETE WINS. -------------------------------------------------------------
	//
	// "Two opponents pulling at once: the one who finishes first gets it, and the other's fill is
	// cancelled." Every hold has the same length, so the first to finish is the one that STARTED
	// first, and the winner is picked by earliest StartServerTime rather than by array order. An
	// exact tie - two presses inside one server tick - breaks on the engine's unique object id, which
	// is stable for the lifetime of the pawn, for the same reason the catch contest's tie-break is:
	// resolving it by roster order would make the answer depend on the order actors happened to be
	// gathered in, which is not a fact about the game.
	const float Hold = GetPullHoldSeconds();

	ATraceCharacter* Winner = nullptr;
	float WinnerStart = 0.f;
	uint32 WinnerKey = 0;

	for (const FTraceCorePullHold& Entry : PullHolds)
	{
		if (!IsValid(Entry.Puller) || (Now - Entry.StartServerTime) < Hold)
		{
			continue;
		}

		const uint32 Key = Entry.Puller->GetUniqueID();
		if (Winner == nullptr || Entry.StartServerTime < WinnerStart
			|| (Entry.StartServerTime == WinnerStart && Key < WinnerKey))
		{
			Winner = Entry.Puller;
			WinnerStart = Entry.StartServerTime;
			WinnerKey = Key;
		}
	}

	if (Winner != nullptr)
	{
		ServerCompletePull(Winner);
	}
}

void ATraceCore::ServerCompletePull(ATraceCharacter* Winner)
{
	if (!HasAuthority() || !IsValid(Winner) || !bLoose)
	{
		return;
	}

	const int32 Losers = FMath::Max(0, PullHolds.Num() - 1);

	// THE LOSER'S FILL CANCELS. Clearing the array is that sentence; their LATCH is left alone,
	// because their finger really is still on the button and if this delivery is voided (the winner
	// dies mid-flight) the next tick should let them start a fresh 0.3 s rather than making them
	// re-press a button they never let go of.
	PullHolds.Reset();

	PullWinner = Winner;
	bLooseAtRest = false;

	// *** THE FULL CORE-THROWN VELOCITY, ASKED OF THE THROW ITSELF. ***
	//
	// Spec v25: "it travels towards the player who completed the pull first at full core thrown
	// velocity ... the same speed constant a thrown Core uses, not a new number." GetThrowSpeed() is
	// that constant AFTER the weight model (base / sqrt(mass)), which is the speed a thrown Core
	// actually leaves at, so retuning CoreThrowSpeed or CoreMassScale moves the pull with it. This is
	// the standing rule from Demo 21 applied to a speed instead of an ability: derived, not duplicated.
	const float Speed = GetThrowSpeed();
	const FVector ToWinner = (Winner->GetActorLocation() - FVector(LooseLocation)).GetSafeNormal();
	LooseVelocity = ToWinner * Speed;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] spec v25 §2: PULL COMPLETE - %s (%s) held for %.2fs and the Core is travelling to ")
		TEXT("them at %.0f uu/s (the full thrown speed). %d other fill(s) cancelled."),
		*GetNameSafe(Winner), *TraceTeamName(Winner->GetTeam()).ToString(), GetPullHoldSeconds(),
		Speed, Losers);
}

bool ATraceCore::ServerTickPullTravel(float DeltaSeconds)
{
	if (!HasAuthority() || PullWinner == nullptr)
	{
		return false;
	}

	ATraceCharacter* Winner = PullWinner;

	if (!IsValid(Winner) || !Winner->IsAlive() || !bLoose)
	{
		// The delivery has nobody to deliver to. The Core stops where it is and the window - if any of
		// it is left - carries on, so a team-mate can still earn it. It is deliberately NOT handed to
		// them by default: a pull is something a player completes, not something a death awards.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] spec v25 §2: the pull's winner is gone; the Core stops at %s and the window ")
			TEXT("(%.1fs left) continues."),
			*FVector(LooseLocation).ToCompactString(), GetTurnoverSecondsRemaining());

		PullWinner = nullptr;
		LooseVelocity = FVector::ZeroVector;
		bLooseAtRest = true;
		return false;
	}

	// The SAME speed constant the completion stamped, asked again rather than stored, so a live
	// retune reaches a delivery already in the air.
	const double Speed = static_cast<double>(GetThrowSpeed());
	const double Step = static_cast<double>(FMath::Clamp(DeltaSeconds, 0.f, 0.1f));

	// Aimed at the capsule CENTRE, which is the same point the pickup poll measures its radius from,
	// so the flight ends exactly where the take happens instead of a capsule-height away from it.
	const FVector Target = Winner->GetActorLocation();
	FVector ToTarget = Target - FVector(LooseLocation);
	const double Distance = ToTarget.Size();

	const double StepLength = Speed * Step;
	const double ArriveWithin = StepLength + static_cast<double>(TraceModeBTuning::PickupRadius());

	if (Distance <= ArriveWithin)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] spec v25 §2: the pulled Core reached %s."), *GetNameSafe(Winner));

		// Through TakeLooseCore like every other loose -> held transition, so the grace rule, the log
		// and Trace.ModeB.Verify all see a pull exactly as they see an interception. It clears the
		// loose state, which clears the turnover and this delivery with it.
		TakeLooseCore(Winner);
		return true;
	}

	const FVector Direction = ToTarget / Distance;
	LooseVelocity = Direction * Speed;
	LooseLocation = FVector(LooseLocation) + Direction * StepLength;
	SetActorLocation(LooseLocation, false, nullptr, ETeleportType::TeleportPhysics);

	return true;
}

bool ATraceCore::ServerTryLoosePickup()
{
	if (!HasAuthority() || !bLoose || bCoreStateLocked)
	{
		return false;
	}

	const float Now = GetServerTimeSeconds();
	const float Radius = TraceModeBTuning::PickupRadius();
	const float SelfLockoutEnd = LooseStartServerTime + TraceModeBTuning::SelfPickupLockout();
	const ATraceCharacter* Thrower = LooseThrower.Get();

	// SPEC v25 §2. Sampled once, outside the loop: the window cannot open or close between two
	// candidates within a single poll, and asking per candidate would make that look possible.
	const bool bLockoutActive = IsTurnoverActive();
	const ETraceTeam PullingTeam = GetTurnoverPullingTeam();

	// DEMO 29 §3(b). A locked-out player standing ON a Core they may not take is LOGGED, at most once
	// a second, with the count for the window on the line.
	//
	// WHY IT EXISTS AT ALL: "the scoring team is locked out" is a rule whose entire observable effect
	// is that nothing happens, and a rule that only ever produces silence cannot be told apart from a
	// rule that is not running. This is the line that says it ran, in a real match, with real
	// distances in it — the evidence the owner's report can be checked against, and the reason the
	// lockout does not need a harness of its own to be believed.
	//
	// RATE-LIMITED, NOT LATCHED: the poll runs every tick and five bots can be inside the radius at
	// once, so a per-frame line would be thousands of them across one window. One a second names the
	// pawn that is currently closest to taking it, which is the interesting one.
	constexpr float LockoutRefusalLogInterval = 1.0f;
	const bool bLogRefusals = bLockoutActive
		&& ((LastLockoutRefusalLogServerTime < 0.f)
			|| ((Now - LastLockoutRefusalLogServerTime) >= LockoutRefusalLogInterval));

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	ATraceCharacter* Best = nullptr;
	double BestOverlapSq = TNumericLimits<double>::Max();

	const FVector CoreLocation = LooseLocation;

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive())
		{
			continue;
		}

		// The ONE exception to "first contact, anyone". The Core leaves from inside the thrower's own
		// pickup radius, so without a brief lockout on them alone, every throw would be caught by the
		// player who threw it on the very next tick and the mechanic would not exist. Everybody else -
		// teammate or enemy - is eligible from frame one, which is what makes interception the point.
		if (Candidate == Thrower && Now < SelfLockoutEnd)
		{
			continue;
		}

		// Distance to the CAPSULE, not to the actor origin: the origin is at the pawn's midpoint, so
		// an origin test would refuse a Core rolling past a player's feet.
		double DistanceSq = FVector::DistSquared(CoreLocation, Candidate->GetActorLocation());
		if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
		{
			const FVector CapsuleCentre = Capsule->GetComponentLocation();
			const double HalfHeight = static_cast<double>(Capsule->GetScaledCapsuleHalfHeight());
			const double CapsuleRadius = static_cast<double>(Capsule->GetScaledCapsuleRadius());

			// Closest point on the capsule's axis, then subtract the radius off the distance.
			FVector Closest = CoreLocation;
			Closest.Z = FMath::Clamp(CoreLocation.Z, CapsuleCentre.Z - HalfHeight, CapsuleCentre.Z + HalfHeight);
			Closest.X = CapsuleCentre.X;
			Closest.Y = CapsuleCentre.Y;

			const double Surface = FMath::Max(0.0, FVector::Dist(CoreLocation, Closest) - CapsuleRadius);
			DistanceSq = Surface * Surface;
		}

		if (DistanceSq > static_cast<double>(Radius) * static_cast<double>(Radius))
		{
			continue;
		}

		// *** SPEC v25 §2. THE 5 s LOCKOUT IS ON THE TEAM THAT DROPPED IT — AND, SINCE DEMO 29 §3(b),
		// ON THE TEAM THAT JUST SCORED. ***
		//
		// Row 2 of the table: during the window only the opposing team may pick the Core up, and this
		// is the whole of that half of it. Row 3 needs no code - IsTurnoverActive() goes false when the
		// window expires and the loop is back to "first contact, anyone", which is exactly "either team
		// can pick up the core by running over it".
		//
		// Written against the TEAM and not against LooseThrower on purpose: the thrower's own
		// lockout above is a 0.35 s anti-self-catch on ONE pawn, and this is a 5 s rule about a SIDE.
		// Conflating them would let a team-mate of the thrower jog over and reclaim the turnover.
		//
		// THROUGH IsTeamLockedOutOfCore() SO THERE IS ONE COPY OF IT. The post-goal lockout and the
		// probe that proves it both ask that function; a second hand-written comparison here is how
		// two tests come to disagree. It is the same expression this line always was, ETraceTeam::None
		// included. Moved BELOW the radius test, which is a pure filter reorder and changes no
		// verdict — it is what lets the refusal below report a real distance.
		if (IsTeamLockedOutOfCore(Candidate->GetTeam()))
		{
			++LockoutRefusalCount;

			if (bLogRefusals)
			{
				LastLockoutRefusalLogServerTime = Now;
				UE_LOG(LogTraceGame, Display,
					TEXT("[Core] DEMO 29 §3(b) LOCKOUT REFUSED %s (%s) at %.0f uu from the Core — %s may ")
					TEXT("not take it for another %.1fs; %s may. %d refusals this window."),
					*GetNameSafe(Candidate), *TraceTeamName(Candidate->GetTeam()).ToString(),
					FMath::Sqrt(DistanceSq),
					*TraceTeamName(TurnoverLockoutTeam).ToString(), GetTurnoverSecondsRemaining(),
					*TraceTeamName(PullingTeam).ToString(), LockoutRefusalCount);
			}

			continue;
		}

		if (DistanceSq < BestOverlapSq)
		{
			BestOverlapSq = DistanceSq;
			Best = Candidate;
		}
	}

	if (Best == nullptr)
	{
		return false;
	}

	TakeLooseCore(Best);
	return true;
}

void ATraceCore::TakeLooseCore(ATraceCharacter* Taker)
{
	if (!HasAuthority() || !IsValid(Taker) || bCoreStateLocked)
	{
		return;
	}

	FCoreStateLock Lock(this);

	const ETraceTeam FromTeam = LooseFromTeam;
	const ETraceTeam TakerTeam = Taker->GetTeam();

	// THE GRACE RULE, and the reason ETraceCoreGrantReason grew two enumerators rather than one.
	// Spec v4 §7 verbatim: turnovers keep mode A's grace, teammates picking it up get none. Both
	// halves are decided by the same AreAllies() line inside GrantTo - all this does is tell GrantTo
	// which team the Core came FROM, since there is no previous holder left to ask.
	const bool bTeamChanged = !(FromTeam != ETraceTeam::None && FromTeam == TakerTeam);
	const ETraceCoreGrantReason Reason = bTeamChanged
		? ETraceCoreGrantReason::Interception
		: ETraceCoreGrantReason::Recovery;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] %s by %s (%s) - Core was thrown by %s, %s"),
		bTeamChanged ? TEXT("INTERCEPTION") : TEXT("RECOVERY"),
		*GetNameSafe(Taker), *TraceTeamName(TakerTeam).ToString(),
		*TraceTeamName(FromTeam).ToString(),
		bTeamChanged ? TEXT("turnover grace applies") : TEXT("no grace, same team"));

	// Clear the loose state BEFORE granting, and set the single-use grace override in the same
	// breath. GrantTo attaches the Core to the taker; leaving bLoose true across that call would let
	// any re-entrant tick integrate a Core that is now parented to a pawn.
#if !UE_BUILD_SHIPPING
	// Captured for Trace.ModeB.Verify BEFORE the grant, because GrantTo can score, reset the field
	// and hand the Core straight back out again — by the time it returns, the take this scenario
	// caused is no longer the possession anybody can observe.
	if (bVerifyAwaitingTake)
	{
		bVerifyAwaitingTake = false;
		bVerifyTakeSeen = true;
		VerifyTookTeam = TakerTeam;
		VerifyFromTeam = FromTeam;
		bVerifyTookGrace = bTeamChanged;
		VerifyTakerName = GetNameSafe(Taker);
	}
#endif

	ClearLooseState();
	GraceOverrideTeam = FromTeam;

	GrantTo(Taker, Reason);

	// GrantTo consumes it, but clear again unconditionally: if GrantTo refused the grant (a dead
	// taker between the poll and here) a stale override must not survive into the next possession.
	GraceOverrideTeam = ETraceTeam::None;
}

void ATraceCore::ClearLooseState()
{
	bLoose = false;
	bLooseAtRest = false;
	bLooseFromThrow = false;

	// DEMO 29 §3. The placement is over the instant the Core stops being loose — somebody took it,
	// a goal reset it, the mode changed. Cleared HERE, at the one funnel every one of those paths
	// runs through, so the flag can never describe a Core that is now in somebody's hands and hold
	// the reset backstop open against a possession it knows nothing about.
	bContestedKickoff = false;
	ContestedFallbackTeam = ETraceTeam::None;
	CatchZoneTarget = nullptr;
	bCatchZoneContested = false;   // Spec v13 §5: a new flight starts its own contest.
	ForgetLastContact();           // Spec v13 §8: and its own contact history, Demo 27's actor with it.
	LaunchAuditDueServerTime = -1.f;  // Demo 27: a flight that ended early is not audited.
	ClearPendingTurnover();        // Spec v19 §1.5: and its own landing.

	// SPEC v25 §2. And its own turnover. Every path that ends a flight comes through here, so this is
	// what guarantees the window, the pull race and the delivery cannot outlive the Core being loose -
	// a turnover left set on a Core somebody is now carrying would lock a whole team out of a Core
	// that is not even on the ground. The LATCH (bTurnoverRegisteredThisFlight) clears with it, so the
	// next throw is judged on its own landing.
	ClearTurnover(TEXT("the Core is no longer loose"));
	bTurnoverRegisteredThisFlight = false;
	PullInputHeld.Reset();

	LooseVelocity = FVector::ZeroVector;
	LooseThrower = nullptr;
	LooseStartServerTime = 0.f;
	// LooseFromTeam is deliberately NOT cleared here: TakeLooseCore reads it immediately afterwards
	// to decide the grace, and KickoffTo/GrantTo overwrite it on the next possession.
}

void ATraceCore::ClearPendingTurnover()
{
	PendingTurnoverLandedServerTime = -1.f;
	PendingTurnoverLandedFrame = 0;
	PendingTurnoverSurfacePoint = FVector::ZeroVector;
	PendingTurnoverSurfaceNormal = FVector::UpVector;
	PendingTurnoverArrivalSpeed = 0.0;
	PendingTurnoverArrivalSin = 1.0;
	bPendingTurnoverByRestProbe = false;
}

void ATraceCore::ResetLooseCore(const TCHAR* Reason)
{
	if (!HasAuthority() || bCoreStateLocked)
	{
		return;
	}

	FCoreStateLock Lock(this);

	// The team that did NOT throw it away gets the restart. A throw nobody collected is a wasted
	// possession, and handing it back to the side that wasted it would make stalling free.
	const ETraceTeam Owed = (LooseFromTeam != ETraceTeam::None)
		? TraceOpposingTeam(LooseFromTeam)
		: TraceCoreTuning::DefaultKickoffTeam;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] loose Core reset (%s) after %.1fs - kickoff to %s"),
		Reason, GetServerTimeSeconds() - LooseStartServerTime, *TraceTeamName(Owed).ToString());

	ClearLooseState();
	LooseFromTeam = ETraceTeam::None;

	KickoffTo(Owed);
}


bool ATraceCore::GetLooseCoreInterceptPoint(float LeadSeconds, FVector& OutPoint) const
{
	if (!bLoose)
	{
		return false;
	}

	const float Lead = FMath::Clamp(LeadSeconds, 0.f, 2.f);

	// BALLISTIC, not linear. A straight extrapolation of the velocity was close enough while the Core
	// flew flat under 55% gravity; under the v5 weight model it falls at roughly twice that rate and
	// leaves the arc within a quarter of a second, so a chaser led by velocity alone runs at a point
	// well above where the Core will actually be. Same integration the loose tick performs, one step.
	const float GravityZ = (bLooseAtRest || GetWorld() == nullptr) ? 0.f : GetThrowGravityZ(GetWorld());

	OutPoint = FVector(LooseLocation)
		+ FVector(LooseVelocity) * Lead
		+ FVector(0.0, 0.0, 0.5 * static_cast<double>(GravityZ) * static_cast<double>(Lead) * static_cast<double>(Lead));
	return true;
}

