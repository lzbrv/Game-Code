// Trace — Elle's SNAP gate. See the header for the clause-by-clause reading of spec v18 §2 and for
// why the carrier rule is asked rather than re-implemented here.

#include "Abilities/Characters/TraceElleGate.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE RED ARM for the teleport itself.
//
// Removes the step-through and NOTHING else: the gates still spawn, still pair, still expire, still
// draw. Trace.Elle.Verify's teleport assertions must go red under it while every other assertion in
// the same run stays green — which is what distinguishes "the teleport is broken" from "the gates
// never got placed", the two failures a single green would confuse.
//
// Deliberately does NOT touch the carrier rule: that has its own arm (Trace.Ability.CarrierImmune),
// it is the framework's, and adding a second switch that could also silence a carrier teleport would
// make Trace.Elle.CarrierTest's red arm ambiguous.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarElleSnapTeleportEnabled(
	TEXT("Trace.Elle.SnapTeleportEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = a paired gate moves whoever steps into it. 0 = the gates exist, "
	     "pair and expire exactly as they do now but move nobody, so every teleport assertion in "
	     "Trace.Elle.Verify must go red. Never ship 0."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarElleSnapStepIn(
	TEXT("Trace.Elle.SnapStepIn"),
	1,
	TEXT("Dev/red arm for DEMO 17 item 1. 1 (default) = a gate takes you on the frame you STEP IN, so "
	     "standing in a mouth does nothing. 0 = the pre-Demo-17 proximity poll, which throws a "
	     "stationary player across the map once per re-entry lockout for the pair's whole life — the "
	     "reported 'it doesn't work at all'. Never ship 0."),
	ECVF_Cheat);

// =================================================================================================
// THE RED ARM FOR DEMO 20 ITEM 4 — "Elle's portal is invisible."
//
// It restores the exact shipped defect, which was ONE missing call: the ring component was given its
// sixty instances and never given a MESH. UInstancedStaticMeshComponent accepts every AddInstance
// without one and reports them all back, but UStaticMeshComponent::CreateSceneProxy returns nullptr
// when GetStaticMesh() is null, so the component is never handed to the renderer at all. The gate
// existed, replicated, paired, expired and teleported perfectly — and nobody could see it.
//
// That is also why the bug outlived Demo 17's dedicated "is it drawn?" harness: GetDrawnBeadCount()
// returned the INSTANCE count, which was a healthy 60 either way, and Trace.Elle.SnapPressTest passed
// "...and it is actually DRAWN on this machine (60 ring beads)" on every build the player was staring
// at an empty floor on. The counter now asks whether there is a mesh as well, so this switch makes
// that assertion — and nothing else in the file — go red.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarElleGateVisible(
	TEXT("Trace.Elle.GateVisible"),
	1,
	TEXT("Dev/red arm for DEMO 20 item 4. 1 (default) = the ring component is given its bead mesh, so "
	     "the gate is on screen. 0 = the instances are built exactly as before but the mesh is never "
	     "set, which is precisely the shipped 'Elle's portal is invisible' bug: every rule still works "
	     "and nothing is drawn. Never ship 0."),
	ECVF_Cheat);

namespace TraceElleGateFile
{
	/** Beads per ring. Twenty reads as a ring without being a mesh budget — the Ripple's number. */
	constexpr int32 BeadsPerRing = 20;

	/** Bead cross-section, uu. /Engine/BasicShapes primitives are 100 uu across. */
	constexpr float BeadThicknessUU = 12.f;

	/**
	 * Three stacked rings — ankle, waist, head — rather than one on the floor.
	 *
	 * A single floor ring is invisible from anywhere but directly above it, and a gate that cannot be
	 * seen across the arena is a gate nobody walks into on purpose. Stacking three turns the same
	 * bead budget into a column that reads at range, which is what the Core's beacon does and for the
	 * same reason.
	 */
	constexpr int32 RingCount = 3;

	/** Height of the topmost ring above the mouth, uu. Roughly a standing pawn. */
	constexpr float ColumnHeightUU = 170.f;

	/**
	 * Emissive strength for M_TraceNeon.
	 *
	 * *** 1.0, NOT THE 3.5 THE REST OF THIS PROJECT USES, AND THE REASON IS THE ASKED-FOR COLOUR. ***
	 *
	 * M_TraceNeon's emissive is Colour x Glow, and the arena is dark enough that auto-exposure lifts
	 * whatever comes out of that. At 3.5 a purple's RED and BLUE channels both clip and the tonemapper
	 * hands back a white core with a PINK halo — which is not what was asked for. This was measured off
	 * the frames rather than argued about, by reading the ring pixels' hue out of the captures:
	 *
	 *   Glow 3.5  ->  hue clusters at 282 deg and 301 deg   (v23elle_green_portal_glow3.5.png) — pink
	 *   Glow 1.4  ->  282 / 290 deg, mean lightness 0.79    — purple, washed out
	 *   Glow 1.0  ->  280 / 290 deg, mean lightness 0.72    (v23elle_green_portal.png) — SHIPPED
	 *
	 * 280-290 deg is squarely purple; 300 is magenta. Lower still would keep saturating and start
	 * losing the ring against the arena's own neon at range, which is the other half of the job.
	 */
	constexpr float RingGlow = 1.0f;

	/**
	 * ...and what a LONE mouth glows at. Below 1 it cannot clear the bloom threshold, which is the
	 * point: see the colour note below for why "is this a portal yet?" is answered in brightness.
	 */
	constexpr float UnpairedGlow = 0.6f;

	// =============================================================================================
	// DEMO 20 ITEM 4 — "Make a circle placeholder portal, glowing purple."
	//
	// PURPLE IS THE ASK, so both mouths are purple; the cyan/magenta pair this shipped with is gone.
	// What the colours still have to answer is WHICH TWO ARE PAIRED, and the two questions a player
	// asks are put on DIFFERENT CHANNELS so bloom cannot swallow both at once:
	//
	//   "is this a portal yet?"  -> BRIGHTNESS. A lone mouth is dim, desaturated and does not bloom;
	//                               a paired one blazes. The two blazing circles are the pair.
	//   "which end am I at?"     -> HUE, inside the purple band. The first mouth is blue-violet, the
	//                               second red-violet. Both read as purple at a glance; side by side
	//                               they are plainly not the same mouth.
	//
	// *** THE PLACEHOLDER'S KNOWN LIMIT, STATED RATHER THAN DESIGNED AROUND. *** Two Elles with two
	// live pairs would show four bright purple circles, and nothing here says which two belong
	// together. Ranking pairs by hue would fix that and it is deliberately not done: the ask was a
	// placeholder, and one Elle per team is the case the request is about.
	//
	// These are file constants rather than settings properties on purpose: UTraceSettings belongs to
	// one agent this pass, Elle's eleven knobs are already on it, and a twelfth added from here would
	// be the merge conflict that loses somebody else's.
	// =============================================================================================

	// BLUE MUST BEAT RED IN THE FINAL PIXEL OR IT IS NOT PURPLE, IT IS PINK, and GREEN must stay near
	// zero or a clipped magenta turns white. These are multiplied by RingGlow (1.0) and then by the
	// scene's auto-exposure, which is what does the clipping in a dark arena — so the ratio between
	// the channels is the only thing under this file's control, and it is the thing that survives.
	// Measured on the shipped frame: 280 deg for the first mouth, 290 deg for the second.
	const FLinearColor FirstMouthColor(0.22f, 0.025f, 0.90f, 1.f);   // blue-violet — the FIRST mouth
	const FLinearColor SecondMouthColor(0.35f, 0.025f, 0.90f, 1.f);  // red-violet — the SECOND mouth

	/** A lone, unpaired gate is dimmer and greyer: it is not a portal yet and must not look like one. */
	const FLinearColor UnpairedColor(0.26f, 0.16f, 0.36f, 1.f);

	/**
	 * Vertical lift applied to a teleport destination, uu.
	 *
	 * The mouth is stored at the CAPSULE CENTRE of the pawn that placed it, so arriving at exactly the
	 * mouth puts a same-sized pawn exactly where Elle stood — which is right. The small lift exists
	 * only so that a gate placed on a slope, or by a crouched pawn, does not deposit the arrival
	 * inside the floor and have TeleportTo refuse the whole move.
	 */
	constexpr float ArrivalLiftUU = 12.f;
}

namespace TraceElleGateFile
{
	// THE TALLY STORAGE LIVES IN THE FILE-NAMED NAMESPACE, NOT AN ANONYMOUS ONE.
	//
	// TraceElle is a SHARED namespace — it is declared in the header, so any other .cpp may open it.
	// A `namespace TraceElle { namespace { ... } }` here would put these two objects at
	// TraceElle::<anonymous>::G*, and the day a second file opens TraceElle with its own anonymous
	// block the unity build concatenates both into one namespace and MSVC rejects the redefinition.
	// That is this project's C2084 lesson (see the guard in Scripts/build.sh), and the fix it names is
	// to name the namespace after its file rather than to rename the symbol.
	TraceElle::FGateTally GCarrierTally;
	TraceElle::FGateTally GOtherTally;
}

namespace TraceElle
{
	FGateTally& CarrierTally() { return TraceElleGateFile::GCarrierTally; }
	FGateTally& OtherTally()   { return TraceElleGateFile::GOtherTally; }

	void ResetTallies()
	{
		TraceElleGateFile::GCarrierTally.Reset();
		TraceElleGateFile::GOtherTally.Reset();
	}
}

// =================================================================================================
// Construction
// =================================================================================================

ATraceElleGate::ATraceElleGate()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	bReplicates = true;
	SetReplicateMovement(false);      // it never moves; replicating a static transform is pure cost
	bAlwaysRelevant = true;           // both teams may use it, so it must not be culled off anybody
	SetNetUpdateFrequency(10.f);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	// NO COLLISION OF ANY KIND, exactly like the Ripple and the Spike. A gate is a place. A collider
	// here would let it stop a bullet, break an Oyster jar or become a movement base — three bugs for
	// nothing gained, because entry is a proximity poll rather than an overlap.
	RingMesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("RingMesh"));
	RingMesh->SetupAttachment(Root);
	RingMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RingMesh->SetCollisionProfileName(TEXT("NoCollision"));
	RingMesh->SetGenerateOverlapEvents(false);
	RingMesh->SetCanEverAffectNavigation(false);
	RingMesh->SetCastShadow(false);
	RingMesh->bReceivesDecals = false;

	// /Engine/BasicShapes ships with every install; M_TraceNeon is generated content and may not.
	// Both lookups are static so the cost is paid once per process, exactly as the Ripple does it.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		BeadMesh = CylinderFinder.Object;
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

void ATraceElleGate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceElleGate, MouthLocation);
	DOREPLIFETIME(ATraceElleGate, RadiusUU);
	DOREPLIFETIME(ATraceElleGate, ExpireMatchTime);
	DOREPLIFETIME(ATraceElleGate, Partner);
	DOREPLIFETIME(ATraceElleGate, SourcePlayerState);
	DOREPLIFETIME(ATraceElleGate, SourceTeam);
	DOREPLIFETIME(ATraceElleGate, bSecondOfPair);
}

void ATraceElleGate::InitialiseGate(APlayerState* InSource, ETraceTeam InSourceTeam, const FVector& InMouth,
                                    float InRadiusUU, float InExpireMatchTime, bool bInSecondOfPair)
{
	SourcePlayerState = InSource;
	SourceTeam        = InSourceTeam;
	MouthLocation     = InMouth;
	RadiusUU          = FMath::Max(0.f, InRadiusUU);
	ExpireMatchTime   = InExpireMatchTime;
	bSecondOfPair     = bInSecondOfPair;

	SetActorLocation(InMouth);
	SetActorRotation(FRotator::ZeroRotator);
}

void ATraceElleGate::PairWith(ATraceElleGate* Other, float InPairExpireMatchTime)
{
	if (Other == nullptr || Other == this)
	{
		return;
	}

	Partner = Other;
	Other->Partner = this;

	// §2: "with both placed, both expire after 8 s". BOTH deadlines are rewritten from the moment the
	// pair completes, so the first gate does not quietly outlive or predecease the second by however
	// long Elle took to press the second time.
	ExpireMatchTime = InPairExpireMatchTime;
	Other->ExpireMatchTime = InPairExpireMatchTime;

	// SEED THE LOCKOUT FOR EVERYBODY ALREADY STANDING IN EITHER MOUTH.
	//
	// This is not defensive tidying, it is the difference between an ability and a joke: Elle is BY
	// CONSTRUCTION stood inside the second gate at the instant she places it ("places a portal gate
	// where she stands"), so without this the pair's very first act is to fling its author back to
	// the first gate. The rule that falls out is a good one and is the one a player will infer — you
	// have to STEP IN, and being handed a gate under your feet does not count.
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr != nullptr)
	{
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || !Candidate->IsAlive())
			{
				continue;
			}
			if (IsInsideMouth(Candidate))
			{
				ApplyLockout(Candidate);
				MarkAlreadyInside(Candidate);
			}
			if (Other->IsInsideMouth(Candidate))
			{
				ApplyLockout(Candidate);
				Other->MarkAlreadyInside(Candidate);
			}
		}
	}
}

void ATraceElleGate::BeginPlay()
{
	Super::BeginPlay();

	// On the server everything is already set by InitialiseGate. On a client the replicated properties
	// may not have landed yet, so the rings are built from Tick instead — BuildRingsIfNeeded is
	// idempotent and cheap until the mouth arrives.
	BuildRingsIfNeeded();
}

float ATraceElleGate::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceElleGate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	BuildRingsIfNeeded();

	if (HasAuthority())
	{
		ServerTickGate();
	}
}

void ATraceElleGate::ServerTickGate()
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	const float Now = MatchTimeNow();

	// EXPIRY IS TESTED FIRST, before anything that could early-return. §2's two lifetimes ("the first
	// expires" / "both expire after 8 s") are the one behaviour that must not depend on the gate being
	// well formed — a gate that somehow reached the world with a zero radius would otherwise take the
	// guard below every tick and live forever.
	if (ExpireMatchTime > 0.f && Now >= ExpireMatchTime)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Elle] gate expired at match time %.2f (paired=%d)."),
			Now, IsPaired() ? 1 : 0);
		Destroy();
		return;
	}

	// A LONE GATE MOVES NOBODY. That is exactly §2's "if no second gate inside 4 s the first expires":
	// for those 4 s the gate is a marker, not a portal, and there is nowhere to send anyone.
	//
	// IsValid, not != nullptr, and the difference is one frame wide but real: Destroy() only MARKS an
	// actor as garbage, and a UPROPERTY reference to it is not cleared until the next collection. The
	// pair normally dies together on one shared deadline and each gate's expiry test above fires
	// first, so this is not reachable today — but "not reachable today" is how a destination inside a
	// destroyed mouth becomes somebody's teleport-into-the-void bug after the next change.
	if (!IsValid(Partner) || RadiusUU <= 0.f)
	{
		return;
	}

	PruneLockouts();

	// THE EDGE IS COLLECTED FIRST AND THE SET IS REBUILT FROM SCRATCH, so a player who dies, is
	// destroyed, or simply walks away cannot leave a stale "already inside" entry that suppresses their
	// next real entry. It is rebuilt every server tick from the same IsInsideMouth() the decision uses,
	// which is what keeps the two from ever disagreeing.
	TSet<TWeakObjectPtr<ATraceCharacter>> InsideNow;

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive() || !IsInsideMouth(Candidate))
		{
			continue;
		}

		InsideNow.Add(Candidate);

		// *** DEMO 17 item 1: A GATE TAKES YOU WHEN YOU STEP IN, NOT WHILE YOU STAND IN IT. ***
		//
		// Without this the pair is a hazard rather than an ability: a stationary player inside either
		// mouth is thrown to the other one every time the re-entry lockout lapses, for the pair's whole
		// 8 s — seven teleports in seven seconds, measured, on an Elle standing in the mouth she had
		// just placed. See InsideLastLook. Trace.Elle.SnapStepIn 0 restores the old poll.
		if (CVarElleSnapStepIn.GetValueOnAnyThread() != 0 && InsideLastLook.Contains(Candidate))
		{
			continue;
		}

		// THE SUBJECT'S CARRIER STATUS IS SAMPLED ONCE, HERE, and every tally below is filed against
		// it. Asking twice would let a pass completing between two lines file the decision under the
		// wrong heading, which is precisely the sort of drift that makes a carrier harness lie.
		const bool bSubjectIsCarrier = UTraceAbilityComponent::IsCarrier(Candidate);
		TraceElle::FGateTally& Tally = bSubjectIsCarrier ? TraceElle::CarrierTally() : TraceElle::OtherTally();

		if (IsLockedOut(Candidate))
		{
			++Tally.RefusedByLockout;
			continue;
		}

		const ETraceAbilityEffect Effect = ClassifyEntry(Candidate);
		const UTraceSettings& Settings = UTraceSettings::Get();

		// THE §2 [ASSUMPTION], AND THE ONE TICK BOX THAT REVERSES IT. Off, Elle's gates are her team's
		// gates and an enemy walks through the mouth without being taken. This is tested BEFORE the
		// choke point on purpose: "your team may not use this" is a rule about the ability, and
		// routing it through CanAffect would express it as a team-damage rule, which it is not.
		if (Effect == ETraceAbilityEffect::Control && !Settings.bElleSnapUsableByBothTeams)
		{
			++Tally.RefusedByTeamKnob;
			continue;
		}

		// *** THE CHOKE POINT. SPEC v18 §2: "a Core carrier must not be teleported by an enemy gate."
		//
		// There is no carrier test here and there must not be one. An enemy entry is classified
		// Control (see ClassifyEntry), and Control on a carrier is what
		// UTraceAbilityComponent::CanAffectTargetDetailed refuses — the same single function that
		// refuses Chut's bash and Oyster's pull. A friendly entry is Beneficial, which passes for a
		// carrier exactly as spec v14 §6 lets a carrier ride Rocco's Ripple. ***
		if (!UTraceAbilityComponent::CanAffect(SourcePlayerState, Candidate, Effect))
		{
			++Tally.RefusedByChokePoint;
			continue;
		}

		// THE VOLUNTARY HALF, AND ONLY THE VOLUNTARY HALF. Being MOVED by an enemy gate was settled
		// above and has no knob. What a designer gets to decide is whether a carrier may deliberately
		// take their OWN team's gate — a portal-assisted carry — and that is what this is.
		if (bSubjectIsCarrier && Effect == ETraceAbilityEffect::Beneficial
			&& !Settings.bElleSnapCarrierMayUseGate)
		{
			++Tally.RefusedByCarrierKnob;
			continue;
		}

		if (CVarElleSnapTeleportEnabled.GetValueOnAnyThread() == 0)
		{
			// RED ARM: every rule above ran and allowed it; only the move is withheld. Nothing is
			// tallied, so the harness sees a teleport that did not happen rather than one that was
			// refused — which is the distinction the arm exists to make.
			continue;
		}

		++Tally.TeleportsCommitted;
		if (Effect == ETraceAbilityEffect::Control)
		{
			// The ENEMY-gate half, tallied separately because that is the half the founding invariant
			// is about. A Beneficial (own-team) entry is allowed for a carrier by design and must not
			// read as a displacement — see FGateTally::TeleportsCommittedAsControl.
			++Tally.TeleportsCommittedAsControl;
		}
		CommitTeleport(Candidate);

		// The arrival is at the PARTNER's mouth, so the subject is no longer in this one. Dropping them
		// here means a player who walks straight back gets a fresh edge rather than being suppressed by
		// a stale record of a mouth they are not standing in any more.
		InsideNow.Remove(Candidate);
	}

	InsideLastLook = MoveTemp(InsideNow);
}

void ATraceElleGate::CommitTeleport(ATraceCharacter* Candidate)
{
	// Re-tested here as well as at the call site: this is the function that actually moves a player,
	// and it must not depend on a caller having checked. See the IsValid note in ServerTickGate.
	if (Candidate == nullptr || !IsValid(Partner))
	{
		return;
	}

	const FVector Destination = Partner->GetMouthLocation() + FVector(0.f, 0.f, TraceElleGateFile::ArrivalLiftUU);

	// ROTATION IS THE PLAYER'S OWN. A portal that also spins you is a portal you cannot aim out of,
	// and control rotation is client-authoritative anyway — turning the pawn here would be corrected
	// away by the next client update and would read as a stutter rather than as a turn.
	//
	// VELOCITY IS DELIBERATELY NOT TOUCHED. TeleportTo preserves it, so a player who sprints into a
	// gate sprints out of it. Zeroing it would make the gate a movement PENALTY for the fast players
	// the rest of this game's movement is built to reward.
	const bool bMoved = Candidate->TeleportTo(Destination, Candidate->GetActorRotation());

	if (!bMoved)
	{
		// Refused by the engine's own encroachment test — the far mouth is inside geometry, or another
		// pawn is standing exactly in it. Say so: a silent failure here reads as "the portal randomly
		// does nothing", which is unresearchable from a log that never mentions it.
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Elle] gate REFUSED by geometry: %s could not be placed at (%s). The far mouth is blocked."),
			*GetNameSafe(Candidate), *Destination.ToCompactString());
		return;
	}

	ApplyLockout(Candidate);

	// THE ARRIVAL IS NOT AN ENTRY. They were PUT in the far mouth; they did not step into it. Without
	// this the far gate sees a body appear inside itself and, one lockout later, sends it straight back
	// — which is the ping-pong the edge rule exists to end.
	Partner->MarkAlreadyInside(Candidate);

	UE_LOG(LogTraceGame, Log,
		TEXT("[Elle] SNAP teleport: %s (team %d, carrier=%d) taken by %s's gate as %s -> (%s)."),
		*GetNameSafe(Candidate), static_cast<int32>(Candidate->GetTeam()),
		UTraceAbilityComponent::IsCarrier(Candidate) ? 1 : 0,
		*GetNameSafe(SourcePlayerState), TraceAbilityEffectToString(ClassifyEntry(Candidate)),
		*Destination.ToCompactString());
}

// =================================================================================================
// The rules, as pure queries — so the harness can print them instead of inferring them
// =================================================================================================

bool ATraceElleGate::IsInsideMouth(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr || RadiusUU <= 0.f)
	{
		return false;
	}
	return FVector::DistSquared(Candidate->GetActorLocation(), FVector(MouthLocation))
		<= (RadiusUU * RadiusUU);
}

int32 ATraceElleGate::GetDrawnBeadCount() const
{
	// *** BUILT IS NOT DRAWN, AND THAT DISTINCTION IS THE WHOLE OF DEMO 20 ITEM 4. ***
	//
	// This used to return the raw instance count, and an InstancedStaticMeshComponent with NO MESH
	// accepts every AddInstance and hands all sixty of them back. So the one number the whole project
	// used to answer "can the player see the gate?" read 60 on a build where the component never got a
	// scene proxy at all — and Trace.Elle.SnapPressTest duly reported "...and it is actually DRAWN on
	// this machine (60 ring beads), so there is something on screen to walk into" while the user was
	// looking at bare floor. A counter that cannot go red is the failure this project keeps paying for.
	//
	// Both halves are now required: instances AND a mesh for the renderer to draw them with. Under
	// Trace.Elle.GateVisible 0 this returns 0 and that assertion fails, which is what makes the fix
	// measurable rather than merely asserted.
	if (RingMesh == nullptr || RingMesh->GetStaticMesh() == nullptr)
	{
		return 0;
	}
	return RingMesh->GetInstanceCount();
}

ETraceAbilityEffect ATraceElleGate::ClassifyEntry(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr)
	{
		return ETraceAbilityEffect::Control;   // the strict answer for a question with no subject
	}

	// SourceTeam is None only when Elle placed the gate before the game mode balanced her onto a team,
	// which is not reachable in a live match. Treating it as "everybody is friendly" would make the
	// carrier rule unreachable in exactly that state, so it resolves the other way.
	const ETraceTeam TheirTeam = Candidate->GetTeam();
	const bool bFriendly = (SourceTeam != ETraceTeam::None) && (TheirTeam == SourceTeam);

	return bFriendly ? ETraceAbilityEffect::Beneficial : ETraceAbilityEffect::Control;
}

const TCHAR* ATraceElleGate::DescribeEntryRefusal(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr)
	{
		return TEXT("<no candidate>");
	}
	if (IsLockedOut(Candidate))
	{
		return TEXT("ReEntryLockout");
	}

	const ETraceAbilityEffect Effect = ClassifyEntry(Candidate);
	if (Effect == ETraceAbilityEffect::Control && !UTraceSettings::Get().bElleSnapUsableByBothTeams)
	{
		return TEXT("EnemyTeam (bElleSnapUsableByBothTeams off)");
	}

	if (const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(SourcePlayerState))
	{
		const ETraceAbilityBlockReason Reason = Comp->CanAffectTargetDetailed(Candidate, Effect);
		if (Reason != ETraceAbilityBlockReason::Allowed)
		{
			return TraceAbilityBlockReasonToString(Reason);
		}
	}

	if (UTraceAbilityComponent::IsCarrier(Candidate) && Effect == ETraceAbilityEffect::Beneficial
		&& !UTraceSettings::Get().bElleSnapCarrierMayUseGate)
	{
		return TEXT("FriendlyCarrier (bElleSnapCarrierMayUseGate off)");
	}

	return TEXT("Allowed");
}

// =================================================================================================
// The re-entry lockout
// =================================================================================================

bool ATraceElleGate::IsLockedOut(const ATraceCharacter* Candidate) const
{
	const float* Until = LockoutUntilMatchTime.Find(TWeakObjectPtr<ATraceCharacter>(const_cast<ATraceCharacter*>(Candidate)));
	return (Until != nullptr) && (MatchTimeNow() < *Until);
}

void ATraceElleGate::ApplyLockout(ATraceCharacter* Candidate)
{
	if (Candidate == nullptr)
	{
		return;
	}

	const float Until = MatchTimeNow() + FMath::Max(0.f, UTraceSettings::Get().ElleSnapTeleportLockoutSeconds);

	// BOTH ENDS, ALWAYS. A lockout the arrival mouth does not know about is not a lockout: the far
	// gate would take the player straight back on the very next tick, which is the trap the knob's
	// own comment names as its reason to exist.
	LockoutUntilMatchTime.Add(Candidate, Until);
	if (Partner != nullptr)
	{
		Partner->LockoutUntilMatchTime.Add(Candidate, Until);
	}
}

void ATraceElleGate::PruneLockouts()
{
	for (auto Iterator = LockoutUntilMatchTime.CreateIterator(); Iterator; ++Iterator)
	{
		if (!Iterator.Key().IsValid())
		{
			Iterator.RemoveCurrent();
		}
	}

	// InsideLastLook needs no pruning: ServerTickGate rebuilds it from scratch out of the pawns it can
	// actually see every tick, so a dead or destroyed one falls out on its own.
}

void ATraceElleGate::MarkAlreadyInside(ATraceCharacter* Candidate)
{
	if (Candidate != nullptr)
	{
		InsideLastLook.Add(Candidate);
	}
}

// =================================================================================================
// The rings
// =================================================================================================

void ATraceElleGate::BuildRingsIfNeeded()
{
	// Rebuilt on exactly one transition — lone to paired — because that changes the colour, and the
	// colour is the only thing telling a player whether this mouth will move them.
	const bool bPairedNow = IsPaired();
	if (bRingsBuilt && bBuiltAsPaired == bPairedNow)
	{
		return;
	}
	if (RadiusUU <= 0.f || BeadMesh == nullptr || RingMesh == nullptr)
	{
		return;   // on a client, until the replicated mouth arrives
	}

	// Shaders are not cooked for server targets, so a dedicated server builds nothing at all.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bRingsBuilt = true;
		bBuiltAsPaired = bPairedNow;
		return;
	}

	const bool bFirstBuild = !bRingsBuilt;
	bRingsBuilt = true;
	bBuiltAsPaired = bPairedNow;

	// *** DEMO 20 ITEM 4: THE ONE LINE THAT MADE THE PORTAL INVISIBLE. ***
	//
	// Without a static mesh an InstancedStaticMeshComponent still accepts, stores and counts every
	// instance below — it simply never gets a scene proxy (UStaticMeshComponent::CreateSceneProxy
	// returns nullptr when GetStaticMesh() is null), so the renderer is never told the gate exists.
	// Every other line of this function ran correctly on the build the user reported: the beads were
	// placed, the colour was set, the log said "gate rings built". It is set BEFORE the instances are
	// added, the order ATraceRippleActor::BuildRingsIfNeeded uses for the same bead-ring pattern.
	if (CVarElleGateVisible.GetValueOnGameThread() != 0)
	{
		RingMesh->SetStaticMesh(BeadMesh);
	}

	const FLinearColor Color = !bPairedNow
		? TraceElleGateFile::UnpairedColor
		: (bSecondOfPair ? TraceElleGateFile::SecondMouthColor : TraceElleGateFile::FirstMouthColor);

	if (RingMID == nullptr)
	{
		UMaterialInterface* Parent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get();
		if (Parent != nullptr)
		{
			RingMID = UMaterialInstanceDynamic::Create(Parent, this);
			if (RingMID != nullptr)
			{
				RingMesh->SetMaterial(0, RingMID);
			}
		}
	}

	if (RingMID != nullptr)
	{
		// "Color" is what both M_TraceNeon and BasicShapeMaterial call it; "Glow" is neon only and is a
		// silent no-op on the fallback, which is why the fallback also gets a matte roughness. It will
		// not bloom, and that is the cost of not having generated the content pack.
		RingMID->SetVectorParameterValue(TEXT("Color"), Color);
		RingMID->SetVectorParameterValue(TEXT("BaseColor"), Color);
		RingMID->SetScalarParameterValue(TEXT("Glow"),
			bPairedNow ? TraceElleGateFile::RingGlow : TraceElleGateFile::UnpairedGlow);
		RingMID->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
	}

	if (!bFirstBuild)
	{
		return;   // geometry is unchanged; only the colour moved
	}

	for (int32 Index = 0; Index < TraceElleGateFile::RingCount; ++Index)
	{
		const float Alpha = (TraceElleGateFile::RingCount > 1)
			? static_cast<float>(Index) / static_cast<float>(TraceElleGateFile::RingCount - 1)
			: 0.f;

		// The mouth is stored at the placer's capsule CENTRE, so the column is hung around it rather
		// than stacked up from it: half a pawn down to the ankles, half a pawn up to the head.
		const float ZOffset = (Alpha - 0.5f) * TraceElleGateFile::ColumnHeightUU;
		AddRing(FVector(MouthLocation) + FVector(0.f, 0.f, ZOffset), RadiusUU);
	}

	// Display, not Verbose, and it says whether there is a MESH. "Rings built" was printed on every
	// invisible build there has ever been, so the log line that was supposed to be the evidence was
	// the same on the broken build as on the fixed one. Beads drawn is the honest number.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Elle] gate rings built: %d rings x %d beads at radius %.0f uu, colour %s, glow %.1f "
		     "(paired=%d, second=%d) — mesh=%s, beads DRAWN=%d."),
		TraceElleGateFile::RingCount, TraceElleGateFile::BeadsPerRing, RadiusUU, *Color.ToString(),
		bPairedNow ? TraceElleGateFile::RingGlow : TraceElleGateFile::UnpairedGlow,
		bPairedNow ? 1 : 0, bSecondOfPair ? 1 : 0,
		*GetNameSafe(RingMesh->GetStaticMesh()), GetDrawnBeadCount());
}

void ATraceElleGate::AddRing(const FVector& Center, float Radius) const
{
	if (RingMesh == nullptr || Radius <= 0.f)
	{
		return;
	}

	// Each bead is a cylinder lying along the ring's tangent, so a ring of them reads as a torus
	// rather than a necklace. 1.2 overlaps them slightly and hides the seams. Straight from the
	// Ripple's AddRing, which is the shape this project has already tuned by eye.
	const float ArcLength = (2.f * PI * Radius / static_cast<float>(TraceElleGateFile::BeadsPerRing)) * 1.2f;
	const FVector BeadScale(TraceElleGateFile::BeadThicknessUU / 100.f,
	                        TraceElleGateFile::BeadThicknessUU / 100.f,
	                        ArcLength / 100.f);

	const FVector CenterRelative = Center - GetActorLocation();

	for (int32 Bead = 0; Bead < TraceElleGateFile::BeadsPerRing; ++Bead)
	{
		const float Angle = (2.f * PI * static_cast<float>(Bead)) / static_cast<float>(TraceElleGateFile::BeadsPerRing);
		const float SinA = FMath::Sin(Angle);
		const float CosA = FMath::Cos(Angle);

		const FVector Offset(CosA * Radius, SinA * Radius, 0.f);
		const FVector Tangent(-SinA, CosA, 0.f);

		const FTransform BeadTransform(FRotationMatrix::MakeFromZ(Tangent).Rotator(),
		                               CenterRelative + Offset, BeadScale);
		RingMesh->AddInstance(BeadTransform);
	}
}
