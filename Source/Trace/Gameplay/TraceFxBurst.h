// Trace — ATraceFxBurst, the one generic server-authored transient. FX_AUDIO_PLAN §1.3.
//
// ===================================================================================================
// WHAT §1.3 ASKS FOR, AND WHY IT IS ONE CLASS RATHER THAN NINE
// ===================================================================================================
//
// Seven abilities need "a one-shot at a world point that every machine sees": Chut's bash impact,
// Mace's spike embed, Elle's teleport flash, Roxie's rocket burst, Mortimer's per-victim quake hit,
// Slimeball's splat and X's sting spark — plus Oyster's jar pop and a generic ring that Rocco's
// second jump and Oyster's pull link both want. Nine effects, one shape of problem:
//
//     the SERVER knows the fact  ->  every machine must draw it and hear it, frame-synced.
//
// A replicated actor IS that broadcast. Spawning one on the server puts it on every client with its
// spawn transform and its FTraceFxBurstSpec attached, so the visual and the sound need no RPC of
// their own: the actor's own replication is the multicast. That is why the sound rides
// TraceAudio::PlayReplicatedLocal (§1.6.3) and why every burst event is declared CLIENT-side in
// Audio/TraceSoundEvents.h — a stray TraceAudio::Play() on one of them can then never multicast a
// second copy on top of this one (the §8.7 double-audio rule).
//
// Nine separate actor classes would be nine copies of the same replication, the same degradation
// ladder, the same lifetime bookkeeping and the same nine chances to get one of them wrong. So the
// TYPE is data — a replicated enum — and the geometry is a switch. Adding a tenth burst is a row in
// the table in the .cpp and nothing else.
//
// ===================================================================================================
// THE FIVE RULES EVERY TYPE OBEYS (ART_BIBLE §3.2, §3.4, §6.2, §6.3, §6.4)
// ===================================================================================================
//
//  1. ONE HUE PER EFFECT, and it is the owner's accent (or the semantic hue where a status is being
//     shown — JarPop is Poisoned green, not Oyster cyan, because semantic beats accent). The table
//     in the .cpp is the whole list; no caller picks a colour except through Spec.Tint, which exists
//     for the ONE type that two different kits share (see FTraceFxBurstSpec::Tint).
//
//  2. GLOW <= 4.2 ON EVERY EMISSIVE PIECE. 7.5 is the ceiling of the world and only the two goal
//     rings sit there; an FX transient lives at or below the smear-head precedent, 4.2. The cap is
//     enforced in code (MaxTransientGlow), not merely written in a comment.
//
//  3. NO LETHAL-TELEGRAPH BRIGHTNESS PULSE. Every burst animation here is MONOTONIC: sizes grow,
//     brightness falls, nothing oscillates. A burst is over in less than half a second, so there is
//     no state for a pulse to communicate, and a blinking impact would be a lying one.
//
//  4. EVERY WORLD-READABLE EMISSIVE IS >= 8 uu WIDE. Sub-8 uu emissive is forbidden outside
//     first-person range because TSR dissolves it into dashes. Where §2's tables asked for a 3 uu
//     radius spark (6 uu across) this file draws 4 uu (8 uu across) — see MinEmissiveRadiusUU.
//
//  5. DRAWN == LETHAL. RocketBurst reads its radius from the LIVE settings knob the damage itself
//     uses, so the shell and the surface ring are the real blast every time, and its decorative
//     spokes are capped at twice that radius so they can never out-read the volume they decorate.
//
// ===================================================================================================
// THE PRIMITIVE BUDGET, AND HOW IT IS MET
// ===================================================================================================
//
// A burst uses AT MOST THREE primitive components (MaxPrimitivesPerBurst), whatever §2's table says
// about "8 radial cylinders" or "6 spheres": every repeated element is ONE
// UInstancedStaticMeshComponent carrying N instances, which is the project's own precedent for bead
// rings (ATraceElleGate's 60 beads, ATraceMortimerQuakeWave's 48, ATraceRippleActor's rings). The
// three slots are:
//
//     ShapeA      one solid piece — a wedge cone, a flash sphere, a teleport column.
//     ScatterMesh the repeated element — sparks, spokes, blobs, speed lines.
//     RingMesh    a bead ring, radius-animated.
//
// A ring is beads and not a scaled disc because /Engine/BasicShapes has no torus and a flat cylinder
// is a filled disc, not a ring — the same reasoning the gate and the quake wave already followed.
//
// ===================================================================================================
// DEGRADATION — WHAT HAPPENS WHEN THE MATERIALS ARE NOT THERE
// ===================================================================================================
//
// Every piece is built through UTraceFxShapes::MakeGlowMID and STORES THE ACHIEVED BLEND. Achieved
// None means the component is hidden: an untextured 100 uu default-grey cylinder standing in a
// player's face is far worse than no effect at all, and "no grey primitive on a forced None" is the
// verification this tranche is measured by (Trace.Fx.BurstForceNone arms exactly that case).
//
// WHICH PIECES ARE ADDITIVE AND WHICH ARE EMISSIVE is a rule, not a per-recipe choice, and it is
// ATraceTracer's: BIG VOLUMES ARE ADDITIVE (its halo sleeve and muzzle cone), THIN AND SMALL PIECES
// ARE EMISSIVE (its core). A big additive piece cannot punch a hole in the arena or hide the pawn it
// decorates, and faded to zero it adds zero; a thin emissive piece can push its hue past a lit
// background, which additive — clamped at 1.0 and able only to ADD — cannot. Both halves were
// measured off this file's first capture run; the reasoning and the frames are cited in the .cpp.
//
// The additive pieces also reject the OPAQUE rungs of the ladder outright rather than degrading onto
// them: a faded-out opaque piece is not gone, it is a dark matte disc.
//
// EMISSIVE PIECES ARE CAPPED BY A HUE HEADROOM as well as by the Glow ceiling, because a saturated
// colour multiplied far enough clips in every channel and comes back WHITE — the failure ATraceElleGate
// measured for its own rings and this file's first run reproduced. See EmissiveHueHeadroom.
//
// THE SOUND STILL PLAYS WHEN THE VISUAL IS GONE. §1.3 says so in as many words, and it is the right
// call: a bash you cannot see but can hear is a bug with a symptom; a silent invisible bash is a bug
// with none.
//
// ===================================================================================================
// ALSO IN THIS FILE: §1.4, THE ATTACHED-LOOP FX BUDGET
// ===================================================================================================
//
// TraceFxLoopBudget (bottom of this header) is the §1.4 helper set the per-kit tranches use for the
// OTHER half of ability FX — the while-active loops that hang off a pawn. It has nothing to do with
// bursts and it lives here because this tranche owns exactly two files; the alternative was to write
// it into somebody else's. Its whole job is to make §1.4's four rules impossible to break by
// accident at one choke point instead of ten.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"      // FVector_NetQuantizeNormal
#include "UObject/ObjectPtr.h"

#include "Gameplay/TraceFxShapes.h"       // ETraceFxBlend — stored per piece, so it must be complete

#include "TraceFxBurst.generated.h"

class APawn;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UPrimitiveComponent;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * WHICH one-shot this is. The value is replicated, so the order of this list is a wire format:
 * append, never re-order.
 *
 * Each entry's geometry, hue, sound and timing live in one row of TraceFxBurstFile::Recipes in the
 * .cpp — that table is the specification, and this enum is only its key.
 */
UENUM()
enum class ETraceFxBurstType : uint8
{
	/** Chut's bash lands: shock wedge along the knock, three speed lines, a mint contact ring. */
	ChutBash,

	/** Mace's spike bites a wall: five violet sparks off the surface plus a small surface ring. */
	SpikeEmbed,

	/** Elle steps through: an orchid column the height of a player, plus a ground ring at the feet. */
	ElleTeleport,

	/** Roxie's rocket ends: an ember shell at the REAL blast radius, eight spokes, a surface ring. */
	RocketBurst,

	/** One victim knocked by the quake: a small slate wedge along the knock direction. */
	QuakeHit,

	/** A slime wall rises: six slime blobs scattering off its base. */
	SlimeSplat,

	/** A bee connects: an amber flash and four sparks. */
	BeeSting,

	/** A jar breaks: six poison blobs thrown 60 uu outward. Semantic green, not Oyster cyan. */
	JarPop,

	/** The shared ring — Rocco's second jump, Oyster's pull link. The one type that takes a tint. */
	GenericRing,

	/** Not a burst. The count, for iteration and for the harness. Keep last. */
	Count UMETA(Hidden)
};

/**
 * Everything a burst needs that its spawn transform does not already say.
 *
 * Small on purpose: this replicates to every machine for every bash, sting and splat in a match, so
 * the direction is quantised (FVector_NetQuantizeNormal, 16 bits per component over [-1,1] — a burst
 * cannot see a tenth of a degree) and the tint is an FColor rather than an FLinearColor, which is
 * 4 bytes instead of 16 for a hue that is only ever one of a handful of constants anyway.
 */
USTRUCT()
struct FTraceFxBurstSpec
{
	GENERATED_BODY()

	/** Which recipe to build. */
	UPROPERTY()
	ETraceFxBurstType Type = ETraceFxBurstType::GenericRing;

	/**
	 * WHAT "DIRECTION" MEANS PER TYPE — it is not the same thing for all nine:
	 *
	 *   ChutBash / QuakeHit         the KNOCK direction. The wedge's apex sits at the burst origin
	 *                               and its base is out along this, so the victim is being shoved
	 *                               the way the cone points.
	 *   SpikeEmbed / RocketBurst    the SURFACE NORMAL at the hit. Sparks and spokes spray off it and
	 *                               the ring lies flat on the surface.
	 *   SlimeSplat / BeeSting       the normal to scatter away from (the wall, the contact).
	 *   ElleTeleport / JarPop       up. The column and the ground ring stand on world Z.
	 *   GenericRing                 the ring's NORMAL. UpVector gives a ground ring, which is what
	 *                               Rocco's second jump wants.
	 *
	 * Normalised on the way in by ATraceFxBurst::Burst; a zero vector becomes UpVector rather than a
	 * NaN rotation.
	 */
	UPROPERTY()
	FVector_NetQuantizeNormal Direction = FVector(0.f, 0.f, 1.f);

	/**
	 * The type's PRINCIPAL radius in uu, or 0 for "use the type's own default".
	 *
	 * Principal radius means the one number a player could measure off the screen — the contact
	 * ring's outer radius for ChutBash, the blast radius for RocketBurst, the scatter distance for
	 * JarPop. Every other dimension in a recipe is a fixed constant. ATraceFxBurst::DefaultRadiusUUFor
	 * documents each one.
	 *
	 * ROCKETBURST'S DEFAULT IS READ LIVE FROM THE DAMAGE KNOB, not copied into this file, so the
	 * drawn blast is the real blast on every build and after every retune (bible §6.2 invariant 1).
	 */
	UPROPERTY()
	float RadiusUU = 0.f;

	/**
	 * OPTIONAL hue override. Zero alpha (the default) means "use the recipe's own hue".
	 *
	 * *** THIS IS A DELIBERATE ADDITION TO §1.3's THREE-FIELD STRUCT, AND HERE IS WHY. *** §2.6 and
	 * §2.9 both spend GenericRing: Oyster's Pickler pull link (Poisoned green) and Rocco's second
	 * jump (Rocco amber). Bible §6.2 says an ability's world FX wears its owner's accent, so one
	 * hard-coded hue for GenericRing would put the wrong colour on one of the two kits. The
	 * alternative — two nearly identical enum entries — is worse: the type list is a wire format and
	 * a type is meant to be a SHAPE, not a colour.
	 *
	 * Every other type ignores this field, because every other type belongs to exactly one kit and
	 * "one hue per effect" is not a decision a call site should be able to make.
	 */
	UPROPERTY()
	FColor Tint = FColor(0, 0, 0, 0);
};

/**
 * One server-authored, client-visible, self-destroying FX one-shot.
 *
 * Lifecycle, and it is the same on every machine:
 *
 *   1. The SERVER calls ATraceFxBurst::Burst(...). The actor spawns with its Spec set BEFORE
 *      replication, so the first bunch a client receives already carries the type.
 *   2. BeginPlay (authority and late joiners) and OnRep_Spec (ordinary clients) both call the
 *      idempotent BuildIfNeeded(), which constructs the primitive set for the type, plays the type's
 *      sound locally, and starts the timeline.
 *   3. Tick runs the timeline: <= 0.45 s of animation, then the pieces are dark.
 *   4. The AUTHORITY destroys the actor at InitialLifeSpan (1.2 s — the bible's objective-scale
 *      ceiling). Clients go with it. Nothing here needs a client-side lifetime of its own; a client
 *      that never hears about the destroy has a dark, motionless, collision-free actor.
 */
UCLASS()
class TRACE_API ATraceFxBurst : public AActor
{
	GENERATED_BODY()

public:
	ATraceFxBurst();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// =============================================================================================
	// THE ONLY ENTRY POINT GAMEPLAY CODE NEEDS
	// =============================================================================================

	/**
	 * AUTHORITY ONLY. Spawns one burst of @p Type at @p Location and returns it (null on failure).
	 *
	 * Call it from the server-side site that already knows the fact — the launch in TryBash, the
	 * embed in the spike, the Destroy site in the rocket — and every machine gets the visual and the
	 * sound. There is no client-side counterpart to write and there must not be one: firing the same
	 * element again from a local TryActivate() would show the owner two of them (W2-FXCORE's router
	 * note 3, and the same rule applies here).
	 *
	 * @param Direction  see FTraceFxBurstSpec::Direction — it means something different per type.
	 * @param RadiusUU   0 for the type's own size, which is what nearly every caller wants.
	 * @param TintOverride  GenericRing only; ignored by every other type. Null = the recipe's hue.
	 */
	static ATraceFxBurst* Burst(UWorld* World, ETraceFxBurstType Type, const FVector& Location,
		const FVector& Direction, float RadiusUU = 0.f, const FLinearColor* TintOverride = nullptr);

	// =============================================================================================
	// THE RECIPE TABLE, READABLE FROM OUTSIDE (harnesses, and the kits that want to know)
	// =============================================================================================

	/** "ChutBash", "SpikeEmbed", ... Never null, including for Count and for a corrupt value. */
	static const TCHAR* TypeName(ETraceFxBurstType Type);

	/** Parses a name (case-insensitive) or a plain index. False when @p Text names nothing. */
	static bool ParseType(const FString& Text, ETraceFxBurstType& OutType);

	/** The §5.1 sound this type plays locally on every machine, or NAME_None when it has none. */
	static FName SoundEventFor(ETraceFxBurstType Type);

	/** The type's hue (bible §2 accents / semantic wheel). GenericRing's is its DEFAULT hue only. */
	static FLinearColor HueFor(ETraceFxBurstType Type);

	/** How long the animation runs, seconds. Always <= MaxAnimSeconds and well inside the lifespan. */
	static float AnimSecondsFor(ETraceFxBurstType Type);

	/**
	 * The type's principal radius when a caller passes 0, uu.
	 *
	 * @param WorldContext  needed only by RocketBurst, which reads the LIVE damage knob rather than
	 *                      a copy of it. Everything else ignores it and null is fine.
	 */
	static float DefaultRadiusUUFor(ETraceFxBurstType Type, const UObject* WorldContext);

	// =============================================================================================
	// QUERIES — for Trace.Fx.BurstTest, and for anybody debugging a burst that did not appear
	// =============================================================================================

	ETraceFxBurstType GetBurstType() const { return Spec.Type; }
	FVector GetBurstDirection() const { return FVector(Spec.Direction); }

	/** The radius this burst RESOLVED to, after the 0-means-default rule. Read off the live actor. */
	float GetResolvedRadiusUU() const { return ResolvedRadiusUU; }

	/** True once BuildIfNeeded has run on this machine. */
	bool IsBuilt() const { return bBuilt; }

	/** How many primitive components this burst actually created. Never more than 3. */
	int32 GetPrimitiveCount() const;

	/**
	 * How many of them are VISIBLE — i.e. resolved a material and were not hidden.
	 *
	 * *** THIS IS THE NUMBER THE FORCED-None TEST READS. *** It must be zero when no material
	 * resolves, because the alternative is engine-default grey on screen, and it must equal
	 * GetPrimitiveCount() on a healthy build.
	 */
	int32 GetVisiblePrimitiveCount() const;

	/** "cone=Additive scatter=Additive ring=Emissive" — the achieved blends, for the log. */
	FString DescribeBlends() const;

	/**
	 * True when the type's sound actually REACHED THE ENGINE on this machine.
	 *
	 * Measured, not assumed: BuildIfNeeded reads UTraceAudioSubsystem's per-event play map either side
	 * of the call, and that map is bumped after the side gate, the settings gate, the device test and
	 * the resolve. "I called Play" and "a sound played" are different claims.
	 */
	bool DidPlaySound() const { return bSoundPlayed; }

	// =============================================================================================
	// CONSTANTS — the numbers other files are allowed to depend on
	// =============================================================================================

	/** §1.3: the actor lives 1.2 s, the bible's objective-scale ceiling (§6.4). */
	static constexpr float LifeSpanSeconds = 1.2f;

	/** §1.3: "each type is <= 0.45 s of animation + fade inside the 1.2 s lifespan". Asserted. */
	static constexpr float MaxAnimSeconds = 0.45f;

	/** Three components, whatever the element counts in §2's tables say. See the header comment. */
	static constexpr int32 MaxPrimitivesPerBurst = 3;

	/** Bible §3.2: an FX transient never exceeds the smear-head precedent. */
	static constexpr float MaxTransientGlow = 4.2f;

	/**
	 * Bible §3.4: sub-8 uu emissive dissolves under TSR, so no world-space piece here is thinner.
	 * §2's tables ask for 3 uu radius sparks in three places; they are drawn at 4 uu.
	 */
	static constexpr float MinEmissiveRadiusUU = 4.f;

#if !UE_BUILD_SHIPPING
	/**
	 * DEV ONLY, LOCAL ONLY, for Trace.Fx.BurstTest's photography.
	 *
	 * Freezes the timeline at @p Alpha and extends the lifespan so a headless screenshot — which
	 * lands on the render thread one or two frames after it is asked for — photographs a KNOWN frame
	 * of the animation instead of whichever one it happened to catch. Nothing in the shipping path
	 * calls this, it does not replicate, and the whole thing compiles out of Shipping.
	 */
	void DebugHoldAt(float Alpha, float ExtraLifeSeconds);
#endif

protected:
	/**
	 * REPLICATED, and it is the whole message. ReplicatedUsing because a client that receives it has
	 * to build immediately: waiting for the next tick would cost a frame of a 0.2 s effect.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Spec)
	FTraceFxBurstSpec Spec;

	UFUNCTION()
	void OnRep_Spec();

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root = nullptr;

	/**
	 * A scene node whose local +Z is Spec.Direction. Not a primitive; it costs nothing to draw.
	 *
	 * It exists so the geometry can be placed with UTraceFxShapes' LOCAL-Z helpers — StretchAlongLocalZ
	 * and PlaceConeAlongLocalZ both write a relative location of (0,0,Z) and therefore only mean what
	 * they say when the parent's +Z IS the axis. That is exactly how ATraceTracer does it (it rotates
	 * the whole actor down the shot); a burst cannot rotate the actor, because the actor's rotation is
	 * spawn-transform state and this class deliberately builds from the REPLICATED Spec.Direction
	 * instead, so that nothing depends on a rotation surviving the wire.
	 */
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Aim = nullptr;

	/** The same node pointing the other way, for cones. See the .cpp: an engine cone's apex is +Z. */
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> AimBack = nullptr;

	/** Slot 1: the single solid piece — wedge cone, flash sphere, teleport column. May be null. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> ShapeA = nullptr;

	/** Slot 2: the repeated element — sparks, spokes, blobs, speed lines. One component, N instances. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> ScatterMesh = nullptr;

	/** Slot 3: the bead ring. One component, N instances, radius animated every tick. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> RingMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ShapeAMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ScatterMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RingMID = nullptr;

private:
	/**
	 * Builds the type's primitive set, plays its sound, and starts the clock. Idempotent: called from
	 * BeginPlay, from OnRep_Spec and from Tick, because which of the three arrives first depends on
	 * whether this machine is the server, an ordinary client or one that joined mid-flight.
	 */
	void BuildIfNeeded();

	/** Drives the timeline. @p Alpha is 0..1 across AnimSecondsFor(Type). */
	void UpdateBurst(float Alpha);

	/**
	 * Creates one mesh component under @p Parent, configures it as decoration, and gives it a MID.
	 *
	 * @param bAdditiveOnly  true for pieces that must NOT fall back to an opaque material: a large
	 *                       opaque sphere or cone writes depth and punches a hole in the world, and
	 *                       a faded-out opaque piece is a dark matte disc rather than nothing at all.
	 *                       This is ATraceTracer's halo/muzzle rule and it is applied for the same
	 *                       measured reason.
	 * @param OutMID         a TObjectPtr& and not a raw pointer&, because a MID has to live in a
	 *                       TObjectPtr UPROPERTY to survive a garbage collection and a
	 *                       `UMaterialInstanceDynamic*&` cannot bind to one. Same constraint
	 *                       UTraceFxShapes::MakeGlowMID documents, solved the same way round.
	 * @return               the component. Its blend is None and it is HIDDEN when nothing resolved —
	 *                       never null-and-forgotten, so GetPrimitiveCount stays honest.
	 */
	UStaticMeshComponent* MakeSolidPiece(USceneComponent* Parent, const TCHAR* NameHint,
		UStaticMesh* Mesh, bool bAdditiveOnly, TObjectPtr<UMaterialInstanceDynamic>& OutMID,
		ETraceFxBlend& OutBlend);

	/** The same, for a repeated element: one component, instances added by the caller. */
	UInstancedStaticMeshComponent* MakeInstancedPiece(USceneComponent* Parent, const TCHAR* NameHint,
		UStaticMesh* Mesh, bool bAdditiveOnly, TObjectPtr<UMaterialInstanceDynamic>& OutMID,
		ETraceFxBlend& OutBlend);

	/**
	 * Pushes hue + intensity into a piece's MID, clamping Glow to MaxTransientGlow and to this burst's
	 * hue headroom on the way.
	 *
	 * A member and not a static because of HueHeadroomAtBuild: the headroom is LATCHED when the burst
	 * is built. It has to be, and the reason is a measurement that came out wrong — the dev override
	 * is re-read on every tick if it is read at all, so four bursts photographed at four different
	 * headroom values re-wrote themselves to whatever the CVar said LAST and produced four identical
	 * frames presented as a ladder. Latching also means a burst cannot change its look in flight.
	 */
	void SetPieceGlow(UMaterialInstanceDynamic* MID, ETraceFxBlend Blend,
		const FLinearColor& Hue, float Intensity) const;

	/** The achieved blends, one per slot. None means "hidden"; see GetVisiblePrimitiveCount. */
	ETraceFxBlend ShapeABlend = ETraceFxBlend::None;
	ETraceFxBlend ScatterBlend = ETraceFxBlend::None;
	ETraceFxBlend RingBlend = ETraceFxBlend::None;

	/** Per-instance directions for ScatterMesh, computed once at build and reused every tick. */
	TArray<FVector> ScatterDirs;

	/** Per-instance lateral offsets for the ChutBash speed lines (the one scatter set that is not radial). */
	TArray<FVector> ScatterOffsets;

	/** The hue actually in use — the recipe's, or Spec.Tint when GenericRing was given one. */
	FLinearColor Hue = FLinearColor::White;

	/** Spec.RadiusUU, or the type's default when that was 0. See DefaultRadiusUUFor. */
	float ResolvedRadiusUU = 0.f;

	/** The hue headroom this burst was BUILT with. See SetPieceGlow for why it is latched. */
	float HueHeadroomAtBuild = 1.f;

	float Elapsed = 0.f;
	bool bBuilt = false;

	/** See DidPlaySound: measured off the audio subsystem's per-event map, not set on faith. */
	bool bSoundPlayed = false;

#if !UE_BUILD_SHIPPING
	/** >= 0 freezes the timeline there. DebugHoldAt only; never replicated, never used by gameplay. */
	float DebugHeldAlpha = -1.f;
#endif
};

// ===================================================================================================
// FX_AUDIO_PLAN §1.4 — THE ATTACHED-LOOP FX BUDGET
// ===================================================================================================
//
// §1.4, in full: "While-active FX are additive only, intensity <= 0.5, inside the capsule footprint
// (<= 96 uu from the capsule axis), max 4 primitive components per pawn. They may bob/rotate
// (motion), and may NOT brightness-pulse if the state is a lethal telegraph — none in this plan are."
//
// That is four numeric rules and one prohibition, and they will be applied by six different kit
// tranches in two later waves. Written out longhand at each site they would be six chances to write
// 0.6 instead of 0.5 and no way to tell afterwards. Written here they are one choke point that
// REFUSES the fifth component and CLAMPS the sixth's intensity, and a console command that prints
// what every pawn is actually carrying.
//
// WHAT THIS IS NOT: it is not an FX system. Kits still build their own geometry, own their own
// pointers and drive their own animation off the §1.2 router edges. This only counts, clamps and
// says no.
//
namespace TraceFxLoopBudget
{
	/** §1.4: at most four attached primitive components per pawn, across ALL kits and effects. */
	constexpr int32 MaxPrimitivesPerPawn = 4;

	/** §1.4: additive intensity ceiling for a while-active effect. Not a suggestion — Clamp enforces it. */
	constexpr float MaxIntensity = 0.5f;

	/** §1.4: "inside the capsule footprint" — no piece further than this from the capsule axis. */
	constexpr float MaxOffsetFromAxisUU = 96.f;

	/** @return @p Requested clamped to [0, MaxIntensity], logging ONCE per process if it had to clamp. */
	TRACE_API float ClampIntensity(float Requested);

	/**
	 * @return @p LocalOffset with its horizontal component pulled inside MaxOffsetFromAxisUU.
	 *
	 * The VERTICAL component is left alone on purpose: "inside the capsule footprint" is a radial
	 * statement (§1.4 says "from the capsule axis"), and Lily's aura rings legitimately travel from
	 * chest height to the feet.
	 */
	TRACE_API FVector ClampToFootprint(const FVector& LocalOffset);

	/** How many loop primitives @p Pawn is currently carrying. Stale entries are pruned as it counts. */
	TRACE_API int32 CountFor(const APawn* Pawn);

	/** True when @p Pawn could take @p Additional more without breaking MaxPrimitivesPerPawn. */
	TRACE_API bool CanAttach(const APawn* Pawn, int32 Additional = 1);

	/**
	 * THE ONE CALL A KIT SHOULD USE. Creates an additive loop primitive on @p AttachTo, registers it
	 * against @p Pawn's budget, and hands it back with its MID.
	 *
	 * Refuses — returning null, logging once — when the pawn is already at MaxPrimitivesPerPawn, when
	 * the mesh or the additive material did not resolve, or when @p AttachTo is not @p Pawn's. Every
	 * refusal is survivable: the kit simply has no loop this time, which is the documented degradation
	 * everywhere else in this project's FX.
	 *
	 * @param Intensity     clamped to MaxIntensity. Pass what the spec asks for and let it clamp.
	 * @param LocalOffset   relative to @p AttachTo; clamped into the footprint.
	 * @param RadiusUU      the piece's radius. Floored at ATraceFxBurst::MinEmissiveRadiusUU.
	 * @param OutMID        the piece's dynamic material, for per-frame hue/intensity writes. The
	 *                      caller may write intensity DOWN freely; writing it up past MaxIntensity is
	 *                      what ClampIntensity is for. It is a TObjectPtr& so that it can be a
	 *                      UPROPERTY of the caller's, which is where a MID has to live to survive a
	 *                      garbage collection.
	 */
	TRACE_API UStaticMeshComponent* AttachLoopPrimitive(APawn* Pawn, USceneComponent* AttachTo,
		UStaticMesh* Mesh, const TCHAR* NameHint, const FLinearColor& Hue, float Intensity,
		const FVector& LocalOffset, float RadiusUU, TObjectPtr<UMaterialInstanceDynamic>& OutMID);

	/**
	 * Releases @p Piece from @p Pawn's budget and destroys the component.
	 *
	 * CALL IT ON EVERY OFF-EDGE — death, character swap, cloak, ability end. §8.9: "no FX component
	 * survives its pawn". A pawn that dies with pieces still registered is not a leak (the entries are
	 * weak and get pruned) but it IS a kit that forgot its detach, and Trace.Fx.LoopBudget will show it.
	 */
	TRACE_API void DetachLoopPrimitive(APawn* Pawn, UPrimitiveComponent* Piece);

	/** Drops every registration for @p Pawn without touching the components. For a pawn being torn down. */
	TRACE_API void ForgetPawn(const APawn* Pawn);
}
