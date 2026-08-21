// Trace — the player pawn.
//
// Strict about collision: the capsule is the ONLY collider on this actor (ECC_Pawn). The skeletal
// mesh and the fallback shape are visual-only and set to NoCollision, so nothing can hit "the mesh"
// instead of "the character" — hitscan resolution and the trail trip test both reason purely about
// the capsule, and lag compensation records only the capsule pose. Adding a colliding component
// here would quietly break all three.
//
// --- VIEW: FIRST PERSON, EXCEPT WHILE CARRYING THE CORE -------------------------------------
//
// Trace is a first-person shooter that becomes third person the moment you pick the Core up, and
// first person again the moment you let it go. That is not a style flourish, it is the rules:
// the carrier cannot be shot and can only be killed by an enemy dashing through the trail it
// leaves BEHIND it (contract §3). First person would hide the only thing that can kill you.
//
// It is ONE camera on ONE spring arm the whole time. The mode change is a 0.35 s eased blend of
// the arm (0 -> 450 uu) and of the arm origin (eye height -> shoulder pivot); nothing is
// re-parented, no second camera actor exists, and there is no cut for the player camera manager
// to blend on top of. The pull-back IS the "you are the carrier now" signal.
//
// Three things move together with that blend and must never be separated:
//
//   1. AIM. In first person the camera is the gun: the arm collapses to length 0 at exactly
//      GetPawnViewLocation() (actor + BaseEyeHeight), which is the same point GetAimDirection()
//      builds its ray from, and GetMuzzleLocation() sits ON that ray. Screen centre, eye, muzzle
//      and bullet are one line, so the crosshair cannot lie. Note the shot geometry does NOT
//      depend on which view mode is active — a carrier cannot fire at all, so every shot in the
//      game is fired from first person, on this one ray, on the server and on the client alike.
//
//   2. OUR OWN BODY. In first person the local pawn's meshes are SetOwnerNoSee — hidden from this
//      player's camera only. Every OTHER player still sees the full character (that is what
//      OwnerNoSee means), and the mesh keeps casting its shadow, so nothing about how you look to
//      the rest of the match changes. This is applied ONLY on the locally controlled pawn.
//
//   3. FACING. First person turns the body with the aim (bUseControllerRotationYaw), which is what
//      makes an enemy's facing readable as "where they are shooting". Carrying flips back to
//      orient-to-movement so the body faces where it runs, which is both the right read for a
//      carrier fleeing with the Core and what keeps ABP_Unarmed's speed-driven blend space looking
//      correct on the body you are now staring at. Bot-controlled pawns are left on
//      orient-to-movement in both modes: nobody is ever in first person inside a bot, and the
//      1D blend space cannot show a strafe.
//
//
// --- THE HANDS. DEMO 23's REFUSAL IS OVER; THIS IS WHAT SHIPPED (spec v31 §6) ----------------
//
// Demo 23 asked for "the default unreal engine mannequin hands and animations, and socket the gun we
// modeled in" and it was refused, correctly and on the record: UE 5.8 ships NO first-person arms
// mesh. /Game/Characters/Mannequins carries three skeletal meshes and all three are FULL BODIES; the
// only hand-shaped assets Epic ships are a VR pointer and the MannyXR controller hands, on a
// different skeleton with no FPS clips. That refusal is now HISTORY RATHER THAN POLICY — the v31 art
// pack supplies the arms Epic does not, so the blocked thing is built. What follows is the build,
// not the argument; the old "we cannot do this" note is deleted because a stale refusal sitting above
// code that does the thing is worse than no comment at all.
//
// WHAT ARRIVED. `Art/Pack/models/gloved_hands.glb` -> /Game/Trace/Art/Pack/Hands: ONE SkeletalMesh
// (SK_TraceHands, a 107-bone rigid hierarchy — the GLB reports `skins: 0` and Interchange converts a
// rigidly-parented node tree into one-bone-per-node) plus TWENTY UAnimSequences on its own skeleton.
// The clips are AUTHORED, not written here. Nothing in this file hand-animates a finger.
//
// FOUR LOADOUTS, and only the combinations that exist in play are baked — there is no shoot-with-
// knife and no reload-for-core:
//
//     knife   Idle_Knife    draw, stab, inspect, jump, wall jump
//     pistol  Idle_Pistol   shoot, reload, jump, wall jump
//     smg     Idle_Smg      shoot, reload, jump, wall jump
//     core    Idle_Core     throw, jump, wall jump          (a two-hand cradle; the others are
//                                                            right-handed, left hand open and free)
//
// NO ANIM BLUEPRINT. One USkeletalMeshComponent in EAnimationMode::AnimationSingleNode, and this
// file SETS THE POSITION EVERY FRAME rather than letting the single-node instance free-run. That is
// the spec's "sample before you advance" rule taken literally, and it is not academic here:
// UAnimSingleNodeInstance advances CurrentTime by DeltaTime and *then* evaluates, so a clip started
// with PlayAnimation() never renders its own frame 0 — on Shoot_{Pistol,Smg}, which is 0.1667 s, the
// first rendered frame would already be 6% into the recoil and the trigger-pull frame would never
// exist. Every clip's time therefore comes from REAL STATE (the weapon's replicated reload deadline,
// the swing lockout, a shot that actually left, the wall-jump counter), never from an accumulator
// that runs on its own.
//
// THE WEAPONS RIDE `wrist_right`, WITHOUT BEING RE-PARENTED TO IT, and that distinction is load
// bearing. Every frame the gun's rig-space transform is recomposed as
// (wrist_right now) * (wrist_right in the reference pose)^-1 * (the gun's shipped rest transform),
// which is exactly what attaching to that bone would produce — but the gun parts stay DIRECT
// children of ViewModelRoot, because UTraceWeaponComponent::RefreshGunPartCache and
// ::GetViewModelCensus both walk `ViewModelRoot->GetChildrenComponents(bIncludeAllDescendants=false)`
// to find them. Re-parenting would push them one level down, out of that walk, and the knife's
// gun-hiding rule would silently stop finding the gun — which is spec v12 §7's "the knife and gun
// can be held at the same time" bug, reintroduced from the far side of a file boundary.
// ATraceCharacter therefore adds a TICK PREREQUISITE on the hands component so the actor ticks
// AFTER the pose is evaluated; without it the gun would be composed against a bone transform one
// frame stale and would visibly swim against the fist through a jump.
//
// THE FALLBACK SURVIVES, UNCHANGED. The procedural cube hands below are still built whenever the
// pack art does not resolve — a fresh clone that has not run `git lfs pull`, or `-TraceNoCharacterArt`
// / `-TraceNoPackHands` asked for on purpose. A missing import must never mean an empty screen.
//
// --- THE FIRST-PERSON VIEWMODEL --------------------------------------------------------------
//
// A first-person shooter with no gun in frame is the single most visible thing this build was
// missing: a crosshair, a tracer, and nothing holding either. ViewModelRoot is a rig hanging off
// the CAMERA carrying a handgun and two gloved forearms, and it is built from /Engine/BasicShapes
// primitives rather than from an imported weapon asset.
//
// AS OF v31 THE HANDS HALF OF THAT SENTENCE IS THE FALLBACK, not the shipped rig: the four hand
// cubes and the two forearm cylinders are replaced by SK_TraceHands whenever the pack art resolves
// (see the block above). The GUN half is unchanged and every argument below still applies to it,
// word for word — including to the cube gun, which is still what a machine with no railgun art gets.
// The three reasons the procedural rig was built that way, and which is why it is still worth
// keeping as the fallback:
//
//   * Epic's SM_Pistol lives in Templates/TemplateResources/Standard/Weapons and expects to mount
//     at /Game/Weapons/Pistol; Scripts/import-mannequin.sh does not fetch it, so requiring it would
//     make the most visible feature in the game depend on an import that does not exist yet.
//   * It is a grey PBR prop. This arena is black surfaces and neon edges; a faceted dark sidearm
//     with team-coloured light channels belongs in it and a photoreal pistol does not.
//   * Every dimension is arithmetic, so where the gun lands on screen is calculable rather than
//     discovered by launching the game and looking - which is what let this land in one pass.
//
// THREE THINGS IT MUST NEVER DO, and how each is prevented:
//
//   1. LIE ABOUT AIM. The rig is a child of the camera and nothing in the shot path reads it.
//      GetMuzzleLocation()/GetAimDirection() are still pure arithmetic on the actor transform and
//      the control rotation, so eyeErr stays 0.00 uu and aimErr 0.0000 deg with the viewmodel
//      swaying, bobbing and recoiling. The gun is scenery that happens to point the right way.
//   2. BE SEEN BY ANYONE ELSE. Every part is SetOnlyOwnerSee(true) and casts no shadow, so no other
//      machine and no other viewer can ever see a floating gun. It is additionally hidden whenever
//      the view blend leaves first person, so the carrier's third-person camera never shows it.
//   3. POKE THROUGH A WALL. Every part is tagged EFirstPersonPrimitiveType::FirstPerson, which
//      makes the renderer draw it through the camera's FirstPersonFieldOfView and compress it into
//      FirstPersonScale of its real depth range. At Scale 0.5 the muzzle - the deepest point of the
//      rig, 60 uu out - is drawn as though it were 30 uu out, and the pawn capsule is 34 uu in
//      radius, so no piece of the viewmodel can reach a surface the body cannot already touch. The
//      matching FirstPersonFieldOfView keeps it from looking wide-angle-distorted at that range.
//
// --- ART ------------------------------------------------------------------------------------
// The visible character is Epic's Mannequin (SKM_Manny_Simple) running ABP_Unarmed, which drives
// the BS_Idle_Walk_Run blend space from the pawn's own velocity — idle, walk, run and fall all
// animate with no work on our side.
//
// Those assets are NOT in the repository. Binary art is imported per developer, not committed:
// Scripts/import-mannequin.sh copies them out of that developer's own UE 5.8 install into
// Content/Characters/Mannequins (gitignored). Everyone is pinned to the same engine version, so
// everyone gets identical files.
//
// Therefore EVERY art asset referenced here is optional at runtime. They are held as soft
// references and resolved once in PostInitializeComponents(); if they are missing the pawn falls
// back to a visible team-coloured capsule shape built from engine primitives and logs a warning
// naming the import script. A missing import must never mean a crash or an invisible player.
//
// That fallback branch stays exercisable on a machine where the import HAS been run, without
// deleting anything: launch with -TraceNoCharacterArt.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"        // FTimerHandle, EEndPlayReason
#include "GameFramework/Character.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"                // ETraceTeam

#include "TraceCharacter.generated.h"

class AController;
class UAnimInstance;
class UAnimSequence;
class UCameraComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class USpringArmComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTraceCharacterMovementComponent;
class UTraceHealthComponent;
class UTraceLagCompensationComponent;
class UTraceTrailComponent;
class UTraceWeaponComponent;

/**
 * What happened when this build went looking for Epic's Mannequin.
 *
 * THIS ENUM IS THE FIX FOR A REAL BUG REPORT. The user asked for "default unreal engine human
 * character models that have running animations, heads, and limbs" — which were already implemented
 * and already loading on the machine they were implemented on. What they saw was the FALLBACK: art
 * is gitignored by design (see the file header), so a clone that has not run
 * Scripts/import-mannequin.sh silently degrades to team-coloured capsules. The degrade was silent
 * because the only evidence was one Warning line in a log nobody reads during a playtest.
 *
 * So the status is now a first-class, queryable fact: decided before any pawn spawns
 * (VerifyCharacterArtInstalled), logged as a banner at Error verbosity, and drawn on screen by
 * ATraceHUD for as long as it is wrong. A missing import must be impossible to mistake for a
 * design decision.
 */
enum class ETraceCharacterArtStatus : uint8
{
	/** Nothing has looked yet. */
	Unknown,
	/** Mannequin mesh AND ABP_Unarmed both resolved: heads, limbs and a real run cycle. */
	Ok,
	/** The mesh is there but the anim blueprint is not: a posed, non-animating character. */
	AnimMissing,
	/** Neither is there. Players are drawn as fallback primitives. THIS is the reported bug. */
	MeshMissing,
	/** -TraceNoCharacterArt: the fallback was asked for on purpose, so it is not a defect. */
	DisabledByCommandLine,
	/** Dedicated server: renders nothing, so there is nothing to warn about. */
	NotRequired
};

UCLASS()
class TRACE_API ATraceCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	/**
	 * The FObjectInitializer overload is mandatory: it is the only way to swap ACharacter's
	 * default movement component for UTraceCharacterMovementComponent, and GENERATED_BODY() does
	 * not declare it for us. Without it the engine silently uses the stock CMC and the dash
	 * prediction disappears.
	 */
	ATraceCharacter(const FObjectInitializer& OI);

	// --- Components ----------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, Category = "Trace|Camera")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Camera")
	TObjectPtr<UCameraComponent> Camera;

	/**
	 * Shown ONLY when the Mannequin import is missing, so a fresh clone still has a visible,
	 * team-coloured player instead of nothing. Engine primitives, visual only, NoCollision.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Visual")
	TObjectPtr<UStaticMeshComponent> FallbackBodyMesh;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Visual")
	TObjectPtr<UStaticMeshComponent> FallbackHeadMesh;

	/**
	 * Parent of every first-person viewmodel part; see the header note above. Attached to the
	 * camera, so it inherits the aim for free, and moved every frame by UpdateViewModel() to carry
	 * the sway / bob / recoil. Exists on every pawn (it is one empty scene component); the parts
	 * themselves are only ever built on a pawn a human is actually looking out of.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Visual")
	TObjectPtr<USceneComponent> ViewModelRoot;

	/**
	 * A glowing skid streak under the feet while sliding.
	 *
	 * The Mannequin set Scripts/import-mannequin.sh imports has NO crouch and NO slide animation —
	 * spec v4 §1 asked for Unreal's stock one and it does not exist — so the body itself is posed
	 * procedurally (UpdateCrouchPresentation: recline, drop, roll, and the locomotion blend space
	 * slowed almost to a stop). This streak is the other half of the read, and the half that carries
	 * across the arena: an unlit team-coloured streak on the deck,
	 * scaled by speed, which the near-mirror floor doubles. It is the light-cycle read the rest of
	 * the arena is built around, and it costs one static mesh component per pawn.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Visual")
	TObjectPtr<UStaticMeshComponent> SlideSkidMesh;

	// --- Art (soft: imported per developer, see the file header) ---------------------------------

	/**
	 * Soft, not hard, and deliberately so. A hard FObjectFinder reference on the CDO would make the
	 * class fail to construct cleanly when the import has not been run, which is the normal state of
	 * a fresh clone. Soft references still get followed by the cooker, so a packaged build keeps
	 * them.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Visual")
	TSoftObjectPtr<USkeletalMesh> CharacterMeshAsset;

	/** ABP_Unarmed: drives BS_Idle_Walk_Run from the pawn's velocity. See the file header. */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Visual")
	TSoftClassPtr<UAnimInstance> CharacterAnimClass;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Components")
	TObjectPtr<UTraceHealthComponent> Health;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Components")
	TObjectPtr<UTraceWeaponComponent> Weapon;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Components")
	TObjectPtr<UTraceTrailComponent> Trail;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Components")
	TObjectPtr<UTraceLagCompensationComponent> LagComp;

	// --- Replicated state ----------------------------------------------------------------------

	/** Set by ATraceCore through SetCarrying(). Drives invulnerability, the trail and the tint. */
	UPROPERTY(ReplicatedUsing = OnRep_IsCarrier)
	bool bIsCarrier = false;

	/**
	 * Server's view of "this pawn is sliding", replicated to everyone EXCEPT the owner.
	 *
	 * INTEGRATOR FIX — the slide did not exist on simulated proxies, and that was a hit-registration
	 * bug, not only a cosmetic one. A slide is expressed entirely through FSavedMove_Trace's
	 * compressed flags, which reach the server and the owning client and nobody else; the capsule
	 * deliberately never resizes, so ACharacter::bIsCrouched (which *is* replicated) never sets
	 * either. A third machine therefore drew a sliding player standing upright, and — because
	 * ATraceCharacter::GetHitZonePostureScale() is derived from BaseEyeHeight — laid that player's
	 * head/hip bands out at STANDING height while the server had them compressed to posture ~0.78.
	 * The shooter's predicted zone and the server's rewound zone then disagreed for the whole slide:
	 * aim at the head you can see, collect 40 damage instead of 100.
	 *
	 * COND_SkipOwner because the owner predicts its own slide and must not be corrected by a value
	 * that is one RTT stale; UpdateCrouchPresentation ORs the two, so an early or late packet can
	 * only ever extend a slide the owner is already showing, never contradict it.
	 */
	UPROPERTY(Replicated)
	bool bReplicatedSliding = false;

	// --- AActor / ACharacter ---------------------------------------------------------------------

	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	/** Drives the first/third person camera blend. See UpdateViewBlend(). */
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void FellOutOfWorld(const class UDamageType& DmgType) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * DORMANT IN THE SHIPPED CONFIGURATION, and kept deliberately.
	 *
	 * Trace's crouch key does not resize anything: UTraceCharacterMovementComponent overrides
	 * CanCrouchInCurrentState() to always return false and consumes the key as slide INPUT instead,
	 * because the capsule is this project's single source of truth for hitscan, for the pose history
	 * the server rewinds and for the trail trip test. So ACharacter::bIsCrouched is never set and
	 * these two never fire today - the eye is moved by UpdateCrouchPresentation() reading
	 * IsSliding() instead.
	 *
	 * They exist because "the capsule never shrinks" is a decision in a file this pass does not own,
	 * and if it is ever revisited the eye MUST follow the capsule down or the crosshair starts
	 * lying: GetPawnViewLocation() is actor + BaseEyeHeight and it is what the aim ray is built
	 * from. CrouchedEyeHeight is therefore computed from the resized capsule rather than being a
	 * constant, and it is computed BEFORE Super - ACharacter::OnStartCrouch is what calls
	 * RecalculateBaseEyeHeight(), which is what reads CrouchedEyeHeight, and the capsule has already
	 * been resized by the time we get here. Hence: compute, then Super.
	 */
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;

	/**
	 * SPEC v31 §6 — the ONLY event in the hand set that the pawn cannot already read off its own
	 * state, so it is latched here rather than inferred.
	 *
	 * "Airborne with positive Z velocity" is not the same fact: it is also true one frame into a
	 * fall off a ledge, on the way up from a launcher, and on the frame a depenetration push pops
	 * the capsule. ACharacter::OnJumped fires exactly when a jump was COMMITTED — on the owning
	 * client and on the server, the two machines that own the input — so it is the honest edge. A
	 * WALL jump comes through here too, and is told apart by
	 * UTraceCharacterMovementComponent::GetWallJumpsSinceGround() having gone up on the same frame;
	 * see UpdateHandsAnimation, which owns that comparison.
	 */
	virtual void OnJumped_Implementation() override;

	/**
	 * MEASURED MULTIPLAYER BUG (spec v5 §0 + §7). Refuses the arena as a movement base.
	 *
	 * THE SYMPTOM. A real client connected to a listen server logged 2948 of these in 30 seconds:
	 *
	 *   LogNetPlayerMovement: Warning: ClientAdjustPosition_Implementation could not resolve the
	 *   new relative movement base actor, IGNORING SERVER CORRECTION! ... on base None
	 *
	 * Every one of those is a position correction the server sent and the client threw away. A
	 * client that cannot be corrected while it is standing on the floor is a client whose position
	 * is free to drift from the server's without limit - which is the network half of "jumping on
	 * the edge of a raised section feels like rubber banding", and no amount of mantle fixes it.
	 *
	 * THE MECHANISM, from the engine source rather than from guesswork.
	 * MovementBaseUtility::IsDynamicBase() is `Mobility == EComponentMobility::Movable` and nothing
	 * else, and UseRelativeLocation() is a straight alias for it. ATraceArenaBuilder creates all
	 * 1187 pieces of its geometry Movable (runtime-spawned components with no baked lighting to gain
	 * from Static), so the engine classifies the FLOOR as a moving platform. The server therefore
	 * sends corrections as base-RELATIVE, naming the primitive component the pawn stands on - and
	 * that component is built locally in BeginPlay on each machine, is not a replicated subobject,
	 * has no NetGUID, and can never resolve on the client. ClientAdjustPosition_Implementation hits
	 * `bUnresolvedBase && bBaseRelativePosition` and returns without applying anything.
	 *
	 * THE FIX. The arena does not move, so it must not be a base. Nulling it here puts the pawn in
	 * exactly the state the engine's own templates are in when standing on Static level geometry:
	 * no base, absolute corrections, which resolve on any machine.
	 *
	 * ONLY the FMovementBaseInterfaceData overload is overridden, and that is deliberate. 5.8
	 * deprecated the UPrimitiveComponent form; its body is now two lines that wrap the argument and
	 * make a VIRTUAL call to this one (Character.cpp:1146), so every path still lands here.
	 * Overriding the deprecated form as well compiles on clang with a -Wdeprecated-declarations
	 * warning and FAILS the Windows build, which treats it as an error.
	 *
	 * Deliberately scoped to the arena rather than to "anything unreplicated": characters and the
	 * Core ARE legitimate dynamic bases and must keep working. If another never-moving actor is ever
	 * added to the world with pawn-blocking collision, add it to ShouldIgnoreAsMovementBase().
	 */
	virtual void SetBase(struct FMovementBaseInterfaceData* MovementBaseInterfaceData, const FName BoneName = NAME_None, bool bNotifyActor = true) override;

	/** True when @p BaseComponent belongs to world geometry that never moves. See SetBase(). */
	static bool ShouldIgnoreAsMovementBase(const UPrimitiveComponent* BaseComponent);

	// --- Queries ---------------------------------------------------------------------------------

	/** Team from the PlayerState, or None while it has not replicated yet. */
	ETraceTeam GetTeam() const;
	bool IsCarrier() const;
	bool IsAlive() const;

	/** True during the dash window. The trail trip test keys off this — see contract §3. */
	bool IsDashing() const;

	/**
	 * Spec v10 §6 — "don't let players shoot while in a dash animation; as soon as they end the
	 * dash, let them shoot again". The gate UTraceWeaponComponent::CanFire and ::CanSwing consult.
	 *
	 * Forwards to UTraceCharacterMovementComponent::AreWeaponActionsBlocked(), which is a pure
	 * function of the dash clock — so it releases on the exact frame the dash ends. It is a GATE,
	 * not a cooldown: there is no timer here, nothing to expire, and no frame of lockout afterwards.
	 *
	 * It is a NAMED forwarder rather than a second IsDashing() call at each gate on purpose. "The
	 * weapon is blocked" and "the pawn is dashing" happen to be the same fact today, and the day
	 * they stop being the same fact (a stun, a mantle, a landing recovery) there must be one place
	 * to change rather than a grep for IsDashing() across two slices.
	 *
	 * VALID on the owning client and on the server; MEANINGLESS on a simulated proxy, where nothing
	 * replicates the dash clock and it reads false forever. Gate the shooter's own machine and the
	 * server, never somebody else's pawn.
	 */
	bool AreWeaponActionsBlocked() const;

	/**
	 * The view mode this pawn WANTS, which is simply "am I not carrying the Core". The camera may
	 * still be mid-blend toward it; GetViewBlendAlpha() is the settled truth about the camera.
	 */
	bool WantsFirstPersonView() const;

	/** 0 = fully first person, 1 = fully third person. Eased, so it is the value actually applied. */
	float GetViewBlendAlpha() const;

	/** Diagnostics only (Trace.DebugViewProbe): 0 means the viewmodel was never built for this pawn. */
	int32 GetViewModelPartCount() const;
	bool IsViewModelVisible() const;

	/** True when the imported railgun art was used; false when the procedural cube gun was built. */
	bool UsesRailgunViewModel() const;

	/**
	 * Verification only (Trace.Railgun.Hold). Pins the fire animation at @p Alpha — 0 is the
	 * discharge frame with the rails at full throw and the glow at peak, 1 is rest — for
	 * @p HoldSeconds, so a screenshot can catch a pose that otherwise lasts 0.36 s. Pass a negative
	 * Alpha to release. Never called by gameplay.
	 */
	void DebugHoldRailgunPhase(float Alpha, float HoldSeconds);

	/** Verification only. The three railgun parts, or nulls on the fallback rig. */
	void DebugGetRailgunParts(UStaticMeshComponent*& OutBody,
		UStaticMeshComponent*& OutRailLeft, UStaticMeshComponent*& OutRailRight) const;

	/**
	 * Verification only. Reads EmissiveIntensity BACK OFF the two live material instances, so a
	 * probe reports what the renderer will actually use rather than what we believe we set. A
	 * parameter name that does not exist on the material silently does nothing when written; the
	 * only way to catch that is to read it.
	 */
	bool DebugGetRailgunEmissive(float& OutCyan, float& OutAmber) const;

	// --- The SMG rig  (spec v30 §2/§3/§4) ---------------------------------------------------------

	/**
	 * WHICH GUN THE RIG IS ACTUALLY DRAWING RIGHT NOW, which is the whole of what spec v30 §2 is
	 * about: demo 24 shipped an SMG that looked exactly like the pistol, and nothing on screen could
	 * tell a player which of them they were holding.
	 *
	 * This is the DRAWN answer, not the selector — under a fallback (no SMG art on disk) the `3` slot
	 * shows the pistol rig and this says Pistol, which is what a probe needs to hear. It reads None
	 * while the guns are stowed, which is UTraceWeaponComponent's rule rather than this class's; see
	 * UpdateWeaponSelection for why the two are kept apart.
	 */
	enum class EShownGun : uint8
	{
		None = 0,    ///< guns stowed: the `1` state, knife only
		Pistol = 1,  ///< the railgun rig, or the procedural cube gun when the art is missing
		Smg = 2      ///< the imported SMG rig
	};
	EShownGun GetShownGun() const;

	/** True when the imported SMG art resolved and the four-part SMG rig was built. */
	bool UsesSmgViewModel() const;

	/**
	 * Verification only (Trace.Smg.Hold), the SMG's twin of DebugHoldRailgunPhase.
	 *
	 * @param Alpha  0 is the shot frame — walls thrown fully apart, cyan at its 4.8x peak — and 1 is
	 *               rest. Negative releases. The whole SMG fire cycle is 0.100 s, so no screenshot
	 *               can catch the shot frame without this.
	 * @param ReloadAlpha  0..1 pins the RELOAD pose instead of leaving the magazine seated: 0.33 is
	 *               the cell fully dropped. Negative leaves the reload driven by the real weapon.
	 */
	void DebugHoldSmgPhase(float Alpha, float ReloadAlpha, float HoldSeconds);

	/** Verification only. The four SMG parts, or nulls when the SMG rig was not built. */
	void DebugGetSmgParts(UStaticMeshComponent*& OutBody, UStaticMeshComponent*& OutWallLeft,
		UStaticMeshComponent*& OutWallRight, UStaticMeshComponent*& OutMag) const;

	/**
	 * Verification only. Reads EmissiveIntensity BACK OFF the live SMG material instances, for the
	 * same reason DebugGetRailgunEmissive does: writing a parameter name that is not on the material
	 * silently does nothing, and reading it is the only way to catch that.
	 *
	 * @param OutCyan   the first circuit_cyan MID's value. The SMG carries THREE (body + both walls)
	 *                  and they are always written together, so one is representative.
	 * @param OutAmber  the core_amber MID's value. That slot exists ONLY on the magazine mesh.
	 */
	bool DebugGetSmgEmissive(float& OutCyan, float& OutAmber) const;

	/**
	 * The two shared viewmodel materials, so anything else that wants to be made of the same stuff
	 * as the gun can ask instead of guessing.
	 *
	 * The knife used to find these by walking ViewModelRoot's children and matching "Neon" in a
	 * component's NAME. That worked only for as long as the gun was a table of cubes named after
	 * their function; the railgun's parts are named after the mesh, so the search found nothing and
	 * the knife silently fell back to BasicShapeMaterial. Ask the owner instead.
	 */
	UMaterialInstanceDynamic* GetViewModelBodyMID() const;
	UMaterialInstanceDynamic* GetViewModelNeonMID() const;

	/**
	 * [DUALWIELD] SPEC v28 §10 — WHERE THE OFF HAND IS, IN VIEWMODEL RIG SPACE.
	 *
	 * @param OutLocation  the left hand's position, whatever pose it is in.
	 * @return             true when that hand is FREE, i.e. it has come off the weapon and is
	 *                     available to hold something. False on both v27 poses, where it is a support
	 *                     hand on the cube gun's frame or the railgun's foregrip.
	 *
	 * UTraceWeaponComponent hangs the first-person knife rig on this point rather than keeping its
	 * own copy of the coordinates. Two objects agreeing about one fact is the failure this codebase
	 * logs by name, and the failure mode here would be silent and ugly: the blade floating a
	 * centimetre off the fist after somebody retuned the pose.
	 *
	 * Answered from state written by EnsureViewModelBuilt, so it is only meaningful once the rig
	 * exists — it returns false with a zero location before then, which is the same "not ready yet"
	 * answer every other viewmodel accessor on this class gives.
	 */
	bool GetViewModelOffHand(FVector& OutLocation) const;

	// --- THE PACK'S GLOVED HANDS  (spec v31 §6) ---------------------------------------------------

	/**
	 * WHICH HAND SHAPE IS ON SCREEN. There are four and they are a property of what is being CARRIED,
	 * not of what is being done with it — the pack authors every action clip per loadout, because a
	 * hand shape is only meaningful against the thing it is gripping.
	 */
	enum class EHandsLoadout : uint8
	{
		Knife = 0,
		Pistol = 1,
		Smg = 2,
		/** The two-hand cradle. Reached by CARRYING THE CORE, which is also third person — see
		  * UpdateHandsAnimation for why this loadout is nearly always off screen and still correct. */
		Core = 3
	};

	/**
	 * ONE ACTION AT A TIME, on top of the loadout's idle. `None` is the idle itself.
	 *
	 * Only the pairs the pack baked exist: no shoot-with-knife, no reload-for-core. Asking for one
	 * that does not exist resolves to the loadout's idle rather than to a wrong clip — see
	 * ResolveHandsClip(), which is the single table both this and the debug command go through.
	 */
	enum class EHandsAction : uint8
	{
		None = 0,
		Draw,        ///< knife only: the wrist flip that snaps the balisong open
		Stab,        ///< knife only
		Inspect,     ///< knife only, and the one action nothing in this file can observe — see below
		Shoot,       ///< pistol / smg
		Reload,      ///< pistol / smg
		Throw,       ///< core only
		Jump,
		Walljump
	};

	/** True when SK_TraceHands resolved and the pack rig was built; false on the procedural cubes. */
	bool UsesPackHands() const;

	/**
	 * THE SEAM FOR EVERY OTHER SLICE THAT WANTS TO HANG SOMETHING OFF A HAND — §5's knife, §7's
	 * muzzle flash and palm glow. Null until the rig is built, and null forever on the fallback.
	 *
	 * Given out rather than duplicated because the alternative is every slice carrying its own copy
	 * of "the hands live at rig (-8.93, -4.47, 0.08), yawed 90 degrees", which is precisely the
	 * two-objects-agreeing-about-one-fact failure this codebase logs by name. Attach with
	 * GetWeaponAttachBoneName() and the engine does the rest.
	 */
	USkeletalMeshComponent* GetViewModelHandsMesh() const;

	/**
	 * `wrist_right` — the bone the hands README names, verified present on the imported skeleton.
	 *
	 * A BONE, NOT A SOCKET, and deliberately: the GLB contains no `SOCKET_` leaf nodes, so Interchange
	 * created no named sockets, and USkeletalMeshComponent resolves a bone name through the same
	 * attachment API a socket would use. If the artist ever adds SOCKET_muzzle / SOCKET_grip, a
	 * re-import picks them up and this constant is the one line that changes.
	 */
	static FName GetWeaponAttachBoneName();

	/**
	 * SPEC v31 §5's half of the contract: "bind F to inspect".
	 *
	 * INSPECT IS THE ONE ACTION THIS FILE CANNOT SEE. Every other clip is driven off state that
	 * already exists on the pawn — a shot that left, the weapon's replicated reload deadline, the
	 * swing lockout, the wall-jump counter — but an inspect flourish has no gameplay consequence at
	 * all, which is exactly what §5 asks of it, so there is nothing to observe. Whoever owns the F
	 * bind calls this; it is a no-op if the loadout cannot do the action.
	 *
	 * Interruptible by construction: any real action outranks it (see the priority table in
	 * UpdateHandsAnimation), so it cannot swallow a shot or a swap.
	 */
	void PlayHandsAction(EHandsAction Action);

	/**
	 * Verification only (Trace.Hands.Probe). What the rig is actually playing, read back off the
	 * component rather than off our own belief about it — the same argument DebugGetRailgunEmissive
	 * makes for reading EmissiveIntensity back instead of trusting the write.
	 */
	bool DebugGetHandsState(FString& OutClipName, float& OutTimeSeconds, float& OutLengthSeconds,
		FString& OutLoadout) const;

	/**
	 * Verification only (Trace.Hands.Hold). Pins one clip at @p Alpha of its length for
	 * @p HoldSeconds, and forces @p Loadout while it holds, so a screenshot can catch a pose that
	 * otherwise lasts 0.1667 s. Negative @p Alpha releases.
	 *
	 * It also forces the rig VISIBLE for the duration, which the Core loadout needs and nothing else
	 * does: carrying the Core is third person, so Idle_Core is a pose the game builds correctly and
	 * almost never shows.
	 */
	void DebugHoldHandsClip(EHandsLoadout Loadout, EHandsAction Action, float Alpha, float HoldSeconds);

	UTraceCharacterMovementComponent* GetTraceMovement() const;

	// --- Server-authoritative state changes -------------------------------------------------------

	/** Server only. Flips carrier state and pulls the trail/weapon/PlayerState along with it. */
	void SetCarrying(bool bNewCarrying);

	/** Server only. Called from the health component's OnDeath; notifies the GameMode. */
	void HandleDeath(AController* Killer, FName Cause);

	// --- SPEC v19 §4.1: OUT OF BOUNDS ------------------------------------------------------------
	//
	// Verbatim: "If a player ever goes out of bounds of the arena, they should die and respawn."
	//
	// [ASSUMPTION], carried from the spec: a REAL death — the death panel names it, the Deaths column
	// moves, the respawn timer runs and their ability cooldowns keep ticking through it — rather than
	// a silent teleport back inside. And credited to NOBODY: it is passed no killer at all, so
	// ATraceGameMode::NotifyCharacterDied's bSelfKill branch names "the arena" and no enemy is paid
	// for it.
	//
	// *** THE TRAP THIS PASS SETS, AND THE REASON THE TEST IS HORIZONTAL-ONLY. *** Two characters
	// landing beside this one make players legitimately go UP: Lily gains five seconds of flight, and
	// Mortimer gains a mantle. "Out of bounds" therefore cannot mean "higher than usual" — the arena
	// has no ceiling and a player above the walls is still over the pitch and still coming down. What
	// it means is OUTSIDE THE FOOTPRINT, or under the floor. There is a ceiling knob
	// (Trace.Bounds.CeilingMargin) and it is OFF by default for exactly that reason.

	/** SPEC v19 §4.1. Server only. Kills this pawn if it is genuinely outside the arena. */
	void ServerCheckArenaBounds();

	/**
	 * SPEC v19 §4.1. Is @p Location outside the arena, by the margins in the Trace.Bounds.* CVars?
	 *
	 * Static and pure so the rule has ONE definition that the harness can also ask, rather than a
	 * check in a Tick and a second, drifting copy in whatever proves it.
	 *
	 * @param OutReason  filled with a short human phrase ("past the east wall") when it returns true.
	 */
	static bool IsLocationOutOfArenaBounds(const UWorld* World, const FVector& Location, FString& OutReason);

	// --- Presentation -----------------------------------------------------------------------------

	/**
	 * Resolves the soft art references exactly once per pawn and either dresses the skeletal mesh
	 * (mannequin + anim blueprint) or switches on the fallback shape. Never fatal.
	 */
	void SetupCharacterVisuals();

	// --- Character art availability (see ETraceCharacterArtStatus) --------------------------------

	/**
	 * FIRST-RUN CHECK. Answers "is the Mannequin actually on this machine" WITHOUT spawning a pawn
	 * and without loading 126 MB, by asking the package store whether the two packages exist.
	 *
	 * Safe and cheap to call from anywhere on any machine; idempotent; decides the status exactly
	 * once per process and logs a boxed banner at Error verbosity when the answer is bad. ATraceHUD
	 * calls it from BeginPlay so the warning is already on screen during warm-up, before the first
	 * character has been dressed.
	 */
	static void VerifyCharacterArtInstalled();

	/** The decided status, running VerifyCharacterArtInstalled() first if nothing has looked yet. */
	static ETraceCharacterArtStatus GetCharacterArtStatus();

	/**
	 * True when this machine is drawing placeholder shapes (or unanimated poses) because the import
	 * was not run — i.e. when the on-screen warning must be up. False for -TraceNoCharacterArt, which
	 * is a deliberate test of the fallback, and false on a dedicated server.
	 *
	 * @param OutHeadline  short, shouty, one line.
	 * @param OutDetail    the exact command to run.
	 */
	static bool GetCharacterArtWarning(FString& OutHeadline, FString& OutDetail);

	/**
	 * (Re)builds the team-coloured MIDs. Idempotent and safe to call from BeginPlay,
	 * OnRep_PlayerState, OnRep_Team (ATracePlayerState calls it) and OnRep_IsCarrier — team data
	 * arrives in an order nobody can rely on, so every path that could learn it calls this.
	 * No-op on a dedicated server.
	 */
	void ApplyTeamColors();

	/** Where bullets leave from. Chest height, slightly ahead of the capsule, yaw-aligned to aim. */
	FVector GetMuzzleLocation() const;

	/** Unit aim direction, corrected so the shot converges on what the crosshair covers. */
	FVector GetAimDirection() const;

	/**
	 * SPEC v26 §4 — where the VIEWMODEL's muzzle is DRAWN, in world space, for the local viewer.
	 *
	 * NOT GetMuzzleLocation(), AND IT MUST NEVER BE CONFUSED WITH IT. GetMuzzleLocation() is the shot
	 * ORIGIN for hit resolution: camera-derived, on the aim ray, evaluated on the server as well as the
	 * client, and deliberately unaffected by anything in this file's rig. This function is purely
	 * cosmetic and exists for one caller — ATraceTracer, which needs the beam to start at the barrel
	 * the player can see instead of at a hand-tuned screen offset.
	 *
	 * WHY IT IS NOT SIMPLY ViewModelMuzzle->GetComponentLocation(). The viewmodel is tagged
	 * EFirstPersonPrimitiveType::FirstPerson, so the renderer does NOT draw it at its own world
	 * transform: it re-projects it through the camera's FirstPersonFieldOfView and squashes its depth
	 * by FirstPersonScale. A world-space beam started at the component's raw location would therefore
	 * land roughly 30% too close to the crosshair — visibly beside the barrel rather than out of it.
	 * FMinimalViewInfo::TransformWorldToFirstPerson() is the engine's own mirror of that GPU morph and
	 * is what this applies, so the answer tracks the camera's live FOV (the VIDEO page's slider moves
	 * it) with no constant of ours in the arithmetic.
	 *
	 * @return false whenever there is nothing meaningful to answer — no viewmodel built, the rig
	 *         hidden (third person, dead), no camera, or a dedicated server. Callers must fall back.
	 */
	bool GetViewModelMuzzleViewPoint(FVector& OutWorldLocation) const;

	/**
	 * Verification only. The raw, un-morphed world transform of the muzzle marker — i.e. where the
	 * component actually is, before the first-person re-projection above. Trace.DebugViewProbe prints
	 * both so the two can be told apart when the beam looks wrong.
	 */
	bool DebugGetViewModelMuzzleRaw(FVector& OutWorldLocation) const;

	UFUNCTION()
	void OnRep_IsCarrier();

	/**
	 * "A shot just left this gun" - cosmetic only, local player only, safe to call from anywhere.
	 *
	 * Kicks the first-person viewmodel back and up and lets it settle. It deliberately knows nothing
	 * about whether the shot was accepted by the server: recoil that waits for a round trip feels
	 * broken, and this is the same predicted-locally / corrected-never treatment the tracer already
	 * gets in UTraceWeaponComponent.
	 *
	 * Re-entrant and rate-limited, so it does not matter how many paths report the same shot.
	 *
	 * The ONE caller is UTraceWeaponComponent::FireOnce(), at the point a round is committed locally.
	 * That is deliberate and it is the only correct place: DoFirePressed() fires once per trigger
	 * PRESS, so driving recoil from there made a held burst kick a single time. FireOnce() is the only
	 * function that knows a round actually left the gun.
	 */
	void NotifyWeaponFired();

	// --- Input entry points (called by ATracePlayerController) --------------------------------------

	/** @param Value X = strafe (+right), Y = forward (+forward), already in -1..1. */
	void DoMove(const FVector2D& Value);

	/** @param Value X = yaw delta, Y = pitch delta, already sign-corrected by the mapping context. */
	void DoLook(const FVector2D& Value);

	/**
	 * Spec §4: mouse1 is overloaded. While CARRYING the Core it begins/holds a pass and the gun is
	 * silent; otherwise it is the trigger. The overload lives here, not in the controller, because
	 * bIsCarrier is a pawn fact and the controller must not have to know the Core's rules.
	 */
	void DoFirePressed();
	void DoFireReleased();

	/**
	 * The explicit PASS bind (RMB by default), kept separate from mouse1 so a player can rebind it.
	 * Both paths funnel into ATraceCore::RequestPassInput, which is the only pass entry point.
	 *
	 * PUBLIC ON PURPOSE: AI/TraceBotController.cpp's TraceBotPawnAPI probes for exactly these two
	 * names, and a protected member fails that detection silently.
	 */
	void DoPassPressed();
	void DoPassReleased();

	// DoPass() IS DELETED. It was a one-line alias for DoPassPressed() that read as a complete
	// fire-and-forget action but only latched the button down — leaving the holder's shield
	// suppressed and their trace invulnerable until something else cancelled it. That cost one real
	// bug (Trace.DebugTakeCore). Call the explicit DoPassPressed/DoPassReleased pair.

	void DoDash();

	/**
	 * PARRY (spec §3) — the carrier's counter to a dash through their trace.
	 *
	 * ROUTING ONLY. The rule ("0.2 s of trace invulnerability, the whole trace turns red, an enemy
	 * dash inside the window neither breaks the trace nor kills the carrier") belongs to
	 * Gameplay/TraceParry.h and UTraceTrailComponent, which own the window, its replication, the red
	 * tint and the refusal reasons. This forwards to TraceParry::RequestParry(), which is documented
	 * as the mechanic's one entry point and which handles carrier-only, the cooldown, the local tint
	 * prediction and the server RPC itself.
	 *
	 * There is deliberately no ServerParry RPC on this class: a second path to the same window is how
	 * a prediction and an authoritative window end up disagreeing.
	 */
	void DoParryPressed();

	/**
	 * Release half of the parry bind. A NO-OP by design and declared anyway.
	 *
	 * Parry is a tap, not a hold — the window is a fixed 0.2 s owned by the trail component, so
	 * holding the key must not extend it. This exists so the controller can bind Started/Completed/
	 * Canceled symmetrically like every other button in ATracePlayerController, which is what stopped
	 * the pass bind from latching a button down across a cancel (spec §8). Do not make it do work.
	 */
	void DoParryReleased();

	/** True while this pawn's trace is inside a parry window. False in a build without the mechanic. */
	bool IsParryActive() const;

	/**
	 * HUD feed for the parry cooldown, mirroring ATracePlayerController::GetDashHudState's contract.
	 * Returns false when the mechanic does not exist in this build, and the HUD then draws no row at
	 * all — a meter that is permanently full teaches the player that the row means nothing.
	 */
	bool GetParryHudState(float& OutRemaining, float& OutTotal, bool& bOutActive) const;

	/**
	 * 0..1 progress through the 0.5s pass hold; NEGATIVE when no pass is in progress.
	 *
	 * The sign convention is the HUD's contract: a negative return means "draw nothing", not "a pass
	 * that has made no progress". ATracePlayerController::GetPassProgress() forwards this straight
	 * through, so do not change the convention without changing the ring in ATraceHUD.
	 */
	float GetPassProgress() const;

	/**
	 * The fraction of the capsule's full height this pawn's BODY currently occupies: 1.0 standing,
	 * ~0.78 at the bottom of a slide. Feeds FTraceLagCompFrame::PostureScale and, through it, the
	 * head/body/leg zone layout.
	 *
	 * Derived from live BaseEyeHeight rather than from a bool, for two reasons. It is already
	 * interpolated (SlideEyeInterpSpeed), so the zones descend with the view instead of snapping a
	 * one-shot-kill volume 34 uu the instant a key goes down; and BaseEyeHeight is maintained on
	 * every machine including a dedicated server, so the value the rewind records is the same value
	 * the shooter was looking at.
	 *
	 * Deliberately NOT the capsule: sliding does not resize the capsule in this build on purpose
	 * (see UTraceCharacterMovementComponent::CanCrouchInCurrentState). This changes only what a
	 * connecting shot is WORTH, never which shots connect.
	 */
	float GetHitZonePostureScale() const;

protected:
	/**
	 * UTraceHealthComponent drives SetDeadPresentation() straight out of OnRep_Health, which is the
	 * only place that knows about a health change on every machine at once.
	 */
	friend class UTraceHealthComponent;

	/** Bound to UTraceHealthComponent::OnDeath on the server; funnels into HandleDeath(). */
	UFUNCTION()
	void OnHealthDeath(AActor* Victim, AController* Killer, FName Cause);

	/**
	 * Freezes (or unfreezes) the pawn for death/respawn: movement disabled, capsule collision off,
	 * corpse dimmed.
	 *
	 * Driven from UTraceHealthComponent::OnRep_Health rather than from a multicast RPC, so the
	 * server, every connected client and any client that joins *after* the death all reach the same
	 * state from the replicated health value alone. A multicast would leave a late joiner with a
	 * corpse that still blocks movement, which the server would then have to correct away.
	 * Idempotent.
	 */
	void SetDeadPresentation(bool bDead);

	/**
	 * The corpse VANISHES. Mesh, fallback shapes, skid streak, trail meshes, viewmodel — everything
	 * this actor draws, gone on the frame the death is known, and back on the frame it is undone.
	 *
	 * Called only from SetDeadPresentation(), which is driven by the REPLICATED health value
	 * (UTraceHealthComponent::OnRep_Health), so the server, every client and any client that joins
	 * later all reach the same answer from the same fact. A multicast would leave a late joiner
	 * looking at a body nobody else can see.
	 *
	 * Hiding at the ACTOR level rather than by clearing each component's own visibility flag is the
	 * whole trick: which components are visible at any moment is decided elsewhere and by several
	 * owners at once — SetupCharacterVisuals() picks mannequin OR fallback shapes depending on
	 * whether the art import was run, UpdateViewBlend() owns the viewmodel, UpdateCrouchPresentation()
	 * owns the skid. bHiddenInGame is an INDEPENDENT second gate on top of all of that, so setting it
	 * hides everything and clearing it restores precisely the state the pawn had, with nothing to
	 * guess back on respawn.
	 */
	void SetCorpseHidden(bool bInHidden);

	// ServerPass / PerformPass ARE DELETED — see the tombstone in the .cpp. Both had zero callers.
	// The pass is decided on the server from the holder's own aim, so a client-supplied direction was
	// unusable by definition, and a reliable server RPC nothing calls is only an attack surface.
	// The live path is DoPassPressed/DoPassReleased -> ATraceCore::RequestPassInput, whose own RPC
	// (ATraceCore::ServerSetPassInput) re-enters the same validation.

	/** Creates the MID on first use, then just pushes the colour. Fallback shapes only. */
	void ApplyColorToMesh(UStaticMeshComponent* InMesh, TObjectPtr<UMaterialInstanceDynamic>& InOutMID, const FLinearColor& InColor);

	/**
	 * Team colour on the Mannequin. One MID per material slot (Manny has two), created once, then
	 * M_Mannequin's "Paint Tint" vector is pushed every time the team, carrier or alive state
	 * changes. The tint recolours the whole suit, which is what makes cyan and orange readable
	 * across the arena; "EmissivePower" is pushed alongside it but is measurably inert on the stock
	 * material instances — see the note in TraceCharacter.cpp before relying on it.
	 */
	void ApplyColorToSkeletalMesh(const FLinearColor& InColor, float InEmissivePower);

	/** Retry hook for the team colour: PlayerState/Team can replicate after the pawn exists. */
	void PollTeamColors();

	// --- View mode (see the file header) ----------------------------------------------------------

	/**
	 * Advances ViewBlendAlpha toward the mode the carrier state asks for and pushes the result into
	 * the spring arm and into our own body's owner-visibility.
	 *
	 * @param DeltaSeconds  frame time; ignored when bSnap.
	 * @param bSnap         jump straight to the target (spawn, possession, respawn) instead of
	 *                      blending. A blend on the first frame would have every pawn in the match
	 *                      start 450 uu behind itself and fly forward.
	 */
	void UpdateViewBlend(float DeltaSeconds, bool bSnap);

	/**
	 * Height of the third-person arm pivot above the capsule centre, in world space.
	 *
	 * Computed rather than constant because it has to clear the top of the carrier's OWN trail — see
	 * TraceCharacterLayout::TrailCameraClearance. Third person and "laying a trail" are the same
	 * state, so the camera and the trail can never be tuned independently.
	 */
	float GetThirdPersonPivotZ() const;

	/**
	 * Applies the rotation model for the current mode (see the file header, point 3). Separate from
	 * the blend because it is a discrete flip, not a lerp: half-orienting a capsule is meaningless.
	 */
	void ApplyRotationMode();

	/**
	 * SetOwnerNoSee on every visual component of this pawn — the mannequin and both fallback shapes.
	 * Local, per-machine and owner-only: it changes nothing about what other players see, and it is
	 * only ever called on the locally controlled pawn.
	 */
	void SetOwnBodyHiddenFromOwner(bool bInHidden);

	// --- First-person viewmodel (see the file header) ---------------------------------------------

	/**
	 * Builds the handgun and the two forearms, once, the first time this pawn turns out to be the
	 * one a human is looking out of.
	 *
	 * Lazy on purpose. Ten pawns spawn per match and exactly one of them is ever seen from the
	 * inside; building sixteen static mesh components on the other nine (and on every bot on a
	 * listen server) would be pure cost for geometry no camera can reach - SetOnlyOwnerSee means it
	 * is not even drawn. Idempotent.
	 */
	void EnsureViewModelBuilt();

	/** One primitive of the viewmodel: owner-only, shadowless, uncollidable, first-person-tagged. */
	UStaticMeshComponent* AddViewModelPart(UStaticMesh* InMesh, const TCHAR* DebugName,
		const FVector& Location, const FRotator& Rotation, const FVector& Size,
		UMaterialInstanceDynamic* MID);

	/**
	 * Sway, walk bob and recoil, as a relative transform on ViewModelRoot.
	 *
	 * All of it is a child transform under the camera and NONE of it is read by the shot path, so
	 * the gun can lag, bounce and kick as much as it likes without the crosshair ever lying — which
	 * is exactly why the weapon is animated here rather than by moving the camera.
	 */
	void UpdateViewModel(float DeltaSeconds);

	/**
	 * Builds the railgun in place of the cube gun's twelve parts. Returns false without building
	 * anything if the art did not resolve, which is the caller's cue to build the fallback.
	 */
	bool BuildRailgunViewModel();

	/**
	 * Plays the discharge-and-decay tail of the artist's Fire clip: the rail walls throw apart and
	 * cant outward, the receiver recoils inside the hands, and both glowing materials flare. Driven
	 * by the authored curve in TraceRailgunFireCurve.h, so the motion and the glow cannot drift
	 * apart. No-op when the fallback gun is in use.
	 */
	void UpdateRailgunFire(float DeltaSeconds);

	/**
	 * SPEC v30 §2 — builds the SMG's four parts alongside the pistol's, not instead of them.
	 *
	 * BOTH GUNS ARE BUILT ONCE AND THEN SHOWN OR HIDDEN, rather than one being torn down and the
	 * other constructed on every swap. A swap is a 0.35 s pullout that a player does mid-fight;
	 * building four static mesh components and five dynamic material instances inside it would be a
	 * hitch on a frame that already has to be smooth, and the alternative — rebuilding on the first
	 * swap and caching after — is the same memory with a stutter bolted to the front of it. Ten
	 * meshes on the one pawn a human is inside is nothing; a hitch in a firefight is not.
	 *
	 * Returns false without building anything if the art did not resolve, which is the caller's cue
	 * to let the `3` slot fall back to whichever pistol rig exists.
	 */
	bool BuildSmgViewModel();

	/**
	 * SPEC v30 §3/§4 — the SMG's motion and glow, reproduced in code from the kit's numbers.
	 *
	 * DRIVEN BY THE WEAPON'S REAL STATE, never by a free-running timer: the fire cycle is restarted
	 * by NotifyWeaponFired (an actual round leaving), and the reload phase is read live off
	 * UTraceWeaponComponent's replicated reload deadline. What is on screen therefore cannot
	 * disagree with what the gun is doing, which is the failure §3 names.
	 */
	void UpdateSmgAnimation(float DeltaSeconds);

	// --- The pack's gloved hands  (spec v31 §6) ---------------------------------------------------

	/**
	 * Builds SK_TraceHands in place of the four hand cubes and the two forearm cylinders. Returns
	 * false without building anything when the art did not resolve, which is the caller's cue to
	 * build the procedural rig — the same contract BuildRailgunViewModel() and BuildSmgViewModel()
	 * honour, for the same reason: a fresh clone must still be playable.
	 *
	 * Also DERIVES, once, the two facts everything else in this section is expressed against: where
	 * `wrist_right` sits in rig space in the reference pose (HandsWristRestRig, the base every weapon
	 * offset is stored relative to) and where the left wrist sits (which is what
	 * GetViewModelOffHand() reports once the pack rig exists, so the knife lands on a hand that is
	 * actually there rather than on a cube that no longer is).
	 */
	bool BuildPackHandsViewModel();

	/**
	 * Picks the clip for a loadout/action pair, or the loadout's IDLE when the pack did not bake that
	 * pair. One table, used by the live driver and by Trace.Hands.Hold alike, so a screenshot cannot
	 * photograph a combination the game can never reach.
	 *
	 * @return an index into HandsAnims, or INDEX_NONE when even the idle is missing.
	 */
	int32 ResolveHandsClip(EHandsLoadout Loadout, EHandsAction Action) const;

	/**
	 * SPEC v31 §6 — the whole hand state machine, and every input to it is REAL STATE.
	 *
	 * Nothing here free-runs. The loadout is the replicated weapon selector (plus IsCarrier() for the
	 * Core); the reload's position is read live off UTraceWeaponComponent's replicated deadline; the
	 * stab is stretched onto TraceMelee::GetSwingAnimSeconds(); a shot is a round that actually left
	 * (NotifyWeaponFired); a wall jump is the movement component's own counter going up. And the
	 * clip's time is SAMPLED BEFORE IT IS ADVANCED, so the frame that follows a shot draws the
	 * trigger-pull frame — on a 0.1667 s clip that frame is 6% of the whole action.
	 */
	void UpdateHandsAnimation(float DeltaSeconds);

	/**
	 * Puts the two gun rigs where `wrist_right` is this frame, WITHOUT re-parenting them.
	 *
	 * Every part's shipped rig-space rest transform is kept (RailgunPartRest / SmgPartRest) and
	 * multiplied by the wrist's delta from its reference pose. That is a value stored RELATIVE TO ITS
	 * BASE, which is the standing rule, and it is why retuning RailgunScale or SmgOrigin still moves
	 * the gun correctly: the rest transform is read back off the component at build time rather than
	 * being a second copy of the constants.
	 *
	 * See the file header for why this is a transform rather than an AttachToComponent.
	 */
	void UpdateWeaponsFollowHands();

	/**
	 * THE ONE WRITER OF EVERY WEAPON PART'S TRANSFORM, and the reason UpdateRailgunFire and
	 * UpdateSmgAnimation did not have to learn about hands: they still compute a RIG-SPACE pose
	 * exactly as they always did, and this is what folds HandsWristDelta in on the way out. With no
	 * pack hands the delta is identity and the value written is byte-for-byte what v30 wrote.
	 */
	void SetViewModelWeaponPose(UStaticMeshComponent* Part, const FVector& RigLocation, const FRotator& RigRotation);

	/**
	 * SPEC v30 §2 — decides which of the two guns is on screen, from the replicated weapon selector.
	 *
	 * Re-asserted every frame rather than hooked to a swap event, for exactly the reason
	 * UTraceWeaponComponent::SetGunViewModelHidden is: this is not the only writer of these
	 * components' visibility, and an edge-triggered version goes stale the first time somebody else
	 * re-shows the rig (a respawn, or handing the Core back).
	 */
	void UpdateWeaponSelection();

	/**
	 * The muzzle marker of the gun THIS RIG IS HOLDING — the SMG's whenever the SMG rig is the one
	 * selected, including while the guns are momentarily stowed. Follows SelectedFirearm rather than
	 * ShownGun deliberately: a stowed rig draws no beam at all (GetViewModelMuzzleViewPoint refuses
	 * on !bViewModelVisible), and the marker must still belong to the gun that comes back out.
	 */
	USceneComponent* GetActiveMuzzleMarker() const;

	/** Guarded show/hide for the whole rig. Also stops UpdateViewModel doing arithmetic for nothing. */
	void SetViewModelVisible(bool bVisible);

	/**
	 * Crouch/slide presentation on the BODY, for everyone else's benefit.
	 *
	 * There is no crouch or slide animation in the imported Mannequin set (Anims/Unarmed has idle,
	 * walk, run, jump, fall and four attacks, and nothing else), so a sliding player would otherwise
	 * be a running pose in a half-height capsule. This leans the mesh into the slide and lights the
	 * skid streak under the feet. It is an approximation and it is documented as one — the real fix
	 * is a crouch/slide sequence and an anim blueprint that can play it.
	 */
	void UpdateCrouchPresentation(float DeltaSeconds);

private:
	/** Engine basic-shape material, resolved in the constructor so the cooker keeps it. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> BasicShapeMaterial;

	/** /Engine/BasicShapes, for the viewmodel and the skid streak. Always present, unlike the art. */
	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh;

	/**
	 * The two generated Tron materials, shared with ATraceArenaBuilder so the gun in your hands is
	 * made of the same stuff as the arena around it. Produced by Scripts/generate_content.py into a
	 * gitignored folder, so both are optional: MakeViewModelMaterials() falls back to
	 * BasicShapeMaterial and the viewmodel renders flat instead of neon.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> NeonMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ViewModelBodyMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ViewModelNeonMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SlideSkidMID;

	/**
	 * The railgun's three meshes: a body plus the two rail walls that throw apart when it fires.
	 * Committed art (Content/Trace/Weapons), authored in Art/Railgun/railgun.glb and imported by
	 * Scripts/import-railgun.sh. All three are optional — a miss on any one builds the older
	 * procedural cube gun instead.
	 */
	UPROPERTY()
	TObjectPtr<UStaticMesh> RailgunBodyMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> RailgunRailLeftMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> RailgunRailRightMesh;

	/** The three railgun parts, in build order: body, left wall, right wall. Null when the fallback
	  * cube gun was built instead, which is what UsesRailgunViewModel() reports. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> RailgunBodyPart;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> RailgunRailLeftPart;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> RailgunRailRightPart;

	/** The two glowing slots, pulled out as dynamic instances so the fire curve can drive their
	  * EmissiveIntensity per player without touching the shared asset. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RailgunCyanMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RailgunAmberMID;

	/** Seconds since the shot that started the fire animation; negative when nothing is playing. */
	float RailgunFireElapsed = -1.f;

	/** How long the played segment lasts, chosen at fire time from the weapon's own fire interval. */
	float RailgunFireDuration = 0.f;

	/** Trace.Railgun.Hold only: a pinned phase and the world time it expires. -1 means not held. */
	float RailgunDebugHoldAlpha = -1.f;
	double RailgunDebugHoldUntil = -1.0;

	// --- The SMG  (spec v30) ----------------------------------------------------------------------
	//
	// FOUR meshes rather than the pistol's three, because this export carries authored pivot nodes:
	// a body, the two rail walls that snap apart on each shot, and the magazine that drops on a
	// reload. Committed art, same contract as the railgun's: every one of them optional, and a miss
	// on any one leaves the `3` slot showing whichever pistol rig exists rather than nothing at all.

	UPROPERTY()
	TObjectPtr<UStaticMesh> SmgBodyMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> SmgWallLeftMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> SmgWallRightMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> SmgMagMesh;

	/** The four SMG parts, in build order. All null when the SMG rig was not built. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> SmgBodyPart;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> SmgWallLeftPart;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> SmgWallRightPart;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> SmgMagPart;

	/**
	 * *** THE SMG'S GLOW IS SPLIT ACROSS MESHES, WHICH THE PISTOL'S IS NOT. ***
	 *
	 * On the railgun both glowing slots sit on the one body mesh, so two MIDs off one component was
	 * the whole story. On the SMG, `circuit_cyan` is on the body AND on both walls (three components,
	 * hence an array — writing only the body's would leave the rail channels dead through every
	 * shot), and `core_amber` exists ONLY on the magazine. Creating the amber MID off the body the
	 * way the pistol does silently yields INDEX_NONE and an ammo readout that never lights.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SmgCyanMIDs;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SmgAmberMID;

	/** Seconds since the shot that started the 0.100 s fire cycle; negative when nothing is playing. */
	float SmgFireElapsed = -1.f;

	/** How long that cycle lasts, taken at fire time from the weapon's own interval. */
	float SmgFireDuration = 0.f;

	/** Trace.Smg.Hold only. Negative means not held; ReloadAlpha < 0 leaves the magazine to the gun. */
	float SmgDebugHoldAlpha = -1.f;
	float SmgDebugHoldReloadAlpha = -1.f;
	double SmgDebugHoldUntil = -1.0;

	// --- The pack's gloved hands  (spec v31 §6) ---------------------------------------------------
	//
	// ONE skeletal mesh and TWENTY sequences, all optional. A miss on the mesh or on any of the four
	// idles builds the procedural cube hands instead and says which asset was absent.

	UPROPERTY()
	TObjectPtr<USkeletalMesh> HandsMesh;

	/** Indexed by ResolveHandsClip(); entries may be null when a single clip failed to resolve. */
	UPROPERTY()
	TArray<TObjectPtr<UAnimSequence>> HandsAnims;

	/** The live rig. Null on the fallback, which is exactly what UsesPackHands() reports. */
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> HandsPart;

	/**
	 * `wrist_right` in RIG SPACE in the mesh's REFERENCE POSE — the base every weapon offset below is
	 * stored relative to, and the only reason the guns can ride the hand without a single hand-typed
	 * offset. Identity until the pack rig is built.
	 */
	FTransform HandsWristRestRig = FTransform::Identity;

	/**
	 * `wrist_right` NOW, expressed as a delta from that reference pose — i.e. the transform every
	 * weapon part is multiplied by to ride the hand. IDENTITY on the fallback rig, which is what lets
	 * one code path serve both: with no pack hands every weapon writes exactly the rig-space
	 * transform it has always written.
	 */
	FTransform HandsWristDelta = FTransform::Identity;

	/** True once the pack rig is up AND its wrist bone was found. Gates every hand-follow write. */
	bool bHandsRigActive = false;

	/**
	 * Each gun rig's shipped rest transform in RIG space, READ BACK off the components at build time
	 * rather than re-typed from the layout constants — parallel to PistolWeaponParts / SmgWeaponParts.
	 *
	 * Read back rather than derived so this covers the twelve procedural cube-gun parts too, which
	 * have no named constants of their own, and so that retuning RailgunOrigin or SmgScale moves the
	 * hand-following rest with it instead of leaving a second copy behind.
	 */
	TArray<FTransform> PistolWeaponRest;
	TArray<FTransform> SmgWeaponRest;

	/** Which clip index is loaded on the component right now, and how far into it we are. */
	int32 HandsClipIndex = INDEX_NONE;
	float HandsClipTime = 0.f;

	/** The action currently playing (None == the loadout idle) and the loadout it belongs to. */
	EHandsAction HandsAction = EHandsAction::None;
	EHandsLoadout HandsLoadout = EHandsLoadout::Pistol;

	/**
	 * Holds the Core cradle open across a throw, which is the ONE action that outlives its loadout:
	 * bIsCarrier is already false by the frame the throw is detected. Released by the clip ending or
	 * by any other event, so it can never strand the hands in a hold the player has left.
	 */
	bool bHandsLoadoutLatched = false;

	/**
	 * One-frame latches, set by the events that cannot be sampled — a jump commit and a round leaving
	 * — and consumed by the next UpdateHandsAnimation. A latch rather than an immediate PlayAnimation
	 * because the clip's time has to be sampled before it is advanced, and that ordering lives in one
	 * place.
	 */
	bool bHandsJumpPending = false;
	bool bHandsShotPending = false;

	/** Edge detectors over state that IS sampleable. -1 / false means "not yet seeded". */
	int32 HandsLastWallJumpCount = -1;
	bool bHandsWasReloading = false;
	bool bHandsWasSwinging = false;
	bool bHandsWasDeploying = false;
	bool bHandsWasCarrier = false;

	/** §5's TraceKnifeView::IsInspecting(), last frame. Presentation only, like the query itself. */
	bool bHandsWasInspecting = false;

	/** Trace.Hands.Hold only. Negative alpha means not held. */
	int32 HandsDebugClipIndex = INDEX_NONE;
	float HandsDebugAlpha = -1.f;
	double HandsDebugUntil = -1.0;
	EHandsLoadout HandsDebugLoadout = EHandsLoadout::Pistol;

	/**
	 * SPEC v30 §2 — the two gun rigs, split out of ViewModelParts so the selector can hide one.
	 *
	 * WEAPON PARTS ONLY. The hands, knuckles, forearms and cuffs are in neither list: they hold
	 * whichever gun won and are never written by the selector, which is the same rule
	 * UTraceWeaponComponent::IsViewModelHandPart encodes for the knife.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> PistolWeaponParts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> SmgWeaponParts;

	/**
	 * What UpdateWeaponSelection last put on screen, including None while the guns are stowed. This
	 * is the DRAWN answer that GetShownGun() reports.
	 */
	EShownGun ShownGun = EShownGun::Pistol;

	/**
	 * WHICH OF THE TWO GUNS THIS RIG IS HOLDING — never None, and unchanged by stowing.
	 *
	 * Separate from ShownGun because "which gun" and "is any gun out" are different questions with
	 * different owners: this file answers the first, UTraceWeaponComponent's knife rule answers the
	 * second. Putting the guns away must not make this file forget which one was in hand, or the
	 * pistol would come back after every `1` regardless of what the player had out.
	 */
	EShownGun SelectedFirearm = EShownGun::Pistol;

	/** The SMG fallback banner is printed once per pawn, not once per frame. */
	bool bSmgFallbackLogged = false;

	/** Every part of the viewmodel, so visibility is one loop and the parts cannot be orphaned. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> ViewModelParts;

	/**
	 * SPEC v26 §4. An empty scene component parked ON the barrel's exit, so "where does the beam start"
	 * has an ANSWER IN THE SCENE GRAPH rather than a constant somewhere that has to be re-tuned every
	 * time the gun moves.
	 *
	 * It is a CHILD OF THE GUN, not of the rig: attached to RailgunBodyPart at the mesh's own
	 * (107.4, 0, 4.5) cm muzzle landmark when the railgun art resolved, and to ViewModelRoot at the
	 * fallback cube gun's muzzle otherwise. Being a child of the body mesh is the whole point — the
	 * body carries the per-shot recoil (UpdateRailgunFire moves it back and pitches it up), the rig
	 * carries the sway/bob/crouch dip, and a marker underneath both inherits every one of them for
	 * free. Nothing here needs updating when any of those are retuned, and if the gun is ever socketed
	 * onto an animated hand the marker rides that too.
	 */
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ViewModelMuzzle;

	/**
	 * SPEC v30 §5, the character's half of it: the SAME arrangement on the SMG's body, at the SMG's
	 * own measured (58.8, 0, 4.5) cm aperture.
	 *
	 * TWO MARKERS, ONE PER GUN, rather than one marker that gets reparented on every swap. Both guns
	 * are built once and shown or hidden (see BuildSmgViewModel), so their muzzles are static facts
	 * about static rigs; GetActiveMuzzleMarker() picks the one belonging to the gun actually drawn.
	 * A single moving marker would have to be detached and reattached inside a swap, and would be
	 * momentarily attached to a hidden gun on any frame the two disagreed — which is precisely a beam
	 * leaving from the wrong barrel, the defect §5 exists to prevent.
	 */
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ViewModelSmgMuzzle;

	bool bViewModelBuilt = false;
	bool bViewModelVisible = false;

	/**
	 * [DUALWIELD] Where EnsureViewModelBuilt actually put the left hand, in rig space, and whether it
	 * was placed as a FREE off hand (spec v28 §10) rather than as a support hand on the weapon.
	 *
	 * Recorded rather than re-derived because the answer depends on three things this class resolves
	 * at build time — the dual-wield switch, whether the railgun art imported, and the cube gun's own
	 * table — and UTraceWeaponComponent needs the same point to hang the knife on. See
	 * GetViewModelOffHand().
	 */
	FVector ViewModelOffHandLocation = FVector::ZeroVector;
	bool bViewModelOffHandFree = false;

	/** Sway/bob/recoil state. Cosmetic, local, never replicated, never read by the shot path. */
	FRotator ViewModelLastControlRotation = FRotator::ZeroRotator;
	bool bViewModelHasLastRotation = false;
	float ViewModelSwayYaw = 0.f;
	float ViewModelSwayPitch = 0.f;
	float ViewModelBobPhase = 0.f;
	float ViewModelBobStrength = 0.f;
	float ViewModelKick = 0.f;
	float ViewModelCrouchDip = 0.f;
	double LastFireKickTime = -1000.0;

	/** 0 = standing, 1 = fully leaned into a crouch/slide. Eased so the pose does not snap. */
	float CrouchLeanAlpha = 0.f;
	float SkidGlowAlpha = 0.f;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FallbackBodyMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FallbackHeadMID;

	/** One per material slot on the skeletal mesh (Manny has two: head/legs and torso). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> CharacterMIDs;

	/** True once SetupCharacterVisuals() has decided; keeps ApplyTeamColors() cheap and correct. */
	bool bUsingSkeletalMesh = false;

	/** Latches so one life produces exactly one death, however many sources fire at once. */
	bool bDeathHandled = false;

	/**
	 * SPEC v19 §4.1. Server-side world time this pawn was FIRST seen outside the arena, or -1.
	 *
	 * A grace, not a hair trigger, and it is there because the alternative has already cost this
	 * project a bug class: a single frame outside the box is what a depenetration push, a teleport
	 * that has not been ratified, or the frame between a spawn transform and its first movement update
	 * all look like. Killing on the first frame would make those indistinguishable from a player
	 * walking out of the world. Trace.Bounds.GraceSeconds is how long they have to come back.
	 */
	float OutOfBoundsSinceServerTime = -1.f;

	/** Current state of SetDeadPresentation(), so repeated calls are free and cannot double-apply. */
	bool bDeadPresentation = false;

	FTimerHandle TeamColorTimerHandle;
	int32 TeamColorAttempts = 0;

	/**
	 * 0 = first person, 1 = third person. Starts at 0 because a pawn spawns without the Core, and is
	 * snapped (not blended) on spawn/possession anyway.
	 */
	float ViewBlendAlpha = 0.f;

	/** Last value pushed through SetOwnerNoSee, so the render state is only dirtied on a change. */
	bool bOwnBodyHiddenFromOwner = false;

	/**
	 * Locally-controlled state as of the last tick. On a client the controller arrives by
	 * replication some frames after the pawn does, and the frame it arrives is the frame the camera
	 * must be SNAPPED into place rather than blended from wherever it happened to be.
	 */
	bool bWasLocallyControlled = false;

	/** Last mode ApplyRotationMode() configured, so the flip only happens when it really changes. */
	bool bRotationModeIsFirstPerson = false;
	bool bRotationModeApplied = false;

};
