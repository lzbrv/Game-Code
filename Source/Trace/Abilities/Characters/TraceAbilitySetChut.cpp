// Trace — CHUT. See the header for the spec v14 §6 reading and for why the bash is polled.

#include "Abilities/Characters/TraceAbilitySetChut.h"

#include "Containers/Ticker.h"                             // FTSTicker — the v24 §10 harness's clock
#include "Engine/Engine.h"                                 // GEngine->GetWorldContexts()
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/GameStateBase.h"                   // PlayerArray
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerState.h"                         // the team test NotifyCharacterDied uses
#include "Gameplay/TraceCore.h"                            // the harness must not let Chut hold it
#include "Gameplay/TraceHealthComponent.h"                 // the v24 §10 harness kills through it
#include "Gameplay/TraceMelee.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"                                    // ETraceTeam, TraceOpposingTeam

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

/**
 * THE RED ARM FOR SPEC v24 §10, and it is a SEPARATE arm from CVarChudEnabled on purpose.
 *
 * With Trace.Chut.ChudEnabled at 0 a marked-up Chut takes full damage whether or not he knifes
 * anybody, which says nothing about the refresh. With THIS arm at 0 Chud still opens, still reduces,
 * still replicates and still expires — the ONLY thing that changes is that a knife kill stops
 * pushing the remaining time back up. That is the single-variable A/B on the sentence §10 added, and
 * it is what Trace.Chut.ChudRefreshTest flips between its two arms.
 */
static TAutoConsoleVariable<int32> CVarChudKillRefresh(
	TEXT("Trace.Chut.ChudKillRefresh"),
	1,
	TEXT("Dev/red arm, spec v24 §10. 1 (default) = a knife kill pushes a RUNNING Chud's remaining time back "
	     "up to ChudDurationSeconds. 0 = the RED arm — Chud still opens, still reduces damage and still "
	     "expires on its own clock, but a knife kill does nothing to it, so every refresh assertion in "
	     "Trace.Chut.ChudRefreshTest must go red."),
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

// =================================================================================================
// SPEC v24 §10 — "CHUT'S CHUD DOESN'T RESET WHEN HE GETS A KNIFE KILL"
//
// Verbatim: "Chut's 'Chud' ability doesn't reset when he gets a knife kill. Rather than the cooldown
// resetting, I want the ability to go back to the max timer on every kill. E.g. if chut has 5seconds
// left on his ability and he gets a kill, the timer goes back up."
//
// ---- (b) WHAT IT MUST DO ------------------------------------------------------------------------
// Push the RUNNING effect's remaining duration back up to ChudDurationSeconds. Not the cooldown —
// the cooldown is what E costs and it keeps running, which is the half the owner explicitly asked
// for by naming it. The assignment below is that, and it is an assignment rather than an addition so
// that "does not stack" survives: five kills in ten seconds give ten seconds, not fifty.
//
// ---- (a) WHY IT LOOKED LIKE NOTHING HAPPENED ----------------------------------------------------
// *** IT WAS INVISIBLE, AND IT HAD NEVER BEEN TESTED THROUGH THE GAME. ***
//
// STATED PLAINLY SO NOBODY RE-CHASES IT: the mechanic MEASURES CORRECT on this build. Chud goes
// 7.00s -> 10.00s on a knife kill delivered through UTraceHealthComponent::ApplyDamage, and
// 6.80s -> 9.23s (against a floor of 8.75s, the max less the wind-up) on a REAL back-stab through
// UTraceWeaponComponent::ServerSwing, with the red arm measuring 5.90s on the identical fixture. So
// "it doesn't work at all" is NOT reproducible as a broken rule. What IS true, and is what the
// report says, is two things that make it look broken and that this pass fixes:
//
//   1. NOTHING IN THE PROJECT EVER DROVE THIS PATH. The only existing test of the knife refresh
//      (Trace.Chut.Verify, in TraceCharacterVerify.cpp) calls UTraceAbilitySetChut::OnKill
//      DIRECTLY. That proves the body of this function and proves NOTHING about the six links
//      between a knife and it: ServerSwing -> ApplyDamage -> BroadcastDeath -> HandleDeath ->
//      ATraceGameMode::NotifyCharacterDied -> UTraceAbilityComponent::NotifyKill. Worse, its one
//      assertion is `AfterRefresh >= BeforeRefresh - 0.05`, which a completely dead refresh
//      SATISFIES — the remaining time barely moves inside one frame. It is a harness that cannot
//      fail, which this project has shipped before and has a house rule about.
//
//   2. THE REFRESH ANNOUNCED ITSELF AT Verbose. The old line here was UE_LOG(..., Verbose, ...),
//      which is off in every ordinary run, and the HUD chip's "10.0s" label reads the same whether
//      the number was pushed back up or is simply what a fresh press looks like. So a player with no
//      way to see the number jump reasonably reports "it doesn't reset". It logs at Display now.
//
// Trace.Chut.ChudRefreshTest below is the red-armed proof over the SHIPPING funnel, and
// Trace.Chut.ChudKillRefresh 0 is the arm that makes every one of its refresh assertions fail.
// =================================================================================================

void UTraceAbilitySetChut::OnKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot)
{
	if (!HasAuthority() || !UTraceSettings::Get().bChudRefreshesOnKnifeKill)
	{
		return;
	}

	// THE RED ARM, and it sits here rather than around the write below so that the arm removes the
	// whole rule (including its log line) instead of leaving a half of it running.
	if (CVarChudKillRefresh.GetValueOnAnyThread() == 0)
	{
		return;
	}

	if (!IsKnifeCause(Cause))
	{
		return;
	}

	// REFRESH, NOT START. §6, and spec v24 §10 restates it: "the ability goes back to the max timer".
	// A refresh is a thing you do to a RUNNING timer. A knife kill with Chud down leaves it down and
	// the cooldown untouched; otherwise the ability would have a second, free activation path that
	// skipped E entirely — which is the case the owner asked to be checked by name.
	if (!IsChudActive())
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Chut] knife kill on %s (%s) with Chud DOWN — nothing started, cooldown untouched (%.2fs "
			     "still owed). Spec v24 §10: a refresh needs something to refresh."),
			*GetNameSafe(Victim), *Cause.ToString(),
			(GetAbilityComponent() != nullptr) ? GetAbilityComponent()->GetActivatedCooldownRemaining() : 0.f);
		return;
	}

	FTraceAbilityNetState& Writable = MutableState();

	// Read BEFORE the write, purely so the log can say what the owner asked to see ("5 seconds left
	// and he gets a kill, the timer goes back up") in the units he asked for.
	const float RemainingBefore = FMath::Max(0.f, Writable.EffectEndMatchTime - MatchTimeNow());
	const float FullDuration = FMath::Max(0.f, UTraceSettings::Get().ChudDurationSeconds);

	// *** BACK TO MAXIMUM, AND THE COOLDOWN IS NOT TOUCHED. *** EffectEndMatchTime is the effect's
	// deadline; ActivatedCooldownEndMatchTime, which lives on the component and is not written here
	// or anywhere else in this function, is E's. Spec v24 §10 asks for the first and explicitly not
	// the second.
	Writable.EffectEndMatchTime = MatchTimeNow() + FullDuration;
	MarkStateDirty();

	// Display, not Verbose, AND THAT IS THE MOST LIKELY HALF OF THE FIX. The mechanic measures correct
	// on this build (Trace.Chut.ChudRefreshTest, both through ApplyDamage and through a real
	// ServerSwing back-stab), so "it doesn't reset" is a report about what a player can SEE: the old
	// line here was Verbose, which is off in every ordinary run, and the HUD chip reads "10.0s"
	// identically whether the number was just pushed back up or the ability was just pressed. One
	// Display line per knife kill, only when Chud is actually up, is cheap and settles the question.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Chut] spec v24 §10: knife kill on %s (%s) — Chud %.2fs -> %.2fs (back to max). E cooldown "
		     "left alone: %.2fs still owed."),
		*GetNameSafe(Victim), *Cause.ToString(), RemainingBefore, FullDuration,
		(GetAbilityComponent() != nullptr) ? GetAbilityComponent()->GetActivatedCooldownRemaining() : 0.f);
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

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Chut.ChudRefreshTest — SPEC v24 §10, RED-ARMED, OVER THE SHIPPING KILL FUNNEL
//
// WHAT MAKES THIS DIFFERENT FROM THE TEST THAT ALREADY EXISTED. Trace.Chut.Verify calls
// UTraceAbilitySetChut::OnKill() directly and asserts `after >= before - 0.05`, which a completely
// dead refresh passes — the remaining time barely moves inside one frame. This one:
//
//   * kills through UTraceHealthComponent::ApplyDamage with TraceMelee::GetKnifeKillCause(), so the
//     whole chain runs (ApplyDamage -> BroadcastDeath -> ATraceCharacter::HandleDeath ->
//     ATraceGameMode::NotifyCharacterDied -> UTraceAbilityComponent::NotifyKill -> OnKill). If any
//     link is missing, this reports it; the direct call could not have.
//   * BURNS REAL TIME OFF THE WINDOW FIRST (ChudBurnSeconds), so "back to max" is a number that has
//     somewhere to travel to. The owner's own example is "5 seconds left".
//   * asserts the E COOLDOWN DID NOT MOVE, which is the half of §10 that says "rather than the
//     cooldown resetting".
//   * asserts a kill with Chud DOWN does not silently start it, which the owner asked for by name.
//   * FINISHES WITH A REAL BLADE. Steps 3 and 4 stage a back-stab and call TraceMelee::RequestSwing,
//     so UTraceWeaponComponent::ServerSwing — the one link the ApplyDamage shortcut cannot reach,
//     and the link the owner's sentence is literally about — is exercised too.
//   * refuses to score itself if the fixture could not produce a real kill notification — a
//     self-kill or a team kill is skipped by NotifyCharacterDied by design, so the harness puts the
//     victim on the opposing team and says so rather than reporting a false failure.
//
// ARMS. Arm 0 is RED (Trace.Chut.ChudKillRefresh 0): everything else is identical and the refresh
// assertions must FAIL. Arm 1 is the shipped build. A run whose red arm produces zero failures is
// reported INVALID, because then the green arm proves nothing.
//
// The namespace is named after this file (MSVC C2084 in unity builds — see the house rules).
// =================================================================================================
namespace TraceAbilitySetChutFile
{
	/** How long to let the window drain before the kill. Long enough that "back up" is unambiguous. */
	constexpr float ChudBurnSeconds = 3.0f;

	/** Comfortably lethal, and the health component clamps at zero, so the exact size does not matter. */
	constexpr float ChudTestLethalDamage = 100000.f;

	struct FChudCheck
	{
		bool bPassed = false;
		FString Label;
		FString Detail;
	};

	struct FChudArmLog
	{
		int32 Arm = 1;
		bool bInvalid = false;
		FString InvalidReason;
		TArray<FChudCheck> Checks;

		void Add(const FString& Label, bool bPassed, const FString& Detail)
		{
			Checks.Add({ bPassed, Label, Detail });
		}

		void Invalidate(const FString& Reason)
		{
			bInvalid = true;
			InvalidReason = Reason;
		}

		int32 CountFailed() const
		{
			int32 Failed = 0;
			for (const FChudCheck& Check : Checks)
			{
				Failed += Check.bPassed ? 0 : 1;
			}
			return Failed;
		}
	};

	struct FChudRun
	{
		TArray<int32> ArmsToRun = { 0, 1 };
		int32 ArmIndex = 0;
		int32 Step = 0;
		double NextRealTime = 0.0;

		TArray<FChudArmLog> Results;
		FChudArmLog Current;

		TWeakObjectPtr<UTraceAbilityComponent> Comp;
		TWeakObjectPtr<UTraceAbilitySetChut> Chut;
		TWeakObjectPtr<ATraceCharacter> ChutPawn;
		TWeakObjectPtr<ATraceCharacter> Victim;

		float RemainingBeforeKill = 0.f;
		float CooldownBeforeKill = 0.f;

		/**
		 * When the real swing was PRESSED, in real seconds.
		 *
		 * Needed because the blade resolves SwingWindupSeconds later and the harness reads the result
		 * later still, so a fresh 10 s window has already been draining for the best part of two
		 * seconds by the time it is measured. The first run of the real-swing step reported the
		 * shipped arm FAILING with "6.80s -> 9.20s (max 10.0s)": the refresh had worked perfectly and
		 * the assertion was simply asking for a number that no correct build could produce. Expecting
		 * `max - elapsed` instead of `max` is the fix, and it costs nothing in separation — the red
		 * arm measures 5.90s on the same fixture.
		 */
		double SwingPressRealTime = 0.0;

		/** -TraceExec fires long before a pawn exists; the same budget Trace.Chut.Verify uses. */
		int32 SetupAttemptsLeft = 40;

		/**
		 * RESTARTS OF THE CURRENT ARM, and this is not defensive padding — it is the fix for a
		 * measured false failure.
		 *
		 * The first run of this harness reported the SHIPPED arm failing with "0.00s left of 10.0s
		 * after 3.0s of real time". Chud had not stopped working: the harness teleports Chut behind a
		 * bot to stage the back-stab, the bot's friends shot him, and spec v19 §4.2's DEATH WIPE
		 * correctly stopped every effect he had running. The fixture had collapsed and the harness
		 * measured the wreckage. A run that cannot tell "the rule broke" from "my subject died" is
		 * exactly the kind of confident nonsense this project has a house rule about, so a collapsed
		 * fixture now RESTARTS the arm and only gives up (INVALID, never FAIL) when it runs out.
		 */
		int32 ArmRestartsLeft = 8;
	};

	UWorld* FindAuthoritativeGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetNetMode() != NM_Client)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** The first non-bot player's component: a bot's character is re-picked by the game mode's fill. */
	UTraceAbilityComponent* FindHumanComponent(UWorld* WorldPtr)
	{
		const AGameStateBase* GS = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (GS == nullptr)
		{
			return nullptr;
		}
		for (APlayerState* PS : GS->PlayerArray)
		{
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PS);
			if (Comp != nullptr && !Comp->IsBot())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/**
	 * A living pawn that is not @p Exclude, is not the Core carrier, and has a health component.
	 *
	 * The carrier is excluded because ApplyDamage refuses an invulnerable target outright — a fixture
	 * that picked one would report "no kill happened" for a reason that has nothing to do with §10.
	 */
	ATraceCharacter* FindKillableVictim(UWorld* WorldPtr, const ATraceCharacter* Exclude)
	{
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Exclude || !Candidate->IsAlive())
			{
				continue;
			}
			const UTraceHealthComponent* CandidateHealth = Candidate->FindComponentByClass<UTraceHealthComponent>();
			if (CandidateHealth == nullptr || CandidateHealth->IsInvulnerable())
			{
				// No health component, or the Core carrier — ApplyDamage drops an invulnerable target's
				// hit outright, so no kill would land and the harness would blame §10 for the fixture.
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	/**
	 * Put the victim on the OPPOSING team, and say so.
	 *
	 * ATraceGameMode::NotifyCharacterDied does not tell the killer's abilities about a self-kill or a
	 * team kill — deliberately, and the scoreboard uses the same test. A harness that ignored that
	 * would report "the refresh never fired" for a kill the game was right not to credit.
	 */
	bool ForceOpposingTeams(ATraceCharacter* Killer, ATraceCharacter* VictimPawn, FString& OutDescription)
	{
		ATracePlayerState* KillerState = (Killer != nullptr) ? Killer->GetPlayerState<ATracePlayerState>() : nullptr;
		ATracePlayerState* VictimState = (VictimPawn != nullptr) ? VictimPawn->GetPlayerState<ATracePlayerState>() : nullptr;
		if (KillerState == nullptr || VictimState == nullptr)
		{
			OutDescription = TEXT("one of the two pawns has no ATracePlayerState");
			return false;
		}

		// Recorded BEFORE anything is forced, and printed either way. "Did the harness have to move
		// somebody?" is itself a finding: if the match already had them opposed then the team gate is
		// NOT what was stopping the refresh in ordinary play, and the report must be able to say so.
		const ETraceTeam KillerTeamBefore = KillerState->Team;
		const ETraceTeam VictimTeamBefore = VictimState->Team;

		if (KillerState->Team == ETraceTeam::None)
		{
			KillerState->SetTeam(ETraceTeam::Blue);
		}
		const ETraceTeam Opposing = TraceOpposingTeam(KillerState->Team);
		if (VictimState->Team != Opposing)
		{
			VictimState->SetTeam(Opposing);
		}

		const bool bAlreadyOpposed = (KillerTeamBefore != ETraceTeam::None)
			&& (VictimTeamBefore != ETraceTeam::None)
			&& (KillerTeamBefore != VictimTeamBefore);

		OutDescription = FString::Printf(TEXT("teams killer %d / victim %d (the match had %d / %d, %s)"),
			static_cast<int32>(KillerState->Team), static_cast<int32>(VictimState->Team),
			static_cast<int32>(KillerTeamBefore), static_cast<int32>(VictimTeamBefore),
			bAlreadyOpposed ? TEXT("ALREADY OPPOSED — the team gate was not blocking anything")
			                : TEXT("NOT opposed — the harness moved somebody"));
		return KillerState != VictimState && KillerState->Team != VictimState->Team;
	}

	/**
	 * Stand Chut on the victim's forward axis at 60% of the blade's reach, facing them.
	 *
	 * Copied in shape from TraceMelee's own Trace.Knife.CarrierImmunityTest staging, and for its
	 * reasons: 60% is comfortably inside the reach and far enough out that the two capsules are not
	 * interpenetrating (which makes the approach vector degenerate and resolves to FRONT). Behind
	 * them means a BACK-STAB, i.e. a one-shot kill on a full-health pawn — so the real-swing step
	 * needs exactly one blade and no softening-up damage from a second, non-knife source that could
	 * be blamed for the kill.
	 *
	 * The CONTROL rotation is set as well as the actor's: the swing origin and arc come from where
	 * the pawn is LOOKING.
	 */
	void PlaceChutBehind(ATraceCharacter* Attacker, const ATraceCharacter* VictimPawn)
	{
		if (Attacker == nullptr || VictimPawn == nullptr)
		{
			return;
		}

		const FRotator VictimRot(0.f, VictimPawn->GetActorRotation().Yaw, 0.f);
		const double Distance = static_cast<double>(TraceMelee::GetSwingRangeUU()) * 0.6;
		const FVector Where = VictimPawn->GetActorLocation() - VictimRot.Vector() * Distance;

		Attacker->SetActorLocationAndRotation(Where, VictimRot, false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* AttackerController = Attacker->GetController())
		{
			AttackerController->SetControlRotation(VictimRot);
		}
	}

	/**
	 * HOLD THE STAGED BACK-STAB TOGETHER FOR THE WHOLE FLIGHT OF THE BLADE.
	 *
	 * MEASURED, NOT ANTICIPATED. Without this the shipped arm restarted four times in a row with
	 * "the staged back-stab did not kill TraceCharacter_N (it moved, or the blade missed)": the
	 * victim is a bot with somewhere to be, the blade resolves SwingWindupSeconds AFTER the press,
	 * and a bot walks further than 60% of the knife's reach in that gap. The staging was correct at
	 * the press and wrong at the resolve, which is the same trap Trace.Knife.CarrierImmunityTest
	 * documents having fallen into — and its answer is the same one: re-apply the scenario every
	 * tick rather than once.
	 *
	 * Its own short-lived ticker rather than the run's step clock, because the step clock is what is
	 * WAITING for the wind-up; this has to run inside that wait, several times.
	 */
	void HoldBackstabStaging(TWeakObjectPtr<ATraceCharacter> Attacker, TWeakObjectPtr<ATraceCharacter> VictimPawn,
	                         double UntilRealTime)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Attacker, VictimPawn, UntilRealTime](float) -> bool
			{
				ATraceCharacter* Att = Attacker.Get();
				ATraceCharacter* Vic = VictimPawn.Get();
				if (Att == nullptr || Vic == nullptr || !Vic->IsAlive() || !Att->IsAlive())
				{
					return false;   // the blade landed, or somebody left. Either way, stop.
				}

				// Stop the victim WALKING as well as re-placing the attacker: a bot that is still
				// accelerating will be somewhere else again by the next tick.
				if (UTraceCharacterMovementComponent* VictimMove = Vic->GetTraceMovement())
				{
					VictimMove->Velocity = FVector::ZeroVector;
				}
				PlaceChutBehind(Att, Vic);

				return FPlatformTime::Seconds() < UntilRealTime;
			}), 0.f);
	}

	void ApplyChudRefreshArm(int32 Arm)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Chut.ChudKillRefresh")))
		{
			Var->Set((Arm == 0) ? 0 : 1, ECVF_SetByConsole);
		}
	}

	bool TickChudRefreshRun(TSharedPtr<FChudRun> Run);
	void ScheduleChudRefresh(TSharedPtr<FChudRun> Run, float DelaySeconds);
	void FinishChudArm(TSharedPtr<FChudRun> Run);

	/**
	 * THE FIXTURE'S OWN HEALTH CHECK, run at the top of every step.
	 *
	 * Re-fetches the pawn from the COMPONENT rather than trusting a cached handle: the component
	 * lives on the PlayerState and survives death, the pawn does not, and a Chut who died and
	 * respawned is a different ATraceCharacter with the same ability set. Also tops his health up,
	 * because the only reason he keeps dying is that the harness teleports him into a firefight.
	 *
	 * @return false when the subject is simply not there yet or the effect it needs has been wiped;
	 *         the caller restarts the arm.
	 */
	bool ReacquireSubject(TSharedPtr<FChudRun> Run, UWorld* WorldPtr, bool bRequireChudActive)
	{
		UTraceAbilityComponent* Comp = Run->Comp.Get();
		if (Comp == nullptr)
		{
			Comp = FindHumanComponent(WorldPtr);
			Run->Comp = Comp;
		}
		if (Comp == nullptr)
		{
			return false;
		}

		ATraceCharacter* ChutPawn = Comp->GetOwningCharacter();
		UTraceAbilitySetChut* Chut = Comp->GetAbilitySetAs<UTraceAbilitySetChut>();
		if (ChutPawn == nullptr || Chut == nullptr || !ChutPawn->IsAlive())
		{
			return false;
		}

		Run->ChutPawn = ChutPawn;
		Run->Chut = Chut;

		// Keep him standing. ResetHealth also clears X's vulnerable mark, which is irrelevant here and
		// cannot touch Chud — Chud lives in the ability component's replicated state, not on health.
		if (UTraceHealthComponent* MyHealth = ChutPawn->Health.Get())
		{
			MyHealth->ResetHealth();
		}

		// *** AND HE MUST NOT BE HOLDING THE CORE. *** Measured, not anticipated: a run reported the
		// shipped arm's real swing as "swing=0 refusal=carrying the Core". The carrier cannot swing —
		// that is the game's rule and it is correct — but it is a fixture failure here, not a §10
		// failure, and it will keep happening because the harness teleports Chut into the middle of
		// the pitch where the Core is. ATraceCore::GrantTo is the project's single possession funnel
		// and takes a Debug reason for exactly this.
		if (ChutPawn->IsCarrier())
		{
			if (ATraceCore* TheCore = ATraceCore::Get(WorldPtr))
			{
				if (ATraceCharacter* Somebody = FindKillableVictim(WorldPtr, ChutPawn))
				{
					TheCore->GrantTo(Somebody, ETraceCoreGrantReason::Debug);
				}
			}
			if (ChutPawn->IsCarrier())
			{
				return false;   // still holding it; the caller restarts the arm
			}
		}

		return !bRequireChudActive || Chut->IsChudActive();
	}

	/** Throw the current arm's half-finished checks away and start it again, or give up honestly. */
	bool RestartOrGiveUp(TSharedPtr<FChudRun> Run, const FString& Why)
	{
		if (Run->ArmRestartsLeft-- > 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CHUDREFRESH] arm %d RESTARTING (%d left): %s"),
				Run->ArmsToRun.IsValidIndex(Run->ArmIndex) ? Run->ArmsToRun[Run->ArmIndex] : -1,
				Run->ArmRestartsLeft, *Why);
			Run->Current = FChudArmLog();
			Run->Step = 0;
			ScheduleChudRefresh(Run, 2.0f);   // give him time to respawn and the bots time to wander off
			return false;
		}

		Run->Current.Invalidate(FString::Printf(
			TEXT("the fixture kept collapsing and the restarts ran out: %s"), *Why));
		FinishChudArm(Run);
		ScheduleChudRefresh(Run, 1.0f);
		return false;
	}

	void ScheduleChudRefresh(TSharedPtr<FChudRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + DelaySeconds;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickChudRefreshRun(Run);
			}), 0.f);
	}

	void FinishChudArm(TSharedPtr<FChudRun> Run)
	{
		Run->Current.Arm = Run->ArmsToRun[Run->ArmIndex];
		Run->Results.Add(Run->Current);
		Run->Current = FChudArmLog();
		++Run->ArmIndex;
		Run->Step = 0;
		Run->ArmRestartsLeft = 8;      // each arm gets its own budget
		Run->SetupAttemptsLeft = 40;
	}

	void ReportChudRefreshVerdict(TSharedPtr<FChudRun> Run)
	{
		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[CHUDREFRESH] ===== spec v24 §10: 'rather than the cooldown resetting, I want the ability to go "
			     "back to the max timer on every kill' — Chud %.1fs / %.0fs cooldown, refresh knob %d ====="),
			Settings.ChudDurationSeconds, Settings.ChudCooldownSeconds,
			Settings.bChudRefreshesOnKnifeKill ? 1 : 0);

		const FChudArmLog* Red = nullptr;
		const FChudArmLog* Green = nullptr;
		for (const FChudArmLog& Log : Run->Results)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[CHUDREFRESH] --- arm %d (%s) ---"),
				Log.Arm, (Log.Arm == 0) ? TEXT("RED: Trace.Chut.ChudKillRefresh 0") : TEXT("shipped"));
			if (Log.bInvalid)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[CHUDREFRESH]   INVALID: %s"), *Log.InvalidReason);
			}
			for (const FChudCheck& Check : Log.Checks)
			{
				// Two call sites rather than a computed verbosity: UE_LOG's verbosity has to be a
				// literal, and a failed assertion has to reach a log that is on by default.
				if (Check.bPassed)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[CHUDREFRESH]   [PASS] %s — %s"),
						*Check.Label, *Check.Detail);
				}
				else
				{
					UE_LOG(LogTraceGame, Error, TEXT("[CHUDREFRESH]   [FAIL] %s — %s"),
						*Check.Label, *Check.Detail);
				}
			}
			if (Log.Arm == 0) { Red = &Log; }
			if (Log.Arm == 1) { Green = &Log; }
		}

		if (Red == nullptr || Green == nullptr || Red->bInvalid || Green->bInvalid)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CHUDREFRESH] VERDICT: INVALID — an arm did not run or could not build its fixture."));
			return;
		}

		const int32 RedFailures = Red->CountFailed();
		const int32 GreenFailures = Green->CountFailed();

		if (RedFailures == 0)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CHUDREFRESH] VERDICT: INVALID — the RED arm did not reproduce (0 failures with the refresh "
				     "removed). This harness cannot fail, so arm 1's green means nothing."));
			return;
		}

		if (GreenFailures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[CHUDREFRESH] VERDICT: PASS — the red arm reproduced %d failure(s); the shipped arm passed "
				     "all %d, through the real kill funnel."),
				RedFailures, Green->Checks.Num());
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CHUDREFRESH] VERDICT: *** FAIL *** — %d of %d assertions failed on the shipped arm (the red "
				     "arm reproduced %d, so the fixture is sound)."),
				GreenFailures, Green->Checks.Num(), RedFailures);
		}
	}

	bool TickChudRefreshRun(TSharedPtr<FChudRun> Run)
	{
		UWorld* WorldPtr = FindAuthoritativeGameWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[CHUDREFRESH] no authoritative world — server only."));
			return false;
		}

		if (!Run->ArmsToRun.IsValidIndex(Run->ArmIndex))
		{
			ApplyChudRefreshArm(1);   // leave the process on the shipped arm
			ReportChudRefreshVerdict(Run);
			return false;
		}

		const int32 Arm = Run->ArmsToRun[Run->ArmIndex];
		const UTraceSettings& Settings = UTraceSettings::Get();

		switch (Run->Step)
		{
		case 0:
		{
			ApplyChudRefreshArm(Arm);

			UTraceAbilityComponent* Comp = FindHumanComponent(WorldPtr);
			if (Comp == nullptr || Comp->GetOwningCharacter() == nullptr)
			{
				if (Run->SetupAttemptsLeft-- > 0)
				{
					ScheduleChudRefresh(Run, 1.0f);
					return false;
				}
				Run->Current.Invalidate(TEXT("no non-bot player with a pawn inside the 40s budget"));
				FinishChudArm(Run);
				ScheduleChudRefresh(Run, 0.f);
				return false;
			}

			Run->Comp = Comp;
			Comp->ServerSetCharacter(ETraceCharacterId::Chut);
			Comp->OnHalfTime();   // clears the cooldown AND the net state, so each arm starts level

			if (!ReacquireSubject(Run, WorldPtr, /*bRequireChudActive*/ false))
			{
				return RestartOrGiveUp(Run, TEXT("the framework had not equipped Chut on a living pawn yet"));
			}

			UTraceAbilitySetChut* Chut = Run->Chut.Get();
			ATraceCharacter* ChutPawn = Run->ChutPawn.Get();

			// --- press E, through the framework, exactly as the player does ---
			const bool bActivated = Comp->TryActivate();
			Run->Current.Add(TEXT("E opens Chud through the framework (the fixture's liveness)"),
				bActivated && Chut->IsChudActive(),
				FString::Printf(TEXT("TryActivate=%d active=%d remaining %.2fs of %.1fs, cooldown %.2fs"),
					bActivated ? 1 : 0, Chut->IsChudActive() ? 1 : 0,
					Chut->GetChudSecondsRemaining(), Settings.ChudDurationSeconds,
					Comp->GetActivatedCooldownRemaining()));

			Run->Step = 1;
			ScheduleChudRefresh(Run, ChudBurnSeconds);   // let the window actually drain
			return false;
		}

		case 1:
		{
			// CHUD MUST STILL BE RUNNING. If it is not, Chut died (spec v19 §4.2 wipes effects on
			// death) — the fixture collapsed and the arm starts again rather than reporting a rule
			// that never got to run.
			if (!ReacquireSubject(Run, WorldPtr, /*bRequireChudActive*/ true))
			{
				return RestartOrGiveUp(Run,
					TEXT("Chud was not running when the kill was due — Chut died and the death wipe stopped it"));
			}
			UTraceAbilityComponent* Comp = Run->Comp.Get();
			UTraceAbilitySetChut* Chut = Run->Chut.Get();
			ATraceCharacter* ChutPawn = Run->ChutPawn.Get();

			ATraceCharacter* VictimPawn = FindKillableVictim(WorldPtr, ChutPawn);
			if (VictimPawn == nullptr)
			{
				if (Run->SetupAttemptsLeft-- > 0)
				{
					ScheduleChudRefresh(Run, 1.0f);
					return false;
				}
				Run->Current.Invalidate(TEXT("no living, killable, non-carrier second pawn to knife"));
				FinishChudArm(Run);
				ScheduleChudRefresh(Run, 0.f);
				return false;
			}
			Run->Victim = VictimPawn;

			FString TeamDescription;
			const bool bTeamsOpposed = ForceOpposingTeams(ChutPawn, VictimPawn, TeamDescription);
			if (!bTeamsOpposed)
			{
				Run->Current.Invalidate(FString::Printf(
					TEXT("could not put the victim on the opposing team (%s) — NotifyCharacterDied skips a team "
					     "kill by design, so a failure here would not be about §10"), *TeamDescription));
				FinishChudArm(Run);
				ScheduleChudRefresh(Run, 0.f);
				return false;
			}

			// --- the state the owner's sentence is about: "5 seconds left" ---
			Run->RemainingBeforeKill = Chut->GetChudSecondsRemaining();
			Run->CooldownBeforeKill = Comp->GetActivatedCooldownRemaining();

			Run->Current.Add(TEXT("the window really drained before the kill (so 'back up' has somewhere to go)"),
				Chut->IsChudActive() && Run->RemainingBeforeKill < Settings.ChudDurationSeconds - 1.f,
				FString::Printf(TEXT("%.2fs left of %.1fs after %.1fs of real time; %s"),
					Run->RemainingBeforeKill, Settings.ChudDurationSeconds, ChudBurnSeconds, *TeamDescription));

			// --- THE KILL, THROUGH THE SHIPPING FUNNEL ---
			UTraceHealthComponent* VictimHealth = VictimPawn->FindComponentByClass<UTraceHealthComponent>();
			AController* KillerController = ChutPawn->GetController();
			if (VictimHealth == nullptr || KillerController == nullptr)
			{
				Run->Current.Invalidate(TEXT("the victim has no health component, or Chut has no controller"));
				FinishChudArm(Run);
				ScheduleChudRefresh(Run, 0.f);
				return false;
			}

			// The cause TraceMelee itself produces for a front knife, and the same call
			// UTraceWeaponComponent::ServerSwing makes. Everything downstream is the shipping game.
			VictimHealth->ApplyDamage(ChudTestLethalDamage, KillerController, TraceMelee::GetKnifeKillCause());

			Run->Current.Add(TEXT("the fixture actually killed somebody (liveness — no kill, no notification)"),
				!VictimPawn->IsAlive(),
				FString::Printf(TEXT("victim %s alive=%d after a '%s' for %.0f"),
					*GetNameSafe(VictimPawn), VictimPawn->IsAlive() ? 1 : 0,
					*TraceMelee::GetKnifeKillCause().ToString(), ChudTestLethalDamage));

			const float RemainingAfter = Chut->GetChudSecondsRemaining();
			const float CooldownAfter = Comp->GetActivatedCooldownRemaining();

			// *** THE SENTENCE. *** Up, and up to MAXIMUM — not merely "not down".
			Run->Current.Add(TEXT("spec v24 §10: the remaining time goes UP on a knife kill"),
				RemainingAfter > Run->RemainingBeforeKill + 0.5f,
				FString::Printf(TEXT("%.2fs -> %.2fs"), Run->RemainingBeforeKill, RemainingAfter));

			Run->Current.Add(TEXT("spec v24 §10: and it goes up to the MAXIMUM, not by a bit"),
				FMath::IsNearlyEqual(RemainingAfter, Settings.ChudDurationSeconds, 0.15f),
				FString::Printf(TEXT("%.2fs, want %.2fs (and NOT %.2fs, which is what extending would give)"),
					RemainingAfter, Settings.ChudDurationSeconds,
					Run->RemainingBeforeKill + Settings.ChudDurationSeconds));

			// *** THE OTHER HALF: "RATHER THAN THE COOLDOWN RESETTING". ***
			Run->Current.Add(TEXT("spec v24 §10: the E COOLDOWN is not reset by the kill"),
				CooldownAfter <= Run->CooldownBeforeKill + 0.05f,
				FString::Printf(TEXT("%.2fs -> %.2fs owed (it may only tick DOWN; a reset would jump it to %.1fs)"),
					Run->CooldownBeforeKill, CooldownAfter, Settings.ChudCooldownSeconds));

			Run->Step = 2;
			// Long enough for the window to expire on its own, so step 2 tests the INACTIVE case.
			ScheduleChudRefresh(Run, Settings.ChudDurationSeconds + 1.0f);
			return false;
		}

		case 2:
		{
			// Chud is SUPPOSED to be down here, so only the pawn has to still exist.
			if (!ReacquireSubject(Run, WorldPtr, /*bRequireChudActive*/ false))
			{
				return RestartOrGiveUp(Run, TEXT("Chut had no living pawn for the inactive case"));
			}
			UTraceAbilityComponent* Comp = Run->Comp.Get();
			UTraceAbilitySetChut* Chut = Run->Chut.Get();
			ATraceCharacter* ChutPawn = Run->ChutPawn.Get();

			// The window should have run out by itself. If it has not, the harness must not pretend
			// it tested the inactive case.
			Run->Current.Add(TEXT("the window expired on its own clock (the inactive case's fixture)"),
				!Chut->IsChudActive(),
				FString::Printf(TEXT("active=%d, %.2fs left"),
					Chut->IsChudActive() ? 1 : 0, Chut->GetChudSecondsRemaining()));

			ATraceCharacter* VictimPawn = FindKillableVictim(WorldPtr, ChutPawn);
			if (VictimPawn == nullptr)
			{
				if (Run->SetupAttemptsLeft-- > 0)
				{
					ScheduleChudRefresh(Run, 1.0f);
					return false;
				}
				Run->Current.Invalidate(TEXT("no second victim for the inactive case (bots had not respawned)"));
				FinishChudArm(Run);
				ScheduleChudRefresh(Run, 0.f);
				return false;
			}

			FString TeamDescription;
			ForceOpposingTeams(ChutPawn, VictimPawn, TeamDescription);

			const float CooldownBefore = Comp->GetActivatedCooldownRemaining();

			UTraceHealthComponent* VictimHealth = VictimPawn->FindComponentByClass<UTraceHealthComponent>();
			AController* KillerController = ChutPawn->GetController();
			if (VictimHealth != nullptr && KillerController != nullptr)
			{
				VictimHealth->ApplyDamage(ChudTestLethalDamage, KillerController, TraceMelee::GetKnifeKillCause());
			}

			// *** THE THING THE OWNER ASKED TO BE CHECKED BY NAME. *** A kill with the ability down must
			// not hand Chut a free activation.
			Run->Current.Add(TEXT("spec v24 §10: a knife kill with Chud INACTIVE does not silently start it"),
				!Chut->IsChudActive() && Chut->GetChudSecondsRemaining() <= 0.01f,
				FString::Printf(TEXT("active=%d, %.2fs left after killing %s (%s)"),
					Chut->IsChudActive() ? 1 : 0, Chut->GetChudSecondsRemaining(),
					*GetNameSafe(VictimPawn), *TeamDescription));

			Run->Current.Add(TEXT("...and the E cooldown is untouched by that kill either"),
				Comp->GetActivatedCooldownRemaining() <= CooldownBefore + 0.05f,
				FString::Printf(TEXT("%.2fs -> %.2fs owed"),
					CooldownBefore, Comp->GetActivatedCooldownRemaining()));

			// --- and now the ONE link the steps above cannot reach: an actual blade ---
			Comp->OnHalfTime();          // clears the cooldown so E is available again
			const bool bReArmed = Comp->TryActivate();
			ETraceMeleeRefusal EquipRefusal = ETraceMeleeRefusal::None;
			const bool bEquipped = TraceMelee::RequestEquip(ChutPawn, ETraceEquippedWeapon::Knife, &EquipRefusal);

			Run->Current.Add(TEXT("re-armed for the REAL SWING: Chud up and the knife in hand"),
				bReArmed && Chut->IsChudActive() && bEquipped,
				FString::Printf(TEXT("activate=%d active=%d equip=%d"),
					bReArmed ? 1 : 0, Chut->IsChudActive() ? 1 : 0, bEquipped ? 1 : 0));

			Run->Step = 3;
			// The pullout, then a burn so the window has drained again before the blade lands.
			ScheduleChudRefresh(Run, TraceMelee::GetSwapSeconds() + ChudBurnSeconds);
			return false;
		}

		// -------------------------------------------------------------------------------------
		// STEP 3 — THE REAL KNIFE. UTraceWeaponComponent::ServerSwing is the ONE link the steps
		// above cannot exercise, and it is the link the owner's sentence is actually about ("when
		// he gets a knife kill"). Everything here is staged with the project's own proven fixture:
		// stand the attacker on the victim's forward axis at 60% of the blade's reach, which is a
		// BACK-STAB and therefore a one-shot kill on a full-health pawn.
		// -------------------------------------------------------------------------------------
		case 3:
		{
			if (!ReacquireSubject(Run, WorldPtr, /*bRequireChudActive*/ true))
			{
				return RestartOrGiveUp(Run,
					TEXT("Chud was not running when the real swing was due — Chut died between steps"));
			}
			UTraceAbilityComponent* Comp = Run->Comp.Get();
			UTraceAbilitySetChut* Chut = Run->Chut.Get();
			ATraceCharacter* ChutPawn = Run->ChutPawn.Get();

			ATraceCharacter* VictimPawn = FindKillableVictim(WorldPtr, ChutPawn);
			if (VictimPawn == nullptr)
			{
				if (Run->SetupAttemptsLeft-- > 0)
				{
					ScheduleChudRefresh(Run, 1.0f);
					return false;
				}
				Run->Current.Invalidate(TEXT("no victim to stand behind for the real swing"));
				FinishChudArm(Run);
				ScheduleChudRefresh(Run, 0.f);
				return false;
			}
			Run->Victim = VictimPawn;

			FString TeamDescription;
			ForceOpposingTeams(ChutPawn, VictimPawn, TeamDescription);
			PlaceChutBehind(ChutPawn, VictimPawn);

			Run->RemainingBeforeKill = Chut->GetChudSecondsRemaining();
			Run->CooldownBeforeKill = Comp->GetActivatedCooldownRemaining();

			ETraceMeleeRefusal SwingRefusal = ETraceMeleeRefusal::None;
			const bool bSwung = TraceMelee::RequestSwing(ChutPawn, &SwingRefusal);

			// A REFUSED SWING IS A BROKEN FIXTURE, NOT A BROKEN RULE, so it restarts the arm instead
			// of failing it. Measured: one run refused with "carrying the Core", which is the game
			// being right (a carrier cannot swing) about a situation the harness put him in.
			if (!bSwung)
			{
				return RestartOrGiveUp(Run, FString::Printf(
					TEXT("the swing was refused (%s)"), LexToString(SwingRefusal)));
			}

			Run->SwingPressRealTime = FPlatformTime::Seconds();

			// The staging has to be true at the RESOLVE, not at the press — see HoldBackstabStaging.
			HoldBackstabStaging(ChutPawn, VictimPawn,
				Run->SwingPressRealTime + static_cast<double>(TraceMelee::GetSwingWindupSeconds()) + 0.35);

			Run->Current.Add(TEXT("REAL SWING: the blade actually left the hand (liveness)"),
				bSwung,
				FString::Printf(TEXT("swing=%d refusal=%s, Chud %.2fs left, %s"),
					bSwung ? 1 : 0, LexToString(SwingRefusal), Run->RemainingBeforeKill, *TeamDescription));

			Run->Step = 4;
			// The blade resolves SwingWindupSeconds after the press; give the death, the game mode's
			// funnel and the replication a comfortable margin on top.
			ScheduleChudRefresh(Run, TraceMelee::GetSwingWindupSeconds() + 0.8f);
			return false;
		}

		case 4:
		{
			UTraceAbilityComponent* Comp = Run->Comp.Get();
			UTraceAbilitySetChut* Chut = Run->Chut.Get();
			ATraceCharacter* VictimPawn = Run->Victim.Get();
			ATraceCharacter* ChutPawn = Run->ChutPawn.Get();

			// DELIBERATELY NOT ReacquireSubject: this step READS the result of the swing, and topping
			// Chut's health up or swapping in a respawned pawn here would edit the very thing being
			// measured. If HE died in the windup the fixture collapsed and the arm restarts.
			if (Comp == nullptr || Chut == nullptr || ChutPawn == nullptr || !ChutPawn->IsAlive())
			{
				return RestartOrGiveUp(Run, TEXT("Chut died during the swing's wind-up"));
			}

			const bool bVictimDead = (VictimPawn == nullptr) || !VictimPawn->IsAlive();

			// LIVENESS FIRST, AND IT INVALIDATES RATHER THAN FAILS. A blade that missed is a broken
			// fixture, not a broken rule, and reporting it as a §10 failure would be exactly the kind
			// of confident nonsense this project has a house rule about.
			if (!bVictimDead)
			{
				return RestartOrGiveUp(Run, FString::Printf(
					TEXT("the staged back-stab did not kill %s (it moved, or the blade missed)"),
					*GetNameSafe(VictimPawn)));
			}

			const float RemainingAfter = Chut->GetChudSecondsRemaining();
			const float CooldownAfter = Comp->GetActivatedCooldownRemaining();

			// *** THE EXPECTED NUMBER IS "MAX MINUS WHAT HAS ELAPSED SINCE THE PRESS", NOT "MAX". ***
			// The blade resolves SwingWindupSeconds after the press and this read is later still, so a
			// window that was correctly pushed to 10.00s has already spent that time by now. Asking for
			// a flat 10.00 here failed a build that was working — see FChudRun::SwingPressRealTime. The
			// lower bound still separates the arms by miles: the red arm measures ~5.9s against a
			// threshold of ~8.0s.
			const float ElapsedSincePress =
				static_cast<float>(FPlatformTime::Seconds() - Run->SwingPressRealTime);
			const float FullDuration = UTraceSettings::Get().ChudDurationSeconds;
			const float FloorForRefreshed = FullDuration - ElapsedSincePress - 0.35f;

			Run->Current.Add(TEXT("*** A REAL KNIFE KILL, THROUGH ServerSwing, pushes Chud back to MAXIMUM ***"),
				RemainingAfter > Run->RemainingBeforeKill + 0.3f
					&& RemainingAfter >= FloorForRefreshed
					&& RemainingAfter <= FullDuration + 0.05f,
				FString::Printf(TEXT("%.2fs -> %.2fs after back-stabbing %s; max %.1fs less the %.2fs spent "
				                     "between the press and this read, so anything >= %.2fs is 'back to max' "
				                     "(the red arm measures ~%.2fs here)"),
					Run->RemainingBeforeKill, RemainingAfter, *GetNameSafe(VictimPawn), FullDuration,
					ElapsedSincePress, FloorForRefreshed,
					FMath::Max(0.f, Run->RemainingBeforeKill - ElapsedSincePress)));

			Run->Current.Add(TEXT("...and the real swing did not reset the E cooldown either"),
				CooldownAfter <= Run->CooldownBeforeKill + 0.05f,
				FString::Printf(TEXT("%.2fs -> %.2fs owed"), Run->CooldownBeforeKill, CooldownAfter));

			FinishChudArm(Run);
			ScheduleChudRefresh(Run, 1.5f);   // let the next arm find a respawned victim
			return false;
		}

		default:
			FinishChudArm(Run);
			ScheduleChudRefresh(Run, 0.f);
			return false;
		}
	}

	FAutoConsoleCommand CmdChudRefreshTest(
		TEXT("Trace.Chut.ChudRefreshTest"),
		TEXT("Spec v24 §10, server only. Red-armed proof that a KNIFE KILL pushes a running Chud back to its "
		     "maximum duration, that it does NOT reset the E cooldown, and that a kill with Chud down does not "
		     "start it. Drives the real kill funnel (ApplyDamage -> NotifyCharacterDied -> NotifyKill), not "
		     "UTraceAbilitySetChut::OnKill directly. Arm 0 sets Trace.Chut.ChudKillRefresh 0 and MUST fail; "
		     "a red arm that passes is reported INVALID. Takes about 30s."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			TSharedPtr<FChudRun> Run = MakeShared<FChudRun>();
			ScheduleChudRefresh(Run, 0.f);
		}));
}
#endif // !UE_BUILD_SHIPPING
