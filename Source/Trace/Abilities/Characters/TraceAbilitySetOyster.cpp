// Trace — OYSTER. See the header for the four carrier-rule vectors and where each is enforced.

#include "Abilities/Characters/TraceAbilitySetOyster.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceAbilityInputRelay.h"
#include "Abilities/Characters/TraceOysterJar.h"
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// SPEC v19 §4.3 — THE TEST ARM FOR "THE JAR SPAWNS AT THE END OF HIS DASH"
//
// 0 (shipped): the dash drops its jar where it ENDS.
// 1:           restores the pre-v19 drop at the START, so Trace.Oyster.DashJarTest can be shown
//              failing on the same dash, over the same ground, in the same process. NEVER SHIP 1.
// =================================================================================================
static TAutoConsoleVariable<int32> CVarOysterLegacyDashJarAtStart(
	TEXT("Trace.Oyster.LegacyDashJarAtStart"), 0,
	TEXT("TEST ARM ONLY. 0 (shipped, spec v19 §4.3): Oyster's dash jar is dropped where the dash ENDS. "
	     "1: restores the pre-v19 drop at the start so Trace.Oyster.DashJarTest can be shown failing. "
	     "Never ship 1."),
	ECVF_Cheat);

namespace
{
	/**
	 * Seconds after a jar jump in which a second one is refused.
	 *
	 * NOT a cooldown on the ability — it is a latch between the two routes that can notice a jump
	 * (the framework's OnJumpPressed hook, and this file's own ground-state poll that backs it up).
	 * Both must be live, because either may be the only one running, and without the latch a single
	 * press could boost twice on the frame they overlap. Short enough that it cannot refuse a genuine
	 * second jump: nobody lands and re-jumps in a fifth of a second.
	 */
	constexpr float JarJumpLatchSeconds = 0.2f;
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetOyster::OnEquipped()
{
	LiveJars.Reset();
	bWasDashing = false;
	bDashTracked = false;
	bDashJarOwedForThisDash = false;
	bWasOnGround = false;
	LastJarJumpMatchTime = -100.f;

	if (HasAuthority())
	{
		if (UTraceAbilityComponent* Comp = GetAbilityComponent())
		{
			UTraceAbilityInputRelay::EnsureOn(Comp->GetOwningPlayerState());
		}
		PublishState();
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	UE_LOG(LogTraceGame, Log,
		TEXT("[Oyster] Equipped. jars %.1fs x%d, break %.0f uu | poison %.0f/%.2fs for %.1fs (-%.0f%% speed) in %.0f uu "
		     "| jar-jump %.0f uu/s within %.0f uu | Pickler %.0f dmg in %.0f uu, pull %.0f uu/s in %.0f uu, cooldown %.0fs "
		     "[ASSUMPTION: §6 unspecified] | SPEC v26 §6: the Pickler jar detonates %.2fs after it lands (x%.2f of the "
		     "pull's own travel time), and ANY poison landing on an enemy resets that %.0fs cooldown to zero."),
		Settings.OysterJarLifetimeSeconds, Settings.OysterMaxJars, Settings.OysterJarBreakRadiusUU,
		Settings.OysterPoisonDamagePerTick, Settings.OysterPoisonTickIntervalSeconds,
		Settings.OysterPoisonDurationSeconds, Settings.OysterPoisonSlowFraction * 100.f, Settings.OysterPoisonRadiusUU,
		Settings.OysterJarJumpZVelocity, Settings.OysterJarJumpRadiusUU,
		Settings.OysterPicklerDamage, Settings.OysterPicklerDamageRadiusUU,
		Settings.OysterPicklerPullSpeed, Settings.OysterPicklerPullRadiusUU,
		GetActivatedCooldownSeconds(),
		ATraceOysterJar::GetPicklerDetonateDelaySeconds(), Settings.OysterPicklerDetonateDelayScale,
		GetActivatedCooldownSeconds());
}

void UTraceAbilitySetOyster::OnUnequipped()
{
	DestroyAllJars();
}

void UTraceAbilitySetOyster::OnPawnDied()
{
	// Cooldowns are untouched — §5. His jars are not: a jar is a thing he put in the world and a
	// corpse should not keep poisoning people for four more seconds.
	DestroyAllJars();
	bWasDashing = false;
	bDashTracked = false;
	bDashJarOwedForThisDash = false;
	bWasOnGround = false;
}

void UTraceAbilitySetOyster::OnHalfTime()
{
	DestroyAllJars();
}

bool UTraceAbilitySetOyster::ShouldDriveMovement() const
{
	return HasAuthority() || IsLocallyControlled();
}

// =================================================================================================
// Jar bookkeeping — "max 3; a fourth despawns the oldest"
// =================================================================================================

int32 UTraceAbilitySetOyster::GetLiveJarCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<ATraceOysterJar>& Entry : LiveJars)
	{
		if (Entry.IsValid())
		{
			++Count;
		}
	}
	return Count;
}

void UTraceAbilitySetOyster::PruneJars()
{
	LiveJars.RemoveAll([](const TWeakObjectPtr<ATraceOysterJar>& Entry) { return !Entry.IsValid(); });

	const int32 MaxJars = FMath::Max(1, UTraceSettings::Get().OysterMaxJars);
	while (LiveJars.Num() > MaxJars)
	{
		// FRONT, not back: the array is in spawn order, so the front is the oldest. "A fourth
		// despawns the oldest" — and it DESPAWNS rather than breaks, so no poison burst here. A jar
		// that vanishes because the player threw another one should not also poison the room.
		//
		// SPEC v19 §4.4. THE OLDEST JAR STILL IN THE AIR IS SKIPPED. Only a Pickler is ever airborne
		// (a dash jar lands on the frame it is dropped), and evicting one mid-flight deleted a throw a
		// player had already spent a 20 s cooldown on — the ability simply never happened, which is
		// the loudest of the three measured causes of "inconsistent to throw". The oldest jar that has
		// actually LANDED goes instead; if every live jar is somehow airborne the rule falls back to
		// the plain oldest, because the cap has to be enforced either way.
		int32 VictimIndex = 0;
		if (!TraceOysterJar::IsLegacyThrow())
		{
			for (int32 Index = 0; Index < LiveJars.Num(); ++Index)
			{
				const ATraceOysterJar* Candidate = LiveJars[Index].Get();
				if (Candidate == nullptr || Candidate->IsGrounded())
				{
					VictimIndex = Index;
					break;
				}
			}
		}

		TWeakObjectPtr<ATraceOysterJar> Victim = LiveJars[VictimIndex];
		LiveJars.RemoveAt(VictimIndex);
		if (ATraceOysterJar* JarActor = Victim.Get())
		{
			UE_LOG(LogTraceGame, Verbose,
				TEXT("[Oyster] Jar cap %d reached — despawning the oldest LANDED jar (index %d)."),
				MaxJars, VictimIndex);
			JarActor->Destroy();
		}
	}
}

void UTraceAbilitySetOyster::DestroyAllJars()
{
	if (HasAuthority())
	{
		for (const TWeakObjectPtr<ATraceOysterJar>& Entry : LiveJars)
		{
			if (ATraceOysterJar* JarActor = Entry.Get())
			{
				JarActor->Destroy();
			}
		}
	}
	LiveJars.Reset();

	if (HasAuthority())
	{
		PublishState();
	}
}

ATraceOysterJar* UTraceAbilitySetOyster::SpawnJar(const FVector& SpawnLocation, const FVector& LaunchVelocity, bool bPickler)
{
	if (!HasAuthority())
	{
		return nullptr;
	}

	UWorld* WorldPtr = GetWorld();
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceAbilityComponent* Comp = GetAbilityComponent();
	if (WorldPtr == nullptr || MyPawn == nullptr || Comp == nullptr)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = MyPawn;
	SpawnParams.Instigator = MyPawn;

	ATraceOysterJar* JarActor = WorldPtr->SpawnActor<ATraceOysterJar>(
		ATraceOysterJar::StaticClass(), SpawnLocation, FRotator::ZeroRotator, SpawnParams);
	if (JarActor == nullptr)
	{
		return nullptr;
	}

	JarActor->Initialise(Comp, MyPawn->GetTeam(), bPickler, LaunchVelocity);

	LiveJars.Add(JarActor);
	PruneJars();
	PublishState();

	return JarActor;
}

ATraceOysterJar* UTraceAbilitySetOyster::FindOwnJarNear(const FVector& Location) const
{
	const float Radius = FMath::Max(1.f, UTraceSettings::Get().OysterJarJumpRadiusUU);

	ATraceOysterJar* Best = nullptr;
	float BestDistance = MAX_flt;

	for (const TWeakObjectPtr<ATraceOysterJar>& Entry : LiveJars)
	{
		ATraceOysterJar* JarActor = Entry.Get();
		if (JarActor == nullptr || !JarActor->IsGrounded())
		{
			continue;
		}
		const float Distance = FVector::Dist(JarActor->GetActorLocation(), Location);
		if (Distance <= Radius && Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = JarActor;
		}
	}
	return Best;
}

// =================================================================================================
// PASSIVE — a jar at the END of every dash (spec v19 §4.3)
// =================================================================================================

bool UTraceAbilitySetOyster::OnDashStarted(const FVector& DashDirection)
{
	NoteDashBegan();
	return false;   // never cancels the dash
}

void UTraceAbilitySetOyster::OnDashEnded(bool bReachedFullDistance)
{
	DropOwedDashJar();
	bDashTracked = false;
}

void UTraceAbilitySetOyster::NoteDashBegan()
{
	// The hook and the poll both call this and either may be first, so a dash already being tracked
	// is not a second dash. Without this the two routes would arm the debt twice on the frame they
	// overlap, and the red arm below would drop two jars for one dash.
	if (!HasAuthority() || bDashTracked)
	{
		return;
	}

	// SPEC v19 §4.3: the dash no longer DROPS the jar when it starts, it only owes one. The drop is at
	// the far end, so the jar covers where he arrives instead of where he left.
	bDashTracked = true;
	bDashJarOwedForThisDash = true;

	if (CVarOysterLegacyDashJarAtStart.GetValueOnAnyThread() != 0)
	{
		DropOwedDashJar();   // the red arm, drop-at-the-start, exactly as before v19
	}
}

void UTraceAbilitySetOyster::DropOwedDashJar()
{
	if (!HasAuthority() || !bDashJarOwedForThisDash)
	{
		return;
	}

	// Cleared FIRST, unconditionally. If the drop is refused (he died on the last frame of the dash)
	// the debt is still settled, or the next tick's poll would try again on a corpse.
	bDashJarOwedForThisDash = false;
	DebugDropDashJar();
}

ATraceOysterJar* UTraceAbilitySetOyster::DebugDropDashJar()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		return nullptr;
	}

	// At his FEET, not at his origin: a jar spawned at capsule centre floats at chest height and the
	// break radius then measures from the wrong place.
	float FootOffset = 0.f;
	if (const UCapsuleComponent* Capsule = MyPawn->GetCapsuleComponent())
	{
		FootOffset = Capsule->GetScaledCapsuleHalfHeight();
	}

	return SpawnJar(MyPawn->GetActorLocation() - FVector(0.f, 0.f, FootOffset),
		FVector::ZeroVector, /*bPickler*/ false);
}

// =================================================================================================
// MOVEMENT — the jar jump
// =================================================================================================

bool UTraceAbilitySetOyster::OnJumpPressed()
{
	ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || MoveComp == nullptr || !MoveComp->IsMovingOnGround())
	{
		return false;   // "while STOOD on one of his jars"
	}

	return DoJarJump(MyPawn->GetActorLocation());
}

bool UTraceAbilitySetOyster::DebugTryJarJump()
{
	ATraceCharacter* MyPawn = GetCharacter();
	return (MyPawn != nullptr) && DoJarJump(MyPawn->GetActorLocation());
}

bool UTraceAbilitySetOyster::DoJarJump(const FVector& FromLocation)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		return false;
	}

	const float Now = MatchTimeNow();
	if ((Now - LastJarJumpMatchTime) < JarJumpLatchSeconds)
	{
		return false;   // the other route already took this press
	}

	// The jar list is server-side only, so the owning client cannot find its own jar this way. It
	// still has to APPLY the boost or the jump would be a round trip late and then corrected; the
	// server's break is what makes it authoritative. See the report.
	if (HasAuthority())
	{
		ATraceOysterJar* JarActor = FindOwnJarNear(FromLocation);
		if (JarActor == nullptr)
		{
			return false;
		}

		LiveJars.RemoveAll([JarActor](const TWeakObjectPtr<ATraceOysterJar>& Entry) { return Entry.Get() == JarActor; });

		// "BREAKS it" — the same break an enemy's touch produces, poison burst and all. §6 gives the
		// jar one break, not two kinds. [ASSUMPTION] flagged in the report: the doc does not say
		// whether Oyster's own jar-jump poisons nearby enemies, and this says yes because "breaks it"
		// is the same verb.
		JarActor->ServerBreakNow(TEXT("Oyster's jar jump"));
		PublishState();
	}

	LastJarJumpMatchTime = Now;

	if (ShouldDriveMovement())
	{
		// "BOOSTS HIM UPWARD." An absolute Z, not an addition: the boost replaces the jump rather
		// than compounding with whatever vertical speed he happened to have.
		const float BoostZ = FMath::Max(0.f, UTraceSettings::Get().OysterJarJumpZVelocity);
		if (MoveComp->IsMovingOnGround())
		{
			MoveComp->SetMovementMode(MOVE_Falling);
		}
		MoveComp->Velocity.Z = FMath::Max(MoveComp->Velocity.Z, BoostZ);
	}

	if (HasAuthority())
	{
		UE_LOG(LogTraceGame, Log, TEXT("[Oyster] Jar jump: broke his own jar and launched at %.0f uu/s."),
			UTraceSettings::Get().OysterJarJumpZVelocity);
	}
	return true;
}

// =================================================================================================
// ACTIVATED — Pickler
// =================================================================================================

float UTraceAbilitySetOyster::GetActivatedCooldownSeconds() const
{
	// [ASSUMPTION] §6 leaves it unspecified; 20 s to match the others, as a knob.
	//
	// SPEC v26 §6a MADE THIS A CEILING RATHER THAN A WAIT. It is still what the framework charges on
	// activation and still what the card prints — but poisoning any enemy clears it outright, from
	// UTraceOysterPoisonComponent::ApplyTo. Nothing changes here, and that is deliberate: the refund
	// is an event that happens to a running cooldown, not a different cooldown length, so the one
	// number the HUD ring and the card share stays the one number the framework charges.
	return FMath::Max(0.f, UTraceSettings::Get().OysterPicklerCooldownSeconds);
}

ATraceOysterJar* UTraceAbilitySetOyster::ThrowPickler()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (!HasAuthority() || MyPawn == nullptr || !MyPawn->IsAlive())
	{
		return nullptr;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Speed = FMath::Max(1.f, Settings.OysterPicklerThrowSpeed);
	const float UpBias = FMath::Max(0.f, Settings.OysterPicklerThrowUpBias);

	// A LOB, not a shot: the up bias is a fraction of the throw speed, so retuning the speed
	// keeps the same arc shape rather than flattening it.
	const FVector LaunchVelocity = MyPawn->GetAimDirection() * Speed + FVector(0.f, 0.f, Speed * UpBias);

	// SPEC v19 §4.4. The muzzle is 22 uu along the aim ray from the eye and the jar's own sphere is 18,
	// so a release AT the muzzle can be inside the world — not at a wall, where the 34 uu capsule keeps
	// him too far away for it to matter, but under a CEILING, which may sit as little as 24 uu above
	// the eye. A swept move that starts penetrating is refused at Time 0, so the jar was born buried in
	// the roof he was standing under. This resolves a release point the jar actually fits in first.
	// Measured both ways by Trace.Oyster.PicklerThrowTest; it is a small fix and the harness says so.
	const FVector DesiredRelease = MyPawn->GetMuzzleLocation();
	const FVector Release = ATraceOysterJar::ResolveReleaseLocation(
		GetWorld(), MyPawn->GetPawnViewLocation(), DesiredRelease, MyPawn);
	const bool bClamped = !Release.Equals(DesiredRelease, 0.5f);

	ATraceOysterJar* JarActor = SpawnJar(Release, LaunchVelocity, /*bPickler*/ true);
	if (JarActor == nullptr)
	{
		return nullptr;
	}
	if (bClamped)
	{
		JarActor->NoteReleaseWasClamped();
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Oyster] Pickler release pulled back %.0f uu — the muzzle was inside geometry."),
			FVector::Dist(Release, DesiredRelease));
	}

	UE_LOG(LogTraceGame, Log, TEXT("[Oyster] Pickler lobbed at %.0f uu/s (+%.0f%% up)%s."),
		Speed, UpBias * 100.f, bClamped ? TEXT(", release clamped clear of geometry") : TEXT(""));

	return JarActor;
}

bool UTraceAbilitySetOyster::ActivateAbility()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		return false;
	}

	if (HasAuthority() && ThrowPickler() == nullptr)
	{
		return false;
	}

	// True on both machines: the client charges its predicted cooldown, the server charges the real
	// one. There is no fizzle case — a lob always leaves his hand.
	return true;
}

ATraceOysterJar* UTraceAbilitySetOyster::DebugThrowPickler()
{
	return ThrowPickler();
}

ATraceOysterJar* UTraceAbilitySetOyster::DebugSpawnJarAt(const FVector& Location, bool bPickler)
{
	return SpawnJar(Location, FVector::ZeroVector, bPickler);
}

// =================================================================================================
// Tick — the two polls that BACK UP the hooks. Both hooks are wired; these catch what they cannot.
// =================================================================================================

void UTraceAbilitySetOyster::TickAbilities(float DeltaSeconds)
{
	if (!UTraceAbilityComponent::AreCharactersEnabled(this))
	{
		if (HasAuthority() && LiveJars.Num() > 0)
		{
			DestroyAllJars();
		}
		return;
	}

	ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (HasAuthority())
	{
		PruneJars();

		// --- THE DASH POLL, NOW WATCHING FOR THE END (spec v19 §4.3) --------------------------------
		//
		// UTraceCharacterMovementComponent DOES call NotifyDashStarted/NotifyDashEnded, so the hooks
		// above are live. This poll is the backstop for the one case they cannot cover: a dash whose
		// clock is cleared rather than allowed to run out (a respawn, a mode change) never reaches the
		// closing hook. IsDashing() is a pure function of that clock and is correct on the server for
		// every pawn, human or bot, so its FALLING edge is the same event the hook would have
		// delivered. The debt latch is shared with the hooks, so neither can double up.
		const bool bDashingNow = (MyPawn != nullptr) && MyPawn->IsDashing();
		if (bDashingNow && !bWasDashing)
		{
			NoteDashBegan();
		}
		else if (!bDashingNow && bWasDashing)
		{
			DropOwedDashJar();
			bDashTracked = false;
		}
		bWasDashing = bDashingNow;
	}

	// --- THE JUMP POLL --------------------------------------------------------------------------
	//
	// Runs on the server AND on the owning client, because the boost has to be applied on both or it
	// rubber-bands. ATracePlayerController's jump binding DOES reach OnJumpPressed above, through
	// UTraceAbilityComponent::HandleJumpPressed; this poll is the backstop for a jump that leaves the
	// ground without going through that binding, and JarJumpLatchSeconds is what keeps the two from
	// boosting twice for one press. The jar is found from the position he was standing at LAST tick —
	// by the time he is airborne he has already left it.
	if (MyPawn != nullptr && MoveComp != nullptr && ShouldDriveMovement())
	{
		const bool bOnGroundNow = MoveComp->IsMovingOnGround();
		if (bWasOnGround && !bOnGroundNow && MoveComp->Velocity.Z > 1.f)
		{
			DoJarJump(LastGroundedLocation);
		}
		if (bOnGroundNow)
		{
			LastGroundedLocation = MyPawn->GetActorLocation();
		}
		bWasOnGround = bOnGroundNow;
	}
}

void UTraceAbilitySetOyster::PublishState()
{
	if (!HasAuthority())
	{
		return;
	}

	FTraceAbilityNetState& NetState = MutableState();
	NetState.Stacks = static_cast<uint8>(FMath::Clamp(GetLiveJarCount(), 0, 255));
	NetState.Flags = (NetState.Stacks > 0) ? TraceAbilityFlags::AuxActive : 0;
	MarkStateDirty();
}

// =================================================================================================
// THE EVIDENCE FOR SPEC v19 §4.3 AND §4.4
// =================================================================================================
//
//   Trace.Oyster.DashJarTest      §4.3. One real dash through the shipping entry point, and the jar
//                                 measured against BOTH ends of it. Two arms: with
//                                 Trace.Oyster.LegacyDashJarAtStart 1 the jar must be at the
//                                 departure (the pre-v19 behaviour, i.e. the harness going red), and
//                                 with the shipped 0 it must be at the arrival.
//
//   Trace.Oyster.PicklerThrowTest §4.4, "more consistent to throw" — which is a bug report, so this
//                                 command is a REPRODUCTION first and a proof second. Three separate
//                                 inconsistencies, each run in a LEGACY arm that must fail and a
//                                 SHIPPED arm that must pass, in one process on the same ground:
//
//                                   1. FRAME RATE, and this is the big one. The identical throw, made
//                                      at a high and a low frame cap. The two landing points must
//                                      coincide. They cannot in the legacy arm, because one
//                                      semi-implicit Euler step per rendered frame over-estimates the
//                                      drop by 0.5*g*dt*t: measured at 118 uu of drift.
//                                   2. RELEASE POINT, and this one is SMALL — say so rather than let
//                                      the list imply otherwise. A lob made under a ceiling 4 uu above
//                                      his head is released inside that ceiling, and a swept move that
//                                      starts penetrating is refused, so the jar is BORN buried in the
//                                      roof. Fixing it changes where the jar comes to rest, not how
//                                      far it flies. It cannot happen at a wall at all: see the note
//                                      above EThrowPose for the arithmetic, and for the wall-hunting
//                                      arm that used to stand here and could not go red.
//                                   3. THE JAR CAP. A throw, then three dashes' worth of jars while
//                                      it is still in the air. The cap must not delete the jar he
//                                      spent a 20 s cooldown on.
//
//   Trace.Oyster.PicklerPullTest  §4.4, "greater pull radius". One Pickler landing, run at the
//                                 pre-v19 260 uu and at the shipped radius, with one enemy inside
//                                 both (the fixture's own proof of life) and one standing in the ring
//                                 between them. Judged from how hard each is actually thrown.
//
// EVERY MEASUREMENT IS TAKEN FROM THE ACTORS IN THE WORLD, never from what the throw intended, and the
// throw itself goes through UTraceAbilitySetOyster::ThrowPickler — the same function E reaches. Each
// command PRINTS THE CONDITIONS IT ACTUALLY ACHIEVED and refuses to read anything into an arm that did
// not stage its defect: different frame rates for 1, a muzzle genuinely inside geometry for 2, a
// victim genuinely standing in the ring for the pull. A green measured over a fixture that never set
// up the failure is worth nothing at all.

#if !UE_BUILD_SHIPPING

#include "Components/BoxComponent.h"     // the ceiling Trace.Oyster.PicklerThrowTest builds for itself
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"                 // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"

#include "Core/TracePlayerController.h"   // spec v28 §4: IsGameInputSuppressed, for the real key press
#include "Gameplay/TraceHealthComponent.h" // ...and ResetHealth, so a victim survives four fixtures

namespace TraceAbilitySetOysterHarness
{
	UWorld* FindAuthoritativeWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetAuthGameMode() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** The select screen pauses the world, and a paused world makes every number below a false zero. */
	void UnpauseAndReport(UWorld* WorldPtr, const TCHAR* Tag)
	{
		if (WorldPtr == nullptr || !WorldPtr->IsPaused())
		{
			return;
		}
		if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
		{
			FirstPC->SetPause(false);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] The world was PAUSED (the character-select screen does that). Unpaused, or every "
				     "measurement below would have been a zero that looks like a pass."), Tag);
		}
	}

	/** The human's ability component. A bot's character belongs to the game mode's fill and would race us. */
	UTraceAbilityComponent* FindHumanAbilityComponent(UWorld* WorldPtr)
	{
		const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (BaseState == nullptr)
		{
			return nullptr;
		}
		for (APlayerState* Entry : BaseState->PlayerArray)
		{
			if (Entry == nullptr || Entry->IsABot())
			{
				continue;
			}
			if (UTraceAbilityComponent* Comp = Entry->FindComponentByClass<UTraceAbilityComponent>())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	void SetIntCVar(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	void SetFloatCVar(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** Holds the pawn exactly where the measurement wants it, facing exactly where it wants. */
	void PosePawn(ATraceCharacter* MyPawn, const FVector& Where, const FRotator& Facing)
	{
		if (MyPawn == nullptr)
		{
			return;
		}
		MyPawn->SetActorLocation(Where, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		if (UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement())
		{
			MoveComp->Velocity = FVector::ZeroVector;
			MoveComp->StopMovementImmediately();
		}
		MyPawn->SetActorRotation(FRotator(0.f, Facing.Yaw, 0.f));
		if (AController* Ctrl = MyPawn->GetController())
		{
			Ctrl->SetControlRotation(Facing);
		}
	}

	// =============================================================================================
	// Trace.Oyster.DashJarTest — spec v19 §4.3
	// =============================================================================================

	struct FDashJarState
	{
		int32 Phase = 0;
		int32 Arm = 0;                 // 0 = LEGACY (drop at the start), 1 = SHIPPED (drop at the end)
		double PhaseStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;

		FVector StartLocation = FVector::ZeroVector;
		FVector EndLocation = FVector::ZeroVector;
		bool    bArrivalSampled = false;
		FVector JarLocation[2] = { FVector::ZeroVector, FVector::ZeroVector };
		float   DashDistance[2] = { 0.f, 0.f };
		float   JarToStart[2] = { 0.f, 0.f };
		float   JarToEnd[2] = { 0.f, 0.f };
		int32   JarsMade[2] = { 0, 0 };
		bool    bDashSeen[2] = { false, false };

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERDASHJAR]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	void RunDashJarTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERDASHJAR] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("OYSTERDASHJAR"));

		TSharedPtr<FDashJarState> State = MakeShared<FDashJarState>();
		State->PhaseStartReal = FPlatformTime::Seconds();

		UE_LOG(LogTraceGame, Display,
			TEXT("[OYSTERDASHJAR] ===== spec v19 §4.3: \"Oyster's jar spawns at the END of his dash, not the "
			     "beginning.\" Two arms on the SAME dash — LEGACY (Trace.Oyster.LegacyDashJarAtStart 1) must put "
			     "the jar at the departure, SHIPPED must put it at the arrival. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERDASHJAR] ABORTED: the world went away."));
				SetIntCVar(TEXT("Trace.Oyster.LegacyDashJarAtStart"), 0);
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: become Oyster -----------------------------------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Oyster)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Oyster);
				}
				UTraceAbilitySetOyster* OysterSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;

				if (OysterSet == nullptr || Human->GetOwningCharacter() == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[OYSTERDASHJAR] VERDICT: INVALID — no human player could be made Oyster. Needs a "
							     "mode-B match with characters enabled, run EARLY before bots claim characters."));
						return false;
					}
					return true;
				}

				State->Subject = Human;
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			UTraceAbilityComponent* Comp = State->Subject.Get();
			UTraceAbilitySetOyster* OysterSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (OysterSet == nullptr || MyPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERDASHJAR] ABORTED: Oyster went away mid-test."));
				SetIntCVar(TEXT("Trace.Oyster.LegacyDashJarAtStart"), 0);
				return false;
			}

			// ---- Phase 1: arm and dash --------------------------------------------------------------
			if (State->Phase == 1)
			{
				SetIntCVar(TEXT("Trace.Oyster.LegacyDashJarAtStart"), (State->Arm == 0) ? 1 : 0);
				OysterSet->DebugDestroyAllJars();

				// WAITS FOR A CHARGE. The first arm spends the pool, and a second DoDash() with nothing
				// banked walks him a few uu and reports 0 jars — which is a harness failure that reads
				// exactly like a broken ability. CanDash() is the same test the input path uses.
				const UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement();
				if (MoveComp == nullptr || !MoveComp->IsMovingOnGround() || !MoveComp->CanDash())
				{
					if ((NowReal - State->PhaseStartReal) > 20.0)
					{
						State->Check(false,
							FString::Printf(TEXT("Oyster was on the ground with a dash charge banked within 20 s "
								"(ground=%d charges=%d)"),
								(MoveComp != nullptr && MoveComp->IsMovingOnGround()) ? 1 : 0,
								(MoveComp != nullptr) ? MoveComp->GetDashCharges() : -1));
						State->Phase = 4;
					}
					return true;
				}

				State->StartLocation = MyPawn->GetActorLocation();
				MyPawn->DoDash();                       // the shipping input entry point
				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 2: watch the dash out ---------------------------------------------------------
			if (State->Phase == 2)
			{
				if (MyPawn->IsDashing())
				{
					State->bDashSeen[State->Arm] = true;
					return true;
				}

				// THE ARRIVAL IS SAMPLED ON THE FRAME THE DASH ENDS, not after the settle wait. A dash
				// hands the player back at DashExitSpeedMultiplier x the ground limit, so he keeps
				// sliding for a few hundred uu afterwards; sampling late measured the jar against where
				// he had COASTED to and made a correct drop look 149 uu adrift.
				if (!State->bArrivalSampled)
				{
					State->bArrivalSampled = true;
					State->EndLocation = MyPawn->GetActorLocation();
					State->PhaseStartReal = NowReal;
					return true;
				}

				// Then the settle wait, so the closing hook and the closing poll have both run.
				if ((NowReal - State->PhaseStartReal) < 0.35)
				{
					return true;
				}

				State->DashDistance[State->Arm] = FVector::Dist(State->StartLocation, State->EndLocation);
				State->JarsMade[State->Arm] = OysterSet->GetLiveJarCount();

				// The jar itself, READ OUT OF THE WORLD rather than out of the ability's bookkeeping.
				// -1 means there was no jar at all, which must never be reported as a distance.
				FVector Found = FVector::ZeroVector;
				bool bFoundJar = false;
				for (TActorIterator<ATraceOysterJar> It(TickWorld); It; ++It)
				{
					if (ATraceOysterJar* JarActor = *It)
					{
						if (JarActor->GetOwner() == MyPawn)
						{
							Found = JarActor->GetActorLocation();
							bFoundJar = true;
							break;
						}
					}
				}
				State->JarLocation[State->Arm] = Found;
				State->JarToStart[State->Arm] = bFoundJar ? FVector::Dist2D(Found, State->StartLocation) : -1.f;
				State->JarToEnd[State->Arm]   = bFoundJar ? FVector::Dist2D(Found, State->EndLocation) : -1.f;

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERDASHJAR] %s arm: dash covered %.0f uu; %d jar(s); jar is %.0f uu from the "
					     "DEPARTURE and %.0f uu from the ARRIVAL (-1 = no jar was found in the world)."),
					(State->Arm == 0) ? TEXT("LEGACY") : TEXT("SHIPPED"),
					State->DashDistance[State->Arm], State->JarsMade[State->Arm],
					State->JarToStart[State->Arm], State->JarToEnd[State->Arm]);

				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: next arm, or judge ---------------------------------------------------------
			if (State->Phase == 3)
			{
				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Phase = 1;
					State->bArrivalSampled = false;
					State->PhaseStartReal = NowReal;
					return true;
				}
				State->Phase = 4;
			}

			// ---- Phase 4: verdict --------------------------------------------------------------------
			SetIntCVar(TEXT("Trace.Oyster.LegacyDashJarAtStart"), 0);

			// The fixture proving itself FIRST. A dash that did not move him makes both distances
			// meaningless and every line below it a green that means nothing.
			State->Check(State->bDashSeen[0] && State->bDashSeen[1],
				TEXT("both arms actually produced a dash (IsDashing() was observed true)"));
			State->Check(State->DashDistance[0] > 200.f && State->DashDistance[1] > 200.f,
				FString::Printf(TEXT("both dashes actually moved him a measurable distance (%.0f uu, %.0f uu) — "
					"without this the two ends are the same place and nothing below can tell them apart"),
					State->DashDistance[0], State->DashDistance[1]));
			State->Check(State->JarsMade[0] == 1 && State->JarsMade[1] == 1,
				FString::Printf(TEXT("exactly ONE jar per dash in each arm (%d, %d) — the two routes that can "
					"notice a dash share one latch"), State->JarsMade[0], State->JarsMade[1]));

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERDASHJAR] --- THE REPRODUCTION: the pre-v19 drop"));
			State->Check(State->JarToStart[0] >= 0.f && State->JarToStart[0] < State->JarToEnd[0],
				FString::Printf(TEXT("LEGACY put the jar at the DEPARTURE (%.0f uu from it, %.0f uu from the "
					"arrival) — this is the behaviour §4.3 asks us to change, and it is what this harness "
					"looks like when it is red"), State->JarToStart[0], State->JarToEnd[0]));

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERDASHJAR] --- THE FIX: \"the END of his dash\""));
			State->Check(State->JarToEnd[1] >= 0.f && State->JarToEnd[1] < 100.f,
				FString::Printf(TEXT("SHIPPED put the jar where the dash ENDED (%.0f uu from the arrival)"),
					State->JarToEnd[1]));
			State->Check(State->JarToEnd[1] >= 0.f && State->JarToEnd[1] < State->JarToStart[1],
				FString::Printf(TEXT("SHIPPED put the jar nearer the arrival than the departure (%.0f uu vs %.0f uu)"),
					State->JarToEnd[1], State->JarToStart[1]));

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERDASHJAR] ===== %d passed, %d failed. ====="),
				State->Passed, State->Failed);
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERDASHJAR] VERDICT: %s"),
				(State->Failed == 0 && State->Passed > 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
			return false;
		}));
	}

	FAutoConsoleCommand CmdOysterDashJarTest(
		TEXT("Trace.Oyster.DashJarTest"),
		TEXT("Dev only, server only. SPEC v19 §4.3: Oyster's dash jar must land where the dash ENDS. Runs the "
		     "same real dash twice, once with the pre-v19 drop-at-the-start restored so the harness is shown red."),
		FConsoleCommandDelegate::CreateStatic(&RunDashJarTest));

	// =============================================================================================
	// Trace.Oyster.PicklerThrowTest — spec v19 §4.4, "more consistent to throw"
	// =============================================================================================

	enum class EThrowPose : uint8
	{
		Open,           // clear ground, a normal lob
		UnderLintel     // standing under a low ceiling, lobbing up into it
	};

	/**
	 * WHERE THE MUZZLE CAN ACTUALLY END UP INSIDE THE WORLD — AND WHERE IT CANNOT.
	 *
	 * This started as two poses flush against a wall found in the map, and the reproduction arm of that
	 * DID NOT GO RED: both arms threw 1867 uu and the release guard never fired. Working the arithmetic
	 * afterwards says it never could, so the fixture was the thing that was wrong, and here is the
	 * measurement it should have been built on:
	 *
	 *   The capsule radius is 34 uu, so no VERTICAL surface can ever be nearer than 34 uu to his axis.
	 *   The muzzle is 22 uu along the aim ray from the eye, which sits on that axis. So at a wall the
	 *   muzzle is at BEST 34 - 22 = 12 uu clear of the face, and the jar's sphere is 18 — a maximum
	 *   overlap of 6 uu, reached only when he is literally touching the wall and aiming dead at it.
	 *   That is real but tiny, and it is smaller than the error in posing a pawn flush against a wall
	 *   probed out of the map, which is why the old arm measured nothing.
	 *
	 *   VERTICALLY there is no such floor. The capsule half height is 88 and the eye is 64 above the
	 *   capsule centre, so a ceiling may be as little as 88 - 64 = 24 uu above the eye. Aim up and the
	 *   muzzle climbs 22 of those 24. THAT is where a jar is genuinely born inside the world, and it is
	 *   a thing players do: standing under an overhang, a lintel or a ramp's underside and lobbing.
	 *
	 * So the reproduction is a ceiling, not a wall, and the fixture SPAWNS it rather than hunting for
	 * one, so the arm runs the same way on every map. Its underside sits 4 uu above his head; at a 60
	 * degree lob the muzzle ends up 9 uu inside it, and the eye stays 10 uu clear — which the fixture
	 * MEASURES and reports before it judges anything, because an arm that failed to embed the muzzle
	 * has tested nothing and must say so rather than pass.
	 *
	 * AND WHAT THE FIX IS AND IS NOT WORTH. Honestly stated, because the comment above it used to
	 * overclaim: a release inside a ceiling does not make the throw travel further once it is fixed —
	 * the jar is against the ceiling either way and stops at once either way. What changes is WHERE it
	 * comes to rest: inside the ceiling, where it cannot be seen, or against its underside, where it
	 * can. The open-ground arms are the ones that carry the "more consistent to throw" claim.
	 */

	struct FThrowPlan
	{
		FString Label;
		bool bLegacy = false;
		float FpsCap = 0.f;             // 0 = leave the cap alone
		EThrowPose Pose = EThrowPose::Open;
		bool bFloodJarCapMidFlight = false;
	};

	struct FThrowResult
	{
		bool  bThrown = false;
		bool  bLanded = false;
		bool  bVanishedInFlight = false;
		bool  bLandingEffect = false;
		bool  bReleaseClamped = false;
		/** The fixture proving itself: was the RAW muzzle inside geometry when this throw was made? */
		bool  bMuzzleWasInsideGeometry = false;
		/** The measurement: did the jar come to rest inside geometry rather than against it? */
		bool  bAtRestInsideGeometry = false;
		FVector Muzzle = FVector::ZeroVector;
		FVector Release = FVector::ZeroVector;
		FVector Landing = FVector::ZeroVector;
		float TravelUU = 0.f;
		float FlightSeconds = 0.f;
		int32 Sweeps = 0;
		int32 FlightFrames = 0;
		float MeanFrameMs = 0.f;
	};

	struct FPicklerThrowState
	{
		int32 Phase = 0;
		int32 Index = 0;                // which plan entry
		int32 SettleFrames = 0;
		double PhaseStartReal = 0.0;
		double ThrowStartReal = 0.0;
		float  FlightRealSeconds = 0.f;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;
		TWeakObjectPtr<ATraceOysterJar> Watched;

		/** BOTH poses stand on the same ground facing the same way. Only the ceiling above him moves. */
		FVector OpenAnchor = FVector::ZeroVector;
		FRotator OpenFacing = FRotator::ZeroRotator;

		/** The spawned ceiling. Destroyed between arms, and on every abort path. */
		TWeakObjectPtr<AActor> Lintel;

		TArray<FThrowPlan> Plan;
		TArray<FThrowResult> Results;

		int32 Passed = 0;
		int32 Failed = 0;

		FVector AnchorFor(EThrowPose /*Pose*/) const
		{
			return OpenAnchor;
		}

		FRotator FacingFor(EThrowPose Pose) const
		{
			// 60 degrees up: far enough that the muzzle is well inside a ceiling 4 uu above his head,
			// shallow enough to still be a lob somebody would actually make under an overhang.
			return (Pose == EThrowPose::UnderLintel)
				? FRotator(60.f, OpenFacing.Yaw, 0.f)
				: OpenFacing;
		}

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERTHROW]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	/** The yaw with the most clear air in front of it, so a lob has room to actually be a lob. */
	FRotator FindOpenFacing(const UWorld* WorldPtr, const ATraceCharacter* MyPawn)
	{
		const FVector Eye = MyPawn->GetPawnViewLocation();
		float BestClear = -1.f;
		float BestYaw = MyPawn->GetActorRotation().Yaw;

		for (int32 Step = 0; Step < 16; ++Step)
		{
			const float Yaw = Step * 22.5f;
			const FVector Dir = FRotator(0.f, Yaw, 0.f).Vector();
			FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceOysterOpenProbe), false, MyPawn);
			FHitResult ProbeHit;
			const bool bBlocked = WorldPtr->LineTraceSingleByChannel(
				ProbeHit, Eye, Eye + Dir * 3000.f, ECC_WorldStatic, Params);
			const float Clear = bBlocked ? ProbeHit.Distance : 3000.f;
			if (Clear > BestClear)
			{
				BestClear = Clear;
				BestYaw = Yaw;
			}
		}
		// Slightly downward: a lob that lands is a lob that can be measured.
		return FRotator(-8.f, BestYaw, 0.f);
	}

	/**
	 * True when a jar could not be at @p Where without being inside the world.
	 *
	 * The sphere is deliberately 3 uu SMALLER than the jar's own, so a jar resting ON a surface — which
	 * a swept move leaves a whisker of overlap against — reads false, and only a jar genuinely buried
	 * in geometry reads true. ECC_WorldStatic as an OBJECT TYPE, because that is exactly what the jar
	 * blocks against; nothing a pawn is made of can answer this question.
	 */
	bool IsInsideGeometry(const UWorld* WorldPtr, const FVector& Where)
	{
		if (WorldPtr == nullptr)
		{
			return false;
		}

		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

		const FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceOysterEmbedProbe), /*bTraceComplex=*/false);
		return WorldPtr->OverlapAnyTestByObjectType(Where, FQuat::Identity, ObjectParams,
			FCollisionShape::MakeSphere(ATraceOysterJar::GetJarCollisionRadiusUU() - 3.f), Params);
	}

	/**
	 * A slab of blocking world geometry, made by the fixture instead of hunted for in the map.
	 *
	 * The old arm probed the level for a wall and posed the pawn against it, and the pose was never
	 * accurate enough to reproduce anything (see the note above EThrowPose). A slab the fixture places
	 * itself is exact, is the same on every map, and is the same KIND of thing a wall is — object type
	 * ECC_WorldStatic, which is the one thing ATraceOysterJar's sphere blocks against.
	 */
	AActor* SpawnHarnessSlab(UWorld* WorldPtr, const FVector& Centre, const FVector& Extent)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AActor* Slab = WorldPtr->SpawnActor<AActor>(AActor::StaticClass(), Centre, FRotator::ZeroRotator, SpawnParams);
		if (Slab == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* Box = NewObject<UBoxComponent>(Slab, TEXT("TraceOysterHarnessSlab"));
		Box->SetBoxExtent(Extent, /*bUpdateOverlaps*/ false);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->SetGenerateOverlapEvents(false);
		Box->SetCanEverAffectNavigation(false);
		Slab->SetRootComponent(Box);
		Box->RegisterComponent();
		Slab->SetActorLocation(Centre);

		return Slab;
	}

	void RunPicklerThrowTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERTHROW] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("PICKLERTHROW"));

		TSharedPtr<FPicklerThrowState> State = MakeShared<FPicklerThrowState>();
		State->PhaseStartReal = FPlatformTime::Seconds();

		// The plan. Legacy and shipped arms interleaved so both see the same match, the same bots and
		// the same geometry; a difference between them cannot be a difference of circumstance.
		State->Plan.Add({ TEXT("LEGACY  fast frames"),   true,  200.f, EThrowPose::Open,        false });
		State->Plan.Add({ TEXT("LEGACY  slow frames"),   true,   12.f, EThrowPose::Open,        false });
		State->Plan.Add({ TEXT("SHIPPED fast frames"),   false, 200.f, EThrowPose::Open,        false });
		State->Plan.Add({ TEXT("SHIPPED slow frames"),   false,  12.f, EThrowPose::Open,        false });
		State->Plan.Add({ TEXT("SHIPPED fast, repeat"),  false, 200.f, EThrowPose::Open,        false });
		State->Plan.Add({ TEXT("LEGACY  under a lintel"),true,  200.f, EThrowPose::UnderLintel, false });
		State->Plan.Add({ TEXT("SHIPPED under a lintel"),false, 200.f, EThrowPose::UnderLintel, false });
		State->Plan.Add({ TEXT("LEGACY  cap flood"),     true,  200.f, EThrowPose::Open,        true  });
		State->Plan.Add({ TEXT("SHIPPED cap flood"),     false, 200.f, EThrowPose::Open,        true  });
		State->Results.SetNum(State->Plan.Num());

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[PICKLERTHROW] ===== spec v19 §4.4 \"more consistent to throw\" — a REPRODUCTION first. "
			     "Throw %.0f uu/s +%.0f%% up, jar sphere %.0f uu, pull %.0f uu, damage %.0f uu, cap %d jars. ====="),
			Settings.OysterPicklerThrowSpeed, Settings.OysterPicklerThrowUpBias * 100.f,
			ATraceOysterJar::GetJarCollisionRadiusUU(), Settings.OysterPicklerPullRadiusUU,
			Settings.OysterPicklerDamageRadiusUU, Settings.OysterMaxJars);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float DeltaReal) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				// The lintel went with the world; nothing to tear down.
				UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERTHROW] ABORTED: the world went away."));
				SetIntCVar(TEXT("Trace.Oyster.LegacyThrow"), 0);
				SetFloatCVar(TEXT("t.MaxFPS"), 0.f);
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: become Oyster -----------------------------------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Oyster)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Oyster);
				}
				UTraceAbilitySetOyster* OysterSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;

				if (OysterSet == nullptr || Human->GetOwningCharacter() == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[PICKLERTHROW] VERDICT: INVALID — no human player could be made Oyster. Needs a "
							     "mode-B match with characters enabled, run EARLY before bots claim characters."));
						return false;
					}
					return true;
				}

				State->Subject = Human;
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			UTraceAbilityComponent* Comp = State->Subject.Get();
			UTraceAbilitySetOyster* OysterSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (OysterSet == nullptr || MyPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERTHROW] ABORTED: Oyster went away mid-test."));
				SetIntCVar(TEXT("Trace.Oyster.LegacyThrow"), 0);
				SetFloatCVar(TEXT("t.MaxFPS"), 0.f);
				if (AActor* StrandedLintel = State->Lintel.Get())
				{
					StrandedLintel->Destroy();   // never leave a slab of invisible wall standing in the match
					State->Lintel = nullptr;
				}
				return false;
			}

			// ---- Phase 1: choose the two poses ------------------------------------------------------
			if (State->Phase == 1)
			{
				const UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement();
				if (MoveComp == nullptr || !MoveComp->IsMovingOnGround())
				{
					if ((NowReal - State->PhaseStartReal) > 8.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[PICKLERTHROW] VERDICT: INVALID — Oyster never reached the ground to throw from."));
						return false;
					}
					return true;
				}

				State->OpenAnchor = MyPawn->GetActorLocation();
				State->OpenFacing = FindOpenFacing(TickWorld, MyPawn);

				UE_LOG(LogTraceGame, Display,
					TEXT("[PICKLERTHROW] One pose for every arm: %s, facing yaw %.0f. The lintel arms stand in the "
					     "same spot and lob 60 deg up into a ceiling the fixture spawns 4 uu above his head."),
					*State->OpenAnchor.ToCompactString(), State->OpenFacing.Yaw);

				State->Phase = 2;
				State->Index = 0;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 2: set up one throw -----------------------------------------------------------
			if (State->Phase == 2)
			{
				if (State->Index >= State->Plan.Num())
				{
					State->Phase = 5;
					return true;
				}

				const FThrowPlan& Entry = State->Plan[State->Index];

				SetIntCVar(TEXT("Trace.Oyster.LegacyThrow"), Entry.bLegacy ? 1 : 0);
				SetFloatCVar(TEXT("t.MaxFPS"), Entry.FpsCap);
				OysterSet->DebugDestroyAllJars();
				Comp->DebugSetActivatedCooldown(0.f);

				PosePawn(MyPawn, State->AnchorFor(Entry.Pose), State->FacingFor(Entry.Pose));

				// The ceiling, put up and taken down per arm so the open arms are measured over genuinely
				// open sky. 88 (half height) + 4 of headroom above the capsule centre is the underside;
				// the slab is 120 thick and 500 across, so nothing can be thrown around its edge.
				if (AActor* OldLintel = State->Lintel.Get())
				{
					OldLintel->Destroy();
					State->Lintel = nullptr;
				}
				if (Entry.Pose == EThrowPose::UnderLintel)
				{
					const FVector SlabExtent(250.f, 250.f, 60.f);
					State->Lintel = SpawnHarnessSlab(TickWorld,
						State->OpenAnchor + FVector(0.f, 0.f, 92.f + SlabExtent.Z), SlabExtent);
				}

				State->SettleFrames = 0;
				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: let the frame cap and the pose settle, then throw ---------------------------
			if (State->Phase == 3)
			{
				// The pose is re-applied every settle frame: gravity and the movement component would
				// otherwise walk him off it before the throw, and then the two arms would not be the
				// same throw at all.
				const FThrowPlan& Entry = State->Plan[State->Index];
				PosePawn(MyPawn, State->AnchorFor(Entry.Pose), State->FacingFor(Entry.Pose));

				if (++State->SettleFrames < 8)
				{
					return true;
				}

				FThrowResult& Result = State->Results[State->Index];

				// THE FIXTURE PROVING ITSELF, taken BEFORE the throw: was the raw muzzle — the point the
				// pre-v19 release used — actually inside the world? An arm that failed to embed it has
				// reproduced nothing, and the verdict below refuses to read anything into it.
				Result.Muzzle = MyPawn->GetMuzzleLocation();
				Result.bMuzzleWasInsideGeometry = IsInsideGeometry(TickWorld, Result.Muzzle);

				ATraceOysterJar* JarActor = OysterSet->DebugThrowPickler();
				if (JarActor == nullptr)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERTHROW] %s: the throw produced NO jar at all."),
						*Entry.Label);
					++State->Index;
					State->Phase = 2;
					return true;
				}

				Result.bThrown = true;
				Result.Release = JarActor->GetLaunchLocation();
				Result.bReleaseClamped = JarActor->WasReleaseClamped();
				State->Watched = JarActor;
				State->ThrowStartReal = NowReal;
				State->FlightRealSeconds = 0.f;
				Result.FlightFrames = 0;

				State->Phase = 4;
				return true;
			}

			// ---- Phase 4: watch the flight -----------------------------------------------------------
			if (State->Phase == 4)
			{
				const FThrowPlan& Entry = State->Plan[State->Index];
				FThrowResult& Result = State->Results[State->Index];
				ATraceOysterJar* JarActor = State->Watched.Get();

				// The cap flood: three dashes' worth of jars while the throw is still in the air.
				if (Entry.bFloodJarCapMidFlight && Result.FlightFrames == 1)
				{
					for (int32 Drop = 0; Drop < 3; ++Drop)
					{
						OysterSet->DebugDropDashJar();
					}
				}

				++Result.FlightFrames;
				State->FlightRealSeconds += DeltaReal;

				if (JarActor == nullptr)
				{
					Result.bVanishedInFlight = true;
					UE_LOG(LogTraceGame, Display,
						TEXT("[PICKLERTHROW] %s: the jar was DESTROYED before it landed — the throw never happened."),
						*Entry.Label);
					++State->Index;
					State->Phase = 2;
					return true;
				}

				if (!JarActor->IsGrounded())
				{
					if ((NowReal - State->ThrowStartReal) > 6.0)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERTHROW] %s: still airborne after 6 s; giving up."),
							*Entry.Label);
						++State->Index;
						State->Phase = 2;
					}
					return true;
				}

				Result.bLanded = true;
				Result.Landing = JarActor->GetActorLocation();
				Result.bLandingEffect = JarActor->HasFiredLandingEffect();
				Result.Sweeps = JarActor->GetFlightSweepCount();
				Result.TravelUU = FVector::Dist(Result.Release, Result.Landing);
				Result.FlightSeconds = State->FlightRealSeconds;
				Result.MeanFrameMs = (Result.FlightFrames > 0)
					? (State->FlightRealSeconds * 1000.f / Result.FlightFrames) : 0.f;
				Result.bAtRestInsideGeometry = IsInsideGeometry(TickWorld, Result.Landing);

				UE_LOG(LogTraceGame, Display,
					TEXT("[PICKLERTHROW] %s: travelled %.0f uu in %.2f s over %d frame(s) (%.1f ms/frame, %d sweeps); "
					     "landed at %s; impact fired %d; release clamped %d; muzzle was inside geometry %d; "
					     "came to rest inside geometry %d."),
					*Entry.Label, Result.TravelUU, Result.FlightSeconds, Result.FlightFrames,
					Result.MeanFrameMs, Result.Sweeps, *Result.Landing.ToCompactString(),
					Result.bLandingEffect ? 1 : 0, Result.bReleaseClamped ? 1 : 0,
					Result.bMuzzleWasInsideGeometry ? 1 : 0, Result.bAtRestInsideGeometry ? 1 : 0);

				++State->Index;
				State->Phase = 2;
				return true;
			}

			// ---- Phase 5: verdict ----------------------------------------------------------------------
			SetIntCVar(TEXT("Trace.Oyster.LegacyThrow"), 0);
			SetFloatCVar(TEXT("t.MaxFPS"), 0.f);
			if (OysterSet != nullptr)
			{
				OysterSet->DebugDestroyAllJars();
			}
			if (AActor* SpentLintel = State->Lintel.Get())
			{
				SpentLintel->Destroy();
				State->Lintel = nullptr;
			}

			const FThrowResult& LegacyFast    = State->Results[0];
			const FThrowResult& LegacySlow    = State->Results[1];
			const FThrowResult& ShippedFast   = State->Results[2];
			const FThrowResult& ShippedSlow   = State->Results[3];
			const FThrowResult& ShippedAgain  = State->Results[4];
			const FThrowResult& LegacyLintel  = State->Results[5];
			const FThrowResult& ShippedLintel = State->Results[6];
			const FThrowResult& LegacyFlood   = State->Results[7];
			const FThrowResult& ShippedFlood  = State->Results[8];

			// ---- 1. FRAME RATE -------------------------------------------------------------------------
			UE_LOG(LogTraceGame, Display,
				TEXT("[PICKLERTHROW] --- 1/3  THE SAME THROW MUST LAND IN THE SAME PLACE AT ANY FRAME RATE"));

			const bool bFrameRatesDiffered =
				LegacyFast.bLanded && LegacySlow.bLanded && ShippedFast.bLanded && ShippedSlow.bLanded
				&& (LegacySlow.MeanFrameMs > LegacyFast.MeanFrameMs * 2.f)
				&& (ShippedSlow.MeanFrameMs > ShippedFast.MeanFrameMs * 2.f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[PICKLERTHROW]   frame deltas actually achieved: legacy %.1f ms vs %.1f ms; shipped %.1f ms vs %.1f ms"),
				LegacyFast.MeanFrameMs, LegacySlow.MeanFrameMs, ShippedFast.MeanFrameMs, ShippedSlow.MeanFrameMs);

			State->Check(bFrameRatesDiffered,
				TEXT("the fixture really did run the two arms at different frame rates — a 'consistent' landing "
				     "measured across two IDENTICAL frame rates would prove nothing at all"));

			if (bFrameRatesDiffered)
			{
				const float LegacySpread  = FVector::Dist(LegacyFast.Landing, LegacySlow.Landing);
				const float ShippedSpread = FVector::Dist(ShippedFast.Landing, ShippedSlow.Landing);
				const float RepeatSpread  = ShippedAgain.bLanded
					? FVector::Dist(ShippedFast.Landing, ShippedAgain.Landing) : -1.f;

				State->Check(LegacySpread > 30.f,
					FString::Printf(TEXT("REPRODUCED: the legacy throw moved %.0f uu when only the frame rate "
						"changed — this is the inconsistency the report is about"), LegacySpread));
				State->Check(ShippedSpread < 15.f,
					FString::Printf(TEXT("FIXED: the shipped throw moved %.0f uu across the same frame-rate change"),
						ShippedSpread));
				State->Check(ShippedSpread < LegacySpread,
					FString::Printf(TEXT("the shipped throw is strictly steadier than the legacy one (%.0f uu vs %.0f uu)"),
						ShippedSpread, LegacySpread));
				State->Check(RepeatSpread >= 0.f && RepeatSpread < 15.f,
					FString::Printf(TEXT("two shipped throws at the SAME frame rate agree to %.0f uu — the fixture "
						"itself is repeatable, so the numbers above are about the frame rate and nothing else"),
						RepeatSpread));
			}

			// ---- 2. RELEASE POINT ----------------------------------------------------------------------
			UE_LOG(LogTraceGame, Display,
				TEXT("[PICKLERTHROW] --- 2/3  A LOB MADE UNDER A LOW CEILING MUST NOT BE BORN INSIDE IT"));

			// The regression half FIRST, and out in the open: a guard that moved throws it had no
			// business touching would be worse than the defect it exists for.
			State->Check(!ShippedFast.bReleaseClamped && !ShippedSlow.bReleaseClamped
				&& !ShippedAgain.bReleaseClamped && !ShippedFlood.bReleaseClamped,
				TEXT("REGRESSION: out in the open the muzzle is nowhere near anything, and the new release guard "
				     "never fires on ANY of the four open throws"));
			State->Check(LegacyFast.bLanded && ShippedFast.bLanded
				&& FVector::Dist(LegacyFast.Landing, ShippedFast.Landing) < 15.f,
				FString::Printf(TEXT("...and an open throw still lands where it always did (%.0f uu from the "
					"pre-v19 landing point)"), FVector::Dist(LegacyFast.Landing, ShippedFast.Landing)));

			// The fixture proving itself. If the spawned ceiling did not actually swallow the raw
			// muzzle then this arm reproduced NOTHING, and no green below it would mean anything.
			const bool bLintelStaged = LegacyLintel.bThrown && ShippedLintel.bThrown
				&& LegacyLintel.bMuzzleWasInsideGeometry && ShippedLintel.bMuzzleWasInsideGeometry;

			State->Check(bLintelStaged,
				FString::Printf(TEXT("the spawned ceiling really did swallow the raw muzzle in both arms "
					"(legacy=%d shipped=%d) — the pre-v19 release point was inside the world"),
					LegacyLintel.bMuzzleWasInsideGeometry ? 1 : 0,
					ShippedLintel.bMuzzleWasInsideGeometry ? 1 : 0));

			if (bLintelStaged)
			{
				State->Check(!LegacyLintel.bReleaseClamped && LegacyLintel.bAtRestInsideGeometry,
					TEXT("REPRODUCED: the pre-v19 throw let go inside the ceiling and the jar came to rest INSIDE "
					     "it — buried in the roof over his head, where a player cannot see the jar they just "
					     "spent a 20 s cooldown on"));
				State->Check(ShippedLintel.bReleaseClamped,
					TEXT("FIXED: the shipped throw notices the muzzle is inside the ceiling and pulls the release "
					     "back down the aim ray to where the jar fits"));
				State->Check(ShippedLintel.bLanded && !ShippedLintel.bAtRestInsideGeometry,
					TEXT("...and the jar comes to rest AGAINST the ceiling instead of inside it"));

				// Said out loud rather than quietly asserted, because it is the honest size of this fix.
				UE_LOG(LogTraceGame, Display,
					TEXT("[PICKLERTHROW]   NOTE, and this is the whole of what this arm claims: the lob is stopped "
					     "by the ceiling either way (legacy travelled %.1f uu, shipped %.1f uu). The guard changes "
					     "WHERE the jar ends up, not how far it flies. The frame-rate and jar-cap sections are the "
					     "two that carry \"more consistent to throw\"."),
					LegacyLintel.TravelUU, ShippedLintel.TravelUU);
			}

			// ---- 3. THE JAR CAP ------------------------------------------------------------------------
			UE_LOG(LogTraceGame, Display,
				TEXT("[PICKLERTHROW] --- 3/3  HIS OWN DASH JARS MUST NOT DELETE A PICKLER IN MID-AIR"));
			State->Check(LegacyFlood.bVanishedInFlight,
				FString::Printf(TEXT("REPRODUCED: under the legacy cap the airborne Pickler was destroyed by his own "
					"dash jars (vanished=%d, impact fired=%d)"),
					LegacyFlood.bVanishedInFlight ? 1 : 0, LegacyFlood.bLandingEffect ? 1 : 0));
			State->Check(!ShippedFlood.bVanishedInFlight && ShippedFlood.bLanded,
				TEXT("FIXED: the shipped cap skipped the airborne jar, and the Pickler landed"));
			State->Check(ShippedFlood.bLandingEffect,
				TEXT("...and its 30-damage impact actually fired, which is the whole of the ability"));

			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERTHROW] ===== %d passed, %d failed. ====="),
				State->Passed, State->Failed);
			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERTHROW] VERDICT: %s"),
				(State->Failed == 0 && State->Passed > 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
			return false;
		}));
	}

	FAutoConsoleCommand CmdPicklerThrowTest(
		TEXT("Trace.Oyster.PicklerThrowTest"),
		TEXT("Dev only, server only. SPEC v19 §4.4 \"more consistent to throw\": reproduces three separate "
		     "inconsistencies in the Pickler lob (frame rate, release point, jar cap) with the pre-v19 behaviour "
		     "restored, then shows the same measurements passing on the shipped one."),
		FConsoleCommandDelegate::CreateStatic(&RunPicklerThrowTest));

	// =============================================================================================
	// Trace.Oyster.PicklerPullTest — spec v19 §4.4, "Oyster's Pickler: greater pull radius"
	// =============================================================================================
	//
	// The other half of §4.4, and the half with no evidence at all before this: the pull radius went
	// 260 -> 380 in Config/DefaultGame.ini and nothing measured that a player would ever feel it.
	//
	// The arms are the two RADII, on the SAME landing, with the SAME two victims standing in the SAME
	// places. One victim is inside both radii — he is the fixture proving itself, and he must be
	// yanked in both arms or the measurement below is just a broken pull. The other stands in the RING
	// BETWEEN the two radii, and he is the whole of the change: untouched at 260, yanked at 380.
	//
	// The distances are derived from the two radii rather than typed in, so retuning the knob retunes
	// the test with it, and a knob that has NOT been raised makes this command say INVALID instead of
	// quietly passing on a ring 0 uu wide.
	//
	// THE FOUNDING INVARIANT IS CHECKED HERE TOO, because a bigger radius is a bigger chance of
	// catching the Core carrier: TraceOyster::CarrierTally().PicklerPulls must not move on either arm.
	// That is a cheap guard, not the proof — Trace.Oyster.CarrierTest is the red-armed proof, and it
	// stages an actual carrier.

	struct FPullVictim
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;
		float PlannedDistance = 0.f;
		float ActualDistance[2] = { -1.f, -1.f };     // measured at the instant the jar landed
		float SpeedTowardJar[2] = { 0.f, 0.f };       // peak, over the frames straight after
	};

	struct FPicklerPullState
	{
		int32 Phase = 0;
		int32 Arm = 0;                  // 0 = the pre-v19 260 uu radius, 1 = the shipped one
		int32 SettleFrames = 0;
		int32 SampleFrames = 0;
		double PhaseStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;
		FPullVictim Inside;             // inside BOTH radii
		FPullVictim InRing;             // outside the old radius, inside the new one

		FVector JarOrigin = FVector::ZeroVector;
		FVector InsideAnchor = FVector::ZeroVector;
		FVector RingAnchor = FVector::ZeroVector;
		FRotator VictimFacing = FRotator::ZeroRotator;

		float LegacyRadius = 260.f;     // the pre-v19 value, kept here as the red arm
		float ShippedRadius = 380.f;    // read from the live settings when the test starts

		int32 CarrierPullsAtStart = 0;
		int32 OtherPullsDelta[2] = { 0, 0 };

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERPULL]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	/** Puts the live radius back however the run ends. A test that retunes the game is not a test. */
	void RestorePullRadius(float Radius)
	{
		if (UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>())
		{
			MutableSettings->OysterPicklerPullRadiusUU = Radius;
		}
	}

	void RunPicklerPullTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERPULL] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("PICKLERPULL"));

		TSharedPtr<FPicklerPullState> State = MakeShared<FPicklerPullState>();
		State->PhaseStartReal = FPlatformTime::Seconds();
		State->ShippedRadius = UTraceSettings::Get().OysterPicklerPullRadiusUU;

		UE_LOG(LogTraceGame, Display,
			TEXT("[PICKLERPULL] ===== spec v19 §4.4 \"Oyster's Pickler: greater pull radius\". Two arms on one "
			     "landing: the pre-v19 %.0f uu and the shipped %.0f uu, judged from where two real enemies are "
			     "actually thrown. ====="),
			State->LegacyRadius, State->ShippedRadius);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERPULL] ABORTED: the world went away."));
				RestorePullRadius(State->ShippedRadius);
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: become Oyster -----------------------------------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Oyster)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Oyster);
				}
				UTraceAbilitySetOyster* OysterSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;

				if (OysterSet == nullptr || Human->GetOwningCharacter() == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[PICKLERPULL] VERDICT: INVALID — no human player could be made Oyster. Needs a "
							     "mode-B match with characters enabled, run EARLY before bots claim characters."));
						return false;
					}
					return true;
				}

				State->Subject = Human;
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			UTraceAbilityComponent* Comp = State->Subject.Get();
			UTraceAbilitySetOyster* OysterSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (OysterSet == nullptr || MyPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERPULL] ABORTED: Oyster went away mid-test."));
				RestorePullRadius(State->ShippedRadius);
				return false;
			}

			// ---- Phase 1: pick the ring, and two enemies to stand in it ------------------------------
			if (State->Phase == 1)
			{
				const UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement();
				if (MoveComp == nullptr || !MoveComp->IsMovingOnGround())
				{
					if ((NowReal - State->PhaseStartReal) > 8.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[PICKLERPULL] VERDICT: INVALID — Oyster never reached the ground."));
						return false;
					}
					return true;
				}

				// A ring narrower than a pawn is a ring nobody can be measured standing in.
				if (State->ShippedRadius < State->LegacyRadius + 80.f)
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[PICKLERPULL] VERDICT: INVALID — the pull radius is %.0f uu against a pre-v19 %.0f uu. "
						     "§4.4 asks for a GREATER pull radius and there is no ring here to stand a victim in. "
						     "Check OysterPicklerPullRadiusUU in Config/DefaultGame.ini."),
						State->ShippedRadius, State->LegacyRadius);
					return false;
				}

				const FRotator OpenFacing = FindOpenFacing(TickWorld, MyPawn);
				const FVector Out = FRotator(0.f, OpenFacing.Yaw, 0.f).Vector();

				// The jar lands at his feet and the victims stand out along his clearest line, so both
				// are on the same ground and neither is inside a wall.
				State->JarOrigin = MyPawn->GetActorLocation();
				State->Inside.PlannedDistance = State->LegacyRadius * 0.7f;
				State->InRing.PlannedDistance = (State->LegacyRadius + State->ShippedRadius) * 0.5f;
				State->InsideAnchor = State->JarOrigin + Out * State->Inside.PlannedDistance;
				State->RingAnchor   = State->JarOrigin + Out * State->InRing.PlannedDistance;
				State->VictimFacing = FRotator(0.f, OpenFacing.Yaw + 180.f, 0.f);

				// Enemies, alive, and NOT the Core carrier — a carrier is refused by the choke point by
				// design, so using one as a victim would measure the invariant and call it a radius.
				TArray<ATraceCharacter*> Candidates;
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || Candidate == MyPawn || !Candidate->IsAlive()
						|| Candidate->IsCarrier() || Candidate->GetTeam() == MyPawn->GetTeam())
					{
						continue;
					}
					Candidates.Add(Candidate);
				}

				if (Candidates.Num() < 2)
				{
					if ((NowReal - State->PhaseStartReal) > 25.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[PICKLERPULL] VERDICT: INVALID — found %d living non-carrier enemies and this "
							     "needs 2. Run with bots on the other team."), Candidates.Num());
						return false;
					}
					return true;
				}

				State->Inside.Pawn = Candidates[0];
				State->InRing.Pawn = Candidates[1];
				State->CarrierPullsAtStart = TraceOyster::CarrierTally().PicklerPulls;

				UE_LOG(LogTraceGame, Display,
					TEXT("[PICKLERPULL] Jar lands at %s. Victim A stands %.0f uu out (inside BOTH radii); victim B "
					     "stands %.0f uu out — outside the pre-v19 %.0f and inside the shipped %.0f."),
					*State->JarOrigin.ToCompactString(), State->Inside.PlannedDistance,
					State->InRing.PlannedDistance, State->LegacyRadius, State->ShippedRadius);

				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}

			ATraceCharacter* VictimA = State->Inside.Pawn.Get();
			ATraceCharacter* VictimB = State->InRing.Pawn.Get();
			if (VictimA == nullptr || VictimB == nullptr || !VictimA->IsAlive() || !VictimB->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[PICKLERPULL] ABORTED: a victim died or left mid-test."));
				RestorePullRadius(State->ShippedRadius);
				return false;
			}

			// ---- Phase 2: hold everybody still ---------------------------------------------------------
			if (State->Phase == 2)
			{
				if (UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>())
				{
					MutableSettings->OysterPicklerPullRadiusUU =
						(State->Arm == 0) ? State->LegacyRadius : State->ShippedRadius;
				}
				OysterSet->DebugDestroyAllJars();

				// Re-posed EVERY frame, and with the velocity zeroed: these are bots, they are running
				// somewhere, and a bot that happens to be sprinting toward the jar would read exactly
				// like a pull. Starting them from a standstill is what makes the number below mean
				// "launched" and not "was already moving".
				PosePawn(VictimA, State->InsideAnchor, State->VictimFacing);
				PosePawn(VictimB, State->RingAnchor, State->VictimFacing);

				if (++State->SettleFrames < 10)
				{
					return true;
				}

				State->Inside.ActualDistance[State->Arm] = FVector::Dist(VictimA->GetActorLocation(), State->JarOrigin);
				State->InRing.ActualDistance[State->Arm] = FVector::Dist(VictimB->GetActorLocation(), State->JarOrigin);

				const int32 OtherPullsBefore = TraceOyster::OtherTally().PicklerPulls;

				// The landing itself. A Pickler placed with no launch velocity is grounded on its first
				// frame, so Land() -> FireLandingEffect() runs inside this call and the tally below is
				// read on the same frame it was written.
				OysterSet->DebugSpawnJarAt(State->JarOrigin, /*bPickler*/ true);

				State->OtherPullsDelta[State->Arm] = TraceOyster::OtherTally().PicklerPulls - OtherPullsBefore;

				State->SampleFrames = 0;
				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: how hard was each of them thrown? ---------------------------------------------
			if (State->Phase == 3)
			{
				// LaunchCharacter lands as a pending launch and shows up as VELOCITY on the next movement
				// tick, so the peak over the first handful of frames is the honest reading. Only the
				// component TOWARD the jar counts: a pull is a direction, not a speed.
				const FVector TowardJarA = (State->JarOrigin - VictimA->GetActorLocation()).GetSafeNormal();
				const FVector TowardJarB = (State->JarOrigin - VictimB->GetActorLocation()).GetSafeNormal();

				State->Inside.SpeedTowardJar[State->Arm] = FMath::Max(State->Inside.SpeedTowardJar[State->Arm],
					static_cast<float>(FVector::DotProduct(VictimA->GetVelocity(), TowardJarA)));
				State->InRing.SpeedTowardJar[State->Arm] = FMath::Max(State->InRing.SpeedTowardJar[State->Arm],
					static_cast<float>(FVector::DotProduct(VictimB->GetVelocity(), TowardJarB)));

				if (++State->SampleFrames < 8)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[PICKLERPULL] %s arm (radius %.0f uu): victim A at %.0f uu was thrown at %.0f uu/s toward "
					     "the jar; victim B at %.0f uu was thrown at %.0f uu/s. Non-carrier pulls this landing: %d."),
					(State->Arm == 0) ? TEXT("PRE-v19") : TEXT("SHIPPED"),
					(State->Arm == 0) ? State->LegacyRadius : State->ShippedRadius,
					State->Inside.ActualDistance[State->Arm], State->Inside.SpeedTowardJar[State->Arm],
					State->InRing.ActualDistance[State->Arm], State->InRing.SpeedTowardJar[State->Arm],
					State->OtherPullsDelta[State->Arm]);

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->SettleFrames = 0;
					State->Phase = 2;
					State->PhaseStartReal = NowReal;
					return true;
				}
				State->Phase = 4;
			}

			// ---- Phase 4: verdict ------------------------------------------------------------------------
			RestorePullRadius(State->ShippedRadius);
			OysterSet->DebugDestroyAllJars();

			// A launch is 1300 uu/s and a bot accelerating from the standstill we forced is nowhere near
			// it within eight frames, so this threshold separates "yanked" from "walked" with room to
			// spare in both directions.
			constexpr float PulledSpeedThreshold = 500.f;

			const bool bInsideA0 = State->Inside.ActualDistance[0] < State->LegacyRadius;
			const bool bInsideA1 = State->Inside.ActualDistance[1] < State->LegacyRadius;
			const bool bRingStaged =
				State->InRing.ActualDistance[0] > State->LegacyRadius + 20.f
				&& State->InRing.ActualDistance[0] < State->ShippedRadius - 20.f
				&& State->InRing.ActualDistance[1] > State->LegacyRadius + 20.f
				&& State->InRing.ActualDistance[1] < State->ShippedRadius - 20.f;

			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERPULL] --- THE FIXTURE PROVING ITSELF FIRST"));
			State->Check(bInsideA0 && bInsideA1,
				FString::Printf(TEXT("victim A really did stand inside the OLD radius on both arms (%.0f uu, %.0f uu "
					"against %.0f uu)"), State->Inside.ActualDistance[0], State->Inside.ActualDistance[1],
					State->LegacyRadius));
			State->Check(bRingStaged,
				FString::Printf(TEXT("victim B really did stand in the RING between the two radii on both arms "
					"(%.0f uu, %.0f uu, between %.0f and %.0f) — without this there is nothing here that the change "
					"could possibly move"), State->InRing.ActualDistance[0], State->InRing.ActualDistance[1],
					State->LegacyRadius, State->ShippedRadius));
			State->Check(State->Inside.SpeedTowardJar[0] > PulledSpeedThreshold
				&& State->Inside.SpeedTowardJar[1] > PulledSpeedThreshold,
				FString::Printf(TEXT("...and the pull itself works on BOTH arms for the man standing well inside it "
					"(%.0f uu/s, %.0f uu/s) — so a zero below is a radius, not a broken ability"),
					State->Inside.SpeedTowardJar[0], State->Inside.SpeedTowardJar[1]));

			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERPULL] --- THE REPRODUCTION: the pre-v19 reach"));
			State->Check(State->InRing.SpeedTowardJar[0] < PulledSpeedThreshold,
				FString::Printf(TEXT("at %.0f uu the pre-v19 Pickler left victim B standing (%.0f uu/s) — he took the "
					"30 damage and simply was not dragged"), State->InRing.ActualDistance[0],
					State->InRing.SpeedTowardJar[0]));

			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERPULL] --- THE CHANGE: \"greater pull radius\""));
			State->Check(State->InRing.SpeedTowardJar[1] > PulledSpeedThreshold,
				FString::Printf(TEXT("at the shipped %.0f uu the same man in the same spot is yanked in at %.0f uu/s"),
					State->ShippedRadius, State->InRing.SpeedTowardJar[1]));
			State->Check(State->OtherPullsDelta[1] > State->OtherPullsDelta[0],
				FString::Printf(TEXT("and the landing pulled strictly more people than it used to (%d -> %d)"),
					State->OtherPullsDelta[0], State->OtherPullsDelta[1]));

			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERPULL] --- THE FOUNDING INVARIANT, AT THE BIGGER RADIUS"));
			State->Check(TraceOyster::CarrierTally().PicklerPulls == State->CarrierPullsAtStart,
				FString::Printf(TEXT("no Core carrier was pulled by either arm (carrier pull tally still %d). This is "
					"a guard and not the proof — Trace.Oyster.CarrierTest stages an actual carrier"),
					State->CarrierPullsAtStart));

			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERPULL] ===== %d passed, %d failed. ====="),
				State->Passed, State->Failed);
			UE_LOG(LogTraceGame, Display, TEXT("[PICKLERPULL] VERDICT: %s"),
				(State->Failed == 0 && State->Passed > 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
			return false;
		}));
	}

	FAutoConsoleCommand CmdPicklerPullTest(
		TEXT("Trace.Oyster.PicklerPullTest"),
		TEXT("Dev only, server only. SPEC v19 §4.4 \"greater pull radius\": lands one Pickler twice, at the pre-v19 "
		     "260 uu and at the shipped radius, with an enemy standing in the ring between them, and measures how "
		     "hard each enemy is actually thrown."),
		FConsoleCommandDelegate::CreateStatic(&RunPicklerPullTest));

	// =============================================================================================
	// Trace.Oyster.ETest — SPEC v26 §6, BOTH HALVES, RED ARM FIRST
	// =============================================================================================
	//
	//   "Change Oyster's E cooldown to reset everytime he poisons someone. The E jar's should explode
	//    once the pull animation finishes, rather than waiting for a jump to trigger them."
	//
	// TWO FIXTURES, run back to back on each arm, and they are deliberately kept APART because the
	// obvious single fixture measures neither cleanly:
	//
	//   FIXTURE 1 (§6b, the fuse)    A Pickler jar landed with NOBODY within reach of it. That
	//                                isolation is the whole point. Land a Pickler on top of an enemy
	//                                and the pre-v26 jar breaks anyway — the victim is dragged inside
	//                                the 100 uu break radius by the pull — so both arms would go bang
	//                                and the harness would be measuring the pull, not the fuse. Alone,
	//                                the old jar has no reason to break at all and the difference is
	//                                the entire behaviour: does the actor still exist a second later.
	//
	//   FIXTURE 2 (§6a, the refund)  E put on its full cooldown, then a DASH jar dropped on an enemy's
	//                                feet so he breaks it himself. A dash jar rather than a Pickler,
	//                                because §6a says "everytime he POISONS someone" and not "every
	//                                time he lands an E" — proving it on the passive jar proves the
	//                                rule was put where the poison is rather than where the ability is.
	//
	// THE FIXTURES PROVE THEMSELVES: fixture 1 refuses to read anything into an arm whose jar never
	// landed, and fixture 2 refuses to read anything into an arm where the victim was never actually
	// poisoned. A cooldown that reads zero because no poison ever happened is not a refund.

	struct FEArmResult
	{
		bool  bJarLanded = false;
		bool  bJarAliveAfterFuse = false;
		float FuseSeconds = 0.f;

		bool  bPoisonLanded = false;
		float CooldownBefore = 0.f;
		float CooldownAfter = 0.f;
	};

	struct FEState
	{
		int32 Phase = 0;
		int32 Arm = 0;                 // 0 = LEGACY (pre-v26), 1 = SHIPPED
		int32 SettleFrames = 0;
		double PhaseStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;
		TWeakObjectPtr<ATraceCharacter> Victim;
		TWeakObjectPtr<ATraceOysterJar> WatchedJar;

		FVector Anchor = FVector::ZeroVector;
		FVector FarAnchor = FVector::ZeroVector;    // fixture 1 parks the victim out here
		FVector NearAnchor = FVector::ZeroVector;   // fixture 2 brings him back in
		FRotator Facing = FRotator::ZeroRotator;

		FEArmResult Arms[2];

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERE]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	/** However this run ends, the shipped arm is what is left switched on. */
	void RestoreEArm()
	{
		SetIntCVar(TEXT("Trace.Oyster.LegacyE"), 0);
	}

	void RunETest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERE] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("OYSTERE"));

		TSharedPtr<FEState> State = MakeShared<FEState>();
		State->PhaseStartReal = FPlatformTime::Seconds();

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[OYSTERE] ===== spec v26 §6. Arm 0 = LEGACY (pre-v26: no fuse, no refund) — it MUST fail. "
			     "Arm 1 = SHIPPED. Pull %.0f uu at %.0f uu/s => %.2fs of travel; fuse x%.2f = %.2fs; jar lifetime "
			     "%.1fs; break radius %.0f uu; E cooldown %.0fs. ====="),
			Settings.OysterPicklerPullRadiusUU, Settings.OysterPicklerPullSpeed,
			(Settings.OysterPicklerPullSpeed > UE_SMALL_NUMBER)
				? Settings.OysterPicklerPullRadiusUU / Settings.OysterPicklerPullSpeed : 0.f,
			Settings.OysterPicklerDetonateDelayScale, ATraceOysterJar::GetPicklerDetonateDelaySeconds(),
			Settings.OysterJarLifetimeSeconds, Settings.OysterJarBreakRadiusUU,
			Settings.OysterPicklerCooldownSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERE] ABORTED: the world went away."));
				RestoreEArm();
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: become Oyster ---------------------------------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Oyster)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Oyster);
				}
				UTraceAbilitySetOyster* Set = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;

				if (Set == nullptr || Human->GetOwningCharacter() == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[OYSTERE] VERDICT: INVALID — no human player could be made Oyster. Needs a "
							     "mode-B match with characters enabled, run EARLY before bots claim characters."));
						RestoreEArm();
						return false;
					}
					return true;
				}

				State->Subject = Human;
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			UTraceAbilityComponent* Comp = State->Subject.Get();
			UTraceAbilitySetOyster* OysterSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (OysterSet == nullptr || MyPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERE] ABORTED: Oyster went away mid-test."));
				RestoreEArm();
				return false;
			}

			// ---- Phase 1: ground, one enemy, two standing spots ---------------------------------
			if (State->Phase == 1)
			{
				const UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement();
				if (MoveComp == nullptr || !MoveComp->IsMovingOnGround())
				{
					if ((NowReal - State->PhaseStartReal) > 12.0)
					{
						UE_LOG(LogTraceGame, Error, TEXT("[OYSTERE] VERDICT: INVALID — Oyster never reached the ground."));
						RestoreEArm();
						return false;
					}
					return true;
				}

				ATraceCharacter* Found = nullptr;
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || Candidate == MyPawn || !Candidate->IsAlive()
						|| Candidate->IsCarrier() || Candidate->GetTeam() == MyPawn->GetTeam())
					{
						continue;   // a carrier is refused by the choke point by design; that is §4, not §6
					}
					Found = Candidate;
					break;
				}

				if (Found == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 25.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[OYSTERE] VERDICT: INVALID — no living non-carrier enemy to poison. Run with bots "
							     "on the other team."));
						RestoreEArm();
						return false;
					}
					return true;
				}

				const FRotator Open = FindOpenFacing(TickWorld, MyPawn);
				const FVector Out = FRotator(0.f, Open.Yaw, 0.f).Vector();

				State->Victim = Found;
				State->Anchor = MyPawn->GetActorLocation();
				State->Facing = Open;
				// FAR: clear of the poison burst (380 uu) AND of the Pickler's damage radius (420 uu),
				// so fixture 1's jar genuinely has nobody to interact with. NEAR: well inside the
				// 100 uu break radius, so fixture 2's victim breaks the dash jar by standing on it.
				State->FarAnchor  = State->Anchor + Out * 1400.f;
				State->NearAnchor = State->Anchor + Out * 40.f;

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERE] Oyster at %s; victim %s. Fixture 1 parks him 1400 uu out (nothing can reach the "
					     "jar); fixture 2 stands him 40 uu away, on the jar."),
					*State->Anchor.ToCompactString(), *GetNameSafe(Found));

				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}

			ATraceCharacter* VictimPawn = State->Victim.Get();
			if (VictimPawn == nullptr || !VictimPawn->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERE] ABORTED: the victim died or went away mid-test."));
				RestoreEArm();
				return false;
			}

			// ---- Phase 2: arm the switch, stage fixture 1 ---------------------------------------
			if (State->Phase == 2)
			{
				SetIntCVar(TEXT("Trace.Oyster.LegacyE"), (State->Arm == 0) ? 1 : 0);
				OysterSet->DebugDestroyAllJars();
				PosePawn(MyPawn, State->Anchor, State->Facing);
				PosePawn(VictimPawn, State->FarAnchor, State->Facing);

				if (++State->SettleFrames < 6)
				{
					return true;   // a bot walks; the pose is re-applied until it stops mattering
				}
				State->SettleFrames = 0;

				FEArmResult& Result = State->Arms[State->Arm];
				Result.FuseSeconds = ATraceOysterJar::GetPicklerDetonateDelaySeconds();

				ATraceOysterJar* JarActor = OysterSet->DebugSpawnJarAt(State->Anchor, /*bPickler*/ true);
				State->WatchedJar = JarActor;
				Result.bJarLanded = (JarActor != nullptr) && JarActor->IsGrounded()
					&& JarActor->HasFiredLandingEffect();

				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: wait out the fuse with a wide margin, then look ------------------------
			if (State->Phase == 3)
			{
				PosePawn(VictimPawn, State->FarAnchor, State->Facing);   // he must not wander into it

				FEArmResult& Result = State->Arms[State->Arm];
				const double Margin = FMath::Max(1.0, static_cast<double>(Result.FuseSeconds) * 3.0);
				if ((NowReal - State->PhaseStartReal) < Margin)
				{
					return true;
				}

				Result.bJarAliveAfterFuse = State->WatchedJar.IsValid();

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERE] arm=%d FIXTURE 1 (the fuse): jar landed and fired its impact %d; %.2fs fuse; "
					     "%.1fs later the jar actor still exists %d."),
					State->Arm, Result.bJarLanded ? 1 : 0, Result.FuseSeconds, Margin,
					Result.bJarAliveAfterFuse ? 1 : 0);

				OysterSet->DebugDestroyAllJars();
				State->Phase = 4;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 4: stage fixture 2 -------------------------------------------------------
			if (State->Phase == 4)
			{
				PosePawn(MyPawn, State->Anchor, State->Facing);
				PosePawn(VictimPawn, State->NearAnchor, State->Facing);

				if (++State->SettleFrames < 6)
				{
					return true;
				}
				State->SettleFrames = 0;

				FEArmResult& Result = State->Arms[State->Arm];

				// The full cooldown, put on directly rather than by pressing E: pressing E would also
				// throw a jar, and this fixture is about the poison paying the cooldown back, not about
				// what put it on.
				Comp->DebugSetActivatedCooldown(FMath::Max(1.f, UTraceSettings::Get().OysterPicklerCooldownSeconds));
				Result.CooldownBefore = Comp->GetActivatedCooldownRemaining();

				// A DASH jar at the victim's feet. He is an enemy inside the break radius, so his own
				// next tick breaks it and the poison lands on him.
				OysterSet->DebugSpawnJarAt(State->NearAnchor, /*bPickler*/ false);

				State->Phase = 5;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 5: measure the refund ----------------------------------------------------
			if (State->Phase == 5)
			{
				PosePawn(VictimPawn, State->NearAnchor, State->Facing);

				if ((NowReal - State->PhaseStartReal) < 0.75)
				{
					return true;
				}

				FEArmResult& Result = State->Arms[State->Arm];
				Result.bPoisonLanded = (UTraceOysterPoisonComponent::Find(VictimPawn) != nullptr);
				Result.CooldownAfter = Comp->GetActivatedCooldownRemaining();

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERE] arm=%d FIXTURE 2 (the refund): the victim really is poisoned %d; E cooldown "
					     "%.1fs -> %.1fs."),
					State->Arm, Result.bPoisonLanded ? 1 : 0, Result.CooldownBefore, Result.CooldownAfter);

				OysterSet->DebugDestroyAllJars();
				Comp->DebugSetActivatedCooldown(0.f);

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Phase = 2;
					State->PhaseStartReal = NowReal;
					return true;
				}

				State->Phase = 6;
				return true;
			}

			// ---- Phase 6: verdict ---------------------------------------------------------------
			RestoreEArm();

			const FEArmResult& Red = State->Arms[0];
			const FEArmResult& Green = State->Arms[1];

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERE] --- THE FIXTURES PROVING THEMSELVES FIRST"));
			State->Check(Red.bJarLanded && Green.bJarLanded,
				TEXT("both arms actually landed a Pickler jar and fired its impact — without that, "
				     "'the jar is gone' would just mean the throw failed"));
			State->Check(Red.bPoisonLanded && Green.bPoisonLanded,
				TEXT("both arms actually poisoned the victim — a cooldown at zero because nothing was "
				     "poisoned is not a refund"));
			State->Check(Red.CooldownBefore > 1.f && Green.CooldownBefore > 1.f,
				FString::Printf(TEXT("both arms started fixture 2 with E genuinely on cooldown (%.1fs, %.1fs)"),
					Red.CooldownBefore, Green.CooldownBefore));

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERE] --- THE REPRODUCTION: the pre-v26 E"));
			State->Check(Red.bJarAliveAfterFuse,
				TEXT("§6b RED: the old Pickler jar was still lying there long after its pull had finished, "
				     "waiting for a touch or a jump — which is exactly what the section says to stop"));
			State->Check(Red.CooldownAfter > 1.f,
				FString::Printf(TEXT("§6a RED: poisoning an enemy refunded the old E nothing (%.1fs still to run)"),
					Red.CooldownAfter));

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERE] --- THE CHANGE"));
			State->Check(!Green.bJarAliveAfterFuse,
				FString::Printf(TEXT("§6b: the same jar in the same spot detonated on its own %.2fs after landing "
					"and no longer exists"), Green.FuseSeconds));
			State->Check(Green.CooldownAfter <= 0.f,
				FString::Printf(TEXT("§6a: the same poison on the same victim reset E from %.1fs to %.1fs"),
					Green.CooldownBefore, Green.CooldownAfter));

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERE] ===== %d passed, %d failed. ====="),
				State->Passed, State->Failed);
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERE] VERDICT: %s"),
				(State->Failed == 0 && State->Passed > 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Oyster.EPressRepro — SPEC v28 §4, THE PLAYER'S SIDE OF "E STILL DOES NOT RESET"
	// =============================================================================================
	//
	//   "Oyster's E is not resetting when he poisons someone. Demo 23 asked for this and it was
	//    REPORTED DELIVERED. It does not work."
	//
	// WHY A SECOND HARNESS WHEN Trace.Oyster.ETest ALREADY COVERS §6a — AND WHY THAT ONE'S GREEN IS
	// THE REASON THIS BUG SHIPPED. ETest poisons a BOT ON THE OPPOSING TEAM. That is the one victim
	// shape v26's refund test accepts, so ETest could only ever come back green. A player checking an
	// ability checks it in the PRACTICE RANGE, whose five dummies are deliberately ETraceTeam::None,
	// and v26's test threw the refund away for anybody without a team. Same ability, same key, same
	// poison, opposite outcome — and no harness in the project stood where the player was standing.
	//
	// So this one stands there. TWO ARMS x TWO FIXTURES, all four in one process:
	//
	//   ARM 0  Trace.Oyster.LegacyRefundTeamTest 1 — the v26 test. On a teamless victim it must FAIL
	//          to refund, and that failure IS the reported bug, reproduced before anything is fixed.
	//   ARM 1  the shipped v28 test. Same fixtures, same victim, and E must come back.
	//
	//   FIXTURE A (the E jar)   press E for real -> the thrown Pickler lands, the enemy is stood on
	//                           it, it bursts and poisons him. The player's own loop, end to end.
	//   FIXTURE B (a dash jar)  press E for real, then drop a DASH jar on him through the shipping
	//                           SpawnJar. A second poison ROUTE onto the same rule, so "the refund is
	//                           on the wrong path" can be told apart from "the refund refuses him".
	//
	// THE KEY IS PRESSED FOR REAL, through Trace.SimInput, and that is the other half of why this is
	// a separate command: ETest puts the cooldown on with Comp->DebugSetActivatedCooldown(), so it
	// never exercises UTraceAbilityComponent::TryActivate — the function a player's E actually runs,
	// which throws the jar first and writes the cooldown afterwards. A harness that never presses the
	// key cannot see anything that goes wrong on the key's path.
	//
	// IN A REAL MATCH BOTH ARMS PASS, and the run says so rather than hiding it: with an enemy on a
	// real team the v26 test is satisfied, which is exactly why this survived Demo 23. Run it in the
	// practice range (Scripts/run-practice-range.sh) to see the arms disagree.

	struct FEPressFixture
	{
		bool  bPressed = false;          // the key injection was actually issued
		float CooldownAfterPress = 0.f;  // what the press bought. ~20s, or the press did nothing
		bool  bPoisonLanded = false;     // the victim really has a poison component
		float CooldownWhenPoisoned = -1.f;
		float CooldownAfterSettle = -1.f;
		float SecondsToPoison = -1.f;
	};

	struct FEPressState
	{
		int32 Phase = 0;
		int32 Arm = 0;                   // 0 = LEGACY team test (v26), 1 = SHIPPED (v28 §4)
		int32 Fixture = 0;               // 0 = the E jar's own poison, 1 = a dash jar's
		int32 SettleFrames = 0;
		double PhaseStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;
		TWeakObjectPtr<ATraceCharacter> Victim;
		ETraceTeam VictimTeam = ETraceTeam::None;
		ETraceTeam MyTeam = ETraceTeam::None;

		FVector Anchor = FVector::ZeroVector;
		FVector VictimAnchor = FVector::ZeroVector;
		FVector VictimHome = FVector::ZeroVector;
		FRotator Facing = FRotator::ZeroRotator;

		FEPressFixture Results[2][2];    // [arm][fixture]

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTEREPRESS]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	/** However this run ends, the SHIPPED refund test is what is left switched on. */
	void RestoreRefundArm()
	{
		SetIntCVar(TEXT("Trace.Oyster.LegacyRefundTeamTest"), 0);
	}

	/** The E key, through the real input pipeline. Returns false when there is no controller to press it on. */
	bool PressAbilityKeyForReal(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr || GEngine == nullptr)
		{
			return false;
		}

		APlayerController* PC = WorldPtr->GetFirstPlayerController();
		if (PC == nullptr)
		{
			return false;
		}

		// SAY IT OUT LOUD RATHER THAN LET IT LOOK LIKE THE ABILITY IGNORED THE KEY. A select screen
		// or an options overlay swallows game input, and a press that never reached the ability at
		// all would otherwise be reported here as "E did not fire" — a different bug entirely, and
		// one this project has chased before.
		if (const ATracePlayerController* TracePC = Cast<ATracePlayerController>(PC))
		{
			if (TracePC->IsGameInputSuppressed())
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[OYSTEREPRESS] game input is SUPPRESSED (a menu or the select screen is up). The E "
					     "press below will not reach the ability; close it and re-run."));
			}
		}

		// Trace.SimInput injects at the input subsystem, so a bind that does not exist, is not mapped
		// or is not routed produces nothing at all — which is itself a result worth having.
		GEngine->Exec(WorldPtr, TEXT("Trace.SimInput E 0.10"));
		return true;
	}

	void RunEPressRepro()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[OYSTEREPRESS] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("OYSTEREPRESS"));

		// §6b stays SHIPPED throughout. This command reproduces a report about §6a's refund, and a
		// stale Trace.Oyster.LegacyE left at 1 by an aborted ETest run would make it "reproduce" a bug
		// that is only that arm doing its job.
		SetIntCVar(TEXT("Trace.Oyster.LegacyE"), 0);
		SetIntCVar(TEXT("Trace.Oyster.LegacyRefundTeamTest"), 1);   // arm 0 first: the v26 test

		TSharedPtr<FEPressState> State = MakeShared<FEPressState>();
		State->PhaseStartReal = FPlatformTime::Seconds();

		UE_LOG(LogTraceGame, Display,
			TEXT("[OYSTEREPRESS] ===== spec v28 §4. Arm 0 = the v26 refund test (must FAIL to refund on a "
			     "teamless victim), arm 1 = SHIPPED. E is PRESSED FOR REAL (Trace.SimInput E) in every "
			     "fixture. E cooldown %.0fs; Pickler fuse %.2fs; poison radius %.0f uu. ====="),
			UTraceSettings::Get().OysterPicklerCooldownSeconds,
			ATraceOysterJar::GetPicklerDetonateDelaySeconds(),
			UTraceSettings::Get().OysterPoisonRadiusUU);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTEREPRESS] ABORTED: the world went away."));
				RestoreRefundArm();
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: become Oyster, find somebody to poison --------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Oyster)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Oyster);
				}

				UTraceAbilitySetOyster* Set = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;
				ATraceCharacter* MyPawn = (Human != nullptr) ? Human->GetOwningCharacter() : nullptr;

				if (Set == nullptr || MyPawn == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[OYSTEREPRESS] VERDICT: INVALID — no human player could be made Oyster."));
						RestoreRefundArm();
						return false;
					}
					return true;
				}

				// *** A TEAMLESS VICTIM IS PREFERRED, AND THAT IS THE WHOLE POINT OF THE FIXTURE. ***
				// The practice range's dummies are ETraceTeam::None and are the targets a player
				// tests an ability on; they are also the ones v26's refund test refuses. In a real
				// match there are none, and this falls through to an ordinary enemy — where, as the
				// note above says, BOTH arms pass and always did.
				ATraceCharacter* Victim = nullptr;
				ATraceCharacter* Fallback = nullptr;
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || Candidate == MyPawn || !Candidate->IsAlive())
					{
						continue;
					}
					if (Candidate->GetTeam() == ETraceTeam::None)
					{
						Victim = Candidate;   // the practice dummy. Take it and stop looking.
						break;
					}
					if (Candidate->GetTeam() != MyPawn->GetTeam() && Fallback == nullptr)
					{
						Fallback = Candidate;
					}
				}
				if (Victim == nullptr)
				{
					Victim = Fallback;
				}

				if (Victim == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[OYSTEREPRESS] VERDICT: INVALID — nobody to poison. Needs the practice range "
							     "(five dummies) or a match with bots on the other side."));
						RestoreRefundArm();
						return false;
					}
					return true;
				}

				State->Subject = Human;
				State->Victim = Victim;
				State->MyTeam = MyPawn->GetTeam();
				State->VictimTeam = Victim->GetTeam();
				State->Anchor = MyPawn->GetActorLocation();
				State->Facing = FRotator(0.f, MyPawn->GetActorRotation().Yaw, 0.f);
				State->VictimHome = State->Anchor + State->Facing.Vector() * 600.f;
				State->VictimAnchor = State->VictimHome;
				State->Phase = 1;
				State->PhaseStartReal = NowReal;

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTEREPRESS] subject %s (team %s) vs victim %s (team %s)%s."),
					*GetNameSafe(Human->GetOwner()), *TraceTeamName(State->MyTeam).ToString(),
					*GetNameSafe(Victim), *TraceTeamName(State->VictimTeam).ToString(),
					(State->VictimTeam == ETraceTeam::None)
						? TEXT(" — a TEAMLESS victim: this is the fixture the report is about")
						: TEXT(" — an ordinary enemy: both arms are expected to pass here"));
				return true;
			}

			UTraceAbilityComponent* Comp = State->Subject.Get();
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			ATraceCharacter* VictimPawn = State->Victim.Get();
			UTraceAbilitySetOyster* OysterSet = (Comp != nullptr)
				? Comp->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;

			// A DEAD OR RESPAWNED VICTIM IS RE-ACQUIRED, NOT AN ABORT. The practice range's dummies
			// die and come back as NEW pawns ("they take damage and come back where they fell"), so a
			// run that insisted on the same pointer would abort halfway through for a fixture that is
			// working exactly as intended.
			if (VictimPawn == nullptr || !VictimPawn->IsAlive())
			{
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate != nullptr && Candidate != MyPawn && Candidate->IsAlive()
						&& Candidate->GetTeam() == State->VictimTeam)
					{
						VictimPawn = Candidate;
						State->Victim = Candidate;
						break;
					}
				}
			}

			if (Comp == nullptr || MyPawn == nullptr || VictimPawn == nullptr || OysterSet == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[OYSTEREPRESS] VERDICT: INVALID — a pawn went away mid-run."));
				RestoreRefundArm();
				return false;
			}

			FEPressFixture& Fixture = State->Results[State->Arm][State->Fixture];

			// ---- Phase 1: pose both pawns, clear the field --------------------------------------
			if (State->Phase == 1)
			{
				State->VictimAnchor = State->VictimHome;
				PosePawn(MyPawn, State->Anchor, State->Facing);
				PosePawn(VictimPawn, State->VictimAnchor, State->Facing);
				OysterSet->DebugDestroyAllJars();

				// *** THE VICTIM STARTS EACH FIXTURE CLEAN, AND THE FIRST RUN OF THIS HARNESS PROVED
				//     WHY. *** A poison lasts 4 s and a fixture takes about 2, so fixtures 2, 3 and 4
				// each opened with the previous fixture's poison still on him. "The victim IS
				// poisoned" then fired on the first frame — 0.04 s after a throw whose jar was still
				// in the air — and the fixture finished measuring before the poison it was actually
				// waiting for had happened. Every number after the first was therefore a reading of
				// the wrong event, and the shipped arm read as a FAIL for a refund that had not been
				// asked for yet.
				//
				// Removed rather than waited out: waiting 4 s per fixture would make the run four
				// times longer and would still be a guess about a duration knob.
				if (UTraceOysterPoisonComponent* Stale = UTraceOysterPoisonComponent::Find(VictimPawn))
				{
					Stale->DestroyComponent();
				}

				// ...and alive, with his health back. Four Pickler impacts plus four poisons is 216
				// damage against 100 HP, so without this the victim dies mid-run and the harness
				// reports "a pawn went away" instead of a measurement.
				if (UTraceHealthComponent* Health = VictimPawn->FindComponentByClass<UTraceHealthComponent>())
				{
					Health->ResetHealth();
				}

				if (++State->SettleFrames < 6)
				{
					return true;
				}
				State->SettleFrames = 0;

				// Start every fixture from READY, so "the cooldown after the press" is a measurement
				// of the press and not of whatever was left over.
				Comp->DebugSetActivatedCooldown(0.f);
				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 2: PRESS E, FOR REAL -----------------------------------------------------
			if (State->Phase == 2)
			{
				Fixture.bPressed = PressAbilityKeyForReal(TickWorld);
				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTEREPRESS] arm %d fixture %s: pressed E through the real input pipeline (%s)."),
					State->Arm, (State->Fixture == 0) ? TEXT("A/E-jar") : TEXT("B/dash-jar"),
					Fixture.bPressed ? TEXT("injected") : TEXT("*** NO LOCAL CONTROLLER ***"));

				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: what did the press buy? ------------------------------------------------
			if (State->Phase == 3)
			{
				if ((NowReal - State->PhaseStartReal) < 0.35)
				{
					return true;
				}

				Fixture.CooldownAfterPress = Comp->GetActivatedCooldownRemaining();
				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTEREPRESS] arm %d fixture %d: E cooldown after the press = %.1fs."),
					State->Arm, State->Fixture, Fixture.CooldownAfterPress);

				// Fixture B poisons by a DASH jar instead of by the thrown one, so the jar the press
				// put in the air is removed first — otherwise both routes fire and the log cannot say
				// which one paid.
				if (State->Fixture == 1)
				{
					OysterSet->DebugDestroyAllJars();
					ATraceOysterJar* DashJar =
						OysterSet->DebugSpawnJarAt(VictimPawn->GetActorLocation(), /*bPickler*/ false);

					// *** A DASH JAR CANNOT BE BROKEN BY A TEAMLESS PAWN, AND THAT IS A REAL FINDING
					//     RATHER THAN A HARNESS DETAIL. *** ATraceOysterJar::FindToucher only accepts
					//     an IsEnemyOfOwner candidate, which requires BOTH teams to be set and
					//     different — so in the practice range, where every dummy is ETraceTeam::None,
					//     a dash jar lies at a dummy's feet untouched until it expires. (The Pickler
					//     jar is unaffected: §6b's fuse breaks it with nobody involved, which is why
					//     fixture A works there and this one needs help.)
					//
					// So the break is forced through ServerBreakNow — the SAME function the touch
					// test, the jar-jump and every other break call — rather than by faking a poison.
					// The burst, the cloud, the choke point and the refund are all the shipping ones;
					// only the trigger is the harness's.
					if (DashJar != nullptr && State->VictimTeam == ETraceTeam::None)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[OYSTEREPRESS] the victim is TEAMLESS, so ATraceOysterJar::FindToucher will "
							     "never break this dash jar (it needs an enemy with a team). Forcing the break "
							     "through the shipping ServerBreakNow so the poison ROUTE is still measured."));
						DashJar->ServerBreakNow(TEXT("harness: a teamless victim cannot trigger the touch test"));
					}
				}

				State->Phase = 4;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 4: watch for the poison, and watch the cooldown across it ------------------
			if (State->Phase == 4)
			{
				// *** THE VICTIM IS BROUGHT TO WHERE THE THROW ACTUALLY LANDED. ***
				//
				// Fixture A throws with the SHIPPING lob (OysterPicklerThrowSpeed 1900 uu/s, up bias
				// 0.35) and that arc carries far further than any sane staging distance. Standing the
				// victim at a guessed range would produce a fixture that reliably measures nothing,
				// which is worse than no fixture at all — so the staging follows the jar rather than
				// predicting it: the moment the E jar is on the ground, the victim is stood on it.
				//
				// NOTHING ABOUT THE POISON IS FAKED BY THIS. The jar is the one the key press threw
				// and it bursts through its own shipping path (its §6b fuse, or his touch).
				if (State->Fixture == 0)
				{
					for (TActorIterator<ATraceOysterJar> JarIt(TickWorld); JarIt; ++JarIt)
					{
						const ATraceOysterJar* Jar = *JarIt;
						if (IsValid(Jar) && Jar->IsGrounded())
						{
							FVector Landed = Jar->GetActorLocation();
							Landed.Z = State->VictimHome.Z;
							State->VictimAnchor = Landed;
							break;
						}
					}
				}

				PosePawn(VictimPawn, State->VictimAnchor, State->Facing);   // he must stay in the blast

				const bool bPoisonedNow = (UTraceOysterPoisonComponent::Find(VictimPawn) != nullptr);
				if (bPoisonedNow && !Fixture.bPoisonLanded)
				{
					Fixture.bPoisonLanded = true;
					Fixture.SecondsToPoison = static_cast<float>(NowReal - State->PhaseStartReal);
					Fixture.CooldownWhenPoisoned = Comp->GetActivatedCooldownRemaining();

					UE_LOG(LogTraceGame, Display,
						TEXT("[OYSTEREPRESS] arm %d fixture %d: the victim IS poisoned (%.2fs after the throw); "
						     "E reads %.1fs on that very frame."),
						State->Arm, State->Fixture, Fixture.SecondsToPoison, Fixture.CooldownWhenPoisoned);
				}

				// A whole second past the poison, because "fired and then overwritten" and "never
				// fired" look identical on the frame itself.
				const double Deadline = Fixture.bPoisonLanded
					? (1.0 + static_cast<double>(Fixture.SecondsToPoison))
					: 4.0;
				if ((NowReal - State->PhaseStartReal) < Deadline)
				{
					return true;
				}

				Fixture.CooldownAfterSettle = Comp->GetActivatedCooldownRemaining();
				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTEREPRESS] arm %d fixture %d: poisoned=%d | E after the press %.1fs -> at the "
					     "poison %.1fs -> a second later %.1fs."),
					State->Arm, State->Fixture, Fixture.bPoisonLanded ? 1 : 0, Fixture.CooldownAfterPress,
					Fixture.CooldownWhenPoisoned, Fixture.CooldownAfterSettle);

				OysterSet->DebugDestroyAllJars();
				Comp->DebugSetActivatedCooldown(0.f);

				if (State->Fixture == 0)
				{
					State->Fixture = 1;
					State->Phase = 1;
					State->PhaseStartReal = NowReal;
					return true;
				}

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Fixture = 0;
					State->Phase = 1;
					State->PhaseStartReal = NowReal;
					SetIntCVar(TEXT("Trace.Oyster.LegacyRefundTeamTest"), 0);
					UE_LOG(LogTraceGame, Display,
						TEXT("[OYSTEREPRESS] --- switching to the SHIPPED refund test (arm 1)"));
					return true;
				}

				State->Phase = 5;
				return true;
			}

			// ---- Phase 5: the verdict, in the words of the report --------------------------------
			RestoreRefundArm();

			const FEPressFixture& RedA = State->Results[0][0];
			const FEPressFixture& RedB = State->Results[0][1];
			const FEPressFixture& GreenA = State->Results[1][0];
			const FEPressFixture& GreenB = State->Results[1][1];
			const bool bTeamlessVictim = (State->VictimTeam == ETraceTeam::None);

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTEREPRESS] --- THE FIXTURES PROVING THEMSELVES FIRST"));
			State->Check(RedA.CooldownAfterPress > 1.f && RedB.CooldownAfterPress > 1.f
				&& GreenA.CooldownAfterPress > 1.f && GreenB.CooldownAfterPress > 1.f,
				FString::Printf(TEXT("all four fixtures really did put E on cooldown BY PRESSING THE KEY "
					"(%.1f, %.1f, %.1f, %.1f) — a zero here means the press never reached the ability"),
					RedA.CooldownAfterPress, RedB.CooldownAfterPress,
					GreenA.CooldownAfterPress, GreenB.CooldownAfterPress));
			State->Check(RedA.bPoisonLanded && RedB.bPoisonLanded && GreenA.bPoisonLanded && GreenB.bPoisonLanded,
				TEXT("all four fixtures really did poison the victim — an E at zero because nothing was "
				     "poisoned is not a refund"));

			if (bTeamlessVictim)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTEREPRESS] --- THE REPRODUCTION: the v26 refund test, on the victim a player uses"));
				State->Check(RedA.bPoisonLanded && RedA.CooldownAfterSettle > 1.f,
					FString::Printf(TEXT("§4 RED, fixture A: the E jar poisoned a teamless victim and E kept "
						"counting down (%.1fs -> %.1fs). THE REPORTED BUG."),
						RedA.CooldownAfterPress, RedA.CooldownAfterSettle));
				State->Check(RedB.bPoisonLanded && RedB.CooldownAfterSettle > 1.f,
					FString::Printf(TEXT("§4 RED, fixture B: a dash jar did the same (%.1fs -> %.1fs) — so it "
						"is the RULE refusing him, not one poison route missing the refund"),
						RedB.CooldownAfterPress, RedB.CooldownAfterSettle));
			}
			else
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[OYSTEREPRESS] --- NO TEAMLESS VICTIM IN THIS WORLD. The red arm cannot reproduce the "
					     "report here: against an ordinary enemy the v26 test passes, which is exactly why the "
					     "bug survived Demo 23. Run this in the practice range for the red arm."));
			}

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTEREPRESS] --- THE CHANGE"));
			State->Check(GreenA.bPoisonLanded && GreenA.CooldownAfterSettle <= 0.f,
				FString::Printf(TEXT("§4 fixture A (the E jar's own poison): E went %.1fs -> %.1fs"),
					GreenA.CooldownAfterPress, GreenA.CooldownAfterSettle));
			State->Check(GreenB.bPoisonLanded && GreenB.CooldownAfterSettle <= 0.f,
				FString::Printf(TEXT("§4 fixture B (a dash jar's poison): E went %.1fs -> %.1fs"),
					GreenB.CooldownAfterPress, GreenB.CooldownAfterSettle));

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTEREPRESS] ===== %d passed, %d failed. ====="),
				State->Passed, State->Failed);
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTEREPRESS] VERDICT: %s"),
				(State->Failed == 0 && State->Passed > 0)
					? TEXT("PASS — E resets on a poison, after a real key press, on the victim the player uses")
					: TEXT("*** FAIL ***"));
			return false;
		}));
	}

	FAutoConsoleCommand CmdEPressRepro(
		TEXT("Trace.Oyster.EPressRepro"),
		TEXT("Dev only, server only. SPEC v28 §4: presses E through the REAL input pipeline, then poisons a "
		     "victim twice (the E jar's own burst, and a dash jar), on the v26 refund test and then on the "
		     "shipped one. The player's side of 'Oyster's E is not resetting when he poisons someone'."),
		FConsoleCommandDelegate::CreateStatic(&RunEPressRepro));

	FAutoConsoleCommand CmdETest(
		TEXT("Trace.Oyster.ETest"),
		TEXT("Dev only, server only. SPEC v26 §6, both halves, red arm first: does the Pickler jar detonate when "
		     "its pull finishes, and does poisoning an enemy reset E? Runs the pre-v26 behaviour first via "
		     "Trace.Oyster.LegacyE so the green has something to be green against."),
		FConsoleCommandDelegate::CreateStatic(&RunETest));
}

#endif   // !UE_BUILD_SHIPPING
