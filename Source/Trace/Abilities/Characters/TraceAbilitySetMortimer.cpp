// Trace — MORTIMER. See the header for the spec v19 §3 reading, for the mantle's history, and for
// exactly which half of this character is live and which half is a knob waiting on somebody else's
// one-line call site.

#include "Abilities/Characters/TraceAbilitySetMortimer.h"

#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

#define LOCTEXT_NAMESPACE "TraceMortimer"

// =================================================================================================
// THE RED ARMS. One per rule, each removing that rule and nothing else, so the verification command
// below can be made to FAIL on an otherwise identical build. Same shape and the same reasoning as
// TraceAbilitySetChut.cpp's three.
// =================================================================================================

/**
 * *** THE ONE THAT MATTERS. *** 0 removes the CanAffectTarget(Victim, Control) call from Quake's
 * per-victim path and NOTHING ELSE — the search, the radius, the falloff and the launch are
 * byte-identical. So a Core carrier standing beside Mortimer IS thrown, which is the founding
 * invariant broken, which is precisely what Trace.Mortimer.BlastCarrierTest's first arm has to
 * reproduce before its second arm's "0 uu/s" means anything at all.
 *
 * ECVF_Cheat, and never ship 0.
 */
static TAutoConsoleVariable<int32> CVarMortimerBlastChoke(
	TEXT("Trace.Mortimer.BlastChoke"),
	1,
	TEXT("Dev/red arm. 1 (default) = Quake asks the spec v14 §4 carrier choke point about every victim, so it "
	     "can never displace a Core carrier. 0 = the choke point is not consulted, so it CAN — the arm that "
	     "makes Trace.Mortimer.BlastCarrierTest's green half mean something. NEVER SHIP 0."),
	ECVF_Cheat);

/**
 * 0 removes §3's posture gate — "only while carrying the core AND standing on the ground or the top
 * of an object" — so E fires from anywhere. Exists so the posture assertions can go red, and so the
 * blast itself can be exercised in a fixture that has no Core.
 */
static TAutoConsoleVariable<int32> CVarMortimerBlastPosture(
	TEXT("Trace.Mortimer.BlastPosture"),
	1,
	TEXT("Dev/red arm. 1 (default) = Quake is refused unless Mortimer is carrying the Core AND grounded "
	     "(spec v19 §3). 0 = it fires from anywhere, so every posture assertion must go red."),
	ECVF_Cheat);

const TCHAR* TraceMortimerBlastRefusalToString(ETraceMortimerBlastRefusal Reason)
{
	switch (Reason)
	{
	case ETraceMortimerBlastRefusal::Allowed:         return TEXT("Allowed");
	case ETraceMortimerBlastRefusal::NoPawn:          return TEXT("NoPawn");
	case ETraceMortimerBlastRefusal::NotCarryingCore: return TEXT("NotCarryingCore");
	case ETraceMortimerBlastRefusal::Airborne:        return TEXT("Airborne");
	default:                                          return TEXT("<invalid>");
	}
}

// =================================================================================================
// PASSIVE — the two halves. Both are pure reads of UTraceSettings, so they retune live.
// =================================================================================================

float UTraceAbilitySetMortimer::GetDashDistanceScale() const
{
	// Clamped rather than trusted: a zero here would be a dash that does not move him at all, which
	// is not "75% shorter", it is a broken movement kit with no error message.
	return FMath::Clamp(UTraceSettings::Get().MortimerDashDistanceScale, 0.05f, 4.f);
}

float UTraceAbilitySetMortimer::GetThrowChargeHoldScale() const
{
	// Floored at 1: a value below 1 would make Mortimer's throw WEAKER than everybody's, which is the
	// opposite of the passive and would look like the sign of the knob had been flipped.
	return FMath::Clamp(UTraceSettings::Get().MortimerThrowChargeHoldScale, 1.f, 8.f);
}

bool UTraceAbilitySetMortimer::AllowsMantle() const
{
	return UTraceSettings::Get().bMortimerCanMantle;
}

float UTraceAbilitySetMortimer::GetMantleGenerosityScale() const
{
	return FMath::Clamp(UTraceSettings::Get().MortimerMantleGenerosity, 1.f, 4.f);
}

// =================================================================================================
// ACTIVATED — QUAKE
// =================================================================================================

float UTraceAbilitySetMortimer::GetActivatedCooldownSeconds() const
{
	return FMath::Max(0.f, UTraceSettings::Get().MortimerBlastCooldownSeconds);
}

ETraceMortimerBlastRefusal UTraceAbilitySetMortimer::CheckBlastPosture() const
{
	const ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		return ETraceMortimerBlastRefusal::NoPawn;
	}

	if (CVarMortimerBlastPosture.GetValueOnAnyThread() == 0)
	{
		return ETraceMortimerBlastRefusal::Allowed;   // RED ARM: the gate is removed, nothing else is.
	}

	// "only while carrying the core". THE SAME PREDICATE THE CHOKE POINT USES, not a second one:
	// UTraceAbilityComponent::IsCarrier ORs the pawn's replicated mirror with ATraceCore's own
	// holder pointer, which is what makes this answer identical on the server and on the client that
	// predicted the press.
	if (!UTraceAbilityComponent::IsCarrier(MyPawn))
	{
		return ETraceMortimerBlastRefusal::NotCarryingCore;
	}

	// "AND standing on the ground or the top of an object".
	//
	// IsGroundedForAbilities(), not IsMovingOnGround(): it is the ABILITY LAYER's definition of
	// grounded and it carries LedgeGroundGraceSeconds of hysteresis, which exists precisely because a
	// one-frame contact blip on a ledge LIP reads as airborne on one machine and grounded on the
	// other. Since "the top of an object" is exactly where a player stands when they are on a lip,
	// using the raw engine answer here would refuse the ability in the one place §3 names.
	//
	// There is deliberately no separate test for "the top of an object" — the floor of the arena and
	// the roof of a crate are the same walkable surface to the movement component, and inventing a
	// second test would be inventing a rule the doc does not have.
	if (!MoveComp->IsGroundedForAbilities())
	{
		return ETraceMortimerBlastRefusal::Airborne;
	}

	return ETraceMortimerBlastRefusal::Allowed;
}

bool UTraceAbilitySetMortimer::CanActivate(FText& OutReason) const
{
	switch (CheckBlastPosture())
	{
	case ETraceMortimerBlastRefusal::Allowed:
		return true;

	case ETraceMortimerBlastRefusal::NotCarryingCore:
		// Phrased as an instruction rather than as a state, because it is one a player can act on —
		// see the UI note in the report: today this sentence exists only in the server log.
		OutReason = LOCTEXT("MortimerNoCore", "QUAKE NEEDS THE CORE");
		return false;

	case ETraceMortimerBlastRefusal::Airborne:
		OutReason = LOCTEXT("MortimerAirborne", "QUAKE NEEDS SOLID GROUND");
		return false;

	case ETraceMortimerBlastRefusal::NoPawn:
	default:
		OutReason = LOCTEXT("MortimerNoPawn", "NOT READY");
		return false;
	}
}

bool UTraceAbilitySetMortimer::ActivateAbility()
{
	if (CheckBlastPosture() != ETraceMortimerBlastRefusal::Allowed)
	{
		// FALSE, so the framework charges no cooldown. §3 makes Quake conditional, and a conditional
		// ability that eats its 20 s on a refused press is a trap rather than a condition.
		return false;
	}

	if (!HasAuthority())
	{
		// THE OWNING CLIENT PREDICTS NOTHING BUT THE PRESS. Every effect Quake has is somebody else's
		// POSITION, which is replicated from the server; a client that launched them locally would be
		// showing itself enemies flying who, a round trip later, never moved. Returning true is what
		// starts the local cooldown ring, which is the only thing there is to predict.
		++BlastCount;
		return true;
	}

	int32 Considered = 0;
	const int32 Knocked = RunBlast(Considered);
	++BlastCount;

	UE_LOG(LogTraceGame, Log,
		TEXT("[Mortimer] QUAKE #%d fired: %d of %d pawn(s) inside %.0f uu were knocked away. "
		     "(The rest were team-mates, out of sight, or refused by the spec §4 choke point.)"),
		BlastCount, Knocked, Considered, FMath::Max(1.f, UTraceSettings::Get().MortimerBlastRadiusUU));

	// TRUE EVEN WHEN NOBODY WAS HIT, and that is a decision. §3 makes the ability conditional on
	// MORTIMER'S OWN POSTURE and not on there being a victim, so a Quake in an empty room is a Quake
	// he chose to spend — the same call Rocco's Ripple makes and the opposite of Mace's spike, which
	// fizzles free because it can miss the WORLD.
	return true;
}

int32 UTraceAbilitySetMortimer::RunBlast(int32& OutConsidered)
{
	OutConsidered = 0;

	if (!HasAuthority())
	{
		return 0;
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UWorld* BlastWorld = (MyPawn != nullptr) ? MyPawn->GetWorld() : nullptr;
	if (MyPawn == nullptr || BlastWorld == nullptr)
	{
		return 0;
	}

	const float Radius = FMath::Max(1.f, UTraceSettings::Get().MortimerBlastRadiusUU);
	const float RadiusSquared = Radius * Radius;
	const FVector Origin = MyPawn->GetActorLocation();

	// A LIST FIRST, THEN THE LAUNCHES. LaunchCharacter writes Velocity, which cannot move an actor out
	// from under a TActorIterator — but it CAN run a listener that destroys one, and iterating a level
	// while it mutates is the kind of thing that works for a year and then does not.
	TArray<ATraceCharacter*, TInlineAllocator<16>> Candidates;
	for (TActorIterator<ATraceCharacter> It(BlastWorld); It; ++It)
	{
		ATraceCharacter* Other = *It;
		if (Other == nullptr || Other == MyPawn || !IsValid(Other) || !Other->IsAlive())
		{
			continue;
		}
		if (FVector::DistSquared(Other->GetActorLocation(), Origin) > RadiusSquared)
		{
			continue;
		}
		Candidates.Add(Other);
	}

	OutConsidered = Candidates.Num();

	int32 Knocked = 0;
	for (ATraceCharacter* Victim : Candidates)
	{
		if (ApplyBlastTo(Victim))
		{
			++Knocked;
		}
	}

	return Knocked;
}

bool UTraceAbilitySetMortimer::ApplyBlastTo(ATraceCharacter* Victim) const
{
	if (!HasAuthority())
	{
		return false;   // A knockback is server truth. See ActivateAbility's prediction note.
	}

	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || Victim == nullptr || Victim == MyPawn || !IsValid(Victim) || !Victim->IsAlive())
	{
		return false;
	}

	// =============================================================================================
	// *** THE CHOKE POINT. SPEC v14 §4, AND THE WHOLE REASON THIS FUNCTION IS SEPARATE. ***
	//
	// Control, because a knockback is movement the target did not ask for. This ONE call is what
	// makes "it cannot displace a Core carrier" true; there is no carrier test anywhere in this file
	// and there must not be one, because a second copy of the rule is a second thing that can rot.
	//
	// It also answers the team question, the dead question and the characters-disabled question, so
	// there is no friendly-fire test in this file either.
	//
	// READ THE HEADER'S LAST PARAGRAPH BEFORE DELETING THIS AS DEAD CODE. With one Core in play an
	// enemy carrier cannot exist while Mortimer is carrying, so in a real match this can never fire —
	// which is exactly why it is here and why the harness reaches this function directly.
	// =============================================================================================
	if (CVarMortimerBlastChoke.GetValueOnAnyThread() != 0
		&& !CanAffect(Victim, ETraceAbilityEffect::Control))
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Mortimer] Quake on %s refused by the choke point (carrier=%d)."),
			*GetNameSafe(Victim), UTraceAbilityComponent::IsCarrier(Victim) ? 1 : 0);
		return false;
	}

	if (!HasLineOfSightTo(Victim))
	{
		return false;
	}

	LaunchVictim(Victim, MyPawn->GetActorLocation());
	return true;
}

void UTraceAbilitySetMortimer::LaunchVictim(ATraceCharacter* Victim, const FVector& FromLocation) const
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	// AWAY FROM HIM, PLANAR, PLUS A POP. Planar because the victim's own height above or below
	// Mortimer must not tilt the shove into the floor or the sky; the deliberate vertical component
	// is MortimerBlastUpBias and only that.
	FVector Away = Victim->GetActorLocation() - FromLocation;
	Away.Z = 0.f;
	if (!Away.Normalize())
	{
		// Standing in exactly the same column — a corner case, not an impossible one. Throwing them
		// along Mortimer's facing is the only direction that means anything here; never a random one.
		const ATraceCharacter* MyPawn = GetCharacter();
		Away = (MyPawn != nullptr) ? MyPawn->GetActorForwardVector() : FVector::ForwardVector;
		Away.Z = 0.f;
		if (!Away.Normalize())
		{
			Away = FVector::ForwardVector;
		}
	}

	float Scale = 1.f;
	if (Settings.bMortimerBlastFallsOff)
	{
		const float Radius = FMath::Max(1.f, Settings.MortimerBlastRadiusUU);
		const float Distance = FMath::Clamp(
			static_cast<float>(FVector::Dist(Victim->GetActorLocation(), FromLocation)), 0.f, Radius);
		const float RimScale = FMath::Clamp(Settings.MortimerBlastMinFalloffScale, 0.f, 1.f);
		Scale = FMath::Lerp(1.f, RimScale, Distance / Radius);
	}

	const float Speed = FMath::Max(0.f, Settings.MortimerBlastKnockbackSpeed) * Scale;
	const FVector Impulse = Away * Speed + FVector::UpVector * (Speed * FMath::Max(0.f, Settings.MortimerBlastUpBias));

	// bXYOverride / bZOverride both true: Quake REPLACES the victim's velocity rather than adding to
	// it, so a player sprinting at Mortimer is thrown exactly as far as one standing still. Identical
	// to Chut's bash, and for the identical reason — a knockback whose strength depended on the
	// victim's own speed would be strongest against the people best placed to punish it.
	Victim->LaunchCharacter(Impulse, true, true);
}

bool UTraceAbilitySetMortimer::HasLineOfSightTo(const ATraceCharacter* Victim) const
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	if (!Settings.bMortimerBlastNeedsLineOfSight)
	{
		return true;
	}

	const ATraceCharacter* MyPawn = GetCharacter();
	UWorld* SightWorld = (MyPawn != nullptr) ? MyPawn->GetWorld() : nullptr;
	if (MyPawn == nullptr || Victim == nullptr || SightWorld == nullptr)
	{
		return false;
	}

	FCollisionQueryParams SightParams(SCENE_QUERY_STAT(TraceMortimerQuakeSight), /*bTraceComplex*/ false);
	// Both pawns are ignored, and both for the same reason the mantle's probes learned the hard way:
	// a trace that starts inside a collider reports that collider at distance zero, so without these
	// the blast would decide it could not see anybody, ever.
	SightParams.AddIgnoredActor(MyPawn);
	SightParams.AddIgnoredActor(Victim);

	FHitResult SightHit;
	const bool bBlocked = SightWorld->LineTraceSingleByChannel(
		SightHit, MyPawn->GetActorLocation(), Victim->GetActorLocation(), ECC_Visibility, SightParams);

	if (bBlocked)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Mortimer] Quake on %s blocked by %s."),
			*GetNameSafe(Victim), *GetNameSafe(SightHit.GetActor()));
	}

	return !bBlocked;
}

// =================================================================================================
// Trace.Mortimer.BlastCarrierTest — SPEC v14 §4, TWO ARMS, RED FIRST
//
// THE QUESTION: does Quake's per-victim path actually route through the carrier choke point?
//
// THE REASON IT IS NOT A LIVE-MATCH TEST: there is one Core, so while Mortimer holds it no enemy can
// be a carrier, and a harness that waited for that situation would wait forever and then report
// green for having measured nothing. So this drives ApplyBlastTo() directly — the same function the
// real ability calls once per victim — at a pawn the harness has deliberately handed the Core to.
//
// THE FIXTURE PROVES ITSELF. Every arm also blasts a CONTROL target who is a living enemy and NOT a
// carrier. If the control does not fly in both arms then the harness has measured nothing about the
// carrier rule and everything about a broken fixture, and it says INVALID rather than PASS. This is
// the failure mode Trace.Ability.CarrierChokeTest was caught in (1 run in 3 reported a vacuous
// green), and the guard is copied from its fix.
//
// SYNCHRONOUS, no ticker: ATraceCore::TryPickup is an authority-side debug grant that lands the same
// frame, and LaunchCharacter writes Velocity the same frame, so every reading below is taken from
// the same call stack that caused it.
// =================================================================================================

// NAMED after the file rather than anonymous — UBT builds this module as a unity/jumbo blob, so two
// files that each open `namespace { }` become one namespace holding both, and "FindEnemy" is exactly
// the kind of name another character file would also want. See Scripts/check-jumbo-build-collisions.py.
namespace TraceMortimerVerifyFile
{
	/** How much velocity change counts as "was displaced", in uu/s. Well below any real launch. */
	constexpr float DisplacedThreshold = 50.f;

	/**
	 * Makes the first HUMAN player Mortimer and returns their set.
	 *
	 * A human, and it must be: handing a BOT a character of the harness's choosing fights
	 * ATraceGameMode::PollCharacterSelect's 4 Hz fill for that player state and measures whichever
	 * won. Same reasoning and the same wording as Slimeball's MakePlayerIntoSlimeball.
	 */
	UTraceAbilitySetMortimer* MakePlayerIntoMortimer(UWorld* WorldPtr, FString& OutWhy)
	{
		if (WorldPtr == nullptr)
		{
			OutWhy = TEXT("no world");
			return nullptr;
		}

		if (!UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
		{
			OutWhy = TEXT("characters are DISABLED in this match (mode A, or the §3 toggle is off) — "
			              "run this in mode B with characters on");
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = Cast<APlayerController>(It->Get());
			if (PC == nullptr || PC->GetPawn() == nullptr)
			{
				continue;
			}
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PC->GetPawn());
			if (Comp == nullptr || Comp->IsBot())
			{
				continue;
			}

			if (Comp->GetCharacterId() != ETraceCharacterId::Mortimer)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::Mortimer);
			}

			if (UTraceAbilitySetMortimer* Found = Comp->GetAbilitySetAs<UTraceAbilitySetMortimer>())
			{
				return Found;
			}

			OutWhy = FString::Printf(
				TEXT("ServerSetCharacter(Mortimer) did not produce a UTraceAbilitySetMortimer (id is now %s) — "
				     "a team-mate may already hold him, or the reflection roster did not find the class"),
				TraceCharacterIdToString(Comp->GetCharacterId()));
			return nullptr;
		}

		OutWhy = TEXT("no human player controller with a pawn");
		return nullptr;
	}

	/** A living enemy of @p Mine, optionally skipping one already spoken for. */
	ATraceCharacter* FindLivingEnemy(UWorld* WorldPtr, const ATraceCharacter* Mine, const ATraceCharacter* Skip)
	{
		if (WorldPtr == nullptr || Mine == nullptr)
		{
			return nullptr;
		}
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Mine || Candidate == Skip || !Candidate->IsAlive())
			{
				continue;
			}
			if (Candidate->GetTeam() == Mine->GetTeam() || Candidate->GetTeam() == ETraceTeam::None)
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	/**
	 * Stand @p Target beside Mortimer, blast them, and answer how hard they were thrown, in uu/s.
	 *
	 * *** IT READS PendingLaunchVelocity, NOT Velocity, AND THAT IS THE WHOLE FIX. *** The first
	 * version of this harness read GetVelocity() before and after and measured 0.0 uu/s on EVERY arm,
	 * including the red one — a harness that could not go red. The cause is that
	 * ACharacter::LaunchCharacter does not write Velocity: UCharacterMovementComponent::Launch parks
	 * the impulse in PendingLaunchVelocity and HandlePendingLaunch spends it during the NEXT movement
	 * tick, so a same-frame reading of Velocity can only ever be zero. PendingLaunchVelocity is the
	 * value the ability actually produced, on the frame it produced it, and it is the honest same-call-
	 * stack observable. (Chut's bash has the identical shape; it looks instant in a match only because
	 * the world goes on to tick.)
	 *
	 * THE TARGET IS MOVED NEXT TO HIM FIRST, and that is not a convenience either: Quake requires line
	 * of sight, so blasting a bot standing across a 3.5:1 arena would be refused by geometry and the
	 * harness would report INVALID for a reason that has nothing to do with the rule under test.
	 * Standing them 150 uu away is what a real Quake looks like.
	 *
	 * @param OutApplied  what ApplyBlastTo itself returned, so "refused" and "launched with 0" are
	 *                    distinguishable in the log rather than both reading as a zero.
	 */
	float MeasureBlastOn(const UTraceAbilitySetMortimer* Mortimer, ATraceCharacter* Target,
	                     const FVector& Offset, bool& OutApplied)
	{
		OutApplied = false;

		const ATraceCharacter* MyPawn = (Mortimer != nullptr) ? Mortimer->GetCharacter() : nullptr;
		if (MyPawn == nullptr || Target == nullptr)
		{
			return -1.f;
		}

		Target->SetActorLocation(MyPawn->GetActorLocation() + Offset, /*bSweep*/ false, nullptr,
			ETeleportType::TeleportPhysics);

		UCharacterMovementComponent* TargetMove = Target->GetCharacterMovement();
		if (TargetMove == nullptr)
		{
			return -1.f;
		}

		TargetMove->PendingLaunchVelocity = FVector::ZeroVector;
		OutApplied = const_cast<UTraceAbilitySetMortimer*>(Mortimer)->ApplyBlastTo(Target);
		return static_cast<float>(TargetMove->PendingLaunchVelocity.Size());
	}

	void RunBlastCarrierTest()
	{
		const TCHAR* const Tag = TEXT("MORTIMERCHOKE");

		UWorld* TestWorld = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->IsGameWorld()
					&& Context.World()->GetAuthGameMode() != nullptr)
				{
					TestWorld = Context.World();
					break;
				}
			}
		}

		if (TestWorld == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. Run this on the server/host, in a live match."),
				Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetMortimer* Mortimer = MakePlayerIntoMortimer(TestWorld, Why);
		if (Mortimer == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — %s."), Tag, *Why);
			return;
		}

		ATraceCharacter* MyPawn = Mortimer->GetCharacter();
		ATraceCore* CoreActor = ATraceCore::Get(TestWorld);
		if (MyPawn == nullptr || CoreActor == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Mortimer has no pawn, or there is no Core in this world."), Tag);
			return;
		}

		ATraceCharacter* Victim = FindLivingEnemy(TestWorld, MyPawn, nullptr);
		ATraceCharacter* Control = FindLivingEnemy(TestWorld, MyPawn, Victim);
		if (Victim == nullptr || Control == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — needs TWO living enemies (one to make a carrier, one control). "
				     "Found victim=%s control=%s. Run this early, in a populated match."),
				Tag, *GetNameSafe(Victim), *GetNameSafe(Control));
			return;
		}

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.BlastChoke"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — Trace.Mortimer.BlastChoke is not registered."), Tag);
			return;
		}
		const int32 ArmBefore = Arm->GetInt();

		float RedCarrier = -1.f;
		float RedControl = -1.f;
		float GreenCarrier = -1.f;
		float GreenControl = -1.f;
		bool bRedVictimWasCarrier = false;
		bool bGreenVictimWasCarrier = false;

		for (int32 ArmIndex = 0; ArmIndex < 2; ++ArmIndex)
		{
			const bool bGreen = (ArmIndex == 1);
			Arm->Set(bGreen ? 1 : 0, ECVF_SetByConsole);

			// Hand the Core to the victim. TryPickup is the authority-side debug grant and it takes
			// the Core off whoever had it, so this is also what guarantees MORTIMER is not holding it
			// — which is the situation a real match cannot produce and the whole point of the test.
			CoreActor->TryPickup(Victim);

			const bool bIsCarrierNow = UTraceAbilityComponent::IsCarrier(Victim);

			// Opposite sides of him, so neither can be standing in the other's line.
			bool bCarrierApplied = false;
			bool bControlApplied = false;
			const float CarrierMoved = MeasureBlastOn(Mortimer, Victim,  FVector(150.f, 0.f, 0.f), bCarrierApplied);
			const float ControlMoved = MeasureBlastOn(Mortimer, Control, FVector(-150.f, 0.f, 0.f), bControlApplied);

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%s  victim=%s carrier=%d launched=%d at %.1f uu/s | control=%s launched=%d at %.1f uu/s"),
				Tag, bGreen ? TEXT("GREEN (shipped)") : TEXT("RED (choke removed)"),
				*GetNameSafe(Victim), bIsCarrierNow ? 1 : 0, bCarrierApplied ? 1 : 0, CarrierMoved,
				*GetNameSafe(Control), bControlApplied ? 1 : 0, ControlMoved);

			if (bGreen)
			{
				GreenCarrier = CarrierMoved;
				GreenControl = ControlMoved;
				bGreenVictimWasCarrier = bIsCarrierNow;
			}
			else
			{
				RedCarrier = CarrierMoved;
				RedControl = ControlMoved;
				bRedVictimWasCarrier = bIsCarrierNow;
			}
		}

		Arm->Set(ArmBefore, ECVF_SetByConsole);

		// ---- the verdict --------------------------------------------------------------------------
		//
		// THE FIXTURE HAS TO PROVE ITSELF FIRST. Three things must be true before "the carrier did not
		// move" is allowed to mean anything: the victim really was a carrier in BOTH arms, the control
		// really did fly in BOTH arms (so the blast works at all and the difference is the rule, not
		// the fixture), and the RED arm really did reach the carrier (so the rule is what stops it).
		const bool bControlWorked = RedControl > DisplacedThreshold && GreenControl > DisplacedThreshold;
		const bool bRedReproduced = RedCarrier > DisplacedThreshold;
		const bool bCarrierHeld   = bRedVictimWasCarrier && bGreenVictimWasCarrier;

		if (!bCarrierHeld || !bControlWorked || !bRedReproduced)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — victimWasCarrierBothArms=%d controlFlewBothArms=%d "
				     "redArmReachedTheCarrier=%d (red carrier %.1f, red control %.1f, green control %.1f uu/s). "
				     "A harness that cannot go red has proved nothing; this reports INVALID rather than PASS."),
				Tag, bCarrierHeld ? 1 : 0, bControlWorked ? 1 : 0, bRedReproduced ? 1 : 0,
				RedCarrier, RedControl, GreenControl);
			return;
		}

		if (GreenCarrier > DisplacedThreshold)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] VERDICT: *** FAIL *** — the shipped build DISPLACED a Core carrier by %.1f uu/s. "
				     "Quake is not routed through UTraceAbilityComponent::CanAffectTarget(Control). This is the "
				     "founding invariant."),
				Tag, GreenCarrier);
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] VERDICT: PASS — with the choke point REMOVED the carrier flew %.1f uu/s; with it in "
			     "place the identical call moved them %.1f uu/s. The control target flew %.1f then %.1f uu/s, "
			     "so the blast itself worked in both arms and the difference is the rule."),
			Tag, RedCarrier, GreenCarrier, RedControl, GreenControl);
	}

	FAutoConsoleCommand CmdBlastCarrierTest(
		TEXT("Trace.Mortimer.BlastCarrierTest"),
		TEXT("SPEC v19 §3 / v14 §4. Two arms, RED FIRST: hands the Core to an enemy and fires Quake's per-victim "
		     "path at them with the carrier choke point removed (must throw them) and then in place (must not). "
		     "A second, non-carrying enemy is blasted in both arms so the fixture proves itself."),
		FConsoleCommandDelegate::CreateStatic(&RunBlastCarrierTest));

	/**
	 * Trace.Mortimer.Verify — the parts of §3 that are answerable without a fixture.
	 *
	 * Deliberately SEPARATE from the choke test: this one is arithmetic and posture and can run in an
	 * empty match, while the choke test needs two live enemies and a Core. Running them together
	 * would mean the cheap check reported INVALID whenever the expensive one could not be staged.
	 */
	void RunMortimerVerify()
	{
		const TCHAR* const Tag = TEXT("MORTIMER");
		const UTraceSettings& Settings = UTraceSettings::Get();

		int32 Passed = 0;
		int32 Failed = 0;
		auto Check = [&](bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; }
			else { ++Failed; UE_LOG(LogTraceGame, Error, TEXT("[%s] FAIL: %s"), Tag, *What); }
		};

		// --- the passive, as numbers -------------------------------------------------------------
		Check(FMath::IsNearlyEqual(Settings.MortimerDashDistanceScale, 0.25f, 0.001f),
			FString::Printf(TEXT("§3 says the dash is 75%% shorter, i.e. a scale of 0.25; the knob is %.3f"),
				Settings.MortimerDashDistanceScale));

		Check(FMath::IsNearlyEqual(Settings.MortimerThrowChargeHoldScale, 2.f, 0.001f),
			FString::Printf(TEXT("§3 says he may charge for 2x as long; the knob is %.3f"),
				Settings.MortimerThrowChargeHoldScale));

		Check(FMath::IsNearlyEqual(Settings.MortimerMantleGenerosity, 1.3f, 0.001f),
			FString::Printf(TEXT("§3 says the mantle is 30%% more generous, i.e. 1.30; the knob is %.3f"),
				Settings.MortimerMantleGenerosity));

		// --- and the derived numbers a designer actually reads ------------------------------------
		const float DashReach = FMath::Max(0.f, Settings.DashSpeed) * FMath::Max(0.f, Settings.DashDuration);
		const float HisReach  = DashReach * Settings.MortimerDashDistanceScale;
		const float FullHold  = FMath::Max(0.01f, Settings.CoreThrowChargeSeconds);
		const float HisHold   = FullHold * Settings.MortimerThrowChargeHoldScale;
		const float Floor     = FMath::Clamp(Settings.CoreThrowChargeFloorFraction, 0.f, 1.f);
		const float HisPower  = Floor + (1.f - Floor) * Settings.MortimerThrowChargeHoldScale;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] DASH   everybody %.0f uu (%.0f uu/s x %.2fs) -> Mortimer %.0f uu."),
			Tag, DashReach, Settings.DashSpeed, Settings.DashDuration, HisReach);
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] THROW  everybody may hold %.2fs for x1.00 launch speed -> Mortimer may hold %.2fs for "
			     "x%.2f, ON THE SAME LINE (Power = %.2f + %.2f x t). NOTE: x%.2f SPEED is not x2 DISTANCE — "
			     "flat-ground range goes as speed squared, so this is nearer 3.4x the range. Flagged."),
			Tag, FullHold, HisHold, HisPower, Floor, 1.f - Floor, HisPower);
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] QUAKE  radius %.0f uu, %.0f uu/s at the centre falling to %.0f%% at the rim, up bias %.2f, "
			     "line of sight %s, cooldown %.0fs."),
			Tag, Settings.MortimerBlastRadiusUU, Settings.MortimerBlastKnockbackSpeed,
			Settings.bMortimerBlastFallsOff ? Settings.MortimerBlastMinFalloffScale * 100.f : 100.f,
			Settings.MortimerBlastUpBias, Settings.bMortimerBlastNeedsLineOfSight ? TEXT("REQUIRED") : TEXT("not required"),
			Settings.MortimerBlastCooldownSeconds);

		// --- THE HONEST PART. Three of his four abilities are not wired to anything yet. -----------
		UE_LOG(LogTraceGame, Warning,
			TEXT("[%s] NOT YET LIVE: the dash scale, the mantle and the Core-throw cap are exposed through "
			     "TraceAbilityTraits and are read by NOTHING today — they need one call each in "
			     "Movement/TraceCharacterMovementComponent.cpp (GetDashSpeed, GetMaxDashCharges' sibling "
			     "CanAttemptMantle) and Gameplay/TraceCore.cpp (GetThrowChargeScaleForHold). Until then "
			     "Mortimer dashes and throws exactly like everybody else and cannot mantle. QUAKE IS LIVE."),
			Tag);

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[%s] VERDICT: PASS — %d checks, 0 failed."), Tag, Passed);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d passed, %d FAILED."), Tag, Passed, Failed);
		}
	}

	FAutoConsoleCommand CmdMortimerVerify(
		TEXT("Trace.Mortimer.Verify"),
		TEXT("SPEC v19 §3. Checks Mortimer's knobs against the doc's numbers, prints the derived dash reach and "
		     "throw power, and says plainly which of his abilities are not yet wired to anything."),
		FConsoleCommandDelegate::CreateStatic(&RunMortimerVerify));
}

#undef LOCTEXT_NAMESPACE
