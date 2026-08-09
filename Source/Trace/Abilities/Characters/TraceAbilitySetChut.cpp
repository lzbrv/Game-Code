// Trace — CHUT. See the header for the spec v14 §6 reading and for why the bash is polled.

#include "Abilities/Characters/TraceAbilitySetChut.h"

#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceMelee.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE RED ARMS. One per ability, each removing that ability and nothing else, so Trace.Chut.Verify
// can be made to fail on an otherwise identical build. See the note in TraceAbilitySetRocco.cpp.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarChutKnifeBuffEnabled(
	TEXT("Trace.Chut.KnifeBuffEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = Chut's knife substitutes his own front/back damage (spec §6: 50 from the "
	     "front). 0 = he swings for the standard numbers, so the knife assertion must go red."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarChutBashEnabled(
	TEXT("Trace.Chut.BashEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = the end of Chut's dash bashes. 0 = it does nothing, so every bash "
	     "assertion must go red."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarChudEnabled(
	TEXT("Trace.Chut.ChudEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = Chud reduces incoming body-shot and melee damage. 0 = the window still "
	     "opens and still replicates, but reduces nothing, so the reduction assertion must go red."),
	ECVF_Cheat);

namespace
{
	/** "Bullet" — UTraceWeaponComponent's cause for a BODY shot. "Headshot" is its own cause. */
	FName BodyShotCause()
	{
		static const FName Cause(TEXT("Bullet"));
		return Cause;
	}

	/** "Trail" — UTraceTrailComponent's death cause. §6 [ASSUMPTION]: Chud does not touch these. */
	FName TrailDeathCause()
	{
		static const FName Cause(TEXT("Trail"));
		return Cause;
	}

	/** "Headshot" — the weapon's head-zone cause. §6 [ASSUMPTION]: Chud does not touch these either. */
	FName HeadshotCause()
	{
		static const FName Cause(TEXT("Headshot"));
		return Cause;
	}

	bool IsKnifeCause(const FName Cause)
	{
		return Cause == TraceMelee::GetKnifeKillCause() || Cause == TraceMelee::GetBackstabKillCause();
	}
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetChut::OnPawnSpawned()
{
	// A new pawn cannot be mid-dash. Chud is NOT cleared: it is on the match clock and spec §5 says
	// a player may spawn with an ability timer still running.
	ResetDashTracking();
}

void UTraceAbilitySetChut::OnPawnDied()
{
	ResetDashTracking();
}

void UTraceAbilitySetChut::OnHalfTime()
{
	// The framework has already zeroed the cooldown and Reset() the net state, which takes Chud's
	// window with it. Only the local dash bookkeeping is left.
	ResetDashTracking();
}

void UTraceAbilitySetChut::ResetDashTracking()
{
	bWasDashing = false;
	DashStartWorldTime = 0.f;
	PolledDashDirection = FVector::ZeroVector;
	LastPolledLocation = FVector::ZeroVector;
	bHasLastPolledLocation = false;
	BashedThisDash.Reset();
}

void UTraceAbilitySetChut::TickAbilities(float DeltaSeconds)
{
	if (!HasAuthority())
	{
		return;
	}

	// --- Chud's window closing --------------------------------------------------------------------
	// Edge-triggered so the flag and the replicated state agree with the clock; nothing else in this
	// class ever clears it, and half time goes through the framework's Reset().
	const FTraceAbilityNetState& Current = State();
	if ((Current.Flags & TraceAbilityFlags::EffectActive) != 0 && MatchTimeNow() >= Current.EffectEndMatchTime)
	{
		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::EffectActive);
		Writable.EffectEndMatchTime = 0.f;
		MarkStateDirty();

		UE_LOG(LogTraceGame, Verbose, TEXT("[Chut] Chud expired."));
	}

	PollDashForBash(DeltaSeconds);
}

// =================================================================================================
// ACTIVATED — Chud
// =================================================================================================

float UTraceAbilitySetChut::GetActivatedCooldownSeconds() const
{
	return UTraceSettings::Get().ChudCooldownSeconds;   // §6: 20 s.
}

bool UTraceAbilitySetChut::ActivateAbility()
{
	if (!HasAuthority())
	{
		// PREDICTED HALF: nothing to predict. Chud is a number the SERVER applies to incoming damage;
		// there is no local motion and no local hit to feel. Returning true greys the HUD button
		// immediately, which is the only client-visible half there is.
		return true;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags |= TraceAbilityFlags::EffectActive;

	// ASSIGNMENT, NOT ADDITION. §6: "Does not stack." Pressing E with Chud already up restarts the
	// 10 s rather than making it 20 — and the same line is what a knife kill re-runs to refresh it.
	Writable.EffectEndMatchTime = MatchTimeNow() + FMath::Max(0.f, Settings.ChudDurationSeconds);
	MarkStateDirty();

	UE_LOG(LogTraceGame, Log,
		TEXT("[Chut] CHUD up for %.1fs (−%.0f%% from body shots and melees; headshots and trace deaths "
		     "unaffected). Cooldown %.0fs."),
		Settings.ChudDurationSeconds, Settings.ChudDamageReduction * 100.f, Settings.ChudCooldownSeconds);

	return true;
}

bool UTraceAbilitySetChut::IsChudActive() const
{
	const FTraceAbilityNetState& Current = State();
	return (Current.Flags & TraceAbilityFlags::EffectActive) != 0
		&& MatchTimeNow() < Current.EffectEndMatchTime;
}

float UTraceAbilitySetChut::GetChudSecondsRemaining() const
{
	const FTraceAbilityNetState& Current = State();
	if ((Current.Flags & TraceAbilityFlags::EffectActive) == 0)
	{
		return 0.f;
	}
	return FMath::Max(0.f, Current.EffectEndMatchTime - MatchTimeNow());
}

// =================================================================================================
// PASSIVE — the knife, and Chud's reduction
// =================================================================================================

float UTraceAbilitySetChut::ModifyOutgoingDamage(float Damage, const FTraceAbilityDamageContext& Context) const
{
	if (CVarChutKnifeBuffEnabled.GetValueOnAnyThread() == 0)
	{
		return Damage;   // RED ARM
	}

	// THE CAUSE IS THE ZONE, and that is not a heuristic: TraceMelee resolves front from back at the
	// one place it can be known exactly and hands the health component two DIFFERENT causes for the
	// two zones ("Knife" and "Backstab"). Reading the cause is therefore reading the melee slice's
	// own answer, not re-deriving it from a damage number that a future retune would break.
	if (Context.Cause == TraceMelee::GetKnifeKillCause())
	{
		// §6: "knife deals 50 from the front (vs the standard 30)".
		return UTraceSettings::Get().ChutKnifeFrontDamage;
	}

	if (Context.Cause == TraceMelee::GetBackstabKillCause())
	{
		// §6 [ASSUMPTION]: "back damage stays 100". Written out rather than left to fall through, so
		// that the assumption is visible at the one place it is made and reversible from one knob.
		return UTraceSettings::Get().ChutKnifeBackDamage;
	}

	return Damage;
}

float UTraceAbilitySetChut::ModifyIncomingDamage(float Damage, const FTraceAbilityDamageContext& Context) const
{
	if (!IsChudActive() || CVarChudEnabled.GetValueOnAnyThread() == 0)
	{
		return Damage;
	}

	// §6 [ASSUMPTION], stated in the spec itself: "headshots and trace deaths are unaffected — it
	// names body shots and melees." Both exclusions are by CAUSE as well as by flag, because the
	// flag is filled in by whichever slice is calling and the cause is not.
	if (Context.bHeadshot || Context.Cause == HeadshotCause() || Context.Cause == TrailDeathCause())
	{
		return Damage;
	}

	const bool bBodyShot = (Context.Cause == BodyShotCause());
	const bool bMeleeHit = Context.bMelee || IsKnifeCause(Context.Cause);
	if (!bBodyShot && !bMeleeHit)
	{
		// Anything else — poison ticks, area damage, a future ability — is left alone. §6 names two
		// categories and this reduction covers exactly those two.
		return Damage;
	}

	const float Reduction = FMath::Clamp(UTraceSettings::Get().ChudDamageReduction, 0.f, 1.f);
	return Damage * (1.f - Reduction);
}

void UTraceAbilitySetChut::OnKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot)
{
	if (!HasAuthority() || !UTraceSettings::Get().bChudRefreshesOnKnifeKill)
	{
		return;
	}

	if (!IsKnifeCause(Cause))
	{
		return;
	}

	// REFRESH, NOT START. §6: "The timer refreshes on a knife kill" — a refresh is a thing you do to
	// a running timer. A knife kill with Chud down leaves it down and the cooldown untouched;
	// otherwise the ability would have a second, free activation path that skipped E entirely.
	if (!IsChudActive())
	{
		return;
	}

	FTraceAbilityNetState& Writable = MutableState();
	Writable.EffectEndMatchTime = MatchTimeNow() + FMath::Max(0.f, UTraceSettings::Get().ChudDurationSeconds);
	MarkStateDirty();

	UE_LOG(LogTraceGame, Verbose, TEXT("[Chut] knife kill on %s (%s) refreshed Chud to %.1fs."),
		*GetNameSafe(Victim), *Cause.ToString(), UTraceSettings::Get().ChudDurationSeconds);
}

// =================================================================================================
// MOVEMENT — the bash
// =================================================================================================

bool UTraceAbilitySetChut::OnDashStarted(const FVector& DashDirection)
{
	// A new dash is a new set of victims. Never cancel the dash: returning true here would break
	// "dashing through a trace still kills the carrier normally", which is the one thing §6 says
	// about Chut's dash that is NOT the bash.
	BashedThisDash.Reset();
	PolledDashDirection = DashDirection.GetSafeNormal();
	return false;
}

void UTraceAbilitySetChut::OnDashEnded(bool bReachedFullDistance)
{
	BashedThisDash.Reset();
}

void UTraceAbilitySetChut::OnDashHitCharacter(ATraceCharacter* Other, float DashProgress)
{
	// The framework's seam. Nothing calls it yet (see the header); it funnels into the same apply
	// path the poll uses, and BashedThisDash makes the two idempotent with respect to each other.
	const UTraceCharacterMovementComponent* Move = GetMovement();
	const FVector Direction = (Move != nullptr) ? Move->GetDashDirection() : PolledDashDirection;
	TryBash(Other, DashProgress, Direction);
}

float UTraceAbilitySetChut::GetDashHitSweepRadius() const
{
	// The ONE knob, read from the ONE place. TryBash re-tests the gap against this same value from
	// the pawn's location at the instant of the hit, so a sweep that is slightly generous cannot
	// widen the bash — it can only offer TryBash a candidate it then refuses.
	return FMath::Max(1.f, UTraceSettings::Get().ChutBashRadiusUU);
}

bool UTraceAbilitySetChut::HasBashed(const ATraceCharacter* Victim) const
{
	for (const TWeakObjectPtr<ATraceCharacter>& Past : BashedThisDash)
	{
		if (Past.Get() == Victim)
		{
			return true;
		}
	}
	return false;
}

bool UTraceAbilitySetChut::TryBash(ATraceCharacter* Victim, float DashProgress, const FVector& DashDirection,
                                   float OverrideGapUU)
{
	if (!HasAuthority() || CVarChutBashEnabled.GetValueOnAnyThread() == 0)
	{
		return false;   // RED ARM, and authority: a knockback is server truth.
	}

	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || Victim == nullptr || Victim == MyPawn)
	{
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// --- "the END of his standard dash" -----------------------------------------------------------
	// §6 is specific about the end rather than the whole dash, so an early contact is REFUSED rather
	// than clamped into the window. ChutBashEndFraction is the size of that end.
	const float WindowStart = 1.f - FMath::Clamp(Settings.ChutBashEndFraction, 0.05f, 1.f);
	if (DashProgress < WindowStart)
	{
		return false;
	}

	// --- reach ------------------------------------------------------------------------------------
	const float Reach = FMath::Max(1.f, Settings.ChutBashRadiusUU);
	const float Gap = (OverrideGapUU >= 0.f)
		? OverrideGapUU
		: FVector::Dist(MyPawn->GetActorLocation(), Victim->GetActorLocation());
	if (Gap > Reach)
	{
		return false;
	}

	if (HasBashed(Victim))
	{
		return false;   // one bash per victim per dash
	}

	// *** THE CHOKE POINT. SPEC §4. ***
	// Control, because a knockback is movement the target did not ask for. This single call is what
	// makes §6's "NO EFFECT ON THE CORE CARRIER" true — there is no carrier test in this file, and
	// there must not be one, because a second copy of the rule is a second thing that can rot.
	if (!CanAffect(Victim, ETraceAbilityEffect::Control))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Chut] bash on %s refused by the choke point (carrier=%d)."),
			*GetNameSafe(Victim), UTraceAbilityComponent::IsCarrier(Victim) ? 1 : 0);
		return false;
	}

	// --- the knock --------------------------------------------------------------------------------
	FVector Direction = DashDirection.GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		// Degenerate input: fall back to the line between the two pawns, which is the only other
		// direction that means anything here. Never bash in an arbitrary direction.
		Direction = (Victim->GetActorLocation() - MyPawn->GetActorLocation()).GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			return false;
		}
	}

	const float Speed = FMath::Max(0.f, Settings.ChutBashKnockbackSpeed);
	const FVector Impulse = Direction * Speed + FVector::UpVector * (Speed * FMath::Max(0.f, Settings.ChutBashUpBias));

	// bXYOverride / bZOverride both true: the bash REPLACES the victim's velocity rather than adding
	// to it, so a player already sprinting away is knocked the same distance as one standing still.
	Victim->LaunchCharacter(Impulse, true, true);

	BashedThisDash.Add(Victim);

	UE_LOG(LogTraceGame, Log,
		TEXT("[Chut] BASH on %s at dash progress %.2f (window >= %.2f), gap %.0f of %.0f uu: %.0f uu/s along (%s) "
		     "+ %.0f up."),
		*GetNameSafe(Victim), DashProgress, WindowStart, Gap, Reach, Speed, *Direction.ToCompactString(),
		Speed * Settings.ChutBashUpBias);

	return true;
}

void UTraceAbilitySetChut::PollDashForBash(float DeltaSeconds)
{
	UWorld* WorldPtr = GetWorld();
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* Move = GetMovement();

	if (WorldPtr == nullptr || MyPawn == nullptr || Move == nullptr)
	{
		bWasDashing = false;
		return;
	}

	const bool bDashing = Move->IsDashing();

	if (bDashing && !bWasDashing)
	{
		// Dash edge. The movement component owns the clock; all this needs is when it started, so
		// that "the end of the dash" can be expressed as a fraction the way §6 does.
		DashStartWorldTime = WorldPtr->GetTimeSeconds();
		BashedThisDash.Reset();
		LastPolledLocation = MyPawn->GetActorLocation();
		bHasLastPolledLocation = true;
	}
	bWasDashing = bDashing;

	if (!bDashing)
	{
		bHasLastPolledLocation = false;
		return;
	}

	PolledDashDirection = Move->GetDashDirection();

	const float Duration = FMath::Max(KINDA_SMALL_NUMBER, UTraceSettings::Get().DashDuration);
	const float Progress = FMath::Clamp((WorldPtr->GetTimeSeconds() - DashStartWorldTime) / Duration, 0.f, 1.f);

	const FVector MyLocation = MyPawn->GetActorLocation();
	const FVector SweepFrom = bHasLastPolledLocation ? LastPolledLocation : MyLocation;
	LastPolledLocation = MyLocation;
	bHasLastPolledLocation = true;

	const float WindowStart = 1.f - FMath::Clamp(UTraceSettings::Get().ChutBashEndFraction, 0.05f, 1.f);
	if (Progress < WindowStart)
	{
		return;
	}

	const float Reach = FMath::Max(1.f, UTraceSettings::Get().ChutBashRadiusUU);

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || Candidate == MyPawn)
		{
			continue;
		}

		// THE SWEPT SEGMENT, not a point. See the note on LastPolledLocation: at 20 Hz a dash moves
		// further between samples than the whole reach, so a point test would step over people.
		const FVector CandidateLocation = Candidate->GetActorLocation();
		const float GapToSweep = FMath::PointDistToSegment(CandidateLocation, SweepFrom, MyLocation);
		if (GapToSweep > Reach)
		{
			continue;
		}

		// TryBash re-checks the reach from Chut's CURRENT location, which would reject exactly the
		// victims this sweep exists to catch, so the swept hit is handed to it as a contact at the
		// nearest point rather than as a bare candidate. Everything that decides whether the bash is
		// ALLOWED — the window and the §4 choke point — still lives in TryBash and is not duplicated.
		TryBash(Candidate, Progress, PolledDashDirection, GapToSweep);
	}
}
