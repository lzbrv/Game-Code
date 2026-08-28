// Trace — Rocco's Ripple, spec v14 §6.
//
// ===================================================================================================
// WHAT §6 ACTUALLY ASKS FOR, CLAUSE BY CLAUSE, AND WHERE EACH ONE LIVES
// ===================================================================================================
//
//   "dash in any direction on a SEPARATE cooldown from the standard dash"
//        The direction is composed by the SAME pure function the standard dash uses
//        (UTraceCharacterMovementComponent::ComputeDashDirection), so "any direction" means exactly
//        what it means for a dash — W/S carry the aim pitch, so straight up and diagonals work.
//        The cooldown is the ability framework's activated cooldown (RoccoRippleCooldownSeconds),
//        which is a different number in a different place from the movement component's dash charge
//        pool. Firing the Ripple does not spend a dash charge and never touches DashTimeRemaining.
//
//   "leaving a ripple behind... any character, EITHER TEAM, may enter the ripple's start and be
//    propelled along the dash's path (including up or diagonally). THE CORE CARRIER CAN USE IT."
//        This actor is that ripple. Entry is asked of the choke point as
//        ETraceAbilityEffect::Beneficial, which is the one effect class spec §4 lets through for a
//        carrier — and it lets teammates and enemies through too. There is no team test here and
//        there must not be one: a second team rule would be a second answer to a question §4 says
//        has exactly one.
//
//   "Players holding guns — Rocco included — CAN SHOOT while riding it, unlike a normal dash."
//        This is why the ride is NOT implemented as a dash. UTraceCharacterMovementComponent::
//        AreWeaponActionsBlocked() is a thin alias of IsDashing(), so anything that went through
//        StartDash() would be silenced for its whole window. The ride writes Velocity directly and
//        never raises the dash flag, so the weapon gate stays open by construction rather than by a
//        special case somebody has to remember. TraceCharacterVerify asserts exactly that.
//
//   "Lasts 4 s, then all effects and visuals vanish."
//        One absolute match-clock deadline (ExpireMatchTime) governs the rides, the rings and the
//        actor's own lifetime. The server destroys the actor at the deadline; the visuals go with it.
//
//   "a short series of rings along the path, with the STARTING RING IN A DIFFERENT COLOUR"
//        Two instanced-static-mesh components, one per colour, so the start ring is a different
//        material instance rather than a per-instance tint that the basic-shape fallback could not
//        honour. RoccoRippleStartRingColor / RoccoRippleTrailRingColor are the knobs.
//
//        DEMO 13 IS THE CANON HERE and FX_AUDIO_PLAN §2.9 spends it: the start ring is ROCCO AMBER
//        (his accent — an ability's world actor wears its owner's, ART_BIBLE §6.2) and the trail is
//        NEUTRAL PALE, the arena's own neon. The bible blesses exactly this split by name. Both
//        knobs moved in this pass; what did NOT move is the ring Glow, 3.5, which is the bible's
//        own §3.2 ladder entry for "ripple rings" in the T2 wayfinding band.
//
// ===================================================================================================
// WHAT FX_AUDIO_PLAN §2.9 ADDED, AND THE ONE PLACE ITS WIRING INSTRUCTION COULD NOT BE FOLLOWED
// ===================================================================================================
//
//   START-RING PULSE   0.8 Hz, ±15%. THE ONE PERMITTED GAMEPLAY PULSE in the whole bible (§3.3):
//                      everything else that breathes is scenery, and nothing that is LETHAL breathes
//                      at all. This one is allowed because it says "take me here" about an entrance,
//                      and it is faster than any of the world's pulses so it cannot be mistaken for
//                      one. Ridden on M_TraceNeon's PulseAmp/PulseRate parameters when the material
//                      has them, and on a per-frame Glow write when it does not (§2.9 asks for both,
//                      in that order).
//   EXPIRY DISSOLVE    the rings fade to nothing over the last 0.3 s instead of vanishing at the
//                      deadline. Bible §6.4: an effect never pops out. It is COSMETIC ONLY — the
//                      ride, the entry radius and the destroy are all still governed by the one
//                      ExpireMatchTime, and a rider who steps in during the fade gets a full ride.
//   RIDE FX + LOOP     three amber speed lines trailing every rider, and TraceSoundEvents::
//                      RoccoRideLoop attached to him, on EVERY machine.
//
// *** THE RIDE FX ARE NOT ON THE §1.2 ROUTER, AND HERE IS WHY. *** §2.9's hook column says "router
// edge on the riding flag". There is no such flag and there cannot usefully be one: the router is
// driven by FTraceAbilityNetState, which belongs to ONE player's ability set, and the rider of a
// ripple is very often somebody else — §6 says "any character, EITHER TEAM". Rocco's state can say
// "my ripple is alive"; it cannot say "Slimeball is on it". Putting a riding bit in the RIDER's
// state instead would mean every one of the ten kits carrying a bit about Rocco's ability.
//
// So the FX live HERE, on the replicated actor that already exists on every machine and already
// knows the path — and each machine works out who is riding from motion it has already received
// (see UpdateRideFx). That is the same "derive it, do not replicate it" shape ATraceRoxieRocket uses
// for its trail, it costs no bandwidth, and it is correct for a simulated proxy, which is exactly
// the case a router edge on Rocco's state would have got wrong.
//
// ===================================================================================================
// THE PREDICTION MODEL, STATED PLAINLY BECAUSE IT IS NOT THE USUAL ONE
// ===================================================================================================
//
// The ride is a per-frame Velocity write, and it runs on TWO kinds of machine:
//
//     the SERVER            for every pawn — authoritative.
//     each CLIENT           for its OWN locally-controlled pawn only — prediction.
//
// Both machines drive the same code from the same replicated inputs (start, direction, length, ride
// speed, deadline), so the local rider's screen and the server agree without a correction on every
// frame. A simulated proxy is left entirely alone: its movement arrives by replication, exactly as
// it does for every other kind of motion in this game.
//
// This deliberately does NOT go through the movement component's saved-move pipeline. Adding a ride
// flag to FSavedMove_Trace is the "right" long-term answer and it is a movement-slice change; it is
// named as a cross-file need in the report rather than smuggled in here.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"     // FVector_NetQuantize
#include "UObject/ObjectPtr.h"

#include "Gameplay/TraceFxShapes.h"      // ETraceFxBlend — stored per piece, so it must be complete

#include "TraceRippleActor.generated.h"

class APlayerState;
class ATraceCharacter;
class UAudioComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class USceneComponent;

/**
 * One live ripple: a straight path through the world that anybody may ride once, plus its rings.
 *
 * Server-spawned and replicated. Rocco's ability set owns exactly one at a time and destroys it at
 * half time; the actor destroys itself at its deadline.
 */
UCLASS()
class TRACE_API ATraceRippleActor : public AActor
{
	GENERATED_BODY()

public:
	ATraceRippleActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * AUTHORITY ONLY. Call immediately after SpawnActor, before anybody can tick.
	 *
	 * @param InSource        Rocco's PlayerState — the instigator the choke point is asked about.
	 * @param InStart         world location of the START ring (the entrance).
	 * @param InDirection     unit direction of the dash's path. Fully 3D.
	 * @param InPathLength    how far the path runs, uu.
	 * @param InRideSpeed     uu/s a rider is propelled at.
	 * @param InEntryRadius   how close to the start a player must be to be picked up.
	 * @param InExpireMatchTime  absolute match-clock time the whole thing vanishes.
	 */
	void InitialiseRipple(APlayerState* InSource, const FVector& InStart, const FVector& InDirection,
	                      float InPathLength, float InRideSpeed, float InEntryRadius, float InExpireMatchTime);

	// --- queries, for the HUD and for the verification harness ------------------------------------

	FVector GetRippleStart() const { return FVector(RippleStart); }
	FVector GetRippleDirection() const { return FVector(RippleDirection); }
	float   GetPathLength() const { return PathLength; }
	float   GetRideSpeed() const { return RideSpeed; }
	float   GetEntryRadius() const { return EntryRadius; }
	float   GetExpireMatchTime() const { return ExpireMatchTime; }

	/** True while @p Candidate is being propelled by this ripple on THIS machine. */
	bool IsRiding(const ATraceCharacter* Candidate) const;

	/** How many pawns this machine is currently propelling. */
	int32 GetRiderCount() const { return Riders.Num(); }

	/** How many pawns have ridden this ripple on this machine, ever. One ride each, by design. */
	int32 GetLifetimeRiderCount() const { return LifetimeRiders.Num(); }

	/** The reason the last entry attempt for @p Candidate was refused, for the harness's logs. */
	static const TCHAR* DescribeEntryRefusal(const ATraceCharacter* Candidate, const APlayerState* Source);

	// --- FX_AUDIO_PLAN §2.9 queries, for Trace.Rocco.FxTest and for anybody debugging a dark ripple -

	/** "start=Emissive trail=Emissive ride=Additive" — the achieved blends, for the log. */
	FString DescribeBlends() const;

	/** How many ring beads are registered for drawing, both components together. ZERO MEANS INVISIBLE. */
	int32 GetDrawnBeadCount() const;

	/** How many pawns this machine is currently drawing ride FX for. May exceed GetRiderCount() on a proxy. */
	int32 GetPresentedRiderCount() const;

	/**
	 * The most pawns this machine has EVER presented at once, for the whole life of this ripple.
	 *
	 * *** A PEAK AND NOT A SNAPSHOT, BECAUSE A RIDE IS SHORTER THAN A HARNESS'S REACTION TIME. ***
	 * A 378 uu path at 1,190 uu/s is over in a third of a second; the first version of
	 * Trace.Rocco.FxTest sampled GetPresentedRiderCount() half a second after staging a rider and
	 * read zero for two riders who had come and gone — reporting a working feature as dead. This
	 * counter cannot be missed by sampling late.
	 */
	int32 GetPeakPresentedRiderCount() const { return PeakPresentedRiders; }

	/** True when the start ring's pulse is being carried by M_TraceNeon rather than by a per-frame write. */
	bool IsPulseInMaterial() const { return bPulseInMaterial; }

	/** 0..1 cosmetic fade applied to every ring right now. 1 until the last 0.3 s, then down to 0. */
	float GetDissolveAlpha() const;

protected:
	/** REPLICATED. The entrance. */
	UPROPERTY(Replicated)
	FVector_NetQuantize RippleStart = FVector::ZeroVector;

	/** REPLICATED. Unit direction of the path — 3D, so "up or diagonally" is not a special case. */
	UPROPERTY(Replicated)
	FVector_NetQuantize RippleDirection = FVector::ZeroVector;

	UPROPERTY(Replicated)
	float PathLength = 0.f;

	UPROPERTY(Replicated)
	float RideSpeed = 0.f;

	UPROPERTY(Replicated)
	float EntryRadius = 0.f;

	/** REPLICATED. Absolute match-clock deadline. See the cooldown contract on the ability component. */
	UPROPERTY(Replicated)
	float ExpireMatchTime = 0.f;

	/** REPLICATED. Rocco's PlayerState, so the choke point has an instigator even after he dies. */
	UPROPERTY(Replicated)
	TObjectPtr<APlayerState> SourcePlayerState = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root = nullptr;

	/** The START ring. Its own component so it can carry its own colour (§6's "different colour"). */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> StartRingMesh = nullptr;

	/** Every ring after the first. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> TrailRingMesh = nullptr;

	/**
	 * §2.9's ride FX: three amber speed lines per rider, all of them instances of ONE component.
	 *
	 * On the actor rather than attached to the rider, which is a budget decision as well as a
	 * lifetime one: §1.4 allows four attached loop primitives PER PAWN across every kit, and a ride
	 * that spent three of them would leave a Lily riding a ripple unable to draw her own flight aura.
	 * Instances here cost the rider nothing and cannot outlive their pawn, because they are not on it.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> RideFxMesh = nullptr;

private:
	/** One rider's progress along the path, on this machine. */
	struct FRippleRider
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;
		float DistanceTravelled = 0.f;
	};

	/** Who this machine is propelling right now. */
	TArray<FRippleRider> Riders;

	/** Who has already ridden. ONE RIDE PER PLAYER PER RIPPLE — see the [ASSUMPTION] in the .cpp. */
	TArray<TWeakObjectPtr<ATraceCharacter>> LifetimeRiders;

	/** One pawn this machine is DRAWING a ride for, and the loop sound attached to it. */
	struct FPresentedRider
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;

		/** TraceSoundEvents::RoccoRideLoop, started locally by StartLoopOn. Faded out on exit. */
		TWeakObjectPtr<UAudioComponent> Loop;
	};

	/** Server + local-prediction half of Tick. */
	void UpdateRides(float DeltaSeconds);

	/** True when this machine is responsible for simulating @p Candidate's movement. */
	bool ShouldSimulate(const ATraceCharacter* Candidate) const;

	/** Cosmetic half. Builds the rings once the replicated path has arrived. */
	void BuildRingsIfNeeded();

	/** Adds one ring of instances to @p Mesh, centred on @p Center, in the plane normal to the path. */
	void AddRing(UInstancedStaticMeshComponent* Mesh, const FVector& Center, const FVector& Direction) const;

	/**
	 * §2.9's ride FX + ride loop, on EVERY machine. Cosmetic only; writes no gameplay state.
	 *
	 * Works out who is visibly riding from information this machine already has — the replicated path
	 * and each pawn's own motion — rather than from a flag nobody replicates. See the header for why
	 * that is not a shortcut but the only correct answer for a rider who may be any character.
	 */
	void UpdateRideFx();

	/** True when @p Candidate's motion says it is being carried by THIS path right now. Pure. */
	bool LooksLikeRiding(const ATraceCharacter* Candidate) const;

	/** Fades out and forgets every ride loop. Called on expiry and on EndPlay. */
	void StopAllRideLoops();

	/** Pushes hue and brightness into one ring's MID, honouring its achieved blend. */
	void SetRingGlow(UMaterialInstanceDynamic* MID, ETraceFxBlend Blend, const FLinearColor& Color,
	                 float Intensity) const;

	/** The match clock. Identical to the ability component's. */
	float MatchTimeNow() const;

	bool bRingsBuilt = false;

	/** True once the start ring's PulseAmp/PulseRate were accepted by the material. See the header. */
	bool bPulseInMaterial = false;

	/** Who this machine is drawing speed lines and playing the ride loop for. */
	TArray<FPresentedRider> PresentedRiders;

	/** The high-water mark of the above. See GetPeakPresentedRiderCount. */
	int32 PeakPresentedRiders = 0;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> StartRingMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> TrailRingMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RideFxMID = nullptr;

	/** The achieved blends, one per piece. None means "hidden", never "grey". */
	ETraceFxBlend StartRingBlend = ETraceFxBlend::None;
	ETraceFxBlend TrailRingBlend = ETraceFxBlend::None;
	ETraceFxBlend RideFxBlend = ETraceFxBlend::None;
};
