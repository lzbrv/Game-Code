// Trace — SLIMEBALL. See the header for the spec v18 §2 reading, for the choke-point note, and for
// the one thing in here that is written but not yet wired (the fire rate).

#include "Abilities/Characters/TraceAbilitySetSlimeball.h"

#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "Abilities/Characters/TraceSlimewall.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceHitZones.h"
#include "Gameplay/TraceMelee.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Net/TraceLagCompensationComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE RED ARMS. One per ability, each removing that ability and nothing else, so the verification
// commands below can be made to fail on an otherwise identical build. See the note in
// TraceAbilitySetChut.cpp; the wall's own two arms live in TraceSlimewall.cpp beside the code they
// disarm.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarSlimeballWallStick(
	TEXT("Trace.Slimeball.WallStick"),
	1,
	TEXT("Dev/red arm. 1 (default) = holding V sticks Slimeball to a wall (spec v18 §2). 0 = the press "
	     "is accepted and does nothing, so he falls past the wall and every stick assertion must go red."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarSlimeballStickJumpCancel(
	TEXT("Trace.Slimeball.StickJumpCancel"),
	1,
	TEXT("Dev/red arm for DEMO 17 item 2. 1 (default) = jump cancels the wall stick and wall-jumps him off "
	     "the face. 0 = the pre-Demo-17 behaviour: the press is ignored while stuck, so the stick is a "
	     "commitment with no exit and Trace.Slimeball.WallJumpTest must go red. Never ship 0."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarSlimeballStuckPassive(
	TEXT("Trace.Slimeball.StuckPassive"),
	1,
	TEXT("Dev/red arm. 1 (default) = while stuck he fires 30% faster and takes 30% less from body shots "
	     "and FRONT knife stabs. 0 = he still sticks and it still replicates, but neither number moves, "
	     "so both passive assertions must go red."),
	ECVF_Cheat);

/**
 * The ACTIVATED ability's arm, and it disarms the whole press rather than the wall.
 *
 * Refusing inside ActivateAbility() (rather than, say, letting the placement sweep fail) is what makes
 * the arm honest: UTraceAbilityComponent::TryActivate charges the cooldown ONLY on a true return, so
 * armed off, the press produces no wall AND costs nothing — which is the same shape as the genuine
 * "nowhere to put it" fizzle the green run also has to distinguish itself from.
 */
static TAutoConsoleVariable<int32> CVarSlimeballSlimewall(
	TEXT("Trace.Slimeball.Slimewall"),
	1,
	TEXT("Dev/red arm. 1 (default) = pressing E throws up a Slimewall in his aim direction (spec v18 §2). "
	     "0 = the press is refused as a free fizzle, so no wall appears and no cooldown is charged, and "
	     "every activation assertion must go red."),
	ECVF_Cheat);

namespace TraceSlimeball
{
	// =============================================================================================
	// Damage causes. The same three names Chut's Chud reads, resolved the same way — by CAUSE, which
	// is the melee slice's own answer about front-vs-back rather than a number re-derived here.
	// =============================================================================================

	/** "Bullet" — UTraceWeaponComponent's cause for a BODY shot. "Headshot" is its own cause. */
	FName BodyShotCause()
	{
		static const FName Cause(TEXT("Bullet"));
		return Cause;
	}

	/** "Headshot" — the weapon's head-zone cause. §2 [ASSUMPTION]: headshots are NOT reduced. */
	FName HeadshotCause()
	{
		static const FName Cause(TEXT("Headshot"));
		return Cause;
	}

	/** "Trail" — UTraceTrailComponent's death cause. Untouched, exactly as Chud leaves it. */
	FName TrailDeathCause()
	{
		static const FName Cause(TEXT("Trail"));
		return Cause;
	}

	float GetFireIntervalScaleFor(const AActor* Shooter)
	{
		if (const UTraceCharacterAbilitySet* Set = UTraceAbilityComponent::GetAbilitySetFor(Shooter))
		{
			if (const UTraceAbilitySetSlimeball* Slime = Cast<const UTraceAbilitySetSlimeball>(Set))
			{
				return Slime->GetFireIntervalMultiplier();
			}
		}
		return 1.f;
	}
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetSlimeball::OnUnequipped()
{
	StopStick(TEXT("unequipped"));
	ClearWall();
}

void UTraceAbilitySetSlimeball::OnPawnSpawned()
{
	// A new pawn cannot be stuck to anything. Cooldowns are NOT touched — spec §5 is explicit that a
	// player may spawn with an ability timer still counting down.
	StopStick(TEXT("new pawn"));

	// A fresh pawn is standing on a spawn pad, so the anti-ladder count starts at zero like anybody
	// else's. This is a POSITION fact, not a cooldown, which is why clearing it here does not conflict
	// with §5's "cooldowns keep running through a death".
	StickJumpsSinceGround = 0;

	// bSecondaryHeld deliberately survives: a player who was holding V through their own death and
	// respawn is still holding it, and the retry in TickAbilities will grab the first wall they jump
	// into. Clearing it here would silently require them to let go and press again.
}

void UTraceAbilitySetSlimeball::OnPawnDied()
{
	StopStick(TEXT("died"));

	// THE WALL IS DELIBERATELY LEFT STANDING. §2 says "lasts 4 s" and says nothing about its owner;
	// a slime wall is a placed world effect like Rocco's Ripple, not an anchor he interacts with like
	// Mace's spike (which IS cleared on death, because she pulls on it). Dying to the enemy you just
	// walled off should not also hand them the sight line back.
}

void UTraceAbilitySetSlimeball::OnHalfTime()
{
	// The framework has already zeroed the cooldown and Reset() the net state. What is left is the
	// local stick bookkeeping and the actor, which the framework knows nothing about.
	StopStick(TEXT("half time"));
	StickReadyMatchTime = 0.f;
	ClearWall();
}

bool UTraceAbilitySetSlimeball::ShouldDriveMovement() const
{
	// A simulated proxy's velocity is replicated; writing it there would fight the interpolation and
	// would show up as somebody else's Slimeball stuttering on a wall.
	return HasAuthority() || IsLocallyControlled();
}

void UTraceAbilitySetSlimeball::PublishState()
{
	if (!HasAuthority())
	{
		return;
	}

	FTraceAbilityNetState& Writable = MutableState();

	uint8 NewFlags = Writable.Flags;
	NewFlags = bStuck
		? static_cast<uint8>(NewFlags | TraceSlimeballFlags::Stuck)
		: static_cast<uint8>(NewFlags & static_cast<uint8>(~TraceSlimeballFlags::Stuck));
	NewFlags = ActiveWall.IsValid()
		? static_cast<uint8>(NewFlags | TraceSlimeballFlags::WallUp)
		: static_cast<uint8>(NewFlags & static_cast<uint8>(~TraceSlimeballFlags::WallUp));

	Writable.Flags = NewFlags;
	Writable.EffectEndMatchTime = StickEndMatchTime;
	MarkStateDirty();
}

void UTraceAbilitySetSlimeball::TickAbilities(float DeltaSeconds)
{
	if (HasAuthority())
	{
		// The wall destroys itself after 4 s and never tells anybody. Edge-triggered so the replicated
		// flag and the world agree; without this the HUD would keep claiming a wall that is gone.
		const bool bFlagSaysWallUp = (State().Flags & TraceSlimeballFlags::WallUp) != 0;
		if (bFlagSaysWallUp != ActiveWall.IsValid())
		{
			PublishState();
		}
	}

	if (!ShouldDriveMovement())
	{
		return;
	}

	// THE GROUND CLEARS THE ANTI-LADDER COUNT, exactly as it does for the movement component's own
	// WallJumpsSinceGround. Checked before the stick branch so that landing while still holding V —
	// which is the ordinary way a chain of these ends — hands the count back.
	if (const UTraceCharacterMovementComponent* GroundCheck = GetMovement())
	{
		if (StickJumpsSinceGround != 0 && GroundCheck->IsMovingOnGround())
		{
			StickJumpsSinceGround = 0;
		}
	}

	if (bStuck)
	{
		ApplyStick(DeltaSeconds);
		return;
	}

	// THE RETRY, and it is the difference between an ability that works and one that is "inconsistent".
	//
	// The natural input for a wall stick is "hold V, then jump into the wall" — the press arrives
	// BEFORE there is anything to grab. Without this, V would have to be pressed inside a ~90 uu
	// window while airborne, and every miss would read to the player as the ability failing at random.
	// This is the same class of fix as Mace's bPullQueued, and it is cheap: it only runs while the key
	// is actually down, at the component's 20 Hz.
	if (bSecondaryHeld)
	{
		const UTraceCharacterMovementComponent* MoveComp = GetMovement();
		if (MoveComp != nullptr && MoveComp->IsFalling())
		{
			FVector ProbePoint = FVector::ZeroVector;
			FVector ProbeNormal = FVector::ZeroVector;
			float ProbeDistance = 0.f;
			FString ProbeWhy;
			if (MatchTimeNow() >= StickReadyMatchTime
				&& CVarSlimeballWallStick.GetValueOnAnyThread() != 0
				&& ProbeForWall(ProbePoint, ProbeNormal, ProbeDistance, ProbeWhy))
			{
				StartStick(ProbeNormal);
			}
		}
	}
}

// =================================================================================================
// MOVEMENT — hold V to stick to a wall
// =================================================================================================

bool UTraceAbilitySetSlimeball::OnSecondaryPressed()
{
	bSecondaryHeld = true;

	if (CVarSlimeballWallStick.GetValueOnAnyThread() == 0)
	{
		return false;   // RED ARM: the press is accepted and does nothing.
	}

	const ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		return false;
	}

	if (bStuck)
	{
		return false;
	}

	// [ASSUMPTION], and the one worth arguing about: HE MUST BE OFF THE FLOOR.
	//
	// §2 says only "sticks to walls while held". Standing on the ground beside a wall and holding V
	// is not sticking to anything — but it WOULD hand him both stuck passives (+30% fire rate, −30%
	// damage taken) for free, permanently, from any corner on the map. Requiring air makes the
	// passives a trade: he is pinned, silhouetted, and gravity is waiting. It is the same reading, and
	// the same sentence, as Mace's "hold V IN THE AIR to suspend".
	//
	// If a playtest wants ground sticking, this is the one condition to delete — and then
	// SlimeballWallStickMaxSeconds (shipped at 0 = unlimited) is what stops him living up there.
	if (!MoveComp->IsFalling())
	{
		return false;
	}

	if (MatchTimeNow() < StickReadyMatchTime)
	{
		return false;
	}

	FVector ProbePoint = FVector::ZeroVector;
	FVector ProbeNormal = FVector::ZeroVector;
	float ProbeDistance = 0.f;
	FString ProbeWhy;
	if (!ProbeForWall(ProbePoint, ProbeNormal, ProbeDistance, ProbeWhy))
	{
		// Not a failure and not a cost — TickAbilities keeps trying while V is held. See the retry.
		UE_LOG(LogTraceGame, Verbose, TEXT("[Slimeball] V pressed with no wall in reach (%s); will keep trying."),
			*ProbeWhy);
		return false;
	}

	StartStick(ProbeNormal);
	return true;
}

void UTraceAbilitySetSlimeball::OnSecondaryReleased()
{
	bSecondaryHeld = false;

	// "while held". Letting go drops him on the same tick — the flag is cleared here and ApplyStick
	// simply stops writing Velocity, so gravity has him back before the next physics step.
	if (bStuck)
	{
		StopStick(TEXT("V released"));
	}
}

bool UTraceAbilitySetSlimeball::OnJumpPressed()
{
	// DEMO 17 item 2: "Slimeball should be able to cancel the wall stick with jump, and perform a wall
	// jump off the wall." Off the wall this hook has no opinion and the ordinary jump runs.
	if (!bStuck)
	{
		return false;
	}

	if (CVarSlimeballStickJumpCancel.GetValueOnAnyThread() == 0)
	{
		// RED ARM: the press is declined exactly as it was before Demo 17, so the stick is a commitment
		// again and the jump key does nothing while he hangs there. Nothing else about the ability
		// changes, which is what makes Trace.Slimeball.WallJumpTest's red half unambiguous.
		return false;
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || MoveComp == nullptr)
	{
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// THE ANTI-LADDER CAP, against the SHIPPED number rather than a new one. See StickJumpsSinceGround:
	// this launch bypasses the movement component's own WallJumpsSinceGround, so without this a stuck
	// Slimeball could stick, jump, stick, jump up a single wall forever — the exact thing spec v8 §7's
	// cap exists to refuse for everybody else.
	const int32 MaxConsecutive = FMath::Max(1, Settings.WallJumpMaxConsecutive);
	const bool bCapReached = (StickJumpsSinceGround >= MaxConsecutive);

	const FVector Normal = StickWallNormal.GetSafeNormal();

	// LET GO FIRST, ALWAYS, cap or no cap. "Cancel the wall stick with jump" is the half of the
	// sentence that must never depend on anything: a press that refused to let go because he was one
	// wall jump over the cap would leave him glued to a wall with the jump key doing nothing, which is
	// worse than the commitment Demo 17 asked us to remove.
	//
	// StopStick charges the ordinary stick cooldown; the LONGER re-grab lockout below is what stops the
	// 20 Hz re-stick sweep gluing him back onto the same face while V is still held.
	StopStick(TEXT("jumped off the wall"));

	const float Restick = FMath::Max(0.f, Settings.SlimeballWallJumpRestickLockoutSeconds);
	StickReadyMatchTime = FMath::Max(StickReadyMatchTime, MatchTimeNow() + Restick);

	if (!Settings.bSlimeballWallStickJumpLaunches || bCapReached || Normal.IsNearlyZero()
		|| CVarSlimeballWallStick.GetValueOnAnyThread() == 0)
	{
		// He is off the wall and falling. Consumed anyway: the alternative is handing the press to
		// ACharacter::Jump, which with no wall-jump window open would either be refused (nothing
		// happens, and he has lost the stick for free) or — if a window happened to be open — fire a
		// SECOND launch on top of the one he was just refused. Neither is a jump the player asked for.
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Slimeball] jump cancelled the stick without a launch (launchEnabled=%d capReached=%d "
			     "consecutive=%d/%d)."),
			Settings.bSlimeballWallStickJumpLaunches ? 1 : 0, bCapReached ? 1 : 0,
			StickJumpsSinceGround, MaxConsecutive);
		return true;
	}

	// --- THE LAUNCH, in the shipped wall jump's own numbers --------------------------------------
	//
	// A stuck pawn's planar velocity is zero (ApplyStick writes it there every tick), which is the case
	// UTraceCharacterMovementComponent::TryWallJump describes as "pressed flat against the wall with no
	// planar speed at all. The outward impulse is the whole launch in that case, which is what makes a
	// standing wall jump an escape rather than a wasted press." The retention term is kept anyway and
	// applied to whatever planar speed he does have, so a tuned-up SlimeballWallStickSlideSpeed or a
	// future non-zero stick velocity cannot silently change the shape of this.
	FVector Planar(MoveComp->Velocity.X, MoveComp->Velocity.Y, 0.f);
	const float EntrySpeed = static_cast<float>(Planar.Size());
	const float IntoWall = static_cast<float>(FVector::DotProduct(Planar, Normal));
	if (IntoWall < 0.f)
	{
		Planar -= 2.f * IntoWall * Normal;      // reflect only what was heading into the face
	}

	FVector LaunchDirection = Planar;
	if (!LaunchDirection.Normalize())
	{
		LaunchDirection = Normal;
	}

	const float Retention = FMath::Clamp(Settings.WallJumpSpeedRetention, 0.f, 1.f);
	const float Outward = FMath::Max(0.f, Settings.WallJumpOutwardImpulse);
	const FVector Launch = LaunchDirection * (EntrySpeed * Retention) + Normal * Outward;

	MoveComp->Velocity.X = Launch.X;
	MoveComp->Velocity.Y = Launch.Y;
	MoveComp->Velocity.Z = MoveComp->JumpZVelocity * FMath::Max(0.f, Settings.WallJumpVerticalMultiplier);
	MoveComp->SetMovementMode(MOVE_Falling);

	++StickJumpsSinceGround;

	UE_LOG(LogTraceGame, Log,
		TEXT("[Slimeball] WALL JUMP off the stick: normal (%s), out %.0f uu/s, up %.0f uu/s (%d of %d before "
		     "the ground resets it). He may not re-grab for %.2fs."),
		*Normal.ToCompactString(), Outward, MoveComp->Velocity.Z,
		StickJumpsSinceGround, MaxConsecutive, Restick);

	return true;
}

void UTraceAbilitySetSlimeball::StartStick(const FVector& WallNormal)
{
	bStuck = true;
	StickWallNormal = WallNormal.GetSafeNormal();

	// 0 means "as long as V is held", which is what §2 describes and what ships. A positive knob is
	// an absolute match-clock deadline like every other timer in this framework.
	const float MaxSeconds = FMath::Max(0.f, UTraceSettings::Get().SlimeballWallStickMaxSeconds);
	StickEndMatchTime = (MaxSeconds > 0.f) ? (MatchTimeNow() + MaxSeconds) : 0.f;

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Slimeball] STUCK to a wall (normal %s). Fire interval x%.3f, %.0f%% off body shots and front "
			     "stabs while he holds on."),
			*StickWallNormal.ToCompactString(), GetFireIntervalMultiplier(),
			FMath::Clamp(UTraceSettings::Get().SlimeballStuckDamageReduction, 0.f, 1.f) * 100.f);
	}
}

void UTraceAbilitySetSlimeball::StopStick(const TCHAR* Why)
{
	if (!bStuck)
	{
		return;
	}

	bStuck = false;
	StickEndMatchTime = 0.f;
	StickWallNormal = FVector::ZeroVector;
	StickReadyMatchTime = MatchTimeNow() + FMath::Max(0.f, UTraceSettings::Get().SlimeballWallStickCooldownSeconds);

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Verbose, TEXT("[Slimeball] let go of the wall (%s)."), Why);
	}
}

void UTraceAbilitySetSlimeball::ApplyStick(float DeltaSeconds)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		StopStick(TEXT("no pawn"));
		return;
	}
	if (!bSecondaryHeld)
	{
		StopStick(TEXT("no longer held"));
		return;
	}
	if (CVarSlimeballWallStick.GetValueOnAnyThread() == 0)
	{
		StopStick(TEXT("red arm"));
		return;
	}
	if (StickEndMatchTime > 0.f && MatchTimeNow() >= StickEndMatchTime)
	{
		StopStick(TEXT("max duration elapsed"));
		return;
	}
	if (MyPawn->IsDashing())
	{
		// A dash is velocity on rails and would fight this write every frame. Dashing off a wall is a
		// deliberate exit, not a bug.
		StopStick(TEXT("dashed off"));
		return;
	}

	// RE-PROBE ALONG THE STORED NORMAL, not a fresh fan. One trace, and it asks the only question that
	// matters while stuck: is the surface he grabbed still there? A pawn can be carried off the end of
	// a wall by a slide knob, a moving platform, or the wall simply ending — and hanging in mid-air
	// beside geometry he is no longer touching is exactly the "new movement vectors with no input"
	// complaint this project is already chasing.
	FVector ProbePoint = FVector::ZeroVector;
	FVector ProbeNormal = FVector::ZeroVector;
	float ProbeDistance = 0.f;
	FString ProbeWhy;
	if (!ProbeForWall(ProbePoint, ProbeNormal, ProbeDistance, ProbeWhy))
	{
		StopStick(TEXT("the wall is gone"));
		return;
	}
	StickWallNormal = ProbeNormal;

	// "STICKS to walls." Fully welded: planar motion is zeroed too, so he cannot creep sideways along
	// the surface. SlimeballWallStickSlideSpeed ships at 0 — a positive value is the classic slow
	// slide down, if being motionless proves too safe in a playtest.
	//
	// Velocity is re-written every tick rather than once, because PhysFalling integrates gravity on
	// every sub-step in between. That is the same 20 Hz granularity limit Mace's suspend documents and
	// measures; it is a real, named limitation of writing movement from the ability layer.
	const float Slide = FMath::Max(0.f, UTraceSettings::Get().SlimeballWallStickSlideSpeed);
	MoveComp->Velocity = FVector(0.f, 0.f, -Slide);
}

bool UTraceAbilitySetSlimeball::IsStuck() const
{
	if (ShouldDriveMovement())
	{
		return bStuck;
	}
	return (State().Flags & TraceSlimeballFlags::Stuck) != 0;
}

float UTraceAbilitySetSlimeball::GetStickSecondsRemaining() const
{
	if (!IsStuck() || StickEndMatchTime <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, StickEndMatchTime - MatchTimeNow());
}

bool UTraceAbilitySetSlimeball::ProbeForWall(FVector& OutPoint, FVector& OutNormal, float& OutDistance,
                                             FString& OutWhy) const
{
	OutPoint = FVector::ZeroVector;
	OutNormal = FVector::ZeroVector;
	OutDistance = 0.f;

	const ATraceCharacter* MyPawn = GetCharacter();
	UWorld* WorldPtr = GetWorld();
	if (MyPawn == nullptr || WorldPtr == nullptr)
	{
		OutWhy = TEXT("no pawn");
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Reach = FMath::Clamp(Settings.SlimeballWallStickRangeUU, 10.f, 500.f);
	const float MaxNormalZ = FMath::Clamp(Settings.SlimeballWallStickMaxSurfaceNormalZ, 0.f, 1.f);
	const FVector Centre = MyPawn->GetActorLocation();

	// BY OBJECT TYPE, not by channel — the same lesson Mace's spike throw records. A channel trace on
	// ECC_WorldStatic is also blocked by every pawn capsule (the stock "Pawn" profile BLOCKS the
	// WorldStatic channel), so a team-mate standing between him and the wall would have eaten the
	// probe and reported "no wall". Asking for WorldStatic OBJECTS asks the question §2 asks.
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceSlimeballWallProbe), /*bTraceComplex*/ false);
	QueryParams.AddIgnoredActor(MyPawn);

	// A FAN, not one ray. He does not have to be LOOKING at the wall to be against it — a player who
	// jumps a gap and reaches for the far side is usually facing along it, not into it. Eight probes
	// is a 45-degree resolution, which at 90 uu of reach cannot miss a flat surface his capsule is
	// touching, and it costs eight line traces on a key press (and one per tick thereafter, along the
	// stored normal — see ApplyStick).
	//
	// If he IS already stuck, the stored normal is tried FIRST and wins ties, so a corner cannot make
	// him flip between two faces every tick and jitter.
	constexpr int32 ProbeCount = 8;

	float BestDistance = TNumericLimits<float>::Max();
	bool bFound = false;
	int32 Rejected = 0;

	auto TryDirection = [&](const FVector& Direction, float Bias)
	{
		if (Direction.IsNearlyZero())
		{
			return;
		}

		FHitResult Hit;
		if (!WorldPtr->LineTraceSingleByObjectType(Hit, Centre, Centre + Direction * Reach, ObjectParams, QueryParams))
		{
			return;
		}

		// "A wall is a surface he cannot stand on." SlimeballWallStickMaxSurfaceNormalZ defaults to
		// the walkable floor limit, so the rule reads exactly that way. It is a SEPARATE knob from the
		// wall jump's 0.40 on purpose (see the note on the knob): the wall jump's threshold has been
		// tuned against, and moving it to fix a different ability would retune the wall jump by
		// accident.
		if (FMath::Abs(Hit.ImpactNormal.Z) > MaxNormalZ)
		{
			++Rejected;
			return;
		}

		const float Score = static_cast<float>(Hit.Distance) - Bias;
		if (Score < BestDistance)
		{
			BestDistance = Score;
			OutPoint = Hit.ImpactPoint;
			OutNormal = Hit.ImpactNormal;
			OutDistance = static_cast<float>(Hit.Distance);
			bFound = true;
		}
	};

	if (bStuck && !StickWallNormal.IsNearlyZero())
	{
		// Into the surface he is already on. The bias is a whole reach, so this face wins unless it
		// has genuinely gone away.
		TryDirection(-StickWallNormal, Reach);
	}

	for (int32 Index = 0; Index < ProbeCount; ++Index)
	{
		const float Angle = (2.f * PI * static_cast<float>(Index)) / static_cast<float>(ProbeCount);
		TryDirection(FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.f), 0.f);
	}

	if (!bFound)
	{
		OutWhy = (Rejected > 0)
			? FString::Printf(TEXT("nothing stickable within %.0f uu (%d surface(s) were too flat to be a wall)"),
				Reach, Rejected)
			: FString::Printf(TEXT("nothing within %.0f uu"), Reach);
		return false;
	}

	OutWhy = TEXT("wall");
	return true;
}

// =================================================================================================
// PASSIVE — while stuck
// =================================================================================================

float UTraceAbilitySetSlimeball::ModifyIncomingDamage(float Damage, const FTraceAbilityDamageContext& Context) const
{
	if (!IsStuck() || CVarSlimeballStuckPassive.GetValueOnAnyThread() == 0)
	{
		return Damage;
	}

	// §2 [ASSUMPTION], stated in the spec itself: headshots and BACK stabs are unreduced, because the
	// doc names "body shots and front knife stabs". Both exclusions are by CAUSE as well as by flag,
	// because the flag is filled in by whichever slice is calling and the cause is not.
	if (Context.bHeadshot || Context.Cause == TraceSlimeball::HeadshotCause()
		|| Context.Cause == TraceSlimeball::TrailDeathCause())
	{
		return Damage;
	}

	// THE CAUSE IS THE ZONE. TraceMelee resolves front from back at the one place it can be known
	// exactly and hands the health component two DIFFERENT causes for the two zones ("Knife" and
	// "Backstab"). Reading the cause is reading the melee slice's own answer rather than re-deriving
	// it from a damage number a future retune would break — the identical argument Chut's file makes.
	if (Context.Cause == TraceMelee::GetBackstabKillCause())
	{
		return Damage;
	}

	const bool bBodyShot  = (Context.Cause == TraceSlimeball::BodyShotCause());
	const bool bFrontStab = (Context.Cause == TraceMelee::GetKnifeKillCause());

	// NARROWER THAN CHUD, ON PURPOSE. Chut's is "body shots and MELEES" and so it reads Context.bMelee;
	// Slimeball's is "body shots and front knife STABS", so a generic melee with no knife cause is left
	// alone. If a future melee arrives that should also be reduced, it belongs on this line by name.
	if (!bBodyShot && !bFrontStab)
	{
		return Damage;
	}

	const float Reduction = FMath::Clamp(UTraceSettings::Get().SlimeballStuckDamageReduction, 0.f, 1.f);
	return Damage * (1.f - Reduction);
}

float UTraceAbilitySetSlimeball::GetFireIntervalMultiplier() const
{
	if (!IsStuck() || CVarSlimeballStuckPassive.GetValueOnAnyThread() == 0)
	{
		return 1.f;
	}

	// *** DIVIDES. See the header. FireInterval is a PERIOD and SlimeballStuckFireRateBonus is a RATE. ***
	const float Bonus = FMath::Max(0.f, UTraceSettings::Get().SlimeballStuckFireRateBonus);
	return 1.f / (1.f + Bonus);
}

void UTraceAbilitySetSlimeball::DebugSetStuck(bool bForceStuck)
{
	if (bForceStuck == bStuck)
	{
		return;
	}

	bStuck = bForceStuck;
	if (!bStuck)
	{
		StickEndMatchTime = 0.f;
		StickWallNormal = FVector::ZeroVector;
	}
	PublishState();
}

// =================================================================================================
// ACTIVATED — Slimewall
// =================================================================================================

float UTraceAbilitySetSlimeball::GetActivatedCooldownSeconds() const
{
	// Read at the point of use, never cached — the whole settings page is live-editable during PIE,
	// and the HUD's cooldown ring asks this every frame it draws. Without this override the base class
	// answers AbilityDefaultCooldownSeconds (20 s) while the select card prints 25 s from the roster
	// table, and Trace.VerifyCharacterData section D goes red on the difference.
	return UTraceSettings::Get().SlimewallCooldownSeconds;
}

bool UTraceAbilitySetSlimeball::CanActivate(FText& OutReason) const
{
	// Nothing character-specific refuses it. He may throw a wall while stuck to one — that is a
	// perfectly good play (wall off the approach, keep hanging above it) and §2 forbids nothing.
	return true;
}

ATraceSlimewall* UTraceAbilitySetSlimeball::GetActiveWall() const
{
	return ActiveWall.Get();
}

void UTraceAbilitySetSlimeball::ClearWall()
{
	if (ATraceSlimewall* Wall = ActiveWall.Get())
	{
		Wall->Destroy();
	}
	ActiveWall = nullptr;

	if (HasAuthority())
	{
		PublishState();
	}
}

bool UTraceAbilitySetSlimeball::ResolveSlimewallPlacement(FVector& OutCenter, FVector& OutPlanarAim,
                                                          FVector& OutHalfExtents, FString& OutWhy) const
{
	OutCenter = FVector::ZeroVector;
	OutPlanarAim = FVector::ZeroVector;
	OutHalfExtents = FVector::ZeroVector;

	const ATraceCharacter* MyPawn = GetCharacter();
	UWorld* WorldPtr = GetWorld();
	if (MyPawn == nullptr || WorldPtr == nullptr)
	{
		OutWhy = TEXT("no pawn");
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// THREE KNOBS, because §2 says "(for now, make this changeable)" of all three dimensions. The axis
	// each one means is the [ASSUMPTION] recorded on the knobs themselves: WIDTH is the thickness
	// along his aim (what an enemy walks through, and therefore what makes the 35% slow last longer
	// than a rounding error), LENGTH is the span across it, HEIGHT is vertical.
	const float Width  = FMath::Max(10.f, Settings.SlimewallWidthUU);
	const float Length = FMath::Max(50.f, Settings.SlimewallLengthUU);
	const float Height = FMath::Max(20.f, Settings.SlimewallHeightUU);
	OutHalfExtents = FVector(Width * 0.5f, Length * 0.5f, Height * 0.5f);

	// "in his AIM DIRECTION", flattened. A wall is a vertical slab standing on the floor, so the
	// pitch of his view decides nothing about it — only the compass bearing does. Looking straight
	// down would otherwise produce a degenerate direction and a wall in an arbitrary orientation.
	FVector PlanarAim = FVector(MyPawn->GetAimDirection().X, MyPawn->GetAimDirection().Y, 0.f).GetSafeNormal();
	if (PlanarAim.IsNearlyZero())
	{
		const FVector Forward = MyPawn->GetActorForwardVector();
		PlanarAim = FVector(Forward.X, Forward.Y, 0.f).GetSafeNormal();
	}
	if (PlanarAim.IsNearlyZero())
	{
		OutWhy = TEXT("no horizontal aim direction");
		return false;
	}
	OutPlanarAim = PlanarAim;

	float CapsuleRadius = 34.f;
	float CapsuleHalfHeight = 88.f;
	if (const UCapsuleComponent* Capsule = MyPawn->GetCapsuleComponent())
	{
		CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceSlimewallPlacement), /*bTraceComplex*/ false);
	QueryParams.AddIgnoredActor(MyPawn);

	// ---- how far in front of him it goes up ------------------------------------------------------
	//
	// Shortened by anything solid in the way, so throwing a wall at a pillar two metres away puts it
	// against the pillar rather than half inside it. The clearance kept is HALF THE THICKNESS, because
	// the centre is what is being placed.
	const float DesiredRange = FMath::Max(0.f, Settings.SlimewallRangeUU);
	const float MinimumRange = CapsuleRadius + (Width * 0.5f) + 10.f;

	float PlacementRange = DesiredRange;
	FHitResult ForwardHit;
	if (WorldPtr->LineTraceSingleByObjectType(ForwardHit, MyPawn->GetActorLocation(),
		MyPawn->GetActorLocation() + PlanarAim * DesiredRange, ObjectParams, QueryParams))
	{
		PlacementRange = static_cast<float>(ForwardHit.Distance) - (Width * 0.5f);
	}

	if (PlacementRange < MinimumRange)
	{
		// A FREE FIZZLE. The framework charges the cooldown only on a true return, and
		// UTraceCharacterAbilitySet's header names "Mace's spike hitting nothing" as the precedent.
		// Standing nose-to-nose with a wall must not cost 25 s.
		OutWhy = FString::Printf(
			TEXT("no room — only %.0f uu of clearance ahead and the slab needs %.0f"),
			FMath::Max(0.f, PlacementRange), MinimumRange);
		return false;
	}

	const FVector Feet = MyPawn->GetActorLocation() - FVector(0.f, 0.f, CapsuleHalfHeight);
	const FVector Ahead = Feet + PlanarAim * PlacementRange;

	// ---- and then drop it onto the floor ---------------------------------------------------------
	//
	// Measured from a whole wall-height ABOVE the target point so that placing it up a short step
	// finds the step rather than the floor underneath it, and traced a long way down so that placing
	// it over a ledge still finds ground rather than leaving the slab hanging at his own feet height.
	float FloorZ = Feet.Z;
	FHitResult FloorHit;
	if (WorldPtr->LineTraceSingleByObjectType(FloorHit, Ahead + FVector(0.f, 0.f, Height),
		Ahead - FVector(0.f, 0.f, Height * 4.f), ObjectParams, QueryParams))
	{
		FloorZ = static_cast<float>(FloorHit.ImpactPoint.Z);
	}

	OutCenter = FVector(Ahead.X, Ahead.Y, FloorZ + Height * 0.5f);
	OutWhy = TEXT("placed");
	return true;
}

bool UTraceAbilitySetSlimeball::ActivateAbility()
{
	if (CVarSlimeballSlimewall.GetValueOnAnyThread() == 0)
	{
		// RED ARM. A false return is a free fizzle, so this removes the ability without also removing
		// the cooldown evidence the green run is compared against.
		UE_LOG(LogTraceGame, Verbose, TEXT("[Slimeball] Slimewall armed OFF (Trace.Slimeball.Slimewall 0)."));
		return false;
	}

	FVector Centre = FVector::ZeroVector;
	FVector PlanarAim = FVector::ZeroVector;
	FVector Extents = FVector::ZeroVector;
	FString Why;

	if (!ResolveSlimewallPlacement(Centre, PlanarAim, Extents, Why))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Slimeball] Slimewall fizzled: %s. No cooldown charged."), *Why);
		return false;
	}

	if (!HasAuthority())
	{
		// PREDICTED HALF: deliberately nothing. The wall is a replicated server actor; spawning a
		// local copy would double-draw it for a frame and leave a ghost when the real one arrives.
		// Returning true greys the HUD button immediately, which is the only client-visible half
		// there is — the same call Chut's Chud makes.
		return true;
	}

	// A second wall REPLACES the first rather than orphaning it. Reachable after a half-time cooldown
	// reset while a wall is still standing.
	ClearWall();

	UWorld* WorldPtr = GetWorld();
	APlayerState* MySource = nullptr;
	ETraceTeam MyTeam = ETraceTeam::None;
	if (UTraceAbilityComponent* Comp = GetAbilityComponent())
	{
		MySource = Comp->GetOwningPlayerState();
		MyTeam = Comp->GetTeam();
	}

	const float Duration = FMath::Max(0.25f, UTraceSettings::Get().SlimewallDurationSeconds);

	ActiveWall = ATraceSlimewall::ServerSpawn(WorldPtr, MySource, MyTeam, Centre, PlanarAim, Extents,
		MatchTimeNow() + Duration);

	if (!ActiveWall.IsValid())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Slimeball] Slimewall could not be spawned. No cooldown charged."));
		return false;
	}

	PublishState();

	UE_LOG(LogTraceGame, Log,
		TEXT("[Slimeball] SLIMEWALL up for %.1fs: %.0f thick x %.0f long x %.0f tall at %.0f uu ahead. "
		     "Bullets pass through it; enemies crossing it lose %.0f%% speed. Cooldown %.0fs."),
		Duration, Extents.X * 2.f, Extents.Y * 2.f, Extents.Z * 2.f, UTraceSettings::Get().SlimewallRangeUU,
		FMath::Clamp(UTraceSettings::Get().SlimewallSlowFraction, 0.f, 0.95f) * 100.f,
		UTraceSettings::Get().SlimewallCooldownSeconds);

	return true;
}

// =================================================================================================
// SLIMEBALL'S SELF-TESTS
//
// ===================================================================================================
// WHAT EACH COMMAND FALSIFIES, AND WHY IT IS ARRANGED THIS WAY
// ===================================================================================================
//
//   Trace.Slimeball.Dump        what the RUNNING process resolved. The .ini wins over the header on
//                               this project, so this is the only honest way to read his numbers.
//
//   Trace.Slimeball.Verify      FOUR arms, each red before green, on one binary:
//
//     A. THE PASSIVES.    Body shot 40 -> 28 and a FRONT stab 50 -> 35 while stuck; a HEADSHOT and a
//                         BACKSTAB unchanged; everything unchanged while NOT stuck. Red arm
//                         (Trace.Slimeball.StuckPassive 0) must leave the body shot at 40.
//
//     B. THE SLOW, AND SPEC §4. A wall is put on top of a living enemy and asked to decide, in the
//                         same statement sequence, with no tick in between so a bot cannot walk into
//                         the measurement. His OWN SIDE is then given the same treatment (§2's
//                         "it does not slow his own team"), and the slow is re-measured with the
//                         linger knob at 0 — the value that used to disable it entirely. Then the
//                         SAME victim is handed the Core and the wall is asked again — the "picked it
//                         up mid-slow" case, which a check-once implementation passes every other test
//                         with. Three red arms: Trace.Slimeball.Slow 0 (nobody is slowed at all),
//                         bSlimewallSlowsOwnTeam on (his own side IS slowed) and
//                         Trace.Ability.CarrierImmune 0 (the carrier IS slowed — which MUST
//                         reproduce, or the green run proves nothing).
//
//     D. THE PRESS. Runs BEFORE C because it moves him. Arms B and C both build their walls with
//                         ATraceSlimewall::ServerSpawn, so until arm D existed nothing in this file
//                         ever called ActivateAbility() or the placement sweep — an ability that
//                         placed no wall at all would have gone green everywhere else. D presses
//                         UTraceAbilityComponent::TryActivate(), the entry point E uses, and then
//                         measures what stood up: how far ahead, facing where, at what size, for how
//                         long, and for what cooldown. It also proves the two refusals — nose-to-nose
//                         with geometry is a FREE fizzle, and a second press inside the 25 s does not
//                         replace the standing wall. Red arm: Trace.Slimeball.Slimewall 0.
//
//     C. "CAN BE SHOT THROUGH", which is the sentence §2 is most explicit about and the failure
//                         nobody would ever diagnose. The REAL resolver
//                         (UTraceLagCompensationComponent::ResolveHitscan) is fired at a real enemy
//                         three times in one tick: with no wall (the control — proves the fixture
//                         has a sight line at all), with a wall carrying BlockAll
//                         (Trace.Slimeball.WallBlocksBullets 1 — the shot MUST be lost, which is the
//                         bug being guarded against), and with the shipped wall (the shot must land,
//                         and then actually kill).
//
//   Trace.Slimeball.StickTest   the movement half, against REAL geometry: he is placed beside a wall
//                               the level actually has, dropped, and told to hold V. Red arm
//                               (Trace.Slimeball.WallStick 0) must see him fall past it.
//
// EVERY MEASUREMENT READS ITS INPUT, ACTS, AND READS ITS OUTPUT IN ONE STATEMENT SEQUENCE with no
// tick and no await in between, for the reason the X fixture's header gives: a headless run fills
// both teams with bots, and a bot shooting the subject between two reads is indistinguishable from a
// broken rule.
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace TraceSlimeballVerify
{
	/** Unambiguous, well short of lethal, and 40 x 0.70 is 28 — two numbers nobody can confuse. */
	constexpr float BodyTestDamage = 40.f;
	/** Chut's front-knife number, so 50 x 0.70 = 35 is likewise unmistakable. */
	constexpr float StabTestDamage = 50.f;

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

	void SetCVar(const TCHAR* CVarName, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** Puts every arm back to its shipped value. Called on every exit path, including the aborts. */
	void RestoreAllArms()
	{
		SetCVar(TEXT("Trace.Slimeball.WallStick"), 1);
		SetCVar(TEXT("Trace.Slimeball.StuckPassive"), 1);
		SetCVar(TEXT("Trace.Slimeball.Slimewall"), 1);
		SetCVar(TEXT("Trace.Slimeball.Slow"), 1);
		SetCVar(TEXT("Trace.Slimeball.WallBlocksBullets"), 0);
		SetCVar(TEXT("Trace.Ability.CarrierImmune"), 1);
	}

	struct FChecklist
	{
		const TCHAR* Tag = TEXT("SLIME");
		int32 Passed = 0;
		int32 Failed = 0;
		bool  bInvalid = false;
		FString InvalidReason;

		void Check(bool bCondition, const FString& CheckName, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[%s] %s  %s  |  %s"),
				Tag, bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *CheckName, *Detail);
		}

		void Invalidate(const FString& Reason)
		{
			bInvalid = true;
			InvalidReason = Reason;
		}

		void Report()
		{
			if (bInvalid)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: INVALID — %s (%d passed, %d failed)"),
					Tag, *InvalidReason, Passed, Failed);
			}
			else if (Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[%s] VERDICT: PASS — %d checks, 0 failed."), Tag, Passed);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d passed, %d FAILED."),
					Tag, Passed, Failed);
			}
		}
	};

	/**
	 * Makes the first HUMAN player Slimeball and returns their set.
	 *
	 * A human, and it must be: the harness hands its subject a character of its own choosing, and
	 * doing that to a bot would fight ATraceGameMode::PollCharacterSelect's 4 Hz fill for ownership of
	 * that player state and measure whichever won. Same reasoning, and the same wording, as the X
	 * fixture's MakePlayerIntoX.
	 */
	UTraceAbilitySetSlimeball* MakePlayerIntoSlimeball(UWorld* WorldPtr, FString& OutWhy)
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

			if (Comp->GetCharacterId() != ETraceCharacterId::Slimeball)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::Slimeball);
			}

			if (UTraceAbilitySetSlimeball* Slime = Comp->GetAbilitySetAs<UTraceAbilitySetSlimeball>())
			{
				return Slime;
			}

			OutWhy = FString::Printf(
				TEXT("ServerSetCharacter(Slimeball) did not produce a UTraceAbilitySetSlimeball (id is now %s) — "
				     "a team-mate may already hold him, or the reflection roster did not find the class"),
				TraceCharacterIdToString(Comp->GetCharacterId()));
			return nullptr;
		}

		OutWhy = TEXT("no human player controller with a pawn");
		return nullptr;
	}

	/** A living enemy of @p Slime who is NOT the carrier, healed to full so damage has somewhere to go. */
	ATraceCharacter* FindEnemyTarget(UWorld* WorldPtr, const UTraceAbilitySetSlimeball* Slime)
	{
		if (WorldPtr == nullptr || Slime == nullptr)
		{
			return nullptr;
		}
		const ATraceCharacter* MyPawn = Slime->GetCharacter();
		ETraceTeam MyTeam = ETraceTeam::None;
		if (const UTraceAbilityComponent* Comp = Slime->GetAbilityComponent())
		{
			MyTeam = Comp->GetTeam();
		}

		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == MyPawn || !Candidate->IsAlive() || Candidate->Health == nullptr)
			{
				continue;
			}
			if (UTraceAbilityComponent::IsCarrier(Candidate))
			{
				continue;
			}
			if (Candidate->GetTeam() == MyTeam || Candidate->GetTeam() == ETraceTeam::None)
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	/** One damage number through the FULL passive pipeline, with no tick in between the two reads. */
	float MeasureThroughPassives(ATraceCharacter* Attacker, ATraceCharacter* Victim, float Amount,
	                             FName Cause, bool bMelee, bool bHeadshot)
	{
		FTraceAbilityDamageContext Context;
		Context.Instigator = Attacker;
		Context.Target = Victim;
		Context.Cause = Cause;
		Context.bMelee = bMelee;
		Context.bHeadshot = bHeadshot;
		return UTraceAbilityComponent::ModifyDamageThroughPassives(Amount, Context);
	}

	// =============================================================================================
	// Trace.Slimeball.Dump
	// =============================================================================================

	void RunDump()
	{
		const UTraceSettings& Settings = UTraceSettings::Get();

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE SLIMEBALL (spec v18 §2) =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("MOVEMENT  wall stick : reach %.0f uu, wall = |normal.Z| <= %.2f, slide %.0f uu/s, "
			     "max %.1fs (0 = while held), cooldown %.1fs"),
			Settings.SlimeballWallStickRangeUU, Settings.SlimeballWallStickMaxSurfaceNormalZ,
			Settings.SlimeballWallStickSlideSpeed, Settings.SlimeballWallStickMaxSeconds,
			Settings.SlimeballWallStickCooldownSeconds);
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSIVE   while stuck: fire rate +%.0f%% -> FireInterval %.3fs becomes %.3fs; "
			     "-%.0f%% from body shots and FRONT stabs (headshots and backstabs untouched)"),
			Settings.SlimeballStuckFireRateBonus * 100.f,
			Settings.FireInterval,
			Settings.FireInterval / (1.f + FMath::Max(0.f, Settings.SlimeballStuckFireRateBonus)),
			Settings.SlimeballStuckDamageReduction * 100.f);
		UE_LOG(LogTraceGame, Display,
			TEXT("ACTIVATED slimewall  : %.0f thick (through) x %.0f long (across) x %.0f tall at %.0f uu ahead, "
			     "%.1fs, cooldown %.1fs"),
			Settings.SlimewallWidthUU, Settings.SlimewallLengthUU, Settings.SlimewallHeightUU,
			Settings.SlimewallRangeUU, Settings.SlimewallDurationSeconds, Settings.SlimewallCooldownSeconds);
		UE_LOG(LogTraceGame, Display,
			TEXT("          the slow   : -%.0f%% speed, lingering %.2fs after leaving; slows his own team = %s; "
			     "CONTROL, so a Core carrier is exempt while bCarrierImmuneToAbilityControl is %s"),
			Settings.SlimewallSlowFraction * 100.f, Settings.SlimewallSlowLingerSeconds,
			Settings.bSlimewallSlowsOwnTeam ? TEXT("YES") : TEXT("no"),
			Settings.bCarrierImmuneToAbilityControl ? TEXT("on") : TEXT("OFF"));
		UE_LOG(LogTraceGame, Display,
			TEXT("          the look   : opacity %.2f. IT NEVER BLOCKS BULLETS — the slab has no collision at "
			     "all; 'obstructs vision' is rendering only."),
			Settings.SlimewallOpacity);

		// THE NUMBER THAT USED TO BE TRUE BUT NOT FELT. It is felt now: the v18 §2 integration pass
		// added UTraceAbilityComponent::GetFireIntervalScaleFor() and both weapon call sites multiply
		// UTraceSettings::FireInterval by it. Printed at Display rather than deleted, because "is the
		// passive actually reaching the gun?" is the question this line exists to answer in one glance,
		// and it now answers it with the live number instead of with a warning.
		UE_LOG(LogTraceGame, Display,
			TEXT("          the gun    : WIRED — UTraceWeaponComponent scales FireInterval by "
			     "UTraceAbilityComponent::GetFireIntervalScaleFor(), which is x%.4f for a stuck Slimeball "
			     "(%.3fs becomes %.3fs) and x1.0000 for everybody else."),
			1.f / FMath::Max(0.01f, 1.f + Settings.SlimeballStuckFireRateBonus),
			FMath::Max(0.01f, Settings.FireInterval),
			FMath::Max(0.01f, Settings.FireInterval) / FMath::Max(0.01f, 1.f + Settings.SlimeballStuckFireRateBonus));
	}

	// =============================================================================================
	// Trace.Slimeball.Verify — arms A, B, D and C (D runs before C: it moves him, and it leaves him
	// standing in the open ground C needs)
	// =============================================================================================

	struct FVerifyState
	{
		int32 Step = 0;
		double Deadline = 0.0;
		FChecklist List;

		// arm A
		bool bRedPassiveReproduced = false;

		// arm B
		bool bSlowLanded = false;
		bool bRedSlowSuppressed = false;
		bool bOwnTeamSpared = false;
		bool bRedOwnTeamReproduced = false;
		bool bZeroLingerStillSlows = false;
		bool bCarrierRefused = false;
		bool bCarrierMidSlowStopped = false;
		bool bRedCarrierReproduced = false;

		// arm D
		bool bRedActivationSuppressed = false;
		bool bWallWentUp = false;
		bool bFizzleWasFree = false;

		// arm C
		int32 CPhase = 0;                                 // 0 = place the subject, 1 = measure
		TWeakObjectPtr<ATraceCharacter> CVictim;
		bool bControlShotLanded = false;
		bool bRedWallAteTheShot = false;
	};

	void RunVerifyStepA(UWorld* WorldPtr, UTraceAbilitySetSlimeball* Slime, FVerifyState& State)
	{
		ATraceCharacter* MyPawn = Slime->GetCharacter();
		FChecklist& List = State.List;

		// --- NOT STUCK: the control. Nothing may move. ---------------------------------------------
		Slime->DebugSetStuck(false);
		const float LooseBody = MeasureThroughPassives(nullptr, MyPawn, BodyTestDamage,
			TraceSlimeball::BodyShotCause(), false, false);
		// THE SEAM, NOT THE MEMBER. TraceSlimeball::GetFireIntervalScaleFor is the exact function the
		// weapon will call once it is wired, and it does more than the member does — it resolves the
		// shooter's PlayerState, finds the ability component, and casts. Measuring the member instead
		// would leave the whole resolution path, which is where a wiring mistake actually lives,
		// unexercised.
		const float LooseInterval = TraceSlimeball::GetFireIntervalScaleFor(MyPawn);

		// --- STUCK, shipped ------------------------------------------------------------------------
		Slime->DebugSetStuck(true);
		const float StuckBody = MeasureThroughPassives(nullptr, MyPawn, BodyTestDamage,
			TraceSlimeball::BodyShotCause(), false, false);
		const float StuckHead = MeasureThroughPassives(nullptr, MyPawn, 100.f,
			TraceSlimeball::HeadshotCause(), false, true);
		const float StuckFrontStab = MeasureThroughPassives(nullptr, MyPawn, StabTestDamage,
			TraceMelee::GetKnifeKillCause(), true, false);
		const float StuckBackStab = MeasureThroughPassives(nullptr, MyPawn, 100.f,
			TraceMelee::GetBackstabKillCause(), true, false);
		const float StuckInterval = TraceSlimeball::GetFireIntervalScaleFor(MyPawn);

		// --- STUCK, RED --------------------------------------------------------------------------
		SetCVar(TEXT("Trace.Slimeball.StuckPassive"), 0);
		const float RedBody = MeasureThroughPassives(nullptr, MyPawn, BodyTestDamage,
			TraceSlimeball::BodyShotCause(), false, false);
		const float RedInterval = TraceSlimeball::GetFireIntervalScaleFor(MyPawn);
		SetCVar(TEXT("Trace.Slimeball.StuckPassive"), 1);

		Slime->DebugSetStuck(false);

		const UTraceSettings& Settings = UTraceSettings::Get();
		const float ExpectedBody = BodyTestDamage * (1.f - FMath::Clamp(Settings.SlimeballStuckDamageReduction, 0.f, 1.f));
		const float ExpectedStab = StabTestDamage * (1.f - FMath::Clamp(Settings.SlimeballStuckDamageReduction, 0.f, 1.f));
		const float ExpectedInterval = 1.f / (1.f + FMath::Max(0.f, Settings.SlimeballStuckFireRateBonus));

		State.bRedPassiveReproduced = FMath::IsNearlyEqual(RedBody, BodyTestDamage, 0.01f)
			&& FMath::IsNearlyEqual(RedInterval, 1.f, 0.001f);

		UE_LOG(LogTraceGame, Display,
			TEXT("[SLIME] arm A RED (Trace.Slimeball.StuckPassive 0, still stuck): body %.2f (want %.2f, i.e. "
			     "UNREDUCED) | fire interval x%.4f (want 1.0) -> %s"),
			RedBody, BodyTestDamage, RedInterval,
			State.bRedPassiveReproduced ? TEXT("REPRODUCED")
				: TEXT("*** DID NOT REPRODUCE — the arm disarmed nothing, so the green run is uninformative ***"));

		List.Check(State.bRedPassiveReproduced,
			TEXT("A-RED: with the passive armed off, a stuck Slimeball takes full body damage and fires at 1.0x"),
			FString::Printf(TEXT("body %.2f, interval x%.4f"), RedBody, RedInterval));
		List.Check(FMath::IsNearlyEqual(LooseBody, BodyTestDamage, 0.01f),
			TEXT("A: NOT stuck — a body shot is unreduced"),
			FString::Printf(TEXT("%.2f (want %.2f)"), LooseBody, BodyTestDamage));
		List.Check(FMath::IsNearlyEqual(LooseInterval, 1.f, 0.001f),
			TEXT("A: NOT stuck — the fire interval is unchanged"),
			FString::Printf(TEXT("x%.4f (want 1.0)"), LooseInterval));
		List.Check(FMath::IsNearlyEqual(StuckBody, ExpectedBody, 0.01f),
			TEXT("A: STUCK — a body shot is reduced (§2: 30%)"),
			FString::Printf(TEXT("%.2f (want %.2f)"), StuckBody, ExpectedBody));
		List.Check(FMath::IsNearlyEqual(StuckFrontStab, ExpectedStab, 0.01f),
			TEXT("A: STUCK — a FRONT knife stab is reduced"),
			FString::Printf(TEXT("%.2f (want %.2f)"), StuckFrontStab, ExpectedStab));
		List.Check(FMath::IsNearlyEqual(StuckHead, 100.f, 0.01f),
			TEXT("A: STUCK — a HEADSHOT is NOT reduced (§2 [ASSUMPTION])"),
			FString::Printf(TEXT("%.2f (want 100.00)"), StuckHead));
		List.Check(FMath::IsNearlyEqual(StuckBackStab, 100.f, 0.01f),
			TEXT("A: STUCK — a BACKSTAB is NOT reduced (§2 [ASSUMPTION])"),
			FString::Printf(TEXT("%.2f (want 100.00)"), StuckBackStab));
		List.Check(FMath::IsNearlyEqual(StuckInterval, ExpectedInterval, 0.001f),
			TEXT("A: STUCK — the fire interval DIVIDES by (1 + bonus), i.e. he fires FASTER"),
			FString::Printf(TEXT("x%.4f (want %.4f); %.3fs becomes %.3fs"),
				StuckInterval, ExpectedInterval, Settings.FireInterval, Settings.FireInterval * StuckInterval));
	}

	void RunVerifyStepB(UWorld* WorldPtr, UTraceAbilitySetSlimeball* Slime, ATraceCharacter* Victim,
	                    FVerifyState& State)
	{
		FChecklist& List = State.List;

		APlayerState* MySource = nullptr;
		ETraceTeam MyTeam = ETraceTeam::None;
		if (UTraceAbilityComponent* Comp = Slime->GetAbilityComponent())
		{
			MySource = Comp->GetOwningPlayerState();
			MyTeam = Comp->GetTeam();
		}

		const UTraceSettings& Settings = UTraceSettings::Get();
		const FVector Extents(FMath::Max(10.f, Settings.SlimewallWidthUU) * 0.5f,
		                      FMath::Max(50.f, Settings.SlimewallLengthUU) * 0.5f,
		                      FMath::Max(20.f, Settings.SlimewallHeightUU) * 0.5f);
		const float ExpectedMultiplier = 1.f - FMath::Clamp(Settings.SlimewallSlowFraction, 0.f, 0.95f);

		// THE WALL IS PUT ON TOP OF THE VICTIM rather than the victim walked into the wall: what is
		// under test is the RULE, not the pathing, and a bot who wandered off mid-approach would
		// invalidate a run in which nothing was wrong.
		auto SpawnOnVictim = [&]() -> ATraceSlimewall*
		{
			return ATraceSlimewall::ServerSpawn(WorldPtr, MySource, MyTeam, Victim->GetActorLocation(),
				FVector::ForwardVector, Extents, /*expire*/ 0.f);
		};

		// ---- B1: the shipped wall slows a living, non-carrier enemy -------------------------------
		ATraceSlimewall* Wall = SpawnOnVictim();
		if (Wall == nullptr)
		{
			List.Invalidate(TEXT("could not spawn a Slimewall on the victim"));
			return;
		}
		const bool bInside = Wall->IsInsideWall(Victim);
		Wall->ServerUpdateSlows();
		const UTraceSlimewallSlowComponent* Slow = UTraceSlimewallSlowComponent::Find(Victim);
		const float LandedMultiplier = (Slow != nullptr) ? Slow->GetSpeedMultiplier() : 1.f;
		State.bSlowLanded = (Slow != nullptr) && Slow->IsSlowActive()
			&& FMath::IsNearlyEqual(LandedMultiplier, ExpectedMultiplier, 0.001f);
		Wall->Destroy();

		// ---- B2: RED — Trace.Slimeball.Slow 0 slows nobody ----------------------------------------
		if (UTraceSlimewallSlowComponent* Existing = UTraceSlimewallSlowComponent::Find(Victim))
		{
			Existing->DestroyComponent();
		}
		SetCVar(TEXT("Trace.Slimeball.Slow"), 0);
		Wall = SpawnOnVictim();
		if (Wall != nullptr)
		{
			Wall->ServerUpdateSlows();
			State.bRedSlowSuppressed = (UTraceSlimewallSlowComponent::Find(Victim) == nullptr);
			Wall->Destroy();
		}
		SetCVar(TEXT("Trace.Slimeball.Slow"), 1);

		// ---- B2a: §2's [ASSUMPTION] — "it does not slow Slimeball or his own team" ----------------
		//
		// Staged by giving the WALL the victim's own team rather than by finding a team-mate, for the
		// reason the whole file is arranged this way: what is under test is the rule, and hunting for a
		// live team-mate who happens to be standing somewhere useful makes the measurement about where
		// the bots wandered. Both halves run, because an assumption asserted only in the direction it
		// currently points is not tested — bSlimewallSlowsOwnTeam is the knob that reverses it and it
		// has to be shown actually reversing it.
		if (UTraceSlimewallSlowComponent* Existing = UTraceSlimewallSlowComponent::Find(Victim))
		{
			Existing->DestroyComponent();
		}
		ATraceSlimewall* FriendlyWall = ATraceSlimewall::ServerSpawn(WorldPtr, MySource, Victim->GetTeam(),
			Victim->GetActorLocation(), FVector::ForwardVector, Extents, /*expire*/ 0.f);
		if (FriendlyWall != nullptr)
		{
			FriendlyWall->ServerUpdateSlows();
			State.bOwnTeamSpared = (UTraceSlimewallSlowComponent::Find(Victim) == nullptr);

			// RED: flip the knob and the identical wall on the identical body DOES slow them.
			UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>();
			const bool bSavedSlowsOwnTeam = (MutableSettings != nullptr) && MutableSettings->bSlimewallSlowsOwnTeam;
			if (MutableSettings != nullptr)
			{
				MutableSettings->bSlimewallSlowsOwnTeam = true;
			}
			FriendlyWall->ServerUpdateSlows();
			const UTraceSlimewallSlowComponent* FriendlySlow = UTraceSlimewallSlowComponent::Find(Victim);
			State.bRedOwnTeamReproduced = (FriendlySlow != nullptr) && FriendlySlow->IsSlowActive();
			if (MutableSettings != nullptr)
			{
				MutableSettings->bSlimewallSlowsOwnTeam = bSavedSlowsOwnTeam;
			}

			FriendlyWall->Destroy();
		}

		// ---- B2b: A LINGER OF 0 MUST STILL SLOW SOMEBODY STANDING INSIDE --------------------------
		//
		// This arm exists because it FAILED. SlimewallSlowLingerSeconds is clamped at 0 and 0 is a
		// reasonable ask ("slow them while they are in it, and not a moment longer"); the expiry test in
		// UTraceSlimewallSlowComponent::ServerRefreshNow used `>=`, so at 0 the slow was called expired
		// on the statement after it was applied and the wall slowed nobody at all, inside or out. It is
		// pinned here at the knob value that broke it rather than only at the shipped 0.75 s.
		if (UTraceSlimewallSlowComponent* Existing = UTraceSlimewallSlowComponent::Find(Victim))
		{
			Existing->DestroyComponent();
		}
		{
			UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>();
			const float SavedLinger = (MutableSettings != nullptr) ? MutableSettings->SlimewallSlowLingerSeconds : 0.f;
			if (MutableSettings != nullptr)
			{
				MutableSettings->SlimewallSlowLingerSeconds = 0.f;
			}

			ATraceSlimewall* ZeroLingerWall = SpawnOnVictim();
			if (ZeroLingerWall != nullptr)
			{
				ZeroLingerWall->ServerUpdateSlows();
				const UTraceSlimewallSlowComponent* ZeroSlow = UTraceSlimewallSlowComponent::Find(Victim);
				State.bZeroLingerStillSlows = (ZeroSlow != nullptr) && ZeroSlow->IsSlowActive();
				ZeroLingerWall->Destroy();
			}

			if (MutableSettings != nullptr)
			{
				MutableSettings->SlimewallSlowLingerSeconds = SavedLinger;
			}
		}

		// ---- B3: SPEC §4 — the same victim, now holding the Core ----------------------------------
		ATraceCore* CoreActor = ATraceCore::Get(WorldPtr);
		if (CoreActor == nullptr)
		{
			List.Invalidate(TEXT("no Core actor in the world — the §4 half cannot be staged"));
			return;
		}

		// First: a slow that is ALREADY on them must stop the instant they become the carrier. This is
		// the case a "check the rule once, at application" implementation passes every other test with.
		if (UTraceSlimewallSlowComponent* Existing = UTraceSlimewallSlowComponent::Find(Victim))
		{
			Existing->DestroyComponent();
		}
		Wall = SpawnOnVictim();
		if (Wall == nullptr)
		{
			List.Invalidate(TEXT("could not spawn the §4 Slimewall"));
			return;
		}
		Wall->ServerUpdateSlows();
		UTraceSlimewallSlowComponent* MidSlow = UTraceSlimewallSlowComponent::Find(Victim);
		const bool bSlowedBeforeCore = (MidSlow != nullptr) && MidSlow->IsSlowActive();

		CoreActor->GrantTo(Victim, ETraceCoreGrantReason::Debug);
		const bool bIsCarrierNow = UTraceAbilityComponent::IsCarrier(Victim);
		if (MidSlow != nullptr)
		{
			MidSlow->ServerRefreshNow();
		}
		MidSlow = UTraceSlimewallSlowComponent::Find(Victim);
		State.bCarrierMidSlowStopped = bSlowedBeforeCore && bIsCarrierNow
			&& (MidSlow == nullptr || !MidSlow->IsSlowActive());

		// Second: a FRESH application on the carrier must be refused outright by the choke point.
		if (UTraceSlimewallSlowComponent* Existing = UTraceSlimewallSlowComponent::Find(Victim))
		{
			Existing->DestroyComponent();
		}
		const int32 CarrierAppsBefore = TraceSlimewall::CarrierTally().Applications;
		Wall->ServerUpdateSlows();
		const UTraceSlimewallSlowComponent* CarrierSlow = UTraceSlimewallSlowComponent::Find(Victim);
		State.bCarrierRefused = bIsCarrierNow && (CarrierSlow == nullptr || !CarrierSlow->IsSlowActive())
			&& (TraceSlimewall::CarrierTally().Applications == CarrierAppsBefore);

		// ---- B4: RED — Trace.Ability.CarrierImmune 0 must let the slow REACH the carrier ----------
		if (UTraceSlimewallSlowComponent* Existing = UTraceSlimewallSlowComponent::Find(Victim))
		{
			Existing->DestroyComponent();
		}
		SetCVar(TEXT("Trace.Ability.CarrierImmune"), 0);
		Wall->ServerUpdateSlows();
		const UTraceSlimewallSlowComponent* RedCarrierSlow = UTraceSlimewallSlowComponent::Find(Victim);
		State.bRedCarrierReproduced = bIsCarrierNow && (RedCarrierSlow != nullptr) && RedCarrierSlow->IsSlowActive();
		SetCVar(TEXT("Trace.Ability.CarrierImmune"), 1);

		if (UTraceSlimewallSlowComponent* Existing = UTraceSlimewallSlowComponent::Find(Victim))
		{
			Existing->DestroyComponent();
		}
		Wall->Destroy();

		UE_LOG(LogTraceGame, Display,
			TEXT("[SLIME] arm B: victim %s inside=%d | shipped slow x%.3f (want x%.3f) | RED Trace.Slimeball.Slow 0 "
			     "suppressed=%d | carrier=%d refused=%d midSlowStopped=%d | RED CarrierImmune 0 reproduced=%d"),
			*GetNameSafe(Victim), bInside ? 1 : 0, LandedMultiplier, ExpectedMultiplier,
			State.bRedSlowSuppressed ? 1 : 0, bIsCarrierNow ? 1 : 0, State.bCarrierRefused ? 1 : 0,
			State.bCarrierMidSlowStopped ? 1 : 0, State.bRedCarrierReproduced ? 1 : 0);

		List.Check(State.bSlowLanded,
			TEXT("B: an enemy standing in a Slimewall is slowed (§2: 35%)"),
			FString::Printf(TEXT("x%.3f (want x%.3f), insideWall=%d"), LandedMultiplier, ExpectedMultiplier,
				bInside ? 1 : 0));
		List.Check(State.bRedSlowSuppressed,
			TEXT("B-RED: with Trace.Slimeball.Slow 0 the identical wall slows nobody"),
			TEXT("this is what makes the line above evidence rather than an assertion"));
		List.Check(State.bOwnTeamSpared,
			TEXT("B: his OWN SIDE walks through his wall unslowed (§2 [ASSUMPTION])"),
			TEXT("staged by giving the wall the victim's team, so the rule is tested rather than the bots' positions"));
		List.Check(State.bRedOwnTeamReproduced,
			TEXT("B-RED: with bSlimewallSlowsOwnTeam ON the same wall DOES slow his own side"),
			TEXT("the assumption is reversible in one knob, and this is the proof it is wired to anything"));
		List.Check(State.bZeroLingerStillSlows,
			TEXT("B: at SlimewallSlowLingerSeconds = 0 an enemy INSIDE the wall is still slowed"),
			TEXT("this went red before the fix: the `>=` expiry test retired the slow on the statement after "
			     "it was applied, so a linger of 0 meant the wall slowed nobody anywhere"));
		List.Check(State.bCarrierRefused,
			TEXT("*** B/§4: a CORE CARRIER standing in a Slimewall is NOT slowed ***"),
			FString::Printf(
				TEXT("no slow component attached, and the carrier alarm did not move across the attempt "
				     "(it stood at %d before and after; it reads %d now only because the RED arm below "
				     "deliberately tripped it)"),
				CarrierAppsBefore, TraceSlimewall::CarrierTally().Applications));
		List.Check(State.bCarrierMidSlowStopped,
			TEXT("*** B/§4: an already-slowed player who PICKS UP the Core stops being slowed ***"),
			TEXT("the case a check-once implementation passes every other test with"));
		List.Check(State.bRedCarrierReproduced,
			TEXT("B-RED/§4: with Trace.Ability.CarrierImmune 0 the slow DOES reach the carrier"),
			TEXT("this is the failure the shipped build must prevent"));
	}

	// =============================================================================================
	// arm D — THE ACTIVATED ABILITY, THROUGH THE REAL PRESS
	// =============================================================================================

	/**
	 * The most open horizontal bearing from @p From, and true when it has at least @p NeedUU of room.
	 *
	 * Deliberately NOT shared with arm C's fixture, which looks superficially similar and asks a
	 * different question: C needs a FIRING LANE with a body standing at the far end of it, this needs
	 * ROOM FOR A SLAB and does not care what is beyond it.
	 */
	bool FindOpenBearing(UWorld* WorldPtr, const ATraceCharacter* MyPawn, float NeedUU, FVector& OutDirection)
	{
		OutDirection = FVector::ZeroVector;
		if (WorldPtr == nullptr || MyPawn == nullptr)
		{
			return false;
		}

		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceSlimeballOpenBearing), /*bTraceComplex*/ false);
		QueryParams.AddIgnoredActor(MyPawn);

		const FVector From = MyPawn->GetActorLocation();
		float BestClearance = 0.f;

		for (int32 Index = 0; Index < 16; ++Index)
		{
			const float Angle = (2.f * PI * static_cast<float>(Index)) / 16.f;
			const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);

			FHitResult Hit;
			const float Clearance = WorldPtr->LineTraceSingleByObjectType(Hit, From, From + Direction * (NeedUU + 200.f),
				ObjectParams, QueryParams)
				? static_cast<float>(Hit.Distance)
				: (NeedUU + 200.f);

			if (Clearance > BestClearance)
			{
				BestClearance = Clearance;
				OutDirection = Direction;
			}
		}

		return !OutDirection.IsNearlyZero() && BestClearance >= NeedUU;
	}

	/** A face he can be stood nose-to-nose with, for the fizzle. Standing height; no elevation hunt. */
	bool FindFaceToStandAgainst(UWorld* WorldPtr, const ATraceCharacter* MyPawn, FVector& OutStand, FVector& OutInto)
	{
		OutStand = FVector::ZeroVector;
		OutInto = FVector::ZeroVector;
		if (WorldPtr == nullptr || MyPawn == nullptr)
		{
			return false;
		}

		float CapsuleRadius = 34.f;
		if (const UCapsuleComponent* Capsule = MyPawn->GetCapsuleComponent())
		{
			CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		}

		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceSlimeballFizzleFixture), /*bTraceComplex*/ false);
		QueryParams.AddIgnoredActor(MyPawn);

		const FVector From = MyPawn->GetActorLocation();
		const float MaxNormalZ = UTraceSettings::Get().SlimeballWallStickMaxSurfaceNormalZ;

		for (int32 Index = 0; Index < 16; ++Index)
		{
			const float Angle = (2.f * PI * static_cast<float>(Index)) / 16.f;
			const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);

			FHitResult Hit;
			if (!WorldPtr->LineTraceSingleByObjectType(Hit, From, From + Direction * 4000.f, ObjectParams, QueryParams))
			{
				continue;
			}
			if (FMath::Abs(Hit.ImpactNormal.Z) > MaxNormalZ)
			{
				continue;   // a floor or a ceiling is not something you can stand nose-to-nose with
			}

			OutStand = Hit.ImpactPoint + Hit.ImpactNormal * (CapsuleRadius + 6.f);
			OutStand.Z = From.Z;
			OutInto = -Hit.ImpactNormal.GetSafeNormal();
			return true;
		}
		return false;
	}

	/**
	 * ARM D — THE ACTIVATED ABILITY, THROUGH THE ENTRY POINT E ACTUALLY USES.
	 *
	 * THIS ARM EXISTS BECAUSE OF WHAT IT COVERS THAT NOTHING ELSE DID. Arms B and C both build their
	 * walls with ATraceSlimewall::ServerSpawn, which is right for what they measure — the slow, and the
	 * bullet — but it means that until this arm was written, UTraceAbilitySetSlimeball::ActivateAbility()
	 * and the placement sweep behind it were never called by any test. A ResolveSlimewallPlacement that
	 * refused every request, or an ActivateAbility that spawned nothing, would have left every other
	 * check in this file green while the ability did nothing whatsoever in a playtest.
	 *
	 * So this one presses the button: UTraceAbilityComponent::TryActivate(), the same call E goes
	 * through, with every one of the framework's refusals in front of it and the cooldown behind it.
	 *
	 * Returns FALSE to ask for another go when he has nowhere to stand — a fact about where the match
	 * put him, not a failure.
	 */
	bool RunVerifyStepD(UWorld* WorldPtr, UTraceAbilitySetSlimeball* Slime, FVerifyState& State)
	{
		FChecklist& List = State.List;

		ATraceCharacter* MyPawn = Slime->GetCharacter();
		UTraceAbilityComponent* Comp = Slime->GetAbilityComponent();
		if (MyPawn == nullptr || Comp == nullptr)
		{
			List.Invalidate(TEXT("arm D lost Slimeball's pawn or ability component"));
			return true;
		}

		const UTraceSettings& Settings = UTraceSettings::Get();

		auto FaceAlong = [MyPawn](const FVector& Direction)
		{
			if (AController* PawnController = MyPawn->GetController())
			{
				PawnController->SetControlRotation(Direction.Rotation());
			}
		};

		// ---- D1: THE FREE FIZZLE, FIRST, so the arm ends with him standing in the open -----------
		//
		// Nose-to-nose with real geometry and looking straight into it. §2 charges 25 s for a wall; it
		// must not charge 25 s for a wall that was never placed, and UTraceCharacterAbilitySet's header
		// names Mace's spike hitting nothing as the precedent.
		FVector FizzleStand = FVector::ZeroVector;
		FVector FizzleInto = FVector::ZeroVector;
		if (FindFaceToStandAgainst(WorldPtr, MyPawn, FizzleStand, FizzleInto))
		{
			Comp->DebugSetActivatedCooldown(0.f);
			MyPawn->TeleportTo(FizzleStand, MyPawn->GetActorRotation(), /*bIsATest*/ false, /*bNoCheck*/ true);
			FaceAlong(FizzleInto);

			const bool bFizzleFired = Comp->TryActivate();
			const float FizzleCooldown = Comp->GetActivatedCooldownRemaining();
			State.bFizzleWasFree = !bFizzleFired && (Slime->GetActiveWall() == nullptr) && (FizzleCooldown <= 0.01f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[SLIME] arm D fizzle: pressed E with his nose against a wall — fired=%d, wall=%s, cooldown "
				     "left %.2fs (want 0.00, i.e. FREE)"),
				bFizzleFired ? 1 : 0, *GetNameSafe(Slime->GetActiveWall()), FizzleCooldown);
		}

		// ---- stage the open ground both remaining halves use -------------------------------------
		const float NeedRoom = FMath::Max(0.f, Settings.SlimewallRangeUU) + FMath::Max(10.f, Settings.SlimewallWidthUU);
		FVector Bearing = FVector::ZeroVector;
		if (!FindOpenBearing(WorldPtr, MyPawn, NeedRoom, Bearing))
		{
			return false;   // boxed in; he or the bots will move
		}
		FaceAlong(Bearing);

		// ---- D2: RED. Armed off, the same press in the same place must do nothing ----------------
		Comp->DebugSetActivatedCooldown(0.f);
		SetCVar(TEXT("Trace.Slimeball.Slimewall"), 0);
		const bool bRedFired = Comp->TryActivate();
		const float RedCooldown = Comp->GetActivatedCooldownRemaining();
		State.bRedActivationSuppressed = !bRedFired && (Slime->GetActiveWall() == nullptr) && (RedCooldown <= 0.01f);
		SetCVar(TEXT("Trace.Slimeball.Slimewall"), 1);

		// ---- D3: GREEN. The shipped press, in the same place, on the same frame's geometry -------
		Comp->DebugSetActivatedCooldown(0.f);
		const bool bFired = Comp->TryActivate();
		ATraceSlimewall* Wall = Slime->GetActiveWall();
		const float Cooldown = Comp->GetActivatedCooldownRemaining();
		State.bWallWentUp = bFired && (Wall != nullptr);

		// ---- D4: the second press, on the same statement sequence, must be refused ---------------
		const bool bSecondFired = Comp->TryActivate();
		const bool bSameWallStanding = (Slime->GetActiveWall() == Wall) && (Wall != nullptr);

		// ---- measure what actually went up -------------------------------------------------------
		float AheadUU = 0.f;
		float AimAgreement = 0.f;
		float LifeSeconds = 0.f;
		FVector Extents = FVector::ZeroVector;
		if (Wall != nullptr)
		{
			const FVector PlanarAim = FVector(MyPawn->GetAimDirection().X, MyPawn->GetAimDirection().Y, 0.f).GetSafeNormal();
			const FVector ToWall = Wall->GetWallCenter() - MyPawn->GetActorLocation();
			AheadUU = static_cast<float>(FVector::DotProduct(FVector(ToWall.X, ToWall.Y, 0.f), PlanarAim));
			AimAgreement = static_cast<float>(FVector::DotProduct(Wall->GetWallAim(), PlanarAim));
			LifeSeconds = Wall->GetExpireMatchTime() - Slime->MatchTimeNow();
			Extents = Wall->GetHalfExtentsUU();
		}

		const FVector WantExtents(FMath::Max(10.f, Settings.SlimewallWidthUU) * 0.5f,
		                          FMath::Max(50.f, Settings.SlimewallLengthUU) * 0.5f,
		                          FMath::Max(20.f, Settings.SlimewallHeightUU) * 0.5f);

		UE_LOG(LogTraceGame, Display,
			TEXT("[SLIME] arm D: RED (armed off) fired=%d cooldown %.2fs | GREEN fired=%d wall=%s at %.0f uu ahead "
			     "(knob %.0f), aim agreement %.3f, %.0fx%.0fx%.0f, lasts %.2fs, cooldown %.1fs | second press "
			     "fired=%d, same wall still standing=%d"),
			bRedFired ? 1 : 0, RedCooldown, bFired ? 1 : 0, *GetNameSafe(Wall), AheadUU, Settings.SlimewallRangeUU,
			AimAgreement, Extents.X * 2.f, Extents.Y * 2.f, Extents.Z * 2.f, LifeSeconds, Cooldown,
			bSecondFired ? 1 : 0, bSameWallStanding ? 1 : 0);

		List.Check(State.bRedActivationSuppressed,
			TEXT("D-RED: with Trace.Slimeball.Slimewall 0 the press puts up no wall and costs no cooldown"),
			FString::Printf(TEXT("fired=%d, cooldown left %.2fs"), bRedFired ? 1 : 0, RedCooldown));
		List.Check(State.bFizzleWasFree,
			TEXT("D: pressing E with nowhere to put a wall is a FREE fizzle, not a wasted 25 s"),
			TEXT("§2 charges for a wall, and a wall that was never placed is not one"));
		List.Check(State.bWallWentUp,
			TEXT("*** D: pressing E actually throws a Slimewall up in the world ***"),
			FString::Printf(TEXT("TryActivate returned %d and GetActiveWall() is %s — the placement sweep and "
				"ActivateAbility are exercised by no other arm in this file"),
				bFired ? 1 : 0, *GetNameSafe(Wall)));
		List.Check(Wall != nullptr && AheadUU > 0.f && AheadUU <= Settings.SlimewallRangeUU + 1.f,
			TEXT("D: it goes up IN FRONT of him, no further than the placement knob"),
			FString::Printf(TEXT("%.0f uu ahead, knob is %.0f"), AheadUU, Settings.SlimewallRangeUU));
		List.Check(Wall != nullptr && AimAgreement > 0.99f,
			TEXT("D: and it faces his aim direction (§2: 'in his aim direction')"),
			FString::Printf(TEXT("dot(wall aim, his planar aim) = %.4f"), AimAgreement));
		List.Check(Wall != nullptr && Extents.Equals(WantExtents, 0.5f),
			TEXT("D: at exactly the three dimension knobs (§2: 'make this changeable')"),
			FString::Printf(TEXT("half extents %s, want %s"), *Extents.ToCompactString(), *WantExtents.ToCompactString()));
		List.Check(Wall != nullptr && FMath::IsNearlyEqual(LifeSeconds, Settings.SlimewallDurationSeconds, 0.2f),
			TEXT("D: and it lasts the duration knob (§2: 4 s)"),
			FString::Printf(TEXT("%.2fs, knob is %.2fs"), LifeSeconds, Settings.SlimewallDurationSeconds));
		List.Check(FMath::IsNearlyEqual(Cooldown, Settings.SlimewallCooldownSeconds, 0.2f),
			TEXT("D: a wall that DID go up charges the cooldown (§2: 25 s)"),
			FString::Printf(TEXT("%.2fs left, knob is %.2fs"), Cooldown, Settings.SlimewallCooldownSeconds));
		List.Check(!bSecondFired && bSameWallStanding,
			TEXT("D: a second press inside the cooldown is refused and does NOT replace the standing wall"),
			FString::Printf(TEXT("second press fired=%d, same wall standing=%d"),
				bSecondFired ? 1 : 0, bSameWallStanding ? 1 : 0));

		// The wall is his, not the harness's: leave nothing standing for arm C to shoot through by
		// accident. Its own 4 s would have done this, but not before C runs.
		if (Wall != nullptr)
		{
			Wall->Destroy();
		}
		Comp->DebugSetActivatedCooldown(0.f);
		return true;
	}

	/**
	 * Runs arm C. Returns FALSE to ask for another go, which happens when no enemy currently has a
	 * clear sight line to Slimeball — in a live arena that is a fact about where the bots wandered,
	 * not a failure, so it is retried rather than reported.
	 */
	bool RunVerifyStepC(UWorld* WorldPtr, UTraceAbilitySetSlimeball* Slime, FVerifyState& State)
	{
		FChecklist& List = State.List;

		ATraceCharacter* MyPawn = Slime->GetCharacter();
		if (MyPawn == nullptr)
		{
			List.Invalidate(TEXT("arm C lost Slimeball's pawn"));
			return true;
		}

		APlayerState* MySource = nullptr;
		ETraceTeam MyTeam = ETraceTeam::None;
		if (UTraceAbilityComponent* Comp = Slime->GetAbilityComponent())
		{
			MySource = Comp->GetOwningPlayerState();
			MyTeam = Comp->GetTeam();
		}

		const UTraceSettings& Settings = UTraceSettings::Get();
		const FVector Origin = MyPawn->GetMuzzleLocation();
		const float Range = FMath::Max(1.f, Settings.HitscanRange);

		float ServerNow = 0.f;
		if (const AGameStateBase* ClockState = WorldPtr->GetGameState())
		{
			ServerNow = static_cast<float>(ClockState->GetServerWorldTimeSeconds());
		}

		// ---- C0: THE FIXTURE PLACES ITS OWN SUBJECT, AND THAT IS A CORRECTION. -------------------
		//
		// The first version took whichever enemy was to hand and asked the real resolver whether it
		// could see them. Measured over a full 60 s of a live match: NEVER — the arena is 1291 cover
		// boxes and ten bots who are, at any instant, almost certainly not standing in one another's
		// open sight lines. The harness duly reported INVALID for a build in which nothing was wrong,
		// which is the same failure mode the X fixture's "the carrier must be an ENEMY" note records.
		//
		// So the subject is STOOD somewhere with a clear line instead of waited for. One bot is moved
		// one time, which is the same class of intervention as the Core grant arm B makes, and it is
		// the only way this measurement is about walls rather than about level layout.
		const float MinimumSeparation = FMath::Max(10.f, Settings.SlimewallWidthUU) + 300.f;

		if (State.CPhase == 0)
		{
			ATraceCharacter* Subject = nullptr;
			for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
			{
				ATraceCharacter* Candidate = *It;
				if (Candidate != nullptr && Candidate != MyPawn && Candidate->IsAlive()
					&& Candidate->Health != nullptr && !UTraceAbilityComponent::IsCarrier(Candidate)
					&& Candidate->GetTeam() != MyTeam && Candidate->GetTeam() != ETraceTeam::None)
				{
					Subject = Candidate;
					break;
				}
			}
			if (Subject == nullptr)
			{
				return false;
			}

			// The most open horizontal direction from the muzzle. Eight probes, and the winner needs
			// room for the slab AND a body behind it, or the "wall" under test would be the arena's.
			FCollisionObjectQueryParams ObjectParams;
			ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceSlimeballShotFixture), /*bTraceComplex*/ false);
			QueryParams.AddIgnoredActor(MyPawn);
			QueryParams.AddIgnoredActor(Subject);

			constexpr float DesiredStandoff = 1000.f;
			float BestClearance = 0.f;
			FVector BestDirection = FVector::ZeroVector;
			for (int32 Index = 0; Index < 8; ++Index)
			{
				const float Angle = (2.f * PI * static_cast<float>(Index)) / 8.f;
				const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);

				FHitResult Hit;
				const float Clearance = WorldPtr->LineTraceSingleByObjectType(Hit, Origin,
					Origin + Direction * (DesiredStandoff + 400.f), ObjectParams, QueryParams)
					? static_cast<float>(Hit.Distance)
					: (DesiredStandoff + 400.f);

				if (Clearance > BestClearance)
				{
					BestClearance = Clearance;
					BestDirection = Direction;
				}
			}

			if (BestDirection.IsNearlyZero() || BestClearance < MinimumSeparation + 200.f)
			{
				return false;   // Slimeball is boxed in; try again next tick, he or the bots will move
			}

			const float Standoff = FMath::Min(DesiredStandoff, BestClearance - 150.f);
			FVector Stand = Origin + BestDirection * Standoff;

			FHitResult FloorHit;
			if (WorldPtr->LineTraceSingleByObjectType(FloorHit, Stand + FVector(0.f, 0.f, 200.f),
				Stand - FVector(0.f, 0.f, 1000.f), ObjectParams, QueryParams))
			{
				Stand.Z = static_cast<float>(FloorHit.ImpactPoint.Z) + 90.f;
			}

			Subject->TeleportTo(Stand, Subject->GetActorRotation(), /*bIsATest*/ false, /*bNoCheck*/ true);
			State.CVictim = Subject;
			State.CPhase = 1;

			// A TICK MUST PASS BEFORE MEASURING, and this is not padding. ResolveHitscan resolves
			// bodies against UTraceLagCompensationComponent's POSE HISTORY, which is written once per
			// tick — so measuring in the same statement sequence as the teleport would test the shot
			// against where the subject WAS, and the control would miss for a reason that has nothing
			// to do with walls. The three arms themselves still run with no tick between them.
			return false;
		}

		ATraceCharacter* Victim = State.CVictim.Get();
		if (Victim == nullptr || !Victim->IsAlive() || Victim->Health == nullptr
			|| UTraceAbilityComponent::IsCarrier(Victim))
		{
			State.CPhase = 0;
			return false;
		}

		// ---- C1: THE CONTROL. No wall at all. If this misses, nothing below means anything. -------
		const FVector ToVictim = Victim->GetActorLocation() - Origin;
		const FVector Dir = ToVictim.GetSafeNormal();

		auto Resolve = [&](ATraceCharacter*& OutHit)
		{
			FVector Impact = FVector::ZeroVector;
			ETraceHitZone Zone = ETraceHitZone::None;
			OutHit = UTraceLagCompensationComponent::ResolveHitscan(WorldPtr, MyPawn, Origin, Dir, Range,
				ServerNow, Impact, Zone);
		};

		ATraceCharacter* ControlHit = nullptr;
		Resolve(ControlHit);
		if (ControlHit != Victim || ToVictim.Size() < MinimumSeparation)
		{
			State.CPhase = 0;   // they moved, or the stand was poor. Re-place and try again.
			return false;
		}
		State.bControlShotLanded = true;

		// ---- C2: RED. A wall with BlockAll across the line. The shot MUST be lost. ----------------
		const FVector Midpoint = Origin + ToVictim * 0.5f;
		const FVector Extents(FMath::Max(10.f, Settings.SlimewallWidthUU) * 0.5f,
		                      FMath::Max(50.f, Settings.SlimewallLengthUU) * 0.5f,
		                      FMath::Max(20.f, Settings.SlimewallHeightUU) * 0.5f);
		const FVector PlanarDir = FVector(Dir.X, Dir.Y, 0.f).GetSafeNormal();

		SetCVar(TEXT("Trace.Slimeball.WallBlocksBullets"), 1);
		ATraceSlimewall* RedWall = ATraceSlimewall::ServerSpawn(WorldPtr, MySource, MyTeam, Midpoint,
			PlanarDir, Extents, /*expire*/ 0.f);
		bool bRedWallCollides = false;
		ATraceCharacter* RedHit = nullptr;
		if (RedWall != nullptr)
		{
			bRedWallCollides = RedWall->HasAnyCollisionEnabled();
			Resolve(RedHit);
			RedWall->Destroy();
		}
		SetCVar(TEXT("Trace.Slimeball.WallBlocksBullets"), 0);
		State.bRedWallAteTheShot = bRedWallCollides && (RedHit != Victim);

		// ---- C3: GREEN. The shipped wall, in the same place. The shot must land AND kill. ---------
		ATraceSlimewall* GreenWall = ATraceSlimewall::ServerSpawn(WorldPtr, MySource, MyTeam, Midpoint,
			PlanarDir, Extents, /*expire*/ 0.f);
		bool bGreenWallCollides = true;
		ATraceCharacter* GreenHit = nullptr;
		bool bKilled = false;
		FString WallMaterial = TEXT("<no wall>");
		if (GreenWall != nullptr)
		{
			bGreenWallCollides = GreenWall->HasAnyCollisionEnabled();
			WallMaterial = GreenWall->DescribeSlabMaterial();
			Resolve(GreenHit);

			// "Prove a shot passes through it and STILL KILLS." Read, damage, read — one statement
			// sequence, with the wall still standing.
			Victim->Health->ResetHealth();
			Victim->Health->ApplyDamage(500.f, nullptr, FName(TEXT("SlimeballVerify")));
			bKilled = !Victim->IsAlive();

			GreenWall->Destroy();
		}
		UE_LOG(LogTraceGame, Display,
			TEXT("[SLIME] arm C: control hit=%s | RED wall (BlockAll, collides=%d) hit=%s | SHIPPED wall "
			     "(collides=%d, material %s) hit=%s, victim killed through it=%d"),
			*GetNameSafe(ControlHit), bRedWallCollides ? 1 : 0, *GetNameSafe(RedHit),
			bGreenWallCollides ? 1 : 0, *WallMaterial, *GetNameSafe(GreenHit), bKilled ? 1 : 0);

		List.Check(State.bRedWallAteTheShot,
			TEXT("C-RED: a wall WITH collision silently eats the hitscan"),
			TEXT("this is the exact bug §2 forbids, and reproducing it is what makes C green mean something"));
		List.Check(!bGreenWallCollides,
			TEXT("*** C: the shipped Slimewall has NO collision on any component ***"),
			TEXT("'can be shot through' — the invariant, asked of the actor rather than of a comment"));
		List.Check(GreenHit == Victim,
			TEXT("*** C: a shot through a standing Slimewall still resolves onto the enemy behind it ***"),
			FString::Printf(TEXT("resolver returned %s"), *GetNameSafe(GreenHit)));
		List.Check(bKilled,
			TEXT("*** C: and it still KILLS ***"),
			FString::Printf(TEXT("victim alive after 500 damage through the wall = %d"),
				Victim->IsAlive() ? 1 : 0));

		return true;
	}

	void RunVerify()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[SLIME] no authoritative game world — run this on the server, in a live match."));
			return;
		}

		TSharedPtr<FVerifyState> State = MakeShared<FVerifyState>();
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[SLIME] ===== spec v18 §2: the two stuck passives, the 35%% slow (with spec §4's carrier rule), "
			     "and 'can be shot through'. Every arm is RED FIRST. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				RestoreAllArms();
				return false;
			}

			FString Why;
			UTraceAbilitySetSlimeball* Slime = MakePlayerIntoSlimeball(TickWorld, Why);
			ATraceCharacter* Victim = (Slime != nullptr) ? FindEnemyTarget(TickWorld, Slime) : nullptr;

			if (Slime == nullptr || Slime->GetCharacter() == nullptr || Victim == nullptr)
			{
				if (FPlatformTime::Seconds() > State->Deadline)
				{
					State->List.Invalidate(FString::Printf(
						TEXT("could not stage: slimeball=%s victim=%s (%s). Run this EARLY in a match, before "
						     "the bots have claimed every character."),
						(Slime != nullptr) ? TEXT("ok") : TEXT("none"), *GetNameSafe(Victim), *Why));
					State->List.Report();
					RestoreAllArms();
					return false;
				}
				return true;
			}

			Victim->Health->ResetHealth();

			switch (State->Step)
			{
			case 0:
				RunVerifyStepA(TickWorld, Slime, *State);
				++State->Step;
				return true;

			case 1:
				RunVerifyStepB(TickWorld, Slime, Victim, *State);
				++State->Step;
				return true;

			case 2:
				// Arm D MOVES SLIMEBALL and must therefore run before arm C, which measures a sight line
				// from wherever he is standing. It deliberately finishes with him in open ground, which
				// is the best position C could ask for. It asks for another go while he is boxed in.
				if (!RunVerifyStepD(TickWorld, Slime, *State))
				{
					if (FPlatformTime::Seconds() <= State->Deadline)
					{
						return true;
					}
					State->List.Invalidate(TEXT("arm D never found open ground to throw a wall into, so the "
					                            "activation path could not be pressed at all"));
				}
				++State->Step;
				return true;

			default:
				break;
			}

			// Arm C kills its victim, so it runs last, and it picks its OWN subject: the enemy it can
			// currently see (step B handed the previous one the Core, and bots move). It asks for
			// another go until somebody is in the open or the deadline runs out.
			if (!RunVerifyStepC(TickWorld, Slime, *State))
			{
				if (FPlatformTime::Seconds() <= State->Deadline)
				{
					return true;
				}
				State->List.Invalidate(TEXT("no enemy ever had a clear sight line to Slimeball, so the CONTROL "
				                            "shot could not be staged and nothing arm C would say about walls "
				                            "would mean anything. Run it with the players in the open."));
			}

			// ---- verdict ---------------------------------------------------------------------
			//
			// THE RED ARMS ARE CHECKED BEFORE THE GREEN RESULTS ARE BELIEVED. If nothing was
			// falsified, a clean green run says only that the code did not crash.
			if (State->bControlShotLanded
				&& (!State->bRedPassiveReproduced || !State->bRedSlowSuppressed
					|| !State->bRedOwnTeamReproduced || !State->bRedActivationSuppressed
					|| !State->bRedCarrierReproduced || !State->bRedWallAteTheShot))
			{
				State->List.Invalidate(FString::Printf(
					TEXT("a RED arm did not reproduce (passive=%d slow=%d ownTeam=%d activation=%d carrier=%d "
					     "shotThrough=%d) — with nothing falsified, the green runs are uninformative"),
					State->bRedPassiveReproduced ? 1 : 0, State->bRedSlowSuppressed ? 1 : 0,
					State->bRedOwnTeamReproduced ? 1 : 0, State->bRedActivationSuppressed ? 1 : 0,
					State->bRedCarrierReproduced ? 1 : 0, State->bRedWallAteTheShot ? 1 : 0));
			}

			State->List.Check(State->bControlShotLanded,
				TEXT("C-CONTROL: with no wall at all, the shot reaches the enemy"),
				TEXT("the fixture proving itself — without this, 'the wall did not block it' is meaningless"));

			UE_LOG(LogTraceGame, Display,
				TEXT("[SLIME] carrier alarm: %d application(s), %d slowed frame(s) on a Core carrier for the life "
				     "of this process (MUST be 0 outside the red arm)."),
				TraceSlimewall::CarrierTally().Applications, TraceSlimewall::CarrierTally().SlowFrames);

			State->List.Report();
			RestoreAllArms();
			return false;
		}), 0.25f);
	}

	// =============================================================================================
	// Trace.Slimeball.StickTest — the movement half, against real geometry
	// =============================================================================================

	struct FStickState
	{
		int32 Arm = 0;          // 0 = RED (Trace.Slimeball.WallStick 0), 1 = GREEN (shipped)
		int32 Step = 0;
		int32 Samples = 0;
		double Deadline = 0.0;
		FVector StartLocation = FVector::ZeroVector;
		float DropRed = 0.f;
		float DropGreen = 0.f;
		bool bStuckRed = false;
		bool bStuckGreen = false;
		FChecklist List;
	};

	/**
	 * Finds a wall the level actually has, and a standing spot beside it that is BOTH airborne and
	 * still touching wall.
	 *
	 * THE SECOND HALF IS THE WHOLE DIFFICULTY, and getting it wrong makes the fixture lie in the
	 * cruellest direction — a green arm that reports "he did not stick" for a build that sticks fine.
	 * Two requirements pull against each other:
	 *
	 *   he must be OFF THE FLOOR, or he cannot stick at all (the ability requires air) and the RED
	 *   arm has nowhere to fall from, so it cannot reproduce either;
	 *   there must be WALL at that height, and most of this arena's geometry is one-player-height
	 *   cover (176 uu), so a standpoint 250 uu up is usually looking at thin air above a box.
	 *
	 * So a candidate face is found at standing height and then RE-PROBED from each candidate
	 * elevation, tallest first, and only an elevation that still finds wall is returned.
	 */
	bool FindWallStandpoint(UWorld* WorldPtr, ATraceCharacter* Pawn, FVector& OutStand, FString& OutWhy)
	{
		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceSlimeballStickFixture), /*bTraceComplex*/ false);
		QueryParams.AddIgnoredActor(Pawn);

		float CapsuleRadius = 34.f;
		if (const UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent())
		{
			CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		}

		const float MaxNormalZ = UTraceSettings::Get().SlimeballWallStickMaxSurfaceNormalZ;
		const float Reach = FMath::Clamp(UTraceSettings::Get().SlimeballWallStickRangeUU, 10.f, 500.f);

		const FVector From = Pawn->GetActorLocation();
		constexpr int32 Rays = 16;
		constexpr float SearchRange = 4000.f;

		// Tallest first: the further he starts from the floor, the more decisively the RED arm falls.
		const float Elevations[] = { 260.f, 200.f, 150.f, 110.f };

		for (int32 Index = 0; Index < Rays; ++Index)
		{
			const float Angle = (2.f * PI * static_cast<float>(Index)) / static_cast<float>(Rays);
			const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);

			FHitResult Hit;
			if (!WorldPtr->LineTraceSingleByObjectType(Hit, From, From + Direction * SearchRange,
				ObjectParams, QueryParams))
			{
				continue;
			}
			if (FMath::Abs(Hit.ImpactNormal.Z) > MaxNormalZ)
			{
				continue;
			}

			for (const float Elevation : Elevations)
			{
				const FVector Candidate =
					Hit.ImpactPoint + Hit.ImpactNormal * (CapsuleRadius + 6.f) + FVector(0.f, 0.f, Elevation);

				// The stick's OWN question, asked from the candidate spot: is there still wall here?
				FHitResult Confirm;
				if (!WorldPtr->LineTraceSingleByObjectType(Confirm, Candidate,
					Candidate - Hit.ImpactNormal * Reach, ObjectParams, QueryParams))
				{
					continue;
				}
				if (FMath::Abs(Confirm.ImpactNormal.Z) > MaxNormalZ)
				{
					continue;
				}

				OutStand = Candidate;
				OutWhy = FString::Printf(TEXT("wall confirmed %.0f uu up, normal %s"),
					Elevation, *Confirm.ImpactNormal.ToCompactString());
				return true;
			}
		}

		OutWhy = FString::Printf(
			TEXT("no wall within %.0f uu of the player, in any of %d directions, that is still wall at 110-260 uu "
			     "above standing height (most of this arena is one-player-height cover)"),
			SearchRange, Rays);
		return false;
	}

	void RunStickTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[SLIMESTICK] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FStickState> State = MakeShared<FStickState>();
		State->List.Tag = TEXT("SLIMESTICK");
		State->Deadline = FPlatformTime::Seconds() + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[SLIMESTICK] ===== spec v18 §2: 'Wall stick (Hold V): sticks to walls while held.' He is placed "
			     "beside a wall the level actually has, dropped, and told to hold V. arm 0 is RED "
			     "(Trace.Slimeball.WallStick 0) and MUST see him fall. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				RestoreAllArms();
				return false;
			}

			FString Why;
			UTraceAbilitySetSlimeball* Slime = MakePlayerIntoSlimeball(TickWorld, Why);
			ATraceCharacter* MyPawn = (Slime != nullptr) ? Slime->GetCharacter() : nullptr;
			UTraceCharacterMovementComponent* MoveComp = (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;

			if (Slime == nullptr || MyPawn == nullptr || MoveComp == nullptr)
			{
				if (FPlatformTime::Seconds() > State->Deadline)
				{
					State->List.Invalidate(FString::Printf(TEXT("could not stage: %s"), *Why));
					State->List.Report();
					RestoreAllArms();
					return false;
				}
				return true;
			}

			// ---- place him ------------------------------------------------------------------
			if (State->Step == 0)
			{
				SetCVar(TEXT("Trace.Slimeball.WallStick"), State->Arm == 0 ? 0 : 1);

				FVector Stand = FVector::ZeroVector;
				FString StandWhy;
				if (!FindWallStandpoint(TickWorld, MyPawn, Stand, StandWhy))
				{
					if (FPlatformTime::Seconds() > State->Deadline)
					{
						State->List.Invalidate(FString::Printf(TEXT("no wall to test against: %s"), *StandWhy));
						State->List.Report();
						RestoreAllArms();
						return false;
					}
					return true;
				}

				MyPawn->TeleportTo(Stand, MyPawn->GetActorRotation(), /*bIsATest*/ false, /*bNoCheck*/ true);
				MoveComp->SetMovementMode(MOVE_Falling);
				MoveComp->Velocity = FVector::ZeroVector;

				State->StartLocation = MyPawn->GetActorLocation();
				State->Samples = 0;
				State->Step = 1;

				// The press goes through the ability's real entry point, not through StartStick, so
				// every gate the shipped build applies is applied here too.
				Slime->OnSecondaryPressed();
				return true;
			}

			// ---- hold V for a while and watch ------------------------------------------------
			++State->Samples;
			const bool bStuckNow = Slime->IsStuck();
			const float Drop = State->StartLocation.Z - static_cast<float>(MyPawn->GetActorLocation().Z);

			if (State->Samples < 6)
			{
				return true;   // ~1.5 s at the ticker's 0.25 s
			}

			Slime->OnSecondaryReleased();

			if (State->Arm == 0)
			{
				State->bStuckRed = bStuckNow;
				State->DropRed = Drop;
				UE_LOG(LogTraceGame, Display,
					TEXT("[SLIMESTICK] arm=0 (RED, Trace.Slimeball.WallStick 0): stuck=%d, fell %.0f uu in ~1.5 s -> %s"),
					bStuckNow ? 1 : 0, Drop,
					(!bStuckNow && Drop > 100.f)
						? TEXT("REPRODUCED — with the ability armed off he falls past the wall.")
						: TEXT("*** DID NOT REPRODUCE — the arm disarmed nothing, so arm 1 proves nothing. ***"));

				State->Arm = 1;
				State->Step = 0;
				return true;
			}

			State->bStuckGreen = bStuckNow;
			State->DropGreen = Drop;
			UE_LOG(LogTraceGame, Display,
				TEXT("[SLIMESTICK] arm=1 (GREEN, shipped): stuck=%d, moved %.0f uu in ~1.5 s (slide knob is %.0f uu/s)"),
				bStuckNow ? 1 : 0, State->DropGreen, UTraceSettings::Get().SlimeballWallStickSlideSpeed);

			const bool bRedReproduced = !State->bStuckRed && State->DropRed > 100.f;
			if (!bRedReproduced)
			{
				State->List.Invalidate(FString::Printf(
					TEXT("the RED arm did not reproduce (stuck=%d, drop %.0f uu) — with nothing falsified, the "
					     "green arm's clean run is uninformative"),
					State->bStuckRed ? 1 : 0, State->DropRed));
			}

			State->List.Check(bRedReproduced,
				TEXT("RED: with Trace.Slimeball.WallStick 0, holding V beside a wall does nothing and he falls"),
				FString::Printf(TEXT("stuck=%d, fell %.0f uu"), State->bStuckRed ? 1 : 0, State->DropRed));
			State->List.Check(State->bStuckGreen,
				TEXT("GREEN: holding V beside a wall STICKS him to it"),
				FString::Printf(TEXT("stuck=%d after ~1.5 s of holding"), State->bStuckGreen ? 1 : 0));
			// THE TOLERANCE IS NOT ZERO, AND THE REASON IS MEASURED RATHER THAN GUESSED. The ability
			// component ticks at 20 Hz while PhysFalling integrates gravity every frame, so a stuck
			// pawn creeps a little between corrections. Mace's suspend measured 37.9 uu of exactly
			// this drift over 1.25 s (spec v15 §6); 120 uu over 1.5 s is twice that, and the second
			// check below is the one that actually distinguishes "held" from "fell" — the red arm has
			// to have moved at least three times as far.
			State->List.Check(FMath::Abs(State->DropGreen) < 120.f,
				TEXT("GREEN: and he stays put while he holds on"),
				FString::Printf(TEXT("moved %.0f uu in ~1.5 s; the 120 uu tolerance is the 20 Hz ability tick "
					"against per-frame gravity — Mace's suspend measures 37.9 uu of the same drift over 1.25 s"),
					State->DropGreen));
			State->List.Check(FMath::Abs(State->DropGreen) * 3.f < State->DropRed,
				TEXT("GREEN vs RED: holding on and falling are not the same outcome"),
				FString::Printf(TEXT("held %.0f uu of drift against %.0f uu of fall on the identical fixture"),
					State->DropGreen, State->DropRed));
			State->List.Check(!Slime->IsStuck(),
				TEXT("GREEN: releasing V lets go immediately"),
				TEXT("§2: 'sticks to walls WHILE HELD'"));

			State->List.Report();
			RestoreAllArms();
			return false;
		}), 0.25f);
	}

	// =============================================================================================
	// Trace.Slimeball.WallJumpTest — DEMO 17 item 2, two arms, RED first
	//
	// Verbatim: "Slimeball should be able to cancel the wall stick with jump, and perform a wall jump
	// off the wall." Two claims, and they fail differently, so both are measured:
	//
	//   CANCEL   after the press he is no longer stuck. The RED arm
	//            (Trace.Slimeball.StickJumpCancel 0) must show him still stuck, i.e. must reproduce the
	//            commitment Demo 17 asked us to remove.
	//   WALL JUMP  he leaves ALONG THE SURFACE NORMAL and upwards, rather than simply dropping.
	//
	// AND THE THIRD THING, WHICH IS THE ONE THAT COULD HURT SOMEBODY ELSE: half a second later he must
	// still be off the wall. V is a HOLD, so a player who wall-jumps is by definition still holding it,
	// and the 20 Hz re-stick sweep would otherwise glue him straight back on — which is wall-jump
	// stickiness, the exact complaint spec v10 §5 removed for the whole roster. Setting
	// SlimeballWallJumpRestickLockoutSeconds to 0 is what makes that assertion go red.
	//
	// The press is the REAL JUMP KEY, injected through the real input pipeline, so it exercises
	// ATracePlayerController::OnJumpStarted -> UTraceAbilityComponent::HandleJumpPressed -> the hook.
	// =============================================================================================

	struct FWallJumpState
	{
		int32 Arm = 0;              // 0 = RED (Trace.Slimeball.StickJumpCancel 0), 1 = GREEN (shipped)
		int32 Step = 0;
		double Deadline = 0.0;
		double StepStartReal = 0.0;

		FVector StickLocation = FVector::ZeroVector;
		FVector StickNormal = FVector::ZeroVector;

		bool bWasStuckBeforeJump = false;
		bool bStuckAfterJumpRed = false;
		bool bStuckAfterJumpGreen = false;

		/** Velocity along the wall normal, and Z, one beat after the press. Measured in BOTH arms. */
		float OutwardAfterJump = 0.f;
		float UpwardAfterJump = 0.f;
		float RedOutwardAfterJump = 0.f;
		float RedUpwardAfterJump = 0.f;

		bool bStuckAgainHalfASecondLater = false;

		FChecklist List;
	};

	void RunWallJumpTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[SLIMEWJ] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FWallJumpState> State = MakeShared<FWallJumpState>();
		State->List.Tag = TEXT("SLIMEWJ");
		State->Deadline = FPlatformTime::Seconds() + 120.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[SLIMEWJ] ===== DEMO 17 item 2: 'cancel the wall stick with jump, and perform a wall jump off "
			     "the wall.' Arm 0 is RED (Trace.Slimeball.StickJumpCancel 0) and MUST leave him stuck. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				RestoreAllArms();
				return false;
			}

			const double NowReal = FPlatformTime::Seconds();

			FString Why;
			UTraceAbilitySetSlimeball* Slime = MakePlayerIntoSlimeball(TickWorld, Why);
			ATraceCharacter* MyPawn = (Slime != nullptr) ? Slime->GetCharacter() : nullptr;
			UTraceCharacterMovementComponent* MoveComp = (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;

			if (Slime == nullptr || MyPawn == nullptr || MoveComp == nullptr)
			{
				if (NowReal > State->Deadline)
				{
					State->List.Invalidate(FString::Printf(TEXT("could not stage: %s"), *Why));
					State->List.Report();
					RestoreAllArms();
					return false;
				}
				return true;
			}

			// ---- Step 0: put him on a wall and hold V --------------------------------------
			if (State->Step == 0)
			{
				SetCVar(TEXT("Trace.Slimeball.StickJumpCancel"), State->Arm == 0 ? 0 : 1);

				FVector Stand = FVector::ZeroVector;
				FString StandWhy;
				if (!FindWallStandpoint(TickWorld, MyPawn, Stand, StandWhy))
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(FString::Printf(TEXT("no wall to test against: %s"), *StandWhy));
						State->List.Report();
						RestoreAllArms();
						return false;
					}
					return true;
				}

				MyPawn->TeleportTo(Stand, MyPawn->GetActorRotation(), /*bIsATest*/ false, /*bNoCheck*/ true);
				MoveComp->SetMovementMode(MOVE_Falling);
				MoveComp->Velocity = FVector::ZeroVector;

				Slime->OnSecondaryPressed();
				State->Step = 1;
				State->StepStartReal = NowReal;
				return true;
			}

			// ---- Step 1: wait for the stick to take ----------------------------------------
			if (State->Step == 1)
			{
				if (!Slime->IsStuck())
				{
					if ((NowReal - State->StepStartReal) > 3.0)
					{
						State->List.Invalidate(TEXT("he never stuck to the wall at all, so there was nothing for "
						                            "the jump to cancel. That is Trace.Slimeball.StickTest's "
						                            "business, not this one's"));
						State->List.Report();
						RestoreAllArms();
						return false;
					}
					return true;
				}

				State->bWasStuckBeforeJump = true;
				State->StickLocation = MyPawn->GetActorLocation();

				// The wall he is on, re-asked through the ability's own probe so the "outward" direction
				// this test judges by is the same one the launch used.
				FVector ProbePoint = FVector::ZeroVector;
				FVector ProbeNormal = FVector::ZeroVector;
				float ProbeDistance = 0.f;
				FString ProbeWhy;
				if (Slime->ProbeForWall(ProbePoint, ProbeNormal, ProbeDistance, ProbeWhy))
				{
					State->StickNormal = ProbeNormal.GetSafeNormal();
				}

				// THE REAL JUMP KEY, not the hook. Everything between the key and the ability — the bind,
				// the controller's first-refusal call into HandleJumpPressed, the "a true return means the
				// normal jump must NOT also run" rule — is only exercised this way.
				if (APlayerController* LocalPC = TickWorld->GetFirstPlayerController())
				{
					LocalPC->ConsoleCommand(TEXT("Trace.SimInput SpaceBar 0.05"), /*bWriteToLog=*/false);
				}

				State->Step = 2;
				State->StepStartReal = NowReal;
				return true;
			}

			// ---- Step 2: one beat after the press, read the launch -------------------------
			if (State->Step == 2)
			{
				if ((NowReal - State->StepStartReal) < 0.20)
				{
					return true;
				}

				const bool bStuckNow = Slime->IsStuck();

				// MEASURED IN BOTH ARMS, and that is the correction this harness needed. Its first
				// version asserted only "the red arm leaves him stuck", which turned out not to be the
				// whole of the old behaviour — a press refused by this hook still reaches
				// ACharacter::Jump, and the movement layer's v10 §5 input BUFFER can spend it on a wall
				// contact a frame or two later. So the honest red/green difference is not "does he come
				// off the wall" but "is the press a WALL JUMP, reliably, in the frame it was made".
				const FVector Velocity = MoveComp->Velocity;
				const float Outward = State->StickNormal.IsNearlyZero()
					? 0.f
					: static_cast<float>(FVector::DotProduct(FVector(Velocity.X, Velocity.Y, 0.f), State->StickNormal));

				if (State->Arm == 0)
				{
					State->bStuckAfterJumpRed = bStuckNow;
					State->RedOutwardAfterJump = Outward;
					State->RedUpwardAfterJump = static_cast<float>(Velocity.Z);
				}
				else
				{
					State->bStuckAfterJumpGreen = bStuckNow;
					State->OutwardAfterJump = Outward;
					State->UpwardAfterJump = static_cast<float>(Velocity.Z);
				}

				State->Step = 3;
				State->StepStartReal = NowReal;
				return true;
			}

			// ---- Step 3: half a second later — is he glued back on? ------------------------
			if (State->Step == 3)
			{
				if ((NowReal - State->StepStartReal) < 0.5)
				{
					return true;
				}

				if (State->Arm == 1)
				{
					State->bStuckAgainHalfASecondLater = Slime->IsStuck();
				}

				// V is released between arms so the second arm starts from the same clean state the
				// first did — a held key surviving into the next arm would re-stick him during staging.
				Slime->OnSecondaryReleased();

				if (State->Arm == 0)
				{
					State->Arm = 1;
					State->Step = 0;
					State->Deadline = NowReal + 120.0;
					return true;
				}

				// ---- verdict ----------------------------------------------------------------
				const UTraceSettings& Knobs = UTraceSettings::Get();

				// THE RED ARM IS JUDGED FIRST. With nothing falsified, a clean green says only that the
				// code did not crash — this project has shipped that mistake twice.
				const float LaunchFloor = Knobs.WallJumpOutwardImpulse * 0.5f;
				const bool bRedLaunched = (State->RedOutwardAfterJump > LaunchFloor)
					&& (State->RedUpwardAfterJump > 0.f);
				if (bRedLaunched)
				{
					State->List.Invalidate(FString::Printf(
						TEXT("the RED arm ALSO launched him (%.0f uu/s out, %.0f up), so the disarmed build and "
						     "the shipped one are indistinguishable here and the green arm proves nothing"),
						State->RedOutwardAfterJump, State->RedUpwardAfterJump));
				}

				State->List.Check(State->bWasStuckBeforeJump && !bRedLaunched,
					TEXT("RED (Trace.Slimeball.StickJumpCancel 0): the press is NOT a wall jump"),
					FString::Printf(TEXT("still stuck=%d, %.0f uu/s outward and %.0f up — a press that either does "
					                     "nothing or drops him, which is the commitment Demo 17 reported"),
						State->bStuckAfterJumpRed ? 1 : 0, State->RedOutwardAfterJump, State->RedUpwardAfterJump));

				State->List.Check(!State->bStuckAfterJumpGreen,
					TEXT("GREEN: JUMP CANCELS THE WALL STICK"),
					FString::Printf(TEXT("stuck after the press = %d"), State->bStuckAfterJumpGreen ? 1 : 0));

				State->List.Check(State->OutwardAfterJump > (Knobs.WallJumpOutwardImpulse * 0.5f),
					TEXT("GREEN: ...and it is a WALL JUMP, not a drop — he leaves ALONG THE WALL NORMAL"),
					FString::Printf(TEXT("%.0f uu/s outward against the shipped %.0f impulse"),
						State->OutwardAfterJump, Knobs.WallJumpOutwardImpulse));

				State->List.Check(State->UpwardAfterJump > 0.f,
					TEXT("GREEN: ...and upwards"),
					FString::Printf(TEXT("%.0f uu/s of Z, i.e. the jump x %.2f"),
						State->UpwardAfterJump, Knobs.WallJumpVerticalMultiplier));

				State->List.Check(!State->bStuckAgainHalfASecondLater,
					TEXT("GREEN: he is STILL OFF the wall half a second later, with V still held"),
					FString::Printf(TEXT("re-grab lockout %.2fs — 0 here is what resurrects wall-jump "
					                     "stickiness for one character"),
						Knobs.SlimeballWallJumpRestickLockoutSeconds));

				State->List.Report();
				RestoreAllArms();
				return false;
			}

			return true;
		}), 0.05f);
	}

	// =============================================================================================
	// Registration
	// =============================================================================================

	FAutoConsoleCommand CmdSlimeballWallJumpTest(
		TEXT("Trace.Slimeball.WallJumpTest"),
		TEXT("DEMO 17 item 2, two arms RED first: stuck to a real wall, the REAL jump key must cancel the "
		     "stick AND launch him off the face, and he must still be off it half a second later with V "
		     "still held. Drives the local pawn — do not batch it with another test that drives pawns."),
		FConsoleCommandDelegate::CreateStatic(&RunWallJumpTest));

	FAutoConsoleCommand CmdSlimeballDump(
		TEXT("Trace.Slimeball.Dump"),
		TEXT("Spec v18 §2: what the RUNNING process resolved for Slimeball — the stick, both stuck passives and "
		     "every Slimewall dimension. The .ini wins over the header, so this is the honest read."),
		FConsoleCommandDelegate::CreateStatic(&RunDump));

	FAutoConsoleCommand CmdSlimeballVerify(
		TEXT("Trace.Slimeball.Verify"),
		TEXT("Spec v18 §2 + §4, four arms, each RED first: the two stuck passives, the 35% slow including a Core "
		     "carrier's exemption, the Slimewall thrown through the real E press, and 'can be shot through' "
		     "proved with the real hitscan resolver."),
		FConsoleCommandDelegate::CreateStatic(&RunVerify));

	FAutoConsoleCommand CmdSlimeballStickTest(
		TEXT("Trace.Slimeball.StickTest"),
		TEXT("Spec v18 §2: hold V beside a real wall and stick to it. Drives the local pawn — do not batch it "
		     "with another test that also drives pawns."),
		FConsoleCommandDelegate::CreateStatic(&RunStickTest));
}   // namespace TraceSlimeballVerify

#endif // !UE_BUILD_SHIPPING
