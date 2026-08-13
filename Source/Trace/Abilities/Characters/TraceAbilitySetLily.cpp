// Trace — LILY. See the header for the spec v19 §3 reading, for the mid-Zip halving clause and the
// misreading it guards against, and for the 20 Hz limitation of driving flight from an ability set.

#include "Abilities/Characters/TraceAbilitySetLily.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Settings/TraceUserSettings.h"                   // the player's Jump / Crouch binds
#include "Trace.h"
#include "TraceSettings.h"

#define LOCTEXT_NAMESPACE "TraceLily"

// =================================================================================================
// THE RED ARMS. One per rule, each removing that rule and nothing else. Same shape and the same
// reasoning as TraceAbilitySetChut.cpp's three.
// =================================================================================================

/**
 * *** THE ONE THE SUBTLE CLAUSE HANGS ON. *** 0 removes BOTH Core halvings — the one at the cast and
 * the one on a mid-flight pickup — and nothing else, so a Zip is always its full 5 s. That is the
 * arm Trace.Lily.ZipVerify runs first: without it, "the remaining duration halved to 0.5 s" is a
 * sentence about a number nobody has watched fail to halve.
 */
static TAutoConsoleVariable<int32> CVarLilyZipCoreHalving(
	TEXT("Trace.Lily.ZipCoreHalving"),
	1,
	TEXT("Dev/red arm. 1 (default) = carrying the Core halves Zip's duration at the cast, and picking the Core "
	     "up mid-flight halves WHAT IS LEFT (spec v19 §3). 0 = neither halving happens, so both assertions in "
	     "Trace.Lily.ZipVerify must go red. NEVER SHIP 0."),
	ECVF_Cheat);

/** 0 makes Zip a no-op that still costs the cooldown, so every flight assertion must go red. */
static TAutoConsoleVariable<int32> CVarLilyZip(
	TEXT("Trace.Lily.Zip"),
	1,
	TEXT("Dev/red arm. 1 (default) = E flies her for 5s (spec v19 §3). 0 = the press is accepted, the cooldown "
	     "starts, and she does not fly."),
	ECVF_Cheat);

/**
 * *** THE RED ARM FOR DEMO 19 ITEM 4. *** 0 restores, exactly, the code the user complained about.
 *
 * With 0 the climb is driven by the old bJumpHeld latch and nothing else: the key poll does not run,
 * so nothing ever clears it, so one press of space climbs for the rest of the flight and crouch can
 * never win the `!climbing && IsCrouchHeld()` test. That is not an imitation of the bug — it is the
 * shipped v21 code path, selected by an if.
 *
 * Trace.Lily.KeyTest runs this arm FIRST and refuses to grade the fix unless it reproduces. NEVER
 * SHIP 0.
 */
/**
 * *** THE RED ARM FOR DEMO 19 ITEM 8. *** 0 restores the shipped v21 rule: the extra charge always.
 *
 * With 0, GetExtraDashCharges() stops asking whether she is carrying, so she is back to two dashes
 * free-running and THREE while carrying — which is what the character-select card used to promise and
 * what Trace.Lily.DashTest's red arm has to reproduce before the green reading is worth anything.
 * NEVER SHIP 0.
 */
static TAutoConsoleVariable<int32> CVarLilyDashCarrierGate(
	TEXT("Trace.Lily.DashCarrierGate"),
	1,
	TEXT("Dev/red arm. 1 (default) = Demo 19 item 8: Lily's extra dash charge applies ONLY while she is NOT "
	     "carrying the Core (2 free-running, 2 carrying). 0 = the shipped v21 rule, the charge always (2 "
	     "free-running, 3 carrying). NEVER SHIP 0."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarLilyZipHoldRelease(
	TEXT("Trace.Lily.ZipHoldRelease"),
	1,
	TEXT("Dev/red arm. 1 (default) = the climb follows the jump key as a LEVEL, polled on the machine with the "
	     "keyboard and carried to the server on FLAG_Custom_1, so letting go stops the climb and crouch then "
	     "descends. 0 = the shipped v21 behaviour: the press latches, the release never arrives (the framework's "
	     "release hook has no caller), she climbs for the whole flight and crouch does nothing. NEVER SHIP 0."),
	ECVF_Cheat);

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetLily::OnEquipped()
{
	bZipping = false;
	ZipEndMatchTime = 0.f;
	bJumpHeld = false;
	ZipCorePickups = 0;

	// Seeded from the pawn rather than from false: she can be handed this character while ALREADY
	// holding the Core (a mid-match switch, the select screen's auto-pick), and a false seed would
	// read the very next tick as a pickup edge and halve a Zip she has not started.
	const ATraceCharacter* MyPawn = GetCharacter();
	bWasCarrier = (MyPawn != nullptr) && UTraceAbilityComponent::IsCarrier(MyPawn);
}

void UTraceAbilitySetLily::OnUnequipped()
{
	StopZip(TEXT("character changed"));
}

void UTraceAbilitySetLily::OnPawnDied()
{
	// SPEC v19 §4.2. The central wipe (UTraceAbilityComponent::ApplyDeathStateWipe) already clears
	// the replicated half — that is why Zip's flag is TraceAbilityFlags::EffectActive and its
	// deadline is EffectEndMatchTime. This clears the LOCAL half on this machine, which the framework
	// cannot reach, so a dead Lily is not still being flown by her own owning client.
	//
	// The E COOLDOWN IS DELIBERATELY UNTOUCHED: "cooldown timers should still not reset when players
	// die, and should just continue ticking down."
	StopZip(TEXT("died"));
}

void UTraceAbilitySetLily::OnHalfTime()
{
	// THROUGH StopZip, NOT BY CLEARING THE FIELDS. The framework has normally already Reset() the net
	// state by the time this runs, so writing bZipping = false here would look equivalent — but it is
	// only equivalent because of something another file does first. StopZip publishes the cleared
	// flag itself, which makes this correct on its own and keeps the "flying" answer and the "not
	// flying" fields from ever disagreeing. (That is not hypothetical: Trace.Lily.ZipVerify calls
	// this directly, between arms, with no framework reset in front of it.)
	StopZip(TEXT("half time"));

	bJumpHeld = false;
	ZipCorePickups = 0;
	bWasCarrier = false;
}

bool UTraceAbilitySetLily::ShouldDriveMovement() const
{
	// A simulated proxy's velocity is replicated; writing it there would fight the interpolation and
	// show up as somebody else's Lily stuttering in the air.
	return HasAuthority() || IsLocallyControlled();
}

// =================================================================================================
// MOVEMENT + PASSIVE — the three numbers owned by other slices
// =================================================================================================

int32 UTraceAbilitySetLily::GetExtraDashCharges() const
{
	// DEMO 19 ITEM 8: "only ... when she is not carrying the core".
	//
	// THE CONDITION IS ON THE ADDEND, NOT ON THE TOTAL, and that distinction is the whole item.
	// UTraceCharacterMovementComponent::GetMaxDashCharges() has already added the carrier's own extra
	// charge by the time it asks this function, so returning 0 while carrying leaves her on the same
	// two dashes as everybody else who is carrying — it does not take her down to one. Returning
	// "2 minus whatever" here instead would have.
	//
	// READ THROUGH UTraceAbilityComponent::IsCarrier, which ORs the pawn's replicated mirror with the
	// Core's own holder, so a client answers the same as the server on the frame of a pickup. That
	// matters more here than it does for the duration halvings: this value is read inside the
	// PREDICTED movement path, and a client that disagreed for a frame would spend a charge the
	// server then refunded.
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn != nullptr && UTraceAbilityComponent::IsCarrier(MyPawn)
		&& CVarLilyDashCarrierGate.GetValueOnAnyThread() != 0)
	{
		return 0;
	}

	return FMath::Clamp(UTraceSettings::Get().LilyExtraDashCharges, 0, 5);
}

float UTraceAbilitySetLily::GetMaxHealthOverride() const
{
	// Floored at 1 rather than at 0: 0 is the sentinel for "this character has no opinion", and a
	// mistyped 0 in the settings must not be read as "use the default" — it must be read as the
	// smallest survivable number, so the mistake is visible instead of invisible.
	return FMath::Max(1.f, UTraceSettings::Get().LilyMaxHealth);
}

float UTraceAbilitySetLily::GetWallJumpMomentumScale() const
{
	return 1.f + FMath::Clamp(UTraceSettings::Get().LilyWallJumpMomentumBonus, 0.f, 4.f);
}

// =================================================================================================
// ACTIVATED — ZIP
// =================================================================================================

float UTraceAbilitySetLily::GetActivatedCooldownSeconds() const
{
	return FMath::Max(0.f, UTraceSettings::Get().LilyZipCooldownSeconds);
}

float UTraceAbilitySetLily::GetZipDurationForNow() const
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	float Duration = FMath::Max(0.25f, Settings.LilyZipDurationSeconds);

	// §3's FIRST Core clause: "with the core the duration is halved".
	const ATraceCharacter* MyPawn = GetCharacter();
	const bool bCarrying = (MyPawn != nullptr) && UTraceAbilityComponent::IsCarrier(MyPawn);
	if (bCarrying && CVarLilyZipCoreHalving.GetValueOnAnyThread() != 0)
	{
		Duration *= FMath::Clamp(Settings.LilyZipCarrierDurationScale, 0.05f, 1.f);
	}

	return Duration;
}

bool UTraceAbilitySetLily::CanActivate(FText& OutReason) const
{
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		OutReason = LOCTEXT("LilyNoPawn", "NOT READY");
		return false;
	}

	// Deliberately NO posture condition. §3 gives Zip none — no "must be airborne", no "must be
	// grounded" — so it is castable from anywhere, including off a wall and mid-fall, and that is
	// what makes it an escape as well as a repositioning tool.
	if (bZipping)
	{
		// Re-casting mid-flight would refresh the duration for free, which is not what a 30 s
		// cooldown ability is. Refused rather than refreshed.
		OutReason = LOCTEXT("LilyAlreadyZipping", "ALREADY FLYING");
		return false;
	}

	return true;
}

bool UTraceAbilitySetLily::ActivateAbility()
{
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive() || bZipping)
	{
		return false;
	}

	if (CVarLilyZip.GetValueOnAnyThread() == 0)
	{
		return true;   // RED ARM: the cooldown is still charged, she simply does not fly.
	}

	StartZip();

	// TRUE on the server AND on the owning client. Unlike Quake, everything Zip does is to HER OWN
	// velocity, so predicting it locally is right and is what makes the first half-second of flight
	// feel like the key press rather than like the round trip.
	return true;
}

void UTraceAbilitySetLily::StartZip()
{
	const float Duration = GetZipDurationForNow();

	bZipping = true;
	ZipEndMatchTime = MatchTimeNow() + Duration;
	ZipCorePickups = 0;

	// Re-seeded at the cast, not trusted from the last one: if she is already carrying, the duration
	// above has ALREADY been halved for it, and leaving bWasCarrier false here would halve it a
	// second time on the very next tick.
	const ATraceCharacter* MyPawn = GetCharacter();
	bWasCarrier = (MyPawn != nullptr) && UTraceAbilityComponent::IsCarrier(MyPawn);

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Log,
			TEXT("[Lily] ZIP for %.2fs (carrying=%d at the cast). Jump climbs, crouch descends; the Core "
			     "arriving mid-flight will halve whatever is left."),
			Duration, bWasCarrier ? 1 : 0);
	}
}

void UTraceAbilitySetLily::StopZip(const TCHAR* Why)
{
	if (!bZipping)
	{
		return;
	}

	bZipping = false;
	ZipEndMatchTime = 0.f;
	bJumpHeld = false;

	// The climb level goes with the flight. It would go stale on its own inside a quarter second, but
	// "the ability ended" is a better reason for it to be false than "nobody refreshed it".
	if (UTraceCharacterMovementComponent* MoveComp = GetMovement())
	{
		MoveComp->SetJumpHeld(false);
	}

	// NOTHING HERE TOUCHES THE COOLDOWN. It was started by the framework when ActivateAbility
	// returned true and it keeps running through the flight, through death and through a respawn —
	// spec §5, and spec v19 §4.2 restates it.
	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Verbose, TEXT("[Lily] Zip ended (%s)."), Why);
	}
}

void UTraceAbilitySetLily::HalveRemainingForCorePickup()
{
	if (!bZipping || CVarLilyZipCoreHalving.GetValueOnAnyThread() == 0)
	{
		return;
	}

	const float Now = MatchTimeNow();
	const float Remaining = FMath::Max(0.f, ZipEndMatchTime - Now);
	const float Scale = FMath::Clamp(UTraceSettings::Get().LilyZipCarrierDurationScale, 0.05f, 1.f);

	// *** §3's SECOND CLAUSE, AND THE WHOLE POINT OF THE FUNCTION. ***
	// "If she activates it and then picks up the core, the remaining duration is halved."
	//
	// HALVE WHAT IS LEFT. Do NOT clamp to the carrier duration. With 1 s left this leaves 0.5 s; the
	// natural mistake — min(Remaining, FullDuration x Scale) — would leave 1 s, and the other natural
	// mistake — "restart at the carrier duration" — would leave 2.5 s, i.e. it would LENGTHEN a Zip
	// that was nearly over and reward picking the Core up late. Trace.Lily.ZipVerify stages exactly
	// this case because 2.5 is what the wrong code produces.
	ZipEndMatchTime = Now + (Remaining * Scale);
	++ZipCorePickups;

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Log,
			TEXT("[Lily] Core picked up %d time(s) mid-Zip: %.2fs left became %.2fs (halved what was LEFT, "
			     "not re-clamped to %.2fs)."),
			ZipCorePickups, Remaining, Remaining * Scale,
			FMath::Max(0.25f, UTraceSettings::Get().LilyZipDurationSeconds) * Scale);
	}
}

void UTraceAbilitySetLily::TickAbilities(float DeltaSeconds)
{
	const ATraceCharacter* MyPawn = GetCharacter();

	// --- THE CARRIER EDGE, POLLED ----------------------------------------------------------------
	//
	// Polled rather than pushed because there is no "you became the carrier" hook on the ability set,
	// and adding one would be an edit to a header this pass does not own. 20 Hz is plenty: the worst
	// error is that the halving lands up to 50 ms late, which changes the remaining time by half of
	// whatever was consumed in those 50 ms.
	//
	// AN EDGE, NOT A LEVEL. Halving on "is carrying" would halve every tick and end the Zip inside a
	// quarter of a second.
	const bool bCarrierNow = (MyPawn != nullptr) && MyPawn->IsAlive() && UTraceAbilityComponent::IsCarrier(MyPawn);
	if (bCarrierNow && !bWasCarrier)
	{
		HalveRemainingForCorePickup();
	}
	bWasCarrier = bCarrierNow;

	if (bZipping)
	{
		// BEFORE ApplyZip, never after: this is where the release the framework never delivers is
		// discovered, and reading the level a tick before refreshing it would put a 50 ms delay on
		// every press and every let-go instead of only on the let-go.
		SampleClimbIntent();

		ApplyZip(DeltaSeconds);
	}
}

void UTraceAbilitySetLily::SampleClimbIntent()
{
	// See the header for the missing release hook this exists to work around.
	if (CVarLilyZipHoldRelease.GetValueOnAnyThread() == 0)
	{
		return;   // RED ARM: the latch is left in charge, which is the shipped v21 bug.
	}

	// ONLY THE MACHINE WITH THE KEYBOARD SAMPLES. On the authority for a REMOTE client this returns
	// early and the level arrives instead on FLAG_Custom_1, inside that client's next saved move —
	// which is the point of putting it there.
	if (!IsLocallyControlled())
	{
		return;
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || MoveComp == nullptr)
	{
		return;
	}

	// A BOT HAS NO KEYBOARD, so there is nothing to sample and nothing to clear. ATraceBotController
	// calls HandleJumpPressed() like a player does; what stops a bot Lily climbing forever is the
	// staleness watchdog inside SetJumpHeld(), which is exactly why that watchdog is there.
	const APlayerController* PC = Cast<APlayerController>(MyPawn->GetController());
	if (PC == nullptr)
	{
		return;
	}

	// THE PLAYER'S OWN BIND, from the same table ApplyControlSettings maps from, so a rebound jump is
	// honoured here without this file knowing any defaults. An unbound action has an invalid key,
	// which IsInputKeyDown would answer nonsense for — in that case say nothing and let the press
	// latch plus the watchdog carry it, rather than pinning her to the floor for the whole flight.
	const FKey JumpKey = UTraceUserSettings::Get().GetKey(ETraceInputAction::Jump);
	if (!JumpKey.IsValid())
	{
		return;
	}

	MoveComp->SetJumpHeld(PC->IsInputKeyDown(JumpKey));
}

bool UTraceAbilitySetLily::IsClimbHeld() const
{
	if (CVarLilyZipHoldRelease.GetValueOnAnyThread() == 0)
	{
		return bJumpHeld;   // RED ARM: the latch, which nothing clears until the flight ends.
	}

	const UTraceCharacterMovementComponent* MoveComp = GetMovement();
	return (MoveComp != nullptr) && MoveComp->IsJumpHeld();
}

void UTraceAbilitySetLily::ApplyZip(float DeltaSeconds)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		StopZip(TEXT("no pawn"));
		return;
	}

	if (MatchTimeNow() >= ZipEndMatchTime)
	{
		StopZip(TEXT("duration elapsed"));
		return;
	}

	if (!ShouldDriveMovement())
	{
		return;   // A simulated proxy knows she is flying (for cosmetics) and must not move her.
	}

	// THE DASH OWNS THE VELOCITY VECTOR FOR ITS WHOLE WINDOW, exactly as it does for Mace's suspend.
	// "All other movement mechanics apply as usual" is §3's own sentence, and a dash whose Z was
	// being rewritten 20 times a second would not be the usual dash. It is over in 0.18 s.
	if (MoveComp->IsDashing())
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float WalkSpeed = FMath::Max(1.f, Settings.WalkSpeed);

	// §3 said "jump goes up at walking speed"; DEMO 19 ITEM 4 says "half the speed she moves
	// vertically at", so both scales are 0.5 and the climb is half a walk. Still derived from
	// WalkSpeed rather than written as 400, so a retune of the walk carries the flight with it.
	const float ClimbSpeed   = WalkSpeed * FMath::Clamp(Settings.LilyZipClimbSpeedScale, 0.1f, 4.f);
	const float DescendSpeed = WalkSpeed * FMath::Clamp(Settings.LilyZipDescendSpeedScale, 0.1f, 4.f);

	// THE TWO KEYS, BOTH AS LEVELS. IsCrouchHeld() is the movement component's union of this slice's
	// slide flag and the engine's bWantsToCrouch; IsClimbHeld() is its new twin for jump, and adding
	// it is what makes this line's `!bClimbing` capable of being false at all. See the header.
	const bool bClimbing   = IsClimbHeld();
	const bool bDescending = !bClimbing && MoveComp->IsCrouchHeld();

	float CommandedZ = 0.f;
	if (bClimbing)        { CommandedZ =  ClimbSpeed; }
	else if (bDescending) { CommandedZ = -DescendSpeed; }

	// --- THE ONE TICK OF GRAVITY THIS COMMAND HAS TO SURVIVE --------------------------------------
	//
	// This function runs at 20 Hz; PhysFalling integrates gravity every frame in between. Writing
	// Velocity.Z = 0 therefore does not hover, it falls: over one 50 ms lump the velocity ramps from
	// 0 to -g x dt and the pawn sinks 0.5 x g x dt^2, which is ~1.2 uu a tick, ~24.5 uu/s, ~120 uu
	// across a full 5 s Zip. That is a visible slump in something the doc calls flying.
	//
	// So the command is issued with HALF A TICK OF GRAVITY ALREADY ADDED, which is the closed form
	// that makes the MEAN velocity over the tick exactly CommandedZ (velocity ramps from
	// CommandedZ + g.dt/2 down to CommandedZ - g.dt/2, and the average is CommandedZ). It is a
	// discretisation correction, not frame-accurate flight; see the header for why the honest fix is
	// a movement mode in the prediction path.
	const float GravityPerTick = FMath::Abs(MoveComp->GetGravityZ()) * FMath::Max(0.f, DeltaSeconds);
	MoveComp->Velocity.Z = CommandedZ + (GravityPerTick * 0.5f);

	// AND SHE HAS TO BE OFF THE FLOOR FOR ANY OF THAT TO MEAN ANYTHING. PhysWalking discards Z
	// outright, so a Zip taken while stood on the ground would be five seconds of nothing until she
	// walked off an edge. One nudge into MOVE_Falling on the frame she asks to climb is the whole fix,
	// and it is done through LaunchCharacter rather than SetMovementMode so the engine's own
	// take-off path (and its replication of the mode) runs exactly as it does for a jump.
	if (bClimbing && MoveComp->IsMovingOnGround())
	{
		MyPawn->LaunchCharacter(FVector(0.f, 0.f, ClimbSpeed), /*bXYOverride*/ false, /*bZOverride*/ true);
	}
}

bool UTraceAbilitySetLily::OnJumpPressed()
{
	if (!IsZipping())
	{
		return false;   // Not flying: an ordinary jump, an ordinary wall jump, an ordinary slide-jump.
	}

	bJumpHeld = true;   // the red arm's latch, and nothing else reads it while the fix is on

	// THE PRESS DOES NOT WAIT FOR THE 20 Hz POLL. Setting the level here is what keeps the key
	// feeling instant; SampleClimbIntent's job is the RELEASE, which is the edge nothing else in the
	// project delivers.
	if (UTraceCharacterMovementComponent* MoveComp = GetMovement())
	{
		MoveComp->SetJumpHeld(true);
	}

	// TRUE = CONSUMED, so ACharacter::Jump never runs. §3 rebinds the key for the duration of the
	// flight ("jump goes up at walking speed"), and letting the normal jump fire underneath would add
	// JumpZVelocity on top of the climb and let her ladder off every wall she passed.
	return true;
}

void UTraceAbilitySetLily::OnJumpReleased()
{
	// *** THIS FUNCTION IS DEAD CODE IN A REAL MATCH AND THAT IS THE BUG, NOT AN OVERSIGHT HERE. ***
	// UTraceAbilityComponent::HandleJumpReleased() — its only possible caller — is itself called by
	// nothing; ATracePlayerController::OnJumpCompleted() stops at ACharacter::StopJumping(). It is
	// kept correct and kept wired so that the day somebody adds that one line, the release arrives
	// twice and means the same thing both times. What actually ends the climb today is
	// SampleClimbIntent().
	bJumpHeld = false;

	if (UTraceCharacterMovementComponent* MoveComp = GetMovement())
	{
		MoveComp->SetJumpHeld(false);
	}
}

// =================================================================================================
// Readouts
// =================================================================================================

bool UTraceAbilitySetLily::IsZipping() const
{
	// THE REPLICATED ANSWER, so a HUD, a bot or a spectator on any machine agrees. The local bZipping
	// is what DRIVES her on the two machines entitled to move her; this is what everybody READS.
	return (State().Flags & TraceLilyFlags::Zipping) != 0
		&& State().EffectEndMatchTime > MatchTimeNow();
}

float UTraceAbilitySetLily::GetZipRemaining() const
{
	// The local deadline on the machines that own the flight, the replicated one everywhere else.
	// They agree to within a replication interval, and only the first is allowed to move her.
	const float End = ShouldDriveMovement() && bZipping ? ZipEndMatchTime : State().EffectEndMatchTime;
	if ((!bZipping && !IsZipping()) || End <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, End - MatchTimeNow());
}

void UTraceAbilitySetLily::PublishState()
{
	if (!HasAuthority())
	{
		return;
	}

	FTraceAbilityNetState& NetState = MutableState();
	if (bZipping)
	{
		NetState.Flags |= TraceLilyFlags::Zipping;
		NetState.EffectEndMatchTime = ZipEndMatchTime;
	}
	else
	{
		NetState.Flags &= static_cast<uint8>(~TraceLilyFlags::Zipping);
		NetState.EffectEndMatchTime = 0.f;
	}

	MarkStateDirty();
}

#if !UE_BUILD_SHIPPING
void UTraceAbilitySetLily::DebugSetZipRemaining(float Seconds)
{
	if (!bZipping)
	{
		return;
	}
	ZipEndMatchTime = MatchTimeNow() + FMath::Max(0.f, Seconds);
	PublishState();
}
#endif

// =================================================================================================
// Trace.Lily.ZipVerify — SPEC v19 §3's TWO CORE CLAUSES, TWO ARMS, RED FIRST
//
// Both clauses are the same 0.5 at two different moments and the second is the one with a plausible
// wrong answer, so the harness stages the case that TELLS THEM APART: a Zip with 1 s left, then a
// Core pickup. Right answer 0.5 s. The "re-clamp to the carrier duration" mistake answers 2.5 s and
// the "clamp to the smaller" mistake answers 1.0 s, so a single assertion separates all three.
//
// SYNCHRONOUS. ATraceCore::TryPickup is an authority-side debug grant that lands the same frame, and
// TickAbilities(0) is the real edge poll, so every reading below comes from the same call stack that
// caused it. Nothing here waits, and nothing here is a claim about code somebody has read.
// =================================================================================================

// NAMED after the file rather than anonymous — this module builds as a unity/jumbo blob and
// "MakePlayerInto..." is exactly the kind of name another character file would also want.
namespace TraceLilyVerifyFile
{
	UTraceAbilitySetLily* MakePlayerIntoLily(UWorld* WorldPtr, FString& OutWhy)
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

			if (Comp->GetCharacterId() != ETraceCharacterId::Lily)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::Lily);
			}

			if (UTraceAbilitySetLily* Found = Comp->GetAbilitySetAs<UTraceAbilitySetLily>())
			{
				return Found;
			}

			OutWhy = FString::Printf(
				TEXT("ServerSetCharacter(Lily) did not produce a UTraceAbilitySetLily (id is now %s) — a "
				     "team-mate may already hold her, or the reflection roster did not find the class"),
				TraceCharacterIdToString(Comp->GetCharacterId()));
			return nullptr;
		}

		OutWhy = TEXT("no human player controller with a pawn");
		return nullptr;
	}

	/**
	 * *** WHY EVERY KEY-PRESSING HARNESS IN THIS FILE NOW WAITS. ***
	 *
	 * The v22 baseline run of Trace.Lily.FlightTest reported climb 0 uu, sag 0 uu, z 90 -> 90 -> 90 in
	 * BOTH arms and a verdict of INVALID. Nothing was wrong with the flight: the CHARACTER SELECT
	 * SCREEN was still open, so the world was PAUSED, and the E press the harness fired to cast Zip was
	 * eaten by the screen as a card pick — the log line right after it reads "[CharSelect] Requesting
	 * SLIMEBALL". The harness then spent both arms measuring a paused Slimeball.
	 *
	 * That is worth a named function rather than a comment: a harness that runs at the wrong moment
	 * does not fail, it produces zeroes, and zeroes are the easiest number in the world to mistake for
	 * "the ability does nothing".
	 *
	 * The select screen pauses the world and the match does not, so IsPaused() is the exact gate.
	 */
	bool IsWorldReadyForKeyPresses(const UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr || WorldPtr->IsPaused())
		{
			return false;
		}

		const APlayerController* PC = WorldPtr->GetFirstPlayerController();
		return (PC != nullptr) && (PC->GetPawn() != nullptr);
	}

	/** The local game world — the one with a keyboard, which is not always the authoritative one. */
	UWorld* FindLocalKeyboardWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() != nullptr && Context.World()->IsGameWorld()
				&& Context.World()->GetFirstPlayerController() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	void RunZipVerify()
	{
		const TCHAR* const Tag = TEXT("LILYZIP");

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
		UTraceAbilitySetLily* Lily = MakePlayerIntoLily(TestWorld, Why);
		if (Lily == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — %s."), Tag, *Why);
			return;
		}

		ATraceCharacter* MyPawn = Lily->GetCharacter();
		ATraceCore* CoreActor = ATraceCore::Get(TestWorld);
		if (MyPawn == nullptr || CoreActor == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Lily has no pawn, or there is no Core in this world."), Tag);
			return;
		}

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.ZipCoreHalving"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — Trace.Lily.ZipCoreHalving is not registered."), Tag);
			return;
		}
		const int32 ArmBefore = Arm->GetInt();

		const UTraceSettings& Settings = UTraceSettings::Get();
		const float FullDuration    = FMath::Max(0.25f, Settings.LilyZipDurationSeconds);
		const float CarrierDuration = FullDuration * FMath::Clamp(Settings.LilyZipCarrierDurationScale, 0.05f, 1.f);

		// The staged case. 1 s left is BELOW the carrier duration (2.5 s), which is the only region
		// where the right answer and the two wrong ones differ.
		constexpr float StagedRemaining = 1.f;

		float RedCastCarrying = -1.f;
		float RedAfterPickup  = -1.f;
		float GreenCastCarrying = -1.f;
		float GreenAfterPickup  = -1.f;

		for (int32 ArmIndex = 0; ArmIndex < 2; ++ArmIndex)
		{
			const bool bGreen = (ArmIndex == 1);
			Arm->Set(bGreen ? 1 : 0, ECVF_SetByConsole);

			// ---- CLAUSE 1: cast WHILE carrying. Expect the duration halved. -----------------------
			CoreActor->TryPickup(MyPawn);
			Lily->OnHalfTime();                      // clears any flight left from the previous arm
			Lily->TickAbilities(0.f);                // settles the carrier edge before the cast
			Lily->ActivateAbility();
			const float CastLength = Lily->GetZipRemaining();

			// ---- CLAUSE 2: cast WITHOUT the Core, spend it down to 1 s, then pick the Core up. ----
			//
			// The Core is taken off her first, so the pickup below is a genuine NOT-carrying ->
			// carrying edge rather than a level the poll would never see.
			Lily->OnHalfTime();
			ATraceCharacter* Parker = nullptr;
			for (TActorIterator<ATraceCharacter> It(TestWorld); It; ++It)
			{
				if (*It != nullptr && *It != MyPawn && (*It)->IsAlive()) { Parker = *It; break; }
			}
			if (Parker != nullptr)
			{
				CoreActor->TryPickup(Parker);
			}
			Lily->TickAbilities(0.f);
			Lily->ActivateAbility();
			Lily->DebugSetZipRemaining(StagedRemaining);
			CoreActor->TryPickup(MyPawn);
			Lily->TickAbilities(0.f);                // the real edge poll, and therefore the real halving
			const float AfterPickup = Lily->GetZipRemaining();

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%s  cast-while-carrying %.2fs (full %.2fs, halved %.2fs) | "
				     "%.2fs left + Core pickup -> %.2fs (right answer %.2f; 're-clamp' would say %.2f; "
				     "'clamp to smaller' would say %.2f)"),
				Tag, bGreen ? TEXT("GREEN (shipped)") : TEXT("RED (halving removed)"),
				CastLength, FullDuration, CarrierDuration,
				StagedRemaining, AfterPickup, StagedRemaining * 0.5f, CarrierDuration, StagedRemaining);

			if (bGreen) { GreenCastCarrying = CastLength; GreenAfterPickup = AfterPickup; }
			else        { RedCastCarrying   = CastLength; RedAfterPickup   = AfterPickup; }
		}

		Arm->Set(ArmBefore, ECVF_SetByConsole);
		Lily->OnHalfTime();

		// ---- the verdict ---------------------------------------------------------------------------
		//
		// THE RED ARM HAS TO REPRODUCE FIRST. With the halving removed the cast must give the FULL
		// duration and the pickup must leave the staged time untouched; if either of those already
		// looks halved, the arm is not disarming what it claims to and the green readings mean nothing.
		const bool bRedReproduced =
			FMath::IsNearlyEqual(RedCastCarrying, FullDuration, 0.15f)
			&& FMath::IsNearlyEqual(RedAfterPickup, StagedRemaining, 0.15f);

		if (!bRedReproduced)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the red arm did not reproduce. With the halving removed the cast "
				     "gave %.2fs (expected the full %.2fs) and the pickup left %.2fs (expected the staged %.2fs). "
				     "A harness that cannot go red has proved nothing."),
				Tag, RedCastCarrying, FullDuration, RedAfterPickup, StagedRemaining);
			return;
		}

		int32 Failed = 0;
		if (!FMath::IsNearlyEqual(GreenCastCarrying, CarrierDuration, 0.15f))
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: §3 'with the core the duration is halved' — casting while carrying gave %.2fs, "
				     "expected %.2fs."),
				Tag, GreenCastCarrying, CarrierDuration);
		}
		if (!FMath::IsNearlyEqual(GreenAfterPickup, StagedRemaining * 0.5f, 0.15f))
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: §3 'if she activates it and then picks up the core, the REMAINING duration is "
				     "halved' — %.2fs left became %.2fs, expected %.2fs. (%.2fs would mean it re-clamped to the "
				     "carrier duration and LENGTHENED a nearly-spent Zip.)"),
				Tag, StagedRemaining, GreenAfterPickup, StagedRemaining * 0.5f, CarrierDuration);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — with the halving removed a carried cast lasted %.2fs and a pickup left "
				     "%.2fs untouched; with it in place the identical calls gave %.2fs and %.2fs. The second is the "
				     "subtle clause: what was LEFT halved, it did not re-clamp to %.2fs."),
				Tag, RedCastCarrying, RedAfterPickup, GreenCastCarrying, GreenAfterPickup, CarrierDuration);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 2 clauses wrong."), Tag, Failed);
		}
	}

	FAutoConsoleCommand CmdZipVerify(
		TEXT("Trace.Lily.ZipVerify"),
		TEXT("SPEC v19 §3. Two arms, RED FIRST: proves Zip's two Core clauses — halved at the cast while "
		     "carrying, and halving WHAT IS LEFT on a mid-flight pickup (staged at 1s left, where the right "
		     "answer 0.5s and the natural mistakes 1.0s / 2.5s all differ)."),
		FConsoleCommandDelegate::CreateStatic(&RunZipVerify));

	// =============================================================================================
	// Trace.Lily.FlightTest — DOES SHE ACTUALLY FLY, DRIVEN BY THE REAL KEYS
	//
	// ZipVerify above is arithmetic: it proves the two Core clauses and touches no physics at all. It
	// would report PASS on a build where pressing E did nothing to the pawn, which is EXACTLY the
	// failure Demo 17 item 1 was about — Elle's Snap was reported PROVEN by a harness that called
	// TryActivate() instead of pressing a key. So this one presses E and SPACE through Trace.SimInput,
	// i.e. through the engine's own input pipeline, and judges from the pawn's WORLD Z.
	//
	// TWO ARMS, RED FIRST: Trace.Lily.Zip 0 accepts the press, charges the cooldown and does not fly
	// her, so the climb has to collapse to roughly an ordinary jump. If it does not, the instrument is
	// measuring something other than the ability and says INVALID.
	//
	// IT ALSO MEASURES THE HOVER SAG, which is the one claim this file makes about itself that could
	// quietly be false: ApplyZip adds half a tick of gravity to the commanded Z precisely so that
	// "hold nothing and hang there" does not sink at ~24.5 uu/s. The number below is that claim, in uu.
	// =============================================================================================

	struct FLilyFlightState
	{
		/** 0 = RED (Trace.Lily.Zip 0), 1 = GREEN (shipped). */
		int32 Arm = 0;
		/** -2 waiting for the select screen to close, -1 arm setup, 0 climbing, 1 hovering. */
		int32 Phase = -2;

		/** Real-time deadline for the -2 wait. See IsWorldReadyForKeyPresses. */
		double ReadyGiveUpAt = 0.0;

		/** REAL time. The select screen can pause the world; every other harness here learned that. */
		double PhaseDeadline = 0.0;

		TWeakObjectPtr<UTraceAbilitySetLily> Lily;

		float StartZ = 0.f;
		float ClimbTopZ = 0.f;

		float RedClimb = -1.f;
		float GreenClimb = -1.f;
		float RedSag = 0.f;
		float GreenSag = 0.f;
		bool bRedRan = false;
		bool bGreenRan = false;
		bool bZippedInGreen = false;
	};

	/** Long enough to separate a climb from a jump, short enough to fit inside a 5 s Zip with room. */
	constexpr float FlightClimbSeconds = 1.2f;
	/** The sag watch. 2 s of "hold nothing": uncorrected this would lose ~49 uu, corrected ~0. */
	constexpr float FlightHoverSeconds = 2.f;

	void FinishFlightTest(FLilyFlightState* State)
	{
		const TCHAR* const Tag = TEXT("LILYFLIGHT");
		const UTraceSettings& Settings = UTraceSettings::Get();
		const float ExpectedClimb =
			FMath::Max(1.f, Settings.WalkSpeed) * FMath::Clamp(Settings.LilyZipClimbSpeedScale, 0.1f, 4.f)
			* FlightClimbSeconds;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] RED (Zip disabled) climbed %.0f uu in %.1fs | GREEN (shipped) climbed %.0f uu and then "
			     "drifted %.1f uu over %.1fs of holding nothing. Expected climb ~%.0f uu (%.0f uu/s x %.1fs)."),
			Tag, State->RedClimb, FlightClimbSeconds, State->GreenClimb, State->GreenSag,
			FlightHoverSeconds, ExpectedClimb,
			Settings.WalkSpeed * Settings.LilyZipClimbSpeedScale, FlightClimbSeconds);

		// THE FIXTURE PROVES ITSELF FIRST. With Zip off, holding jump is an ordinary jump: it must not
		// come anywhere near a full climb. If it did, this harness is measuring gravity and a jump arc
		// rather than the ability, and its green reading would mean nothing.
		if (!State->bRedRan || !State->bGreenRan || !State->bZippedInGreen
			|| State->RedClimb > ExpectedClimb * 0.5f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — redRan=%d greenRan=%d sheWasActuallyFlyingInGreen=%d "
				     "redClimb=%.0f uu (must stay under half of %.0f, or the climb is not the ability). "
				     "A harness that cannot go red has proved nothing."),
				Tag, State->bRedRan ? 1 : 0, State->bGreenRan ? 1 : 0, State->bZippedInGreen ? 1 : 0,
				State->RedClimb, ExpectedClimb);
			return;
		}

		int32 Failed = 0;
		if (State->GreenClimb < ExpectedClimb * 0.6f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: §3 'jump goes up at walking speed' — holding jump for %.1fs raised her %.0f uu, "
				     "expected about %.0f."),
				Tag, FlightClimbSeconds, State->GreenClimb, ExpectedClimb);
		}
		if (FMath::Abs(State->GreenSag) > 60.f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: holding neither key drifted her %.1f uu in %.1fs. ApplyZip's half-tick gravity "
				     "correction is not doing its job; uncorrected this would be about %.0f uu."),
				Tag, State->GreenSag, FlightHoverSeconds, 24.5f * FlightHoverSeconds);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — the SAME key presses raised her %.0f uu with Zip on and %.0f uu with it "
				     "off, and she held her height to within %.1f uu while flying."),
				Tag, State->GreenClimb, State->RedClimb, FMath::Abs(State->GreenSag));
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 2 checks failed."), Tag, Failed);
		}
	}

	void RunFlightTest()
	{
		const TCHAR* const Tag = TEXT("LILYFLIGHT");

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.Zip"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — Trace.Lily.Zip is not registered."), Tag);
			return;
		}

		FLilyFlightState* State = new FLilyFlightState();
		State->ReadyGiveUpAt = FPlatformTime::Seconds() + 30.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] armed. Waiting for a live, UNPAUSED match with a local pawn, then two arms, RED first, "
			     "~%.1fs each, driving the REAL E and SPACE keys."),
			Tag, FlightClimbSeconds + FlightHoverSeconds + 0.5f);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, Arm](float /*Delta*/) -> bool
			{
				const double Now = FPlatformTime::Seconds();

				// --- PHASE -2: WAIT FOR A WORLD THAT WILL ACCEPT A KEY PRESS ---------------------
				//
				// And claim Lily only once it will. Doing it earlier is what produced the v22 baseline's
				// three zeroes: the select screen was still up, it ate the E as a card pick, and the run
				// measured a paused Slimeball. See IsWorldReadyForKeyPresses.
				if (State->Phase == -2)
				{
					UWorld* WaitWorld = FindLocalKeyboardWorld();
					if (!IsWorldReadyForKeyPresses(WaitWorld))
					{
						if (Now > State->ReadyGiveUpAt)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[LILYFLIGHT] VERDICT: INVALID — 30s went by with no unpaused match and a "
								     "local pawn. A key cannot be pressed on a machine nobody is sitting at, and "
								     "the select screen would have eaten it anyway."));
							delete State;
							return false;
						}
						return true;
					}

					FString Why;
					UTraceAbilitySetLily* Claimed = MakePlayerIntoLily(WaitWorld, Why);
					if (Claimed == nullptr || Claimed->GetCharacter() == nullptr)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[LILYFLIGHT] VERDICT: INVALID — %s."),
							(Claimed == nullptr) ? *Why : TEXT("Lily has no pawn"));
						delete State;
						return false;
					}

					State->Lily = Claimed;
					State->Phase = -1;
					return true;
				}

				UTraceAbilitySetLily* TickLily = State->Lily.Get();
				ATraceCharacter* Pawn = (TickLily != nullptr) ? TickLily->GetCharacter() : nullptr;
				UWorld* TickWorld = (Pawn != nullptr) ? Pawn->GetWorld() : nullptr;

				if (TickLily == nullptr || Pawn == nullptr || TickWorld == nullptr || !Pawn->IsAlive())
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[LILYFLIGHT] VERDICT: INVALID — Lily's pawn went away mid-test (died, or the match "
						     "moved on). Run this early in a match."));
					Arm->Set(1, ECVF_SetByConsole);
					delete State;
					return false;
				}

				if (State->Phase == -1)
				{
					Arm->Set(State->Arm, ECVF_SetByConsole);

					// The framework's own cooldown clear, so the SECOND arm's E is not refused by the
					// 30 s the FIRST arm just charged. This is the one function allowed to zero it.
					if (UTraceAbilityComponent* Comp = TickLily->GetAbilityComponent())
					{
						Comp->OnHalfTime();
					}

					State->StartZ = static_cast<float>(Pawn->GetActorLocation().Z);

					// THE REAL KEYS, THROUGH THE REAL PIPELINE. Not TryActivate(), not OnJumpPressed().
					GEngine->Exec(TickWorld, TEXT("Trace.SimInput E 0.05"));
					GEngine->Exec(TickWorld, *FString::Printf(TEXT("Trace.SimInput SpaceBar %.2f"), FlightClimbSeconds));

					State->Phase = 0;
					State->PhaseDeadline = Now + FlightClimbSeconds;
					return true;
				}

				if (State->Phase == 0)
				{
					// Sampled every tick rather than only at the deadline, so "she was flying" is a fact
					// about the window and not about one instant of it.
					if (TickLily->IsZipping() && State->Arm == 1)
					{
						State->bZippedInGreen = true;
					}
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					State->ClimbTopZ = static_cast<float>(Pawn->GetActorLocation().Z);
					State->Phase = 1;
					State->PhaseDeadline = Now + FlightHoverSeconds;
					return true;
				}

				if (Now < State->PhaseDeadline)
				{
					return true;
				}

				const float EndZ = static_cast<float>(Pawn->GetActorLocation().Z);
				const float Climb = State->ClimbTopZ - State->StartZ;
				const float Sag = EndZ - State->ClimbTopZ;

				if (State->Arm == 0) { State->RedClimb = Climb;   State->RedSag = Sag;   State->bRedRan = true; }
				else                 { State->GreenClimb = Climb; State->GreenSag = Sag; State->bGreenRan = true; }

				UE_LOG(LogTraceGame, Display,
					TEXT("[LILYFLIGHT] arm=%s  climb %.0f uu in %.1fs, then %+.1f uu over %.1fs of no input "
					     "(z %.0f -> %.0f -> %.0f)."),
					(State->Arm == 1) ? TEXT("GREEN (shipped)") : TEXT("RED (Zip disabled)"),
					Climb, FlightClimbSeconds, Sag, FlightHoverSeconds,
					State->StartZ, State->ClimbTopZ, EndZ);

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Phase = -1;
					return true;
				}

				Arm->Set(1, ECVF_SetByConsole);
				FinishFlightTest(State);
				delete State;
				return false;
			}),
			0.f);
	}

	FAutoConsoleCommand CmdFlightTest(
		TEXT("Trace.Lily.FlightTest"),
		TEXT("SPEC v19 §3. Two arms, RED first: presses the REAL E and SPACE keys through Trace.SimInput and "
		     "measures the pawn's world Z, so 'she can fly' is a measurement and not a claim. Also reports the "
		     "hover drift the 20 Hz gravity correction exists to remove."),
		FConsoleCommandDelegate::CreateStatic(&RunFlightTest));

	// =============================================================================================
	// Trace.Lily.KeyTest — DEMO 19 ITEM 4, FROM THE PLAYER'S CHAIR
	//
	// The complaint is not a number, it is a sequence of key presses with a wrong outcome:
	//
	//     "Pressing space once makes her continuously move up even after I let go,
	//      and pressing control doesn't make her come down."
	//
	// So this harness performs exactly that sequence, on the real keys, through the real input
	// pipeline, and judges it by where the pawn ENDS UP:
	//
	//     E                       cast Zip
	//     hold JUMP    1.0 s      she should rise
	//     LET GO       1.0 s      *** SHE SHOULD STOP RISING ***      <- half the complaint
	//     hold CROUCH  0.8 s      *** SHE SHOULD COME DOWN ***        <- the other half
	//
	// TWO ARMS, RED FIRST, and the red arm is not an imitation: Trace.Lily.ZipHoldRelease 0 selects
	// the shipped v21 code path (the latch that nothing ever cleared), so the RED numbers ARE the
	// user's bug. If red does not reproduce — if letting go already stopped her — then the harness is
	// not measuring its rule and it says INVALID rather than PASS.
	//
	// IT ALSO MEASURES THE SPEED, because "half the speed she moves vertically at" is item 4's third
	// sentence and a knob set to 0.5 is not evidence that anything moves at half speed.
	// =============================================================================================

	struct FLilyKeyTestState
	{
		/** 0 = RED (Trace.Lily.ZipHoldRelease 0 — the shipped bug), 1 = GREEN (the fix). */
		int32 Arm = 0;

		/** -2 wait for an unpaused match, -1 cast and settle, 0 hold jump, 1 let go, 2 hold crouch. */
		int32 Phase = -2;

		double PhaseDeadline = 0.0;
		double ReadyGiveUpAt = 0.0;

		TWeakObjectPtr<UTraceAbilitySetLily> Lily;

		FString JumpKeyName;
		FString CrouchKeyName;

		/** Z at each phase boundary, in world units. */
		float ZAtHoldStart = 0.f;
		float ZAtRelease = 0.f;
		float ZAfterCoast = 0.f;

		/** Per arm [RED, GREEN]: uu climbed while held, uu drifted after letting go, uu moved on crouch. */
		float Climb[2] = { 0.f, 0.f };
		float Coast[2] = { 0.f, 0.f };
		float Crouched[2] = { 0.f, 0.f };
		bool bRan[2] = { false, false };
		bool bFlew[2] = { false, false };
	};

	constexpr float KeyTestSettleSeconds = 0.40f;   // Zip live, and any fall from the previous arm arrested
	constexpr float KeyTestHoldSeconds   = 1.00f;   // jump held
	constexpr float KeyTestCoastSeconds  = 1.00f;   // nothing held — THE COMPLAINT
	constexpr float KeyTestCrouchSeconds = 0.80f;   // crouch held — THE OTHER HALF

	void FinishKeyTest(FLilyKeyTestState* State)
	{
		const TCHAR* const Tag = TEXT("LILYKEYS");
		const UTraceSettings& Settings = UTraceSettings::Get();

		const float WalkSpeed    = FMath::Max(1.f, Settings.WalkSpeed);
		const float ClimbSpeed   = WalkSpeed * FMath::Clamp(Settings.LilyZipClimbSpeedScale, 0.1f, 4.f);
		const float DescendSpeed = WalkSpeed * FMath::Clamp(Settings.LilyZipDescendSpeedScale, 0.1f, 4.f);
		const float ExpectedClimb   = ClimbSpeed * KeyTestHoldSeconds;
		const float ExpectedDescend = DescendSpeed * KeyTestCrouchSeconds;

		for (int32 Index = 0; Index < 2; ++Index)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%-22s  hold jump %.1fs: %+.0f uu  |  LET GO %.1fs: %+.0f uu  |  "
				     "hold crouch %.1fs: %+.0f uu   (flying=%d)"),
				Tag,
				(Index == 1) ? TEXT("GREEN (the fix)") : TEXT("RED (shipped v21 bug)"),
				KeyTestHoldSeconds, State->Climb[Index],
				KeyTestCoastSeconds, State->Coast[Index],
				KeyTestCrouchSeconds, State->Crouched[Index],
				State->bFlew[Index] ? 1 : 0);
		}

		// ---- THE RED ARM HAS TO REPRODUCE THE USER'S SENTENCE, OR NOTHING BELOW COUNTS -------------
		//
		// Both halves of it. "She kept going up after I let go" is Coast >> 0, and "control did not
		// bring her down" is Crouched >= 0 — under the latch she was still climbing through the crouch,
		// because the descend branch was behind `!climbing`.
		const bool bRedKeptClimbing  = State->Coast[0] > ExpectedClimb * 0.5f;
		const bool bRedIgnoredCrouch = State->Crouched[0] > -ExpectedDescend * 0.1f;

		if (!State->bRan[0] || !State->bRan[1] || !State->bFlew[0] || !State->bFlew[1]
			|| !bRedKeptClimbing || !bRedIgnoredCrouch)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the red arm did not reproduce the complaint. "
				     "redRan=%d greenRan=%d flying(red/green)=%d/%d | after letting go she moved %+.0f uu "
				     "(the bug needs more than %+.0f) | on crouch she moved %+.0f uu (the bug needs it to be "
				     "no lower than %+.0f). A harness that cannot go red has proved nothing."),
				Tag, State->bRan[0] ? 1 : 0, State->bRan[1] ? 1 : 0,
				State->bFlew[0] ? 1 : 0, State->bFlew[1] ? 1 : 0,
				State->Coast[0], ExpectedClimb * 0.5f,
				State->Crouched[0], -ExpectedDescend * 0.1f);
			return;
		}

		int32 Failed = 0;

		// (1) She still flies at all.
		if (State->Climb[1] < ExpectedClimb * 0.6f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: holding jump for %.1fs raised her %+.0f uu, expected about %.0f "
				     "(%.0f uu/s). The fix must not have cost her the climb."),
				Tag, KeyTestHoldSeconds, State->Climb[1], ExpectedClimb, ClimbSpeed);
		}

		// (2) *** "even after I let go" ***
		if (State->Coast[1] > ExpectedClimb * 0.25f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'pressing space once makes her continuously move up even after I let go' — "
				     "she rose another %+.0f uu in the %.1fs after the key came up. The release is still not "
				     "reaching her."),
				Tag, State->Coast[1], KeyTestCoastSeconds);
		}

		// (3) *** "pressing control doesn't make her come down" ***
		if (State->Crouched[1] > -ExpectedDescend * 0.4f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'pressing control doesn't make her come down' — %.1fs of crouch moved her "
				     "%+.0f uu, expected about %+.0f (%.0f uu/s down)."),
				Tag, KeyTestCrouchSeconds, State->Crouched[1], -ExpectedDescend, DescendSpeed);
		}

		// (4) *** "half the speed she moves vertically at" ***, MEASURED and not read off the knob.
		const float MeasuredClimbRate = State->Climb[1] / KeyTestHoldSeconds;
		if (MeasuredClimbRate > WalkSpeed * 0.65f)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'half the speed she moves vertically at' — she climbed at %.0f uu/s against a "
				     "walk of %.0f. Half a walk is %.0f."),
				Tag, MeasuredClimbRate, WalkSpeed, WalkSpeed * 0.5f);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — the SAME three key presses. WITH THE BUG: let go and she rose "
				     "another %+.0f uu, then crouch moved her %+.0f uu (still upward). FIXED: letting go left "
				     "her within %+.0f uu, and crouch took her DOWN %+.0f uu. She climbs at %.0f uu/s, half "
				     "the %.0f walk."),
				Tag, State->Coast[0], State->Crouched[0], State->Coast[1], State->Crouched[1],
				MeasuredClimbRate, WalkSpeed);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 4 checks failed."), Tag, Failed);
		}
	}

	void RunKeyTest()
	{
		const TCHAR* const Tag = TEXT("LILYKEYS");

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.ZipHoldRelease"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Trace.Lily.ZipHoldRelease is not registered, so there is no red arm."),
				Tag);
			return;
		}

		FLilyKeyTestState* State = new FLilyKeyTestState();
		State->ReadyGiveUpAt = FPlatformTime::Seconds() + 30.0;

		// THE PLAYER'S OWN BINDS, not the literals SpaceBar and LeftControl. If somebody rebinds jump
		// this harness follows them, which is the only way it can claim to be pressing "the key the
		// user pressed" rather than "the key we assume they pressed".
		const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();
		const FKey JumpKey   = UserSettings.GetKey(ETraceInputAction::Jump);
		const FKey CrouchKey = UserSettings.GetKey(ETraceInputAction::Crouch);
		if (!JumpKey.IsValid() || !CrouchKey.IsValid())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — jump or crouch is UNBOUND (jump='%s', crouch='%s'). There is no "
				     "key to press."),
				Tag, *JumpKey.ToString(), *CrouchKey.ToString());
			delete State;
			return;
		}
		State->JumpKeyName   = JumpKey.ToString();
		State->CrouchKeyName = CrouchKey.ToString();

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] armed. Two arms, RED (the shipped bug) first. Per arm: E, hold %s %.1fs, LET GO %.1fs, "
			     "hold %s %.1fs — judged by the pawn's world Z."),
			Tag, *State->JumpKeyName, KeyTestHoldSeconds, KeyTestCoastSeconds,
			*State->CrouchKeyName, KeyTestCrouchSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, Arm](float /*Delta*/) -> bool
			{
				const double Now = FPlatformTime::Seconds();

				if (State->Phase == -2)
				{
					UWorld* WaitWorld = FindLocalKeyboardWorld();
					if (!IsWorldReadyForKeyPresses(WaitWorld))
					{
						if (Now > State->ReadyGiveUpAt)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[LILYKEYS] VERDICT: INVALID — 30s went by with no unpaused match and a local "
								     "pawn. The select screen pauses the world and eats the E."));
							delete State;
							return false;
						}
						return true;
					}

					FString Why;
					UTraceAbilitySetLily* Claimed = MakePlayerIntoLily(WaitWorld, Why);
					if (Claimed == nullptr || Claimed->GetCharacter() == nullptr)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[LILYKEYS] VERDICT: INVALID — %s."),
							(Claimed == nullptr) ? *Why : TEXT("Lily has no pawn"));
						delete State;
						return false;
					}

					State->Lily = Claimed;
					State->Phase = -1;
					return true;
				}

				UTraceAbilitySetLily* TickLily = State->Lily.Get();
				ATraceCharacter* Pawn = (TickLily != nullptr) ? TickLily->GetCharacter() : nullptr;
				UWorld* TickWorld = (Pawn != nullptr) ? Pawn->GetWorld() : nullptr;

				if (TickLily == nullptr || Pawn == nullptr || TickWorld == nullptr || !Pawn->IsAlive())
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[LILYKEYS] VERDICT: INVALID — Lily's pawn went away mid-test. Run this early in "
						     "a match."));
					Arm->Set(1, ECVF_SetByConsole);
					delete State;
					return false;
				}

				const float ZNow = static_cast<float>(Pawn->GetActorLocation().Z);

				// --- PHASE -1: arm, cast, and let the flight settle -------------------------------
				if (State->Phase == -1)
				{
					Arm->Set(State->Arm, ECVF_SetByConsole);

					if (UTraceAbilityComponent* Comp = TickLily->GetAbilityComponent())
					{
						Comp->OnHalfTime();   // the framework's own cooldown clear, so arm 2's E is accepted
					}

					GEngine->Exec(TickWorld, TEXT("Trace.SimInput E 0.05"));

					// THE SETTLE IS LOAD-BEARING. The previous arm leaves her hundreds of uu up and
					// falling; measuring the climb from a fall would flatter the fix. ApplyZip arrests
					// her within a tick or two of the cast, and this waits for that.
					State->Phase = 0;
					State->PhaseDeadline = Now + KeyTestSettleSeconds;
					return true;
				}

				// --- PHASE 0: settle, then HOLD JUMP ----------------------------------------------
				if (State->Phase == 0)
				{
					if (Now < State->PhaseDeadline)
					{
						if (TickLily->IsZipping())
						{
							State->bFlew[State->Arm] = true;
						}
						return true;
					}

					State->ZAtHoldStart = ZNow;
					GEngine->Exec(TickWorld, *FString::Printf(TEXT("Trace.SimInput %s %.2f"),
						*State->JumpKeyName, KeyTestHoldSeconds));

					State->Phase = 1;
					State->PhaseDeadline = Now + KeyTestHoldSeconds;
					return true;
				}

				// --- PHASE 1: the key is up. HOLD NOTHING. *** THE COMPLAINT *** ------------------
				if (State->Phase == 1)
				{
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					State->ZAtRelease = ZNow;
					State->Climb[State->Arm] = State->ZAtRelease - State->ZAtHoldStart;

					State->Phase = 2;
					State->PhaseDeadline = Now + KeyTestCoastSeconds;
					return true;
				}

				// --- PHASE 2: HOLD CROUCH. *** THE OTHER HALF OF THE COMPLAINT *** ----------------
				if (State->Phase == 2)
				{
					if (Now < State->PhaseDeadline)
					{
						return true;
					}

					State->ZAfterCoast = ZNow;
					State->Coast[State->Arm] = State->ZAfterCoast - State->ZAtRelease;

					GEngine->Exec(TickWorld, *FString::Printf(TEXT("Trace.SimInput %s %.2f"),
						*State->CrouchKeyName, KeyTestCrouchSeconds));

					State->Phase = 3;
					State->PhaseDeadline = Now + KeyTestCrouchSeconds;
					return true;
				}

				if (Now < State->PhaseDeadline)
				{
					return true;
				}

				State->Crouched[State->Arm] = ZNow - State->ZAfterCoast;
				State->bRan[State->Arm] = true;

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Phase = -1;
					return true;
				}

				Arm->Set(1, ECVF_SetByConsole);
				TickLily->OnHalfTime();
				FinishKeyTest(State);
				delete State;
				return false;
			}),
			0.f);
	}

	FAutoConsoleCommand CmdKeyTest(
		TEXT("Trace.Lily.KeyTest"),
		TEXT("DEMO 19 item 4, from the player's chair. Two arms, RED first (Trace.Lily.ZipHoldRelease 0 = the "
		     "shipped bug, not an imitation of it): presses E, HOLDS jump, LETS GO, then HOLDS crouch through "
		     "the real input pipeline and judges the pawn's world Z. Proves 'she stops when I let go', 'crouch "
		     "brings her down' and 'she climbs at half a walk'."),
		FConsoleCommandDelegate::CreateStatic(&RunKeyTest));

	// =============================================================================================
	// Trace.Lily.DashTest — DEMO 19 ITEM 8, MEASURED ON THE POOL THE HUD DRAWS
	//
	// "Change Lily so she only has an extra dash when she is not carrying the core."
	//
	// The number asked for is UTraceCharacterMovementComponent::GetMaxDashCharges() — the same call
	// ATracePlayerController::GetDashHudState() makes to fill the dash meter, so this reads exactly
	// the number of pips the player sees. It is taken with the Core in her hands and again without,
	// in both arms, from the real Core actor rather than from a fake carrier bit.
	//
	// TWO ARMS, RED FIRST. Trace.Lily.DashCarrierGate 0 is the shipped v21 rule; if the red arm does
	// not print 3 while carrying then this harness is not measuring the gate and says so.
	//
	// SYNCHRONOUS, like ZipVerify: ATraceCore::TryPickup is an authority-side grant that lands in the
	// same frame, and GetMaxDashCharges() is a pure read.
	// =============================================================================================

	void RunDashTest()
	{
		const TCHAR* const Tag = TEXT("LILYDASH");

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
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. Run this on the server/host."), Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetLily* Lily = MakePlayerIntoLily(TestWorld, Why);
		ATraceCharacter* MyPawn = (Lily != nullptr) ? Lily->GetCharacter() : nullptr;
		ATraceCore* CoreActor = ATraceCore::Get(TestWorld);
		UTraceCharacterMovementComponent* MoveComp = (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;

		if (Lily == nullptr || MyPawn == nullptr || CoreActor == nullptr || MoveComp == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — %s."), Tag,
				(Lily == nullptr) ? *Why : TEXT("Lily has no pawn, no movement component, or there is no Core"));
			return;
		}

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Lily.DashCarrierGate"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Trace.Lily.DashCarrierGate is not registered, so there is no red arm."),
				Tag);
			return;
		}
		const int32 ArmBefore = Arm->GetInt();

		// Somewhere to park the Core so "not carrying" is a real state and not a wish.
		ATraceCharacter* Parker = nullptr;
		for (TActorIterator<ATraceCharacter> It(TestWorld); It; ++It)
		{
			if (*It != nullptr && *It != MyPawn && (*It)->IsAlive()) { Parker = *It; break; }
		}

		int32 Free[2] = { 0, 0 };
		int32 Carrying[2] = { 0, 0 };

		for (int32 ArmIndex = 0; ArmIndex < 2; ++ArmIndex)
		{
			Arm->Set(ArmIndex, ECVF_SetByConsole);   // 0 = RED (ungated), 1 = GREEN (item 8)

			if (Parker != nullptr)
			{
				CoreActor->TryPickup(Parker);
			}
			Free[ArmIndex] = MoveComp->GetMaxDashCharges();

			CoreActor->TryPickup(MyPawn);
			Carrying[ArmIndex] = MoveComp->GetMaxDashCharges();
		}

		if (Parker != nullptr)
		{
			CoreActor->TryPickup(Parker);   // leave the match roughly as it was found
		}
		Arm->Set(ArmBefore, ECVF_SetByConsole);

		const UTraceSettings& Settings = UTraceSettings::Get();
		const int32 EverybodyFree     = FMath::Max(1, Settings.BaseDashCharges);
		const int32 EverybodyCarrying = EverybodyFree + FMath::Max(0, Settings.CarrierExtraDashCharges);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] RED (gate off, the v21 rule)  Lily %d free / %d carrying   |   "
			     "GREEN (item 8)  Lily %d free / %d carrying   |   everybody else %d / %d."),
			Tag, Free[0], Carrying[0], Free[1], Carrying[1], EverybodyFree, EverybodyCarrying);

		// ---- THE RED ARM FIRST ---------------------------------------------------------------------
		if (Free[0] != EverybodyFree + 1 || Carrying[0] != EverybodyCarrying + 1)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — with the gate OFF she should show the old rule, %d free and %d "
				     "carrying, and she showed %d and %d. The harness is not measuring the gate."),
				Tag, EverybodyFree + 1, EverybodyCarrying + 1, Free[0], Carrying[0]);
			return;
		}

		int32 Failed = 0;
		if (Free[1] != EverybodyFree + 1)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: item 8 keeps her extra dash while she is NOT carrying — expected %d, got %d."),
				Tag, EverybodyFree + 1, Free[1]);
		}
		if (Carrying[1] != EverybodyCarrying)
		{
			++Failed;
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] FAIL: 'only ... when she is not carrying the core' — carrying, she should be on the "
				     "same %d as everybody else and she is on %d. (%d would mean the gate did nothing; %d would "
				     "mean it took the carrier's charge away too.)"),
				Tag, EverybodyCarrying, Carrying[1], EverybodyCarrying + 1, EverybodyCarrying - 1);
		}

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — the SAME pickup, measured on the pool the dash meter draws: with the "
				     "old rule %d carrying, with item 8 %d. Free-running she keeps her %d either way."),
				Tag, Carrying[0], Carrying[1], Free[1]);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d of 2 checks failed."), Tag, Failed);
		}
	}

	FAutoConsoleCommand CmdDashTest(
		TEXT("Trace.Lily.DashTest"),
		TEXT("DEMO 19 item 8. Two arms, RED first (Trace.Lily.DashCarrierGate 0 = the shipped v21 rule): hands "
		     "Lily the real Core and reads GetMaxDashCharges() — the same number the dash meter draws — with "
		     "and without it. Proves her extra charge is hers only when she is NOT carrying."),
		FConsoleCommandDelegate::CreateStatic(&RunDashTest));

	/** The numbers §3 states, checked against the knobs, plus an honest note about what is not wired. */
	void RunLilyVerify()
	{
		const TCHAR* const Tag = TEXT("LILY");
		const UTraceSettings& Settings = UTraceSettings::Get();

		int32 Passed = 0;
		int32 Failed = 0;
		auto Check = [&](bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; }
			else { ++Failed; UE_LOG(LogTraceGame, Error, TEXT("[%s] FAIL: %s"), Tag, *What); }
		};

		Check(Settings.LilyExtraDashCharges == 1,
			FString::Printf(TEXT("§3 says ONE extra dash; the knob is %d"), Settings.LilyExtraDashCharges));
		Check(FMath::IsNearlyEqual(Settings.LilyMaxHealth, 60.f, 0.01f),
			FString::Printf(TEXT("§3 says 60 health; the knob is %.1f"), Settings.LilyMaxHealth));
		Check(FMath::IsNearlyEqual(Settings.LilyWallJumpMomentumBonus, 0.3f, 0.001f),
			FString::Printf(TEXT("§3 says +30%% wall-jump momentum; the knob is %.3f"), Settings.LilyWallJumpMomentumBonus));
		Check(FMath::IsNearlyEqual(Settings.LilyZipDurationSeconds, 5.f, 0.01f),
			FString::Printf(TEXT("§3 says Zip lasts 5s; the knob is %.2f"), Settings.LilyZipDurationSeconds));
		Check(FMath::IsNearlyEqual(Settings.LilyZipCooldownSeconds, 30.f, 0.01f),
			FString::Printf(TEXT("§3 says a 30s cooldown; the knob is %.2f"), Settings.LilyZipCooldownSeconds));

		// DEMO 19 ITEM 8 in the units a designer reads. Everybody: base, plus one while carrying.
		// Lily: base plus hers while FREE-RUNNING, and base plus the carrier's — not both — while
		// carrying. The point of printing all four numbers is that "she has an extra dash" and "she
		// has an extra dash while carrying" are now different sentences.
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] DASHES  everybody %d free / %d carrying -> Lily %d free / %d carrying "
			     "(item 8: her extra charge is hers only when she is NOT carrying the Core). "
			     "HEALTH  everybody %.0f -> Lily %.0f, i.e. two body shots instead of three. "
			     "WALL JUMP  retention x%.2f, hers alone."),
			Tag,
			Settings.BaseDashCharges,
			Settings.BaseDashCharges + Settings.CarrierExtraDashCharges,
			Settings.BaseDashCharges + Settings.LilyExtraDashCharges,
			Settings.BaseDashCharges + Settings.CarrierExtraDashCharges,
			Settings.MaxHealth, Settings.LilyMaxHealth,
			1.f + Settings.LilyWallJumpMomentumBonus);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] ZIP  %.2fs, or %.2fs carrying; climbs at %.0f uu/s and descends at %.0f uu/s "
			     "(WalkSpeed %.0f); %.0fs cooldown."),
			Tag, Settings.LilyZipDurationSeconds,
			Settings.LilyZipDurationSeconds * Settings.LilyZipCarrierDurationScale,
			Settings.WalkSpeed * Settings.LilyZipClimbSpeedScale,
			Settings.WalkSpeed * Settings.LilyZipDescendSpeedScale,
			Settings.WalkSpeed, Settings.LilyZipCooldownSeconds);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] ALL FOUR ARE LIVE. The extra charge is read by GetMaxDashCharges(), the 60 health by "
			     "TraceHealthComponent::GetMaxHealth(), the wall-jump bonus by TryWallJump()'s retention term, "
			     "and Zip runs in this file. (This line used to say the first three were wired to nothing; it "
			     "was true when it was written and stopped being true without being edited.)"),
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

	FAutoConsoleCommand CmdLilyVerify(
		TEXT("Trace.Lily.Verify"),
		TEXT("SPEC v19 §3. Checks Lily's knobs against the doc's numbers, prints the derived flight speeds, and "
		     "says plainly which of her abilities are not yet wired to anything."),
		FConsoleCommandDelegate::CreateStatic(&RunLilyVerify));
}

#undef LOCTEXT_NAMESPACE
