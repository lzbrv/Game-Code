// Trace — SLIMEBALL's Slimewall. See the header for the spec v18 §2 reading, for why the slab has no
// collision at all, and for why the choke point is re-asked every tick.

#include "Abilities/Characters/TraceSlimewall.h"

#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                      // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"                 // TraceAbilityFlags::MovementActive
#include "Abilities/TraceAbilityWorldSubsystem.h"        // GatherAllComponents — the stick poll's roll call
#include "Abilities/Characters/TraceAbilitySetElle.h"    // IsCloakVisualApplied — §1.2's cloak rule
#include "Audio/TraceAudio.h"
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterRoster.h"                   // THE accent. See SlimeAccent() below.
#include "Gameplay/TraceFxBurst.h"                       // ATraceFxBurst + TraceFxLoopBudget
#include "Gameplay/TraceFxShapes.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

#if !UE_BUILD_SHIPPING
// Harness only — Trace.Slimeball.StickGoo and Trace.Slimeball.FxParade, at the foot of this file.
#include "Camera/CameraActor.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"                 // FScreenshotRequest
#endif

// =================================================================================================
// THE RED ARMS. Named after the file so the unity build cannot collide them (see
// Scripts/check-jumbo-build-collisions.py).
// =================================================================================================

/**
 * *** THE SHOT-THROUGH ARM. This one is INVERTED and that is deliberate. ***
 *
 * Every other arm on this project ships at 1 and is switched OFF to reproduce a failure. This one
 * ships at 0 and is switched ON, because the failure it reproduces is a wall that BLOCKS bullets —
 * i.e. the bug §2 forbids — and there is no way to express "remove the absence of collision".
 *
 * With it at 1 the slab gets BlockAll, which truncates UTraceLagCompensationComponent::ResolveHitscan's
 * ECC_Visibility world trace short of anybody standing behind it. That is exactly the silent failure
 * the header describes, and Trace.Slimeball.Verify uses it to prove its shot-through check can fail.
 */
static TAutoConsoleVariable<int32> CVarSlimewallBlocksBullets(
	TEXT("Trace.Slimeball.WallBlocksBullets"),
	0,
	TEXT("TEST ARM ONLY, and INVERTED. 0 (shipped): the Slimewall has no collision and cannot stop a "
	     "bullet, a body or a trace — spec v18 §2's 'can be shot through'. 1: the slab is given "
	     "BlockAll, which reproduces the bug where the wall silently eats hitscan. Never ship 1."),
	ECVF_Cheat);

/**
 * *** PATCH 28 ITEM 2's A/B ARM. ***
 *
 * 1 (shipped): ResolveForwardRun lays the wall ALONG his aim — a 176 x 176 uu cross-section running
 * SlimewallLengthUU away from him, starting SlimewallRangeUU in front.
 * 0: the PRE-PATCH placement, byte for byte — the slab CENTRED SlimewallRangeUU ahead with its
 * SlimewallLengthUU span ACROSS his aim, retreating rather than shortening when the way is blocked.
 *
 * It exists because a placement change cannot be photographed as a change from one binary otherwise,
 * and because "the wall is lateral again" has to be reachable in one console line if the owner
 * decides they preferred it. Trace.Slimeball.FxParade photographs both arms.
 */
static TAutoConsoleVariable<int32> CVarSlimewallForward(
	TEXT("Trace.Slimeball.WallForward"),
	1,
	TEXT("PATCH 28 §2. 1 (shipped): the Slimewall runs FORWARD, away from him, from the placement "
	     "knob. 0: the pre-Patch-28 LATERAL wall, centred on the placement knob and spanning across "
	     "his aim. The A/B arm for the placement change."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarSlimewallSlow(
	TEXT("Trace.Slimeball.Slow"),
	1,
	TEXT("Dev/red arm. 1 (default) = walking through a Slimewall slows enemies (spec v18 §2: 35%). "
	     "0 = the wall still goes up, still replicates and still blocks sight, but slows nobody, so "
	     "every slow assertion must go red."),
	ECVF_Cheat);

namespace TraceSlimewallTuning
{
	/** /Engine/BasicShapes primitives are 100 uu across, so a scale of 1 is 100 uu on that axis. */
	constexpr float BasicShapeSizeUU = 100.f;

	/**
	 * Emissive strength for M_TraceNeon. Well below the arena's 3.5 on purpose: the slab is a large
	 * flat surface right in front of a player's face, and a bloom-clearing wall of it is a flashbang.
	 */
	constexpr float SlabGlow = 0.9f;

	/**
	 * THE SLAB's own green. Not team-coloured — both teams read the same object, and it is a hazard,
	 * not a marker. Deliberately NOT the accent, and deliberately NOT derived from it either.
	 *
	 * This is a 1100 x 176 uu opaque surface at arm's length, the single biggest piece of coloured
	 * geometry any ability puts on screen, and the accent at that area reads as a wall of light. So
	 * the face is a darker, duller sample and the accent is spent where §2.10 asks for it — on the
	 * LIP and the SEAMS, which are thin. ART_BIBLE §3.2: lips and trim carry the read, faces carry
	 * the mass.
	 *
	 * *** WHAT SEPARATES THE TRIM FROM THE FACE IS GLOW, NOT HUE, AND THAT IS NOW LOAD-BEARING. ***
	 * It used to be both: the pre-W6 accent was #D4F66F (sRGB hue 75.1) against this face's 101.1,
	 * a 26 deg gap. The re-space moved Slimeball to #9BF66F (hue 100.4), so face and trim are now
	 * within 0.7 deg of each other and the whole read rests on the brightness ratio — lip
	 * 0.33/0.92/0.16 x 2.2 against face 0.32/0.78/0.18 x 0.9, i.e. the lip is 2.9x the face on its
	 * brightest channel and the seams 2.1x. That ratio is unchanged by the re-space and it is the
	 * mechanism §3.2 names, so the wall still reads; but a future re-tune that flattens the GLOWS
	 * would now cost the dressing its separation entirely, where before there was a hue to fall back
	 * on. Left as authored rather than re-darkened because these numbers were solved against a
	 * capture and re-deriving them from the accent is a change to how the wall LOOKS, which is not
	 * what a colour-sync pass is for.
	 */
	const FLinearColor SlimeColor(0.32f, 0.78f, 0.18f, 1.f);

	/**
	 * Slimeball's accent, for the thin pieces — the lip, the seams and the stick goo.
	 *
	 * *** READ FROM THE ROSTER, NOT COPIED. *** This was
	 * `const FLinearColor SlimeAccent(0.66f, 0.92f, 0.16f, 1.f)`, a transcription of ART_BIBLE §2.3
	 * row 8. The W5 re-space moved him to (0.33, 0.92, 0.16) and this copy did not move, so his wall
	 * and his goo wore last wave's slime while his body wore this wave's. The literal is gone rather
	 * than corrected, because a corrected literal is the same bug waiting for the next re-tune.
	 */
	FLinearColor SlimeAccent()
	{
		if (const TraceCharacterRoster::FTraceCharacterEntry* Row =
			TraceCharacterRoster::Find(static_cast<uint8>(ETraceCharacterId::Slimeball)))
		{
			return FLinearColor(Row->Accent.R, Row->Accent.G, Row->Accent.B, 1.f);
		}
		return FLinearColor::White;
	}

	// =============================================================================================
	// FX_AUDIO_PLAN §2.10 — THE DRESSING. Every number is local to the slab; see the header on why.
	// =============================================================================================

	/** §2.10: the lip's Glow. T1 "structure read" — a band you range-find the wall's top edge against. */
	constexpr float LipGlow = 2.2f;

	/** §2.10: the seams' Glow. T0 face trim, and below the lip, which is ART_BIBLE §3.2's ratio. */
	constexpr float SeamGlow = 1.6f;

	/**
	 * §2.10 asks for a lip 6 uu tall. It is drawn at 8.
	 *
	 * ART_BIBLE §3.4: a world-readable emissive under 8 uu across dissolves under TSR, and the lip's
	 * HEIGHT is the dimension carrying its read — a player's eye sits below the top of a 176 uu wall,
	 * so the band is seen edge-on. Same floor, same reason and the same precedent as ATraceFxBurst's
	 * MinEmissiveRadiusUU and Oyster's jar collar.
	 */
	constexpr float LipHeightUU = 8.f;

	/** §2.10: "full width x 10 uu x 6 uu" — the 10 uu is how far the lip stands proud of the faces. */
	constexpr float LipProudUU = 10.f;

	/**
	 * How far the lip's top sits ABOVE the slab's, in uu.
	 *
	 * Not decoration for its own sake: at 0 the lip's top face and the slab's are coplanar over the
	 * whole footprint and z-fight into a shimmering mess at range. 2 uu on a 176 uu wall is 1.1% and it
	 * is the only place the dressing exceeds the slab's own silhouette — the lip is a lip, not a decal.
	 */
	constexpr float LipRiseUU = 2.f;

	/** §2.10: "3 vertical seam strips (8 uu wide)". Eight is also ART_BIBLE §3.4's floor exactly. */
	constexpr int32 NumSeams = 3;
	constexpr float SeamWidthUU = 8.f;

	/** How far each seam stands proud of EACH face, so the wall is dressed from both sides. */
	constexpr float SeamProudUU = 2.f;

	/** §2.10: "slab scales Z 0->full over 0.18 s (rise)". */
	constexpr float SpawnRiseSeconds = 0.18f;

	/** §2.10: "Expiry: 0.3 s Glow fade (ART_BIBLE §6.4: never pop-out)". */
	constexpr float ExpiryFadeSeconds = 0.3f;

	/** Below this the rise has produced nothing worth drawing; the pieces are hidden for a frame or two. */
	constexpr float MinDrawnHeightUU = 1.f;
}

// =================================================================================================
// The alarm
// =================================================================================================

namespace TraceSlimewall
{
	FSlowTally& CarrierTally()
	{
		// Function-local statics: one set per process, alive before any world exists, never torn down
		// between PIE sessions. Same reasoning as TraceAbilityIntegration::Counters().
		static FSlowTally Instance;
		return Instance;
	}

	FSlowTally& OtherTally()
	{
		static FSlowTally Instance;
		return Instance;
	}

	void ResetTallies()
	{
		CarrierTally().Reset();
		OtherTally().Reset();
	}

	void RecordEffect(const ATraceCharacter* Target, const TCHAR* VectorName, int32 FSlowTally::* Field)
	{
		const bool bCarrier = UTraceAbilityComponent::IsCarrier(Target);
		FSlowTally& Tally = bCarrier ? CarrierTally() : OtherTally();
		++(Tally.*Field);

		if (bCarrier)
		{
			// LOUD, and at Error. Spec §4's invariant has been broken if this ever prints on a shipped
			// build; a Verbose line would be indistinguishable from the noise a match already makes.
			UE_LOG(LogTraceGame, Error,
				TEXT("[Slimewall] *** SPEC v18 §4 VIOLATION: '%s' landed on the CORE CARRIER %s. ***"),
				VectorName, *GetNameSafe(Target));
		}
	}
}

// =================================================================================================
// UTraceSlimewallSlowComponent — the debuff on the victim
// =================================================================================================

UTraceSlimewallSlowComponent::UTraceSlimewallSlowComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	SetIsReplicatedByDefault(true);
}

void UTraceSlimewallSlowComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UTraceSlimewallSlowComponent, EndMatchTime);
	DOREPLIFETIME(UTraceSlimewallSlowComponent, bSlowActive);
}

ATraceCharacter* UTraceSlimewallSlowComponent::GetVictim() const
{
	return Cast<ATraceCharacter>(GetOwner());
}

float UTraceSlimewallSlowComponent::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

UTraceSlimewallSlowComponent* UTraceSlimewallSlowComponent::Find(const ATraceCharacter* Target)
{
	return (Target != nullptr) ? Target->FindComponentByClass<UTraceSlimewallSlowComponent>() : nullptr;
}

UTraceSlimewallSlowComponent* UTraceSlimewallSlowComponent::ApplyTo(ATraceCharacter* Target, APlayerState* InSource,
                                                                    float LingerSeconds)
{
	if (Target == nullptr || !Target->HasAuthority() || !Target->IsAlive())
	{
		return nullptr;
	}

	UTraceSlimewallSlowComponent* Existing = Find(Target);
	if (Existing == nullptr)
	{
		Existing = NewObject<UTraceSlimewallSlowComponent>(Target);
		if (Existing == nullptr)
		{
			return nullptr;
		}
		Existing->RegisterComponent();
	}

	Existing->SourcePlayerState = InSource;

	// ASSIGNMENT, NOT ADDITION. Standing in the slab refreshes the linger rather than stacking it —
	// otherwise a player who walked the long way through a 1100 uu wall would come out the far side
	// slowed for the rest of the round.
	Existing->EndMatchTime = Existing->MatchTimeNow() + FMath::Max(0.f, LingerSeconds);

	// Decided immediately rather than on the next tick: the wall has already asked the choke point to
	// get here, and leaving bSlowActive false for one tick would make the very first frame of the slow
	// a frame in which nothing happened — the difference between "it slows you" and "it slows you,
	// eventually".
	Existing->ServerRefreshNow();

	// ServerRefreshNow destroys the component on an expired linger. That cannot happen on the line
	// above (EndMatchTime was just pushed into the future), but returning a pointer without checking
	// would make this function's contract depend on that never changing.
	return IsValid(Existing) ? Existing : nullptr;
}

float UTraceSlimewallSlowComponent::GetSpeedMultiplier() const
{
	if (!bSlowActive)
	{
		return 1.f;
	}

	// Clamped at 0.95 for the same reason Oyster's is: a knob of 1.0 would be a total stop, which is
	// not a slow — it is a stun, and §2 does not ask for one.
	const float Fraction = FMath::Clamp(UTraceSettings::Get().SlimewallSlowFraction, 0.f, 0.95f);
	return 1.f - Fraction;
}

void UTraceSlimewallSlowComponent::ServerRefreshNow()
{
	ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr || !Victim->HasAuthority())
	{
		return;
	}

	// STRICTLY GREATER, AND THE ONE CHARACTER MATTERS — AT A LINGER OF 0.
	//
	// SlimewallSlowLingerSeconds is clamped at 0, and 0 is a reasonable thing for a designer to ask
	// for: "slow them while they are standing in it, and not a moment longer". With `>=` that reading
	// was silently inverted. ApplyTo pushes EndMatchTime to exactly now and then calls this function
	// on the next statement; `now >= now` called the slow expired before it had ever been in force,
	// the component destroyed itself, ApplyTo returned null, and the wall slowed NOBODY — not even
	// the player standing inside it — with no warning and no log line. That reads in a playtest as
	// "the Slimewall does nothing", which is the hardest kind of bug to report.
	//
	// With `>` the slow is in force for the frame it is applied, and the wall re-applies it every
	// tick a body is inside, so a linger of 0 means exactly what it says. Nothing moves at the
	// shipped 0.75 s: the two forms differ only on the single frame where now == EndMatchTime.
	const bool bExpired = (MatchTimeNow() > EndMatchTime);

	// *** THE CHOKE POINT, RE-ASKED. See the header: this is the "picked up the Core mid-slow" case,
	// and it is the whole reason this is a ticking component rather than a one-shot velocity write. ***
	const bool bAllowed =
		!bExpired
		&& Victim->IsAlive()
		&& CVarSlimewallSlow.GetValueOnAnyThread() != 0
		&& UTraceAbilityComponent::CanAffect(SourcePlayerState.Get(), Victim, ETraceAbilityEffect::Control);

	if (bAllowed != bSlowActive)
	{
		bSlowActive = bAllowed;
	}

	if (bSlowActive)
	{
		TraceSlimewall::RecordEffect(Victim, TEXT("35% slow (in force this tick)"),
			&TraceSlimewall::FSlowTally::SlowFrames);
	}

	if (bExpired)
	{
		// The slow is a property of a body and it is over. Destroying the component rather than
		// leaving an inert one behind keeps TraceAbilityDebuff's FindComponentByClass honest.
		DestroyComponent();
	}
}

void UTraceSlimewallSlowComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                 FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr)
	{
		return;
	}

	if (Victim->HasAuthority())
	{
		ServerRefreshNow();
		if (!IsValid(this))
		{
			return;   // ServerRefreshNow destroyed us on expiry
		}
	}

	ApplySlowClamp();

	// EVERY MACHINE, and after the authority half so a slow that just lapsed does not spend a frame
	// telling players it is still on. See the header block on UpdateSlowTellFx.
	SlowTellElapsed += DeltaTime;
	UpdateSlowTellFx();
}

// =================================================================================================
// FX_AUDIO_PLAN §2.10 — THE VICTIM SLOW TELL
// =================================================================================================

namespace TraceSlimewallTell
{
	/** ART_BIBLE §2.5 semantic wheel, `Slowed` = linear (0.35, 0.55, 1.00) = #A0C4FF. */
	const FLinearColor SlowedHue(0.35f, 0.55f, 1.00f, 1.f);

	/** §2.10: "feet shimmer ring r 40 uu". Inside the 96 uu footprint §1.4 allows, with room to spare. */
	constexpr float RingRadiusUU = 40.f;

	/** The ring's own thickness. A flat disc, not a sphere: it is a pool at the feet. */
	constexpr float RingHeightUU = 8.f;

	/** §2.10: "additive ... I 0.3". The PEAK of the shimmer, so the effect never exceeds what §2.10 asks. */
	constexpr float PeakIntensity = 0.3f;

	/**
	 * The shimmer: +-20% of the peak at 2 Hz.
	 *
	 * PERMITTED, and the distinction is the whole of ART_BIBLE §3.3's prohibition. A brightness
	 * oscillation is forbidden on a LETHAL TELEGRAPH — a kill volume that blinks is lying about when it
	 * kills. This is a STATUS READOUT on a victim: it says "you are slowed", it kills nobody, and
	 * FX_AUDIO_PLAN §1.4 permits motion and this kind of modulation on a while-active loop explicitly.
	 * The wall itself, which IS the volume, holds a constant Glow — see BuildSlabIfNeeded.
	 */
	constexpr float ShimmerHz = 2.f;
	constexpr float ShimmerDepth = 0.2f;
}

bool UTraceSlimewallSlowComponent::IsSlowTellDrawn() const
{
	return SlowTellRing != nullptr && IsValid(SlowTellRing) && SlowTellRing->IsVisible();
}

void UTraceSlimewallSlowComponent::DetachSlowTellFx()
{
	if (SlowTellRing == nullptr)
	{
		return;
	}

	// Through the budget helper, which owns the per-pawn registration as well as the component.
	// FX_AUDIO_PLAN §8.9: "no FX component survives its pawn".
	TraceFxLoopBudget::DetachLoopPrimitive(Cast<APawn>(GetOwner()), SlowTellRing);
	SlowTellRing = nullptr;
	SlowTellMID = nullptr;
}

void UTraceSlimewallSlowComponent::UpdateSlowTellFx()
{
	// A dedicated server cooks no shaders and has nobody to show them to. Every rule above still ran.
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ATraceCharacter* Victim = GetVictim();

	// bSlowActive is the whole condition, and it is replicated: the tell is on exactly on the ticks the
	// slow is in force, which means a victim who picks up the Core loses the ring in the same frame the
	// §4 choke point stops slowing them. That is the tell being HONEST rather than merely present.
	if (Victim == nullptr || !Victim->IsAlive() || !bSlowActive)
	{
		DetachSlowTellFx();
		return;
	}

	// Elle's cloak — FX_AUDIO_PLAN §1.2's last rule. A cloaked Elle who crosses a slime wall must not
	// be outlined by a glowing blue pool she did not ask for and cannot switch off. Hidden rather than
	// detached, so decloaking costs no rebuild and no budget churn. (The identical treatment, for the
	// identical reason, as Oyster's poison drips.)
	bool bHiddenByCloak = false;
	if (const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Victim))
	{
		if (const UTraceAbilitySetElle* Elle = Comp->GetAbilitySetAs<UTraceAbilitySetElle>())
		{
			bHiddenByCloak = Elle->IsCloakVisualApplied();
		}
	}

	if (SlowTellRing == nullptr)
	{
		UStaticMesh* Cylinder = UTraceFxShapes::GetCylinder();
		if (Cylinder == nullptr)
		{
			return;
		}

		float FeetZ = 0.f;
		if (const UCapsuleComponent* Capsule = Victim->GetCapsuleComponent())
		{
			FeetZ = -Capsule->GetScaledCapsuleHalfHeight() + 0.5f * TraceSlimewallTell::RingHeightUU;
		}

		// THE §1.4 CHOKE POINT. It clamps the intensity to 0.5, keeps the piece inside the capsule
		// footprint and REFUSES a fifth primitive — which is the live case here, because a victim can
		// be poisoned (three drips) and slimed at the same time and this ring is the fourth.
		SlowTellRing = TraceFxLoopBudget::AttachLoopPrimitive(
			Victim, Victim->GetRootComponent(), Cylinder, TEXT("SlimeSlowRing"),
			TraceSlimewallTell::SlowedHue, TraceSlimewallTell::PeakIntensity,
			FVector(0.f, 0.f, FeetZ), TraceSlimewallTell::RingRadiusUU, SlowTellMID);

		if (SlowTellRing == nullptr)
		{
			return;   // refused, and the budget said so. Survivable: no ring this time.
		}

		// A DISC, NOT A BALL. AttachLoopPrimitive sizes every piece as a sphere would be sized, which
		// for a cylinder mesh is a 80 uu-tall column at the ankles; the ring is 8 uu thick, so the Z is
		// re-written here. /Engine/BasicShapes has no torus and a flattened cylinder is a filled disc
		// rather than an annulus — the same conclusion ATraceElleGate and ATraceFxBurst reached, and at
		// this size and intensity a filled pool of light at the feet is the better read anyway.
		SlowTellRing->SetRelativeScale3D(FVector(
			UTraceFxShapes::ShapeScaleForRadiusUU(TraceSlimewallTell::RingRadiusUU),
			UTraceFxShapes::ShapeScaleForRadiusUU(TraceSlimewallTell::RingRadiusUU),
			UTraceFxShapes::ShapeScaleForLengthUU(TraceSlimewallTell::RingHeightUU)));

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Slimewall] slow tell attached to %s: r %.0f uu, additive Slowed, peak I %.2f."),
			*GetNameSafe(Victim), TraceSlimewallTell::RingRadiusUU, TraceSlimewallTell::PeakIntensity);
	}

	SlowTellRing->SetVisibility(!bHiddenByCloak);
	if (bHiddenByCloak)
	{
		return;
	}

	const float Shimmer = 1.f - TraceSlimewallTell::ShimmerDepth
		* 0.5f * (1.f - FMath::Cos(2.f * PI * TraceSlimewallTell::ShimmerHz * SlowTellElapsed));

	UTraceFxShapes::SetGlow(SlowTellMID, ETraceFxBlend::Additive,
		TraceSlimewallTell::SlowedHue, TraceSlimewallTell::PeakIntensity * Shimmer);
}

void UTraceSlimewallSlowComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DetachSlowTellFx();
	Super::EndPlay(EndPlayReason);
}

void UTraceSlimewallSlowComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// BOTH, because they are different paths: ServerRefreshNow calls DestroyComponent() on expiry,
	// a client loses this component to replication, and a level teardown loses it to EndPlay.
	// DetachSlowTellFx is idempotent, so whichever arrives second does nothing.
	DetachSlowTellFx();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UTraceSlimewallSlowComponent::ApplySlowClamp()
{
	if (!bSlowActive)
	{
		return;
	}

	ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr || !Victim->IsAlive())
	{
		return;
	}

	// Only the machines that actually simulate this pawn. A simulated proxy's velocity is replicated;
	// clamping it there would fight the interpolation and would slow somebody else's view of a player
	// the server never slowed.
	if (!Victim->HasAuthority() && !Victim->IsLocallyControlled())
	{
		return;
	}

	UTraceCharacterMovementComponent* MoveComp = Victim->GetTraceMovement();
	if (MoveComp == nullptr)
	{
		return;
	}

	// [ASSUMPTION], and the same two halves Oyster's poison flags, for the same reasons:
	//
	// NOT DURING A DASH: a dash is velocity on rails and GetMaxSpeed() returns DashSpeed while it
	// runs, so clamping there would make the wall a dash nerf of a completely different magnitude
	// than 35%. The LINGER is what stops that being a hole — a player who dashes through the slab
	// comes out the far side already slimed and is slowed for SlimewallSlowLingerSeconds after.
	//
	// GROUND ONLY: the air ceilings here are the momentum model (soft cap, hard cap, falloff) and are
	// not expressed through GetMaxSpeed(); clamping planar air speed to 65% of the WALK speed would
	// not be a slow, it would delete air momentum entirely.
	if (MoveComp->IsFalling() || Victim->IsDashing())
	{
		return;
	}

	// *** THE FRACTION IS NOT RE-APPLIED HERE, AND THIS IS THE FAR SIDE OF THE MIGRATION THE OLD
	// COMMENT PROMISED. *** FX_AUDIO_PLAN §7.3 (F7) landed the missing aggregator line:
	// TraceAbilityDebuff::GetMoveSpeedMultiplier (Abilities/TraceAbilityTypes.cpp) now multiplies by
	// UTraceSlimewallSlowComponent::GetSpeedMultiplier(), and
	// UTraceCharacterMovementComponent::GetMaxSpeed() already multiplies by that aggregator — so
	// GetMaxSpeed() IS the slimed ceiling and ACCELERATION targets it rather than being clipped after
	// the fact.
	//
	// The `* GetSpeedMultiplier()` that used to be on this line was therefore deleted, and nothing
	// else changed, exactly as the instruction it carried said to do: multiplying a second time would
	// compound 0.65 into 0.42 — a -58% slow wearing a -35% label. This clamp's remaining job is only
	// to stop the acceleration model overshooting a ceiling it already knows about, which is word for
	// word what UTraceOysterPoisonComponent::ApplySlowClamp says on the far side of the identical
	// transition.
	const float Limit = FMath::Max(1.f, MoveComp->GetMaxSpeed());

	FVector Vel = MoveComp->Velocity;
	const FVector Planar(Vel.X, Vel.Y, 0.f);
	const float PlanarSpeed = Planar.Size();
	if (PlanarSpeed > Limit && PlanarSpeed > KINDA_SMALL_NUMBER)
	{
		const FVector Capped = Planar * (Limit / PlanarSpeed);
		Vel.X = Capped.X;
		Vel.Y = Capped.Y;
		MoveComp->Velocity = Vel;
	}
}

// =================================================================================================
// ATraceSlimewall — construction
// =================================================================================================

ATraceSlimewall::ATraceSlimewall()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	bReplicates = true;
	SetReplicateMovement(false);      // it never moves; replicating a static transform is pure cost
	bAlwaysRelevant = true;           // a 4 s sight blocker must not be culled off a client
	SetNetUpdateFrequency(10.f);

	WallRoot = CreateDefaultSubobject<USceneComponent>(TEXT("WallRoot"));
	SetRootComponent(WallRoot);

	Slab = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Slab"));
	Slab->SetupAttachment(WallRoot);

	// *** SPEC v18 §2: "can be shot through". NOTHING may collide with this. ***
	// Not "no blocking on the weapon channel" — none at all. See the file header for the two traces
	// this protects and for why a blocking slab would fail silently rather than loudly.
	Slab->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Slab->SetCollisionProfileName(TEXT("NoCollision"));
	Slab->SetGenerateOverlapEvents(false);
	Slab->SetCanEverAffectNavigation(false);
	Slab->SetCastShadow(false);        // a 1100 uu slab casting shadows would darken the whole approach
	Slab->bReceivesDecals = false;

	// FX_AUDIO_PLAN §2.10's dressing. Both are pure decoration and take the shared "this is FX" pass,
	// so neither can quietly acquire a collision profile — which matters more here than anywhere else
	// in the project, because HasAnyCollisionEnabled() below is the assertion that makes "can be shot
	// through" testable and it walks EVERY primitive on this actor.
	Lip = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Lip"));
	Lip->SetupAttachment(WallRoot);
	Lip->SetVisibility(false);
	UTraceFxShapes::ConfigureFxComponent(Lip);
	Lip->SetCanEverAffectNavigation(false);

	Seams = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Seams"));
	Seams->SetupAttachment(WallRoot);
	Seams->SetVisibility(false);
	UTraceFxShapes::ConfigureFxComponent(Seams);
	Seams->SetCanEverAffectNavigation(false);

	// /Engine/BasicShapes ships with every install; M_TraceNeon is the committed parent under
	// /Game/Trace/Materials/Parents (MAP_PLAN §9 — it used to be read from the gitignored, now deleted
	// /Game/Generated tree). Both lookups are static so the cost is paid once per process, exactly as
	// ATraceRippleActor does.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		SlabMesh = CubeFinder.Object;
		Lip->SetStaticMesh(CubeFinder.Object);
		Seams->SetStaticMesh(CubeFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Trace/Materials/Parents/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		NeonMaterial = NeonFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BasicFinder.Succeeded())
	{
		FallbackMaterial = BasicFinder.Object;
	}
}

void ATraceSlimewall::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceSlimewall, WallCenter);
	DOREPLIFETIME(ATraceSlimewall, WallAim);
	DOREPLIFETIME(ATraceSlimewall, HalfExtents);
	DOREPLIFETIME(ATraceSlimewall, ExpireMatchTime);
	DOREPLIFETIME(ATraceSlimewall, SpawnMatchTime);
	DOREPLIFETIME(ATraceSlimewall, SourcePlayerState);
	DOREPLIFETIME(ATraceSlimewall, SourceTeam);
}

// =================================================================================================
// PATCH 28 ITEM 2 — "the slimewall should be placed FORWARD instead of laterally in front of him"
// =================================================================================================

bool ATraceSlimewall::ResolveForwardRun(const UWorld* WorldPtr, const ATraceCharacter* Placer,
                                        const FVector& PlanarAim, float ThicknessUU, float ForwardSpanUU,
                                        float HeightUU, float StandoffUU,
                                        FVector& OutCenter, FVector& OutSlabNormal, FVector& OutHalfExtents,
                                        FString& OutWhy)
{
	OutCenter      = FVector::ZeroVector;
	OutSlabNormal  = FVector::ZeroVector;
	OutHalfExtents = FVector::ZeroVector;

	if (WorldPtr == nullptr || Placer == nullptr)
	{
		OutWhy = TEXT("no world or no placer");
		return false;
	}

	// FLATTENED. A wall is a vertical slab standing on the floor, so the pitch of his view decides
	// nothing about it — only the compass bearing does. Looking straight down would otherwise produce
	// a degenerate direction and a wall in an arbitrary orientation.
	const FVector Aim = FVector(PlanarAim.X, PlanarAim.Y, 0.f).GetSafeNormal();
	if (Aim.IsNearlyZero())
	{
		OutWhy = TEXT("no horizontal aim direction");
		return false;
	}

	const float Thickness = FMath::Max(10.f, ThicknessUU);
	const float Span      = FMath::Max(50.f, ForwardSpanUU);
	const float Height    = FMath::Max(20.f, HeightUU);

	float CapsuleRadius = 34.f;
	float CapsuleHalfHeight = 88.f;
	if (const UCapsuleComponent* Capsule = Placer->GetCapsuleComponent())
	{
		CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}

#if !UE_BUILD_SHIPPING
	// ---- THE A/B ARM: the pre-Patch-28 LATERAL placement, reproduced exactly -----------------------
	//
	// Not an approximation of it. The slab is CENTRED StandoffUU ahead with its long span ACROSS his
	// aim (so the aim itself is the slab normal), the clearance kept is HALF THE THICKNESS because the
	// centre is what is being placed, and a blocked lane pulls the wall BACK toward him instead of
	// shortening it. Every line below is the code this function replaced.
	if (CVarSlimewallForward.GetValueOnAnyThread() == 0)
	{
		float CapsuleRadiusLegacy = 34.f;
		float CapsuleHalfHeightLegacy = 88.f;
		if (const UCapsuleComponent* Capsule = Placer->GetCapsuleComponent())
		{
			CapsuleRadiusLegacy = Capsule->GetScaledCapsuleRadius();
			CapsuleHalfHeightLegacy = Capsule->GetScaledCapsuleHalfHeight();
		}

		FCollisionObjectQueryParams LegacyObjects;
		LegacyObjects.AddObjectTypesToQuery(ECC_WorldStatic);
		FCollisionQueryParams LegacyQuery(SCENE_QUERY_STAT(TraceSlimewallLegacyPlacement), /*bTraceComplex*/ false);
		LegacyQuery.AddIgnoredActor(Placer);

		const FVector OriginLegacy = Placer->GetActorLocation();
		const float DesiredRange = FMath::Max(0.f, StandoffUU);
		const float MinimumRange = CapsuleRadiusLegacy + (Thickness * 0.5f) + 10.f;

		float PlacementRange = DesiredRange;
		FHitResult LegacyForwardHit;
		if (WorldPtr->LineTraceSingleByObjectType(LegacyForwardHit, OriginLegacy,
			OriginLegacy + Aim * DesiredRange, LegacyObjects, LegacyQuery))
		{
			PlacementRange = static_cast<float>(LegacyForwardHit.Distance) - (Thickness * 0.5f);
		}

		if (PlacementRange < MinimumRange)
		{
			OutWhy = FString::Printf(
				TEXT("[LEGACY LATERAL ARM] no room — only %.0f uu of clearance ahead and the slab needs %.0f"),
				FMath::Max(0.f, PlacementRange), MinimumRange);
			return false;
		}

		const FVector FeetLegacy = OriginLegacy - FVector(0.f, 0.f, CapsuleHalfHeightLegacy);
		const FVector AheadLegacy = FeetLegacy + Aim * PlacementRange;

		float LegacyFloorZ = FeetLegacy.Z;
		FHitResult LegacyFloorHit;
		if (WorldPtr->LineTraceSingleByObjectType(LegacyFloorHit, AheadLegacy + FVector(0.f, 0.f, Height),
			AheadLegacy - FVector(0.f, 0.f, Height * 4.f), LegacyObjects, LegacyQuery))
		{
			LegacyFloorZ = static_cast<float>(LegacyFloorHit.ImpactPoint.Z);
		}

		OutCenter      = FVector(AheadLegacy.X, AheadLegacy.Y, LegacyFloorZ + Height * 0.5f);
		OutSlabNormal  = Aim;                                              // THE AIM ITSELF — lateral wall
		OutHalfExtents = FVector(Thickness * 0.5f, Span * 0.5f, Height * 0.5f);
		OutWhy = TEXT("placed [LEGACY LATERAL ARM: Trace.Slimeball.WallForward 0]");
		return true;
	}
#endif

	// HE MUST NEVER BE STANDING INSIDE HIS OWN WALL. The lateral wall got this for free from its
	// half-thickness clearance; a forward wall starts at a point on his aim line, so the floor under
	// the knob has to be his own capsule. Read live, so a capsule retune carries.
	const float MinimumStandoff = CapsuleRadius + 10.f;
	const float NearRange = FMath::Max(MinimumStandoff, StandoffUU);

	const FVector Origin = Placer->GetActorLocation();

	// BY OBJECT TYPE, not by channel, for the reason recorded on UTraceAbilitySetSlimeball::
	// ProbeForWall: a channel trace on ECC_WorldStatic is also blocked by every pawn capsule, so a
	// team-mate standing in the lane would shorten the wall to nothing.
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceSlimewallForwardRun), /*bTraceComplex*/ false);
	QueryParams.AddIgnoredActor(Placer);

	// ---- how far the run is allowed to reach ------------------------------------------------------
	//
	// Traced at his CAPSULE CENTRE rather than at his feet, which is where a pillar actually is.
	// SHORTENED, NEVER RETREATED — see the header for why a forward wall cannot back off the way the
	// lateral one did. The 10 uu is the same clearance the old placement kept, on the far end instead
	// of the near one.
	constexpr float FarClearanceUU = 10.f;

	float FarRange = NearRange + Span;
	FHitResult ForwardHit;
	if (WorldPtr->LineTraceSingleByObjectType(ForwardHit, Origin, Origin + Aim * FarRange,
		ObjectParams, QueryParams))
	{
		FarRange = static_cast<float>(ForwardHit.Distance) - FarClearanceUU;
	}

	const float AchievedSpan = FarRange - NearRange;
	if (AchievedSpan < Thickness)
	{
		// A FREE FIZZLE. A slab shorter than it is thick is a block, not a wall, and §2's ability is
		// the long run. UTraceCharacterAbilitySet's header names "Mace's spike hitting nothing" as the
		// precedent: standing nose-to-nose with a pillar must not cost 25 s.
		OutWhy = FString::Printf(
			TEXT("no room — only %.0f uu of clear run past the %.0f uu standoff, and the slab is %.0f uu thick"),
			FMath::Max(0.f, AchievedSpan), NearRange, Thickness);
		return false;
	}

	// ---- and then drop it onto the floor ----------------------------------------------------------
	//
	// Under the MIDDLE of the achieved run, measured from a whole wall-height above it so that placing
	// it up a short step finds the step rather than the floor underneath, and traced a long way down
	// so that a run that ends over a ledge still finds ground.
	const FVector Feet = Origin - FVector(0.f, 0.f, CapsuleHalfHeight);
	const FVector Middle = Feet + Aim * (NearRange + AchievedSpan * 0.5f);

	float FloorZ = Feet.Z;
	FHitResult FloorHit;
	if (WorldPtr->LineTraceSingleByObjectType(FloorHit, Middle + FVector(0.f, 0.f, Height),
		Middle - FVector(0.f, 0.f, Height * 4.f), ObjectParams, QueryParams))
	{
		FloorZ = static_cast<float>(FloorHit.ImpactPoint.Z);
	}

	OutCenter = FVector(Middle.X, Middle.Y, FloorZ + Height * 0.5f);

	// *** THE 90 DEGREES. *** The slab's local +X is its NORMAL (see the class header), so handing
	// ServerSpawn the LATERAL vector is what turns the long local +Y span from "across his aim" into
	// "along his aim". Up x Aim is well conditioned because Aim is planar by construction.
	OutSlabNormal = FVector::CrossProduct(FVector::UpVector, Aim).GetSafeNormal();

	// (thickness/2 along the normal, ACHIEVED span/2 along his aim, height/2 vertical).
	OutHalfExtents = FVector(Thickness * 0.5f, AchievedSpan * 0.5f, Height * 0.5f);

	OutWhy = (AchievedSpan < Span - 1.f)
		? FString::Printf(TEXT("placed, shortened to %.0f uu of %.0f by something in the lane"), AchievedSpan, Span)
		: TEXT("placed");
	return true;
}

ATraceSlimewall* ATraceSlimewall::ServerSpawn(UWorld* WorldPtr, APlayerState* InSource, ETraceTeam InSourceTeam,
                                              const FVector& InCenter, const FVector& InPlanarAim,
                                              const FVector& InHalfExtents, float InExpireMatchTime)
{
	if (WorldPtr == nullptr)
	{
		return nullptr;
	}

	const FVector Aim = FVector(InPlanarAim.X, InPlanarAim.Y, 0.f).GetSafeNormal();
	if (Aim.IsNearlyZero() || InHalfExtents.GetMin() <= 0.f)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Slimewall] refused a degenerate wall: aim=%s halfExtents=%s"),
			*InPlanarAim.ToCompactString(), *InHalfExtents.ToCompactString());
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ATraceSlimewall* Wall = WorldPtr->SpawnActor<ATraceSlimewall>(
		ATraceSlimewall::StaticClass(), InCenter, Aim.Rotation(), SpawnParams);
	if (Wall == nullptr)
	{
		return nullptr;
	}

	Wall->SourcePlayerState = InSource;
	Wall->SourceTeam        = InSourceTeam;
	Wall->WallCenter        = InCenter;
	Wall->WallAim           = Aim;
	Wall->HalfExtents       = InHalfExtents;
	Wall->ExpireMatchTime   = InExpireMatchTime;
	// FX_AUDIO_PLAN §2.10's rise needs a start as well as an end, and both are absolute match times.
	// See the field's own comment for why this is replicated rather than derived from the duration knob.
	Wall->SpawnMatchTime    = Wall->MatchTimeNow();

	Wall->BuildSlabIfNeeded();
	Wall->UpdateWallAnimation();

	// =============================================================================================
	// FX_AUDIO_PLAN §2.10 — the SPLAT and the CAST SOUND, both fired from the one server-side site
	// that knows a wall just went up
	// =============================================================================================
	//
	// §2.10: "Spawn | slab scales Z 0->full over 0.18 s (rise), + ATraceFxBurst(SlimeSplat) at BASE:
	// 6 slime spheres r 10 uu scatter + squash, 0.3 s | server spawn site" and "Audio | `SlimeballWall`
	// World (server, cast)".
	//
	// AT THE BASE, not at the centre: a wall is a thing that comes UP OUT OF THE FLOOR, and six blobs
	// thrown from 88 uu in the air would read as something being dropped. UpVector is the surface
	// normal the scatter sprays off — the floor.
	//
	// THIS IS THE CAST SITE, even though the cast decision lives in UTraceAbilitySetSlimeball: this
	// function is server-only, is the ONE way a wall is ever made, and runs after the placement sweep
	// has decided where the wall actually ended up — so the sound is at the wall rather than at the
	// aim ray it was requested along. Every refusal path in the kit returns before reaching here, so a
	// wall that was never placed is never heard.
	//
	// TraceAudio::PlayAt, i.e. the ordinary table-driven authority path: `SlimeballWall` is declared
	// World-side, so this multicasts once and every machine plays exactly one copy. Not
	// PlayReplicatedLocal — the wall replicates, but this function does not run on a client, so there
	// is no already-on-every-machine call site here to ride.
	ATraceFxBurst::Burst(WorldPtr, ETraceFxBurstType::SlimeSplat,
		InCenter - FVector(0.f, 0.f, InHalfExtents.Z), FVector::UpVector);

	TraceAudio::PlayAt(WorldPtr, TraceSoundEvents::SlimeballWall, InCenter);

	UE_LOG(LogTraceGame, Log,
		TEXT("[Slimewall] up at %s facing %s — %.0f wide (through) x %.0f long (across) x %.0f tall, "
		     "cast at match time %.2f, expires at %.2f. Dressing: %s."),
		*InCenter.ToCompactString(), *Aim.ToCompactString(),
		InHalfExtents.X * 2.f, InHalfExtents.Y * 2.f, InHalfExtents.Z * 2.f,
		Wall->SpawnMatchTime, InExpireMatchTime, *Wall->DescribeDressing());

	return Wall;
}

void ATraceSlimewall::BeginPlay()
{
	Super::BeginPlay();

	// On the server everything is already set by ServerSpawn. On a client the replicated properties
	// may not have landed yet, so the slab is built from Tick instead — BuildSlabIfNeeded is
	// idempotent and cheap until the extents arrive. Same shape as ATraceRippleActor's rings.
	BuildSlabIfNeeded();
}

float ATraceSlimewall::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceSlimewall::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	BuildSlabIfNeeded();

	// EVERY MACHINE, before the authority guard: the rise and the expiry fade are driven entirely off
	// the two replicated match times, so nothing about either animation goes on the wire. Same shape,
	// and the same reasoning, as ATraceOysterPoisonCloud::Tick.
	UpdateWallAnimation();

	if (!HasAuthority())
	{
		return;
	}

	// "Lasts 4 s." One deadline, one destroy, and the visuals leave with the actor rather than being
	// faded out by a second timer that could disagree. Tested BEFORE the shape guard below so a wall
	// that somehow reached the world malformed still expires.
	if (ExpireMatchTime > 0.f && MatchTimeNow() >= ExpireMatchTime)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Slimewall] expired after slowing %d player(s)."),
			LifetimeSlowed.Num());
		Destroy();
		return;
	}

	ServerUpdateSlows();
}

bool ATraceSlimewall::IsInsideWall(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr)
	{
		return false;
	}

	const FVector Extents = FVector(HalfExtents);
	if (Extents.GetMin() <= 0.f)
	{
		return false;   // on a client, until the replicated extents arrive
	}

	const FVector Forward = FVector(WallAim).GetSafeNormal();
	if (Forward.IsNearlyZero())
	{
		return false;
	}

	// Up x Forward is the slab's local +Y — the LONG axis. Forward is planar by construction, so this
	// is always well conditioned — no FindBestAxisVectors needed.
	//
	// PATCH 28 ITEM 2 NEEDED NO EDIT HERE, and that is the check on the claim in the class header:
	// this test is written entirely in the slab's OWN basis (Extents.X across the normal, Extents.Y
	// along the long axis, Extents.Z vertical), so turning the wall 90 degrees turns its slow field
	// with it. What changed is only which world direction WallAim points in.
	const FVector Across = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();

	// The capsule, not the actor origin: a pawn is 34 uu of radius and 88 uu of half height, and a
	// point test would let somebody skim the face of the slab untouched while visibly inside it.
	float CapsuleRadius = 34.f;
	float CapsuleHalfHeight = 88.f;
	if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
	{
		CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}

	const FVector Delta = Candidate->GetActorLocation() - FVector(WallCenter);

	return FMath::Abs(FVector::DotProduct(Delta, Forward)) <= Extents.X + CapsuleRadius
		&& FMath::Abs(FVector::DotProduct(Delta, Across))  <= Extents.Y + CapsuleRadius
		&& FMath::Abs(Delta.Z)                             <= Extents.Z + CapsuleHalfHeight;
}

void ATraceSlimewall::ServerUpdateSlows()
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr || !HasAuthority())
	{
		return;
	}

	if (CVarSlimewallSlow.GetValueOnAnyThread() == 0)
	{
		return;   // RED ARM: the wall still stands and still blocks sight; it just slows nobody.
	}

	const float Linger = FMath::Max(0.f, UTraceSettings::Get().SlimewallSlowLingerSeconds);
	const bool bSlowsOwnTeam = UTraceSettings::Get().bSlimewallSlowsOwnTeam;

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive())
		{
			continue;
		}

		if (!IsInsideWall(Candidate))
		{
			continue;
		}

		// §2 [ASSUMPTION]: "it does not slow Slimeball or his own team." Written HERE rather than left
		// to the choke point's friendly-fire clause on purpose — that clause is off when friendly fire
		// is ON, and a designer turning friendly fire on must not accidentally make Slimeball's own
		// wall a trap for his team. bSlimewallSlowsOwnTeam reverses it in one knob.
		if (!bSlowsOwnTeam && SourceTeam != ETraceTeam::None && Candidate->GetTeam() == SourceTeam)
		{
			continue;
		}

		// *** THE CHOKE POINT. SPEC §4. ***
		// Control, because a slow is movement the target did not ask for. This single call is what
		// makes "a Core carrier is not slowed by a slime wall" true — there is no carrier test in this
		// file, and there must not be one, because a second copy of the rule is a second thing that
		// can rot. The STATIC form, because the wall outlives its owner's pawn.
		if (!UTraceAbilityComponent::CanAffect(SourcePlayerState, Candidate, ETraceAbilityEffect::Control))
		{
			continue;
		}

		if (UTraceSlimewallSlowComponent::ApplyTo(Candidate, SourcePlayerState, Linger) != nullptr)
		{
			TraceSlimewall::RecordEffect(Candidate, TEXT("slow applied"),
				&TraceSlimewall::FSlowTally::Applications);

			bool bAlready = false;
			for (const TWeakObjectPtr<ATraceCharacter>& Past : LifetimeSlowed)
			{
				if (Past.Get() == Candidate)
				{
					bAlready = true;
					break;
				}
			}
			if (!bAlready)
			{
				LifetimeSlowed.Add(Candidate);
				UE_LOG(LogTraceGame, Verbose,
					TEXT("[Slimewall] %s walked into the slime — %.0f%% slower, lingering %.2fs."),
					*GetNameSafe(Candidate),
					FMath::Clamp(UTraceSettings::Get().SlimewallSlowFraction, 0.f, 0.95f) * 100.f, Linger);
			}
		}
	}
}

// =================================================================================================
// The slab — "obstructs vision", and nothing else
// =================================================================================================

void ATraceSlimewall::BuildSlabIfNeeded()
{
	const FVector Extents = FVector(HalfExtents);
	if (Slab == nullptr || SlabMesh == nullptr || Extents.GetMin() <= 0.f)
	{
		return;
	}

	// The transform is re-asserted every call rather than only on the first, because on a client the
	// spawn transform and the replicated centre can arrive in either order and the second one must
	// win. It is three float compares in the common case.
	const FVector Forward = FVector(WallAim).GetSafeNormal();
	if (!Forward.IsNearlyZero())
	{
		SetActorLocationAndRotation(FVector(WallCenter), Forward.Rotation());
	}

	// The slab's SCALE and POSITION are written by UpdateWallAnimation, not here: FX_AUDIO_PLAN §2.10
	// gives the wall a 0.18 s rise, so the drawn Z is a function of the clock rather than a constant.
	// It is called from ServerSpawn and from every Tick, on every machine.

	// THE MESH IS SET ON EVERY NET MODE, INCLUDING A DEDICATED SERVER, and only the MATERIAL is
	// skipped there. Geometry is a gameplay fact and shaders are not: ATraceRippleActor can skip its
	// rings wholesale because they are pure decoration, whereas skipping this mesh would mean the
	// "has no collision" invariant was trivially true on a dedicated server for the wrong reason —
	// there would be nothing to have collision — and Trace.Slimeball.Verify's RED arm could not
	// reproduce there. An invariant that is only testable on one net mode is not an invariant.
	if (!bSlabBuilt)
	{
		bSlabBuilt = true;
		Slab->SetStaticMesh(SlabMesh);
	}

	// THE RED ARM, applied AFTER the mesh (a body instance exists to carry it) and re-applied on every
	// call so toggling the CVar mid-match takes effect on a wall that is already standing. On the
	// shipped arm the else-branch is the single line that makes "can be shot through" true.
	if (CVarSlimewallBlocksBullets.GetValueOnAnyThread() != 0)
	{
		if (Slab->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics)
		{
			Slab->SetCollisionProfileName(TEXT("BlockAll"));
			Slab->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		}
	}
	else if (Slab->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
	{
		Slab->SetCollisionProfileName(TEXT("NoCollision"));
		Slab->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (bSlabDressed)
	{
		return;
	}

	// Shaders are not cooked for server targets, so a dedicated server never builds the material.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bSlabDressed = true;
		return;
	}

	bSlabDressed = true;

	UMaterialInterface* Parent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get();
	if (Parent == nullptr)
	{
		// No material at all: draw it anyway with the engine default. A grey slab still obstructs
		// vision, which is the gameplay half, and a wall you cannot see is far worse than an ugly one.
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Slimewall] neither M_TraceNeon nor BasicShapeMaterial resolved — the slab draws with the "
			     "engine default. It still blocks sight and still does not block bullets."));
		return;
	}

	SlabMID = UMaterialInstanceDynamic::Create(Parent, this);
	if (SlabMID == nullptr)
	{
		return;
	}

	// *** SlimewallOpacity IS A BRIGHTNESS KNOB, NOT A TRANSPARENCY ONE, AND THAT IS A LIMITATION. ***
	//
	// §2 asks for a wall that "obstructs vision", and an OPAQUE slab is the strongest possible reading
	// of that — but this project ships no translucent material: Scripts/generate_content.py builds
	// M_TraceNeon and M_TraceSurface as BLEND_Opaque, and blend mode is compiled into the parent, so a
	// dynamic instance cannot change it. (The one translucent thing nearby, EmissiveMeshMaterial, is
	// BLEND_Additive and writes no depth, i.e. it occludes NOTHING — the exact opposite of what this
	// ability needs, and the reason Oyster's poison cloud uses it.)
	//
	// So the knob dims the slab rather than making it see-through, and 1.0 is what §2 actually
	// describes. A translucent parent material is a content-side need and is filed in the report.
	const float Opacity = FMath::Clamp(UTraceSettings::Get().SlimewallOpacity, 0.f, 1.f);
	const FLinearColor SlabColor = TraceSlimewallTuning::SlimeColor * Opacity;

	// "Color" is what both M_TraceNeon and BasicShapeMaterial call it; "Glow" is neon only and is a
	// no-op on the fallback. Same three-parameter push ATraceRippleActor makes, for the same reason.
	SlabMID->SetVectorParameterValue(TEXT("Color"), SlabColor);
	SlabMID->SetVectorParameterValue(TEXT("BaseColor"), SlabColor);
	SlabMID->SetScalarParameterValue(TEXT("Glow"), TraceSlimewallTuning::SlabGlow * Opacity);
	SlabMID->SetScalarParameterValue(TEXT("Roughness"), 0.85f);
	Slab->SetMaterial(0, SlabMID);

	// CONSTANT, AND THIS IS THE ONE PLACE THE SLAB'S GLOW IS WRITTEN AT FULL STRENGTH. ART_BIBLE §3.3
	// lists "ability lethal volumes (jars, slimewall, gates, quake ring)" among the things that must
	// never brightness-pulse, and the slab is the volume. The only other write is the monotonic expiry
	// fade in UpdateWallAnimation, which is a dissolve rather than an oscillation.

	BuildDressingIfNeeded(Extents);
}

// =================================================================================================
// FX_AUDIO_PLAN §2.10 — the lip, the seams, the rise and the fade
// =================================================================================================

void ATraceSlimewall::BuildDressingIfNeeded(const FVector& Extents)
{
	if (bDressingBuilt || Lip == nullptr || Seams == nullptr || SlabMesh == nullptr)
	{
		return;
	}
	if (Extents.GetMin() <= 0.f)
	{
		return;   // on a client, until the replicated extents arrive
	}
	if (GetNetMode() == NM_DedicatedServer)
	{
		bDressingBuilt = true;   // no shaders there, and unlike the slab no invariant is asked of these
		return;
	}

	bDressingBuilt = true;

	const float ShapeSize = TraceSlimewallTuning::BasicShapeSizeUU;

	// ---- the lip ---------------------------------------------------------------------------------
	//
	// A band capping the slab: the full span in both horizontal axes plus LipProudUU so it stands
	// clear of every face, and LipHeightUU tall. Its Z is written by UpdateWallAnimation, because it
	// has to ride the rising top.
	Lip->SetRelativeScale3D(FVector(
		(Extents.X * 2.f + TraceSlimewallTuning::LipProudUU) / ShapeSize,
		(Extents.Y * 2.f + TraceSlimewallTuning::LipProudUU) / ShapeSize,
		TraceSlimewallTuning::LipHeightUU / ShapeSize));

	LipMID = UTraceFxShapes::MakeGlowMID(Lip, 0, ETraceFxBlend::Emissive, LipBlend);
	if (LipMID != nullptr)
	{
		UTraceFxShapes::SetGlow(LipMID, LipBlend,
			TraceSlimewallTuning::SlimeAccent(), TraceSlimewallTuning::LipGlow);
	}
	// HIDDEN, NOT GREY — the ordinary ART_BIBLE §6.1 ruling, because unlike the slab this is pure
	// decoration. The slab takes the opposite branch (see above: a wall you cannot see is worse than
	// an ugly one) and the difference is exactly that one of them is the gameplay object.
	Lip->SetVisibility(LipMID != nullptr && LipBlend != ETraceFxBlend::None);

	// ---- the seams -------------------------------------------------------------------------------
	//
	// Three vertical strips, evenly dividing the face into four panels, each passing THROUGH the slab
	// and standing SeamProudUU clear of both faces — see the header: a wall about to be re-placed must
	// look the same from either side. Positions are fractions of the half-span, so they follow a
	// retune of SlimewallLengthUU without arithmetic anywhere else.
	Seams->ClearInstances();
	SeamMID = UTraceFxShapes::MakeGlowMID(Seams, 0, ETraceFxBlend::Emissive, SeamBlend);
	if (SeamMID != nullptr)
	{
		UTraceFxShapes::SetGlow(SeamMID, SeamBlend,
			TraceSlimewallTuning::SlimeAccent(), TraceSlimewallTuning::SeamGlow);
	}
	Seams->SetVisibility(SeamMID != nullptr && SeamBlend != ETraceFxBlend::None);

	if (Seams->IsVisible())
	{
		const FVector SeamScale(
			(Extents.X * 2.f + TraceSlimewallTuning::SeamProudUU * 2.f) / ShapeSize,
			TraceSlimewallTuning::SeamWidthUU / ShapeSize,
			1.f);   // the Z is written per-frame by UpdateWallAnimation, with the rise

		for (int32 Index = 0; Index < TraceSlimewallTuning::NumSeams; ++Index)
		{
			// -0.5, 0, +0.5 of the half-span for three seams: four equal panels across the face.
			const float Fraction = -1.f + 2.f * (static_cast<float>(Index) + 1.f)
				/ static_cast<float>(TraceSlimewallTuning::NumSeams + 1);

			Seams->AddInstance(FTransform(FRotator::ZeroRotator,
				FVector(0.f, Fraction * Extents.Y, 0.f), SeamScale));
		}
	}

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Slimewall] dressed: lip %.0f uu tall (+%.0f proud), %d seam(s) %.0f uu wide, %s."),
		TraceSlimewallTuning::LipHeightUU, TraceSlimewallTuning::LipProudUU,
		Seams->GetInstanceCount(), TraceSlimewallTuning::SeamWidthUU, *DescribeDressing());
}

float ATraceSlimewall::GetRisenFraction() const
{
	// *** A WALL WITH NO DEADLINE IS UP FROM ITS FIRST FRAME, AND THAT IS NOT A HARNESS COURTESY. ***
	//
	// ExpireMatchTime == 0 means "this wall never expires", which no CAST wall is: spec v18 §2 gives
	// the ability a 4 s life and UTraceAbilitySetSlimeball always passes a real deadline. The only
	// walls that reach the world without one are FIXTURE walls, and every fixture in this project
	// spawns a wall and measures it in the SAME STATEMENT SEQUENCE — deliberately, so that a bot
	// wandering past cannot land inside a measurement (ATraceSlimewall::ServerUpdateSlows says so in
	// as many words).
	//
	// This branch is here because it was MISSING and Trace.Slimeball.Verify went red: its shot-through
	// RED arm spawns a wall with BlockAll across the firing line and fires in the same frame, and a
	// wall that is 0% risen is a slab of zero height that the shot sails over — so the arm could not
	// reproduce the bug it exists to reproduce, and the verdict came back INVALID (29 passed, 1
	// failed) rather than merely wrong. A cosmetic timeline had made a rule untestable, which is the
	// exact failure ART_BIBLE §6.2 is about, pointed the other way.
	//
	// The zero cases below are the ordinary ones: no cast time yet (a client whose first bunch has not
	// arrived — it must not draw a stump) or the rise switched off.
	if (ExpireMatchTime <= 0.f || SpawnMatchTime <= 0.f || TraceSlimewallTuning::SpawnRiseSeconds <= 0.f)
	{
		return 1.f;
	}
	return FMath::Clamp((MatchTimeNow() - SpawnMatchTime) / TraceSlimewallTuning::SpawnRiseSeconds, 0.f, 1.f);
}

float ATraceSlimewall::GetExpiryFadeFraction() const
{
	if (ExpireMatchTime <= 0.f || TraceSlimewallTuning::ExpiryFadeSeconds <= 0.f)
	{
		return 1.f;   // no deadline: the fixtures' walls, which are destroyed by hand
	}
	return FMath::Clamp((ExpireMatchTime - MatchTimeNow()) / TraceSlimewallTuning::ExpiryFadeSeconds, 0.f, 1.f);
}

int32 ATraceSlimewall::GetDressingPieceCount() const
{
	int32 Count = 0;
	if (Lip != nullptr && Lip->IsVisible())
	{
		++Count;
	}
	if (Seams != nullptr && Seams->IsVisible())
	{
		Count += Seams->GetInstanceCount();
	}
	return Count;
}

FString ATraceSlimewall::DescribeDressing() const
{
	return FString::Printf(TEXT("slab=%s lip=%s seams=%s(%d)"),
		*DescribeSlabMaterial(),
		UTraceFxShapes::BlendName(LipBlend),
		UTraceFxShapes::BlendName(SeamBlend),
		(Seams != nullptr) ? Seams->GetInstanceCount() : 0);
}

void ATraceSlimewall::ApplyRiseGeometry(const FVector& Extents, float Risen)
{
	const float ShapeSize = TraceSlimewallTuning::BasicShapeSizeUU;

	// FROM THE BASE. The slab's local origin is the actor's, so a shorter slab has to be pushed DOWN
	// by exactly the height it is missing or it would shrink toward the middle and look like a wall
	// being crushed rather than one coming up out of the floor. The base therefore stays at -Extents.Z
	// for the whole rise, which is also what makes the drawn volume a SUBSET of the lethal one rather
	// than an offset copy of it — see GetRisenFraction's comment.
	const float DrawnHalfHeight = Extents.Z * Risen;
	const float TopZ = -Extents.Z + 2.f * DrawnHalfHeight;

	Slab->SetRelativeScale3D(FVector(
		(Extents.X * 2.f) / ShapeSize,
		(Extents.Y * 2.f) / ShapeSize,
		(DrawnHalfHeight * 2.f) / ShapeSize));
	Slab->SetRelativeLocation(FVector(0.f, 0.f, -Extents.Z + DrawnHalfHeight));

	// ---- the dressing rides the rising top -------------------------------------------------------
	const bool bTallEnough = (DrawnHalfHeight * 2.f) > TraceSlimewallTuning::MinDrawnHeightUU;

	if (Lip != nullptr && LipBlend != ETraceFxBlend::None)
	{
		Lip->SetVisibility(bTallEnough);
		// Sat onto the top with LipRiseUU of overhang: its top face is above the slab's rather than
		// coplanar with it, which is the difference between a lip and a z-fight.
		Lip->SetRelativeLocation(FVector(0.f, 0.f,
			TopZ + TraceSlimewallTuning::LipRiseUU - 0.5f * TraceSlimewallTuning::LipHeightUU));
	}

	if (Seams == nullptr || SeamBlend == ETraceFxBlend::None || Seams->GetInstanceCount() <= 0)
	{
		return;
	}

	// The seams run from the base to just under the lip, so the three of them grow with the wall.
	const float SeamHeight = FMath::Max(0.f, (TopZ - TraceSlimewallTuning::LipHeightUU) + Extents.Z);
	const bool bSeamsVisible = bTallEnough && SeamHeight > TraceSlimewallTuning::MinDrawnHeightUU;
	Seams->SetVisibility(bSeamsVisible);

	if (!bSeamsVisible)
	{
		return;
	}

	const int32 SeamCount = Seams->GetInstanceCount();
	const float SeamCentreZ = -Extents.Z + 0.5f * SeamHeight;
	for (int32 Index = 0; Index < SeamCount; ++Index)
	{
		FTransform InstanceTransform;
		if (!Seams->GetInstanceTransform(Index, InstanceTransform, /*bWorldSpace*/ false))
		{
			continue;
		}

		FVector Scale = InstanceTransform.GetScale3D();
		Scale.Z = SeamHeight / ShapeSize;
		InstanceTransform.SetScale3D(Scale);

		FVector Location = InstanceTransform.GetLocation();
		Location.Z = SeamCentreZ;
		InstanceTransform.SetLocation(Location);

		// The render state is marked dirty once, on the last instance, rather than three times.
		Seams->UpdateInstanceTransform(Index, InstanceTransform, /*bWorldSpace*/ false,
			/*bMarkRenderStateDirty*/ Index == SeamCount - 1, /*bTeleport*/ true);
	}
}

void ATraceSlimewall::UpdateWallAnimation()
{
	const FVector Extents = FVector(HalfExtents);
	if (Slab == nullptr || Extents.GetMin() <= 0.f)
	{
		return;
	}

	// ---- the rise --------------------------------------------------------------------------------
	//
	// 0.18 s out of a 4 s life, so for 95% of a wall's existence re-running this would be writing the
	// same transforms over and over. Skipped once it has settled, and re-entered if it ever moves.
	const float Risen = GetRisenFraction();
	if (!FMath::IsNearlyEqual(Risen, LastAppliedRisen, 0.001f))
	{
		LastAppliedRisen = Risen;
		ApplyRiseGeometry(Extents, Risen);
	}

	// ---- the expiry fade -------------------------------------------------------------------------
	//
	// §2.10: "Expiry: 0.3 s Glow fade". MONOTONIC — brightness only ever falls — so this is a dissolve
	// and not the brightness pulse ART_BIBLE §3.3 forbids on a lethal volume. The wall keeps its full
	// extents and keeps obstructing vision for every one of those 0.3 s; what is fading is a telegraph
	// that it is about to be gone, which is true information, and the alternative is the pop-out §6.4
	// forbids outright.
	const float Fade = GetExpiryFadeFraction();
	if (FMath::IsNearlyEqual(Fade, LastAppliedFade, 0.005f))
	{
		return;   // a steady wall is not three material writes a frame
	}
	LastAppliedFade = Fade;

	const float Opacity = FMath::Clamp(UTraceSettings::Get().SlimewallOpacity, 0.f, 1.f);
	if (SlabMID != nullptr)
	{
		SlabMID->SetScalarParameterValue(TEXT("Glow"), TraceSlimewallTuning::SlabGlow * Opacity * Fade);
	}
	if (LipMID != nullptr)
	{
		UTraceFxShapes::SetGlow(LipMID, LipBlend,
			TraceSlimewallTuning::SlimeAccent(), TraceSlimewallTuning::LipGlow * Fade);
	}
	if (SeamMID != nullptr)
	{
		UTraceFxShapes::SetGlow(SeamMID, SeamBlend,
			TraceSlimewallTuning::SlimeAccent(), TraceSlimewallTuning::SeamGlow * Fade);
	}
}

bool ATraceSlimewall::HasAnyCollisionEnabled() const
{
	TArray<UPrimitiveComponent*> Primitives;
	GetComponents<UPrimitiveComponent>(Primitives);

	for (const UPrimitiveComponent* Primitive : Primitives)
	{
		if (Primitive != nullptr && Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
		{
			return true;
		}
	}
	return false;
}

FString ATraceSlimewall::DescribeSlabMaterial() const
{
	if (Slab == nullptr)
	{
		return TEXT("<no slab>");
	}
	if (const UMaterialInterface* Applied = Slab->GetMaterial(0))
	{
		return GetNameSafe(Applied);
	}
	return TEXT("<none>");
}

// =================================================================================================
// FX_AUDIO_PLAN §2.10 — THE STICK GOO
//
// "one additive slime sphere r 26 uu I 0.4 at the pawn-wall contact point + 2 drip spheres r 6 uu
//  falling 30 uu, loop"
//
// See the class comment in the header for why this polls MovementActive instead of riding the §1.2
// router edge, and for the one line a kit owner needs to add.
// =================================================================================================

namespace TraceSlimeGooTuning
{
	/** §2.10: the blob. 26 uu radius = 52 uu across, far above ART_BIBLE §3.4's floor. */
	constexpr float BlobRadiusUU = 26.f;

	/** §2.10: the two drips. 6 uu radius = 12 uu across. */
	constexpr float DripRadiusUU = 6.f;

	/** §2.10: "falling 30 uu". */
	constexpr float DripFallUU = 30.f;

	/** §2.10: additive I 0.4. TraceFxLoopBudget clamps to §1.4's 0.5 whatever is asked for here. */
	constexpr float Intensity = 0.4f;

	/** One drip lands every 0.35 s; two drips, staggered, so the goo runs rather than blinks. */
	constexpr float DripSeconds = 0.7f;

	/** The blob and the drips fade in and out over this fraction of a fall — never a pop. */
	constexpr float EdgeFadeFraction = 0.2f;

	/** How often the wall is re-probed while stuck. He slides at most SlimeballWallStickSlideSpeed. */
	constexpr float ProbeIntervalSeconds = 0.2f;

	/** Probes taken around the pawn. Eight is one every 45 degrees — enough for a flat wall face. */
	constexpr int32 NumProbes = 8;
}

UTraceSlimeStickFxComponent::UTraceSlimeStickFxComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Cosmetic and LOCAL. It replicates nothing: the fact it draws (MovementActive) is already
	// replicated on the ability component, so a second copy on the wire would be a second thing that
	// can disagree with the first.
	SetIsReplicatedByDefault(false);
}

UTraceSlimeStickFxComponent* UTraceSlimeStickFxComponent::Find(const ATraceCharacter* Pawn)
{
	return (Pawn != nullptr) ? Pawn->FindComponentByClass<UTraceSlimeStickFxComponent>() : nullptr;
}

UTraceSlimeStickFxComponent* UTraceSlimeStickFxComponent::EnsureOn(ATraceCharacter* Pawn)
{
	if (Pawn == nullptr || !IsValid(Pawn))
	{
		return nullptr;
	}

	// A dedicated server cooks no shaders and has nobody to show them to, and this component does
	// nothing else — so it is not created there at all rather than created and told to do nothing.
	if (Pawn->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}

	if (UTraceSlimeStickFxComponent* Existing = Find(Pawn))
	{
		return Existing;
	}

	UTraceSlimeStickFxComponent* Made = NewObject<UTraceSlimeStickFxComponent>(Pawn);
	if (Made == nullptr)
	{
		return nullptr;
	}
	Made->RegisterComponent();
	return Made;
}

int32 UTraceSlimeStickFxComponent::GetGooPieceCount() const
{
	int32 Count = 0;
	for (const UStaticMeshComponent* Piece : GooPieces)
	{
		if (Piece != nullptr && IsValid(Piece))
		{
			++Count;
		}
	}
	return Count;
}

void UTraceSlimeStickFxComponent::SetStuck(bool bStuck, const FVector& ContactWorld)
{
	// The router seam. Once a kit calls this, the poll stops second-guessing it — otherwise a kit that
	// wired the edge correctly would still be fighting the fallback on every tick.
	bDrivenExternally = true;
	bStuckNow = bStuck;

	if (!bStuck)
	{
		DetachGoo();
		return;
	}

	const ATraceCharacter* Pawn = Cast<ATraceCharacter>(GetOwner());
	if (Pawn == nullptr)
	{
		return;
	}

	if (!ContactWorld.IsNearlyZero())
	{
		ContactLocal = Pawn->GetActorTransform().InverseTransformPosition(ContactWorld);
	}
	else
	{
		FVector Probed = FVector::ZeroVector;
		if (ProbeContactPoint(Probed))
		{
			ContactLocal = Pawn->GetActorTransform().InverseTransformPosition(Probed);
		}
	}

	AttachGoo();
}

bool UTraceSlimeStickFxComponent::ProbeContactPoint(FVector& OutContactWorld) const
{
	const ATraceCharacter* Pawn = Cast<ATraceCharacter>(GetOwner());
	const UWorld* WorldPtr = GetWorld();
	if (Pawn == nullptr || WorldPtr == nullptr)
	{
		return false;
	}

	float CapsuleRadius = 34.f;
	if (const UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent())
	{
		CapsuleRadius = Capsule->GetScaledCapsuleRadius();
	}

	// Out to the same distance the ability itself calls "in reach", so the goo cannot appear on a wall
	// the stick could not have grabbed. Read from the knob rather than typed, for the usual reason.
	const float Reach = CapsuleRadius
		+ FMath::Clamp(UTraceSettings::Get().SlimeballWallStickRangeUU, 10.f, 500.f);

	const FVector Origin = Pawn->GetActorLocation();
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceSlimeGooProbe), /*bTraceComplex*/ false, Pawn);

	bool bFound = false;
	float BestDistance = TNumericLimits<float>::Max();

	for (int32 Index = 0; Index < TraceSlimeGooTuning::NumProbes; ++Index)
	{
		const float Yaw = 360.f * static_cast<float>(Index) / static_cast<float>(TraceSlimeGooTuning::NumProbes);
		const FVector Direction = FVector::ForwardVector.RotateAngleAxis(Yaw, FVector::UpVector);

		FHitResult Hit;
		if (!WorldPtr->LineTraceSingleByChannel(Hit, Origin, Origin + Direction * Reach,
			ECC_Visibility, QueryParams))
		{
			continue;
		}

		if (Hit.Distance < BestDistance)
		{
			BestDistance = static_cast<float>(Hit.Distance);
			// Pulled a blob-radius back off the surface so the sphere sits ON the wall rather than
			// half inside it — additive geometry writes no depth, but a blob centred in the wall would
			// still be half-hidden by everything drawn in front of it.
			OutContactWorld = Hit.ImpactPoint + Hit.ImpactNormal * (TraceSlimeGooTuning::BlobRadiusUU * 0.5f);
			bFound = true;
		}
	}

	return bFound;
}

void UTraceSlimeStickFxComponent::AttachGoo()
{
	if (GooPieces.Num() > 0)
	{
		return;
	}

	ATraceCharacter* Pawn = Cast<ATraceCharacter>(GetOwner());
	UStaticMesh* Sphere = UTraceFxShapes::GetSphere();
	if (Pawn == nullptr || Sphere == nullptr || !Pawn->IsAlive())
	{
		return;
	}

	// Blob first, then the two drips, so that if the §1.4 budget can only afford some of them the
	// player still gets the piece that says "you are stuck here" rather than two loose droplets.
	// (A Slimeball who is stuck AND poisoned is carrying three drips already and will be refused; that
	//  is the budget doing its job and it is logged by the helper, not silently swallowed here.)
	const float Radii[3] = {
		TraceSlimeGooTuning::BlobRadiusUU,
		TraceSlimeGooTuning::DripRadiusUU,
		TraceSlimeGooTuning::DripRadiusUU };

	for (int32 Index = 0; Index < 3; ++Index)
	{
		TObjectPtr<UMaterialInstanceDynamic> MID = nullptr;
		UStaticMeshComponent* Piece = TraceFxLoopBudget::AttachLoopPrimitive(
			Pawn, Pawn->GetRootComponent(), Sphere,
			*FString::Printf(TEXT("SlimeGoo%d"), Index),
			TraceSlimewallTuning::SlimeAccent(), TraceSlimeGooTuning::Intensity,
			ContactLocal, Radii[Index], MID);

		if (Piece == nullptr)
		{
			break;   // the budget refused; keep what we have rather than an incomplete second half
		}

		GooPieces.Add(Piece);
		GooMIDs.Add(MID);
	}

	if (GooPieces.Num() > 0)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Slimewall] stick goo attached to %s: %d of 3 piece(s) at local %s."),
			*GetNameSafe(Pawn), GooPieces.Num(), *ContactLocal.ToCompactString());
	}
}

void UTraceSlimeStickFxComponent::DetachGoo()
{
	if (GooPieces.Num() == 0)
	{
		return;
	}

	APawn* Pawn = Cast<APawn>(GetOwner());
	for (UStaticMeshComponent* Piece : GooPieces)
	{
		TraceFxLoopBudget::DetachLoopPrimitive(Pawn, Piece);
	}
	GooPieces.Reset();
	GooMIDs.Reset();
}

void UTraceSlimeStickFxComponent::PollStuckState()
{
	const ATraceCharacter* Pawn = Cast<ATraceCharacter>(GetOwner());
	if (Pawn == nullptr)
	{
		bStuckNow = false;
		return;
	}

	// THE SAME REPLICATED FACT THE ROUTER WOULD HAVE HANDED US. MovementActive is Slimeball's "stuck to
	// a wall" flag (TraceAbilitySetSlimeball.h:94-95 names it), it lives in the replicated ability
	// scratch pad, and it is therefore already correct on every machine. Reading it here rather than
	// being told about it costs one frame of latency and nothing else.
	const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Pawn);
	bStuckNow = (Comp != nullptr)
		&& Comp->GetCharacterId() == ETraceCharacterId::Slimeball
		&& (Comp->GetNetState().Flags & TraceAbilityFlags::MovementActive) != 0;
}

void UTraceSlimeStickFxComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	Elapsed += DeltaTime;

	if (!bDrivenExternally)
	{
		PollStuckState();
	}

	const ATraceCharacter* Pawn = Cast<ATraceCharacter>(GetOwner());
	if (Pawn == nullptr || !Pawn->IsAlive() || !bStuckNow)
	{
		DetachGoo();
		return;
	}

	// Elle's cloak — FX_AUDIO_PLAN §1.2. She cannot hold a slime wall, but the rule is about ATTACHED
	// emissive FX and not about who owns them, and applying it uniformly is what stops the next effect
	// added here from being the exception.
	bool bHiddenByCloak = false;
	if (const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Pawn))
	{
		if (const UTraceAbilitySetElle* Elle = Comp->GetAbilitySetAs<UTraceAbilitySetElle>())
		{
			bHiddenByCloak = Elle->IsCloakVisualApplied();
		}
	}

	// Re-probe on a timer rather than every frame: he can slide down the wall
	// (SlimeballWallStickSlideSpeed), so the contact point moves, but it moves slowly and eight line
	// traces a frame for a cosmetic would be a real cost for no visible gain.
	if (!bDrivenExternally && Elapsed >= NextProbeSeconds)
	{
		NextProbeSeconds = Elapsed + TraceSlimeGooTuning::ProbeIntervalSeconds;
		FVector Probed = FVector::ZeroVector;
		if (ProbeContactPoint(Probed))
		{
			ContactLocal = Pawn->GetActorTransform().InverseTransformPosition(Probed);
		}
	}

	if (GooPieces.Num() == 0)
	{
		AttachGoo();
		if (GooPieces.Num() == 0)
		{
			return;   // refused by the budget, or no mesh. Survivable: no goo this time.
		}
	}

	for (int32 Index = 0; Index < GooPieces.Num(); ++Index)
	{
		UStaticMeshComponent* Piece = GooPieces[Index];
		if (Piece == nullptr)
		{
			continue;
		}

		Piece->SetVisibility(!bHiddenByCloak);
		if (bHiddenByCloak)
		{
			continue;
		}

		float Intensity = TraceSlimeGooTuning::Intensity;
		FVector Offset = ContactLocal;

		if (Index == 0)
		{
			// The blob is static at the contact point. It IS the "you are attached here" statement, and
			// a blob that moved would be saying something else.
			Piece->SetRelativeLocation(TraceFxLoopBudget::ClampToFootprint(Offset));
		}
		else
		{
			// The drips run down from it, staggered half a cycle apart.
			const float Phase = FMath::Frac(Elapsed / TraceSlimeGooTuning::DripSeconds
				+ 0.5f * static_cast<float>(Index - 1));
			Offset.Z -= TraceSlimeGooTuning::DripFallUU * Phase;
			Piece->SetRelativeLocation(TraceFxLoopBudget::ClampToFootprint(Offset));

			const float Edge = TraceSlimeGooTuning::EdgeFadeFraction;
			Intensity *= FMath::Min(FMath::Min(Phase, 1.f - Phase) / Edge, 1.f);
		}

		if (GooMIDs.IsValidIndex(Index))
		{
			UTraceFxShapes::SetGlow(GooMIDs[Index].Get(), ETraceFxBlend::Additive,
				TraceSlimewallTuning::SlimeAccent(), Intensity);
		}
	}
}

void UTraceSlimeStickFxComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DetachGoo();
	Super::EndPlay(EndPlayReason);
}

void UTraceSlimeStickFxComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	DetachGoo();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

// =================================================================================================
// UTraceSlimeStickSubsystem — the stick moment's ONE producer. See the header for why it exists.
// =================================================================================================

namespace TraceSlimeStickFile
{
	/**
	 * 20 Hz. The thing being watched is a LEVEL that a player holds for a second or more, not an
	 * impulse, so the only cost of polling slowly is how late the sound is: at 20 Hz a stick is heard
	 * within 50 ms of the flag, which is inside the window this project already accepts for a
	 * replicated cosmetic (the goo component's own contact re-probe runs at 5 Hz).
	 */
	constexpr float StickPollIntervalSeconds = 0.05f;
}

bool UTraceSlimeStickSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Game and PIE only, exactly as UTraceAbilityWorldSubsystem restricts itself, and for the same
	// reason: nothing here should run inside an editor preview world.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UTraceSlimeStickSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTraceSlimeStickSubsystem, STATGROUP_Tickables);
}

UTraceSlimeStickSubsystem* UTraceSlimeStickSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* WorldPtr = (WorldContextObject != nullptr) ? WorldContextObject->GetWorld() : nullptr;
	return (WorldPtr != nullptr) ? WorldPtr->GetSubsystem<UTraceSlimeStickSubsystem>() : nullptr;
}

void UTraceSlimeStickSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	PollAccumulator += DeltaTime;
	if (PollAccumulator < TraceSlimeStickFile::StickPollIntervalSeconds)
	{
		return;
	}
	PollAccumulator = 0.f;

	UWorld* const WorldPtr = GetWorld();
	if (WorldPtr == nullptr || WorldPtr->GetGameState() == nullptr)
	{
		return;   // pre-match: nobody has an ability component yet, so there is nothing to poll.
	}

	// The authority decides the SOUND; every machine including the authority draws the GOO. A pure
	// client keeps no edge state at all — see the header.
	const bool bAuthority = (WorldPtr->GetNetMode() != NM_Client);
	const bool bDraws = (WorldPtr->GetNetMode() != NM_DedicatedServer);

	// A LOCAL, not a member. The array holds raw UObject pointers and is alive for the length of one
	// poll; kept as a member it would be an unreflected root nobody nulls on GC, which is the shape of
	// a use-after-free that only shows up on a seamless travel. Twenty allocations a second of a
	// ten-element array is not a cost worth taking that risk for.
	TArray<UTraceAbilityComponent*> Components;
	UTraceAbilityWorldSubsystem::GatherAllComponents(WorldPtr, Components);

	// The weak keys go stale on a disconnect but do NOT compare equal to a null key, so the set has to
	// be swept rather than Remove()d — otherwise it grows by one entry per player who ever stuck and
	// then left.
	for (auto It = WereStuck.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}

	for (UTraceAbilityComponent* Comp : Components)
	{
		if (Comp == nullptr || Comp->GetCharacterId() != ETraceCharacterId::Slimeball)
		{
			// NOT a "was he stuck" reset: a character SWITCH already drops MovementActive, and a
			// player who leaves takes his component with him. Nothing to forget.
			continue;
		}

		ATraceCharacter* const Pawn = Comp->GetOwningCharacter();
		if (Pawn == nullptr || !IsValid(Pawn))
		{
			continue;
		}

		// THE ONE LINE THE GOO HAS BEEN WAITING FOR since it was written. Idempotent, refuses itself
		// on a dedicated server, and costs a FindComponentByClass at 20 Hz per Slimeball after the
		// first call. Its class comment asks for exactly this and names the function.
		UTraceSlimeStickFxComponent* Goo = nullptr;
		if (bDraws)
		{
			Goo = UTraceSlimeStickFxComponent::EnsureOn(Pawn);
		}

		if (!bAuthority)
		{
			continue;
		}

		// THE FACT, from the one place that owns it. `MovementActive` is Slimeball's "stuck to a wall"
		// flag (TraceAbilitySetSlimeball.h names it) and it lives in the replicated ability scratch
		// pad, so this is the same read the goo makes one level down — not a second source of truth.
		//
		// The forced arm wins when something has taken the goo over through SetStuck(), so the sound
		// has ONE producer whether the stick came from the kit or from Trace.Slimeball.StickGoo.
		const bool bStuck = (Goo != nullptr && Goo->IsForcedStuck())
			|| ((Comp->GetNetState().Flags & TraceAbilityFlags::MovementActive) != 0);

		const TWeakObjectPtr<UTraceAbilityComponent> Key(Comp);
		const bool bWas = WereStuck.Contains(Key);

		if (bStuck == bWas)
		{
			continue;
		}

		if (!bStuck)
		{
			WereStuck.Remove(Key);
			continue;
		}

		WereStuck.Add(Key);

		// WORLD-SIDE, so this is a multicast and everybody nearby hears it — the event is declared
		// that way in Audio/TraceSoundEvents.h ("somebody sticks to slime"), which is the right side
		// for it: the interesting listener is the enemy who now knows a Slimeball is on the wall
		// above them, not the Slimeball who pressed the key.
		//
		// AT THE PAWN and not at the wall contact. The contact point is only known on machines that
		// drew the goo (the probe is client-side and a dedicated server has no goo at all), and the
		// two are inside 50 uu of each other against an attenuation whose inner radius is measured in
		// thousands — so using the pawn is not an approximation anybody can hear, and it is the one
		// location that exists on every kind of authority.
		TraceAudio::PlayAt(WorldPtr, TraceSoundEvents::SlimeballStick, Pawn->GetActorLocation());
		++StickSoundCount;

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Slimewall] stick sound: %s stuck (%s) -> SlimeballStick at %s. Total this world: %d."),
			*GetNameSafe(Pawn),
			(Goo != nullptr && Goo->IsForcedStuck()) ? TEXT("forced") : TEXT("MovementActive"),
			*Pawn->GetActorLocation().ToCompactString(), StickSoundCount);
	}
}

#if !UE_BUILD_SHIPPING
// =================================================================================================
// THE EVIDENCE for the two elements of §2.10 that have no shipping call site yet.
//
// `Trace.Slimeball.StickGoo [0|1]` puts a UTraceSlimeStickFxComponent on every Slimeball pawn in the
// world and forces it on or off. It exists because the router edge this effect is specified to ride
// lives in a file this tranche does not own (see the class comment), and an FX that cannot be made to
// appear cannot be judged — a release report claiming "implemented" for something nobody has ever
// seen on screen is exactly the kind of claim this project's verification culture exists to prevent.
//
// It is a CHEAT command inside #if !UE_BUILD_SHIPPING, like every other harness here.
// =================================================================================================
namespace TraceSlimeGooVerify
{
	UWorld* FindGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	void StickGooCommand(const TArray<FString>& Args)
	{
		UWorld* WorldPtr = FindGameWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[SlimeGoo] no game world."));
			return;
		}

		const bool bOn = (Args.Num() == 0) || (FCString::Atoi(*Args[0]) != 0);

		int32 Touched = 0;
		int32 Drawn = 0;
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Pawn = *It;
			if (Pawn == nullptr || !Pawn->IsAlive())
			{
				continue;
			}

			UTraceSlimeStickFxComponent* Goo = UTraceSlimeStickFxComponent::EnsureOn(Pawn);
			if (Goo == nullptr)
			{
				continue;
			}

			Goo->SetStuck(bOn, FVector::ZeroVector);
			++Touched;
			Drawn += Goo->GetGooPieceCount();
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[SlimeGoo] forced %s on %d pawn(s); %d goo piece(s) drawn on this machine. "
			     "(FX_AUDIO_PLAN §2.10 asks for 3 per stuck pawn; fewer means the §1.4 loop budget "
			     "refused some, which it logs.)"),
			bOn ? TEXT("ON") : TEXT("OFF"), Touched, Drawn);
	}

	FAutoConsoleCommand CmdSlimeStickGoo(
		TEXT("Trace.Slimeball.StickGoo"),
		TEXT("FX_AUDIO_PLAN §2.10. Forces the stick goo on (1, default) or off (0) for every living "
		     "pawn, so the effect can be photographed before its router edge is wired. Cheat/dev only."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&TraceSlimeGooVerify::StickGooCommand));
}

// =================================================================================================
// FX_AUDIO_PLAN §2.10 — THE PARADE
//
// Trace.Slimeball.FxParade
//     SERVER. Puts a real wall up through ATraceSlimewall::ServerSpawn — the shipping path, so the
//     SlimeSplat burst and the SlimeballWall sound fire exactly as they do in a match — and
//     photographs it at three moments that are three different claims:
//
//       rise    ~0.09 s in, i.e. HALF WAY THROUGH the 0.18 s rise: the wall must be a partial slab
//               standing ON THE FLOOR, not a shrunken one floating at mid-height, and the lip must
//               be riding its top rather than parked where the top will eventually be.
//       up      once it is up: the lip, the three seams and the splat blobs, judged together.
//       fade    inside the last 0.3 s: dimmer than the "up" frame, and still there.
//
//     Then it forces the stick goo onto the local pawn and photographs it, and slows a victim and
//     photographs the SLOWED ring from outside — neither of which can be seen from a first-person
//     view of one's own rifle.
//
// Every step prints what it measured off the live actor (rise fraction, fade fraction, dressing piece
// count, achieved blends) so the log and the frame are two independent claims about the same thing.
// =================================================================================================

namespace TraceSlimeFxParade
{
	struct FParadeRun
	{
		int32 Step = 0;
		int32 AttemptsLeft = 40;
		double NextRealTime = 0.0;

		TWeakObjectPtr<ATraceSlimewall> Wall;
		TWeakObjectPtr<ATraceCharacter> Victim;
		TWeakObjectPtr<ACameraActor> ShotCamera;
		FString ShotPath;
		FVector WallCentre = FVector::ZeroVector;

		/** Sampled BEFORE the cast, so the wall's own sound is a delta and not a running total. */
		int32 AudioBaselineWall = 0;
	};

	/**
	 * How many times @p Event has reached the ENGINE on this machine. The subsystem's map is bumped
	 * after the side gate, the settings gate, the device test and the resolve, so a delta across a
	 * step separates "I called Play" from "a sound played".
	 */
	int32 AudioPlays(UWorld* WorldPtr, FName Event)
	{
		const UTraceAudioSubsystem* Audio = (WorldPtr != nullptr)
			? WorldPtr->GetSubsystem<UTraceAudioSubsystem>() : nullptr;
		if (Audio == nullptr)
		{
			return 0;
		}
		const int32* Found = Audio->GetPlaysByEvent().Find(Event);
		return (Found != nullptr) ? *Found : 0;
	}

	bool TickParade(TSharedPtr<FParadeRun> Run);

	void Schedule(TSharedPtr<FParadeRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + static_cast<double>(DelaySeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickParade(Run);
			}), 0.f);
	}

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

	/**
	 * Point the view at @p Focus from @p DistanceUU away along @p Away (a planar unit vector) and
	 * @p HeightUU up. Split out of LookAt at Patch 28 item 2 because a wall that RUNS FORWARD has to
	 * be photographed from two places to be judged at all: from behind him, where the run reads as
	 * depth, and from beside it, where it reads as length.
	 */
	ACameraActor* LookFrom(UWorld* WorldPtr, TSharedPtr<FParadeRun> Run, const FVector& Focus,
		const FVector& Away, float DistanceUU, float HeightUU)
	{
		APlayerController* PC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		if (PC == nullptr)
		{
			return nullptr;
		}

		const FVector Eye = Focus + Away * DistanceUU + FVector(0.f, 0.f, HeightUU);

		if (ACameraActor* Old = Run->ShotCamera.Get())
		{
			Old->Destroy();
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transient;

		ACameraActor* Camera = WorldPtr->SpawnActor<ACameraActor>(
			ACameraActor::StaticClass(), FTransform((Focus - Eye).Rotation(), Eye), SpawnParams);
		if (Camera != nullptr)
		{
			PC->SetViewTargetWithBlend(Camera, 0.f);
			Run->ShotCamera = Camera;
		}
		return Camera;
	}

	/**
	 * The original framing: back along the line to the local pawn — the one direction already known to
	 * be open, because the player is standing in it.
	 */
	ACameraActor* LookAt(UWorld* WorldPtr, TSharedPtr<FParadeRun> Run, const FVector& Focus,
		float DistanceUU, float HeightUU)
	{
		FVector Away(1.f, 0.f, 0.f);
		const APlayerController* PC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		if (PC != nullptr)
		{
			if (const APawn* LocalPawn = PC->GetPawn())
			{
				const FVector Towards = (LocalPawn->GetActorLocation() - Focus).GetSafeNormal2D();
				if (!Towards.IsNearlyZero())
				{
					Away = Towards;
				}
			}
		}
		return LookFrom(WorldPtr, Run, Focus, Away, DistanceUU, HeightUU);
	}

	FString Shoot(const TCHAR* Label)
	{
		const FString FileName = FString::Printf(TEXT("SlimeFx_%s_pid%d.png"),
			Label, FPlatformProcess::GetCurrentProcessId());
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);

		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Path));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[SlimeFx] Screenshot requested: %s"), *Path);
		return Path;
	}

	void ConfirmShot(const FString& Path)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.FileExists(*Path))
		{
			UE_LOG(LogTraceGame, Display, TEXT("[SlimeFx] Screenshot written (%lld bytes): %s"),
				PlatformFile.FileSize(*Path), *Path);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[SlimeFx] No screenshot appeared at: %s"), *Path);
		}
	}

	void ReportWall(const TCHAR* Moment, const ATraceSlimewall* Wall)
	{
		if (Wall == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[SlimeFx] %s: *** NO WALL ***"), Moment);
			return;
		}
		UE_LOG(LogTraceGame, Display,
			TEXT("[SlimeFx] %s: risen %.2f, fade %.2f, %d dressing piece(s), %s, collision=%d."),
			Moment, Wall->GetRisenFraction(), Wall->GetExpiryFadeFraction(),
			Wall->GetDressingPieceCount(), *Wall->DescribeDressing(),
			Wall->HasAnyCollisionEnabled() ? 1 : 0);
	}

	/** Stops a bot where it stands — see the twin in Oyster's parade for why a moving subject is useless. */
	void FreezeVictim(ATraceCharacter* Victim)
	{
		if (Victim == nullptr)
		{
			return;
		}
		if (UTraceCharacterMovementComponent* MoveComp = Victim->GetTraceMovement())
		{
			MoveComp->StopMovementImmediately();
			MoveComp->DisableMovement();
		}
	}

	void ThawVictim(ATraceCharacter* Victim)
	{
		if (Victim == nullptr)
		{
			return;
		}
		if (UTraceCharacterMovementComponent* MoveComp = Victim->GetTraceMovement())
		{
			MoveComp->SetMovementMode(MOVE_Walking);
		}
	}

	/**
	 * A living ENEMY of @p NotThis, falling back to anybody else.
	 *
	 * The enemy test matters here for the same reason it does in Oyster's parade: the §4 choke point
	 * refuses Control on a team-mate, and ATraceSlimewall additionally refuses its own side outright
	 * (bSlimewallSlowsOwnTeam), so a SLOWED ring staged on a friendly bot would never light up.
	 */
	ATraceCharacter* FindVictim(UWorld* WorldPtr, const ATraceCharacter* NotThis)
	{
		const ETraceTeam MyTeam = (NotThis != nullptr) ? NotThis->GetTeam() : ETraceTeam::None;
		ATraceCharacter* Fallback = nullptr;

		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == NotThis || !Candidate->IsAlive())
			{
				continue;
			}
			// AND NOT ONE THIS MACHINE IS NOT DRAWING. The first frame of the SLOWED ring came back as a
			// ring lying on an empty floor: the tell was correct, on the right pawn, at the right hue —
			// and the pawn's body was simply not rendered (a corpse still hidden, or a cloak). A
			// photograph of a status effect needs the BODY it is a status of, so an invisible candidate
			// is skipped here rather than explained away in a report.
			if (Candidate->IsHidden() || (Candidate->GetMesh() != nullptr && !Candidate->GetMesh()->IsVisible()))
			{
				continue;
			}

			// AND NOT THE CARRIER. Spec §4 is the founding invariant — no ability Control reaches a
			// player holding the Core — so a fixture that stages one on the carrier is photographing
			// the rule working, labelled as the effect failing. That is precisely what this parade's
			// first run did: SLOWED came back active=0 and the frame shows the Core in the victim's
			// hands. Excluded here rather than "handled" at the shot, because there is nothing to fix.
			if (UTraceAbilityComponent::IsCarrier(Candidate))
			{
				continue;
			}

			const ETraceTeam TheirTeam = Candidate->GetTeam();
			if (MyTeam != ETraceTeam::None && TheirTeam != ETraceTeam::None && TheirTeam != MyTeam)
			{
				return Candidate;
			}
			if (Fallback == nullptr)
			{
				Fallback = Candidate;
			}
		}
		return Fallback;
	}

	bool TickParade(TSharedPtr<FParadeRun> Run)
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[SlimeFx] No authoritative world. SERVER ONLY."));
			return false;
		}
		if (WorldPtr->IsPaused())
		{
			if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
			{
				FirstPC->SetPause(false);
				UE_LOG(LogTraceGame, Warning, TEXT("[SlimeFx] The world was PAUSED (character select). Unpaused."));
			}
		}

		UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
		ATraceCharacter* Pawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
		if (Comp == nullptr || Pawn == nullptr)
		{
			if (Run->AttemptsLeft-- > 0)
			{
				Schedule(Run, 1.0f);
				return false;
			}
			UE_LOG(LogTraceGame, Error, TEXT("[SlimeFx] No human pawn inside the budget."));
			return false;
		}

		const UTraceSettings& Settings = UTraceSettings::Get();

		switch (Run->Step)
		{
		// ---- 0: put a real wall up, then shoot it MID-RISE -----------------------------------------
		case 0:
		{
			Comp->ServerSetCharacter(ETraceCharacterId::Slimeball);

			const FVector Facing = Pawn->GetActorForwardVector().GetSafeNormal2D();

			// *** THROUGH THE SHIPPED PLACEMENT, AS OF PATCH 28 ITEM 2. ***
			//
			// This step used to build the wall itself — centre at Facing x 700, extents straight off
			// the three knobs, the aim as the slab normal. That was harmless while the cast produced
			// the same shape, and became a LIE the moment the cast started laying the wall forward: a
			// parade that photographs a geometry the game no longer makes is worse than no parade.
			// ResolveForwardRun is the same function UTraceAbilitySetSlimeball::ActivateAbility calls,
			// so these frames are of the wall a press actually puts up — including under the
			// Trace.Slimeball.WallForward 0 arm, which is how the before/after pair is taken.
			FVector Extents = FVector::ZeroVector;
			FVector SlabNormal = FVector::ZeroVector;
			FString PlacementWhy;
			if (!ATraceSlimewall::ResolveForwardRun(WorldPtr, Pawn, Facing,
					Settings.SlimewallWidthUU, Settings.SlimewallLengthUU, Settings.SlimewallHeightUU,
					FMath::Max(0.f, Settings.SlimewallRangeUU),
					Run->WallCentre, SlabNormal, Extents, PlacementWhy))
			{
				// A live arena is full of pillars; where he happens to be standing is not this
				// parade's subject. Turn him around and try again rather than reporting a failure.
				UE_LOG(LogTraceGame, Warning,
					TEXT("[SlimeFx] no room for a wall from here (%s). Turning him and retrying."),
					*PlacementWhy);
				Pawn->SetActorRotation(FRotator(0.f, Pawn->GetActorRotation().Yaw + 90.f, 0.f));
				if (Run->AttemptsLeft-- > 0)
				{
					Schedule(Run, 0.5f);
					return false;
				}
				UE_LOG(LogTraceGame, Error, TEXT("[SlimeFx] never found room for a wall."));
				return false;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[SlimeFx] PLACEMENT (%s): centre %s, slab normal %s, half extents %s — "
				     "|dot(normal, facing)| %.3f, |dot(long axis, facing)| %.3f. Forward arm is %d."),
				*PlacementWhy, *Run->WallCentre.ToCompactString(), *SlabNormal.ToCompactString(),
				*Extents.ToCompactString(),
				FMath::Abs(static_cast<float>(FVector::DotProduct(SlabNormal, Facing))),
				FMath::Abs(static_cast<float>(FVector::DotProduct(
					FVector::CrossProduct(FVector::UpVector, SlabNormal).GetSafeNormal(), Facing))),
				CVarSlimewallForward.GetValueOnAnyThread());

			// The camera FIRST, so the rise frame is not a picture of the ground: the shot 90 ms from
			// now is the whole point of this step and a view-target blend costs a frame of its own.
			LookAt(WorldPtr, Run, Run->WallCentre, 900.f, 220.f);

			const float Expire = static_cast<float>(WorldPtr->GetGameState()->GetServerWorldTimeSeconds())
				+ FMath::Max(1.f, Settings.SlimewallDurationSeconds);

			Run->AudioBaselineWall = AudioPlays(WorldPtr, TraceSoundEvents::SlimeballWall);

			Run->Wall = ATraceSlimewall::ServerSpawn(WorldPtr, Pawn->GetPlayerState(), Pawn->GetTeam(),
				Run->WallCentre, SlabNormal, Extents, Expire);

			Run->Step = 1;
			Schedule(Run, 0.09f);   // half way through the 0.18 s rise
			return false;
		}

		case 1:
		{
			ReportWall(TEXT("RISE"), Run->Wall.Get());

			int32 Splats = 0;
			for (TActorIterator<ATraceFxBurst> It(WorldPtr); It; ++It)
			{
				if (*It != nullptr && (*It)->GetBurstType() == ETraceFxBurstType::SlimeSplat)
				{
					++Splats;
					UE_LOG(LogTraceGame, Display,
						TEXT("[SlimeFx] SlimeSplat live: %s, radius %.0f uu, %d/%d primitive(s) visible."),
						*(*It)->DescribeBlends(), (*It)->GetResolvedRadiusUU(),
						(*It)->GetVisiblePrimitiveCount(), (*It)->GetPrimitiveCount());
				}
			}
			UE_LOG(LogTraceGame, Display,
				TEXT("[SlimeFx] CAST: %d SlimeSplat burst(s) at the base; SlimeballWall reached the engine "
				     "%d time(s) — FX_AUDIO_PLAN §8.7 wants exactly 1 per cast."),
				Splats, AudioPlays(WorldPtr, TraceSoundEvents::SlimeballWall) - Run->AudioBaselineWall);

			Run->ShotPath = Shoot(TEXT("rise"));
			Run->Step = 2;
			Schedule(Run, 1.6f);
			return false;
		}

		// ---- 2: the wall UP — lip, seams, splat ---------------------------------------------------
		case 2:
		{
			ConfirmShot(Run->ShotPath);
			ReportWall(TEXT("UP"), Run->Wall.Get());
			Run->ShotPath = Shoot(TEXT("up"));
			Run->Step = 10;
			Schedule(Run, 0.55f);
			return false;
		}

		// ---- 10 / 11: PATCH 28 ITEM 2 — the placement, from BEHIND him and from BESIDE the wall ----
		//
		// TWO FRAMES BECAUSE ONE CANNOT SETTLE THE QUESTION. From behind him a forward wall is a
		// corridor running away and a lateral wall is a barrier across the view — that frame says
		// which of the two he got. From beside it a forward wall is 1100 uu long and a lateral one is
		// 176 uu of end-on slab — that frame says the run is really there and is not foreshortening.
		// The eye is placed off the wall's OWN basis rather than off a world axis, so both framings
		// follow the wall whichever way it points, including under the legacy arm.
		case 10:
		{
			ConfirmShot(Run->ShotPath);

			const ATraceSlimewall* Wall = Run->Wall.Get();
			const FVector Normal = (Wall != nullptr) ? Wall->GetWallAim().GetSafeNormal() : Pawn->GetActorForwardVector();
			const FVector LongAxis = FVector::CrossProduct(FVector::UpVector, Normal).GetSafeNormal();

			// BEHIND HIM — anchored to the PAWN, not to the wall.
			//
			// The first version of this measured 1150 uu back from the WALL CENTRE, which is fine for a
			// forward wall (its centre is ~950 uu ahead of him, so the eye lands just behind his head)
			// and useless for the lateral arm, whose centre is only 400 uu ahead: the eye went 750 uu
			// BEHIND him, through the arena shell, and the frame came back an empty blue field. Anchored
			// to his own capsule it is the same over-the-shoulder framing whichever way the wall points,
			// which is also what the request asked for in as many words — "from behind Slimeball".
			const FVector Facing2 = Pawn->GetActorForwardVector().GetSafeNormal2D();
			const FVector Behind = Facing2.IsNearlyZero() ? -LongAxis : -Facing2;
			LookFrom(WorldPtr, Run, Run->WallCentre,
				(Pawn->GetActorLocation() + Behind * 320.f + FVector(0.f, 0.f, 260.f) - Run->WallCentre)
					.GetSafeNormal(),
				static_cast<float>((Pawn->GetActorLocation() + Behind * 320.f
					+ FVector(0.f, 0.f, 260.f) - Run->WallCentre).Size()),
				0.f);

			Run->ShotPath = Shoot(TEXT("place_behind"));
			Run->Step = 11;
			Schedule(Run, 0.55f);
			return false;
		}

		case 11:
		{
			ConfirmShot(Run->ShotPath);

			const ATraceSlimewall* Wall = Run->Wall.Get();
			const FVector Normal = (Wall != nullptr) ? Wall->GetWallAim().GetSafeNormal() : Pawn->GetActorForwardVector();

			// BESIDE IT: straight out along the slab's own normal, which is broadside to the run.
			LookFrom(WorldPtr, Run, Run->WallCentre, Normal, 1500.f, 520.f);

			Run->ShotPath = Shoot(TEXT("place_beside"));
			Run->Step = 3;
			// Land inside the last 0.3 s of a SlimewallDurationSeconds life. Computed from the wall's
			// own deadline rather than from a guess, so a retune of the duration moves it.
			{
				const float Now = static_cast<float>(WorldPtr->GetGameState()->GetServerWorldTimeSeconds());
				const float ToFade = (Wall != nullptr)
					? FMath::Max(0.2f, (Wall->GetExpireMatchTime() - Now) - 0.12f) : 1.f;
				Schedule(Run, ToFade);
			}
			return false;
		}

		// ---- 3: the expiry fade -------------------------------------------------------------------
		case 3:
		{
			ConfirmShot(Run->ShotPath);
			ReportWall(TEXT("FADE"), Run->Wall.Get());
			Run->ShotPath = Shoot(TEXT("fade"));
			Run->Step = 4;
			Schedule(Run, 2.0f);
			return false;
		}

		// ---- 4: the stick goo, on the local pawn, seen from outside --------------------------------
		case 4:
		{
			ConfirmShot(Run->ShotPath);

			UTraceSlimeStickFxComponent* Goo = UTraceSlimeStickFxComponent::EnsureOn(Pawn);
			if (Goo != nullptr)
			{
				// A CONTACT POINT IS SUPPLIED rather than probed for. The pawn is standing in the open
				// here — there is no wall to stick to — and the probe correctly finds nothing and drops
				// the goo to the capsule centre, which is the right DEGRADATION but the wrong picture:
				// §2.10 places it "at the pawn-wall contact point", and a frame of the fallback would be
				// evidence for a claim nobody made. So the router's own seam is used to say where the
				// wall is, at hip height one capsule-radius in front, which is where a real stick puts it.
				float Radius = 34.f;
				float HipZ = 0.f;
				if (const UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent())
				{
					Radius = Capsule->GetScaledCapsuleRadius();
					HipZ = -0.25f * Capsule->GetScaledCapsuleHalfHeight();
				}
				Goo->SetStuck(true, Pawn->GetActorLocation()
					+ Pawn->GetActorForwardVector().GetSafeNormal2D() * Radius
					+ FVector(0.f, 0.f, HipZ));
			}

			LookAt(WorldPtr, Run, Pawn->GetActorLocation(), 260.f, 40.f);
			Run->Step = 5;
			Schedule(Run, 0.8f);
			return false;
		}

		case 5:
		{
			const UTraceSlimeStickFxComponent* Goo = UTraceSlimeStickFxComponent::Find(Pawn);
			UE_LOG(LogTraceGame, Display,
				TEXT("[SlimeFx] STICK GOO on %s: %d piece(s) (FX_AUDIO_PLAN §2.10 asks for 3; fewer means "
				     "the §1.4 loop budget refused some, which it logs)."),
				*GetNameSafe(Pawn), (Goo != nullptr) ? Goo->GetGooPieceCount() : -1);

			LookAt(WorldPtr, Run, Pawn->GetActorLocation(), 230.f, 30.f);
			Run->ShotPath = Shoot(TEXT("stickgoo"));
			Run->Step = 6;
			Schedule(Run, 2.0f);
			return false;
		}

		// ---- 6: the SLOWED ring on a victim ---------------------------------------------------------
		case 6:
		{
			ConfirmShot(Run->ShotPath);

			if (UTraceSlimeStickFxComponent* Goo = UTraceSlimeStickFxComponent::Find(Pawn))
			{
				Goo->SetStuck(false, FVector::ZeroVector);
			}

			ATraceCharacter* Victim = FindVictim(WorldPtr, Pawn);
			if (Victim == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[SlimeFx] No second pawn, so the SLOWED ring cannot be photographed. Run with ?bots=N."));
				Run->Step = 8;
				Schedule(Run, 0.f);
				return false;
			}

			// Through the shipping entry point, with the shipping linger, so what is photographed is the
			// component a wall really attaches and not a special harness object.
			FreezeVictim(Victim);
			UTraceSlimewallSlowComponent::ApplyTo(Victim, Pawn->GetPlayerState(),
				FMath::Max(2.f, Settings.SlimewallSlowLingerSeconds));
			Run->Victim = Victim;

			LookAt(WorldPtr, Run, Victim->GetActorLocation() - FVector(0.f, 0.f, 40.f), 230.f, 70.f);
			Run->Step = 7;
			Schedule(Run, 0.6f);
			return false;
		}

		case 7:
		{
			const ATraceCharacter* Victim = Run->Victim.Get();
			const UTraceSlimewallSlowComponent* Slow = UTraceSlimewallSlowComponent::Find(Victim);
			UE_LOG(LogTraceGame, Display,
				TEXT("[SlimeFx] SLOWED tell on %s (hidden=%d, mesh visible=%d): active=%d, multiplier %.3f, "
				     "ring drawn=%d."),
				*GetNameSafe(Victim),
				(Victim != nullptr && Victim->IsHidden()) ? 1 : 0,
				(Victim != nullptr && Victim->GetMesh() != nullptr && Victim->GetMesh()->IsVisible()) ? 1 : 0,
				(Slow != nullptr && Slow->IsSlowActive()) ? 1 : 0,
				(Slow != nullptr) ? Slow->GetSpeedMultiplier() : 1.f,
				(Slow != nullptr && Slow->IsSlowTellDrawn()) ? 1 : 0);

			// THE F7 LINE, MEASURED WHERE IT MATTERS. TraceAbilityDebuff::GetMoveSpeedMultiplier is what
			// UTraceCharacterMovementComponent::GetMaxSpeed() multiplies by, so this is the number that
			// actually reaches movement — and it must equal the component's own multiplier exactly. If
			// the clamp deletion and the aggregator line ever came apart, this line prints 0.42 beside
			// a 0.65 and says so without anybody having to read the movement code.
			const float Aggregated = TraceAbilityDebuff::GetMoveSpeedMultiplier(Victim);
			const float Expected = (Slow != nullptr) ? Slow->GetSpeedMultiplier() : 1.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("[SlimeFx] F7 AGGREGATOR: TraceAbilityDebuff::GetMoveSpeedMultiplier = %.4f, component says "
				     "%.4f — %s. (SlimewallSlowFraction %.2f)"),
				Aggregated, Expected,
				FMath::IsNearlyEqual(Aggregated, Expected, 0.001f) ? TEXT("PASS, applied exactly once")
					: TEXT("*** FAIL: the slow is compounding or missing ***"),
				Settings.SlimewallSlowFraction);

			// Re-aimed at the body that is actually there when the shutter opens.
			if (Victim != nullptr)
			{
				LookAt(WorldPtr, Run, Victim->GetActorLocation() - FVector(0.f, 0.f, 40.f), 230.f, 70.f);
			}

			Run->ShotPath = Shoot(TEXT("slowed"));
			Run->Step = 8;
			Schedule(Run, 2.0f);
			return false;
		}

		default:
		{
			ConfirmShot(Run->ShotPath);

			ThawVictim(Run->Victim.Get());

			if (APlayerController* PC = WorldPtr->GetFirstPlayerController())
			{
				PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
			}
			if (ACameraActor* Camera = Run->ShotCamera.Get())
			{
				Camera->Destroy();
			}

			UE_LOG(LogTraceGame, Display, TEXT("[SlimeFx] ===== parade complete. ====="));
			return false;
		}
		}
	}

	FAutoConsoleCommand CmdSlimeFxParade(
		TEXT("Trace.Slimeball.FxParade"),
		TEXT("Dev only, SERVER. FX_AUDIO_PLAN §2.10. Puts a real Slimewall up through the shipping spawn "
		     "path and photographs it mid-rise, up (lip + seams + splat) and mid-fade; then the stick goo "
		     "and a victim's SLOWED ring, both framed from outside. Prints the rise/fade fractions, the "
		     "dressing count, the achieved blends and the F7 aggregator's answer beside the component's."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			Schedule(MakeShared<FParadeRun>(), 0.f);
		}));
}

#endif   // !UE_BUILD_SHIPPING
